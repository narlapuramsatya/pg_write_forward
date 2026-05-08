/* pg_write_forward--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_write_forward" to load this file. \quit

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

CREATE FUNCTION pg_write_forward_disconnect()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_write_forward_disconnect'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_write_forward_disconnect() IS
  'Close the per-session connection to the primary.  '
  'Restricted to superusers.';

-- Inner C also enforces superuser; revoke from PUBLIC for defence in depth.
REVOKE EXECUTE ON FUNCTION pg_write_forward_disconnect() FROM PUBLIC;
