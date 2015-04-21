/*
 * decoding_json.c
 */

/*-------------------------------------------------------------------------
 *
 * test_decoding.c
 *		  example logical decoding output plugin
 *
 * Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/test_decoding/test_decoding.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/sysattr.h"

#include "catalog/pg_class.h"
#include "catalog/pg_type.h"

#include "nodes/parsenodes.h"

#include "replication/output_plugin.h"
#include "replication/logical.h"

#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

extern void _PG_init(void);
extern void _PG_output_plugin_init(OutputPluginCallbacks *cb);

typedef struct _DecodingJsonData DecodingJsonData;
struct _DecodingJsonData {
	MemoryContext context;
	bool xact_wrote_changes;
};

static void pg_decode_startup(LogicalDecodingContext* ctx, OutputPluginOptions* opt, bool is_init);
static void pg_decode_shutdown(LogicalDecodingContext* ctx);
static void pg_decode_begin_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn);
static void pg_output_begin(LogicalDecodingContext* ctx, DecodingJsonData* data, ReorderBufferTXN* txn, bool last_write);
static void pg_decode_commit_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, XLogRecPtr commit_lsn);
static void pg_decode_change(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, Relation rel, ReorderBufferChange* change);

void _PG_init(void) {
}

void _PG_output_plugin_init(OutputPluginCallbacks *cb) {
	AssertVariableIsOfType(&_PG_output_plugin_init, LogicalOutputPluginInit);

	cb->startup_cb = pg_decode_startup;
	cb->begin_cb = pg_decode_begin_txn;
	cb->change_cb = pg_decode_change;
	cb->commit_cb = pg_decode_commit_txn;
	cb->shutdown_cb = pg_decode_shutdown;
}

static void pg_decode_startup(LogicalDecodingContext* ctx, OutputPluginOptions* opt, bool is_init) {
	DecodingJsonData* data;

	data = palloc0(sizeof(DecodingJsonData));
	data->context = AllocSetContextCreate(
		ctx->context,
	  "text conversion context",
	  ALLOCSET_DEFAULT_MINSIZE,
	  ALLOCSET_DEFAULT_INITSIZE,
	  ALLOCSET_DEFAULT_MAXSIZE
	);

	ctx->output_plugin_private = data;
	opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
}

static void pg_decode_shutdown(LogicalDecodingContext* ctx) {
	DecodingJsonData* data = ctx->output_plugin_private;
	MemoryContextDelete(data->context);
}

static void pg_decode_begin_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn) {
	DecodingJsonData* data = ctx->output_plugin_private;
	data->xact_wrote_changes = false;
	pg_output_begin(ctx, data, txn, true);
}

static void pg_output_begin(LogicalDecodingContext* ctx, DecodingJsonData* data, ReorderBufferTXN* txn, bool last_write) {
	OutputPluginPrepareWrite(ctx, last_write);
	appendStringInfoString(ctx->out, "BEGIN (testing)");
	OutputPluginWrite(ctx, last_write);
}

static void pg_decode_commit_txn(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, XLogRecPtr commit_lsn) {
	//DecodingJsonData* data = ctx->output_plugin_private;
	OutputPluginPrepareWrite(ctx, true);
  appendStringInfoString(ctx->out, "COMMIT");
	OutputPluginWrite(ctx, true);
}

static void print_literal(StringInfo s, Oid typid, char* outputstr) {
	const char* valptr;

	switch (typid) {
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			appendStringInfoString(s, outputstr);
			break;

		case BITOID:
		case VARBITOID:
			appendStringInfo(s, "B'%s'", outputstr);
			break;

		case BOOLOID:
			if (strcmp(outputstr, "t") == 0)
				appendStringInfoString(s, "true");
			else
				appendStringInfoString(s, "false");
			break;

		default:
			appendStringInfoChar(s, '\'');
			for (valptr = outputstr; *valptr; valptr++) {
				char ch = *valptr;

				if (SQL_STR_DOUBLE(ch, false))
					appendStringInfoChar(s, ch);
				appendStringInfoChar(s, ch);
			}
			appendStringInfoChar(s, '\'');
			break;
	}
}

static void tuple_to_stringinfo(StringInfo s, TupleDesc tupdesc, HeapTuple tuple, bool skip_nulls) {
	int natt;
	Oid oid;

	if ((oid = HeapTupleHeaderGetOid(tuple->t_data)) != InvalidOid) {
		appendStringInfo(s, " oid[oid]:%u", oid);
	}

	for (natt = 0; natt < tupdesc->natts; natt++) {
		Form_pg_attribute attr;
		Oid typid;
		Oid typoutput;
		bool typisvarlena;
		Datum origval;
		bool isnull;

		attr = tupdesc->attrs[natt];

		if (attr->attisdropped)
			continue;

		if (attr->attnum < 0)
			continue;

		typid = attr->atttypid;

		/* get Datum from tuple */
		origval = fastgetattr(tuple, natt + 1, tupdesc, &isnull);

		if (isnull && skip_nulls)
			continue;

		/* print attribute name */
		appendStringInfoChar(s, ' ');
		appendStringInfoString(s, quote_identifier(NameStr(attr->attname)));

		/* print attribute type */
		appendStringInfoChar(s, '[');
		appendStringInfoString(s, format_type_be(typid));
		appendStringInfoChar(s, ']');

		/* query output function */
		getTypeOutputInfo(typid, &typoutput, &typisvarlena);

		/* print separator */
		appendStringInfoChar(s, ':');

		/* print data */
		if (isnull) {
			appendStringInfoString(s, "null");
    } else if (typisvarlena && VARATT_IS_EXTERNAL_ONDISK(origval)) {
			appendStringInfoString(s, "unchanged-toast-datum");
    } else if (!typisvarlena) {
			print_literal(s, typid, OidOutputFunctionCall(typoutput, origval));
    } else {
			Datum val;	/* definitely detoasted Datum */

			val = PointerGetDatum(PG_DETOAST_DATUM(origval));
			print_literal(s, typid, OidOutputFunctionCall(typoutput, val));
		}
	}
}

