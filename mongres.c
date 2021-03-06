/* -------------------------------------------------------------------------
 *
 * mongres.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "libpq/libpq.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

#include "bson.h"
#include "mongo.h"

#include "bjson.h"

#include "utils/json.h"

PG_MODULE_MAGIC;

void	_PG_init(void);

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int mongres_listen_port = 27017;

pgsocket	MGListenSocket[1];

/* Function prototypes */


/*
 * Signal handler for SIGTERM
 * 		Set a flag to let the main loop to terminate, and set our latch to wake
 * 		it up.
 */
static void
mongres_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 * 		Set a flag to let the main loop to reread the config file, and set
 * 		our latch to wake it up.
 */
static void
mongres_sighup(SIGNAL_ARGS)
{
	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);
}

static Oid
find_function(const char *regprocedure)
{
	return DatumGetObjectId(
			DirectFunctionCall1(regprocedurein,
					CStringGetDatum(regprocedure)));
}

static bool
handle_insert(pgsocket sock, mongo_header *header,
			  char *payload, size_t payload_size)
{
	int32		flags;
	NameData	namespace;
	bson		b;
	StringInfoData jsonbuf;
	char	   *nameptr, *endptr;
	Oid			funcoid;
	Datum		result;

	elog(LOG, "OP_INSERT");
	flags = *((int32 *) payload);
	nameptr = payload + sizeof(int32);

	StrNCpy(NameStr(namespace), nameptr, NAMEDATALEN);
	ereport(LOG,
			(errmsg("namespace = %s", NameStr(namespace))));

	b.data = (nameptr + strlen(NameStr(namespace)) + 1);
	endptr = payload + payload_size;
	initStringInfo(&jsonbuf);
	appendStringInfoChar(&jsonbuf, '[');
	while (b.data < endptr)
	{

		if (jsonbuf.len > 1)
			appendStringInfoChar(&jsonbuf, ',');

		bson_to_json(&jsonbuf, b.data);
		b.data += bson_size(&b);
	}
	appendStringInfoChar(&jsonbuf, ']');

	funcoid = find_function("mongres_insert(text, json)");

	result = OidFunctionCall2(funcoid,
							  CStringGetTextDatum(NameStr(namespace)),
							  CStringGetTextDatum(jsonbuf.data));

	return true;
}

static bool
send_reply(pgsocket sock, mongo_header *request_header,
		   StringInfo documents, int ndocs)
{
	mongo_reply reply;
	mongo_header *header = &reply.head;
	mongo_reply_fields *fields = &reply.fields;
	int hsize = sizeof(mongo_header) + sizeof(mongo_reply_fields);

	header->len = hsize + documents->len;
	header->id = rand();
	header->responseTo = request_header->id;
	header->op = 1;

	fields->flag = 0;
	fields->cursorID = 0;
	fields->start = 0;
	fields->num = ndocs;

	if (write(sock, &reply, hsize) != hsize)
	{
		ereport(LOG,
				(errmsg("could not write reply: %m")));
		return false;
	}

	if (write(sock, documents->data, documents->len) !=
			documents->len)
	{
		ereport(LOG,
				(errmsg("could not write reply: %m")));
		return false;
	}

	return true;
}

