# pg_write_forward - design and internals

This document describes how `pg_write_forward` works inside a standby
backend, where it hooks into the executor and utility paths, what state
it keeps per backend, and the correctness model of each consistency
mode.

## Goal

Make a hot standby look, to an application, like a writable endpoint
with read-after-write consistency for that session — without changing
the application or the replication topology, and without losing any of
the standby's read scalability for non-write traffic.

## Non-goals

- Multi-master / write-anywhere semantics.
- Cross-shard or cross-database transactions.
- Forwarding DDL.
- 2PC.
- Connection pooling on the standby->primary link (use pgbouncer).

## Architecture

```
   ┌──────────────┐                         ┌──────────────┐
   │   client     │ ─── SQL ──────────────▶ │   standby    │
   └──────────────┘                         │   backend    │
                                            │  (read-only) │
                                            │              │
                                  ┌─────────┤  hooks       │
                                  │         │              │
                          forward │         │  per-session │
                            (libpq)         │  state       │
                                  ▼         └──────────────┘
                              ┌─────────┐
                              │ primary │
                              └─────────┘
                                  │
                                  │ stream WAL
                                  ▼
                              standby applies
                              up to LSN(forward)
```

Every standby backend that issues a forwardable statement maintains its
own libpq connection to the primary. There is no shared cache, no
background worker, no shared memory.

## Hooks

The extension installs three hooks at `_PG_init`:

| hook | purpose |
|---|---|
| `ProcessUtility_hook` | catches `PREPARE`, `EXECUTE`, `EXPLAIN ANALYZE`, transaction-control statements (`BEGIN` / `COMMIT` / `ROLLBACK`), and `SET` / `RESET` for GUC mirroring |
| `ExecutorStart_hook` | catches DML (`INSERT` / `UPDATE` / `DELETE` / `MERGE`) and locking `SELECT`s before the planner-emitted plan starts executing |
| `ExecutorRun_hook` | drains rows from the primary back to the destination receiver when a forwarded query has `RETURNING` or is a locking `SELECT` |

The classification logic decides up-front whether a query is *forwardable*
(write-shaped or locking SELECT) **and** whether the cluster is currently
in recovery (i.e., we are on a standby). If either is false the hook is a
no-op and the statement runs through the normal path.

## Forwardable statement set

```
INSERT, UPDATE, DELETE, MERGE
SELECT ... FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE / FOR KEY SHARE
PREPARE <forwardable>
EXECUTE <prepared name of forwardable>
EXPLAIN ANALYZE <forwardable>
```

Anything else (DDL, plain `SELECT`, cursor commands, `LISTEN/NOTIFY`,
`PREPARE TRANSACTION`, `COPY`) is skipped by the classifier and goes
through the normal path. Plain DDL therefore continues to raise the
usual read-only error.

The classifier walks the parse tree once per statement; cost is
proportional to the size of the top-level node, which is constant for
all the statement classes above.

## Per-backend state

Each backend that has ever forwarded keeps a `WfSession` struct in its
local memory:

```c
typedef struct WfSession {
    PGconn     *conn;            /* libpq connection, NULL if not yet open */
    bool        in_remote_xact;  /* lazy BEGIN was issued on the primary */
    XLogRecPtr  last_remote_lsn; /* observed after most-recent forward */
    int64       forwarded_count; /* incrementing counter for status() */
    HTAB       *mirrored_gucs;   /* name -> latest value, for reconnect */
    /* …classification scratch, error buffers… */
} WfSession;
```

The connection is opened **lazily** on first forward and torn down on
backend exit (`shmem_exit_callback` + `on_proc_exit`).

## Statement lifecycle (single-statement)

```
   client ── INSERT INTO t ─▶ standby backend
                                    │
                                    │  ExecutorStart_hook
                                    ▼
                           classify(query)  → forwardable
                                    │
                                    ▼
                           ensure_remote_session()
                              ├── connect if needed
                              ├── apply mirrored GUCs
                              └── BEGIN if in local xact
                                    │
                                    ▼
                           PQexec(remote, source_text)   ◀── 1 RTT
                                    │
                                    ▼
                           if RETURNING/locking SELECT:
                               stream rows → client's DestReceiver
                                    │
                                    ▼
                           PQexec(remote,
                                  "SELECT pg_current_wal_insert_lsn()")
                                    │       (or pg_last_committed_xact_lsn
                                    │        for COMMIT path)
                                    ▼
                           switch (consistency)
                              eventual : return
                              session  : WaitForLSN(captured_lsn,
                                                   timeout_ms)
                              global   : WaitForLSN(primary_now_lsn,
                                                   timeout_ms)
                                    │
                                    ▼
                              return CommandComplete to client
```

