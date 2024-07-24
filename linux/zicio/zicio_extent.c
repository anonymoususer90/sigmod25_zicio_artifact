#include <linux/cpumask.h>
#include <linux/types.h>

#include <linux/buffer_head.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_mem.h"

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
/*
 * Debug function for dumping metadata buffer
 */
void
zicio_dump_metadata_buffer(zicio_meter *meter, void *meta_buf)
{
	unsigned i, prod = meter->produced_no_mod;
	struct zicio_ext4_extent *current_extent;
	unsigned page_mod, offset;
	zicio_ext4_fsblk_t pblk;
	zicio_ext4_lblk_t lblk;
	int len;
	
	for (i = 0 ; i < prod ; i++) {
		page_mod = i / ZICIO_NUM_EXTENT_IN_PAGE;
		page_mod &= ~(ZICIO_METADATA_PAGE_NUM_MASK);
		offset = i % ZICIO_NUM_EXTENT_IN_PAGE;
		current_extent = (struct zicio_ext4_extent *)
					((char*)meta_buf + page_mod * 4096 +
					offset * sizeof(struct zicio_ext4_extent));

		pblk = le32_to_cpu(current_extent->ee_start_lo);
		pblk |= ((zicio_ext4_fsblk_t)
					le16_to_cpu(current_extent->ee_start_hi) << 31) << 1;
		lblk = le32_to_cpu(current_extent->ee_block);
		len = le16_to_cpu(current_extent->ee_len);
		printk(KERN_WARNING "[Kernel Message] %u[%u] : %llu~>%d\n",
					lblk, i, pblk, len);
	}
}

void
zicio_dump_index_extent_block(
			struct zicio_ext4_extent_header *extent_header)
{
	short int num_entries = le16_to_cpu(extent_header->eh_entries);
	struct zicio_ext4_extent_idx *current_idx_extent =
				(struct zicio_ext4_extent_idx *)(extent_header + 1);
	int i;
	zicio_ext4_fsblk_t pblk;
	zicio_ext4_lblk_t lblk;

	for (i = 0 ; i < num_entries ; i++, current_idx_extent++) {
		pblk = le32_to_cpu(current_idx_extent->ei_leaf_lo);
		pblk |= ((zicio_ext4_fsblk_t)
					le16_to_cpu(current_idx_extent->ei_leaf_hi) << 31) << 1;
		lblk = le32_to_cpu(current_idx_extent->ei_block);
		printk(KERN_WARNING "[Kernel Message] [%u->%llu]\n",
					lblk, pblk);
	}
}
#endif

/*
 * zicio_get_extent_in_buffer
 *
 * Get the extent from buffer
 */
struct zicio_ext4_extent *
zicio_get_extent_in_buffer(char * buffer, unsigned page_num,
			unsigned page_off)
{
	return (struct zicio_ext4_extent*)zicio_get_buffer(buffer, page_num,
				page_off, sizeof(struct zicio_ext4_extent));
}

/*
 * zicio_search_first_ext
 *
 * Get first ext4 of its depth
 */
static struct zicio_ext4_extent_idx *
zicio_search_first_ext(struct inode *inode,
			struct zicio_ext4_ext_path *path)
{
	struct zicio_ext4_extent_header *extent_header = path->p_hdr;
	struct zicio_ext4_extent_idx *first =
				ZICIO_EXT_FIRST_INDEX(extent_header);

	path->p_idx = first;

	return first;
}

/*
 * zicio_copy_index_extent_to_buffer
 *
 * Copy index extent to zicio extent tree buffer
 * It is a recursive function and copies the idx extent of the extent tree whose
 * level is 1 to the extent buffer while traversing the extent tree.
 */