static bool
handle_query(pgsocket sock, mongo_header *header,
			 char *payload, size_t payload_size)
{
	int32		flags;
	NameData	namespace;
	bson		b;
	char	   *json;

	Oid			funcoid;
	Datum		result;
	Datum	   *values;
	int			nelem, i;
	StringInfoData output;

	elog(LOG, "OP_QUERY");
	flags = *((int32 *) payload);
	payload += sizeof(int32);

	StrNCpy(NameStr(namespace), payload, NAMEDATALEN);
	ereport(LOG,
			(errmsg("namespace = %s", NameStr(namespace))));

	payload += strlen(NameStr(namespace)) + 1;
	// Skip + Return
	payload += 8;

	b.data = payload;
	json = bson_to_json(NULL, b.data);
	ereport(LOG,
			(errmsg("json = %s", json)));

	funcoid = find_function("mongres_find(text, json, int, int)");

	result = OidFunctionCall4(funcoid,
							  PointerGetDatum(cstring_to_text(NameStr(namespace))),
							  PointerGetDatum(cstring_to_text(json)),
							  Int32GetDatum(-1),
							  Int32GetDatum(0));
	pfree(json);

	initStringInfo(&output);
	//{
	//	bson bres;
	//	bres.data = json_to_bson(DatumGetTextP(result));
	//	appendBinaryStringInfo(&output, bres.data, bson_size(&bres));
	//	nelem = DatumGetInt32(DirectFunctionCall1(json_array_length, result));
	//}
	deconstruct_array(DatumGetArrayTypeP(result),
					  JSONOID, -1, false, 'i', &values, NULL, &nelem);
	for (i = 0; i < nelem; i++)
	{
		bson	bres;

		bres.data = json_to_bson(DatumGetTextP(values[i]));
		appendBinaryStringInfo(&output, bres.data, bson_size(&bres));
	}

	return send_reply(sock, header, &output, nelem);
}

static bool
handle_message(pgsocket sock, SockAddr raddr)
{
	mongo_header	header;
	int				payload_size;
	char		   *payload;

	if (read(sock, &header, sizeof(header)) != sizeof(header))
	{
		ereport(LOG,
				(errmsg("could not read message header: %m")));
		return false;
	}

	payload_size = header.len - sizeof(header);

	payload = (char *) palloc(payload_size);
	if (read(sock, payload, payload_size) != payload_size)
	{
		ereport(LOG,
				(errmsg("could not read payload: %m")));
		return false;
	}

	switch (header.op)
	{
		case MONGO_OP_MSG:
			break;
		case MONGO_OP_UPDATE:
			break;
		case MONGO_OP_INSERT:
			return handle_insert(sock, &header, payload, payload_size);
			break;
		case MONGO_OP_QUERY:
			return handle_query(sock, &header, payload, payload_size);
			break;
		case MONGO_OP_GET_MORE:
			return handle_query(sock, &header, payload, payload_size);
		case MONGO_OP_DELETE:
			break;
		case MONGO_OP_KILL_CURSORS:
			break;
		default:
			ereport(LOG,
					(errmsg("unknown message op: %d", header.op)));
	}

	pfree(payload);

	return false;
}

/*
 * Initialize workspace for a worker process: create the schema if it doesn't
 * already exist.
 */
static void
initialize_mongres(void)
{
//	int		ret;
//	int		ntup;
//	bool	isnull;
//	StringInfoData	buf;
	int		status;

	MGListenSocket[0] = PGINVALID_SOCKET;

	status = StreamServerPort(AF_UNSPEC, NULL,
							  (unsigned short) mongres_listen_port,
							  NULL,
							  MGListenSocket, 1);

	if (status != STATUS_OK)
		ereport(WARNING,
				(errmsg("could not listen port %d", mongres_listen_port)));

//	SetCurrentStatementStartTimestamp();
//	StartTransactionCommand();
//	SPI_connect();
//	PushActiveSnapshot(GetTransactionSnapshot());
//	pgstat_report_activity(STATE_RUNNING, "initializing mongres");
//
//	PopActiveSnapshot();
//	CommitTransactionCommand();
//	pgstat_report_activity(STATE_IDLE, NULL);
}

