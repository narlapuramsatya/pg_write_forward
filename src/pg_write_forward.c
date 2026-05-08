/*-------------------------------------------------------------------------
 *
 * pg_write_forward.c
 *	  Forward write statements from a hot standby to the primary.
 *
 * Loaded on a hot standby (typically via shared_preload_libraries),
 * this extension intercepts INSERT/UPDATE/DELETE/MERGE,
 * SELECT ... FOR UPDATE/SHARE/..., PREPARE, EXECUTE, and EXPLAIN of any
 * of the above.  Instead of failing with the usual "cannot execute X in
 * a read-only transaction" error, it forwards the statement text to the
 * primary over libpq, optionally waits for the primary's resulting LSN
 * to be replayed locally, and returns the result to the client as if
 * the statement had executed locally.
 *
 * Consistency modes (GUC pg_write_forward.consistency):
 *
 *   off       - do not forward; original read-only error is raised.
 *   eventual  - forward, do not wait.  Subsequent reads on this standby
 *               may not see the write yet.
 *   session   - forward, then wait for the LSN of our own write to be
 *               replayed locally before returning.  The session sees
 *               its own writes.
 *   global    - forward, then wait for the primary's current WAL
 *               insert LSN (which is >= our commit LSN).  The session
 *               sees every write committed on the primary up to the
 *               moment of our forward.
 *
 * Transaction blocks:
 *
 *   The first forwardable statement inside an explicit BEGIN block
 *   triggers a lazy BEGIN on the primary that mirrors the local
 *   isolation level / DEFERRABLE.  All subsequent statements in the
 *   block (including plain SELECTs) are then forwarded so the session
 *   observes its own uncommitted writes.  COMMIT / ROLLBACK on the
 *   standby trigger the matching action on the primary, with LSN
 *   capture + replay wait at COMMIT time.
 *
 * Session state mirroring:
 *
 *   On each fresh primary connection a curated set of session GUCs
 *   (search_path, role, application_name, time-zone style, ...) is
 *   pulled from the local backend and applied on the primary.
 *   Explicit SET / RESET issued by the user is intercepted, mirrored
 *   to the primary live, and remembered (deduped by GUC name) so the
 *   same state is re-applied if the connection is rebuilt.
 *
 * Limitations (v1.0):
 *
 *   - DDL is not forwarded.
 *   - Cursors / portals (DECLARE, FETCH, MOVE) are not forwarded.
 *   - SAVEPOINT / ROLLBACK TO are not mirrored to the primary.
 *   - 2PC (PREPARE TRANSACTION) is not forwarded.
 *   - LISTEN / NOTIFY are not forwarded.
 *   - Temporary tables on the standby are local; statements touching
 *     them must execute locally and will fail (read-only).
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "access/xlogrecovery.h"
#include "access/xlogwait.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/prepare.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq-fe.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/* --- GUCs --- */

typedef enum
{
	WF_CONSISTENCY_OFF,
	WF_CONSISTENCY_EVENTUAL,
	WF_CONSISTENCY_SESSION,
	WF_CONSISTENCY_GLOBAL,
} WfConsistency;

static const struct config_enum_entry consistency_options[] = {
	{"off", WF_CONSISTENCY_OFF, false},
	{"eventual", WF_CONSISTENCY_EVENTUAL, false},
	{"session", WF_CONSISTENCY_SESSION, false},
	{"global", WF_CONSISTENCY_GLOBAL, false},
	{NULL, 0, false}
};

static char *pwf_primary_conninfo = NULL;
static int	pwf_consistency = WF_CONSISTENCY_OFF;
static bool pwf_enabled = true;
static int	pwf_lsn_wait_timeout_ms = 60000;	/* 60s default */

/* --- Statistics --- */
static uint64 pwf_forwarded_count = 0;
static uint64 pwf_forwarded_failures = 0;
static uint64 pwf_cancellations = 0;
static uint64 pwf_reconnects = 0;
static XLogRecPtr pwf_last_remote_lsn = InvalidXLogRecPtr;

/* --- Per-session state --- */
static PGconn *pwf_conn = NULL;

/* Linked list of QueryDescs we've taken responsibility for. */
typedef struct ForwardedQuery
{
	QueryDesc  *queryDesc;
	PGresult   *result;			/* primary's result, may be NULL after consume */
	bool		has_returning;	/* whether to push tuples to dest */
	struct ForwardedQuery *next;
} ForwardedQuery;

static ForwardedQuery *pwf_pending = NULL;

/* --- Transaction-block state --- */
typedef enum
{
	WF_XACT_NONE,				/* no primary-side transaction is open */
	WF_XACT_PRIMARY,			/* we have BEGIN-issued on the primary */
} WfXactState;

static WfXactState pwf_xact_state = WF_XACT_NONE;

/* --- Mirrored session state --- */
typedef struct WfSetEntry
{
	char	   *name;			/* GUC name (lower-cased canonical) */
	char	   *sql;			/* full SET / RESET source text */
} WfSetEntry;

/* List of WfSetEntry*; deduped by name; replayed on every fresh connect. */
static List *pwf_session_sets = NIL;

/*
 * GUCs we proactively snapshot from the standby session and apply on the
 * primary connection at open time.  This catches state that came from
 * postgresql.conf, ALTER ROLE / ALTER DATABASE, or libpq startup options
 * - i.e. anything the user did not explicitly SET in this session.
 */
static const char *const wf_mirror_gucs[] = {
	"search_path",
	"role",
	"session_authorization",
	"application_name",
	"timezone",
	"datestyle",
	"intervalstyle",
	"extra_float_digits",
	"statement_timeout",
	"lock_timeout",
	"idle_in_transaction_session_timeout",
	NULL,
};

/* --- Hook chains --- */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

PG_FUNCTION_INFO_V1(pg_write_forward_status);
PG_FUNCTION_INFO_V1(pg_write_forward_disconnect);

void		_PG_init(void);