unsigned long
zicio_copy_index_extent_to_buffer(
			struct inode *inode,
			struct zicio_ext4_ext_path *path,
			struct zicio_ext4_extent_header *extent_header, short int ppos,
			short int depth, void *extent_tree_buffer, unsigned long cur_cnt)
{
	struct buffer_head *bh;
	struct zicio_ext4_extent_header *cur_extent_header;
	struct zicio_ext4_extent_idx *first, *current_idx_extent;
	short int idx;
	unsigned long cnt = 0;
	unsigned long ret;
	short int num_entries;
	char *dest_ptr;

	if (unlikely(!path)) {
		return -ENOMEM;
	}

	/* Get the first extent in block */
	if (!(first = zicio_search_first_ext(inode, &path[ppos]))) {
		printk(KERN_WARNING "[Kernel Message] Cannot get first index extent\n");
		return -ENOMEM;
	}

	/* depth is 1, copy inode's idx extent to extent tree buffer */
	if (depth == 1) {
		num_entries = le16_to_cpu(extent_header->eh_entries);

		dest_ptr = (char*)extent_tree_buffer + 
					cur_cnt * sizeof(struct zicio_ext4_extent_idx);
		memcpy(dest_ptr, first, sizeof(struct zicio_ext4_extent_idx)
					* num_entries);

		return num_entries;
	}

	/*
	 * If the current depth is greater than 1, tree traversing is performed in
	 * the for statement below. Get the physical block number from the index
	 * extent in the tree and use it to read the descendant extent tree block.
	 */
	for (idx = 0, current_idx_extent = first ; idx < extent_header->eh_entries ;
					idx++, current_idx_extent++) {
		path[ppos].p_idx = current_idx_extent;
		path[ppos].p_block = zicio_get_ext_idx_pblock(path[ppos].p_idx);
		path[ppos].p_depth = depth;
		path[ppos].p_ext = NULL;

		bh = zicio_read_extent_tree_block(inode, path[ppos].p_block,
					depth - 1, ZICIO_EXT4_EX_FORCE_CACHE);

		if (IS_ERR(bh)) {
			printk(KERN_WARNING "Cannot get idx extent\n");
			ret = PTR_ERR(bh);
			return ret;
		}

		cur_extent_header = zicio_get_ext_block_hdr(bh);

		path[ppos + 1].p_bh = bh;
		path[ppos + 1].p_hdr = cur_extent_header;

		/* Traversing extent tree to get depth=1 idx node */
		if ((ret = zicio_copy_index_extent_to_buffer(inode, path,
						cur_extent_header, ppos + 1, depth - 1,
						extent_tree_buffer, cur_cnt + cnt)) < 0) {
			if (!ppos) {
				printk(KERN_WARNING "Cannot copy size of extent tree "
									"buffer\n");
				zicio_ext4_drop_refs(path);
				kfree(path);
			}
			return ret;
		}
		cnt += ret;
	}
	return cnt;
}

/*
 * zicio_get_max_idx_extent
 *
 * Gets the maximum number of index extents that can exist in the tree.
 */
static unsigned long
zicio_get_max_idx_extent(short int depth, short int entries)
{
	unsigned long num_idx = ZICIO_NUM_EXTENT_IN_PAGE;

	while (depth > 1) {
		num_idx *= ZICIO_NUM_EXTENT_IN_PAGE;
		depth--;
	}
	
	return num_idx * entries;
}

/*
 * zicio_initialize_extent_buffers
 *
 * Initialize extent buffers and return how many entries are loaded
 */
unsigned long
zicio_initialize_extent_buffers(zicio_file_struct *zicio_file,
			struct file * file)
{
	struct inode *file_inode = zicio_get_inode_from_file(file);
	struct zicio_ext4_ext_path *path;
	struct zicio_ext4_extent_header *extent_header;
	struct zicio_ext4_extent *first;
	gfp_t gfp_flags = GFP_NOFS;
	short int depth, ppos = 0;
	unsigned long index_ext_cnt = 0;
	unsigned long exp_index_ext_cnt;
	unsigned num_entries;
	void *extent_tree_buffer;

	/* Reading inode, access shared lock */
	inode_lock_shared(file_inode);
 
	/* Initialization for idx node traversing */
	extent_header = zicio_get_ext_inode_hdr(file_inode);

	/*  Get the number of entries from inode */
	if (!(num_entries = le16_to_cpu(extent_header->eh_entries))) {
		inode_unlock_shared(file_inode);
		return 0;
	}

	/* Get the current max level */
	depth = le16_to_cpu(extent_header->eh_depth);

	/* Check if depth is 0 or not, then just reading extents to metadata 
	 * buffer */
	if (!depth) {
		/*
		 * Extents are directly contained in inodes. It is only copied to the
		 * extent buffer, and cached in the extent tree buffer to retrieve it
		 * immediately without traversing the tree later.
		 */
		zicio_file->has_index_extent = false;
		zicio_file->extent_tree_buffer = kmalloc(num_entries *
					sizeof(struct zicio_ext4_extent_idx), GFP_KERNEL);

		if (unlikely(!(zicio_file->extent_tree_buffer))) {
			inode_unlock_shared(file_inode);
			printk(KERN_WARNING "[Kernel Message] Cannot create extent tree "
							"buffer\n");
			return -ENOMEM;
		}

		first = ZICIO_EXT_FIRST_EXTENT(extent_header);
		memcpy(zicio_file->extent_tree_buffer, first,
					sizeof(struct zicio_ext4_extent) * num_entries);

		inode_unlock_shared(file_inode);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "[Kernel Message] depth is 0. Set to metadata "
					"buffer only\n");
#endif
		/* Return the copied number of extent(Non-index) */
		return num_entries;
	} else {
		zicio_file->has_index_extent = true;
	}

	/* 
	 * Start traversing for getting level 1 index node from extent tree 
	 */
	path = kcalloc(depth + 2, sizeof(struct zicio_ext4_ext_path),
				gfp_flags);

	if (unlikely(!path)) {
		inode_unlock_shared(file_inode);
		printk(KERN_WARNING "[Kernel Message] Cannot get path for traversing "
					"extent tree");
		return -ENOMEM;
	}

	path[0].p_maxdepth = depth + 1;
	path[0].p_hdr = extent_header;
	path[0].p_bh = NULL;

	/* Calculate maximum extent size using depth */
	exp_index_ext_cnt = zicio_get_max_idx_extent(depth,
			le16_to_cpu(extent_header->eh_entries));

	/* Allocate extent tree buffer */
	extent_tree_buffer = kmalloc(exp_index_ext_cnt * 
				sizeof(struct zicio_ext4_extent_idx), GFP_KERNEL);

	if (unlikely(!(extent_tree_buffer))) {
		inode_unlock_shared(file_inode);
		kfree(path);
		printk(KERN_WARNING "[Kernel Message] Cannot create extent tree " 
					"buffer\n");
		return -ENOMEM;
	}

	/* Copy idx extent to extent tree buffer */
	index_ext_cnt = zicio_copy_index_extent_to_buffer(file_inode,
					path, extent_header, ppos, depth, extent_tree_buffer, 0);

	inode_unlock_shared(file_inode);
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] Get extent number[%d] : %ld < %ld\n",
			depth, index_ext_cnt, exp_index_ext_cnt);