static void
mongres_main(void *main_arg)
{
//	worktable	   *table = (worktable *) main_arg;
//	StringInfoData	buf;
	pgsocket sock = 0;
	SockAddr raddr;
	sigjmp_buf	local_sigjmp_buf;

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("mongres", NULL);

//	elog(LOG, "%s initialized with %s.%s",
//		 MyBgworkerEntry->bgw_name, table->schema, table->name);
	initialize_mongres();

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		closesocket(sock);
		/* Report the error to the client and/or server log */
		EmitErrorReport();

		HOLD_INTERRUPTS();
		/*
		 * Abort the current transaction in order to recover.
		 */
		AbortCurrentTransaction();
		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm)
	{
//		int		ret;
		int		rc;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatchOrSocket(&MyProc->procLatch,
							   WL_LATCH_SET | WL_TIMEOUT |
							   WL_POSTMASTER_DEATH | WL_SOCKET_READABLE,
							   MGListenSocket[0], 1000L);
		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (rc & WL_SOCKET_READABLE)
		{

			raddr.salen = sizeof(raddr.addr);
			sock = accept(MGListenSocket[0],
						  (struct sockaddr *) &raddr, &raddr.salen);

			if (sock < 0)
			{
				ereport(LOG,
						(errcode_for_socket_access(),
						 errmsg("could not accept new connection: %m")));
				continue;
			}

			for (;;)
			{
				bool	res;

				SetCurrentStatementStartTimestamp();
				StartTransactionCommand();
				PushActiveSnapshot(GetTransactionSnapshot());
				res = handle_message(sock, raddr);
				PopActiveSnapshot();
				CommitTransactionCommand();

				if (!res)
					break;
			}
			closesocket(sock);
		}
		///*
		// * Start a transaction on which we can run queries.  Note that each
		// * StartTransactionCommand() call should be preceded by a
		// * SetCurrentStatementStartTimestamp() call, which sets both the time
		// * for the statement we're about the run, and also the transaction
		// * start time.  Also, each other query sent to SPI should probably be
		// * preceded by SetCurrentStatementStartTimestamp(), so that statement
		// * start time is always up to date.
		// *
		// * The SPI_connect() call lets us run queries through the SPI manager,
		// * and the PushActiveSnapshot() call creates an "active" snapshot which
		// * is necessary for queries to have MVCC data to work on.
		// *
		// * The pgstat_report_activity() call makes our activity visible through
		// * the pgstat views.
		// */
		//SetCurrentStatementStartTimestamp();
		//StartTransactionCommand();
		//SPI_connect();
		//PushActiveSnapshot(GetTransactionSnapshot());
		//pgstat_report_activity(STATE_RUNNING, buf.data);

		///* We can now execute queries via SPI */
		//ret = SPI_execute(buf.data, false, 0);

		//if (ret != SPI_OK_UPDATE_RETURNING)
		//	elog(FATAL, "cannot select from table %s.%s: error code %d",
		//		 table->schema, table->name, ret);

		//if (SPI_processed > 0)
		//{
		//	bool	isnull;
		//	int32	val;

		//	val = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
		//									   SPI_tuptable->tupdesc,
		//									   1, &isnull));
		//	if (!isnull)
		//		elog(LOG, "%s: count in %s.%s is now %d",
		//			 MyBgworkerEntry->bgw_name,
		//			 table->schema, table->name, val);
		//}

		///*
		// * And finish our transaction.
		// */
		//SPI_finish();
		//PopActiveSnapshot();
		//CommitTransactionCommand();
		//pgstat_report_activity(STATE_IDLE, NULL);
	}

	proc_exit(0);
}

/*
 * Entrypoint of this module.
 *
 * We register more than one worker process here, to demonstrate how that can
 * be done.
 */
void
_PG_init(void)
{
	BackgroundWorker	worker;

	/* get the configuration */
	DefineCustomIntVariable("mongres.listen_port",
				"mongres listen port",
				NULL,
				&mongres_listen_port,
				27017,
				1024,
				65535,
				PGC_POSTMASTER,
				0,
				NULL,
				NULL,
				NULL);

	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = mongres_main;
	worker.bgw_sighup = mongres_sighup;
	worker.bgw_sigterm = mongres_sigterm;
	worker.bgw_name = "mongres";

	RegisterBackgroundWorker(&worker);
}
