/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The ZicIO infrastructure. Public interface and data structures
 */

#include <linux/blk_types.h>
#include <linux/types.h>
#include <linux/rbtree_types.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/nvme.h>

#include <linux/file.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <uapi/linux/zicio.h>
#include <asm/current.h>

#ifndef __LINUX_ZICIO_H
#define __LINUX_ZICIO_H

#ifdef CONFIG_ZICIO
/* ZicIO flags */
#define ZICIO_FLAG_NORMAL (0)
#define ZICIO_FLAG_PREMAPPING_TEST (1)
#define ZICIO_FLAG_FORCEFUL_UNMAPPING_TEST (2)

#define NR_ZICIO_OPEN_DEFAULT BITS_PER_LONG
#define ZICIO_MAX_NUM_ALLOC UINT_MAX

/* 
 * TODO: We only assume that data ingestion is handled locally, we change this
 * logic later.
 */
/* Data buffer chunk size */
#define ZICIO_DATABUFFER_CHUNK_SIZE (1UL << 21) /* 2 MiB */
/* Data buffer chunk count */
#define ZICIO_DATABUFFER_CHUNK_NUM (16UL)
#define ZICIO_INODEBUFFER_CHUNK_NUM (1UL)
/* Data buffer chunk number mask*/
#define ZICIO_DATABUFFER_NUM_MASK (~(ZICIO_DATABUFFER_CHUNK_NUM - 1))
/* Data buffer size */
#define ZICIO_DATABUFFER_SIZE \
	(ZICIO_DATABUFFER_CHUNK_NUM * ZICIO_DATABUFFER_CHUNK_SIZE) /* 32 MiB */
#define ZICIO_DATABUFFER_PAGENUM \
	(ZICIO_DATABUFFER_SIZE / ZICIO_PAGE_SIZE)


#define ZICIO_BITBIT_NR(nr) BITS_TO_LONGS(BITS_TO_LONGS(nr))
#define ZICIO_BITBIT_SIZE(nr) (ZICIO_BITBIT_NR(nr) * sizeof(long))

#define ZICIO_HUGE_PAGE_SHIFT (PAGE_SHIFT + 9UL)
#define ZICIO_HUGE_PAGE_SIZE (1 << ZICIO_HUGE_PAGE_SHIFT)

/* The number of support raw device */
#define ZICIO_NUM_SUPP_RAW_DEV 1

/* Page layout for zicio, it just follows the config of kernel */
/* Chunk size order of zicio */
#define ZICIO_CHUNK_SHIFT (21UL)
/* Page size order of zicio */
#define ZICIO_PAGE_SHIFT PAGE_SHIFT
/* Page size of zicio */
#define ZICIO_PAGE_SIZE	PAGE_SIZE
/* Page mask of sanatace */
#define ZICIO_PAGE_MASK	(~(ZICIO_PAGE_SIZE-1))
/* Macro to round up */
#define ZICIO_PAGE_ROUNDUP(sz) \
			(((sz) + ZICIO_PAGE_SIZE - 1) \
			& ~(ZICIO_PAGE_SIZE-1))
#define ZICIO_PAGE_TO_CHUNK_SHIFT (ZICIO_CHUNK_SHIFT - \
			ZICIO_PAGE_SHIFT)
#define ZICIO_PAGE_TO_CHUNK_MASK (~(((1UL) << \
			ZICIO_PAGE_TO_CHUNK_SHIFT) - 1))

/* Order of zicio chunk */
#define ZICIO_CHUNK_ORDER (9UL)
#define ZICIO_SWITCHBOARD_ORDER 0
#define ZICIO_STATBOARD_ORDER 0
/* One buffe chunk for zicio */
#define ZICIO_CHUNK_SIZE (1UL << (ZICIO_CHUNK_ORDER + \
			ZICIO_PAGE_SHIFT))
#define ZICIO_NVME_PAGE_SIZE 4096

/* Order of bit to 1 byte */
#define ZICIO_BYTE_MIN_ORDER 3
/* Order of 1byte to 8byte */
#define ZICIO_ULONG_ORDER 3