#endif

	zicio_file->extent_tree_buffer = kmalloc(index_ext_cnt *
				sizeof(struct zicio_ext4_extent_idx), GFP_KERNEL);

	if (unlikely(!zicio_file->extent_tree_buffer)) {
		kfree(path);
		printk(KERN_WARNING "[Kernel Message] Cannot create extent tree " 
					"buffer\n");
		return -ENOMEM;
	}

	memcpy(zicio_file->extent_tree_buffer, extent_tree_buffer,
				sizeof(struct zicio_ext4_extent_idx) * index_ext_cnt);

	kfree(path);
	kfree(extent_tree_buffer);

	return index_ext_cnt;
}

/*
 * zicio_ext4_copy_metadata_to_buffer
 *
 * Copy extent from extent block to paginated metadata buffer
 * Before calling this function, enough space must be prepared for copying.
 */
unsigned
zicio_ext4_copy_metadata_to_buffer(char *metadata_buffer,
			struct zicio_ext4_extent *src_ext, unsigned num_copied,
			short int entries) {
	char *dest_ptr;
	unsigned num_copied1;
	unsigned num_copied2;
	unsigned copied_page_mod;
	unsigned start_idx;

	/* If there is not enough space in the buffer, num_copied is returned
	 * immediately. */
	if (num_copied + entries >= ZICIO_MAX_EXTENT_IN_BUFFER) {
		return num_copied;
	}

	/* Calculate the page to be copied and the empty space within the page
	 * using the copied extent and the number of extents to be copied */
	copied_page_mod = num_copied / ZICIO_NUM_EXTENT_IN_PAGE;
	copied_page_mod &= ~(ZICIO_METADATA_PAGE_NUM_MASK);
	start_idx = num_copied % ZICIO_NUM_EXTENT_IN_PAGE;
	num_copied1 = ZICIO_NUM_EXTENT_IN_PAGE - start_idx;

	if (num_copied1 > (unsigned)entries) {
		num_copied1 = (unsigned)entries;
		num_copied2 = 0;
	} else {
		num_copied2 = (unsigned)entries - num_copied1;
	}

	/* Copy is performed using the above values. */
	dest_ptr = metadata_buffer + copied_page_mod * ZICIO_PAGE_SIZE +
				start_idx * sizeof(struct zicio_ext4_extent);
	memcpy(dest_ptr, src_ext, sizeof(struct zicio_ext4_extent) *
				num_copied1);

	if (!num_copied2) {
		return num_copied + num_copied1;
	}

	/* If the current page is used up, copying starts on the next page. */
	copied_page_mod = (copied_page_mod + 1) &
			~(ZICIO_METADATA_PAGE_NUM_MASK);
	dest_ptr = metadata_buffer + copied_page_mod * ZICIO_PAGE_SIZE;
	memcpy(dest_ptr, src_ext + num_copied1,
				sizeof(struct zicio_ext4_extent) * num_copied2);

	return num_copied + num_copied2 + num_copied1;
}

/*
 * zicio_feed_metadata_buffer
 *
 * Feed metadata buffer for depth 0 extent tree(when all extents exist in 
 * inode).
 */