`WaitForLSN` is the upstream `xlogwait.h` API (PG 17+). It blocks the
backend until the standby's last-replayed LSN is `>= target`, with a
timeout that maps onto `pg_write_forward.lsn_wait_timeout_ms`.

## Transaction-block lifecycle

A standby session is, by default, in implicit auto-commit mode. When the
client issues `BEGIN`, the extension marks the session as "in local
xact" but does not contact the primary yet — many transactions will only
ever read.

The first forwardable statement inside the block triggers a lazy
`BEGIN` on the primary, mirroring the local isolation level (and
`DEFERRABLE` if set). From that point on **every** statement in the
block, including read-only `SELECT`s, is forwarded so the session
observes its own uncommitted writes.

```
BEGIN ISOLATION LEVEL REPEATABLE READ      → local: in_local_xact = true
                                              primary: not yet contacted
SELECT count(*) FROM t                     → local: runs as usual
INSERT INTO t VALUES (...)                 → primary: BEGIN ISOLATION ... ; INSERT ...
SELECT * FROM t                            → primary: SELECT * FROM t  (forwarded for self-visibility)
COMMIT                                     → primary: COMMIT ;
                                              capture LSN ;
                                              LSN-wait per consistency
```

`ROLLBACK` mirrors as `ROLLBACK`. If the primary's `COMMIT` fails the
local transaction is converted to an abort and the client sees the
primary's error verbatim.

## Session GUC mirroring

The result of `INSERT INTO t SELECT ... WHERE schema.col = ...` depends
on `search_path`. The result of `now()` depends on `timezone`. To make
forwarding transparent, the extension mirrors a curated list of GUCs:

```
search_path                         role
session_authorization               application_name
timezone                            datestyle
intervalstyle                       extra_float_digits
statement_timeout                   lock_timeout
idle_in_transaction_session_timeout
```

Two cases:

1. **On fresh primary connection.** Pull the local backend's current
   value of each curated GUC and `SET` it on the primary.
2. **On user `SET` / `RESET`.** Intercept in `ProcessUtility_hook`,
   apply locally as usual, then mirror to the primary live and remember
   the new value in the per-backend `mirrored_gucs` hash so it survives
   a reconnect. `RESET ALL` clears the remembered list.

`pg_write_forward.*` GUCs are deliberately excluded so a `SET
pg_write_forward.consistency = 'global'` on the standby doesn't try to
set the same on the primary (where the GUC is also defined but
behaviorally meaningless).

## LSN capture

After the forwarded statement completes, the extension issues:

```sql
-- non-COMMIT path
SELECT pg_current_wal_insert_lsn();

-- COMMIT path (after PQexec("COMMIT") succeeded)
SELECT pg_last_committed_xact_lsn();   -- via pg_stat_activity / pg_xact_commit_timestamp
```

The captured LSN is what `session` mode waits for. `global` mode skips
the second query and waits for the value of `pg_current_wal_insert_lsn`
sampled *after* our forward, which is `>=` everything committed on the
primary up to that point.

In both cases the wait happens on the **standby**, against the local
recovery progress, via `WaitForLSN`. No interaction with the primary
during the wait.

## Consistency model summary

| mode | guarantees on the standby session that did the forward |
|---|---|
| `off` | none (forwarding disabled) |
| `eventual` | the write is durable on the primary; this session may not see it on subsequent reads from this standby |
| `session` | this session sees its own writes |
| `global` | this session sees its own writes **and** every commit on the primary up to the moment of the forward |

Notably none of the modes give cross-session linearizability between
standbys — that would require a coordinator.

## Failure modes

