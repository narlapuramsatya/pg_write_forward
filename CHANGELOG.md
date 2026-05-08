# Changelog

All notable changes to **pg_write_forward** are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [1.1] — production hardening

### Added
- **Cancellable forwarding.** All statements forwarded to the primary
  now run via the asynchronous libpq API. A local `Ctrl-C` (or backend
  termination) sends a `PQcancel()` to the primary instead of leaving
  an orphaned long-running query there.
- **Reconnect-once on broken connection.** If the primary connection
  drops between statements outside an open primary-side transaction,
  the next forwarded statement transparently reconnects and retries.
  Counted in the new `reconnects` column.
- **New status counters:** `forwarded_failures`, `cancellations`,
  `reconnects` exposed by `pg_write_forward_status()`.
- **`pg_monitor` access** to `pg_write_forward_status()` (with
  `primary_conninfo` redacted for non-superusers).
- TAP test `002_hardening.pl` covering all of the above.
- `LICENSE` (PostgreSQL license), `CHANGELOG.md`, `doc/SECURITY.md`,
  GitHub Actions CI workflow.

### Changed
- **`pg_write_forward_status()` signature** now returns 9 columns
  (was 6).  Existing 1.0 installs are upgraded by
  `ALTER EXTENSION pg_write_forward UPDATE TO '1.1'`.
- **`primary_conninfo` is redacted** in the status view for
  non-superusers (previously visible to anyone who could call the
  function — exposed embedded passwords).
- **`pg_write_forward_disconnect()` now requires superuser** (was
  callable by any role; could disrupt the primary connection or force
  re-authentication races).
- `EXECUTE` privilege on both functions revoked from `PUBLIC` (was
  default).

### Refused (with explicit error)
The following statement classes are now hard-refused while forwarding
is active. Previously they were silently passed through to local
execution, which produced incorrect or unsafe behaviour:

- `SAVEPOINT` / `RELEASE` / `ROLLBACK TO` — would split the primary-side
  transaction into sub-transactions that are not mirrored.
- `PREPARE TRANSACTION` (two-phase commit) — cannot be coordinated
  across two servers without a transaction manager.
- `LISTEN` / `NOTIFY` / `UNLISTEN` — would set up channels on the
  primary that the standby session cannot consume.
- `COPY ... FROM` — bulk import is unsupported in 1.x; run on the
  primary or use INSERTs.
- `DECLARE CURSOR ... FOR UPDATE/SHARE` — cursor state cannot be
  tracked across the wire.

### Fixed
- Statement failures no longer silently increment `forwarded_count`
  (they now count toward `forwarded_failures` instead).
- Connection-loss errors are no longer reported as "no result from
  primary"; the underlying libpq error message is propagated.

## [1.0] — initial release

Initial extraction from PostgreSQL contrib. Forwards INSERT / UPDATE /
DELETE / MERGE, locking SELECT, PREPARE / EXECUTE, EXPLAIN ANALYZE,
multi-statement transactions, and session GUC mirroring (search_path,
role, application_name, time-zone, etc.). Three consistency modes
(`eventual`, `session`, `global`).