unsigned long
zicio_feed_metadata_buffers(zicio_descriptor *desc,
			zicio_file_struct *zicio_file)
{
	struct zicio_ext4_extent *start_point_to_fetch;

	char *metadata_buffer = desc->buffers.metadata_buffer;
	unsigned consumed_no_mod, produced_no_mod;
	unsigned consumed_ppage_mod, consumed_poff_mod;
	unsigned copied = 0;
	unsigned rest_in_buffer;
	unsigned max_ext;
	unsigned num_copy;
	unsigned maximum_copy_amount;

	/*
	 * This function should only be called in situations where the index extent
	 * is not cached.
	 */ 
	BUG_ON(zicio_file->has_index_extent);

	/* First, checking whether the current buffer is exhausted. */
	if (zicio_file->extent_tree_meter.produced_no_mod ==
				zicio_file->extent_tree_meter.consumed_no_mod) {
		return 0;
	}


	consumed_no_mod = desc->metadata_ctrl.metadata_meter.consumed_no_mod;
	produced_no_mod = desc->metadata_ctrl.metadata_meter.produced_no_mod;
	rest_in_buffer = ZICIO_MAX_EXTENT_IN_BUFFER -
			produced_no_mod + consumed_no_mod;
	consumed_ppage_mod = produced_no_mod / ZICIO_NUM_EXTENT_IN_PAGE;
	consumed_ppage_mod &= ~(ZICIO_METADATA_PAGE_NUM_MASK);
	consumed_poff_mod = produced_no_mod % ZICIO_NUM_EXTENT_IN_PAGE;

	/* If file size is 0 */
	if (zicio_file->file_size == 0) {
		printk(KERN_WARNING "[Kernel Message] file size 0\n");
		return 0;
	}

	/* Get the start point to fetch extent */
	start_point_to_fetch =
				(struct zicio_ext4_extent *)zicio_file->extent_tree_buffer +
				zicio_file->extent_tree_meter.consumed_no_mod;

	/* Metadata feeding MUST be called, when number of entries is less then 
	 * the unreserved number of metadata buffer */
	max_ext = zicio_file->extent_tree_meter.produced_no_mod -
				zicio_file->extent_tree_meter.consumed_no_mod;

	/* Calculate where to get the extent */
	metadata_buffer = (char*)zicio_get_extent_in_buffer(metadata_buffer,
				consumed_ppage_mod, consumed_poff_mod);

	/* Set considering the amount already supplied. */
	maximum_copy_amount = ZICIO_NUM_EXTENT_IN_PAGE - consumed_poff_mod;

	/* copy extents from inode */
	while (copied < max_ext) {
		num_copy = min_t(unsigned, maximum_copy_amount, max_ext - copied);

		/* Checking metadata buffer's capability */
		if (num_copy > rest_in_buffer) {
			copied += rest_in_buffer;
			memcpy(metadata_buffer, start_point_to_fetch,
					sizeof(struct zicio_ext4_extent) * rest_in_buffer);
			break;
		}

		/* Copying the extent */
		memcpy(metadata_buffer, start_point_to_fetch,
					sizeof(struct zicio_ext4_extent) * num_copy);

		/* Exhausted the current data page. increase the page */		
		consumed_ppage_mod++;
		consumed_ppage_mod &= ~(ZICIO_METADATA_PAGE_NUM_MASK);
		metadata_buffer = (char*)desc->buffers.metadata_buffer +
				(consumed_ppage_mod << ZICIO_PAGE_SHIFT);

		maximum_copy_amount = ZICIO_NUM_EXTENT_IN_PAGE;

		start_point_to_fetch += num_copy;
		copied += num_copy;
		rest_in_buffer -= num_copy;
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] copied : %u\n", copied);
#endif

	/* Add all copied amount to meter */
	desc->metadata_ctrl.metadata_meter.produced_no_mod += copied;
	zicio_file->extent_tree_meter.consumed_no_mod += copied;

	return copied;
}

/*
 * zicio_ext4_simple_copy_extents
 *
 * When file's extent tree depth is 0, then just copy extents
 */
unsigned long
zicio_ext4_simple_copy_extents(zicio_descriptor *desc,
			char *metadata_buffer, zicio_file_struct *zicio_file,
			zicio_meter *metadata_meter)
{
	unsigned max_ext, i, num_copy, copied = 0;
	struct zicio_ext4_extent *current_extent;

	/* If file size is 0 */
	if (zicio_file->extent_tree_meter.produced_no_mod == 0) {
		zicio_file->extent_tree_meter.consumed_no_mod = 1;
		zicio_file->extent_tree_meter.produced_no_mod = 1;

		printk(KERN_WARNING "[Kernel Message] file size 0\n");
		return 0;
	}

	max_ext = (zicio_file->extent_tree_meter.produced_no_mod <
						ZICIO_MAX_EXTENT_IN_BUFFER) ?
								zicio_file->extent_tree_meter.produced_no_mod:
								ZICIO_MAX_EXTENT_IN_BUFFER;
	num_copy = ZICIO_NUM_EXTENT_IN_PAGE;
	current_extent =
			(struct zicio_ext4_extent *)(zicio_file->extent_tree_buffer);

	/* copy extents from inode */
	for (i = 0 ; i < max_ext ; i += num_copy) {
		if (max_ext - i < ZICIO_NUM_EXTENT_IN_PAGE) {
			num_copy = max_ext - i;
		}
		memcpy(metadata_buffer, current_extent,
					sizeof(struct zicio_ext4_extent) * num_copy);

		copied += num_copy;
		metadata_buffer += 4096;
		current_extent += num_copy;
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message] initial metadata copied : %u\n",
		copied);
