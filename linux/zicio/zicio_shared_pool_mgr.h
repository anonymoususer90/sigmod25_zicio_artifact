#ifndef __ZICIO_SHARED_POOL_MGR_H
#define __ZICIO_SHARED_POOL_MGR_H

#include <linux/types.h>
#include <linux/zicio_notify.h>
#include <uapi/linux/zicio.h>

#include "zicio_shared_pool.h"

void __init zicio_init_shared_pool_mgr(void);
long zicio_close_shared_pool(zicio_shared_pool_key_t zicio_shared_pool_key);
long zicio_detach_shared_pool(zicio_descriptor *desc,
			zicio_global_shared_pool_desc *zicio_desc);
long zicio_delete_all_shared_pool(void);
void zicio_dump_all_shared_pool(void);
long zicio_attach_channel_to_shared_pool(unsigned int *fs, struct fd *fd,
		struct zicio_args *zicio_args, zicio_descriptor *desc);
u64 zicio_get_nsecs_from_jiffy(void);

#define ZICIO_SHARED_POOL_BASE 65536
#define ZICIO_SHARED_POOL_MAX (UINT_MAX - ZICIO_SHARED_POOL_BASE)
#define ZICIO_SHARED_POOL_KEY_ENCODE(id) (id + ZICIO_SHARED_POOL_BASE)
#define ZICIO_SHARED_POOL_KEY_DECODE(id) (id - ZICIO_SHARED_POOL_BASE)
#endif
