#ifndef __ZICIO_EXTENT_H
#define __ZICIO_EXTENT_H

#include <linux/buffer_head.h>
#include <linux/types.h>
#include <linux/zicio_notify.h>

struct ext4_inode_info;

#define ZICIO_EXT4_EX_NOCACHE			0x40000000
#define ZICIO_EXT4_EX_FORCE_CACHE		0x20000000
#define ZICIO_EXT4_EX_NOFAIL			0x10000000

#define ZICIO_EXT4_MAX_LOGICAL_BLOCK			0xFFFFFFFE
#define ZICIO_EXT_MAX_BLOCKS			0xFFFFFFFF
#define ZICIO_EXT_INIT_MAX_LEN			(1UL << 15)
#define ZICIO_CHANNEL_INIT_FILE_NUM		2

#define ZICIO_EXT4_MAP_MAPPED		BIT(BH_Mapped)
#define ZICIO_EXT4_INODE_EXTENTS	19
#define ZICIO_EXT4_PAGE_SHIFT		12
#define ZICIO_INODE_BUFFER_BITS ((1UL) << (ZICIO_CHUNK_SHIFT - \
			ZICIO_EXT4_PAGE_SHIFT))
#define ZICIO_INODE_BUFFER_BITS_MASK (~(ZICIO_INODE_BUFFER_BITS - 1))
#define ZICIO_MOVE_CURSOR_FORWARD 1
#define ZICIO_MOVE_CURSOR_BACKWARD -1

typedef unsigned zicio_ext4_lblk_t;
typedef unsigned long long zicio_ext4_fsblk_t;
typedef struct zicio_shared_pool zicio_shared_pool;

/*
 * This is the extent on-disk structure.
 * It's used at the bottom of the tree.
 */
struct zicio_ext4_extent {
	__le32	ee_block;	/* first logical block extent covers */
	__le16	ee_len;		/* number of blocks covered by extent */
	__le16	ee_start_hi;	/* high 16 bits of physical block */
	__le32	ee_start_lo;	/* low 32 bits of physical block */
};

struct zicio_ext4_extent_idx {
	__le32	ei_block;	/* index covers logical blocks from 'block' */
	__le32	ei_leaf_lo;	/* pointer to the physical block of the next *
				 * level. leaf or next index could be there */
	__le16	ei_leaf_hi;	/* high 16 bits of physical block */
	__u16	ei_unused;
};

#define ZICIO_NUM_EXTENT_IN_PAGE (ZICIO_PAGE_SIZE / \
			sizeof(struct zicio_ext4_extent_idx))

#define ZICIO_MAX_EXTENT_IN_BUFFER (ZICIO_NUM_EXTENT_IN_PAGE * \
			ZICIO_METADATABUFFER_PAGENUM)

/*
 * Each block (leaves and indexes), even inode-stored has header.
 */
struct zicio_ext4_extent_header {
	__le16	eh_magic;	/* probably will support different formats */
	__le16	eh_entries;	/* number of valid entries */
	__le16	eh_max;		/* capacity of store in entries */
	__le16	eh_depth;	/* has tree real underlying blocks? */
	__le32	eh_generation;	/* generation of the tree */
};

/*
 * Array of ext4_ext_path contains path to some extent.
 * Creation/lookup routines use it for traversal/splitting/etc.
 * Truncate uses it to simulate recursive walking.
 */
struct zicio_ext4_ext_path {
	zicio_ext4_fsblk_t			p_block;
	__u16				p_depth;
	__u16				p_maxdepth;
	struct zicio_ext4_extent		*p_ext;
	struct zicio_ext4_extent_idx		*p_idx;
	struct zicio_ext4_extent_header	*p_hdr;
	struct buffer_head		*p_bh;
};

/*
 * Logical to physical block mapping, used by zicio block mapping
 */
struct zicio_ext4_map_blocks {
	zicio_ext4_fsblk_t m_pblk;
	zicio_ext4_lblk_t m_lblk;
	unsigned int m_len;
	unsigned int m_flags;
};

#define ZICIO_EXT_FIRST_INDEX(__hdr__) \
	((struct zicio_ext4_extent_idx *) (((char *) (__hdr__)) +	\
				     sizeof(struct zicio_ext4_extent_header)))