#endif

	zicio_file->extent_tree_meter.consumed_no_mod = copied;
	metadata_meter->produced_no_mod = copied;

	return copied;
}

/*
 * zicio_ext4_simple_copy_extents_shared
 *
 * When file's extent tree depth is 0, then just copy extents
 * Copy extents to shared metadata buffer
 */
unsigned long
zicio_ext4_simple_copy_extents_shared(zicio_shared_pool *shared_pool,
			zicio_file_struct *zicio_file, unsigned long shared_pool_copied)
{
	char *metadata_buffer;

	/* If file size is 0 */
	if (zicio_file->extent_tree_meter.produced_no_mod == 0) {
		zicio_file->extent_tree_meter.consumed_no_mod = 1;
		zicio_file->extent_tree_meter.produced_no_mod = 1;

		printk(KERN_WARNING "[Kernel Message] file size 0\n");
		return 0;
	}

	/*
	 * The number is small when only extents, not idx extents, are included in
	 * the file inode. So wrap-around of index extent buffer is not considered.
	 */
	metadata_buffer = (char *)((struct zicio_ext4_extent *)
			shared_pool->shared_metadata_ctrl.shared_metadata_buffer +
					shared_pool_copied);
			
	memcpy(metadata_buffer, zicio_file->extent_tree_buffer,
			sizeof(struct zicio_ext4_extent) *
					zicio_file->extent_tree_meter.produced_no_mod);

	zicio_file->extent_tree_meter.consumed_no_mod =
				zicio_file->extent_tree_meter.produced_no_mod;

	return zicio_file->extent_tree_meter.produced_no_mod;
}

/*
 * zicio_ext4_read_extents
 *
 * It is used to read the file's metadata into the metadata buffer during the
 * initialization phase.
 */
unsigned long
zicio_ext4_read_extents(zicio_descriptor *desc,
			char *metadata_buffer, zicio_meter *metadata_meter,
			char *inode_buffer, zicio_chunk_bitmap_meter *inode_meter,
			zicio_file_struct *zicio_file, struct inode* file_inode)
{
	struct buffer_head *bh;
	struct zicio_ext4_extent_header *extent_header;
	struct zicio_ext4_extent_idx *current_index_extent;
	zicio_ext4_fsblk_t leaf_pblk;		
	unsigned long eidx_consumed_no_mod, copied, prev_copied;

	current_index_extent =
			(struct zicio_ext4_extent_idx *)(zicio_file->extent_tree_buffer);

	copied = prev_copied = metadata_meter->produced_no_mod;
	/* Scan extent tree buffer and read leaf extent node and copy its data */
	for (eidx_consumed_no_mod = 0 ;
			eidx_consumed_no_mod < zicio_file->extent_tree_meter.produced_no_mod ;
					eidx_consumed_no_mod++, current_index_extent++) {
		leaf_pblk = le32_to_cpu(current_index_extent->ei_leaf_lo);
		leaf_pblk |= ((zicio_ext4_fsblk_t)
					le16_to_cpu(current_index_extent->ei_leaf_hi) << 31) << 1;

		/* Read extent tree */
		bh = zicio_read_extent_tree_block(file_inode, leaf_pblk, 0,
					ZICIO_EXT4_EX_FORCE_CACHE);

		if (IS_ERR(bh)) {
			printk(KERN_WARNING "Cannot get leaf extent for init\n");
			return PTR_ERR(bh);
		}

		extent_header = zicio_get_ext_block_hdr(bh);

		/* Copy extents to metadata buffer */
		copied = zicio_ext4_copy_metadata_to_buffer(metadata_buffer, 
				ZICIO_EXT_FIRST_EXTENT(extent_header), copied,
					le16_to_cpu(extent_header->eh_entries));

		metadata_meter->produced_no_mod = copied;

		if (prev_copied == copied) {
			memcpy(inode_buffer + ((inode_meter->requested_no_mod &
					~ZICIO_INODE_BUFFER_BITS_MASK) << ZICIO_PAGE_SHIFT),
						extent_header, ZICIO_PAGE_SIZE);
			zicio_set_inode_bitmap(inode_meter,
						inode_meter->requested_no_mod++);
			break;
		} else {
			prev_copied = copied;
		}
	}

	zicio_file->extent_tree_meter.consumed_no_mod = eidx_consumed_no_mod;

	return copied;
}

/*
 * zicio_ext4_read_extents_shared
 *
 * Initialize metadata buffers and return how many entries are loaded for 
 * shared pool.
 * Read the extent leaf node and copy the inner extent to the metadata buffer.
 */
unsigned long
zicio_ext4_read_extents_shared(zicio_shared_pool *shared_pool,
			zicio_file_struct *zicio_file, unsigned long shared_pool_copied,
			struct inode *file_inode)
{
	struct buffer_head *bh;
	struct zicio_ext4_extent_header *extent_header;
	struct zicio_ext4_extent_idx *current_index_extent;
	zicio_ext4_fsblk_t leaf_pblk;
	unsigned long eidx_consumed_no_mod, ext_copied = 0, copied_size;
	char *metadata_buffer;