/*
 * callback for individual changed tuples
 */
static void pg_decode_change(LogicalDecodingContext* ctx, ReorderBufferTXN* txn, Relation relation, ReorderBufferChange* change) {
	DecodingJsonData* data;
	Form_pg_class class_form;
	TupleDesc	tupdesc;
	MemoryContext old;

	data = ctx->output_plugin_private;

	/* output BEGIN if we haven't yet */
	if (!data->xact_wrote_changes) {
		pg_output_begin(ctx, data, txn, false);
	}
	data->xact_wrote_changes = true;

	class_form = RelationGetForm(relation);
	tupdesc = RelationGetDescr(relation);

	old = MemoryContextSwitchTo(data->context);

	OutputPluginPrepareWrite(ctx, true);

	appendStringInfoString(ctx->out, "table ");
	appendStringInfoString(
    ctx->out,
    quote_qualified_identifier(
      get_namespace_name(
        get_rel_namespace(
          RelationGetRelid(relation)
        )
      ),
      NameStr(class_form->relname)
    )
  );
	appendStringInfoString(ctx->out, ":");

	switch (change->action) {
		case REORDER_BUFFER_CHANGE_INSERT:
			appendStringInfoString(ctx->out, " INSERT:");
			if (change->data.tp.newtuple == NULL) {
				appendStringInfoString(ctx->out, " (no-tuple-data)");
      } else {
				tuple_to_stringinfo(
          ctx->out, tupdesc,
          &change->data.tp.newtuple->tuple,
          false
        );
      }
			break;
		case REORDER_BUFFER_CHANGE_UPDATE:
			appendStringInfoString(ctx->out, " UPDATE:");
			if (change->data.tp.oldtuple != NULL) {
				appendStringInfoString(ctx->out, " old-key:");
				tuple_to_stringinfo(
          ctx->out, tupdesc,
          &change->data.tp.oldtuple->tuple,
          true
        );
				appendStringInfoString(ctx->out, " new-tuple:");
			}

			if (change->data.tp.newtuple == NULL) {
				appendStringInfoString(ctx->out, " (no-tuple-data)");
      } else {
				tuple_to_stringinfo(
          ctx->out, tupdesc,
          &change->data.tp.newtuple->tuple,
          false
        );
      }
			break;
		case REORDER_BUFFER_CHANGE_DELETE:
			appendStringInfoString(ctx->out, " DELETE:");

			/* if there was no PK, we only know that a delete happened */
			if (change->data.tp.oldtuple == NULL) {
				appendStringInfoString(ctx->out, " (no-tuple-data)");
      } else {
        /* In DELETE, only the replica identity is present; display that */
				tuple_to_stringinfo(
          ctx->out, tupdesc,
          &change->data.tp.oldtuple->tuple,
          true
        );
      }
			break;
		default:
			Assert(false);
	}

	MemoryContextSwitchTo(old);
	MemoryContextReset(data->context);

	OutputPluginWrite(ctx, true);
}
