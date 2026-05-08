/* pg_write_forward--1.0--1.1.sql */
-- Upgrade pg_write_forward from 1.0 to 1.1.

\echo Use "ALTER EXTENSION pg_write_forward UPDATE" to load this file. \quit

-- Status function gained three new columns: forwarded_failures,
-- cancellations, reconnects.  Output-parameter changes require DROP +
-- CREATE in PostgreSQL.
DROP FUNCTION IF EXISTS pg_write_forward_status();

CREATE FUNCTION pg_write_forward_status(
    OUT primary_conninfo text,
    OUT consistency text,
    OUT enabled bool,
    OUT connected bool,
    OUT forwarded_count bigint,
    OUT last_remote_lsn pg_lsn,
    OUT forwarded_failures bigint,
    OUT cancellations bigint,
    OUT reconnects bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_write_forward_status'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_write_forward_status() IS
  'Per-session status and counters for pg_write_forward. '
  'primary_conninfo is redacted for non-superusers.';

REVOKE EXECUTE ON FUNCTION pg_write_forward_status() FROM PUBLIC;
GRANT  EXECUTE ON FUNCTION pg_write_forward_status() TO   pg_monitor;

REVOKE EXECUTE ON FUNCTION pg_write_forward_disconnect() FROM PUBLIC;

COMMENT ON FUNCTION pg_write_forward_disconnect() IS
  'Close the per-session connection to the primary.  '
  'Restricted to superusers.';