| failure | behavior |
|---|---|
| primary unreachable on first forward | `ERROR`: includes libpq's diagnostic; local xact aborts |
| primary connection dies mid-xact | `ERROR`; local xact aborts; cached connection cleared |
| primary returns `ERROR` | re-raise the primary's error verbatim to the client |
| `WaitForLSN` times out | `ERROR`: "primary LSN <X> not replayed within <T> ms" |
| standby promoted while a forward is in flight | `WaitForLSN` returns; subsequent forwards fail because the local cluster is no longer in recovery — this is correct, the application should reconnect |

All errors propagate via `ereport(ERROR, ...)`, so normal transaction
abort semantics apply.

## Concurrency / locking on the primary

A forwarded `SELECT ... FOR UPDATE` takes its row lock on the **primary**.
Other forwarded sessions doing the same converge on the same primary
locks — this is exactly the behavior the application would see if it had
connected directly to the primary. Standby-local locks are not used by
the forwarded statement (the standby never executes it locally).

## Resource cost per backend

| resource | cost |
|---|---|
| memory | one `PGconn` (~50 KB), `mirrored_gucs` HTAB (~1 KB), classification scratch | 
| open fds | 1 socket to primary |
| connections to primary | 1 per forwarding standby backend |
| CPU per forwarded stmt | parse-tree walk + libpq round-trip + optional LSN wait |
| wall time per forwarded stmt | 1 RTT to primary + LSN capture + LSN-wait (mode-dependent) |

For deployments with many forwarders, run pgbouncer between the standby
and the primary so the standby fans many backend connections into a few
primary sessions.

## Why this design (and not alternatives)

- **A background worker that batches forwards.** Rejected: would force
  asynchronous semantics for `RETURNING` and locking SELECTs, and would
  serialize all standby writes through one process.
- **Logical replication apply.** Rejected: doesn't help with
  read-after-write for the writer's own session, only adds a slower
  replication path.
- **A proxy in front of the standby that re-routes writes.** Equivalent
  in spirit but adds a network hop and a separate operational
  component. `pg_write_forward` is in-process.
- **Catalog / shared cache for primary connections.** Rejected for v1.0:
  per-backend connections keep the security model identical to a direct
  primary connection (each backend authenticates as its own user, with
  its own session GUCs).

## Code map

| file | role |
|---|---|
| `src/pg_write_forward.c` | everything: hooks, classifier, libpq wrapper, GUC mirroring, transaction tracking, LSN waits, helper functions |
| `sql/pg_write_forward--1.0.sql` | `pg_write_forward_status()`, `pg_write_forward_disconnect()` |
| `pg_write_forward.control` | extension metadata, marks the extension `relocatable = true` |
| `t/001_basic.pl` | TAP test bringing up primary+standby and exercising every supported statement class against `eventual`, `session`, and `global` modes |

## Compatibility

- **Server version**: PG 17+. The `xlogwait.h` API
  (`WaitForLSN(target_lsn, timeout)`) is what makes `session` and
  `global` modes possible without busy-polling. On older servers the
  extension would compile but `session`/`global` would have to fall
  back to polling `pg_last_wal_replay_lsn()` in a loop, which is
  intentionally not done.
- **Replication mode**: streaming replication (sync or async). Logical
  standbys are not the target use case.
- **Server platforms**: same as PostgreSQL — Linux/macOS/Windows. The
  `Makefile` is plain PGXS; on Windows use `make.exe` from MSYS or
  build via Meson per the upstream instructions.

## Testing

`t/001_basic.pl` covers:

- All four consistency modes (`off`, `eventual`, `session`, `global`).
- `INSERT` / `UPDATE` / `DELETE` / `MERGE`, with and without
  `RETURNING`.
- `SELECT FOR UPDATE` and `SELECT FOR SHARE`.
- `PREPARE` + `EXECUTE` of a forwardable.
- `EXPLAIN ANALYZE` of a write.
- `BEGIN` / `COMMIT` / `ROLLBACK` over multiple forwarded statements.
- Self-visibility inside an explicit transaction.
- GUC mirroring (set local `search_path`, observe primary uses it).
- `pg_write_forward_status()` / `pg_write_forward_disconnect()` /
  reconnect.
- Read-only paths still error with `consistency = off`.
- Failover survival (`pg_write_forward_disconnect` + new conninfo).