#define ZICIO_EXT_FIRST_EXTENT(__hdr__) \
	((struct zicio_ext4_extent *) (((char *) (__hdr__)) +	\
					sizeof(struct zicio_ext4_extent_header)))
extern unsigned long
zicio_initialize_extent_buffers(
			zicio_file_struct *zicio_obj, struct file *file);
extern struct zicio_extent_idx *
zicio_get_extent_idx_in_buffer(char * buffer, unsigned page_num,
			unsigned page_off);
extern unsigned long
zicio_feed_metadata_buffers(zicio_descriptor *desc,
			zicio_file_struct *zicio_obj);
extern long zicio_initialize_extent_and_metadata(zicio_descriptor *desc,
		struct fd* fs, int nr_fd);
extern long zicio_initialize_extent_and_metadata_shared(
		zicio_shared_pool *zicio_shared_pool, struct fd* fs, int nr_fd);
extern int zicio_produce_metadata(zicio_descriptor* sd);
struct zicio_ext4_extent * zicio_get_extent_in_buffer(
		char * buffer, unsigned page_num, unsigned page_off);

/*
 * zicio_get_inode_from_file
 *
 * get inode from file pointer
 */
static inline struct inode*
zicio_get_inode_from_file(struct file *file)
{
	return file->f_inode;
}

/* metadata buffer size is 2 MiB */
#define ZICIO_METADATA_BUF_SIZE (2 * 1024 * 1024)
#define ZICIO_METADATA_BUF_PAGE_NUM \
			(ZICIO_METADATA_BUF_SIZE >> ZICIO_PAGE_SHIFT)
#define ZICIO_METADATA_PAGE_NUM_MASK \
			(~(ZICIO_METADATA_BUF_PAGE_NUM - 1))
#define ZICIO_EXTENT_NUM_IN_PAGE (ZICIO_PAGE_SIZE / \
			sizeof(struct zicio_ext4_extent))
#define ZICIO_PAGE_NUM_IN_METADATA_BUF \
			(ZICIO_METADATA_BUF_SIZE / ZICIO_PAGE_SIZE)

/* the max number of extent */
#define ZICIO_METADATA_BUF_MAX \
			(ZICIO_EXTENT_NUM_IN_PAGE * ZICIO_PAGE_NUM_IN_METADATA_BUF)
#define ZICIO_METADATA_BUF_GET_EXTENT(buf, idx) \
						((struct zicio_ext4_extent *) ((char *) buf + \
							ZICIO_PAGE_SIZE * \
							(idx / ZICIO_EXTENT_NUM_IN_PAGE) + \
							sizeof(struct zicio_ext4_extent) * \
							(idx % ZICIO_EXTENT_NUM_IN_PAGE)))

/* inode buffer size is 2 MiB */
#define ZICIO_INODE_BUF_SIZE (2 * 1024 * 1024)
/* the number of raw inode */
#define ZICIO_INODE_BUF_MAX (ZICIO_INODE_BUF_SIZE / ZICIO_PAGE_SIZE)

/* get block from inode buffer  */
#define ZICIO_INODE_BUF_GET_BLOCK(buf, idx) \
			((void *) ((char *) buf + ZICIO_PAGE_SIZE * idx))
/* get extent header from block */
#define ZICIO_BLOCK_GET_EXTENT_HEADER(block) \
			((struct zicio_ext4_extent_header *)block)
/* get first extent from block */
#define ZICIO_BLOCK_GET_FIRST_EXTENT(block) \
			((struct zicio_ext4_extent *) ((char *) block + \
						sizeof(struct zicio_ext4_extent_header)))

static inline void *
zicio_get_metadata_buffer(zicio_descriptor* sd)
{
	return (void *) sd->buffers.metadata_buffer;
}

static inline void *
zicio_get_inode_buffer(zicio_descriptor* sd)
{
	return (void *) sd->metadata_ctrl.inode_buffer;
}

/* Turn on a bit of inode buffer */
static inline void
zicio_set_inode_bitmap(zicio_chunk_bitmap_meter *inode_meter,
		unsigned long processed)
{
	unsigned long loc_bit = processed & ~ZICIO_INODE_BUFFER_BITS_MASK;
	unsigned long in_bitmap_offset = loc_bit >>
				ZICIO_INTERNAL_BITVECTOR_ORDER;
	unsigned long in_byte_offset = loc_bit & ~ZICIO_INTERNAL_BITVECTOR_MASK;
	unsigned long *bit_sector = &inode_meter->produced[in_bitmap_offset];
	unsigned long bit = (1UL) << in_byte_offset;

	*bit_sector |= bit;
}