/* --- Forward decls --- */
static bool wf_should_handle(void);
static bool wf_is_dml_query(QueryDesc *queryDesc);
static bool wf_is_locking_select(QueryDesc *queryDesc);
static PGconn *wf_get_conn(void);
static void wf_close_conn_callback(int code, Datum arg);
static void wf_apply_session_state(PGconn *conn);
static void wf_remember_set(VariableSetStmt *stmt, const char *queryString);
static void wf_mirror_set_to_primary(const char *queryString);
static PGresult *wf_send_to_primary(const char *sql, bool capture_lsn,
									bool *ok, char *errbuf,
									size_t errbufsize, XLogRecPtr *out_lsn);
static void wf_wait_for_lsn(XLogRecPtr target);
static void wf_register_pending(QueryDesc *queryDesc, PGresult *res,
								bool has_returning);
static ForwardedQuery *wf_lookup_pending(QueryDesc *queryDesc);
static void wf_release_pending(QueryDesc *queryDesc);
static void wf_push_result(QueryDesc *queryDesc, PGresult *res);
static void wf_begin_primary_xact(void);
static void wf_xact_callback(XactEvent event, void *arg);

static void wf_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void wf_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
						   uint64 count);
static void wf_ExecutorFinish(QueryDesc *queryDesc);
static void wf_ExecutorEnd(QueryDesc *queryDesc);
static void wf_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
							  bool readOnlyTree, ProcessUtilityContext context,
							  ParamListInfo params, QueryEnvironment *queryEnv,
							  DestReceiver *dest, QueryCompletion *qc);

/*
 * _PG_init
 *	  Module init: register GUCs and install hooks.
 */