	current_index_extent =
			(struct zicio_ext4_extent_idx *)(zicio_file->extent_tree_buffer);
	metadata_buffer = (char *)((struct zicio_ext4_extent *)
			shared_pool->shared_metadata_ctrl.shared_metadata_buffer +
					shared_pool_copied);

	/* Scan extent tree buffer and read leaf extent node and copy its data */
	for (eidx_consumed_no_mod = 0 ;
			eidx_consumed_no_mod < zicio_file->extent_tree_meter.produced_no_mod ;
					eidx_consumed_no_mod++, current_index_extent++) {
		leaf_pblk = le32_to_cpu(current_index_extent->ei_leaf_lo);
		leaf_pblk |= ((zicio_ext4_fsblk_t)
					le16_to_cpu(current_index_extent->ei_leaf_hi) << 31) << 1;

		/* Read leaf node */
		bh = zicio_read_extent_tree_block(file_inode, leaf_pblk, 0,
					ZICIO_EXT4_EX_FORCE_CACHE);

		if (IS_ERR(bh)) {
			printk(KERN_WARNING "Cannot get leaf extent for init\n");
			return PTR_ERR(bh);
		}

		extent_header = zicio_get_ext_block_hdr(bh);
		copied_size = sizeof(struct zicio_ext4_extent) *
					le16_to_cpu(extent_header->eh_entries);
		memcpy(metadata_buffer, ZICIO_EXT_FIRST_EXTENT(extent_header),
					copied_size);

		ext_copied += le16_to_cpu(extent_header->eh_entries);
		metadata_buffer += copied_size;
	}

	zicio_file->extent_tree_meter.consumed_no_mod =
			zicio_file->extent_tree_meter.produced_no_mod;

	return ext_copied;
}

/*
 * zicio_initialize_metadata_buffer
 *
 * Initialize metadata buffers and return how many entries are loaded
 * Determines the operation after checking whether the extent buffer of the file
 * has an extent or an index extent.
 */
unsigned long
zicio_initialize_metadata_buffer(zicio_descriptor *desc,
			zicio_file_struct *zicio_file_struct, struct file *file)
{
	struct inode *file_inode = zicio_get_inode_from_file(file);
	zicio_chunk_bitmap_meter *inode_meter;
	zicio_meter *metadata_meter;
	unsigned int copied = 0;

	metadata_meter = &(desc->metadata_ctrl.metadata_meter);

	/* If the target file has only extents, it simply performs a copy to the
	 * metadata buffer. */
	if (!zicio_file_struct->has_index_extent) {
		return zicio_ext4_simple_copy_extents(desc,
				desc->buffers.metadata_buffer, zicio_file_struct,
						metadata_meter);
	}

	inode_meter = &(desc->metadata_ctrl.inode_meter);

	inode_lock_shared(file_inode);

	/* If the index extent is cached, the leaf extent node is read using that
	 * extent. */
	copied = zicio_ext4_read_extents(
				desc, desc->buffers.metadata_buffer,
				metadata_meter, desc->metadata_ctrl.inode_buffer,
				inode_meter, zicio_file_struct, file_inode);

	inode_unlock_shared(file_inode);

	return copied;
}

/*
 * zicio_initialize_metadata_buffer_shared
 *
 * Initialize metadata buffers and return how many entries are loaded
 */
unsigned long
zicio_initialize_metadata_buffer_shared(
		zicio_shared_pool *zicio_shared_pool, zicio_file_struct *zicio_file,
		struct file *file, unsigned long *start_file_ext_number,
		unsigned long shared_pool_copied)
{
	struct inode *file_inode = zicio_get_inode_from_file(file);
	unsigned long copied = 0;

	/* If depth == 0, then copy the extents from inode */
	if (!zicio_file->has_index_extent) {
		*start_file_ext_number = zicio_ext4_simple_copy_extents_shared(
					zicio_shared_pool, zicio_file, shared_pool_copied);
		return *start_file_ext_number;
	}

	inode_lock_shared(file_inode);

	/* If the index extent is cached, the leaf extent node is read using that
	 * extent. */
	copied = zicio_ext4_read_extents_shared(zicio_shared_pool,
			zicio_file, shared_pool_copied, file_inode);

	if (!IS_ERR_VALUE(copied)) {
		*start_file_ext_number += copied;
	}

	inode_unlock_shared(file_inode);

	return copied;
}

/*
 * zicio_initialize_extent_and_metadata
 *
 * Initialize extent tree and metadata
 */
long
zicio_initialize_extent_and_metadata(zicio_descriptor *desc,
		struct fd* fs, int nr_fd)
{
	zicio_notify_descriptor *zicio_notify_desc
		= (zicio_notify_descriptor*)desc;
	zicio_read_files *zicio_rfile;
	zicio_file_struct *cur_file;
	unsigned long index_ext_cnt;
	struct inode *file_inode;
	int i, num_ext_init_file;
	unsigned int offset_in_hugepage;

