/*-------------------------------------------------------------------------
 *
 * zicio.h
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/zicio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ZICIO_H
#define ZICIO_H

#include "storage/lwlock.h"
#include "utils/relcache.h"

#define MAX_SHAREDPOOL_NUMS 128

typedef struct ZicioSharedpoolData
{
	int queryNumber;
	int relationOid;
	int zicioSharedpoolKey;
	int fds[MAX_NUM_ZICIO_FD];	/* fd array pointer */
	int fdNums;						/* # of fd */
} ZicioSharedpoolData;

typedef ZicioSharedpoolData* ZicioSharedpool;

typedef struct ZicioMetaData
{
	/* Zicio Sharedpools */
	ZicioSharedpoolData sharedpools[MAX_SHAREDPOOL_NUMS];
	int sharedpoolCapacity;	/* capacity of sharedpool */
	int sharedpoolNums;		/* the # of sharedpool */

	LWLock sharedpoolLock;
} ZicioMetaData;

typedef ZicioMetaData* ZicioMeta;

extern Size ZicioShmemSize(void);
extern void InitZicioMeta(void);
extern void ZicioSharedpoolLockAcquire(LWLockMode mode);
extern void ZicioSharedpoolLockRelease(void);
extern ZicioSharedpool FindZicioSharedpool(int relationOid, 
														int queryNumber);
extern bool InsertZicioSharedpool(int relationOid, int queryNumber, 
											int zicioSharedpoolKey, 
											int *fds, int fdNums);
extern bool DeleteZicioSharedpool(int relationOid, int queryNumber);

#endif						