#define ZICIO_INTERNAL_BITVECTOR_ORDER 6
#define ZICIO_INTERNAL_BIT_CHUNK_COVER_ORDER (ZICIO_BYTE_MIN_ORDER + \
			ZICIO_INTERNAL_BITVECTOR_ORDER)
#define ZICIO_INTERNAL_BITVECTOR_MASK (~((sizeof(unsigned long) << \
			ZICIO_BYTE_MIN_ORDER) - 1))
#define ZICIO_GET_CHUNK_BIT_LOC (x) (x >> \
			ZICIO_BYTE_MIN_ORDER)

#define zicio_free_if_not_null(x)			\
	do {			\
		if (x) kfree(x);		\
	} while (0)

#define ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX		(-1)
#define ZICIO_DEFAULT_TIMER_SOFTIRQ_INTERVAL	(25)
#define ZICIO_DEFAULT_ZOMBIE_WAKEUP_INTERVAL	(5)

/*
 * zicio_get_per_cpu_ptr_with_dev
 * Get per cpu pointer matched with dev
 */
#define zicio_get_per_cpu_ptr_with_dev(ptr, cpu, dev_idx) ({			\
	void __percpu *__ptr_dev_ptr_base = (per_cpu_ptr(&ptr, cpu));			\
	(likely((*((typeof((ptr)) __kernel __force *)(__ptr_dev_ptr_base))))) ?	\
		*((typeof((ptr)) __kernel __force *)(__ptr_dev_ptr_base)) +			\
			(dev_idx) : NULL; })

/*
 * zicio_get_per_cpu_ptr_with_dev
 * Get this cpu pointer matched with dev
 */
#define zicio_get_this_cpu_ptr_with_dev(ptr, dev_idx) ( \
	zicio_get_per_cpu_ptr_with_dev(ptr, raw_smp_processor_id(), dev_idx))

#define zicio_do_div(result, n, base) \
({											  \
	result = n;								  \
	sector_div(result, base);				  \
})

#define zicio_sector_div(result, n, base) \
		(zicio_do_div(result, n, base))

typedef struct zicio_shared_pool zicio_shared_pool;
typedef struct zicio_shared_page_control_block
		zicio_shared_page_control_block;
typedef struct zicio_descriptor zicio_descriptor;
typedef struct zicio_file_struct zicio_file_struct;
typedef struct zicio_nvme_cmd_list zicio_nvme_cmd_list;
typedef struct zicio_device zicio_device;
typedef struct zicio_dev_map_node zicio_dev_map_node;

/*
 * zicio_meter
 *
 * Structure to indicate how much data has been produced and consumed
 */
typedef struct zicio_meter {
	unsigned long consumed_no_mod; /* consumed point */
	unsigned long produced_no_mod; /* produced point */
} zicio_meter;

/*
 * zicio_chunk_bitmap_meter
 *
 * Structure to indicate how much data has been produced and consumed
 * through bitmap
 */
typedef struct zicio_chunk_bitmap_meter {
	/* The number of consumed data */
	unsigned long consumed_no_mod;
	/* The number of requested data */
	unsigned long requested_no_mod;
	/* Produced bitmap matched with pages in a chunk */
	DECLARE_BITMAP(produced,
			1UL << (ZICIO_CHUNK_SHIFT - ZICIO_PAGE_SHIFT));
} zicio_chunk_bitmap_meter;

/*
 * zicio_buffer
 *
 * Manages the addresses of buffers to be used by users and i/o handlers
 */
typedef struct zicio_buffer {
	void **data_buffer; /* data buffers */
	void *metadata_buffer; /* metadata buffer */

	int num_data_buffer_pages; /* number of data buffer huge page */
} zicio_buffer;

/* Device type to be supported currently */
enum zicio_device_type {
	ZICIO_NVME = 0,
	/* end of real device */
	ZICIO_MD
};

