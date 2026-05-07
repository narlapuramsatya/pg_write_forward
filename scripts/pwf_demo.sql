-- pwf_demo.sql -- Demo of pg_write_forward features.
--
-- Run this against the STANDBY (port 55502 in the demo setup):
--   psql -h /home/azureuser/pg/pwf_demo/sockets -p 55502 -U postgres -f pwf_demo.sql postgres
--
-- Each section is self-explanatory; \echo lines label what's happening.

\set ON_ERROR_STOP on
\timing on
\set ECHO queries

-- The script pauses before each section.  Run with `-v pause=0` to disable
-- pausing (useful for CI / non-interactive runs).
\if :{?pause}
\else
  \set pause 1
\endif

\echo
\echo '================================================================'
\echo ' 0. Sanity: confirm we are on the standby and the extension loaded'
\echo '================================================================'

SELECT pg_is_in_recovery() AS on_standby,
       current_setting('shared_preload_libraries')   AS preload,
       current_setting('pg_write_forward.consistency') AS consistency_mode;

\echo '*** Creating extension on the primary (DDL is not forwarded):'
\! /home/azureuser/pg/install/bin/psql -h /home/azureuser/pg/pwf_demo/sockets -p 55501 -U postgres -c "CREATE EXTENSION IF NOT EXISTS pg_write_forward;" postgres
SELECT pg_sleep(0.3);

SELECT * FROM pg_write_forward_status();

\if :pause \prompt '--- press ENTER for section 1 (bootstrap demo schema) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 1. Demo schema (created on the *primary* by forwarding DDL?    '
\echo '    No - DDL is NOT forwarded.  We bootstrap by creating the    '
\echo '    table on the primary directly.                              '
\echo '================================================================'

\echo '*** Creating demo table on primary via dblink-less workaround:'
\echo '*** (the easy way is to just psql the primary; we do that here)'
\! /home/azureuser/pg/install/bin/psql -h /home/azureuser/pg/pwf_demo/sockets -p 55501 -U postgres -c "DROP TABLE IF EXISTS pwf_demo; CREATE TABLE pwf_demo (id serial PRIMARY KEY, payload text, ts timestamptz DEFAULT now());" postgres

-- Wait for replay so the standby sees the table.
SELECT pg_sleep(0.3);

\echo '*** Standby sees the table:'
\d pwf_demo

\if :pause \prompt '--- press ENTER for section 2 (forward INSERT in OFF mode) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 2. Forward an INSERT in OFF mode (default behaviour)            '
\echo '    Expected: ERROR cannot execute INSERT in a read-only         '
\echo '    transaction                                                  '
\echo '================================================================'

SET pg_write_forward.consistency = 'off';
-- This should fail:
\set ON_ERROR_STOP off
INSERT INTO pwf_demo (payload) VALUES ('this should fail in off mode');
\set ON_ERROR_STOP on

\if :pause \prompt '--- press ENTER for section 3 (INSERT in SESSION mode) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 3. Forward an INSERT in SESSION mode                            '
\echo '    Expected: success; subsequent SELECT sees the row            '
\echo '================================================================'

SET pg_write_forward.consistency = 'session';

INSERT INTO pwf_demo (payload) VALUES ('hello from standby session 1');
INSERT INTO pwf_demo (payload) VALUES ('hello from standby session 2') RETURNING id, payload;

SELECT count(*) AS rows_visible_on_standby FROM pwf_demo;

\if :pause \prompt '--- press ENTER for section 4 (UPDATE / DELETE / MERGE) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 4. UPDATE / DELETE / MERGE all forward                          '
\echo '================================================================'

UPDATE pwf_demo SET payload = upper(payload) WHERE id = 1 RETURNING id, payload;
DELETE FROM pwf_demo WHERE id = 2 RETURNING id, payload;

MERGE INTO pwf_demo AS d
USING (VALUES (3, 'merged-row'), (4, 'another-merged')) AS s(id, payload)
ON d.id = s.id
WHEN MATCHED THEN UPDATE SET payload = s.payload
WHEN NOT MATCHED THEN INSERT (id, payload) VALUES (s.id, s.payload);

-- The MERGE inserted explicit ids; re-sync the sequence on the primary
-- (setval() is a function-level write that the extension does not
-- classify as forwardable, so we run it on the primary directly).
\! /home/azureuser/pg/install/bin/psql -h /home/azureuser/pg/pwf_demo/sockets -p 55501 -U postgres -c "SELECT setval('pwf_demo_id_seq', (SELECT max(id) FROM pwf_demo));" postgres
SELECT pg_sleep(0.2);

SELECT id, payload FROM pwf_demo ORDER BY id;