	if (!nr_fd) {
		return 0;
	}

	zicio_rfile = &desc->read_files;

	/* Initialize the memory and metadata for nvme_cmd_infos */
	if (zicio_notify_desc->nr_fd_of_batches != 0) {
		zicio_notify_desc->nr_nvme_cmd_infos = 0;
		zicio_notify_desc->nr_nvme_cmd_info_start_offsets = 0;
		offset_in_hugepage = 0;
	}

	/*
	 * Initialize extent tree buffer
	 */
	for (i = 0 ; i < nr_fd ; i++) {
		cur_file = zicio_get_id_file_struct(zicio_rfile, i);

		BUG_ON(!cur_file);

		file_inode = zicio_get_inode_from_file(fs[i].file);

		write_inode_now(file_inode, true);

		inode_dio_begin(file_inode);

		if (zicio_notify_desc->nr_fd_of_batches != 0) {
			zicio_set_nvme_cmd_infos((void*)desc, i, file_inode,
									 &offset_in_hugepage);
		} else {
			index_ext_cnt = zicio_initialize_extent_buffers(
						cur_file, fs[i].file);

			if (IS_ERR_VALUE(index_ext_cnt)) {
				printk(KERN_WARNING "[Kernel Message] Error in "
						"zicio_initialize_extent_buffers()\n");
				return index_ext_cnt;
			}

			/* Set meter to show extent tree meter's produced size */
			cur_file->extent_tree_meter.consumed_no_mod = 0;
			cur_file->extent_tree_meter.produced_no_mod = index_ext_cnt;
		}
	}

	if (zicio_notify_desc->nr_fd_of_batches == 0) {
		/* Using the set value, it determines how much file metadata to read. */
		num_ext_init_file = (ZICIO_CHANNEL_INIT_FILE_NUM > nr_fd) ?
				nr_fd : ZICIO_CHANNEL_INIT_FILE_NUM;

		/* Metadata initialization is performed on the file to be read first. */
		for (i = 0 ; i < num_ext_init_file ; i++) {
			cur_file = zicio_get_id_file_struct(zicio_rfile, i);
			zicio_initialize_metadata_buffer(desc, cur_file, fs[i].file);
		}
	}

	return 0;
}


/*
 * zicio_initialize_extent_and_metadata_shared
 *
 * Initialize extent tree and metadata
 */
long
zicio_initialize_extent_and_metadata_shared(
			zicio_shared_pool *zicio_shared_pool, struct fd* fs, int nr_fd)
{
	zicio_read_files *zicio_rfile;
	zicio_file_struct *cur_file;
	unsigned long index_ext_cnt, total_idx_ext_cnt = 0;
	unsigned long *file_start_point;
	struct inode *file_inode;
	int i;

	if (!nr_fd) {
		return 0;
	}

	zicio_rfile = &(zicio_shared_pool->shared_files.registered_read_files);
	file_start_point = kmalloc(sizeof(unsigned long) * zicio_rfile->num_fds,
			GFP_KERNEL|__GFP_ZERO);

	if (unlikely(!file_start_point)) {
		return -ENOMEM;
	}

	/*
	 * Initialize extent tree buffer
	 */
	for (i = 0 ; i < nr_fd ; i++) {
		cur_file = zicio_get_id_file_struct(zicio_rfile, i);

		BUG_ON(!cur_file);
		file_inode = zicio_get_inode_from_file(fs[i].file);

		write_inode_now(file_inode, true);

		inode_dio_begin(file_inode);
		index_ext_cnt = zicio_initialize_extent_buffers(cur_file,
					fs[i].file);

		if (IS_ERR_VALUE(index_ext_cnt)) {
			printk(KERN_WARNING "[Kernel Message] Error in "
					"zicio_initialize_extent_buffers()\n");
			return index_ext_cnt;
		}

		/* Set meter to show extent tree meter's produced size */
		cur_file->extent_tree_meter.consumed_no_mod = 0;
		cur_file->extent_tree_meter.produced_no_mod = index_ext_cnt;
		total_idx_ext_cnt += index_ext_cnt;
	}

	/*
	 * Initializing metadata buffers
	 */

	/* If channel is shared mode, then initializing and allocating shared
	   metadata buffer */
	zicio_allocate_and_initialize_shared_metadata_ctrl(
			zicio_shared_pool, total_idx_ext_cnt, file_start_point);
	for (i = 0 ; i < nr_fd ; i++) {
		cur_file = zicio_get_id_file_struct(zicio_rfile, i);

		zicio_initialize_metadata_buffer_shared(zicio_shared_pool,
				cur_file, fs[i].file,
				(i != nr_fd - 1) ? &(file_start_point[i + 1]) :
					&(zicio_shared_pool->shared_metadata_ctrl.num_metadata),
					file_start_point[i]);

		if (i != nr_fd - 1)  {
			file_start_point[i + 1] += file_start_point[i];
		}
	}

	/* Set number of metadata for this file */
	zicio_shared_pool->shared_metadata_ctrl.num_metadata +=
		file_start_point[nr_fd - 1];
	zicio_reset_shared_metadata_buffer(zicio_shared_pool);

	return 0;
}