/* zicio command creator hook structure per device */
typedef struct zicio_command_creator {
	/*
	 * zicio command creator hook
	 * descriptor, command size, file chunk id, local page index,
	 * whether to create metadata
	 */
	/* Zombie command set */
	void (*zicio_set_device_zombie_command_list)(zicio_descriptor *,
			zicio_nvme_cmd_list **, int);
	/* Create device command for local channel */
	void *(*zicio_create_device_command)(zicio_descriptor *,
		zicio_file_struct *, ssize_t, unsigned long, int, bool);
	/* Create device command for shared pool */
	void *(*zicio_create_device_command_shared)(zicio_descriptor *,
		zicio_file_struct *, int, ssize_t, unsigned long, int, bool);
	/* Allocate device map */
	void (*zicio_allocate_dev_map)(zicio_descriptor *, 
		zicio_device *, struct device *, zicio_dev_map_node *, int,
		void *);
	/* Map dma buffer */
	void (*zicio_map_dma_buffer)(void *, int);
	/* Map dma buffer */
	void (*zicio_map_dma_buffer_shared)(void *, void *, int);
	/* Unmap dma buffer */
	void (*zicio_unmap_dma_buffer)(void*, int);
	/* Map dma buffer */
	void (*zicio_unmap_dma_buffer_shared)(void *, void *, int);
	void (*zicio_set_dma_mapping)(zicio_descriptor *,
		zicio_nvme_cmd_list *, int);
	int (*zicio_get_current_io_size)(zicio_descriptor *, int);
} zicio_command_creator;

/*
 * zicio per device private data
 */
typedef struct zicio_per_device_data {
	/* Hooks for zicio command creator */
	zicio_command_creator zicio_cmd_creator;
	/* Per-device data */
	void *device_private_data;
} zicio_per_device_data;

/*
 * zicio device struct
 */
typedef struct zicio_device {
	/* Type of device */
	enum zicio_device_type device_type;
	/* kernel device structure */
	struct device *device;
	/* Raw device index managed by the entire zicio
	   (Timer wheel, flow controller has the same order.) */
	int zicio_global_device_idx;
	/* Required information for each device type. */
	zicio_per_device_data device_private;
	/* Command lists memory pool */
	struct kmem_cache *cmd_lists_cache;
} zicio_device;

/*
 * zicio_user_maps
 *
 * User address mapping to zicio_buffer
 */
typedef struct zicio_user_maps {
	unsigned long data_buffer; /* user vm address of data buffer */
	unsigned long switch_board; /* user vm address of switch board */
	unsigned long stat_board; /* user vm address of stat board */
} zicio_user_maps;

/*
 * zicio_dev_map
 *
 * DMA mapping information for the area to be directly shown to the device 
 */
typedef struct zicio_dev_map {
	/* PRP DMA address array of data buffer 4KiB pages */
	void *prp_data_address;
	/* PRP DMA address array of inode buffer 4KiB pages */
	void *prp_inode_address;
	/* PRP DMA address of DMA address array pointing to data buffer pages */
	dma_addr_t *prp_data_array;
	/* PRP DMA address of DMA address array pointing to inode buffer pages */
	dma_addr_t prp_inode_array[ZICIO_INODEBUFFER_CHUNK_NUM];
	/* SGL DMA address array of data buffer 4KiB pages */
	void *sgl_data_address;
	/* SGL DMA address of DMA address array pointing to data buffer pages */
	dma_addr_t *sgl_data_array;
} zicio_dev_map;

/*
 * zicio_dev_map_node
 *
 * per device map in channel
 */
typedef struct zicio_dev_map_node {
	int nr_maps;
	int raw_dev_idx_in_channel;
	int start_point_inner_dev_map;
	zicio_dev_map *dev_map;
	zicio_device *zicio_devs;
	struct device *devs;
	/* ZicIO deivce array matched member array devs */
	zicio_device **zicio_inner_devs;
	struct device **inner_devs;
} zicio_dev_map_node;

/*
 * zicio_inner_dev_map
 *
 * md and devices consisting of md mapper
 */
typedef struct zicio_inner_dev_map {
	int mddev_idx;
	int raw_dev_idx_in_channel;
	zicio_device *zicio_inner_dev;
} zicio_inner_dev_map;

/*
 * zicio_dev_maps
 *
 * Mapping information generated for each different device
 */
