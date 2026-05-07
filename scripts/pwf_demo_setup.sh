#!/usr/bin/env bash
# pwf_demo_setup.sh - Bring up a primary + hot-standby pair preconfigured
# with pg_write_forward, ready for the demo SQL in pwf_demo.sql.
#
# Usage:
#   ./pwf_demo_setup.sh start    # initdb + start primary, basebackup standby, start standby
#   ./pwf_demo_setup.sh stop     # stop both
#   ./pwf_demo_setup.sh nuke     # stop and rm -rf the data dirs (DESTRUCTIVE)
#   ./pwf_demo_setup.sh psql_p   # psql to primary
#   ./pwf_demo_setup.sh psql_s   # psql to standby
#   ./pwf_demo_setup.sh status   # show roles
#
# Layout (under $BASE):
#   primary/   port 55501  (writable)
#   standby/   port 55502  (read-only with pg_write_forward)
#
# Connections use the unix socket directory $BASE/sockets so we don't
# touch /tmp or /var/run.
set -euo pipefail

PGINSTALL=${PGINSTALL:-/home/azureuser/pg/install}
PG_CTL="$PGINSTALL/bin/pg_ctl"
INITDB="$PGINSTALL/bin/initdb"
PG_BASEBACKUP="$PGINSTALL/bin/pg_basebackup"
PSQL="$PGINSTALL/bin/psql"

BASE=${BASE:-/home/azureuser/pg/pwf_demo}
PRIMARY="$BASE/primary"
STANDBY="$BASE/standby"
SOCKDIR="$BASE/sockets"

PRIMARY_PORT=55501
STANDBY_PORT=55502

mkdir -p "$BASE" "$SOCKDIR"

start_primary() {
    if [ ! -s "$PRIMARY/PG_VERSION" ]; then
        echo ">>> initdb primary"
        "$INITDB" -D "$PRIMARY" -U postgres --auth=trust >/dev/null

        cat >> "$PRIMARY/postgresql.conf" <<EOF
port = $PRIMARY_PORT
unix_socket_directories = '$SOCKDIR'
listen_addresses = ''
wal_level = replica
max_wal_senders = 4
hot_standby = on
log_line_prefix = '%t [primary %p] '
EOF
        # allow standby's basebackup + replication on socket
        cat >> "$PRIMARY/pg_hba.conf" <<EOF
local replication postgres trust
EOF
    fi
    "$PG_CTL" -D "$PRIMARY" -l "$BASE/primary.log" -w start
}

start_standby() {
    if [ ! -s "$STANDBY/PG_VERSION" ]; then
        echo ">>> base backup primary -> standby"
        "$PG_BASEBACKUP" -h "$SOCKDIR" -p $PRIMARY_PORT -U postgres \
            -D "$STANDBY" -R -X stream -c fast >/dev/null

        cat >> "$STANDBY/postgresql.conf" <<EOF
port = $STANDBY_PORT
unix_socket_directories = '$SOCKDIR'
listen_addresses = ''
log_line_prefix = '%t [standby %p] '

# pg_write_forward
shared_preload_libraries = 'pg_write_forward'
pg_write_forward.primary_conninfo = 'host=$SOCKDIR port=$PRIMARY_PORT dbname=postgres user=postgres'
pg_write_forward.consistency       = session
pg_write_forward.enabled           = on
pg_write_forward.lsn_wait_timeout_ms = 60000
EOF
    fi
    "$PG_CTL" -D "$STANDBY" -l "$BASE/standby.log" -w start
}

stop_node() {
    local d=$1
    [ -s "$d/postmaster.pid" ] && "$PG_CTL" -D "$d" -m fast stop || true
}

case "${1:-start}" in
    start)
        start_primary
        start_standby
        echo
        echo "Primary    : psql -h $SOCKDIR -p $PRIMARY_PORT -U postgres postgres"
        echo "Standby    : psql -h $SOCKDIR -p $STANDBY_PORT -U postgres postgres"
        echo "Logs       : $BASE/primary.log , $BASE/standby.log"
        echo
        echo "Demo SQL   : psql -h $SOCKDIR -p $STANDBY_PORT -U postgres -f $(dirname "$0")/pwf_demo.sql postgres"
        ;;
    stop)
        stop_node "$STANDBY"
        stop_node "$PRIMARY"
        ;;
    nuke)
        stop_node "$STANDBY" || true
        stop_node "$PRIMARY" || true
        rm -rf "$BASE"
        ;;
    psql_p)
        exec "$PSQL" -h "$SOCKDIR" -p $PRIMARY_PORT -U postgres postgres
        ;;
    psql_s)
        exec "$PSQL" -h "$SOCKDIR" -p $STANDBY_PORT -U postgres postgres
        ;;
    status)
        for d in "$PRIMARY" "$STANDBY"; do
            if [ -s "$d/postmaster.pid" ]; then
                role=$("$PSQL" -h "$SOCKDIR" -p "$(grep ^port "$d/postgresql.conf" | awk '{print $3}')" \
                       -U postgres -tAc "SELECT CASE WHEN pg_is_in_recovery() THEN 'standby' ELSE 'primary' END" postgres 2>/dev/null || echo "?")
                printf "%-50s [%s]\n" "$d" "$role"
            else
                printf "%-50s [stopped]\n" "$d"
            fi
        done
        ;;
    *)
        echo "Usage: $0 {start|stop|nuke|psql_p|psql_s|status}" >&2
        exit 2
        ;;
esac
