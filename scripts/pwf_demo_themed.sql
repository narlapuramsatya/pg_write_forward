-- pwf_demo_themed.sql
-- ============================================================================
--   📡  pg_write_forward  ::  RELAY STATION  ::  themed walkthrough
-- ============================================================================
-- Don't run this directly with psql; use scripts/pwf_demo_themed.sh which
-- injects ANSI colour variables and PAUSE handling.
\set ECHO queries
\timing on

-- ─── pause helper ────────────────────────────────────────────────────────────
\if :{?pause}
\else
  \set pause 1
\endif

-- ─── ASCII banner ────────────────────────────────────────────────────────────
\echo
\echo :BLU '   ╔══════════════════════════════════════════════════════════════════╗' :RST
\echo :BLU '   ║' :BG_BLU '         📡   p g _ w r i t e _ f o r w a r d   📡                 ' :RST :BLU '║' :RST
\echo :BLU '   ║' :CYN '         the standby that secretly forwards your writes           ' :RST :BLU '║' :RST
\echo :BLU '   ╚══════════════════════════════════════════════════════════════════╝' :RST
\echo
\echo :DIM '       client ──INSERT──▶ ' :RST :YLW '🛰  STANDBY' :RST :DIM ' ───relay──▶ ' :RST :GRN '🏛  PRIMARY' :RST
\echo :DIM '                                  ▲                              │     ' :RST
\echo :DIM '                                  └──── streaming replication ◀──┘     ' :RST
\echo

-- ============================================================================
-- 🛰  SCENE 0  ::  pre-flight check
-- ============================================================================
\echo :BG_BLU ' SCENE 0 ' :RST :BLD ' Pre-flight check — are we really on the standby? ' :RST
SELECT pg_is_in_recovery()                                  AS on_standby,
       current_setting('shared_preload_libraries')          AS preload,
       current_setting('pg_write_forward.consistency')      AS consistency_mode;

\echo :CYN '── current relay status ──' :RST
SELECT * FROM pg_write_forward_status();

\if :pause \prompt '   ⏎  press ENTER for SCENE 1 (bootstrap schema) … ' _ \endif

-- ============================================================================
-- 🏛  SCENE 1  ::  bootstrap schema directly on the primary
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 1 ' :RST :BLD ' Bootstrap schema on the primary (DDL is NOT forwarded) ' :RST
\echo :DIM '   Why? DDL would need catalog locks the standby cannot replay.' :RST
\echo :DIM '   We connect to the primary directly to set the stage.' :RST

\connect "host=/home/azureuser/pg/pwf_demo/sockets port=55501 user=postgres dbname=postgres"
\echo :GRN '   ✓ connected to primary' :RST
DROP TABLE IF EXISTS pwf_demo;
CREATE TABLE pwf_demo (id serial PRIMARY KEY, payload text);
INSERT INTO pwf_demo (payload) VALUES ('written-directly-on-primary');

\connect "host=/home/azureuser/pg/pwf_demo/sockets port=55502 user=postgres dbname=postgres"
\echo :YLW '   ↩ back on the standby' :RST
SELECT pg_sleep(0.3);
\d pwf_demo

\if :pause \prompt '   ⏎  press ENTER for SCENE 2 (the read-only wall) … ' _ \endif

-- ============================================================================
-- 🚧  SCENE 2  ::  the read-only wall (extension OFF)
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 2 ' :RST :BLD ' INSERT with forwarding OFF — expect a wall ' :RST
\echo :DIM '   The standby is read-only by default; INSERT must fail.' :RST

SET pg_write_forward.enabled = off;
\set ON_ERROR_STOP off
INSERT INTO pwf_demo (payload) VALUES ('this should fail');
\set ON_ERROR_STOP on
\echo :RED '   🚧 expected: cannot execute INSERT in a read-only transaction' :RST

\if :pause \prompt '   ⏎  press ENTER for SCENE 3 (open the relay) … ' _ \endif

-- ============================================================================
-- 📡  SCENE 3  ::  open the relay (SESSION mode)
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 3 ' :RST :BLD ' Flip the relay ON and INSERT from the standby ' :RST

SET pg_write_forward.enabled = on;
INSERT INTO pwf_demo (payload) VALUES ('hello-from-standby');
\echo :GRN '   📡 INSERT relayed primary→standby; row should now be visible:' :RST
SELECT count(*) AS rows_visible_on_standby FROM pwf_demo;

\if :pause \prompt '   ⏎  press ENTER for SCENE 4 (UPDATE / DELETE / MERGE) … ' _ \endif

-- ============================================================================
-- ✏️  SCENE 4  ::  UPDATE / DELETE / MERGE all relay
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 4 ' :RST :BLD ' UPDATE, DELETE, MERGE — every write hops to the primary ' :RST

UPDATE pwf_demo SET payload = upper(payload) WHERE id = 1;
DELETE FROM pwf_demo WHERE payload LIKE '%-locked%';