\if :pause \prompt '--- press ENTER for section 5 (SELECT FOR UPDATE) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 5. Locking SELECT (FOR UPDATE) inside a transaction             '
\echo '    Lock is taken on the *primary* for the duration              '
\echo '================================================================'

BEGIN;
SELECT id, payload FROM pwf_demo WHERE id = 3 FOR UPDATE;
UPDATE pwf_demo SET payload = payload || '-locked' WHERE id = 3 RETURNING id, payload;
COMMIT;

\if :pause \prompt '--- press ENTER for section 6 (PREPARE / EXECUTE) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 6. PREPARE / EXECUTE forwards too                               '
\echo '================================================================'

PREPARE ins(text) AS INSERT INTO pwf_demo (payload) VALUES ($1) RETURNING id;
EXECUTE ins('via-prepared-1');
EXECUTE ins('via-prepared-2');
-- (DEALLOCATE is a local-only utility; not needed and not forwardable.)

\if :pause \prompt '--- press ENTER for section 7 (EXPLAIN ANALYZE of a write) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 7. EXPLAIN ANALYZE of a write forwards too (timings = primary) '
\echo '================================================================'

EXPLAIN (ANALYZE, BUFFERS, COSTS off, TIMING off, SUMMARY off)
INSERT INTO pwf_demo (payload) VALUES ('explained');

\if :pause \prompt '--- press ENTER for section 8 (multi-stmt xact self-visibility) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 8. Multi-statement transaction with self-visibility             '
\echo '================================================================'

BEGIN;
INSERT INTO pwf_demo (payload) VALUES ('xact-row-A');
INSERT INTO pwf_demo (payload) VALUES ('xact-row-B');
-- This SELECT is forwarded to the primary inside the same xact, so it
-- sees uncommitted writes.
SELECT id, payload FROM pwf_demo WHERE payload LIKE 'xact-row-%' ORDER BY id;
COMMIT;

\if :pause \prompt '--- press ENTER for section 9 (session GUC mirroring) --- ' _ \endif
\echo
\echo '================================================================'
\echo ' 9. Session GUC mirroring: search_path, application_name, TZ    '
\echo '================================================================'

SET application_name = 'pwf-demo-mirrored';
SET TIME ZONE 'Asia/Kolkata';
SET search_path = public;

INSERT INTO pwf_demo (payload) VALUES ('current_setting(application_name)=' || current_setting('application_name'));
SELECT id, payload FROM pwf_demo WHERE payload LIKE 'current_setting%' ORDER BY id DESC LIMIT 1;

RESET application_name;
RESET TIME ZONE;

\if :pause \prompt '--- press ENTER for section 10 (GLOBAL consistency) --- ' _ \endif
\echo
\echo '================================================================'
\echo '10. GLOBAL consistency: see other sessions writes too            '
\echo '================================================================'

-- Concurrently write to primary in another session, then in this session
-- a global-mode read sees it.  (consistency=global only forces a wait
-- when *we* forward a write; for a pure local SELECT to see a recent
-- foreign primary write, we either issue a forwarded probe first or
-- briefly wait for replay.  We do the latter for clarity.)
\! /home/azureuser/pg/install/bin/psql -h /home/azureuser/pg/pwf_demo/sockets -p 55501 -U postgres -c "INSERT INTO pwf_demo (payload) VALUES ('written-directly-on-primary');" postgres

SET pg_write_forward.consistency = 'global';
-- Forward a no-op write to drag standby up to primary's current LSN.
INSERT INTO pwf_demo (payload) VALUES ('global-mode-probe');
SELECT count(*) FILTER (WHERE payload = 'written-directly-on-primary') AS sees_primary_write
  FROM pwf_demo;

\if :pause \prompt '--- press ENTER for section 11 (status view) --- ' _ \endif
\echo
\echo '================================================================'
\echo '11. Inspect status                                               '
\echo '================================================================'

SELECT * FROM pg_write_forward_status();

\if :pause \prompt '--- press ENTER for section 12 (disconnect / reconnect) --- ' _ \endif
\echo
\echo '================================================================'
\echo '12. Disconnect (e.g. after primary failover, then reconnect)     '
\echo '================================================================'

SELECT pg_write_forward_disconnect();
SELECT * FROM pg_write_forward_status();   -- connected = f
INSERT INTO pwf_demo (payload) VALUES ('after-reconnect');
SELECT * FROM pg_write_forward_status();   -- connected = t again

\if :pause \prompt '--- press ENTER for final summary --- ' _ \endif
\echo
\echo '================================================================'
\echo ' DONE.  Final table contents:'
\echo '================================================================'

SELECT id, payload FROM pwf_demo ORDER BY id;