/*
 * zicio_produce_metadata - produce metadata using inode buffer.
 * @sd: zicio descriptor for control
 *
 * The metadata buffer is filled by interrupt handler.
 * When this function is called, extent creation begin.
 */
int
zicio_produce_metadata(zicio_descriptor* sd)
{
	zicio_metadata_ctrl *ctrl;
	unsigned metadata_consumed_no_mod, metadata_produced_no_mod;
	unsigned metadata_produce_idx;
	unsigned inode_consumed_no_mod, inode_produced_no_mod;
	unsigned inode_consume_idx;
	void *inode_buffer; /* zicio descriptor's inode buffer */
	void *metadata_buffer; /* zicio descriptor's metadata buffer */
	int depth;

	/* For tracking the size of metadata produced */
	unsigned orig_metadata_produced_no_mod;

	if (unlikely(sd == NULL)) {
		return -EINVAL;
	}

	if (unlikely(sd->metadata_ctrl.inode_buffer == NULL)) {
		return -EINVAL;
	}

	ctrl = &sd->metadata_ctrl;

	 /* Get consumed and produced index of inode buffer */
	inode_consumed_no_mod = ctrl->inode_meter.consumed_no_mod;
	inode_produced_no_mod = zicio_get_produced_inode_bitmap(
				&ctrl->inode_meter, inode_consumed_no_mod);

	if (unlikely(inode_consumed_no_mod > inode_produced_no_mod))
		return -EINVAL;

	/* Get metadata buffer and inode buffer from zicio descriptor */
	metadata_buffer = zicio_get_metadata_buffer(sd);
	inode_buffer = zicio_get_inode_buffer(sd);

	orig_metadata_produced_no_mod = ctrl->metadata_meter.produced_no_mod;

	if (inode_consumed_no_mod == inode_produced_no_mod) {
		return 0;
	}

	/*
	 * If there is inode that we can use to produce mapping information, we make
	 * mapping information.
	 */

	while (inode_consumed_no_mod < inode_produced_no_mod)
	{
		struct zicio_ext4_extent_header *extent_header;
		struct zicio_ext4_extent *ex;
		int i, len;
		void *inode_block;

		inode_consume_idx = inode_consumed_no_mod % ZICIO_INODE_BUF_MAX;
		inode_block = ZICIO_INODE_BUF_GET_BLOCK(inode_buffer,
					inode_consume_idx);

		extent_header = ZICIO_BLOCK_GET_EXTENT_HEADER(inode_block);
		ex = ZICIO_BLOCK_GET_FIRST_EXTENT(inode_block);

		/* Get inode's depth */
		depth = le16_to_cpu(extent_header->eh_depth);

		/* We process only leaf inodes. If we find internal nodes,
		 * return error. */
		if (depth != 0)
			return -EINVAL;

		len = le16_to_cpu(extent_header->eh_entries);

		metadata_consumed_no_mod = ctrl->metadata_meter.consumed_no_mod;
		metadata_produced_no_mod = ctrl->metadata_meter.produced_no_mod;

		/*
		 * Check whether the buffer size of metadata is sufficient to store new
		 * extents. We can't provide new mapping info, just end loop.
		 */
		if ((metadata_produced_no_mod + len - metadata_consumed_no_mod) >
				ZICIO_METADATA_BUF_MAX) {
			break;
		}

		for (i = 0; i < len; i++, ex++)
		{
			struct zicio_ext4_extent *metadata;

			metadata_produce_idx = metadata_produced_no_mod %
					ZICIO_METADATA_BUF_MAX;
			metadata = ZICIO_METADATA_BUF_GET_EXTENT(metadata_buffer,
					metadata_produce_idx);

			metadata->ee_block = ex->ee_block;
			metadata->ee_len = ex->ee_len;
			metadata->ee_start_hi = ex->ee_start_hi;
			metadata->ee_start_lo = ex->ee_start_lo;
			metadata_produced_no_mod += 1;
		}

		zicio_clear_inode_bitmap(&ctrl->inode_meter, inode_consumed_no_mod);
		inode_consumed_no_mod += 1;

		/* increment consumed count */
		ctrl->metadata_meter.produced_no_mod = metadata_produced_no_mod;
	}

	ctrl->inode_meter.consumed_no_mod = inode_consumed_no_mod;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "[Kernel Message]produced meta :%lu\n",
				ctrl->metadata_meter.produced_no_mod -
							orig_metadata_produced_no_mod);
#endif
	return metadata_produced_no_mod - (int)orig_metadata_produced_no_mod;
}
