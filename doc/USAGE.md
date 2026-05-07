# pg_write_forward - feature usage cookbook

All examples assume the extension is loaded on the standby per
[INSTALL.md](INSTALL.md), and the application is connected to the
**standby**.

## 1. The basic case: forward a write

```sql
-- session is connected to the standby
=> SET pg_write_forward.consistency = 'session';
=> INSERT INTO orders (customer_id, total) VALUES (42, 19.99);
INSERT 0 1
=> SELECT * FROM orders WHERE customer_id = 42;
 id | customer_id | total
----+-------------+-------
 17 |          42 | 19.99
```

The `INSERT` ran on the primary; the subsequent `SELECT` ran on the
standby and saw the new row because `session` mode waited for the
primary's LSN to be replayed locally.

## 2. Choosing a consistency mode

| mode | wait? | session sees own writes? | when to use |
|---|---|---|---|
| `off` | n/a (forwarding disabled) | n/a | default; turn forwarding off |
| `eventual` | no | not guaranteed | fire-and-forget; UI hint, audit log |
| `session` | yes, until our LSN | **yes** | typical OLTP read-after-write |
| `global` | yes, until primary's current LSN | yes + sees others' writes | reports / dashboards needing fresh data |

```sql
SET pg_write_forward.consistency = 'eventual';   -- no wait
SET pg_write_forward.consistency = 'session';    -- wait for own LSN
SET pg_write_forward.consistency = 'global';     -- wait for primary's current LSN
```

You can `SET` per-statement, per-transaction, per-session, or via
`ALTER ROLE app_user SET pg_write_forward.consistency = 'session'`.

## 3. RETURNING

```sql
=> INSERT INTO orders (customer_id, total)
        VALUES (42, 19.99)
        RETURNING id, total * 0.1 AS tax;
 id | tax
----+-------
 18 | 1.999
```

`RETURNING` works for `INSERT`, `UPDATE`, `DELETE`, and `MERGE`. The rows
come back from the primary.

## 4. Locking SELECTs

```sql
BEGIN;
SELECT * FROM orders WHERE id = 18 FOR UPDATE;
-- forwarded; row is locked on the *primary*
UPDATE orders SET total = total * 1.1 WHERE id = 18;
COMMIT;
```

`SELECT ... FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE / FOR KEY SHARE`
are all forwarded. The lock lives on the primary for the duration of the
forwarded transaction.

## 5. Explicit transactions

The first forwardable statement inside `BEGIN` triggers a lazy `BEGIN`
on the primary (mirroring the local isolation level). All subsequent
statements in the block — **including plain SELECTs** — are forwarded so
the session observes its own uncommitted writes.

```sql
BEGIN ISOLATION LEVEL REPEATABLE READ;
INSERT INTO orders (customer_id, total) VALUES (99, 5.00) RETURNING id;  -- forwarded
SELECT total FROM orders WHERE customer_id = 99;                          -- forwarded (sees the insert)
COMMIT;                                                                   -- forwarded; LSN-wait per consistency mode
```

`ROLLBACK` is forwarded too; the primary aborts cleanly.

## 6. Prepared statements

```sql
PREPARE upd(int, numeric) AS UPDATE orders SET total = $2 WHERE id = $1;
EXECUTE upd(18, 21.99);   -- forwarded
EXECUTE upd(19, 30.00);   -- forwarded; reuses cached primary connection
```

`PREPARE` / `EXECUTE` of any forwardable statement work transparently.
The prepared name is local to the standby session; under the hood the
primary sees the expanded SQL on each `EXECUTE`.

## 7. EXPLAIN ANALYZE of a write

```sql
EXPLAIN (ANALYZE, BUFFERS)
  INSERT INTO orders (customer_id, total) VALUES (default, 1.00);
```

`EXPLAIN ANALYZE` of an `INSERT` / `UPDATE` / `DELETE` / `MERGE` is
forwarded; the timing and buffer numbers come from the primary.

## 8. MERGE

```sql
MERGE INTO orders AS o
USING (VALUES (18, 25.00), (99, 7.50)) AS s(id, total)
ON o.id = s.id
WHEN MATCHED THEN UPDATE SET total = s.total
WHEN NOT MATCHED THEN INSERT (id, total) VALUES (s.id, s.total);
```

Full `MERGE` (with `RETURNING` if desired) is forwarded.