typedef struct zicio_dev_maps {
	/* Device mapping info */
	struct device** devs;
	/* The number of device in this device map array */
	int nr_dev;
	int nr_raw_dev;
	/* The device map array */
	zicio_dev_map_node *dev_node_array;
	int nr_md_inner_dev;
	/* Inner device map for MD */
	zicio_inner_dev_map *zicio_inner_dev_maps;
} zicio_dev_maps;

/* 
 * zicio_file_struct
 *
 * Information of files accessed by zicio
 */
typedef struct zicio_file_struct {
	/* File structure for these file struct */
	struct fd fd;
	/* Indicates which device this file is located on in channel. */
	int device_idx_in_channel;
	/* size of file */
	unsigned file_size;

	//TODO: change it to the bitvector
	loff_t next_loff;

	unsigned long start_page_idx_in_set;

	zicio_meter extent_tree_meter;
	void *extent_tree_buffer;
	bool has_index_extent;
} zicio_file_struct;

/*
 * zicio_read_files
 *
 * Information about files currently being worked on and files to be worked on
 * in the future.
 */
typedef struct zicio_read_files {
	/* Atomic counter for read files */
	atomic_t count;
	/* Cursor of file descriptor */
	atomic_t fd_cursor;
	/* Gaurd for writer */
	spinlock_t read_file_guard;
	/* Wait queue for resizing wait */
	wait_queue_head_t resize_wait;
	/* Number of file descriptors */
	int num_fds;
	/* ZicIO file object array */
	zicio_file_struct __rcu **zicio_file_array;
	/* Whether resizing is progressed or not */
	bool resize_in_progress;
	/* Total file size */
	unsigned long total_file_size;
	/* Total requested size */
	atomic64_t total_requested_size;
} zicio_read_files;

/*
 * zicio_metadata_ctrl
 *
 * Structure containing information required for metadata management
 */
typedef struct zicio_metadata_ctrl {
	/* Include both the amount of metadata entries supplied and the data 
		consumed */
	atomic_t metadata_producing;
	zicio_meter metadata_meter;
	zicio_chunk_bitmap_meter inode_meter;

	void *inode_buffer;
} zicio_metadata_ctrl;

/* ZicIO command list structure for I/O handler */
typedef struct zicio_nvme_cmd_list {
	struct nvme_command cmd;
	struct fd fd;
	int device_idx;
	unsigned long start_mem;
	unsigned start_lpos;
	unsigned long start_fpos;
	int local_huge_page_idx;
	unsigned int file_chunk_id;
	struct zicio_nvme_cmd_list *next;
	bool is_metadata;
	bool is_on_track_cmd;
} zicio_nvme_cmd_list;

/**
 * zicio_firehose_ctrl
 *
 * Structure containing information required for data buffer management
 */
typedef struct zicio_firehose_ctrl {
	atomic_t requested_flag_per_local_huge_page[ZICIO_MAX_NUM_CHUNK];
	atomic_t needed_pages_per_local_huge_page[ZICIO_MAX_NUM_CHUNK];
	atomic_t filled_pages_per_local_huge_page[ZICIO_MAX_NUM_CHUNK];
	unsigned long last_user_avg_tsc_delta;
	struct list_head active_req_timers;
	struct list_head active_zombie_req_timers;
	spinlock_t lock;
	u64 requested;
	s64 bandwidth;

	/*
	 * TODO
	 * Currently, the flow controller for each device writes down the user
	 * consumption rate in user_bandwidth, a member variable of the flow
	 * controller. In the channel, user consumption rate is managed within the
	 * firehose controller structure.
	 *
	 * When a command in a device handles its completion after processing, it
	 * becomes a user bandwidth update in the flow controller for each device.
	 * And this update process is implemented in a way that subtracts the user
	 * bandwidth of the previously small channel from the user bandwidth value
	 * in the flow controller and adds the new user bandwidth obtained using
	 * the user consumption rate.
	 *
	 * If this is not recorded for each device, later, when flow out or when
	 * updating the user bandwidth for different devices in the file replacement
	 * process, the previously updated value is excluded and the new value
	 * cannot be updated.
	 */
	s64 *last_update_bandwidth;
} zicio_firehose_ctrl;

