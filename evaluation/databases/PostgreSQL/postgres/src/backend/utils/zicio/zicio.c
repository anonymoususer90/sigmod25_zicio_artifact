/*-------------------------------------------------------------------------
 *
 * zicio.c
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/zicio/zicio.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef ZICIO

#include <libzicio.h>

#include "postgres.h"
#include "storage/shmem.h"
#include "utils/zicio.h"

static ZicioMeta ShmemZicioMeta;

/*
 * ZicioShmemSize
 *
 * We calculate shared memory size for zicio metadata.
 */
Size
ZicioShmemSize(void)
{
	Size		size;

	size = sizeof(ZicioMetaData);
	return size;
}

/*
 * InitZicioMeta
 */
void
InitZicioMeta(void)
{
	bool		foundSharedpools;

	/* Align descriptors to a cacheline boundary. */
	ShmemZicioMeta = (ZicioMeta)
		ShmemInitStruct("Zicio Sharedpools", sizeof(ZicioMetaData),
														&foundSharedpools);

	if (!foundSharedpools)
	{
		/* Init values. */
		LWLockInitialize(&ShmemZicioMeta->sharedpoolLock, 
									LWTRANCHE_ZICIO_SHAREDPOOL);
		ShmemZicioMeta->sharedpoolCapacity = MAX_SHAREDPOOL_NUMS;
		ShmemZicioMeta->sharedpoolNums = 0;

		for (int i = 0; i < ShmemZicioMeta->sharedpoolCapacity; ++i)
		{
			memset(&ShmemZicioMeta->sharedpools[i], 0,
							sizeof(ZicioSharedpoolData));
		}
	}
}

/* 
 * ZicioSharedpoolLockAcquire 
 */
void
ZicioSharedpoolLockAcquire(LWLockMode mode)
{
	LWLockAcquire(&ShmemZicioMeta->sharedpoolLock, mode);
}

/* 
 * ZicioSharedpoolLockRelease
 */
void
ZicioSharedpoolLockRelease(void)
{
	LWLockRelease(&ShmemZicioMeta->sharedpoolLock);
}

/*
 * FindZicioSharedpool
 *
 * Find sharedpool from zicio meta.
 *
 * Return NULL, if not find.
 * Return ZicioSharepool pointer, if find.
 */
ZicioSharedpool
FindZicioSharedpool(int relationOid, int queryNumber)
{
	ZicioSharedpool ret = NULL;

	for (int i = 0; i < ShmemZicioMeta->sharedpoolCapacity; ++i)
	{
		ZicioSharedpool tmp = &ShmemZicioMeta->sharedpools[i];

		if (tmp->relationOid == relationOid && tmp->queryNumber == queryNumber)
		{
			/* We find it from array. */
			ret = tmp;
			break;
		}
	}

	return ret;
}


/*
 * InsertZicioSharedpool
 *
 * Insert sharedpool from zicio meta.
 */
bool 
InsertZicioSharedpool(int relationOid, int queryNumber, 
							int zicioSharedpoolKey, int *fds, int fdNums)
{
	bool ret = false;

	/* Check if it already exists. */
	if (FindZicioSharedpool(relationOid, queryNumber))
		return ret;

	/* Check if it has empty entry. */
	if (ShmemZicioMeta->sharedpoolCapacity <=
					ShmemZicioMeta->sharedpoolNums)
		return ret;

	for (int i = 0; i < ShmemZicioMeta->sharedpoolCapacity; ++i)
	{
		ZicioSharedpool tmp = &ShmemZicioMeta->sharedpools[i];

		if (tmp->relationOid == 0 && tmp->queryNumber == 0)
		{
			/* Write values for insertion. */
			tmp->relationOid = relationOid;
			tmp->queryNumber = queryNumber;
			tmp->zicioSharedpoolKey = zicioSharedpoolKey;
			tmp->fdNums = fdNums;

			memcpy(tmp->fds, fds, fdNums * sizeof(int));

			ShmemZicioMeta->sharedpoolNums += 1;
			ret = true;
			break;
		}
	}

	return ret;
}


/*
 * DeleteZicioSharedpool
 *
 * Delete sharedpool from zicio meta.
 */
bool
DeleteZicioSharedpool(int relationOid, int queryNumber)
{
	bool ret = false;

	for (int i = 0; i < ShmemZicioMeta->sharedpoolCapacity; ++i)
	{
		ZicioSharedpool tmp = &ShmemZicioMeta->sharedpools[i];

		if (tmp->relationOid == relationOid && tmp->queryNumber == queryNumber)
		{
			/* Init values for deletion. */
			tmp->relationOid = 0;
			tmp->queryNumber = 0;
			tmp->zicioSharedpoolKey = 0;
			tmp->fdNums = 0;
			memset(tmp->fds, 0, sizeof(int) * MAX_NUM_ZICIO_FD);

			ShmemZicioMeta->sharedpoolNums -= 1;
			ret = true;
			break;
		}
	}

	return ret;
}

#endif