void
_PG_init(void)
{
	DefineCustomStringVariable("pg_write_forward.primary_conninfo",
							   "libpq connection string for the primary.",
							   "Used by hot-standby backends to forward write "
							   "statements.  Must include enough credentials "
							   "for autonomous connection (no peer auth, no "
							   ".pgpass dependency unless that file is "
							   "available to the postgres user).",
							   &pwf_primary_conninfo,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomEnumVariable("pg_write_forward.consistency",
							 "Read-after-write consistency mode.",
							 "off=do not forward; eventual=forward, do not wait; "
							 "session=wait for own writes; "
							 "global=wait for primary's current LSN.",
							 &pwf_consistency,
							 WF_CONSISTENCY_OFF,
							 consistency_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_write_forward.enabled",
							 "Master switch for write forwarding.",
							 NULL,
							 &pwf_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("pg_write_forward.lsn_wait_timeout_ms",
							"Timeout in ms for the post-forward LSN wait.",
							NULL,
							&pwf_lsn_wait_timeout_ms,
							60000,
							0, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_write_forward");

	/* Install hooks */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = wf_ExecutorStart;

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = wf_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = wf_ExecutorFinish;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = wf_ExecutorEnd;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = wf_ProcessUtility;

	RegisterXactCallback(wf_xact_callback, NULL);

	on_proc_exit(wf_close_conn_callback, (Datum) 0);
}

/* ---------------------------------------------------------------------
 * Connection management
 * --------------------------------------------------------------------- */

/*
 * wf_exec_cancellable
 *	  Run `sql` on `conn` and return the (last) PGresult, while remaining
 *	  responsive to query-cancel and SIGTERM on the local backend.
 *
 * If the user cancels the local query, we forward a libpq cancel to the
 * primary, drain its result, then let CHECK_FOR_INTERRUPTS() throw the
 * usual cancel error.
 *
 * On connection failure mid-query the function returns NULL (caller
 * should free the conn and propagate the error or retry).
 */
static PGresult *
wf_exec_cancellable(PGconn *conn, const char *sql)
{
	PGresult   *res = NULL;
	PGresult   *next;
	int			sock;
	bool		cancel_sent = false;

	if (!PQsendQuery(conn, sql))
		return NULL;

	sock = PQsocket(conn);
	if (sock < 0)
		return NULL;

	for (;;)
	{
		int			wakeups;

		/*
		 * Wait briefly for socket-readable or latch.  We use a 1s timeout
		 * as a belt-and-suspenders against any latch-set we might miss
		 * (signals normally set the latch).
		 */
		wakeups = WaitLatchOrSocket(MyLatch,
									WL_LATCH_SET | WL_SOCKET_READABLE |
									WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
									sock,
									1000L,
									PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);

		if (wakeups & WL_SOCKET_READABLE)
		{
			if (!PQconsumeInput(conn))
			{
				/* connection broken */
				return NULL;
			}
		}

		/*
		 * If the user has asked us to cancel (or we're being terminated),
		 * forward a cancel to the primary once, then keep draining until
		 * the server sends us back its "query canceled" result.
		 */
		if (!cancel_sent && (QueryCancelPending || ProcDiePending))
		{
			PGcancel   *c = PQgetCancel(conn);

			if (c != NULL)
			{
				char		cebuf[256];

				if (!PQcancel(c, cebuf, sizeof(cebuf)))
				{
					/* best-effort; log and move on */
					ereport(LOG,
							(errmsg("pg_write_forward: PQcancel failed: %s",
									cebuf)));
				}
				PQfreeCancel(c);
				pwf_cancellations++;
			}
			cancel_sent = true;
		}

		if (!PQisBusy(conn))
			break;

		/*
		 * Don't call CHECK_FOR_INTERRUPTS() here yet: we want PQisBusy()
		 * to flip false (i.e. server has answered) before we let the
		 * cancel propagate.  Otherwise we'd leak a half-finished
		 * conversation on the connection.
		 */
	}

	/*
	 * Server is no longer busy: collect the result(s).  PQgetResult()
	 * returns one result per command; we keep the first non-empty one
	 * and drain the rest.
	 */
	res = PQgetResult(conn);
	while ((next = PQgetResult(conn)) != NULL)
		PQclear(next);

	/*
	 * Now safely throw the local cancel/terminate, if any.  The result we
	 * just collected may already be a PGRES_FATAL_ERROR caused by our
	 * forwarded cancel, in which case caller will see *ok=false anyway.
	 */
	CHECK_FOR_INTERRUPTS();

	return res;
}

static PGconn *
wf_get_conn(void)
{
	if (pwf_conn != NULL)
	{
		if (PQstatus(pwf_conn) == CONNECTION_OK)
			return pwf_conn;
		PQfinish(pwf_conn);
		pwf_conn = NULL;
	}

	if (pwf_primary_conninfo == NULL || pwf_primary_conninfo[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("pg_write_forward: pg_write_forward.primary_conninfo is not set")));

	pwf_conn = PQconnectdb(pwf_primary_conninfo);
	if (PQstatus(pwf_conn) != CONNECTION_OK)
	{
		char	   *err = pstrdup(PQerrorMessage(pwf_conn));

		PQfinish(pwf_conn);
		pwf_conn = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("pg_write_forward: could not connect to primary: %s",
						err)));
	}

	/*
	 * Apply mirrored session state (search_path, role, etc. + any user
	 * SETs we've been remembering).  Forced read-write so DML succeeds.
	 */
	wf_apply_session_state(pwf_conn);

	return pwf_conn;
}

static void
wf_close_conn_callback(int code, Datum arg)
{
	if (pwf_conn != NULL)
	{
		PQfinish(pwf_conn);
		pwf_conn = NULL;
	}
	pwf_xact_state = WF_XACT_NONE;
}

/* ---------------------------------------------------------------------
 * Session-state mirroring
 *
 * On each fresh connection we send:
 *   1. SET default_transaction_read_only = off (force RW semantics)
 *   2. The current value of every GUC in wf_mirror_gucs[]
 *   3. Every entry in pwf_session_sets (live SETs the user has issued)
 * --------------------------------------------------------------------- */

static void
wf_apply_session_state(PGconn *conn)
{
	StringInfoData buf;
	PGresult   *r;
	ListCell   *lc;

	/*
	 * Force RW semantics first, in its own statement, so that even if a
	 * later SET fails, this one stuck.
	 */
	r = PQexec(conn, "SET default_transaction_read_only = off");
	if (r)
		PQclear(r);

	initStringInfo(&buf);

	for (int i = 0; wf_mirror_gucs[i] != NULL; i++)
	{
		const char *name = wf_mirror_gucs[i];
		const char *val;

		val = GetConfigOptionByName(name, NULL, true);
		if (val == NULL || val[0] == '\0')
			continue;

		/*
		 * Skip the magic "none" value used by `role` and
		 * `session_authorization` to mean "no override" - sending it
		 * verbatim would fail on the primary, which doesn't have a
		 * role literally named "none".
		 */
		if ((strcmp(name, "role") == 0 ||
			 strcmp(name, "session_authorization") == 0) &&
			strcmp(val, "none") == 0)
			continue;

		resetStringInfo(&buf);

		/*
		 * search_path quirk: the string-literal form
		 *   SET search_path = '"$user", public'
		 * is parsed in a way that loses entries when the value
		 * contains the special "$user" token.  Use the list form
		 * (SET ... TO val) to round-trip cleanly.  The value is
		 * already a GUC-validated identifier list, so injecting it
		 * directly is safe.
		 */
		if (strcmp(name, "search_path") == 0)
		{
			appendStringInfo(&buf, "SET search_path TO %s", val);
		}
		else
		{
			char	   *escaped = PQescapeLiteral(conn, val, strlen(val));

			if (escaped == NULL)
				continue;
			appendStringInfo(&buf, "SET %s = %s",
							 quote_identifier(name), escaped);
			PQfreemem(escaped);
		}

		/*
		 * Send individually so that one bad GUC (e.g. extension-only on
		 * the standby, or a role missing on the primary) does not abort
		 * the others.  Failures are warned, not raised.
		 */
		r = PQexec(conn, buf.data);
		if (r == NULL || PQresultStatus(r) != PGRES_COMMAND_OK)
		{
			ereport(WARNING,
					(errmsg("pg_write_forward: failed to mirror %s to primary",
							name),
					 errdetail("%s",
							   r ? PQresultErrorMessage(r)
								 : PQerrorMessage(conn))));
		}
		if (r)
			PQclear(r);
	}

	foreach(lc, pwf_session_sets)
	{
		WfSetEntry *e = (WfSetEntry *) lfirst(lc);

		r = PQexec(conn, e->sql);
		if (r == NULL || PQresultStatus(r) != PGRES_COMMAND_OK)
		{
			ereport(WARNING,
					(errmsg("pg_write_forward: failed to replay SET on primary"),
					 errdetail("%s",
							   r ? PQresultErrorMessage(r)
								 : PQerrorMessage(conn))));
		}
		if (r)
			PQclear(r);
	}

	pfree(buf.data);
}

/*
 * Remember a SET / RESET we've already executed locally, so we can replay
 * it on a future reconnect.  De-duped by GUC name.
 *
 * For RESET ALL we drop everything (and remember the RESET ALL itself).
 */
static void
wf_remember_set(VariableSetStmt *stmt, const char *queryString)
{
	WfSetEntry *e;
	MemoryContext old;
	ListCell   *lc;

	if (stmt == NULL || queryString == NULL)
		return;

	/* Don't track our own GUCs - causes recursion on apply. */
	if (stmt->name != NULL &&
		strncmp(stmt->name, "pg_write_forward.", 17) == 0)
		return;

	old = MemoryContextSwitchTo(TopMemoryContext);

	if (stmt->kind == VAR_RESET_ALL)
	{
		foreach(lc, pwf_session_sets)
		{
			WfSetEntry *old_e = (WfSetEntry *) lfirst(lc);

			pfree(old_e->name);
			pfree(old_e->sql);
			pfree(old_e);
		}
		list_free(pwf_session_sets);
		pwf_session_sets = NIL;
		MemoryContextSwitchTo(old);
		return;
	}

	if (stmt->name == NULL)
	{
		MemoryContextSwitchTo(old);
		return;
	}

	/* Drop any prior entry for this name. */
	foreach(lc, pwf_session_sets)
	{
		WfSetEntry *old_e = (WfSetEntry *) lfirst(lc);

		if (strcmp(old_e->name, stmt->name) == 0)
		{
			pwf_session_sets = foreach_delete_current(pwf_session_sets, lc);
			pfree(old_e->name);
			pfree(old_e->sql);
			pfree(old_e);
			break;
		}
	}

	e = (WfSetEntry *) palloc(sizeof(WfSetEntry));
	e->name = pstrdup(stmt->name);
	e->sql = pstrdup(queryString);
	pwf_session_sets = lappend(pwf_session_sets, e);

	MemoryContextSwitchTo(old);
}

/*
 * Send a SET / RESET to the primary right now, if a connection is open.
 * Used to keep state in sync mid-session without forcing a reconnect.
 */
static void
wf_mirror_set_to_primary(const char *queryString)
{
	PGresult   *r;

	if (pwf_conn == NULL || PQstatus(pwf_conn) != CONNECTION_OK)
		return;
	if (queryString == NULL || queryString[0] == '\0')
		return;

	r = PQexec(pwf_conn, queryString);
	if (r == NULL || PQresultStatus(r) != PGRES_COMMAND_OK)
	{
		ereport(WARNING,
				(errmsg("pg_write_forward: failed to mirror SET to primary"),
				 errdetail("%s",
						   r ? PQresultErrorMessage(r) : PQerrorMessage(pwf_conn))));
	}
	if (r)
		PQclear(r);
}

/* ---------------------------------------------------------------------
 * Forwarding plumbing
 * --------------------------------------------------------------------- */

/*
 * wf_should_handle - quick predicate.
 *
 * We only act when (a) we're on a hot standby, (b) the master switch is on,
 * (c) consistency != off (consistency == off means "behave as if extension
 * is not installed"), and (d) a primary_conninfo is configured.
 */
static bool
wf_should_handle(void)
{
	if (!pwf_enabled)
		return false;
	if (pwf_consistency == WF_CONSISTENCY_OFF)
		return false;
	if (!RecoveryInProgress())
		return false;
	if (pwf_primary_conninfo == NULL || pwf_primary_conninfo[0] == '\0')
		return false;
	return true;
}

static bool
wf_is_dml_query(QueryDesc *queryDesc)
{
	switch (queryDesc->operation)
	{
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
		case CMD_MERGE:
			return true;
		default:
			return false;
	}
}

/*
 * Is this a SELECT with row marks (FOR UPDATE/SHARE/...)?  These would
 * normally fail on a standby, just like a DML.
 */
static bool
wf_is_locking_select(QueryDesc *queryDesc)
{
	if (queryDesc->operation != CMD_SELECT)
		return false;
	if (queryDesc->plannedstmt == NULL)
		return false;
	return queryDesc->plannedstmt->rowMarks != NIL;
}

/*
 * Lazily begin a transaction on the primary that mirrors our local one.
 * Called from the forward path the first time we forward a statement that
 * is running inside an explicit local BEGIN block.
 *
 * On success, pwf_xact_state is set to WF_XACT_PRIMARY, and the matching
 * COMMIT/ROLLBACK will be issued by wf_xact_callback().
 */
static void
wf_begin_primary_xact(void)
{
	StringInfoData buf;
	PGresult   *r;
	PGconn	   *conn;

	if (pwf_xact_state == WF_XACT_PRIMARY)
		return;

	conn = wf_get_conn();

	initStringInfo(&buf);
	appendStringInfoString(&buf, "BEGIN");

	switch (XactIsoLevel)
	{
		case XACT_READ_UNCOMMITTED:
		case XACT_READ_COMMITTED:
			appendStringInfoString(&buf, " ISOLATION LEVEL READ COMMITTED");
			break;
		case XACT_REPEATABLE_READ:
			appendStringInfoString(&buf, " ISOLATION LEVEL REPEATABLE READ");
			break;
		case XACT_SERIALIZABLE:
			appendStringInfoString(&buf, " ISOLATION LEVEL SERIALIZABLE");
			break;
	}

	if (XactDeferrable)
		appendStringInfoString(&buf, " DEFERRABLE");

	r = wf_exec_cancellable(conn, buf.data);
	if (r == NULL || PQresultStatus(r) != PGRES_COMMAND_OK)
	{
		char	   *err = pstrdup(r ? PQresultErrorMessage(r)
									: PQerrorMessage(conn));

		if (r)
			PQclear(r);
		pfree(buf.data);
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("pg_write_forward: BEGIN on primary failed"),
				 errdetail("%s", err)));
	}
	PQclear(r);
	pfree(buf.data);

	pwf_xact_state = WF_XACT_PRIMARY;
}