## 9. Per-session GUCs

The standby session's GUCs that can affect the **result** are mirrored
to the primary on each fresh primary connection:

`search_path`, `role`, `session_authorization`, `application_name`,
`timezone`, `datestyle`, `intervalstyle`, `extra_float_digits`,
`statement_timeout`, `lock_timeout`,
`idle_in_transaction_session_timeout`.

Explicit `SET` / `RESET` you issue on the standby is intercepted, applied
locally **and** mirrored live, then remembered so the same state is
re-applied if the primary connection has to be rebuilt.

```sql
SET search_path = app, public;
SET TIME ZONE 'Asia/Kolkata';
SET application_name = 'forwarder-test';
INSERT INTO orders ...   -- runs on primary with the same search_path / TZ / app_name
RESET ALL;               -- clears remembered list; primary picks up its own defaults
```

`pg_write_forward.*` GUCs themselves are deliberately **not** mirrored,
to avoid feedback loops.

## 10. Disabling forwarding ad-hoc

```sql
-- temporarily off for one session
SET pg_write_forward.consistency = 'off';

-- or use the kill switch
SET pg_write_forward.enabled = off;
```

With either, the standby reverts to the usual
`ERROR: cannot execute INSERT in a read-only transaction` for writes.
Read-only statements are unaffected.

## 11. Inspecting state

```sql
SELECT * FROM pg_write_forward_status();
```

Returns one row per backend with:

| column | meaning |
|---|---|
| `primary_conninfo` | current GUC value |
| `consistency` | current GUC value |
| `enabled` | current GUC value |
| `connected` | whether this backend has a live libpq session to the primary |
| `forwarded_count` | total forwarded statements in this backend |
| `last_remote_lsn` | most recent LSN observed on the primary by this backend |

```sql
SELECT pg_write_forward_disconnect();
```

Drops the cached libpq connection. Useful after credential rotation or
failover in already-open sessions.

## 12. What does NOT get forwarded

| statement | behavior |
|---|---|
| DDL (`CREATE`, `ALTER`, `DROP`, ...) | not forwarded; raises read-only error. Run on primary. |
| `SAVEPOINT` / `ROLLBACK TO` | local only; not mirrored. Avoid in forwarded transactions. |
| Cursors (`DECLARE`, `FETCH`, `MOVE`) | not forwarded. |
| `PREPARE TRANSACTION` (2PC) | not forwarded. |
| `LISTEN` / `NOTIFY` | not forwarded. |
| Anything touching standby-local objects (e.g. local `TEMP`) | local; standby is read-only, will fail. |
| `COPY ... FROM` | not forwarded in v1.0; raises read-only error. |

## 13. Common patterns

### Default-on for an application user

```sql
ALTER ROLE app SET pg_write_forward.consistency = 'session';
```

Every connection by `app` to the standby gets read-after-write
automatically; nothing in the application changes.

### Read-replica that occasionally needs to write

Leave `consistency = off` (default). When the application needs to write,
have it `SET pg_write_forward.consistency = 'session'` for the duration
of that transaction:

```sql
BEGIN;
SET LOCAL pg_write_forward.consistency = 'session';
INSERT INTO audit_log ...;
SELECT * FROM audit_log WHERE ...;
COMMIT;
```

`SET LOCAL` confines the change to the current transaction.

### Reporting against fresh data

```sql
SET pg_write_forward.consistency = 'global';
SELECT count(*) FROM orders WHERE created_at > now() - interval '10 seconds';
```

`global` forces a wait until the standby has caught up to the primary's
current LSN, giving a snapshot fresher than the standby's normal replay
lag.

## 14. Operational gotchas

- **One libpq connection per backend.** Many concurrent forwarders ⇒
  many primary connections. Put pgbouncer between the standby and the
  primary for fan-in.
- **`pg_stat_statements` on the primary** sees forwarded statements
  attributed to the `pg_write_forward` libpq session, not to the
  originating standby session. Aggregate by `application_name` if you
  set it via the mirrored GUC.
- **Latency cost**: each forward = 1 RTT to primary + 1 LSN-capture
  query, plus an LSN-replay wait if `consistency` is `session` or
  `global`. Expect single-digit milliseconds in a typical LAN setup.
- **Failover**: see [INSTALL.md § Failover](INSTALL.md#failover--primary-swap).
