#ifndef __ZICIO_TRIGGER_H
#define __ZICIO_TRIGGER_H

#include <linux/zicio_notify.h>

#define ZICIO_LOCAL_INIT_PAGE_IDX 0
#define ZICIO_INIT_PER_CMD_KB 32

long zicio_init_read_trigger(long id);
ssize_t zicio_trigger_read_from_softirq(zicio_descriptor *desc,
			zicio_nvme_cmd_list *cmd, unsigned cpu_id);
#endif /* ZICIO_TRIGGER_H */