-- keep MERGE source ids out of the primary serial sequence's way
-- (setval() isn't a forwarded statement, so we bump it on the primary)
\connect "host=/home/azureuser/pg/pwf_demo/sockets port=55501 user=postgres dbname=postgres"
SELECT setval(pg_get_serial_sequence('pwf_demo','id'),
              GREATEST((SELECT max(id) FROM pwf_demo), 100));
\connect "host=/home/azureuser/pg/pwf_demo/sockets port=55502 user=postgres dbname=postgres"
SET pg_write_forward.enabled = on;
MERGE INTO pwf_demo d
USING (VALUES ('merged-row-locked'), ('another-merged')) AS s(p)
   ON false
 WHEN NOT MATCHED THEN INSERT (payload) VALUES (s.p);

SELECT id, payload FROM pwf_demo ORDER BY id;

\if :pause \prompt '   ⏎  press ENTER for SCENE 5 (locking SELECT) … ' _ \endif

-- ============================================================================
-- 🔒  SCENE 5  ::  locking SELECT FOR UPDATE
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 5 ' :RST :BLD ' SELECT … FOR UPDATE — lock taken on the primary ' :RST

BEGIN;
SELECT id, payload FROM pwf_demo WHERE id = 1 FOR UPDATE;
\echo :MAG '   🔒 row locked on primary; lock released on COMMIT' :RST
COMMIT;

\if :pause \prompt '   ⏎  press ENTER for SCENE 6 (PREPARE / EXECUTE) … ' _ \endif

-- ============================================================================
-- 📨  SCENE 6  ::  PREPARE / EXECUTE
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 6 ' :RST :BLD ' Prepared writes relay just like ad-hoc ones ' :RST

PREPARE ins (text) AS INSERT INTO pwf_demo (payload) VALUES ($1);
EXECUTE ins('via-prepared-1');
EXECUTE ins('via-prepared-2');

\if :pause \prompt '   ⏎  press ENTER for SCENE 7 (EXPLAIN ANALYZE) … ' _ \endif

-- ============================================================================
-- 🔬  SCENE 7  ::  EXPLAIN ANALYZE of a write
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 7 ' :RST :BLD ' EXPLAIN ANALYZE INSERT — timings come from the primary ' :RST

EXPLAIN (ANALYZE, BUFFERS, VERBOSE)
INSERT INTO pwf_demo (payload) VALUES ('explained');

\if :pause \prompt '   ⏎  press ENTER for SCENE 8 (multi-stmt xact) … ' _ \endif

-- ============================================================================
-- 🧵  SCENE 8  ::  multi-statement transaction (self-visibility)
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 8 ' :RST :BLD ' Multi-statement transaction sees its own writes ' :RST

BEGIN;
INSERT INTO pwf_demo (payload) VALUES ('xact-row-A');
INSERT INTO pwf_demo (payload) VALUES ('xact-row-B');
SELECT id, payload FROM pwf_demo
 WHERE payload LIKE 'xact-row-%' ORDER BY id;
COMMIT;

\if :pause \prompt '   ⏎  press ENTER for SCENE 9 (GUC mirroring) … ' _ \endif

-- ============================================================================
-- 🪞  SCENE 9  ::  session GUC mirroring
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 9 ' :RST :BLD ' search_path / application_name / TZ mirror to primary ' :RST

SET application_name = 'pwf-demo-mirrored';
SET TIME ZONE 'Asia/Tokyo';
INSERT INTO pwf_demo (payload)
VALUES ('current_setting(application_name)='
        || current_setting('application_name'));
RESET TIME ZONE;

\if :pause \prompt '   ⏎  press ENTER for SCENE 10 (GLOBAL consistency) … ' _ \endif

-- ============================================================================
-- 🌐  SCENE 10 ::  GLOBAL consistency
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 10 ' :RST :BLD ' GLOBAL: the standby waits for replay to catch up ' :RST

SET pg_write_forward.consistency = global;
\echo :CYN '   primary writes a row directly; standby must see it after replay' :RST
\connect "host=/home/azureuser/pg/pwf_demo/sockets port=55501 user=postgres dbname=postgres"
INSERT INTO pwf_demo (payload) VALUES ('written-directly-on-primary-2');
\connect "host=/home/azureuser/pg/pwf_demo/sockets port=55502 user=postgres dbname=postgres"
SET pg_write_forward.consistency = global;
SET pg_write_forward.enabled    = on;
-- a forwarded write drags our replay LSN to >= primary's
INSERT INTO pwf_demo (payload) VALUES ('global-mode-probe');
SELECT count(*) FILTER (WHERE payload LIKE 'written-directly-on-primary%')
       AS sees_primary_writes
  FROM pwf_demo;

\if :pause \prompt '   ⏎  press ENTER for SCENE 11 (status) … ' _ \endif

-- ============================================================================
-- 📊  SCENE 11 ::  status snapshot
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 11 ' :RST :BLD ' Inspect the relay station ' :RST

SELECT * FROM pg_write_forward_status();

\if :pause \prompt '   ⏎  press ENTER for SCENE 12 (disconnect/reconnect) … ' _ \endif

-- ============================================================================
-- 🔌  SCENE 12 ::  disconnect & reconnect
-- ============================================================================
\echo
\echo :BG_BLU ' SCENE 12 ' :RST :BLD ' Drop the relay link and bring it back ' :RST

SELECT pg_write_forward_disconnect();
SELECT * FROM pg_write_forward_status();   -- connected = f
INSERT INTO pwf_demo (payload) VALUES ('after-reconnect');
SELECT * FROM pg_write_forward_status();   -- connected = t again

\if :pause \prompt '   ⏎  press ENTER for the curtain call … ' _ \endif

-- ============================================================================
-- 🎬  CURTAIN  ::  final table contents
-- ============================================================================
\echo
\echo :BG_GRN ' CURTAIN ' :RST :BLD ' Every row below was written from the standby (or globally) ' :RST
SELECT id, payload FROM pwf_demo ORDER BY id;

\echo
\echo :GRN '   ✅  demo complete — relay station signing off' :RST
\echo :DIM '   tip: re-run with --no-pause for a fast replay,' :RST
\echo :DIM '        or --tee out.log to capture a transcript.' :RST
\echo
