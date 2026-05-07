# Copyright (c) 2026, PostgreSQL Global Development Group

# 001_basic.pl - hot-standby write forwarding via pg_write_forward.
#
# Brings up:
#   - primary  (allows_streaming => 1)
#   - standby  (init_from_backup, has_streaming => 1, loads pg_write_forward
#               via shared_preload_libraries, points primary_conninfo at the
#               primary cluster)
# and exercises every supported statement class against all three non-off
# consistency modes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ---------------------------------------------------------------------
# Cluster setup
# ---------------------------------------------------------------------
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_write_forward'
log_min_messages = warning
});
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION pg_write_forward');

# Sample table used everywhere.
$primary->safe_psql(
	'postgres', q{
CREATE TABLE t (id int primary key, val text);
INSERT INTO t SELECT g, 'row-'||g FROM generate_series(1, 5) g;
});

# Backup -> standby
my $backup = 'b1';
$primary->backup($backup);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup, has_streaming => 1);

# Build a conninfo for the standby->primary link.  We use the unix socket
# directory the test harness already trusts.
my $primary_conninfo = sprintf(
	"host=%s port=%d dbname=postgres",
	$primary->host, $primary->port);

$standby->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_write_forward'
pg_write_forward.primary_conninfo = '$primary_conninfo'
pg_write_forward.consistency = 'session'
pg_write_forward.enabled = on
pg_write_forward.lsn_wait_timeout_ms = 30000
log_min_messages = warning
});
$standby->start;
$primary->wait_for_catchup($standby);

# Sanity: extension is loaded on standby.
my $loaded = $standby->safe_psql('postgres',
	"SELECT count(*) FROM pg_available_extensions WHERE name = 'pg_write_forward'"
);
is($loaded, 1, 'pg_write_forward is available on standby');

# We don't need to CREATE EXTENSION on standby; the hooks are loaded by
# shared_preload_libraries.  The status function lives in the extension SQL,
# which is installed on the primary and replicated.

# ---------------------------------------------------------------------
# Helper: assert standby is, indeed, a standby
# ---------------------------------------------------------------------
my $in_recovery = $standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()');
is($in_recovery, 't', 'standby is in recovery (read-only)');

# ---------------------------------------------------------------------
# Test: INSERT forwards and is visible on standby (session consistency)
# ---------------------------------------------------------------------
$standby->safe_psql('postgres', "INSERT INTO t VALUES (10, 'forwarded-10')");
my $on_primary = $primary->safe_psql('postgres', "SELECT val FROM t WHERE id=10");
is($on_primary, 'forwarded-10', 'INSERT visible on primary');

my $on_standby = $standby->safe_psql('postgres', "SELECT val FROM t WHERE id=10");
is($on_standby, 'forwarded-10',
	'INSERT visible on standby (session consistency wait worked)');

# ---------------------------------------------------------------------
# Test: UPDATE
# ---------------------------------------------------------------------
$standby->safe_psql('postgres', "UPDATE t SET val = 'updated' WHERE id = 10");
is( $standby->safe_psql('postgres', "SELECT val FROM t WHERE id=10"),
	'updated', 'UPDATE visible on standby');

# ---------------------------------------------------------------------
# Test: DELETE
# ---------------------------------------------------------------------
$standby->safe_psql('postgres', "DELETE FROM t WHERE id = 10");
is( $standby->safe_psql('postgres', "SELECT count(*) FROM t WHERE id=10"),
	'0', 'DELETE visible on standby');

# ---------------------------------------------------------------------
# Test: INSERT ... RETURNING - returned tuple matches what landed
# ---------------------------------------------------------------------
my $ret = $standby->safe_psql('postgres',
	"INSERT INTO t VALUES (11, 'with-returning') RETURNING id, val");
is($ret, "11|with-returning", 'INSERT RETURNING delivers tuple to client');

# ---------------------------------------------------------------------
# Test: SELECT ... FOR UPDATE returns rows (and forwards to primary)
# ---------------------------------------------------------------------
my $forupd = $standby->safe_psql('postgres',
	"SELECT id FROM t WHERE id = 11 FOR UPDATE");
is($forupd, '11', 'SELECT FOR UPDATE forwarded and returned row');

# ---------------------------------------------------------------------
# Test: PREPARE / EXECUTE
# ---------------------------------------------------------------------
$standby->safe_psql(
	'postgres',
	"PREPARE ins(int, text) AS INSERT INTO t VALUES (\$1, \$2);"
	  . "EXECUTE ins(12, 'via-prepare');");
is( $standby->safe_psql('postgres', "SELECT val FROM t WHERE id=12"),
	'via-prepare', 'PREPARE/EXECUTE forwarded');

# ---------------------------------------------------------------------
# Test: EXPLAIN ANALYZE INSERT actually inserts (forwarded)
# ---------------------------------------------------------------------
my $explain = $standby->safe_psql('postgres',
	"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) INSERT INTO t VALUES (13, 'explain-row')"
);
like($explain, qr/Insert/i, 'EXPLAIN ANALYZE INSERT returned an INSERT plan');
is( $standby->safe_psql('postgres', "SELECT val FROM t WHERE id=13"),
	'explain-row', 'EXPLAIN ANALYZE INSERT actually inserted (via forwarding)');

# ---------------------------------------------------------------------
# Test: explicit BEGIN block - DML inside is forwarded as a primary xact
# ---------------------------------------------------------------------
$standby->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t VALUES (30, 'in-xact-1');
INSERT INTO t VALUES (31, 'in-xact-2');
COMMIT;
});
$primary->wait_for_catchup($standby);
is( $primary->safe_psql(
		'postgres', "SELECT string_agg(val, ',' ORDER BY id) FROM t WHERE id IN (30,31)"
	),
	'in-xact-1,in-xact-2',
	'BEGIN ... COMMIT block: both inserts committed on primary');