/* ZicIO structure for ghost mapping */
typedef struct zicio_ghost {
	pud_t *pud;							/* pud entry for ghost table */
	unsigned long user_base_address;	/* user address for data buffer */
	pgprot_t orig_prot;
	unsigned long orig_vm_flags;			/* vm_flags before ghost mapping */
	atomic64_t premapping_iter;		/* iterator for premapping  */
	atomic64_t unmapping_iter;			/* iterator for forceful unmapping */
} zicio_ghost;

typedef struct zicio_global_shared_pool_desc {
	/* shared pool structure */
	zicio_shared_pool_key_t shared_pool_key;

	/* shared pool data */
	void *zicio_shared_pool;

	/* shared pool channel local data */
	void *zicio_shared_pool_local;

	/* number of reserved work in softirq */
	volatile atomic_t zicio_num_works;
} zicio_global_shared_pool_desc;

#define ZICIO_SHARED_POOL_DESC_SIZE \
			sizeof(zicio_global_shared_pool_desc)

/* ZicIO descriptor structure */
typedef struct zicio_descriptor {
	struct task_struct *zicio_current; /* user proc */

	int zicio_flag;

	/* sturucture used for ghost mapping */
	zicio_ghost ghost;

	/* Temp switch board for transfer mem context with user */	
	zicio_switch_board *switch_board;

#ifdef CONFIG_ZICIO_STAT
	/* data ingestion stat */
	zicio_stat_board *stat_board;
#endif /* CONFIG_ZICIO_STAT */

	/* request meta-data in data buffer */
	unsigned long meta_requested;

	/* zicio buffer kernel address */
	zicio_buffer buffers;

	/* user mapping info */
	zicio_user_maps user_map;

	/* device mapping info */
	zicio_dev_maps dev_maps;

	/* files info to read */
	zicio_read_files read_files;

	/* read for metadata */
	zicio_metadata_ctrl metadata_ctrl;

	/* TODO: Information on the logical block read to make the request and
		variables for displaying the shared data are needed */
	zicio_firehose_ctrl firehose_ctrl;

	zicio_global_shared_pool_desc* zicio_shared_pool_desc;

	unsigned int cpu_id;

	/* Flags about the channel operation */
	int channel_operation_flags;

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	/* order of attachment to shared pool */
	int channel_index;

	int derail_try_count;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
} zicio_descriptor;

/*
 * Reference to: struct fdtable
 * (include/linux/fdtable.h)
 */
typedef struct zicio_id_table {
	unsigned int max_ids;
	void __rcu **zicio_struct; /* current zicio struct array */
	unsigned long *close_on_exec;
	unsigned long *open_ids;
	unsigned long *full_ids_bits;

	struct rcu_head rcu;
} zicio_id_table;

/*
 * Reference to: struct files_struct
 * (include/linux/fdtable.h)
 */
typedef struct zicio_id_allocator {
	/*
	 * read mostly part
	 */
	atomic_t count;
	bool resize_in_progress;
	wait_queue_head_t resize_wait;

	zicio_id_table __rcu *id_table;
	zicio_id_table id_table_init;

	/*
	 * written part on a separate cache line in SMP
	 */
	spinlock_t zicio_id_allocator_lock ____cacheline_aligned_in_smp;
	unsigned int next_id;
	unsigned long close_on_exec_init[1];
	unsigned long open_ids_init[1];
	unsigned long full_ids_bits_init[1];
	void __rcu *zicio_struct_array[NR_ZICIO_OPEN_DEFAULT];
} zicio_id_allocator;

/*
 * zicio_iomap_dio
 *
 * This is a zicio direct i/o structure. Currently, the contents are the
 * same as the iomap_dio structure.
 */
struct zicio_iomap_dio {
	struct kiocb		*iocb;
	const struct iomap_dio_ops *dops;
	loff_t			i_size;
	loff_t			size;
	atomic_t		ref;
	unsigned		flags;
	int			error;
	bool			wait_for_completion;

