/*
 * pg_hexedit_poke.c - write raw bytes into a shared-buffer page.
 *
 * There is no standard way to write into shared_buffers (pageinspect's
 * get_raw_page() reads, but has no inverse), so pg_hexedit's live GDB bridge
 * cannot persist edits without a little backend code.  This extension
 * provides exactly one function: overwrite a byte range within one page,
 * in the buffer, and mark it dirty so it is eventually flushed to disk.
 *
 * The write is NOT WAL-logged: it changes the page in shared memory (and, at
 * the next checkpoint/bgwriter flush, on disk) but does not replicate, is not
 * crash-safe until flushed, and does not advance the page LSN.  That suits a
 * raw hex-editor / corruption-simulation tool, and it is why this is a
 * separate, superuser-only, dev-only extension rather than anything general.
 *
 * DO NOT install this where you care about the data.
 */
#include "postgres.h"

#include "access/relation.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "varatt.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_hexedit_poke);

/*
 * pg_hexedit_poke(rel regclass, blkno int, offset int, data bytea) -> void
 *
 * Overwrite data at the given byte offset within block blkno of rel's main
 * fork, in shared_buffers.
 */
Datum
pg_hexedit_poke(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	int32		offset = PG_GETARG_INT32(2);
	bytea	   *data = PG_GETARG_BYTEA_PP(3);
	int			len = VARSIZE_ANY_EXHDR(data);
	Relation	rel;
	Buffer		buf;
	Page		page;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to use pg_hexedit_poke")));

	if (offset < 0 || len < 0 || (int64) offset + len > BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("write of %d bytes at offset %d does not fit in an %d byte block",
						len, offset, (int) BLCKSZ)));

	/* AccessShareLock just keeps the relation from being dropped under us */
	rel = relation_open(relid, AccessShareLock);

	if (blkno < 0 || (BlockNumber) blkno >= RelationGetNumberOfBlocks(rel))
	{
		BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

		relation_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("block number %d is out of range for relation with %u blocks",
						blkno, nblocks)));
	}

	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	if (len > 0)
	{
		memcpy((char *) page + offset, VARDATA_ANY(data), len);
		MarkBufferDirty(buf);
	}

	UnlockReleaseBuffer(buf);
	relation_close(rel, AccessShareLock);

	PG_RETURN_VOID();
}
