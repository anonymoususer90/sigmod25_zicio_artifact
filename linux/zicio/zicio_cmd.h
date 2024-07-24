#ifndef __ZICIO_CMD_H
#define __ZICIO_CMD_H

#include <linux/math.h>
#include "zicio_mem.h"

typedef struct zicio_device zicio_device;

/* NVMe controller page shift */
#define ZICIO_NVME_CTRL_PAGE_SHIFT	12
/* Default page size handled by NVMe */
#define ZICIO_NVME_CTRL_PAGE_SIZE	(1 << ZICIO_NVME_CTRL_PAGE_SHIFT)
/* Shift to the default sector size of 512 bytes */
#define ZICIO_NVME_SECTOR_SHIFT 9
/* Sector size of NVMe */
#define ZICIO_NVME_SECTOR_SIZE (1 << ZICIO_NVME_SECTOR_SHIFT)
/* Shift to convert page to sector */
#define ZICIO_PAGE_TO_SECTOR_SHIFT (ZICIO_PAGE_SHIFT - \
			ZICIO_NVME_SECTOR_SHIFT)
/* Macro to convert chunk ID to local page number */
#define ZICIO_START_PAGENUM_IN_CHUNK(x) ((unsigned)(x) \
			<< ZICIO_CHUNK_ORDER)
#define ZICIO_START_PAGENUM_IN_CHUNK_WITH_STRIDE(x, stride) ((unsigned)(x) \
			<< (ZICIO_CHUNK_ORDER + stride))

/* Set memory page related information for zombie command */
void zicio_set_zombie_command_list(zicio_descriptor *desc,
			zicio_nvme_cmd_list **cmd_list, int local_huge_page_idx);
void zicio_set_command_creator(zicio_device *zicio_device);

static inline bool
zicio_has_cmd_list_in_set(zicio_nvme_cmd_list **cmd_lists, int num_dev)
{
	int i;
	if (!cmd_lists) {
		return false;
	}

	for (i = 0 ; i < num_dev ; i++) {
		if (cmd_lists[i]) {
			return true;
		}
	}
	return false;
}
#endif