	union {
		/* used during submission and for synchronous completion: */
		struct {
			struct iov_iter		*iter;
			struct task_struct	*waiter;
			struct request_queue	*last_queue;
			blk_qc_t		cookie;
		} submit;

		/* used for aio completion: */
		struct {
			struct work_struct	work;
		} aio;
	};
};

/*
 * zicio_get_shared_pool
 *
 * If the channel is attached to the shared pool and operates, the shared pool
 * singleton is returned.
 */
static inline void*
zicio_get_shared_pool(zicio_descriptor *desc)
{
	BUG_ON(!desc);
	if (desc->zicio_shared_pool_desc) {
		return desc->zicio_shared_pool_desc->zicio_shared_pool;
	}
	BUG_ON(true);
	return NULL;
}

/*
 * zicio_get_shared_pool_local
 *
 * If the channel is attached to the shared pool and operates, the shared pool
 * local singleton is returned.
 */
static inline void*
zicio_get_shared_pool_local(zicio_descriptor *desc)
{
	BUG_ON(!desc);
	if (desc->zicio_shared_pool_desc) {
		return desc->zicio_shared_pool_desc->zicio_shared_pool_local;
	}
	BUG_ON(true);
	return NULL;
}

/*
 * zicio_id_node
 *
 * It is a node attached to the task struct and is used to identify which
 * channels are currently created and managed in the process.
 */
typedef struct zicio_id_node {
	struct zicio_id_node *id_next; /* id node queue link */
	unsigned int id; /* node id */
} zicio_id_node;

/*
 * zicio_id_list_empty
 *
 * Check if the id list managed in the channel is empty.
 */
static inline int
zicio_id_list_empty(const struct zicio_id_list *sfl)
{
	return sfl->head == NULL;
}

/*
 * zicio_id_list_init
 *
 * Used to initialize the list when creating a process.
 */
static inline void
zicio_id_list_init(struct zicio_id_list *sfl)
{
	sfl->head = sfl->tail = NULL;
}

/*
 * zicio_id_list_for_each
 *
 * It is used to traverse the channels attached to the process.
 */
#define zicio_id_list_for_each(zicio_node, sfl) \
	for (zicio_node = (sfl)->head; zicio_node; zicio_node = zicio_node->id_next)

/*
 * zicio_id_list_size
 *
 * It is used to find the pending list in the list.
 */
static inline unsigned
zicio_id_list_size(const struct zicio_id_list *sfl)
{
	unsigned sz = 0;
	struct zicio_id_node *id;

	zicio_id_list_for_each(id, sfl) {
		sz++;
	}

	return sz;
}

/*
 * zicio_id_list_add
 *
 * Used to append a new id list node to the tail of the list.
 */
static inline void
zicio_id_list_add(struct zicio_id_list *sfl,
			struct zicio_id_node *id)
{
	id->id_next = NULL;

	if (sfl->tail) {
		sfl->tail->id_next = id;
	} else {
		sfl->head = id;
	}

	sfl->tail = id;
}

/*
 * zicio_id_list_add_head
 *
 * Used to add a new id list node to the head of the list.
 */
static inline void
zicio_id_list_add_head(struct zicio_id_list *sfl,
			struct zicio_id_node *id)
{
	id->id_next = sfl->head;

	sfl->head = id;

	if (!sfl->tail)
		sfl->tail = id;
}

/*
 * zicio_id_list_peek
 *
 * Read head of id list
 */
static inline struct zicio_id_node *
zicio_id_list_peek(struct zicio_id_list *sfl)
{
	return sfl->head;
}

/*
 * zicio_id_list_pop
 *
 * Use to pop the head of a list out of the list.
 */
static inline struct zicio_id_node *
zicio_id_list_pop(struct zicio_id_list *sfl)
{
	struct zicio_id_node *id = sfl->head;

	if (id) {
		sfl->head = sfl->head->id_next;
		if (!sfl->head)
			sfl->tail = NULL;

		id->id_next = NULL;
	}

	return id;
}

/*
 * zicio_id_list_pick
 *
 * It is used to extract a node with a specific id value from a list.
 */
