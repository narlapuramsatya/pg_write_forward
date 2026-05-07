# pg_write_forward

Forward write statements from a hot standby to the primary, transparently.

This is a standalone PostgreSQL extension. It lets a read-only hot standby
accept `INSERT`, `UPDATE`, `DELETE`, `MERGE`, locking `SELECT`s, and
`PREPARE`/`EXECUTE`/`EXPLAIN ANALYZE` of any of the above. It opens a
libpq connection to the primary, re-runs the verbatim SQL there,
optionally waits until the primary's resulting LSN is replayed locally,
and returns the primary's results to the client as if the statement had
run locally.

Read-only statements are unaffected and execute on the standby as usual.

Conceptually similar to AWS Aurora's "write forwarding" feature.

## Repository layout

```
pg_write_forward/
├── Makefile                       PGXS-based build
├── pg_write_forward.control       Extension control file
├── README.md                      This file
├── src/
│   └── pg_write_forward.c         C source
├── sql/
│   └── pg_write_forward--1.0.sql  Catalog objects
├── t/
│   └── 001_basic.pl               TAP test (primary+standby)
└── doc/
    ├── INSTALL.md                 Build / install / configure
    ├── USAGE.md                   Cookbook of features
    └── DESIGN.md                  Internals + architecture
```

## Quick start

```bash
make PG_CONFIG=/path/to/pg_config
sudo make PG_CONFIG=/path/to/pg_config install
```

Add to the **standby's** `postgresql.conf`:

```
shared_preload_libraries = 'pg_write_forward'
pg_write_forward.primary_conninfo = 'host=primary-host port=5432 dbname=postgres user=...'
pg_write_forward.consistency      = 'session'
```

Restart the standby, then in any database where you need the helpers:

```sql
CREATE EXTENSION pg_write_forward;
```

After this, applications connected to the standby can issue writes
normally; they are forwarded to the primary and (with `consistency =
session`) the session immediately sees its own writes on the standby.

See [doc/INSTALL.md](doc/INSTALL.md) for the full install guide,
[doc/USAGE.md](doc/USAGE.md) for a feature-by-feature cookbook, and
[doc/DESIGN.md](doc/DESIGN.md) for the architecture and rationale.

## Requirements

- PostgreSQL 17 or newer (uses `xlogwait.h` API for LSN-replay waits).
- libpq development headers (already provided by any PostgreSQL build).
- Perl + `IPC::Run` for running the TAP test.

## Tests

```bash
make PG_CONFIG=/path/to/pg_config check
```

Brings up a primary + streaming hot standby, configures the standby to
forward, and exercises every supported statement class against all three
non-`off` consistency modes.

## License

PostgreSQL License (same terms as the PostgreSQL server).
