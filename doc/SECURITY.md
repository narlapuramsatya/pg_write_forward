# Security policy

## Reporting a vulnerability

Please report security issues **privately** by opening a
[GitHub security advisory](https://github.com/narlapuramsatya/pg_write_forward/security/advisories/new)
or emailing the maintainer listed in the repo metadata.  Do not file
public issues for unpatched vulnerabilities.

We aim to acknowledge reports within 5 business days and to ship a fix
or mitigation within 30 days for high-severity issues.

## Threat model

`pg_write_forward` runs in every backend on a hot standby that has it
loaded via `shared_preload_libraries`.  The threat model assumes:

1. The standby and primary are operated by the same trust domain.
   Network traffic between them is assumed to be authenticated and
   either encrypted (TLS) or on a trusted network.
2. The primary's `pg_hba.conf` allows the standby's identity to log in
   as the configured user.  Use SCRAM or certificate auth, not `trust`.
3. Operators are responsible for keeping `pg_write_forward.primary_conninfo`
   secret.  See **Credentials** below.

## Hardening guidance

### Credentials in `primary_conninfo`

`primary_conninfo` is a libpq connection string and may include a
password.  To avoid leaking it:

- Set the GUC at server start time (`postgresql.conf` /
  `postgresql.auto.conf`), not via `ALTER SYSTEM` from a SQL session
  that other roles can read.
- The GUC is `PGC_SUSET` — only superusers can change it at runtime.
- `pg_write_forward_status()`:
  - Requires `EXECUTE` privilege.  Revoked from `PUBLIC` by default;
    granted to `pg_monitor`.
  - **Redacts `primary_conninfo` for non-superusers**, returning the
    literal string `<insufficient privilege>` instead of the conninfo.
- Prefer `.pgpass` or a dedicated client certificate over inline
  passwords.

### Privileged functions

| Function                          | Required privilege                                      |
|-----------------------------------|---------------------------------------------------------|
| `pg_write_forward_status()`       | `EXECUTE` (granted to `pg_monitor`); SU sees full conninfo |
| `pg_write_forward_disconnect()`   | **Superuser** (enforced both by C check and by `REVOKE`)|

### GUCs

| GUC                                       | Context     |
|-------------------------------------------|-------------|
| `pg_write_forward.primary_conninfo`       | `PGC_SUSET` (server start / superuser SET) |
| `pg_write_forward.consistency`            | `PGC_USERSET` |
| `pg_write_forward.enabled`                | `PGC_USERSET` (per-session opt-out) |
| `pg_write_forward.lsn_wait_timeout_ms`    | `PGC_USERSET` |

### Statements refused while forwarding is active

The following are hard-refused (with `ERRCODE_FEATURE_NOT_SUPPORTED`)
to avoid silent split-brain behaviour:

- `SAVEPOINT`, `RELEASE`, `ROLLBACK TO`
- `PREPARE TRANSACTION` (2PC)
- `LISTEN`, `NOTIFY`, `UNLISTEN`
- `COPY ... FROM`
- `DECLARE CURSOR ... FOR UPDATE/SHARE`

DDL (`CREATE`, `ALTER`, `DROP`) is **not forwarded** and runs locally,
where it will fail with the standby's normal read-only error.  This is
intentional — DDL on a standby would conflict with replay.

### Cancellation propagation

A local query cancel (`Ctrl-C` or backend termination) is forwarded to
the primary via `PQcancel()`.  Verified by TAP test `002_hardening.pl`.

## Audit log

For audit, enable on the standby:

```
log_statement = 'mod'      # logs every forwardable statement
log_connections = on
log_disconnections = on
```

Each forwarded statement also runs on the primary, so it will appear
in the primary's logs as well — useful for tracing.