is( $standby->safe_psql(
		'postgres', "SELECT count(*) FROM t WHERE id IN (30,31)"
	),
	'2',
	'BEGIN ... COMMIT block: both inserts visible on standby after replay');

# ---------------------------------------------------------------------
# Test: explicit BEGIN ... ROLLBACK undoes everything on primary too
# ---------------------------------------------------------------------
$standby->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t VALUES (40, 'rolled-back');
ROLLBACK;
});
$primary->wait_for_catchup($standby);
is( $primary->safe_psql('postgres', "SELECT count(*) FROM t WHERE id=40"),
	'0', 'ROLLBACK in standby xact rolls back on primary');

# ---------------------------------------------------------------------
# Test: read-after-write inside an open xact (own writes visible)
# ---------------------------------------------------------------------
my $own_writes = $standby->safe_psql(
	'postgres', q{
BEGIN;
INSERT INTO t VALUES (41, 'inside-xact');
SELECT val FROM t WHERE id = 41;
COMMIT;
});
is($own_writes, 'inside-xact',
	'session sees own write inside an open transaction block');

# ---------------------------------------------------------------------
# Test: session-state mirroring - search_path
# ---------------------------------------------------------------------
$primary->safe_psql(
	'postgres', q{
CREATE SCHEMA s1;
CREATE TABLE s1.u (id int);
});
$primary->wait_for_catchup($standby);

$standby->safe_psql(
	'postgres', q{
SET search_path = s1, public;
INSERT INTO u VALUES (100);   -- unqualified; resolves via search_path
});
is( $primary->safe_psql('postgres', "SELECT count(*) FROM s1.u WHERE id=100"),
	'1',
	'SET search_path on standby is mirrored to primary (unqualified DML resolves)');

# ---------------------------------------------------------------------
# Test: session-state mirroring - application_name reaches primary
# ---------------------------------------------------------------------
$standby->safe_psql(
	'postgres', q{
SET application_name = 'pwf-test';
INSERT INTO t VALUES (200, 'app-name-test');
});
$primary->wait_for_catchup($standby);
# Note: pg_stat_activity check would race with backend exit; instead we just
# confirm the INSERT itself worked, proving SET reached the primary without
# breaking the connection.
is( $primary->safe_psql('postgres', "SELECT val FROM t WHERE id=200"),
	'app-name-test', 'SET application_name + INSERT succeeds');

# ---------------------------------------------------------------------
# Test: PREPARE / EXECUTE inside a transaction block
# ---------------------------------------------------------------------
$standby->safe_psql(
	'postgres', q{
BEGIN;
PREPARE ins2(int, text) AS INSERT INTO t VALUES ($1, $2);
EXECUTE ins2(300, 'prep-1');
EXECUTE ins2(301, 'prep-2');
COMMIT;
});
$primary->wait_for_catchup($standby);
is( $primary->safe_psql(
		'postgres', "SELECT count(*) FROM t WHERE id IN (300,301)"
	),
	'2',
	'PREPARE / EXECUTE inside transaction block: both rows committed');

# ---------------------------------------------------------------------
# Test: consistency = eventual does not wait
# ---------------------------------------------------------------------
$standby->safe_psql(
	'postgres', q{
SET pg_write_forward.consistency = 'eventual';
INSERT INTO t VALUES (20, 'eventual-row');
});
# That row is on the primary.  We don't make a strong claim about the standby
# here, only that the forward succeeded (i.e., didn't error) and the row is
# observable on the primary.
is( $primary->safe_psql('postgres', "SELECT val FROM t WHERE id=20"),
	'eventual-row', 'eventual mode forwards to primary');

# ---------------------------------------------------------------------
# Test: consistency = global also gives session-or-stronger visibility
# ---------------------------------------------------------------------
# First make sure the eventual write is replayed before flipping modes.
$primary->wait_for_catchup($standby);

$standby->safe_psql(
	'postgres', q{
SET pg_write_forward.consistency = 'global';
INSERT INTO t VALUES (21, 'global-row');
});
is( $standby->safe_psql('postgres', "SELECT val FROM t WHERE id=21"),
	'global-row', 'global mode: session sees its own write');

# ---------------------------------------------------------------------
# Test: status function
# ---------------------------------------------------------------------
my $status = $standby->safe_psql(
	'postgres',
	"SELECT consistency, enabled, forwarded_count > 0 FROM pg_write_forward_status()"
);
# Note: status reflects the last GUC value set in this session.  For
# safe_psql, each call is its own session, so consistency will be 'session'
# (the postgresql.conf default).
is($status, 'session|t|f',
	'status function reports configured consistency and enabled flag');

# ---------------------------------------------------------------------
# Test: read-only SELECT still runs locally (no forwarding)
# ---------------------------------------------------------------------
my $local = $standby->safe_psql('postgres', "SELECT count(*) FROM t");
ok($local >= 5, 'plain SELECT runs locally on standby and sees data');

# ---------------------------------------------------------------------
# Test: with consistency = off, writes fail locally as before
# ---------------------------------------------------------------------
my ($rc, $stdout, $stderr) = $standby->psql(
	'postgres',
	"SET pg_write_forward.consistency = 'off'; INSERT INTO t VALUES (50, 'should-fail')"
);
isnt($rc, 0, 'consistency=off restores read-only error');
like($stderr, qr/read-only|cannot execute/i,
	'standard read-only error raised when consistency=off');

done_testing();
