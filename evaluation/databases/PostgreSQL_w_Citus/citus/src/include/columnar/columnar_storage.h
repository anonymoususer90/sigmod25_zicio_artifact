/*-------------------------------------------------------------------------
 *
 * columnar_storage.h
 *
 * Type and function declarations for storage of columnar data in blocks.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLUMNAR_STORAGE_H
#define COLUMNAR_STORAGE_H

#include "postgres.h"

#include "storage/smgr.h"
#include "utils/rel.h"

#include "columnar/columnar_tableam.h"

#ifdef ZICIO
#include <libzicio.h>
#endif

#define COLUMNAR_INVALID_ROW_NUMBER ((uint64) 0)
#define COLUMNAR_FIRST_ROW_NUMBER ((uint64) 1)
#define COLUMNAR_MAX_ROW_NUMBER ((uint64) \
								 (COLUMNAR_FIRST_ROW_NUMBER + \
								  VALID_ITEMPOINTER_OFFSETS * \
								  VALID_BLOCKNUMBERS))


/*
 * Logical offsets never fall on the first two physical pages. See
 * comments in columnar_storage.c.
 */
#define ColumnarInvalidLogicalOffset 0
#define ColumnarFirstLogicalOffset ((BLCKSZ - SizeOfPageHeaderData) * 2)
#define ColumnarLogicalOffsetIsValid(X) ((X) >= ColumnarFirstLogicalOffset)

#ifdef ZICIO
typedef struct ZICIODesc
{
	struct zicio zicio;

	bool should_scan;
#ifdef ZICIO_DEBUG
	int notify_page_nums;
	int put_page_nums;
	int get_page_nums;
#endif
} ZICIODesc;

typedef struct PhysicalAddr
{
	BlockNumber blockno;
	uint32 offset;
} PhysicalAddr;

extern PhysicalAddr ZICIOLogicalToPhysical(uint64 logicalOffset);
extern uint64 ZICIOPhysicalToLogical(PhysicalAddr addr);
extern void ZICIOGetPhysicalLocation(Relation rel, uint64 logicalOffset,
											uint64 *physicalOffset, int *fd);
#endif

extern void ColumnarStorageInit(SMgrRelation srel, uint64 storageId);
extern bool ColumnarStorageIsCurrent(Relation rel);
extern void ColumnarStorageUpdateCurrent(Relation rel, bool upgrade,
										 uint64 reservedStripeId,
										 uint64 reservedRowNumber,
										 uint64 reservedOffset);

extern uint64 ColumnarStorageGetVersionMajor(Relation rel, bool force);
extern uint64 ColumnarStorageGetVersionMinor(Relation rel, bool force);
extern uint64 ColumnarStorageGetStorageId(Relation rel, bool force);
extern uint64 ColumnarStorageGetReservedStripeId(Relation rel, bool force);
extern uint64 ColumnarStorageGetReservedRowNumber(Relation rel, bool force);
extern uint64 ColumnarStorageGetReservedOffset(Relation rel, bool force);

extern uint64 ColumnarStorageReserveData(Relation rel, uint64 amount);
extern uint64 ColumnarStorageReserveRowNumber(Relation rel, uint64 nrows);
extern uint64 ColumnarStorageReserveStripeId(Relation rel);

#ifdef ZICIO
extern void ColumnarStorageRead(Relation rel, uint64 logicalOffset,
				char *data, uint32 amount, bool shouldPut, ZICIODesc *desc); 
#else
extern void ColumnarStorageRead(Relation rel, uint64 logicalOffset,
								char *data, uint32 amount);
#endif
extern void ColumnarStorageWrite(Relation rel, uint64 logicalOffset,
								 char *data, uint32 amount);
extern bool ColumnarStorageTruncate(Relation rel, uint64 newDataReservation);

#endif /* COLUMNAR_STORAGE_H */