/*
 * Transaction callback: drives COMMIT / ROLLBACK on the primary in
 * lockstep with the standby session's local transaction.
 *
 * Important: we run the primary COMMIT at PRE_COMMIT.  If the primary
 * commit fails we ereport(ERROR), which converts the local commit into
 * an abort, and the ABORT path then issues a best-effort ROLLBACK.
 * (At this point the primary has not actually committed, so this is
 * the right behavior.)
 */
static void
wf_xact_callback(XactEvent event, void *arg)
{
	if (pwf_xact_state != WF_XACT_PRIMARY)
		return;

	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			{
				bool		ok;
				char		errbuf[1024] = {0};
				XLogRecPtr	lsn = InvalidXLogRecPtr;
				PGresult   *r;

				r = wf_send_to_primary("COMMIT", true, &ok,
									   errbuf, sizeof(errbuf), &lsn);
				if (!ok)
				{
					/*
					 * Primary refused the COMMIT.  Mark our state as
					 * idle and try a best-effort ROLLBACK to clean up
					 * the connection, then raise.
					 */
					pwf_xact_state = WF_XACT_NONE;
					if (pwf_conn != NULL &&
						PQstatus(pwf_conn) == CONNECTION_OK)
					{
						PGresult   *rb = PQexec(pwf_conn, "ROLLBACK");

						if (rb)
							PQclear(rb);
					}
					ereport(ERROR,
							(errcode(ERRCODE_T_R_INTEGRITY_CONSTRAINT_VIOLATION),
							 errmsg("pg_write_forward: COMMIT on primary failed"),
							 errdetail("%s", errbuf)));
				}
				if (r)
					PQclear(r);
				pwf_xact_state = WF_XACT_NONE;

				/* Wait for replay so post-commit reads see our writes. */
				wf_wait_for_lsn(lsn);
				break;
			}

		case XACT_EVENT_ABORT:
			{
				if (pwf_conn != NULL &&
					PQstatus(pwf_conn) == CONNECTION_OK)
				{
					PGresult   *r = PQexec(pwf_conn, "ROLLBACK");

					if (r)
						PQclear(r);
				}
				pwf_xact_state = WF_XACT_NONE;
				break;
			}

		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_PRE_PREPARE:
			break;
	}
}