static inline
struct zicio_id_node *zicio_id_list_pick(struct zicio_id_list *sfl,
			int id)
{
	struct zicio_id_node *id_node, *prev_node = NULL;

	zicio_id_list_for_each(id_node, sfl) {
		if (id_node->id == id) {
			break;
		}
		prev_node = id_node;
	}

	if (!id_node) {
		return NULL;
	}

	if (prev_node) {
		prev_node->id_next = id_node->id_next;
	}
	if (sfl->head == id_node) {
		sfl->head = id_node->id_next;	
	}

	if (sfl->tail == id_node) {
		sfl->tail = prev_node;
	}

	return id_node;
}

void zicio_init_device_mgmt(void);
void zicio_init_with_device_number(void);
void zicio_init(void);
void zicio_init_slab_caches(void);
zicio_descriptor *
zicio_allocate_and_initialize_mem(struct device **devices,
		zicio_device **zicio_devs, struct zicio_args *zicio_args);
unsigned long zicio_mmap_buffers(zicio_descriptor *desc, int stflg,
		unsigned long user_base_address);

extern int zicio_check_vm_mapped(struct mm_struct *mm, unsigned long start,
				unsigned long len, struct vm_area_struct **pprev,
				struct rb_node ***link, struct rb_node **parent);
extern unsigned long zicio_mmap_region_buffers(void * buffer_vm, 
				unsigned long addr, unsigned long len, vm_flags_t vm_flags, 
				struct list_head *uf, unsigned long pgoff);
extern int zicio_munmap_buffers(zicio_descriptor *sd);
extern int zicio_munmap_buffer(unsigned long addr, size_t len);
extern long zicio_free_buffers(zicio_descriptor *buffer_meta);
extern zicio_nvme_cmd_list **
zicio_create_command(zicio_descriptor *desc, int local_page_idx,
			int *num_dev, bool create_metadata_cmd);
extern zicio_nvme_cmd_list **
zicio_create_command_shared(zicio_descriptor *desc, int local_page_idx,
		int *num_dev, bool is_on_track);
extern void zicio_set_one_command_metadata(zicio_descriptor *desc,
			zicio_nvme_cmd_list *cmds, struct nvme_command *old_cmd,
			bool sgl_supported);
extern void zicio_dump_nvme_cmd(struct nvme_command *cmds);
extern long zicio_close_id(unsigned int id, bool from_doexit);
extern void zicio_free_nvme_cmd_list(void *cmd);
extern void zicio_task_exit(struct task_struct *exit_task);
extern void zicio_complete_command(void *req, u32 queue_depth);
extern void zicio_free_dio(void *dio);
extern int zicio_do_softtimer_idle(int cpu, int global_device_idx);
extern int zicio_do_softtimer_softirq(int cpu, int global_device_idx);
extern void zicio_trigger_softtimer_timer_softirq(int cpu, int dev_idx,
			unsigned long interval);
extern void zicio_set_dma_mapping_to_command(zicio_descriptor *desc,
			zicio_nvme_cmd_list * nvme_cmds, int cmd_flag);
extern int zicio_descriptor_close(
			zicio_descriptor *sd, bool from_doexit);
extern void zicio_produced_data_chunk_to_shared_pool(unsigned int cpu_id);
extern void zicio_set_pci_driver(void *zicio_pci_driver, int dev_idx);
extern void zicio_set_md_device(void *mddev);
extern void zicio_do_softtimer_jobs(int cpu);
extern void zicio_set_bio_vector(struct bio *bio,
			zicio_descriptor *desc, zicio_nvme_cmd_list *cmd_list);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
#define PRINT_ZICIO_DEBUG(cpu_id, task_id, file, line, func) do { \
		printk(KERN_WARNING "cpu_id[%u][task %d]: [%s:%d] [%s]\n", \
				cpu_id, task_id, file, line, func); \
		} while (0)
#else
#define PRINT_ZICIO_DEBUG(cpu_id, task_id, file, line, func) do { \
		} while (0)
#endif

#else /* CONFIG_ZICIO */

static inline void zicio_init(void) { }

static inline void zicio_notify_kernel_init(void) { }

#endif /* CONFIG_ZICIO */

#endif /* _LINUX_ZICIO_H */
