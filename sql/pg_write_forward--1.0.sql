/* contrib/pg_write_forward/pg_write_forward--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_write_forward" to load this file. \quit

CREATE FUNCTION pg_write_forward_status(
    OUT primary_conninfo text,
    OUT consistency text,
    OUT enabled bool,
    OUT connected bool,
    OUT forwarded_count bigint,
    OUT last_remote_lsn pg_lsn
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_write_forward_status'
LANGUAGE C VOLATILE;

CREATE FUNCTION pg_write_forward_disconnect()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_write_forward_disconnect'
LANGUAGE C VOLATILE;