/*
 * wf_send_to_primary
 *	  Send the SQL to the primary; optionally capture the resulting LSN.
 *
 * On success returns a freshly allocated PGresult; caller must PQclear() it.
 * On failure returns NULL with *ok = false and a message in errbuf.
 *
 * If capture_lsn is true, *out_lsn receives the primary's
 * pg_current_wal_insert_lsn() after the statement.  When forwarding inside
 * an open transaction block we skip the LSN probe (we'll capture it once
 * at COMMIT time).
 */
static PGresult *
wf_send_to_primary(const char *sql, bool capture_lsn,
				   bool *ok, char *errbuf, size_t errbufsize,
				   XLogRecPtr *out_lsn)
{
	PGconn	   *conn = wf_get_conn();
	PGresult   *res;
	PGresult   *lsn_res;
	XLogRecPtr	lsn = InvalidXLogRecPtr;

	*ok = false;
	if (out_lsn)
		*out_lsn = InvalidXLogRecPtr;

	/*
	 * Send the user's SQL.  We use the cancellable wrapper so a local
	 * Ctrl-C is forwarded to the primary instead of leaving an
	 * orphaned long-running query there.
	 */
	res = wf_exec_cancellable(conn, sql);
	if (res == NULL)
	{
		/*
		 * Connection died (or PQsendQuery failed).  Try once to
		 * reconnect-and-resend, but only if we are NOT inside a primary
		 * transaction block (where a reconnect would silently abort the
		 * remote xact and lose isolation guarantees).
		 */
		snprintf(errbuf, errbufsize, "%s", PQerrorMessage(conn));
		if (pwf_xact_state != WF_XACT_PRIMARY)
		{
			PQfinish(pwf_conn);
			pwf_conn = NULL;
			pwf_reconnects++;
			conn = wf_get_conn();
			res = wf_exec_cancellable(conn, sql);
		}
		if (res == NULL)
		{
			pwf_forwarded_failures++;
			return NULL;
		}
	}

	switch (PQresultStatus(res))
	{
		case PGRES_COMMAND_OK:
		case PGRES_TUPLES_OK:
		case PGRES_EMPTY_QUERY:
			break;
		default:
			snprintf(errbuf, errbufsize, "%s",
					 PQresultErrorMessage(res));
			PQclear(res);
			pwf_forwarded_failures++;
			return NULL;
	}

	if (capture_lsn)
	{
		lsn_res = wf_exec_cancellable(conn, "SELECT pg_current_wal_insert_lsn()::text");
		if (lsn_res != NULL && PQresultStatus(lsn_res) == PGRES_TUPLES_OK &&
			PQntuples(lsn_res) == 1 && !PQgetisnull(lsn_res, 0, 0))
		{
			Datum		d;
			const char *txt = PQgetvalue(lsn_res, 0, 0);

			d = DirectFunctionCall1(pg_lsn_in, CStringGetDatum(txt));
			lsn = DatumGetLSN(d);
		}
		if (lsn_res)
			PQclear(lsn_res);

		if (out_lsn)
			*out_lsn = lsn;
		pwf_last_remote_lsn = lsn;
	}

	pwf_forwarded_count++;

	*ok = true;
	return res;
}

/*
 * wf_wait_for_lsn
 *	  Wait for replay to reach `target` (or primary's current LSN, in global
 *	  mode) before returning to the client.
 */
static void
wf_wait_for_lsn(XLogRecPtr target)
{
	XLogRecPtr	wait_for = target;
	WaitLSNResult res;

	if (pwf_consistency == WF_CONSISTENCY_OFF ||
		pwf_consistency == WF_CONSISTENCY_EVENTUAL)
		return;

	if (pwf_consistency == WF_CONSISTENCY_GLOBAL)
	{
		PGresult   *r = wf_exec_cancellable(wf_get_conn(),
											"SELECT pg_current_wal_insert_lsn()::text");

		if (r && PQresultStatus(r) == PGRES_TUPLES_OK &&
			PQntuples(r) == 1 && !PQgetisnull(r, 0, 0))
		{
			Datum		d = DirectFunctionCall1(pg_lsn_in,
												CStringGetDatum(PQgetvalue(r, 0, 0)));

			wait_for = DatumGetLSN(d);
		}
		if (r)
			PQclear(r);
	}

	if (XLogRecPtrIsInvalid(wait_for))
		return;

	res = WaitForLSN(WAIT_LSN_TYPE_STANDBY_REPLAY, wait_for,
					 (int64) pwf_lsn_wait_timeout_ms * 1000);

	switch (res)
	{
		case WAIT_LSN_RESULT_SUCCESS:
		case WAIT_LSN_RESULT_NOT_IN_RECOVERY:
			break;
		case WAIT_LSN_RESULT_TIMEOUT:
			ereport(WARNING,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("pg_write_forward: timed out waiting for replay of LSN %X/%08X",
							LSN_FORMAT_ARGS(wait_for))));
			break;
	}
}

