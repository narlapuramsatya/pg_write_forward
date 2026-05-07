# Installing pg_write_forward

## Prerequisites

| component | minimum |
|---|---|
| PostgreSQL server | 17.0 (uses `xlogwait.h`) |
| libpq dev headers | comes with the server build |
| GNU make | any modern version |
| Perl + `IPC::Run` | only for `make check` |

You need a working `pg_config` whose `--pgxs` path is writable (or you
install with `sudo`). The same `pg_config` must point at the PostgreSQL
build that the **standby** will run, because the compiled `.so` is loaded
into that cluster's backends.

## Build

```bash
cd pg_write_forward
make PG_CONFIG=/path/to/pg_config
```

This produces `pg_write_forward.so` in the source tree.

## Install

```bash
sudo make PG_CONFIG=/path/to/pg_config install
```

This places:

- `$libdir/pg_write_forward.so`
- `$sharedir/extension/pg_write_forward.control`
- `$sharedir/extension/pg_write_forward--1.0.sql`

Use the same `pg_config` you used to build.

## Configure the standby

`pg_write_forward` installs SQL hooks at backend startup and is therefore
required in `shared_preload_libraries`. Edit the **standby's**
`postgresql.conf`:

```
# Required
shared_preload_libraries = 'pg_write_forward'

# Connection string the standby uses to talk to the primary.  Anything
# libpq accepts works.  Use a service file for credentials in production.
pg_write_forward.primary_conninfo = 'host=primary.internal port=5432 dbname=postgres user=fwd_user'

# Consistency mode: off | eventual | session | global
pg_write_forward.consistency = 'session'

# Optional: master kill switch (default on) and LSN-wait timeout (default 60000 ms).
pg_write_forward.enabled = on
pg_write_forward.lsn_wait_timeout_ms = 60000
```

Restart the standby for `shared_preload_libraries` to take effect:

```bash
pg_ctl -D /path/to/standby restart
```

## Create the extension

In each database where you want the helper functions:

```sql
CREATE EXTENSION pg_write_forward;
```

Forwarding works without `CREATE EXTENSION`; the helpers
(`pg_write_forward_status()`, `pg_write_forward_disconnect()`) require
it.

## GUC reference

| GUC | Type | Default | Scope | Notes |
|---|---|---|---|---|
| `pg_write_forward.primary_conninfo` | text | `''` | SUSET | libpq conninfo for the primary. |
| `pg_write_forward.consistency` | enum | `off` | USERSET | `off`, `eventual`, `session`, `global`. |
| `pg_write_forward.enabled` | bool | `on` | USERSET | Master kill switch. |
| `pg_write_forward.lsn_wait_timeout_ms` | int | `60000` | USERSET | Replay-wait timeout (ms). |

`SUSET` GUCs require superuser to change. `USERSET` GUCs can be changed
freely by any session — the typical pattern is a per-session `SET
pg_write_forward.consistency = 'session'` after connect, or via
`ALTER ROLE ... SET`.

## Verifying the install

On the standby, with the extension loaded:

```sql
=> SELECT * FROM pg_write_forward_status();
       primary_conninfo        | consistency | enabled | connected | forwarded_count | last_remote_lsn
-------------------------------+-------------+---------+-----------+-----------------+-----------------
 host=primary.internal port... | session     | t       | f         |               0 | 0/0
```

Then issue a write:

```sql
=> INSERT INTO t VALUES (default, 'first forwarded write');
INSERT 0 1
=> SELECT * FROM pg_write_forward_status();
       primary_conninfo        | consistency | enabled | connected | forwarded_count | last_remote_lsn
-------------------------------+-------------+---------+-----------+-----------------+-----------------
 host=primary.internal port... | session     | t       | t         |               1 | 0/1A2B3C40
```

If `connected` stays `false` after a write, check the standby log for
libpq connection errors (most commonly: `pg_hba.conf` on the primary
doesn't permit the standby's source IP for the configured user).

## Failover / primary swap

After a failover:

1. Update `pg_write_forward.primary_conninfo` on the new standby
   (typically driven by a service-discovery script that templates
   `postgresql.auto.conf`).
2. `SELECT pg_reload_conf();` to pick up the new value.
3. In any session that already had a forwarded statement run before the
   failover, call `SELECT pg_write_forward_disconnect();` to drop the
   stale cached libpq connection. Subsequent forwards will reconnect.

## Uninstall

```sql
DROP EXTENSION pg_write_forward;
```

Then on each standby, remove `pg_write_forward` from
`shared_preload_libraries` and restart.

```bash
sudo make PG_CONFIG=/path/to/pg_config uninstall
```