/* Turn off a bit of inode buffer bitmap */
static inline void
zicio_clear_inode_bitmap(zicio_chunk_bitmap_meter *inode_meter,
		unsigned long processed)
{
	unsigned long loc_bit = processed & ~ZICIO_INODE_BUFFER_BITS_MASK;
	unsigned long in_bitmap_offset = loc_bit >>
				ZICIO_INTERNAL_BITVECTOR_ORDER;
	unsigned long in_byte_offset = loc_bit & ~ZICIO_INTERNAL_BITVECTOR_MASK;
	unsigned long *bit_sector = &inode_meter->produced[in_bitmap_offset];
	unsigned long bit = (1UL) << in_byte_offset;

	*bit_sector &= ~bit;
}

/* Get the number of produced bit of inode bitmap */
static inline unsigned long
zicio_get_produced_inode_bitmap(zicio_chunk_bitmap_meter *inode_meter,
		unsigned long consumed)
{
	unsigned long i = consumed;
	unsigned long loc_bit;
	unsigned long in_bitmap_offset;
	unsigned long in_byte_offset;
	unsigned long bit;

	do {
		loc_bit = i & ~ZICIO_INODE_BUFFER_BITS_MASK;
		in_bitmap_offset = loc_bit >> ZICIO_INTERNAL_BITVECTOR_ORDER;
		in_byte_offset = loc_bit & ~ZICIO_INTERNAL_BITVECTOR_MASK;
		bit = (1UL) << in_byte_offset;
		i++;
	} while (inode_meter->produced[in_bitmap_offset] & bit);

	return i - 1;
}

/* Get the extent header from inode */
static inline struct zicio_ext4_extent_header *
zicio_get_ext_inode_hdr(struct inode *inode)
{
	return (struct zicio_ext4_extent_header *)
				(__zicio_ext_inode_hdr(inode));
}

/* Get the extent header from extent block */
static inline struct zicio_ext4_extent_header *
zicio_get_ext_block_hdr(struct buffer_head *bh)
{
	return (struct zicio_ext4_extent_header *) bh->b_data;
}

/* Get the depth of extent tree */
static inline unsigned short
zicio_ext_depth(struct inode *inode)
{
	return le16_to_cpu(zicio_get_ext_inode_hdr(inode)->eh_depth);
}

/* Get index extent physical block */
static inline zicio_ext4_fsblk_t
zicio_get_ext_idx_pblock(struct zicio_ext4_extent_idx *ix)
{
	zicio_ext4_fsblk_t block;

	block = le32_to_cpu(ix->ei_leaf_lo);
	block |= ((zicio_ext4_fsblk_t) le16_to_cpu(ix->ei_leaf_hi) << 31) << 1;
	return block;
}

/*
 * zicio_check_ext_current_metadata
 *
 * Checking if current metadata pointer points to relevant data
 */
static inline int
zicio_check_ext_current_metadata(struct zicio_ext4_extent *ext,
			zicio_ext4_lblk_t cur_lblock)
{
	zicio_ext4_lblk_t meta_lblock;
	unsigned int meta_len;

	meta_lblock = le32_to_cpu(ext->ee_block);
	meta_len = le16_to_cpu(ext->ee_len);

	if ((meta_lblock <= cur_lblock) &&
				(meta_lblock + meta_len > cur_lblock)) {
		return 0;
	} else if (meta_lblock > cur_lblock) {
		return ZICIO_MOVE_CURSOR_BACKWARD;
	} else {
		return ZICIO_MOVE_CURSOR_FORWARD;
	}
}

#define zicio_decode_index_extent(index_extent, cur_lba, found_lblock)	\
({																			\
	cur_lba = le32_to_cpu(index_extent_to_read->ei_leaf_lo);				\
	cur_lba |= ((zicio_ext4_fsblk_t)le16_to_cpu(						\
			index_extent->ei_leaf_hi) << 31) << 1;							\
	le32_to_cpu(index_extent->ei_block);									\
})
#endif /* ZICIO_EXTENT_H */