/* ---------------------------------------------------------------------
 * Pending-query bookkeeping
 *
 * We attach our state to QueryDesc* by side-list rather than embedding,
 * so we can hand the QueryDesc back to the standard executor pipeline
 * for ExecutorEnd cleanup of any partial state we created.
 * --------------------------------------------------------------------- */

static void
wf_register_pending(QueryDesc *queryDesc, PGresult *res, bool has_returning)
{
	ForwardedQuery *fq;
	MemoryContext old;

	old = MemoryContextSwitchTo(TopMemoryContext);
	fq = (ForwardedQuery *) palloc(sizeof(ForwardedQuery));
	fq->queryDesc = queryDesc;
	fq->result = res;
	fq->has_returning = has_returning;
	fq->next = pwf_pending;
	pwf_pending = fq;
	MemoryContextSwitchTo(old);
}

static ForwardedQuery *
wf_lookup_pending(QueryDesc *queryDesc)
{
	ForwardedQuery *p;

	for (p = pwf_pending; p != NULL; p = p->next)
		if (p->queryDesc == queryDesc)
			return p;
	return NULL;
}

static void
wf_release_pending(QueryDesc *queryDesc)
{
	ForwardedQuery **pp;

	for (pp = &pwf_pending; *pp != NULL; pp = &(*pp)->next)
	{
		if ((*pp)->queryDesc == queryDesc)
		{
			ForwardedQuery *gone = *pp;

			*pp = gone->next;
			if (gone->result)
				PQclear(gone->result);
			pfree(gone);
			return;
		}
	}
}

/*
 * Push tuples from a primary PGresult into the QueryDesc's destination
 * receiver.  Builds a TupleDesc from PGresult metadata, converts each text
 * value via the type's input function, and feeds the slot.
 */
static void
wf_push_result(QueryDesc *queryDesc, PGresult *res)
{
	int			nfields;
	int			ntuples;
	DestReceiver *dest = queryDesc->dest;
	TupleDesc	tupdesc;
	TupleTableSlot *slot;
	int			row;

	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK)
		return;

	nfields = PQnfields(res);
	ntuples = PQntuples(res);

	tupdesc = CreateTemplateTupleDesc(nfields);
	for (int i = 0; i < nfields; i++)
	{
		Oid			typid = PQftype(res, i);
		int32		typmod = PQfmod(res, i);

		/*
		 * If we've never heard of the type OID, fall back to text.  This
		 * could happen for primary-only types in a heterogeneous setup; we
		 * choose to surface the data rather than fail.
		 */
		if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(typid)))
			typid = TEXTOID;

		TupleDescInitEntry(tupdesc, (AttrNumber) (i + 1),
						   PQfname(res, i), typid, typmod, 0);
	}
	TupleDescFinalize(tupdesc);
	BlessTupleDesc(tupdesc);

	dest->rStartup(dest, queryDesc->operation, tupdesc);

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

	for (row = 0; row < ntuples; row++)
	{
		ExecClearTuple(slot);
		for (int col = 0; col < nfields; col++)
		{
			if (PQgetisnull(res, row, col))
			{
				slot->tts_values[col] = (Datum) 0;
				slot->tts_isnull[col] = true;
			}
			else
			{
				Form_pg_attribute attr = TupleDescAttr(tupdesc, col);
				Oid			typinput;
				Oid			typioparam;

				getTypeInputInfo(attr->atttypid, &typinput, &typioparam);
				slot->tts_values[col] =
					OidInputFunctionCall(typinput,
										 PQgetvalue(res, row, col),
										 typioparam,
										 attr->atttypmod);
				slot->tts_isnull[col] = false;
			}
		}
		ExecStoreVirtualTuple(slot);
		dest->receiveSlot(slot, dest);
	}

	ExecDropSingleTupleTableSlot(slot);
	dest->rShutdown(dest);

	if (queryDesc->estate)
		queryDesc->estate->es_processed = ntuples;
}

/* ---------------------------------------------------------------------
 * Hooks
 * --------------------------------------------------------------------- */

static void
wf_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		handle;
	bool		in_primary_xact = (pwf_xact_state == WF_XACT_PRIMARY);

	/*
	 * If we're in the middle of a forwarded transaction we MUST forward
	 * everything (including plain SELECT) so the session sees its own
	 * uncommitted writes.  Otherwise, only forward DML and locking SELECTs.
	 */
	if (in_primary_xact)
		handle = true;
	else
		handle = wf_should_handle() &&
			(wf_is_dml_query(queryDesc) || wf_is_locking_select(queryDesc));

	if (!handle)
	{
		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
		return;
	}

	/* EXPLAIN ONLY of a write goes through ProcessUtility hook; not here. */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
	{
		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
		return;
	}

	/*
	 * If the user has opened an explicit transaction block locally and we
	 * haven't yet mirrored it to the primary, do so now.  Subsequent
	 * statements in the block (including plain SELECTs) will then be
	 * forwarded automatically by the gate above.
	 */
	if (!in_primary_xact && IsTransactionBlock())
	{
		wf_begin_primary_xact();
		in_primary_xact = true;
	}

	/* Forward to primary now. */
	{
		bool		ok;
		char		errbuf[1024] = {0};
		XLogRecPtr	remote_lsn;
		PGresult   *res;
		bool		has_returning;

		res = wf_send_to_primary(queryDesc->sourceText,
								 !in_primary_xact,
								 &ok, errbuf, sizeof(errbuf),
								 &remote_lsn);
		if (!ok)
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("pg_write_forward: primary rejected forwarded statement"),
					 errdetail("%s", errbuf)));

		has_returning = (PQresultStatus(res) == PGRES_TUPLES_OK);

		/*
		 * Wait for replay before exposing the result, so the client's next
		 * read on this standby in this session sees its own write.
		 *
		 * We skip per-statement waits inside an open primary xact: nothing
		 * is committed yet, and waiting on an as-yet-unwritten LSN would
		 * be a no-op anyway.  The wait happens at COMMIT time.
		 */
		if (!in_primary_xact)
			wf_wait_for_lsn(remote_lsn);

		/*
		 * Build a minimal EState so that ExecutorRun/Finish/End won't crash
		 * dereferencing it.  We deliberately do NOT call standard_ExecutorStart
		 * because that would run CheckXactReadOnly() on our DML and abort.
		 */
		queryDesc->estate = CreateExecutorState();
		queryDesc->estate->es_top_eflags = eflags;
		queryDesc->estate->es_processed = 0;
		queryDesc->estate->es_param_list_info = queryDesc->params;
		queryDesc->estate->es_sourceText = queryDesc->sourceText;

		wf_register_pending(queryDesc, res, has_returning);
	}
}

static void
wf_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	ForwardedQuery *fq = wf_lookup_pending(queryDesc);

	if (fq == NULL)
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
		return;
	}

	/* Push tuples (RETURNING / SELECT FOR UPDATE) or set rowcount. */
	if (fq->has_returning && fq->result != NULL)
	{
		wf_push_result(queryDesc, fq->result);
	}
	else if (fq->result != NULL)
	{
		const char *cmd = PQcmdTuples(fq->result);

		if (cmd && cmd[0] != '\0')
			queryDesc->estate->es_processed = strtoull(cmd, NULL, 10);

		/*
		 * For DML without RETURNING the destination is None; we still must
		 * report the row count back to the SQL command tag.  PortalRun reads
		 * estate->es_processed.
		 */
	}

	/*
	 * Free the PGresult eagerly to release memory; ExecutorEnd will still
	 * call wf_release_pending() which is now a no-op for `result`.
	 */
	if (fq->result)
	{
		PQclear(fq->result);
		fq->result = NULL;
	}
}

static void
wf_ExecutorFinish(QueryDesc *queryDesc)
{
	ForwardedQuery *fq = wf_lookup_pending(queryDesc);

	if (fq != NULL)
		return;					/* nothing to finish: we never planned anything */

	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

static void
wf_ExecutorEnd(QueryDesc *queryDesc)
{
	ForwardedQuery *fq = wf_lookup_pending(queryDesc);

	if (fq != NULL)
	{
		/*
		 * We constructed a minimal EState in ExecutorStart; tear it down
		 * symmetrically.  Do NOT call standard_ExecutorEnd, as it expects
		 * a full plan tree.
		 */
		if (queryDesc->estate)
		{
			FreeExecutorState(queryDesc->estate);
			queryDesc->estate = NULL;
		}
		wf_release_pending(queryDesc);
		return;
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/* ---------------------------------------------------------------------
 * ProcessUtility hook
 *
 * Handles utility statements:
 *
 *   - PREPARE / EXECUTE          : forwarded as raw SQL string.
 *   - EXPLAIN of a forwardable   : sent to primary verbatim.
 *
 * Other utility statements are passed through unchanged, including DDL
 * (which we explicitly do NOT forward) and read-only utilities.
 * --------------------------------------------------------------------- */

static bool
utility_is_forwardable(Node *parsetree)
{
	if (parsetree == NULL)
		return false;
	switch (nodeTag(parsetree))
	{
		case T_PrepareStmt:
		case T_ExecuteStmt:
			return true;
		case T_ExplainStmt:
			{
				ExplainStmt *e = (ExplainStmt *) parsetree;
				ListCell   *lc;

				/* EXPLAIN ANALYZE of a write would actually execute it. */
				foreach(lc, e->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0 &&
						defGetBoolean(opt))
						return true;
				}
				/* Plain EXPLAIN is read-only and can run locally. */
				return false;
			}
		default:
			return false;
	}
}

static void
wf_run_utility_on_primary(const char *queryString, DestReceiver *dest,
						  QueryCompletion *qc, CmdType operation)
{
	bool		ok;
	char		errbuf[1024] = {0};
	XLogRecPtr	remote_lsn;
	PGresult   *res;
	const char *cmd;
	bool		in_primary_xact;

	/* Lazy-begin if we're inside an explicit local xact block. */
	if (pwf_xact_state == WF_XACT_NONE && IsTransactionBlock())
		wf_begin_primary_xact();
	in_primary_xact = (pwf_xact_state == WF_XACT_PRIMARY);

	res = wf_send_to_primary(queryString, !in_primary_xact, &ok,
							 errbuf, sizeof(errbuf), &remote_lsn);
	if (!ok)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("pg_write_forward: primary rejected forwarded utility"),
				 errdetail("%s", errbuf)));

	if (!in_primary_xact)
		wf_wait_for_lsn(remote_lsn);

	/*
	 * For utility we can't easily build a TupleDesc from the parser side, so
	 * we mirror PGresult shape directly into the destination.
	 */
	if (PQresultStatus(res) == PGRES_TUPLES_OK && dest != NULL)
	{
		QueryDesc	tmp;

		memset(&tmp, 0, sizeof(tmp));
		tmp.operation = operation;
		tmp.dest = dest;
		tmp.estate = CreateExecutorState();
		wf_push_result(&tmp, res);
		FreeExecutorState(tmp.estate);
	}

	cmd = PQcmdStatus(res);
	if (qc != NULL && cmd != NULL && cmd[0] != '\0')
	{
		const char *t = PQcmdTuples(res);

		if (t && t[0] != '\0')
			qc->nprocessed = strtoull(t, NULL, 10);
	}

	PQclear(res);
}

static void
wf_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				  bool readOnlyTree, ProcessUtilityContext context,
				  ParamListInfo params, QueryEnvironment *queryEnv,
				  DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	/*
	 * Refuse statements that we cannot safely forward when forwarding is
	 * active.  The key invariant is that no committed write should ever
	 * be invisible to the local transaction's view of "what I asked for":
	 *
	 *   - SAVEPOINT / RELEASE / ROLLBACK TO would split a primary-side
	 *     transaction into sub-transactions that we don't mirror, so a
	 *     ROLLBACK TO on the standby would silently keep the writes on
	 *     the primary.  Refuse outright.
	 *   - PREPARE TRANSACTION (2PC) cannot be tracked across two
	 *     servers without a coordinator.
	 *   - LISTEN / NOTIFY / UNLISTEN would set up channels on the
	 *     primary that the standby session cannot consume.
	 *   - COPY ... FROM would stream a potentially huge data volume
	 *     through us; not supported in v1.x.  COPY ... TO is read-only
	 *     and runs locally.
	 */
	if (parsetree != NULL && wf_should_handle())
	{
		switch (nodeTag(parsetree))
		{
			case T_TransactionStmt:
				{
					TransactionStmt *t = (TransactionStmt *) parsetree;

					if (t->kind == TRANS_STMT_SAVEPOINT ||
						t->kind == TRANS_STMT_RELEASE ||
						t->kind == TRANS_STMT_ROLLBACK_TO)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("pg_write_forward does not support savepoints"),
								 errhint("Disable forwarding (SET pg_write_forward.consistency=off) to use savepoints.")));
					if (t->kind == TRANS_STMT_PREPARE)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("pg_write_forward does not support PREPARE TRANSACTION (two-phase commit)")));
					break;
				}
			case T_ListenStmt:
			case T_NotifyStmt:
			case T_UnlistenStmt:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("pg_write_forward does not support LISTEN / NOTIFY")));
				break;
			case T_CopyStmt:
				{
					CopyStmt   *c = (CopyStmt *) parsetree;

					if (c->is_from)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("pg_write_forward does not support COPY ... FROM"),
								 errhint("Run COPY directly on the primary, or write a series of INSERTs.")));
					break;
				}
			case T_DeclareCursorStmt:
				/*
				 * Cursors that contain row marks (FOR UPDATE) would need
				 * to live on the primary; we don't track cursor state
				 * across the wire.  Read-only cursors are fine and
				 * execute locally.
				 */
				{
					DeclareCursorStmt *d = (DeclareCursorStmt *) parsetree;
					Query	   *q = (Query *) d->query;

					if (q != NULL && q->rowMarks != NIL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("pg_write_forward does not support cursors with FOR UPDATE/SHARE")));
					break;
				}
			default:
				break;
		}
	}

	/*
	 * SET / RESET: always run locally first; on success, mirror to the
	 * primary (now and on every future reconnect).  We deliberately do
	 * NOT short-circuit local execution: the user expects local
	 * search_path/role/etc. to update too.
	 */
	if (parsetree != NULL && IsA(parsetree, VariableSetStmt))
	{
		VariableSetStmt *stmt = (VariableSetStmt *) parsetree;

		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);

		/* Local SET succeeded; remember + mirror. */
		if (wf_should_handle())
		{
			wf_remember_set(stmt, queryString);
			wf_mirror_set_to_primary(queryString);
		}
		return;
	}

	if (wf_should_handle() && utility_is_forwardable(parsetree))
	{
		wf_run_utility_on_primary(queryString, dest, qc, CMD_UTILITY);
		return;
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* ---------------------------------------------------------------------
 * SQL-callable helpers
 * --------------------------------------------------------------------- */

Datum
pg_write_forward_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[9];
	bool		nulls[9] = {false};
	HeapTuple	tuple;
	const char *consistency_str;
	const char *conninfo_disp;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	switch (pwf_consistency)
	{
		case WF_CONSISTENCY_OFF:
			consistency_str = "off";
			break;
		case WF_CONSISTENCY_EVENTUAL:
			consistency_str = "eventual";
			break;
		case WF_CONSISTENCY_SESSION:
			consistency_str = "session";
			break;
		case WF_CONSISTENCY_GLOBAL:
			consistency_str = "global";
			break;
		default:
			consistency_str = "unknown";
	}

	/*
	 * Scrub the conninfo before returning it: a non-superuser must not be
	 * able to read out a password embedded in primary_conninfo via this
	 * function.  Superusers see the full string.
	 */
	if (pwf_primary_conninfo == NULL || pwf_primary_conninfo[0] == '\0')
		conninfo_disp = "";
	else if (superuser())
		conninfo_disp = pwf_primary_conninfo;
	else
		conninfo_disp = "<insufficient privilege>";

	values[0] = CStringGetTextDatum(conninfo_disp);
	values[1] = CStringGetTextDatum(consistency_str);
	values[2] = BoolGetDatum(pwf_enabled);
	values[3] = BoolGetDatum(pwf_conn != NULL && PQstatus(pwf_conn) == CONNECTION_OK);
	values[4] = Int64GetDatum((int64) pwf_forwarded_count);
	if (XLogRecPtrIsInvalid(pwf_last_remote_lsn))
		nulls[5] = true;
	else
		values[5] = LSNGetDatum(pwf_last_remote_lsn);
	values[6] = Int64GetDatum((int64) pwf_forwarded_failures);
	values[7] = Int64GetDatum((int64) pwf_cancellations);
	values[8] = Int64GetDatum((int64) pwf_reconnects);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
pg_write_forward_disconnect(PG_FUNCTION_ARGS)
{
	/*
	 * Closing the primary connection is privileged: it could be used to
	 * disrupt other backends sharing the same primary, and to force
	 * password-bearing reconnect attempts that touch postgresql.conf
	 * state.  Restrict to superusers; an admin can GRANT EXECUTE to a
	 * specific role if they want to delegate.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to disconnect pg_write_forward")));

	if (pwf_conn != NULL)
	{
		PQfinish(pwf_conn);
		pwf_conn = NULL;
	}
	PG_RETURN_VOID();
}
