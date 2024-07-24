#include <linux/types.h>
#include <linux/nvme.h>
#include <linux/zicio_notify.h>
#include <linux/blk_types.h>

#include "zicio_cmd.h"
#include "zicio_device.h"
#include "zicio_extent.h"
#include "zicio_files.h"
#include "zicio_md_flow_ctrl.h"
#include "zicio_mem.h"
#include "zicio_req_submit.h"
#include "zicio_shared_pool.h"


/*
 * zicio_dump_nvme_cmd
 *
 * Dump NVMe command for debugging
 *
 * @struct nvme_command *cmd: NVMe command for dump
 */
void
zicio_dump_nvme_cmd(struct nvme_command *cmd)
{
	unsigned short opcode, flags, command_id;
	int nsid;
	unsigned long long rsvd2;
	long long metadata;
	long long slba;
	unsigned length;
	short int control;
	int dsmgmt, reftag;
	short int apptag, appmask;
	dma_addr_t prp1, prp2;
	dma_addr_t addr;
	unsigned dma_length;
	short rsvd[3];
	short type;

	if (cmd->rw.opcode != nvme_cmd_read) {
		return;
	}

	opcode = cmd->rw.opcode;
	flags = cmd->rw.flags;
	command_id = le16_to_cpu(cmd->rw.command_id);
	nsid = le32_to_cpu(cmd->rw.nsid);
	rsvd2 = le64_to_cpu(cmd->rw.rsvd2);
	metadata = le64_to_cpu(cmd->rw.metadata);
	if (flags == 0) {
		prp1 = le64_to_cpu(cmd->rw.dptr.prp1);
		prp2 = le64_to_cpu(cmd->rw.dptr.prp2);
	} else if (flags & NVME_CMD_SGL_METABUF) {
		addr = le64_to_cpu(cmd->rw.dptr.sgl.addr);
		dma_length = le32_to_cpu(cmd->rw.dptr.sgl.length);
		rsvd[0] = cmd->rw.dptr.sgl.rsvd[0];
		rsvd[1] = cmd->rw.dptr.sgl.rsvd[1];
		rsvd[2] = cmd->rw.dptr.sgl.rsvd[2];
		type = cmd->rw.dptr.sgl.type;
	} else {
		printk(KERN_WARNING "Unexpected command flag set\n");
	}
	slba = le64_to_cpu(cmd->rw.slba);
	length = le16_to_cpu(cmd->rw.length);
	control = le16_to_cpu(cmd->rw.control);
	dsmgmt = le32_to_cpu(cmd->rw.dsmgmt);
	reftag = le32_to_cpu(cmd->rw.reftag);
	apptag = le32_to_cpu(cmd->rw.apptag);
	appmask = le32_to_cpu(cmd->rw.appmask);

	printk(KERN_WARNING "opcode : %d\n", opcode);
	printk(KERN_WARNING "flags : %d\n", flags);
	printk(KERN_WARNING "command_id : %d\n", command_id);
	printk(KERN_WARNING "nsid : %d\n", nsid);
	printk(KERN_WARNING "rsvd2 : %llu\n", rsvd2);
	printk(KERN_WARNING "metadata : %lld\n", metadata);
	if (flags == 0) {
		printk(KERN_WARNING "prp1 : %lld\n", prp1);
		printk(KERN_WARNING "prp2 : %lld\n", prp2);
	} else {
		printk(KERN_WARNING "addr : %lld\n", addr);
		printk(KERN_WARNING "dma_length : %d\n", dma_length);
		printk(KERN_WARNING "rsvd[0] : %d\n", rsvd[0]);
		printk(KERN_WARNING "rsvd[1] : %d\n", rsvd[1]);
		printk(KERN_WARNING "rsvd[2] : %d\n", rsvd[2]);
		printk(KERN_WARNING "type : %d\n", type);
	}
	printk(KERN_WARNING "slba : %lld\n", slba);
	printk(KERN_WARNING "length : %u\n", length);
	printk(KERN_WARNING "control : %d\n", control);
	printk(KERN_WARNING "dsmgmt : %d\n", dsmgmt);
	printk(KERN_WARNING "reftag : %d\n", reftag);
	printk(KERN_WARNING "apptag : %d\n", apptag);
	printk(KERN_WARNING "appmask : %d\n", appmask);
}
EXPORT_SYMBOL(zicio_dump_nvme_cmd);

/*
 * zicio_get_block_partition_remap_offset
 *
 * Get the offset of partition where the file is located in block device.
 *
 * @fd: target file
 *
 * Return: Remapping offset
 */
static inline sector_t
zicio_get_block_partition_remap_offset(struct fd fd)
{
	/*
	 * At the time of calling this function, verification of the pointer has
	 * already been completed while checking which block device the file is
	 * located on.
	 */
	return fd.file->f_mapping->host->i_sb->s_bdev->bd_start_sect;
}

/*
 * zicio_get_block_partition_remap_offset
 *
 * Get the offset of partition where the file is located in block device
 *
 * @rdev: device in MD context where file is located.
 *
 * Return: Remapping offset
 */
static inline sector_t
zicio_get_block_partition_remap_offset_from_rdev(struct md_rdev *rdev)
{
	/*
	 * At the time of calling this function, verification of the pointer has
	 * already been completed while checking which block device the file is
	 * located on.
	 */
	return rdev->bdev->bd_start_sect;
}

/*
 * zicio_initialize_nvme_cmd
 *
 * Initialize nvme command
 *
 * @nvme_cmd: NVMe command to set
 * @device_idx: device idx in channel
 * @fd: target file struct
 * @logical_block_idx: logical block location
 * @fs_block_idx: file system's block location
 * @file_chunk_id: zicio channel's chunk location
 * @local_page_idx: local page index in DRAM
 * @is_metadata: whether or not this command is metadata
 */
static inline void
zicio_initialize_nvme_cmd(zicio_nvme_cmd_list *nvme_cmd, int device_idx,
			struct fd fd, zicio_ext4_lblk_t logical_block_idx,
			zicio_ext4_fsblk_t fs_block_idx, unsigned int file_chunk_id,
			int local_page_idx, bool is_metadata)
{
	/* Set nvme_cmd_list members */
	nvme_cmd->device_idx = device_idx;
	nvme_cmd->fd = fd;
	nvme_cmd->file_chunk_id = file_chunk_id;
	nvme_cmd->local_huge_page_idx = local_page_idx;
	nvme_cmd->is_metadata = is_metadata;
	nvme_cmd->start_lpos = logical_block_idx;
	nvme_cmd->start_fpos = fs_block_idx;
}

/*
 * zicio_initialize_md_cmd
 *
 * Initialize md command
 *
 * @nvme_cmd: NVMe command to set
 * @fd: target file struct
 * @logical_block_idx: logical block location
 * @fs_block_idx: file system's block location
 * @file_chunk_id: zicio channel's chunk location
 * @local_page_idx: local page index in DRAM
 * @is_metadata: whether or not this command is metadata
 */
static inline void
zicio_initialize_md_cmd(zicio_nvme_cmd_list *nvme_cmd,
		struct fd fd, zicio_ext4_lblk_t logical_block_idx,
		zicio_ext4_fsblk_t fs_block_idx, unsigned int file_chunk_id,
		int local_page_idx, bool is_metadata)
{
	/* Set nvme cmd list members for md */
	nvme_cmd->fd = fd;
	nvme_cmd->file_chunk_id = file_chunk_id;
	nvme_cmd->local_huge_page_idx = local_page_idx;
	nvme_cmd->is_metadata = is_metadata;
	nvme_cmd->start_lpos = logical_block_idx;
	nvme_cmd->start_fpos = fs_block_idx;
}

/*
 * zicio_allocate_nvme_cmd
 *
 * Create nvme command
 *
 * @nvme_cmd_list: command list pended existing commands
 * @nvme_cmd: newly allocated command pointer
 *
 * Return: allocated nvme command struct
 */
static struct zicio_nvme_cmd_list *
zicio_allocate_nvme_cmd(
			zicio_nvme_cmd_list **nvme_cmd_list,
			zicio_nvme_cmd_list **nvme_cmd)
{
	/* Allocate nvme command and pending command list per chunk */
	if (*nvme_cmd_list) {
		(*nvme_cmd)->next = zicio_get_nvme_cmd_list();
		*nvme_cmd = (*nvme_cmd)->next;
	} else {
		*nvme_cmd_list = *nvme_cmd = zicio_get_nvme_cmd_list();
	}
	/* Return allocated nvme command struct  */
	return (*nvme_cmd);
}

/*
 * zicio_set_nvme_block_mapping_command
 *
 * Set fields of NVMe command
 * At this point, opcode, slba(start logical block area) and length are set
 *
 * @nvme_cmd_list: command list pended existing commands
 * @nvme_cmd: newly allocated command pointer
 */
static inline void
zicio_set_nvme_block_mapping_command(zicio_nvme_cmd_list *nvme_cmd,
		enum nvme_opcode nvme_opcode, zicio_ext4_fsblk_t cur_block,
		sector_t bd_start_sector, unsigned int cmd_length)
{
	nvme_cmd->cmd.rw.opcode = nvme_opcode;
	nvme_cmd->cmd.rw.slba = cpu_to_le64((cur_block <<
			ZICIO_PAGE_TO_SECTOR_SHIFT) + bd_start_sector);
	nvme_cmd->cmd.rw.length = cpu_to_le16((cmd_length <<
			ZICIO_PAGE_TO_SECTOR_SHIFT) - 1);
}

/*
 * zicio_find_strip_zone
 *
 * Find strip zone from sector number and get the offset in the zone
 *
 * @conf: raid0 config that has striping zone information
 * @sector_offset_in_zone: sector offset in zone
 *
 * Return: strip zone structure pointer
 */
static struct strip_zone *
zicio_find_strip_zone(struct r0conf *conf, sector_t *sector_offset_in_zone)
{
	int i;
	struct strip_zone *z = conf->strip_zone;
	sector_t sector = *sector_offset_in_zone;

	for (i = 0 ; i < conf->nr_strip_zones ; i++) {
		/* Checking the current sector is in zone */
		if (sector < z[i].zone_end) {
			/* Then, Return zone, and get the offset in the zone */
			if (i) {
				*sector_offset_in_zone = sector - z[i - 1].zone_end;
			}
			return z + i;
		}
	}
	/* Codes out of zone are treated as BUG. */
	BUG();
}

/*
 * zicio_map_sector
 *
 * Get the sector number on raw devices from the sector number of file system
 *
 * @mddev: multi-device structure
 * @zone: information of zone to read
 * @start_sector: start sector in md
 * @sector_offset: sector offset in zone and mdevice
 * @psector_offset_in_chunk: sector offet in chunk pointer
 *
 * Return raw device in md set
 */
static struct md_rdev *
zicio_map_sector(struct mddev *mddev, struct strip_zone *zone,
			sector_t start_sector, sector_t *sector_offset,
			sector_t *psector_offset_in_chunk)
{
	struct r0conf *conf = mddev->private;
	int raid_disks = conf->strip_zone[0].nb_dev;
	sector_t chunk_number_in_zone;
	unsigned int chunk_length = mddev->chunk_sectors;
	unsigned int sector_offset_in_chunk;

	if (is_power_of_2(chunk_length)) {
		/* Find the first zero bit from ~chunk_sects) */
		int chunksect_bits = ffz(~chunk_length);
		/* Find the sector offset inside the chunk */
		sector_offset_in_chunk = start_sector & (chunk_length - 1);
		/* Find the global chunk number */
		start_sector >>= chunksect_bits;
		/* Find the chunk number in zone */
		chunk_number_in_zone = *sector_offset;
		/* Quotient is the chunk in real device and remainder is
		 * the sector offset in chunk */
		*psector_offset_in_chunk = sector_div(chunk_number_in_zone,
					zone->nb_dev << chunksect_bits);
	} else {
		/* Find the sector offset inside the chunk */
		sector_offset_in_chunk = sector_div(start_sector, chunk_length);
		chunk_number_in_zone = *sector_offset;
		/* Quotient is the chunk in real device and remainder is
		 * the sector offset in chunk */
		*psector_offset_in_chunk = sector_div(chunk_number_in_zone,
					chunk_length * zone->nb_dev);
	}

	/*
	 * position in the real device
	 * real sector = start sector of zone + start sector of chunk in zone +
	 *				 sector offset in chunk
	 */
	*sector_offset = (chunk_number_in_zone * chunk_length) +
			sector_offset_in_chunk;
	return conf->devlist[(zone - conf->strip_zone) * raid_disks +
				sector_div(start_sector, zone->nb_dev)];
}

/*
 * zicio_get_command_length_for_md
 *
 * Get command length of per-device consisting of md.
 *
 * @mddev: md device structure
 * @start_sector: start sector of this command
 * @total_cmd_sectors: total length of this command
 * @num_inner_dev: number of inner device
 * @chunk_sects: the number of chunk consisting of one chunk
 *
 * Return: command sector array
 */
static sector_t *
zicio_get_command_length_for_md(zicio_descriptor *desc, int device_idx,
			struct mddev *mddev, sector_t start_sector,
			sector_t total_cmd_sectors, int num_inner_dev, unsigned chunk_sects)
{
	struct r0conf *conf = mddev->private;
	struct strip_zone *zone;
	struct md_rdev *current_rdev;
	int idx, first_inner_dev_idx = -1, inner_dev_idx;
	unsigned int num_chunk_sets, num_inner_cmd, start_page;
	sector_t sector_offset_in_chunk, first_chunk_set_length = 0, last_sectors;
	sector_t sector;
	/* TODO: Change this to pool */
	sector_t *per_cmd_sectors = zicio_alloc_cmd_lists_set_with_desc(
				desc, device_idx);

	/* Get the number device to create command */
	zicio_sector_div(num_inner_cmd, total_cmd_sectors - 1, chunk_sects);
	if (num_inner_cmd >= num_inner_dev) {
		num_inner_cmd = num_inner_dev;
	} else {
		num_inner_cmd++;
	}

	/* Get the length of command per device */
	for (idx = 0 ; idx < num_inner_cmd ; idx++) {
		/* Get the start sector of device command and find a zone */
		sector = start_sector + first_chunk_set_length;
		start_page = sector >> ZICIO_PAGE_TO_SECTOR_SHIFT;
		zone = zicio_find_strip_zone(mddev->private, &sector);

		/* Get a sector location in raw device */
		switch (conf->layout) {
			case RAID0_ORIG_LAYOUT:
				current_rdev = zicio_map_sector(mddev, zone, start_sector,
						&sector, &sector_offset_in_chunk);
				break;
			case RAID0_ALT_MULTIZONE_LAYOUT:
				current_rdev = zicio_map_sector(mddev, zone, sector,
						&sector, &sector_offset_in_chunk);
				break;
			default:
				BUG();
		}
		/* Find an idx into multi-device */
		inner_dev_idx = sector_div(start_page, zone->nb_dev);
		if (first_inner_dev_idx == -1) {
			first_inner_dev_idx = inner_dev_idx;
		}
		/* If the starting position of the chunk is not the same as the reading
		 * starting position, adjustment is required. */
		per_cmd_sectors[inner_dev_idx] = chunk_sects * (inner_dev_idx + 1) -
				sector_offset_in_chunk;
		first_chunk_set_length += per_cmd_sectors[inner_dev_idx];
	}

	last_sectors = total_cmd_sectors - first_chunk_set_length;

	if (!last_sectors) {
		return per_cmd_sectors;
	}

	last_sectors = zicio_sector_div(num_chunk_sets, last_sectors,
				chunk_sects * num_inner_dev);

	/* Calculate per device length for command */
	for (idx = 0 ; idx < num_inner_cmd ; idx++) {
		inner_dev_idx = idx + first_inner_dev_idx;
		inner_dev_idx = sector_div(inner_dev_idx,
				chunk_sects * num_inner_dev >> ZICIO_PAGE_TO_SECTOR_SHIFT);
		per_cmd_sectors[inner_dev_idx] += chunk_sects * num_chunk_sets;

		if (last_sectors > (chunk_sects * (idx + 1))) {
			per_cmd_sectors[inner_dev_idx] += chunk_sects;
			last_sectors -= chunk_sects;
		} else {
			per_cmd_sectors[inner_dev_idx] += last_sectors;
			last_sectors = 0;
		}
	}

	return per_cmd_sectors;
}

/*
 * zicio_set_md_block_mapping_command
 *
 * Set fields of NVMe command
 * At this point, opcode, slba(start logical block area) and length are set
 */
static inline void 
zicio_set_md_block_mapping_command(zicio_descriptor *desc,
		zicio_nvme_cmd_list **start_cmd_lists, struct fd fd, int device_idx,
		zicio_ext4_lblk_t logical_block_idx,
		zicio_ext4_fsblk_t cur_block, unsigned int file_chunk_id,
		int local_page_idx, unsigned int cmd_length,
		enum nvme_opcode nvme_opcode, bool is_metadata)
{
	zicio_device *zicio_device = zicio_get_zicio_device_with_desc(desc,
				device_idx);
	zicio_md_private_data *zicio_md_data = zicio_get_md_private_data(
				zicio_device);
	zicio_dev_map_node *zicio_dev_map_node =
				&desc->dev_maps.dev_node_array[device_idx];
	zicio_nvme_cmd_list **nvme_cmd_lists;
	struct mddev *mddev = zicio_md_data->mddev;
	struct r0conf *conf = mddev->private;
	struct strip_zone *zone;
	struct md_rdev *current_rdev;
	unsigned chunk_sects = mddev->chunk_sectors;	
	unsigned cmd_sectors = cmd_length << ZICIO_PAGE_TO_SECTOR_SHIFT;
	int num_inner_dev = zicio_md_data->num_inner_dev, num_inner_cmd;
	int idx, inner_dev_idx;
	unsigned int start_page;
	sector_t orig_sector;
	sector_t sector;
	sector_t first_chunk_set_length = 0, sector_offset_in_chunk;
	sector_t *per_cmd_sectors;
	sector_t bd_start_sector;

	zicio_sector_div(num_inner_cmd, cmd_sectors - 1, chunk_sects);

	if (num_inner_cmd >= num_inner_dev) {
		num_inner_cmd = num_inner_dev;
	} else {
		num_inner_cmd++;
	}

	/* Get the strip zone and set the relative location in zone to sector */
	orig_sector = cur_block << ZICIO_PAGE_TO_SECTOR_SHIFT;

	/* Find per-device cmd length */
	per_cmd_sectors = zicio_get_command_length_for_md(desc, device_idx,
			mddev, orig_sector, cmd_sectors, num_inner_dev, chunk_sects);

	/* Set nvme command lists */
	nvme_cmd_lists = start_cmd_lists + num_inner_dev;
	/* Allocate and initialize command and map device address for md */
	for (idx = 0 ; idx < num_inner_dev ; idx++) {
		sector = orig_sector + first_chunk_set_length;
		start_page = sector >> ZICIO_PAGE_TO_SECTOR_SHIFT;
		zone = zicio_find_strip_zone(mddev->private, &sector);

		/* Get a sector location in raw device */
		switch (conf->layout) {
			case RAID0_ORIG_LAYOUT:
				current_rdev = zicio_map_sector(mddev, zone,
						orig_sector, &sector, &sector_offset_in_chunk);
				break;
			case RAID0_ALT_MULTIZONE_LAYOUT:
				current_rdev = zicio_map_sector(mddev, zone, sector,
						&sector, &sector_offset_in_chunk);
				break;
			default:
				BUG();
		}

		/* Get block partition start sector in md partition */
		bd_start_sector = zicio_get_block_partition_remap_offset_from_rdev(
						current_rdev);

		/* Currently, we set strip size to 4KiB, so just device logical file
		 * page number to number of device */
		inner_dev_idx = sector_div(start_page, zone->nb_dev);

		/* If currrent command do not needs, then create next comamnd */
		if (!per_cmd_sectors[inner_dev_idx]) {
			continue;
		}

		/* Set the block location to command */
		zicio_allocate_nvme_cmd(&start_cmd_lists[inner_dev_idx],
				&nvme_cmd_lists[inner_dev_idx]);
		zicio_initialize_md_cmd(nvme_cmd_lists[inner_dev_idx], fd,
					logical_block_idx, cur_block + (first_chunk_set_length >>
							ZICIO_PAGE_TO_SECTOR_SHIFT), file_chunk_id,
									local_page_idx, is_metadata);

		/* Set command fields for block mapping (sector, opcode, length) */
		nvme_cmd_lists[inner_dev_idx]->cmd.rw.opcode = nvme_opcode;
		nvme_cmd_lists[inner_dev_idx]->cmd.rw.slba = cpu_to_le64(
				sector + zone->dev_start + current_rdev->data_offset +
						bd_start_sector);
		nvme_cmd_lists[inner_dev_idx]->cmd.rw.length = cpu_to_le16(
				per_cmd_sectors[inner_dev_idx] - 1);

		/* Set an index of device in channel */
		nvme_cmd_lists[inner_dev_idx]->device_idx =
				zicio_dev_map_node->start_point_inner_dev_map + inner_dev_idx +
						desc->dev_maps.nr_dev;

		/* If the starting position of the chunk is not the same as the reading
		 * starting position, adjustment is required. */
		first_chunk_set_length += (chunk_sects * (inner_dev_idx + 1) -
					sector_offset_in_chunk);
		logical_block_idx += (chunk_sects * (inner_dev_idx + 1) -
				sector_offset_in_chunk) >> ZICIO_PAGE_TO_SECTOR_SHIFT;
	}
	zicio_free_cmd_lists_set_with_desc(desc, device_idx, per_cmd_sectors);
}

/*
 * zicio_set_block_mapping_shared_for_nvme
 *
 * Set the mapping of block device to nvme_command
 * Set NVMe command sector information
 */
zicio_nvme_cmd_list *
zicio_set_block_mapping_shared_for_nvme(zicio_descriptor *desc,
			zicio_nvme_cmd_list **start_cmd_lists, int device_idx,
			struct fd fd, int current_file_idx,
			zicio_ext4_lblk_t start_lblock, unsigned int tot_req_blk_cnt,
			unsigned int per_req_blk_cnt, unsigned int file_chunk_id,
			int local_page_idx)
{
	zicio_nvme_cmd_list *nvme_cmd_list = NULL, *nvme_cmd;
	unsigned long cur_metadata_loc; /* consumed metadata */
	struct zicio_ext4_extent *current_extent; /* Extent pointer for checking */
	unsigned cur_len = 0, last_len; /* Current extent's contiguous block length */
	unsigned last_blk_len = per_req_blk_cnt;
	zicio_ext4_fsblk_t cur_block; /* Current block number */
	zicio_ext4_lblk_t cur_lblock; /* Current block number */
	zicio_ext4_lblk_t file_lblock; /* Previous block number */
	zicio_ext4_lblk_t loc_start_lblock;

	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	sector_t bd_start_sector;

	/* Get the start sector of file system */
	bd_start_sector = zicio_get_block_partition_remap_offset(fd);

	/* Bring shared pool and shared pool local information for metadata buffer
	 * reference. */
	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	/* Set start block number for traversing metadata buffer */
	cur_lblock = start_lblock;
	loc_start_lblock = start_lblock;

	/* Set the consumed data of metadata */
	cur_metadata_loc = zicio_get_metadata_for_chunk(zicio_shared_pool,
				zicio_shared_pool_local, current_file_idx, start_lblock);

	BUG_ON(!zicio_check_current_metadata((struct zicio_ext4_extent *)
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer +
					cur_metadata_loc, start_lblock));

	/* Get the current first extent from buffer */
	current_extent =
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer;
	current_extent += cur_metadata_loc;

	/*
	 * Main loop to set NVME command fields related with fs block 
	 */
	while (true) {
		/* Allocate nvme command and pending it to command list */
		zicio_allocate_nvme_cmd(&nvme_cmd_list, &nvme_cmd);

		/* Code Segments to check if the start block of request is not 
		 * the start of cached extent. */
		file_lblock = le32_to_cpu(current_extent->ee_block);

		cur_block = le32_to_cpu(current_extent->ee_start_lo);
		cur_block |=  ((zicio_ext4_fsblk_t) le16_to_cpu(
					current_extent->ee_start_hi) << 31) << 1;

		/* If the start block of request is not the same with the start
		 * block of extents */
		cur_len = min_t(unsigned, (file_lblock + le16_to_cpu(
					current_extent->ee_len) - cur_lblock), last_blk_len);
		cur_block = cur_block + cur_lblock - file_lblock;

		/* Initialize nvme command list descriptor */
		zicio_initialize_nvme_cmd(nvme_cmd, device_idx, fd, cur_lblock,
				cur_block, file_chunk_id, local_page_idx, false);

		if (cur_len + cur_lblock >= start_lblock + tot_req_blk_cnt) {
			/* If every requested block are set to NVMe command */
			last_len = start_lblock + tot_req_blk_cnt - cur_lblock;
			/* Set the command for block mapping */
			zicio_set_nvme_block_mapping_command(nvme_cmd, nvme_cmd_read,
					cur_block, bd_start_sector, last_len);
			if (cur_len + cur_lblock ==
				file_lblock + le16_to_cpu(current_extent->ee_len)) {

				cur_metadata_loc++;
			}
			break;	
		}

		if (cur_len + cur_lblock >= loc_start_lblock + per_req_blk_cnt) {
			cur_len = loc_start_lblock + per_req_blk_cnt - cur_lblock;
			loc_start_lblock = cur_lblock + cur_len;
			last_blk_len = per_req_blk_cnt;
		} else {
			last_blk_len -= cur_len;
		}

		cur_lblock += cur_len;

		/* Set the command for block mapping */
		zicio_set_nvme_block_mapping_command(nvme_cmd, nvme_cmd_read,
				cur_block, bd_start_sector, cur_len);

		if (cur_lblock < file_lblock + le16_to_cpu(current_extent->ee_len)) {
			continue;
		}

		cur_metadata_loc++;
		current_extent++;
	}
	start_cmd_lists[1] = nvme_cmd;

	/* Return the command */
	return nvme_cmd_list;
}

/*
 * zicio_set_block_mapping_shared_for_md
 *
 * Set the mapping of block device to nvme_command
 * Set NVMe command sector information
 */
static zicio_nvme_cmd_list **
zicio_set_block_mapping_shared_for_md(zicio_descriptor *desc,
			int device_idx, int num_inner_device, struct fd fd,
			int current_file_idx, zicio_ext4_lblk_t start_lblock,
			unsigned int tot_req_blk_cnt, unsigned int per_req_blk_cnt,
			unsigned int file_chunk_id, int local_page_idx)
{
	zicio_nvme_cmd_list **nvme_cmd_lists = NULL;
	unsigned long cur_metadata_loc; /* consumed metadata */
	struct zicio_ext4_extent *current_extent; /* Extent pointer for checking */
	unsigned cur_len = 0, last_len; /* Current extent's contiguous block length */
	unsigned last_blk_len;
	unsigned req_blk_cnt;
	zicio_ext4_fsblk_t cur_block; /* Current block number */
	zicio_ext4_lblk_t cur_lblock; /* Current block number */
	zicio_ext4_lblk_t file_lblock; /* Previous block number */
	zicio_ext4_lblk_t loc_start_lblock;

	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;

	/* Bring shared pool and shared pool local information for metadata buffer
	 * reference. */
	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	/* Set start block number for traversing metadata buffer */
	cur_lblock = start_lblock;
	loc_start_lblock = start_lblock;

	/* Set the consumed data of metadata */
	cur_metadata_loc = zicio_get_metadata_for_chunk(zicio_shared_pool,
				zicio_shared_pool_local, current_file_idx, start_lblock);

	BUG_ON(!zicio_check_current_metadata((struct zicio_ext4_extent *)
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer +
					cur_metadata_loc, start_lblock));

	/* Get the current first extent from buffer */
	current_extent =
			zicio_shared_pool->shared_metadata_ctrl.shared_metadata_buffer;
	current_extent += cur_metadata_loc;

	/* Set total block count per command and last block length */
	req_blk_cnt = per_req_blk_cnt * num_inner_device;
	last_blk_len = req_blk_cnt;

	nvme_cmd_lists =  zicio_alloc_cmd_lists_set_with_desc(desc, device_idx);
 
	/*
	 * Main loop to set NVME command fields related with fs block 
	 */
	while (true) {
		/* Code Segments to check if the start block of request is not 
		 * the start of cached extent. */
		file_lblock = le32_to_cpu(current_extent->ee_block);

		cur_block = le32_to_cpu(current_extent->ee_start_lo);
		cur_block |=  ((zicio_ext4_fsblk_t) le16_to_cpu(
					current_extent->ee_start_hi) << 31) << 1;

		/* If the start block of request is not the same with the start
		 * block of extents */
		cur_len = min_t(unsigned, (file_lblock + le16_to_cpu(
					current_extent->ee_len) - cur_lblock), last_blk_len);
		cur_block = cur_block + cur_lblock - file_lblock;

		/* If every requested block are set to NVMe command */
		if (cur_len + cur_lblock >= start_lblock + tot_req_blk_cnt) {
			/* then, get the length of last command */
			last_len = start_lblock + tot_req_blk_cnt - cur_lblock;

			/* Map block address to command */
			zicio_set_md_block_mapping_command(desc, nvme_cmd_lists, fd,
					device_idx, cur_lblock, cur_block, file_chunk_id,
					local_page_idx, last_len, nvme_cmd_read, false);
			/* If blocks mapped by current extent is consumed all, then
			 * increase one metadata cursor */
			if (cur_len + cur_lblock ==
				file_lblock + le16_to_cpu(current_extent->ee_len)) {
				cur_metadata_loc++;
			}
			break;	
		}

		if (cur_len + cur_lblock >= loc_start_lblock + req_blk_cnt) {
			/* If every block to read are in the current extent, then we can
			 * use all of mapping information from this extent. */
			cur_len = loc_start_lblock + req_blk_cnt - cur_lblock;
			loc_start_lblock = cur_lblock + cur_len;
			last_blk_len = req_blk_cnt;
		} else {
			/* Otherwise, there's possibilities that data can be located
			 * dispartly. So, what is left in the extent is read. */
			last_blk_len -= cur_len;
		}

		/* Map block address to command */
		zicio_set_md_block_mapping_command(desc, nvme_cmd_lists, fd,
				device_idx, cur_lblock, cur_block, file_chunk_id,
				local_page_idx, cur_len, nvme_cmd_read, false);

		cur_lblock += cur_len;

		if (cur_lblock < file_lblock + le16_to_cpu(current_extent->ee_len)) {
			continue;
		}

		cur_metadata_loc++;
		current_extent++;
	}

	/* Return the command */
	return nvme_cmd_lists;
}


/*
 * zicio_adjust_metadata_cursor
 *
 * When multiple requests are allocated, the location of metadata may not match.
 * At this time, adjacent metadata is acquired for location adjustment.
 */
static struct zicio_ext4_extent *
zicio_adjust_metadata_cursor(zicio_descriptor *desc, int device_idx,
		unsigned meta_consumed_no_mod, unsigned meta_produced_no_mod,
		zicio_ext4_lblk_t cur_lblock, int move_direction)
{
	unsigned meta_cursor_no_mod = meta_consumed_no_mod;
	unsigned meta_consumed_page_mod;
	unsigned meta_consumed_offset;
	struct zicio_ext4_extent *current_extent;

	do {
		meta_cursor_no_mod += move_direction;

		if (meta_cursor_no_mod == UINT_MAX) {
			meta_cursor_no_mod = meta_produced_no_mod - 1;
		}
		/* First page number of consumed metadata */
		meta_consumed_page_mod = meta_consumed_no_mod /
			ZICIO_NUM_EXTENT_IN_PAGE;
		meta_consumed_page_mod &= ~ZICIO_METADATA_PAGE_NUM_MASK;
		/* First in-page offset of consumed metadata */
		meta_consumed_offset = meta_consumed_no_mod %
			ZICIO_NUM_EXTENT_IN_PAGE;

		/* Get current extent */
		current_extent = zicio_get_extent_in_buffer(
				desc->buffers.metadata_buffer, meta_consumed_page_mod,
				meta_consumed_offset);
	} while (zicio_check_ext_current_metadata(current_extent, cur_lblock));

	return current_extent;
}

/*
 * zicio_set_block_mapping_for_nvme
 *
 * Set the mapping of block device to nvme_command
 */
static zicio_nvme_cmd_list *
zicio_set_block_mapping_for_nvme(zicio_descriptor *desc,
			zicio_nvme_cmd_list **nvme_cmd_lists, int device_idx,
			struct fd fd, unsigned long current_file_page_idx,
			zicio_ext4_lblk_t start_lblock, unsigned tot_req_blk_cnt,
			unsigned per_req_blk_cnt, unsigned *need_feed,
			int local_huge_page_idx)
{
	/* Metadata meter which shows consumed and produced metadata */
	zicio_meter *metadata_meter;
	zicio_nvme_cmd_list *nvme_cmd_list = NULL, *nvme_cmd = NULL; /* nvme command list */
	unsigned meta_consumed_no_mod; /* consumed metadata */
	unsigned meta_produced_no_mod;  /* produced metadata */
	unsigned meta_consumed_page_mod; /* Page number of consumed metadata */
	unsigned meta_consumed_offset; /* In-page offset of consumed metadata */
	unsigned cur_meta_consumed = 0; /* Current consumed metadata  */
	struct zicio_ext4_extent *current_extent; /* Extent pointer for checking */
	unsigned cur_len = 0, last_len; /* Current extent's contiguous block length */
	unsigned last_blk_len = per_req_blk_cnt;
	zicio_ext4_fsblk_t cur_block; /* Current block number */
	zicio_ext4_lblk_t cur_lblock; /* Current block number */
	zicio_ext4_lblk_t file_lblock; /* Previous block number */
	zicio_ext4_lblk_t loc_start_lblock;
	sector_t bd_start_sector;
	int cursor_direction;

	/* Get the start sector of file system */
	bd_start_sector = zicio_get_block_partition_remap_offset(fd);

	/* Get metadata meter for command */
	metadata_meter = &desc->metadata_ctrl.metadata_meter;
	/* Set start block number for traversing metadata buffer */
	cur_lblock = start_lblock;
	loc_start_lblock = start_lblock;

	/* Set the consumed data of metadata */
	meta_consumed_no_mod = metadata_meter->consumed_no_mod;
	/* Set the produced data of metadata */
	meta_produced_no_mod = metadata_meter->produced_no_mod;
	/* First page number of consumed metadata */
	meta_consumed_page_mod = meta_consumed_no_mod /
				ZICIO_NUM_EXTENT_IN_PAGE;
	meta_consumed_page_mod &= ~ZICIO_METADATA_PAGE_NUM_MASK;
	/* First in-page offset of consumed metadata */
	meta_consumed_offset = meta_consumed_no_mod % ZICIO_NUM_EXTENT_IN_PAGE;

	/* Get the current first extent from buffer */
	current_extent = zicio_get_extent_in_buffer(
			desc->buffers.metadata_buffer, meta_consumed_page_mod,
					meta_consumed_offset);

	/* Checking current cursor is correctly specifying the metadata to read */
	cursor_direction = zicio_check_ext_current_metadata(current_extent,
				cur_lblock);

	/*
	 * When multiple requests are allocated, the location of metadata may not
	 * be matched. At this time, adjacent metadata is checked for repositioning.
	 */
	if (cursor_direction) {
		current_extent = zicio_adjust_metadata_cursor(desc, device_idx,
				meta_consumed_no_mod, meta_produced_no_mod, cur_lblock,
						cursor_direction);
	}

	/*
	 * Main loop to set NVME command fields related with fs block 
	 */
	while (cur_meta_consumed < meta_produced_no_mod - meta_consumed_no_mod) {
		/* Allocate nvme command and pending it to command list */
		zicio_allocate_nvme_cmd(&nvme_cmd_list, &nvme_cmd);

		/* Code Segments to check if the start block of request is not 
		 * the start of cached extent. */
		file_lblock = le32_to_cpu(current_extent->ee_block);

		cur_block = le32_to_cpu(current_extent->ee_start_lo);
		cur_block |= ((zicio_ext4_fsblk_t) le16_to_cpu(
				current_extent->ee_start_hi) << 31) << 1;

		/* If the start block of request is not the same with the start
		 * block of extents */
		cur_len = min_t(unsigned, (file_lblock + le16_to_cpu(
				current_extent->ee_len) - cur_lblock), last_blk_len);
		cur_block = cur_block + cur_lblock - file_lblock;

		/* Initialize nvme command list descriptor */
		zicio_initialize_nvme_cmd(nvme_cmd, device_idx, fd,
					cur_lblock, cur_block, current_file_page_idx >>
					ZICIO_PAGE_TO_CHUNK_SHIFT, local_huge_page_idx, false);

		if (cur_len + cur_lblock >= start_lblock + tot_req_blk_cnt) {
			/* If every requested block are set to NVMe command */
			last_len = start_lblock + tot_req_blk_cnt - cur_lblock;

			/* Set the command for block mapping */
			zicio_set_nvme_block_mapping_command(nvme_cmd,
					nvme_cmd_read, cur_block, bd_start_sector, last_len);
			if (cur_len + cur_lblock ==
				file_lblock + le16_to_cpu(current_extent->ee_len)) {

				cur_meta_consumed++;
			}
			break;	
		}

		if (cur_len + cur_lblock >= loc_start_lblock + per_req_blk_cnt) {
			cur_len = loc_start_lblock + per_req_blk_cnt - cur_lblock;
			loc_start_lblock = cur_lblock + cur_len;
			last_blk_len = per_req_blk_cnt;
		} else {
			last_blk_len -= cur_len;
		}

		cur_lblock += cur_len;

		/* Set the command for block mapping */
		zicio_set_nvme_block_mapping_command(nvme_cmd,
				nvme_cmd_read, cur_block, bd_start_sector, cur_len);

		if (cur_lblock < file_lblock + le16_to_cpu(current_extent->ee_len)) {
			continue;
		}

		cur_meta_consumed++;
		if (meta_consumed_offset + 1 == ZICIO_NUM_EXTENT_IN_PAGE) {
			meta_consumed_offset = 0;
			if (meta_consumed_page_mod + 1 == ZICIO_METADATABUFFER_PAGENUM) {
				meta_consumed_page_mod = 0;
			}
			meta_consumed_page_mod++;
			current_extent = zicio_get_extent_in_buffer(
					desc->buffers.metadata_buffer, meta_consumed_page_mod,
					meta_consumed_offset);
		} else {
			meta_consumed_offset++;
			current_extent++;
		}
	}

	if (cursor_direction >= 0) {
		metadata_meter->consumed_no_mod += cur_meta_consumed;
	}

	if (meta_produced_no_mod - meta_consumed_no_mod - cur_meta_consumed < 
			ZICIO_MAX_EXTENT_IN_BUFFER >> 1) {
		*need_feed = ZICIO_MAX_EXTENT_IN_BUFFER >> 2;
	}
	nvme_cmd_lists[1] = nvme_cmd;

	/* Return the command */
	return nvme_cmd_list;
}

/*
 * zicio_set_block_mapping_for_md
 *
 * Set the mapping of block device to nvme_command
 */
static zicio_nvme_cmd_list **
zicio_set_block_mapping_for_md(zicio_descriptor *desc, int device_idx,
			int num_inner_device, struct fd fd,
			unsigned long current_file_page_idx,
			zicio_ext4_lblk_t start_lblock, unsigned tot_req_blk_cnt,
			unsigned per_req_blk_cnt, unsigned *need_feed,
			int local_huge_page_idx)
{
	/* Metadata meter which shows consumed and produced metadata */
	zicio_meter *metadata_meter;
	zicio_nvme_cmd_list **nvme_cmd_lists;
	unsigned meta_consumed_no_mod; /* consumed metadata */
	unsigned meta_produced_no_mod;  /* produced metdata */
	unsigned meta_consumed_page_mod; /* Page number of consumed metadata */
	unsigned meta_consumed_offset; /* In-page offset of consumed metadata */
	unsigned cur_meta_consumed = 0; /* Current consumed metadata  */
	struct zicio_ext4_extent *current_extent; /* Extent pointer for checking */
	unsigned cur_len = 0, last_len; /* Current extent's contiguous block length */
	unsigned last_blk_len; /* Last command length */
	unsigned req_blk_cnt;
	zicio_ext4_fsblk_t cur_block; /* Current block number */
	zicio_ext4_lblk_t cur_lblock; /* Current block number */
	zicio_ext4_lblk_t file_lblock; /* Previous block number */
	zicio_ext4_lblk_t loc_start_lblock;
	int cursor_direction;

	/* Get metadata meter for command */
	metadata_meter = &desc->metadata_ctrl.metadata_meter;
	/* Set start block number for traversing metadata buffer */
	cur_lblock = start_lblock;
	loc_start_lblock = start_lblock;

	/* Set the consumed data of metadata */
	meta_consumed_no_mod = metadata_meter->consumed_no_mod;
	/* Set the produced data of metadata */
	meta_produced_no_mod = metadata_meter->produced_no_mod;
	/* First page number of consumed metadata */
	meta_consumed_page_mod = meta_consumed_no_mod /
				ZICIO_NUM_EXTENT_IN_PAGE;
	meta_consumed_page_mod &= ~ZICIO_METADATA_PAGE_NUM_MASK;
	/* First in-page offset of consumed metadata */
	meta_consumed_offset = meta_consumed_no_mod % ZICIO_NUM_EXTENT_IN_PAGE;

	/* Get the current first extent from buffer */
	current_extent = zicio_get_extent_in_buffer(
			desc->buffers.metadata_buffer, meta_consumed_page_mod,
					meta_consumed_offset);

	/* Checking current cursor is correctly specifying the metadata to read */
	cursor_direction = zicio_check_ext_current_metadata(current_extent,
				cur_lblock);

	/*
	 * When multiple requests are allocated, the location of metadata may not
	 * be matched. At this time, adjacent metadata is checked for repositioning.
	 */
	if (cursor_direction) {
		current_extent = zicio_adjust_metadata_cursor(desc, device_idx,
				meta_consumed_no_mod, meta_produced_no_mod, cur_lblock,
						cursor_direction);
	}

	nvme_cmd_lists = zicio_alloc_cmd_lists_set_with_desc(desc, device_idx);

	/* Set total block count per command and last block length */
	req_blk_cnt = per_req_blk_cnt * num_inner_device;
	last_blk_len = req_blk_cnt;

	/*
	 * Case 1. All command information included in the extent area.
	 *				  extent
	 * |====================================|
	 *			<---------------------->
	 *				   req_blk_cnt
	 *
	 * Create one command per device.
	 *
	 * Case 2. Includes all command information across multiple extents.
	 *      extent A		 extent B
	 * |===============||================|
	 *			<---------------------->
	 *				   req_blk_cnt
	 *
	 * More commands are created per device
	 */

	/*
	 * Main loop to set NVME command fields related with fs block 
	 */
	while (cur_meta_consumed < meta_produced_no_mod - meta_consumed_no_mod) {
		/* Code Segments to check if the start block of request is not 
		 * the start of cached extent. */
		file_lblock = le32_to_cpu(current_extent->ee_block);

		cur_block = le32_to_cpu(current_extent->ee_start_lo);
		cur_block |= ((zicio_ext4_fsblk_t) le16_to_cpu(
				current_extent->ee_start_hi) << 31) << 1;

		/* If the start block of request is not the same with the start
		 * block of extents */
		cur_len = min_t(unsigned, (file_lblock + le16_to_cpu(
				current_extent->ee_len) - cur_lblock), last_blk_len);
		cur_block = cur_block + cur_lblock - file_lblock;

		/* If every requested block are set to NVMe command */
		if (cur_len + cur_lblock >= start_lblock + tot_req_blk_cnt) {
			/* Get the length of last command */
			last_len = start_lblock + tot_req_blk_cnt - cur_lblock;
			/* Map block address to command */
			zicio_set_md_block_mapping_command(desc,
					nvme_cmd_lists, fd, device_idx, cur_lblock, cur_block,
					current_file_page_idx << ZICIO_CHUNK_ORDER,
					local_huge_page_idx, last_len, nvme_cmd_read, false);
			/* If blocks mapped by current extent is consumed all, then
			   increase one metadata */ 
			if (cur_len + cur_lblock ==
				file_lblock + le16_to_cpu(current_extent->ee_len)) {
				cur_meta_consumed++;
			}
			break;
		}

		if (cur_len + cur_lblock >= loc_start_lblock + req_blk_cnt) {
			/* If every block to read are in the current extent, then we can
			 * use all of mapping information from this extent. */
			cur_len = loc_start_lblock + req_blk_cnt - cur_lblock;
			loc_start_lblock = cur_lblock + cur_len;
			last_blk_len = req_blk_cnt;
		} else {
			/* Otherwise, there's possibilities that data can be located
			 * dispartly. So, What is left in the extent is read. */ 
			last_blk_len -= cur_len;
		}

		/* Map block address to command */
		zicio_set_md_block_mapping_command(desc, nvme_cmd_lists, fd,
				device_idx, cur_lblock, cur_block,
				current_file_page_idx << ZICIO_CHUNK_ORDER,
				local_huge_page_idx, cur_len, nvme_cmd_read, false);

		cur_lblock += cur_len;

		if (cur_lblock < file_lblock + le16_to_cpu(current_extent->ee_len)) {
			continue;
		}

		cur_meta_consumed++;
		if (meta_consumed_offset + 1 == ZICIO_NUM_EXTENT_IN_PAGE) {
			meta_consumed_offset = 0;
			if (meta_consumed_page_mod + 1 == ZICIO_METADATABUFFER_PAGENUM) {
				meta_consumed_page_mod = 0;
			}
			meta_consumed_page_mod++;
			current_extent = zicio_get_extent_in_buffer(
					desc->buffers.metadata_buffer, meta_consumed_page_mod,
					meta_consumed_offset);
		} else {
			meta_consumed_offset++;
			current_extent++;
		}
	}

	if (cursor_direction >= 0) {
		metadata_meter->consumed_no_mod += cur_meta_consumed;
	}

	if (meta_produced_no_mod - meta_consumed_no_mod - cur_meta_consumed < 
			ZICIO_MAX_EXTENT_IN_BUFFER >> 1) {
		*need_feed = ZICIO_MAX_EXTENT_IN_BUFFER >> 2;
	}

	/* Return the command */
	return nvme_cmd_lists;
}

/*
 * __zicio_preset_dma_mapping_for_md
 *
 * The decision on whether to use sgl or prp as the dma mapping method is made
 * in the nvme driver. This function is used to designate the memory address to
 * be mapped before entering the driver.
 */
static unsigned
__zicio_preset_dma_mapping_for_md(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmds, unsigned long buf_start,
			int num_inner_dev, unsigned start_offset, unsigned chunk_sects)
{
	unsigned alloced = 0, first_cmd_offset = 0, tmp;
	zicio_nvme_cmd_list *nvme_cmd = nvme_cmds;
	int io_size;
	int chunk_pages;

	/* If this command is for data, then we locate first location in DRAM */
	if (!nvme_cmds->is_metadata) {
		chunk_pages = chunk_sects >> ZICIO_PAGE_TO_SECTOR_SHIFT;
		first_cmd_offset = zicio_sector_div(tmp, nvme_cmds->start_fpos,
				chunk_pages * num_inner_dev);
	}

	/* To calculate the next location of DRAM, set the stride size */
	io_size = (nvme_cmd->next) ?
			(le16_to_cpu(nvme_cmd->next->cmd.rw.length) + 1) : 0;
	io_size >>= ZICIO_PAGE_TO_SECTOR_SHIFT;

	/* Set start memory address */
	while (nvme_cmd) {
		nvme_cmd->start_mem = buf_start + ((alloced + start_offset +
					first_cmd_offset) << ZICIO_PAGE_SHIFT);
		if (!nvme_cmds->is_metadata) {
			alloced += ((le16_to_cpu(nvme_cmd->cmd.rw.length) + 1) >>
					ZICIO_PAGE_TO_SECTOR_SHIFT) +
							io_size * (num_inner_dev - 1);
		} else {
			alloced += ((le16_to_cpu(nvme_cmd->cmd.rw.length) + 1) >>
					ZICIO_PAGE_TO_SECTOR_SHIFT);
		}
		nvme_cmd = nvme_cmd->next;
	}

	return alloced;
}

/*
 * __zicio_preset_dma_shared_mapping_for_md
 *
 * The decision on whether to use sgl or prp as the dma mapping method is made
 * in the nvme driver. This function is used to designate the memory address to
 * be mapped before entering the driver.
 */
static unsigned
__zicio_preset_dma_shared_mapping_for_md(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmds, unsigned long buf_start,
			int num_inner_dev, unsigned chunk_sects, bool is_on_track)
{
	unsigned alloced = 0, first_cmd_offset = 0, tmp;
	zicio_nvme_cmd_list *nvme_cmd = nvme_cmds;
	int io_size;
	int chunk_pages;

	/* If this command is for data, then we locate first location in DRAM */
	chunk_pages = chunk_sects >> ZICIO_PAGE_TO_SECTOR_SHIFT;
	first_cmd_offset = zicio_sector_div(tmp, nvme_cmds->start_fpos,
			chunk_pages * num_inner_dev);

	/* To calculate the next location of DRAM, set the stride size */
	io_size = (nvme_cmd->next) ?
			(le16_to_cpu(nvme_cmd->next->cmd.rw.length) + 1) : 0;
	io_size >>= ZICIO_PAGE_TO_SECTOR_SHIFT;

	/* Set start memory address */
	while (nvme_cmd) {
		nvme_cmd->start_mem = buf_start + ((alloced + first_cmd_offset)
				<< ZICIO_PAGE_SHIFT);
		nvme_cmd->is_on_track_cmd = is_on_track;
		alloced += ((le16_to_cpu(nvme_cmd->cmd.rw.length) + 1) >>
				ZICIO_PAGE_TO_SECTOR_SHIFT) + io_size * (num_inner_dev - 1);
		nvme_cmd = nvme_cmd->next;
	}

	return alloced;
}

/*
 * __zicio_preset_dma_mapping_for_nvme
 *
 * The decision on whether to use sgl or prp as the dma mapping method is made
 * in the nvme driver. This function is used to designate the memory address to
 * be mapped before entering the driver.
 */
static unsigned
__zicio_preset_dma_mapping_for_nvme(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmds, unsigned long buf_start)
{
	unsigned alloced = 0;
	zicio_nvme_cmd_list *nvme_cmd = nvme_cmds;

	/* Set start memory address */
	while (nvme_cmd) {
		nvme_cmd->start_mem = buf_start + (alloced << ZICIO_PAGE_SHIFT);
		alloced += (le16_to_cpu(nvme_cmd->cmd.rw.length) + 1) >>
					ZICIO_PAGE_TO_SECTOR_SHIFT;
		nvme_cmd = nvme_cmd->next;
	}

	return alloced;
}

/*
 * __zicio_preset_dma_shared_mapping_for_nvme
 *
 * The decision on whether to use sgl or prp as the dma mapping method is made
 * in the nvme driver. This function is used to designate the memory address to
 * be mapped before entering the driver.
 */
static unsigned
__zicio_preset_dma_shared_mapping_for_nvme(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmds, unsigned long buf_start,
			bool is_on_track)
{
	unsigned alloced = 0;
	zicio_nvme_cmd_list *nvme_cmd = nvme_cmds;

	/* Set start memory address */
	while (nvme_cmd) {
		nvme_cmd->start_mem = buf_start + (alloced << ZICIO_PAGE_SHIFT);
		nvme_cmd->is_on_track_cmd = is_on_track;
		alloced += (le16_to_cpu(nvme_cmd->cmd.rw.length) + 1) >>
					ZICIO_PAGE_TO_SECTOR_SHIFT;
		nvme_cmd = nvme_cmd->next;
	}

	return alloced;
}

/*
 * zicio_preset_dma_mapping_to_command_for_md
 *
 * Wrapper function to preset DMA mapping for MD device
 */
static unsigned
zicio_preset_dma_mapping_to_command_for_md(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmd, int num_inner_dev,
			unsigned start_offset, unsigned chunk_sects)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned int local_page_idx = nvme_cmd->local_huge_page_idx;
	unsigned long start_mem;

	/* If this command is for shared pool, than set spcb's chunk pointer to its
	   command */
	if (nvme_cmd->is_on_track_cmd) {
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		start_mem = (unsigned long)zicio_spcb->zicio_spcb.chunk_ptr;
	} else {
		start_mem = (unsigned long)desc->buffers.data_buffer[local_page_idx];
	}
	return __zicio_preset_dma_mapping_for_md(desc, nvme_cmd, start_mem,
				num_inner_dev, start_offset, chunk_sects);
}

/*
 * zicio_preset_dma_mapping_to_shared_command_for_md
 *
 * Wrapper function to preset DMA mapping for MD device
 */
static unsigned
zicio_preset_dma_mapping_to_shared_command_for_md(
			zicio_descriptor *desc, zicio_nvme_cmd_list *nvme_cmd,
			int num_inner_dev, unsigned chunk_sects, bool is_on_track)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned int local_page_idx = nvme_cmd->local_huge_page_idx;
	unsigned long start_mem;

	/* If this command is for shared pool, than set spcb's pointer
	   to its command */
	if (is_on_track) {
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		start_mem = (unsigned long)zicio_spcb->zicio_spcb.chunk_ptr;
	} else {
		start_mem = (unsigned long)desc->buffers.data_buffer[local_page_idx];
	}

	return __zicio_preset_dma_shared_mapping_for_md(desc, nvme_cmd,
				start_mem, num_inner_dev, chunk_sects, is_on_track);
}

/*
 * zicio_preset_dma_mapping_to_command_for_nvme
 *
 * Wrapper function to preset DMA mapping for NVMe device
 */
static void
zicio_preset_dma_mapping_to_command_for_nvme(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmd)
{
	unsigned int local_page_idx = nvme_cmd->local_huge_page_idx;
	unsigned long start_mem;

	start_mem = (unsigned long)desc->buffers.data_buffer[local_page_idx];
	__zicio_preset_dma_mapping_for_nvme(desc, nvme_cmd, start_mem);
}

/*
 * zicio_preset_dma_mapping_to_shared_command_for_nvme
 *
 * Wrapper function to preset DMA mapping for NVMe device
 */
static void
zicio_preset_dma_mapping_to_shared_command_for_nvme(
			zicio_descriptor *desc, zicio_nvme_cmd_list *nvme_cmd,
			bool is_on_track)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned int local_page_idx = nvme_cmd->local_huge_page_idx;
	unsigned long start_mem;

	if (is_on_track) {
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		start_mem = (unsigned long)zicio_spcb->zicio_spcb.chunk_ptr;
	} else {
		start_mem = (unsigned long)desc->buffers.data_buffer[local_page_idx];
	}

	__zicio_preset_dma_shared_mapping_for_nvme(desc, nvme_cmd, start_mem,
			is_on_track);
}

/*
 * zicio_preset_dma_mapping_to_metadata_command_for_md
 *
 * Wrapper function to preset DMA mapping to metadata command for MD device
 */
static void
zicio_preset_dma_mapping_to_metadata_command_for_md(
			zicio_descriptor *desc, zicio_nvme_cmd_list *nvme_cmd)
{
	unsigned int local_page_idx = nvme_cmd->local_huge_page_idx;

	__zicio_preset_dma_mapping_for_md(desc, nvme_cmd,
				(unsigned long)desc->metadata_ctrl.inode_buffer +
				(local_page_idx << ZICIO_PAGE_SHIFT), 0, 0, 0);
}

/*
 * zicio_preset_dma_mapping_to_metadata_command_for_nvme
 *
 * Wrapper function to prese DMA mapping to metadata command for NVMe device
 */
static void
zicio_preset_dma_mapping_to_metadata_command_for_nvme(
			zicio_descriptor *desc, zicio_nvme_cmd_list *nvme_cmd)
{
	unsigned int local_page_idx = nvme_cmd->local_huge_page_idx;

	__zicio_preset_dma_mapping_for_nvme(desc, nvme_cmd,
				(unsigned long)desc->metadata_ctrl.inode_buffer +
				(local_page_idx << ZICIO_PAGE_SHIFT));
}

/*
 * zicio_preset_dma_mapping_for_md
 *
 * Entry point to preset DMA mapping for MD
 */
static void
zicio_preset_dma_mapping_for_md(zicio_descriptor *desc,
			zicio_nvme_cmd_list **start_cmd_lists, int mddev_idx)
{
	zicio_device *zicio_device = zicio_get_zicio_device_with_desc(desc,
			mddev_idx);
	zicio_md_private_data *zicio_md_data = zicio_get_md_private_data(
			zicio_device);
	struct mddev *mddev = zicio_md_data->mddev;
	int num_inner_dev = zicio_get_num_inner_device(zicio_device), dev_idx;
	unsigned start_offset = 0;
	unsigned chunk_sects = mddev->chunk_sectors;

	/*
	 * Create as many command lists as there are devices. And while traversing
	 * this list, it preset DMA map.
	 */
	for (dev_idx = 0 ; dev_idx < num_inner_dev ; dev_idx++) {
		if (start_cmd_lists[dev_idx]) {
			if (start_cmd_lists[dev_idx]->is_metadata) {
				zicio_preset_dma_mapping_to_metadata_command_for_md(desc,
						start_cmd_lists[dev_idx]);
			} else {
				zicio_preset_dma_mapping_to_command_for_md(desc,
						start_cmd_lists[dev_idx], num_inner_dev, start_offset,
							chunk_sects);
			}
		}
	}
}

/*
 * zicio_preset_dma_mapping_for_md_shared
 *
 * Entry point to preset DMA mapping for MD
 */
static void
zicio_preset_dma_mapping_for_md_shared(zicio_descriptor *desc,
		zicio_nvme_cmd_list **start_cmd_lists, int mddev_idx,
		bool is_on_track)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_device *zicio_device = zicio_get_zicio_device_with_shared_pool(
			zicio_shared_pool, mddev_idx);
	zicio_md_private_data *zicio_md_data = zicio_get_md_private_data(
			zicio_device);
	struct mddev *mddev = zicio_md_data->mddev;
	int num_inner_dev = zicio_get_num_inner_device(zicio_device), dev_idx;
	unsigned chunk_sects = mddev->chunk_sectors;

	/*
	 * Create as many command lists as there are devices. And while traversing
	 * this list, it preset DMA map.
	 */
	for (dev_idx = 0 ; dev_idx < num_inner_dev ; dev_idx++) {
		if (start_cmd_lists[dev_idx]) {
			zicio_preset_dma_mapping_to_shared_command_for_md(desc,
					start_cmd_lists[dev_idx], num_inner_dev, chunk_sects,
							is_on_track);
		}
	}
}

/*
 * zicio_preset_dma_mapping_for_nvme_shared
 *
 * Entry point to preset DMA mapping for NVMe
 */
static void
zicio_preset_dma_mapping_for_nvme_shared(zicio_descriptor *desc,
		zicio_nvme_cmd_list *nvme_cmd_list, bool is_on_track)
{
	/*
	 * While traversing a command list, it preset DMA map.
	 */
	zicio_preset_dma_mapping_to_shared_command_for_nvme(
			desc, nvme_cmd_list, is_on_track);
}

/*
 * zicio_preset_dma_mapping_for_nvme
 *
 * Entry point to preset DMA mapping for NVMe
 */
static void
zicio_preset_dma_mapping_for_nvme(zicio_descriptor *desc,
		zicio_nvme_cmd_list *nvme_cmd_list)
{
	/*
	 * While traversing a command list, it preset DMA map.
	 */
	if (nvme_cmd_list->is_metadata) {
		zicio_preset_dma_mapping_to_metadata_command_for_nvme(desc,
				nvme_cmd_list);
	} else {
		zicio_preset_dma_mapping_to_command_for_nvme(desc, nvme_cmd_list);
	}
}

/*
 * zicio_set_metadata_block_mapping
 *
 * set block mapping for extent read
 */
zicio_nvme_cmd_list *
zicio_set_metadata_block_mapping(zicio_descriptor *desc,
			unsigned req_amount, zicio_nvme_cmd_list **last,
			zicio_file_struct *zicio_file)
{
	struct fd fd;
	struct zicio_ext4_extent_idx *index_extent_to_read;
	zicio_ext4_fsblk_t cur_lba;
	zicio_nvme_cmd_list *meta_cmd_list = NULL, *meta_cmd;
	zicio_meter *extent_tree_meter;
	zicio_chunk_bitmap_meter *inode_meter;
	void *extent_tree_buffer;
	sector_t bd_start_sector;
	unsigned found_lblock, prev_lblock;
	unsigned cur_rest = req_amount;
	unsigned consumed_no_mod, produced_no_mod;
	unsigned i;


	/* If We consume all index extents of current file, then attempts to perform
	 * I/O on the extent of the next file */
	if (zicio_check_all_extent_consumed(zicio_file))  {
		if (!(zicio_file = zicio_get_next_file_struct(&desc->read_files))) {
			return NULL;
		}

		/* If the next file also consumes all the data, no additional extent
		 * feed is currently performed. */
		if (zicio_check_all_extent_consumed(zicio_file)) {
			return NULL;
		}

		/* If the next file is caching an extent other than the index extent,
		 * the corresponding data is fetched immediately. */
		if (!zicio_file->has_index_extent) {
			zicio_feed_metadata_buffers(desc, zicio_file);
			return NULL;
		}
	}

	fd = zicio_file->fd;
	/* Get the offset of partition where the file is located in block device */
	bd_start_sector = zicio_get_block_partition_remap_offset(fd);

	/* Get next file extent and produce its extent */
	extent_tree_meter = &zicio_file->extent_tree_meter;
	consumed_no_mod = extent_tree_meter->consumed_no_mod;
	produced_no_mod = extent_tree_meter->produced_no_mod;

	inode_meter = &desc->metadata_ctrl.inode_meter;

	/* Cannot be suppplied extent data when the inode buffer is full */
	if (inode_meter->consumed_no_mod + ZICIO_INODE_BUFFER_BITS - 1 ==
		zicio_get_produced_inode_bitmap(inode_meter,
		inode_meter->consumed_no_mod)) {
		return NULL;
	}

	extent_tree_buffer = zicio_file->extent_tree_buffer;
	index_extent_to_read =
			(struct zicio_ext4_extent_idx *)extent_tree_buffer +
					consumed_no_mod;

	cur_lba = le32_to_cpu(index_extent_to_read->ei_leaf_lo);
	cur_lba |= ((zicio_ext4_fsblk_t)
				le16_to_cpu(index_extent_to_read->ei_leaf_hi) << 31) << 1;
	prev_lblock = found_lblock = le32_to_cpu(index_extent_to_read->ei_block);
	index_extent_to_read++;

	/* Set the location on the block device where the extent to be read is
	 * located. */
	for (i = 1 ; i < produced_no_mod - consumed_no_mod ;
				i++, index_extent_to_read++) {
		prev_lblock = found_lblock;
		found_lblock = le32_to_cpu(index_extent_to_read->ei_block);

		if (found_lblock - prev_lblock > cur_rest) {
			break;
		}

		zicio_allocate_nvme_cmd(&meta_cmd_list, &meta_cmd);
		extent_tree_meter->consumed_no_mod++;

		zicio_initialize_nvme_cmd(meta_cmd, zicio_file->device_idx_in_channel,
				fd, prev_lblock + 1, cur_lba, UINT_MAX,
				inode_meter->requested_no_mod++ & (~ZICIO_INODEBUFFER_MASK),
				true);

		/* Set the command for block mapping */
		zicio_set_nvme_block_mapping_command(meta_cmd, nvme_cmd_read,
				cur_lba, bd_start_sector, 1);

		cur_lba = le32_to_cpu(index_extent_to_read->ei_leaf_lo);
		cur_lba |= ((zicio_ext4_fsblk_t)
					le16_to_cpu(index_extent_to_read->ei_leaf_hi) << 31) << 1;

		cur_rest -= (found_lblock - prev_lblock);
	}

	zicio_allocate_nvme_cmd(&meta_cmd_list, &meta_cmd);
	extent_tree_meter->consumed_no_mod++;

	zicio_initialize_nvme_cmd(meta_cmd, zicio_file->device_idx_in_channel, fd,
			prev_lblock + 1, cur_lba, UINT_MAX,
			inode_meter->requested_no_mod++ & (~ZICIO_INODEBUFFER_MASK),
			true);

	/* Set the command for block mapping */
	zicio_set_nvme_block_mapping_command(meta_cmd, nvme_cmd_read,
			cur_lba, bd_start_sector, 1);

	*last = meta_cmd;

	return meta_cmd_list;
}

/*
 * zicio_set_metadata_block_mapping_for_md
 *
 * set block mapping for extent read
 */
static void
zicio_set_metadata_block_mapping_for_md(zicio_descriptor *desc,
			unsigned req_amount, zicio_nvme_cmd_list **last_meta_cmds,
			zicio_file_struct *zicio_file, int num_inner_device)
{
	struct fd fd;
	struct zicio_ext4_extent_idx *index_extent_to_read;
	zicio_ext4_fsblk_t cur_fpos;
	zicio_meter *extent_tree_meter;
	zicio_chunk_bitmap_meter *inode_meter;
	void *extent_tree_buffer;
	unsigned found_lblock, prev_lblock;
	unsigned cur_rest = req_amount;
	unsigned consumed_no_mod, produced_no_mod;
	unsigned i;

	/* If We consume all index extents of current file, then attempts to perform
	 * I/O on the extent of the next file */
	if (zicio_check_all_extent_consumed(zicio_file)) {
		if (!(zicio_file = zicio_get_next_file_struct(&desc->read_files))) {
			return;
		}

		/* If the next file also consumes all the data, no additional extent
		 * feed is currently performed. */
		if (zicio_check_all_extent_consumed(zicio_file)) {
			return;
		}

		/* If the next file is caching an extent other than the index extent,
		 * the corresponding data is fetched immediately. */
		if (!zicio_file->has_index_extent) {
			zicio_feed_metadata_buffers(desc, zicio_file);
			return;
		}
	}

	/* Get file descriptor and extent tree meter for reading extents. */
	fd = zicio_file->fd;
	extent_tree_meter = &zicio_file->extent_tree_meter;
	consumed_no_mod = extent_tree_meter->consumed_no_mod;
	produced_no_mod = extent_tree_meter->produced_no_mod;
	
	/* Get inode meter to set memory location for DRAM. */
	inode_meter = &desc->metadata_ctrl.inode_meter;

	/* We cannot supply extent from device when the inode buffer is full. */
	if (inode_meter->consumed_no_mod + ZICIO_INODE_BUFFER_BITS - 1 ==
				zicio_get_produced_inode_bitmap(inode_meter,
						inode_meter->consumed_no_mod)) {
		return;
	}

	/* Set the index extent to read. */
	extent_tree_buffer = zicio_file->extent_tree_buffer;
	index_extent_to_read =
			(struct zicio_ext4_extent_idx *)extent_tree_buffer +
					consumed_no_mod;

	/* Calculate the file's position from index extent. */
	cur_fpos = le32_to_cpu(index_extent_to_read->ei_leaf_lo);
	cur_fpos |= (((zicio_ext4_fsblk_t)
				le16_to_cpu(index_extent_to_read->ei_leaf_hi) << 31) << 1);
	/* Get the start file position of data */
	prev_lblock = found_lblock = le32_to_cpu(index_extent_to_read->ei_block);
	index_extent_to_read++;

	/* Set the location on the block device where the extent to be read is
	 * located. */
	for (i = 1 ; i < produced_no_mod - consumed_no_mod ;
				i++, index_extent_to_read++) {
		prev_lblock = found_lblock;
		found_lblock = le32_to_cpu(index_extent_to_read->ei_block);

		if (found_lblock - prev_lblock > cur_rest) {
			break;
		}

		/* Map block address to command */
		zicio_set_md_block_mapping_command(desc, last_meta_cmds, fd,
				zicio_file->device_idx_in_channel, prev_lblock + 1, cur_fpos,
				UINT_MAX,
				inode_meter->requested_no_mod++ & (~ZICIO_INODEBUFFER_MASK), 
				1, nvme_cmd_read, true);

		extent_tree_meter->consumed_no_mod++;

		/* Calculate the file's position from index extent. */
		cur_fpos = le32_to_cpu(index_extent_to_read->ei_leaf_lo);
		cur_fpos |= ((zicio_ext4_fsblk_t)
					le16_to_cpu(index_extent_to_read->ei_leaf_hi) << 31) << 1;

		cur_rest -= (found_lblock - prev_lblock);
	}

	/* Map block address to command */
	zicio_set_md_block_mapping_command(desc, last_meta_cmds, fd,
			zicio_file->device_idx_in_channel, prev_lblock + 1, cur_fpos, UINT_MAX,
			inode_meter->requested_no_mod++ & (~ZICIO_INODEBUFFER_MASK), 
			1, nvme_cmd_read, true);
	extent_tree_meter->consumed_no_mod++;
}

/*
 * __zicio_create_nvme_command_shared
 *
 * NVMe command creation function for a channel that operates by attaching to 
 * a shared pool
 */
static zicio_nvme_cmd_list *
__zicio_create_nvme_command_shared(zicio_descriptor *desc,
			zicio_nvme_cmd_list **start_cmd_lists,
			zicio_file_struct *zicio_file, int current_file_idx,
			ssize_t per_cmd_size, unsigned long file_chunk_idx,
			int local_page_idx, bool is_on_track)
{
	/* zicio nvme command descriptor to return */
	zicio_nvme_cmd_list *nvme_cmds;

	/* shared pool and shared pool info for channel */
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;

	/* Request counts */
	int per_req_cnt = per_cmd_size >> 2;
	int tot_req_cnt = ZICIO_CHUNK_SIZE >> ZICIO_PAGE_SHIFT;

	int device_idx;
	struct fd fd;
	unsigned int in_file_chunk_idx;
	unsigned long file_size, requested_size;

	/* The size of cmd should be larger than one page size */
	BUG_ON(per_req_cnt == 0);

    if (file_chunk_idx == UINT_MAX) {
        /* We consumed all of required file chunks. */
        return NULL;
    }

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	BUG_ON(file_chunk_idx >= zicio_shared_pool->shared_files.total_chunk_nums);

	/* Set device idx */
	device_idx = zicio_file->device_idx_in_channel;
	/* Set file Descriptor */
	fd = zicio_file->fd;
	/* Get the total file size of total chunk number */
	file_size = zicio_shared_pool->shared_files.total_chunk_nums;
	/* Get the requested chunk numbers */
	requested_size = atomic_read(&zicio_shared_pool_local->num_mapped);

	/* in-file chunk index */
	in_file_chunk_idx = zicio_get_in_file_chunk_id(zicio_shared_pool,
				zicio_shared_pool_local, current_file_idx, file_chunk_idx);
	/* Last chunk couldn't be aligned to 2MB, set total count of command
	 * considering it */
	tot_req_cnt = zicio_get_current_tot_req_cnt_shared(zicio_shared_pool,
				zicio_shared_pool_local, zicio_file, in_file_chunk_idx, tot_req_cnt);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] [ZICIO] Request : %ld(%d/%ld)%%\n",
			desc->cpu_id, 100 * atomic_read(&zicio_shared_pool_local->num_mapped)
			/ file_size, atomic_read(&zicio_shared_pool_local->num_mapped),
			file_size);
#endif /* CONFIG_ZICIO_DEBUG */

	/* Set block mapping information for shared pool */
	nvme_cmds = zicio_set_block_mapping_shared_for_nvme(desc,
				start_cmd_lists, device_idx, fd, current_file_idx,
				in_file_chunk_idx << ZICIO_PAGE_TO_CHUNK_SHIFT, tot_req_cnt,
				per_req_cnt, file_chunk_idx, local_page_idx);

	/*
	 * If there is an assigned command, set the memory information for dma
	 * mapping to the command.
	 */
	if (nvme_cmds) {
		zicio_preset_dma_mapping_for_nvme_shared(desc, nvme_cmds,
				is_on_track);
	}

	return nvme_cmds;
}

/*
 * zicio_create_nvme_command_shared
 *
 * NVMe command creation function for a channel that operates by attaching to 
 * a shared pool
 */
static void *
zicio_create_nvme_command_shared(zicio_descriptor *desc,
			zicio_file_struct *zicio_file, int current_file_idx,
			ssize_t per_cmd_size, unsigned long file_chunk_idx,
			int local_page_idx, bool is_on_track)
{
	zicio_nvme_cmd_list **start_cmd_lists;

	start_cmd_lists = zicio_alloc_cmd_lists_set_with_desc(desc,
				zicio_file->device_idx_in_channel);
	/*
	 * Create nvme commands for shared pool.
	 */
	start_cmd_lists[0] = __zicio_create_nvme_command_shared(desc,
			start_cmd_lists, zicio_file, current_file_idx, per_cmd_size,
			file_chunk_idx, local_page_idx, is_on_track);

	if (!start_cmd_lists[0]) {
		zicio_free_cmd_lists_set_with_desc(desc,
					zicio_file->device_idx_in_channel, start_cmd_lists);
		return NULL;
	}

	return start_cmd_lists;
}

/*
 * __zicio_set_dma_mapping_to_shared_command_for_nvme
 *
 * Set dma mapping to command
 */
static void
__zicio_set_dma_mapping_to_shared_command_for_nvme(dma_addr_t *prp_addr,
			void *prp_data, struct nvme_command *nvme_cmds,
			unsigned prp_off, unsigned alloc, short int flags)
{
	dma_addr_t dev_data;
	dma_addr_t *first_dma_addr;
	unsigned block_len = (alloc + 1) << ZICIO_NVME_SECTOR_SHIFT;
	unsigned prp_off_len;

	BUG_ON(prp_off >= ZICIO_NUM_PRP_ENTRIES_IN_PAGE);

	prp_off_len = prp_off * sizeof(dma_addr_t);
	first_dma_addr = (dma_addr_t *)((char*)prp_data + prp_off_len);
	dev_data = *first_dma_addr;

	nvme_cmds->rw.flags = flags;

	if (flags == 0) {
		/*
		 * If it is difficult to use the SGL currently, perform the following
		 * settings.
		 */
		if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE) {
			nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
			nvme_cmds->rw.dptr.prp2 = 0;
		} else if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE * 2) {
			nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
			dev_data = *(first_dma_addr + 1);
			nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
		} else {
			nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
			dev_data = *prp_addr + (prp_off + 1) * sizeof(dma_addr_t);
			nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
		}
	} else if (flags & NVME_CMD_SGL_METABUF) {
		/*
		 * If mapping using sgl is required, set the following values.
		 */
		nvme_cmds->rw.dptr.sgl.addr = cpu_to_le64(dev_data);
		nvme_cmds->rw.dptr.sgl.length = block_len;
		nvme_cmds->rw.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;
	} else {
		printk(KERN_WARNING "Error Unexpected DMA flags\n");
		BUG_ON(true);
	}
}



/*
 * __zicio_set_dma_mapping_to_command
 *
 * Set dma mapping to command
 */
static void
__zicio_set_dma_mapping_to_command_for_nvme(dma_addr_t *prp_addr,
			void *prp_data, struct nvme_command *nvme_cmds,
			unsigned mem_produced, unsigned alloc, short int flags)
{
	dma_addr_t dev_data;
	dma_addr_t *first_dma_addr;
	unsigned long sprp = mem_produced / ZICIO_NUM_PRP_ENTRIES_IN_PAGE;
	unsigned long prp_off = mem_produced % ZICIO_NUM_PRP_ENTRIES_IN_PAGE;
	unsigned block_len = (alloc + 1) << ZICIO_NVME_SECTOR_SHIFT;
	unsigned prp_off_len;

	prp_off_len = prp_off * sizeof(dma_addr_t);
	first_dma_addr = (dma_addr_t *)((char*)prp_data +
				ZICIO_PAGE_SIZE * sprp + prp_off_len);
	dev_data = *first_dma_addr;

	nvme_cmds->rw.flags = flags;

	if (flags == 0) {
		/*
		 * If it is difficult to use the SGL currently, perform the following
		 * settings.
		 */
		if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE) {
			nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
			nvme_cmds->rw.dptr.prp2 = 0;
		} else if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE * 2) {
			nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
			dev_data = *(first_dma_addr + 1);
			nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
		} else {
			nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
			dev_data = prp_addr[sprp] + (prp_off + 1)* sizeof(dma_addr_t);
			nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
		}
	} else if (flags & NVME_CMD_SGL_METABUF) {
		/*
		 * If mapping using sgl is required, set the following values.
		 */
		nvme_cmds->rw.dptr.sgl.addr = cpu_to_le64(dev_data);
		nvme_cmds->rw.dptr.sgl.length = block_len;
		nvme_cmds->rw.dptr.sgl.type = NVME_SGL_FMT_DATA_DESC << 4;
	} else {
		printk(KERN_WARNING "Error Unexpected DMA flags\n");
		BUG_ON(true);
	}
}

/*
 * __zicio_set_dma_prp_mapping_to_command_for_md
 *
 * Set dma mapping to command
 */
static void
__zicio_set_dma_prp_mapping_to_command_for_md(dma_addr_t *prp_addr,
			void *prp_data, struct nvme_command *nvme_cmds,
			unsigned mem_produced, unsigned alloc, unsigned int stride,
			unsigned int in_page_offset, short int flags)
{
	dma_addr_t dev_data;
	dma_addr_t *first_dma_addr;
	unsigned long sprp =
			mem_produced / (ZICIO_NUM_PRP_ENTRIES_IN_PAGE * stride);
	unsigned long prp_chunk_off;
	unsigned long prp_off = (mem_produced % ZICIO_NUM_PRP_ENTRIES_IN_PAGE) /
			stride;
	unsigned block_len = (alloc + 1) << ZICIO_NVME_SECTOR_SHIFT;
	unsigned prp_off_len;

	prp_chunk_off = mem_produced % (ZICIO_NUM_PRP_ENTRIES_IN_PAGE * stride);
	prp_chunk_off /= (ZICIO_NUM_PRP_ENTRIES_IN_PAGE);

	prp_off += in_page_offset;
	prp_off += prp_chunk_off * (ZICIO_NUM_PRP_ENTRIES_IN_PAGE / stride);
	prp_off_len = prp_off * sizeof(dma_addr_t);

	first_dma_addr = (dma_addr_t *)((char*)prp_data +
				ZICIO_PAGE_SIZE * sprp + prp_off_len);
	dev_data = *first_dma_addr;

	nvme_cmds->rw.flags = flags;

	/*
	 * If it is difficult to use the SGL currently, perform the following
	 * settings.
	 */
	if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE) {
		nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
		nvme_cmds->rw.dptr.prp2 = 0;
	} else if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE * 2) {
		nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
		dev_data = *(first_dma_addr + 1);
		nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
	} else {
		nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
		dev_data = prp_addr[sprp] + (prp_off + 1) * sizeof(dma_addr_t);
		nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
	}
}

/*
 * __zicio_set_dma_prp_mapping_to_shared_command_for_md
 *
 * Set dma mapping to command
 */
static void
__zicio_set_dma_prp_mapping_to_shared_command_for_md(dma_addr_t *prp_addr,
			void *prp_data, struct nvme_command *nvme_cmds,
			unsigned mem_produced, unsigned alloc, unsigned int stride,
			unsigned int in_page_offset, short int flags)
{
	dma_addr_t dev_data;
	dma_addr_t *first_dma_addr;
	unsigned long prp_off = mem_produced / stride;
	unsigned block_len = (alloc + 1) << ZICIO_NVME_SECTOR_SHIFT;
	unsigned prp_off_len;

	BUG_ON(mem_produced >= (ZICIO_NUM_PRP_ENTRIES_IN_PAGE * stride));

	prp_off += in_page_offset;
	prp_off_len = prp_off * sizeof(dma_addr_t);

	first_dma_addr = (dma_addr_t *)((char*)prp_data + prp_off_len);
	dev_data = *first_dma_addr;

	nvme_cmds->rw.flags = flags;

	/*
	 * If it is difficult to use the SGL currently, perform the following
	 * settings.
	 */
	if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE) {
		nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
		nvme_cmds->rw.dptr.prp2 = 0;
	} else if (block_len <= ZICIO_NVME_CTRL_PAGE_SIZE * 2) {
		nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
		dev_data = *(first_dma_addr + 1);
		nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
	} else {
		nvme_cmds->rw.dptr.prp1 = cpu_to_le64(dev_data);
		dev_data = *prp_addr + (prp_off + 1) * sizeof(dma_addr_t);
		nvme_cmds->rw.dptr.prp2 = cpu_to_le64(dev_data);
	}
}

/*
 * __zicio_set_dma_sgl_mapping_to_command_for_md
 *
 * Set dma mapping to command
 */
static void
__zicio_set_dma_sgl_mapping_to_command_for_md(dma_addr_t *sgl_addr,
			void *sgl_data, struct nvme_command *nvme_cmds,
			unsigned mem_produced, unsigned alloc, unsigned int stride,
			unsigned int in_page_offset, short int flags)
{
	dma_addr_t map_data;
	unsigned long sgl_page =
			mem_produced / (ZICIO_NUM_SGL_ENTRIES_IN_PAGE * stride);
	unsigned long sgl_chunk_off;
	unsigned long sgl_off = ((mem_produced % ZICIO_NUM_SGL_ENTRIES_IN_PAGE) /
			stride) + in_page_offset;
	unsigned num_pages = (alloc + 1) >> ZICIO_PAGE_TO_SECTOR_SHIFT;
	unsigned sgl_off_len;

	sgl_chunk_off = mem_produced % (ZICIO_NUM_SGL_ENTRIES_IN_PAGE * stride);
	sgl_chunk_off /= (ZICIO_NUM_SGL_ENTRIES_IN_PAGE);

	sgl_off += sgl_chunk_off * (ZICIO_NUM_SGL_ENTRIES_IN_PAGE / stride);
	sgl_off_len = sgl_off * sizeof(struct nvme_sgl_desc);
	map_data = sgl_addr[sgl_page] + sgl_off_len;

	nvme_cmds->rw.flags = flags;

	/*
	 * If mapping using sgl is required, set the following values.
	 */
	nvme_cmds->rw.dptr.sgl.addr = cpu_to_le64(map_data);
	nvme_cmds->rw.dptr.sgl.length =
				cpu_to_le32(num_pages * sizeof(struct nvme_sgl_desc));
	nvme_cmds->rw.dptr.sgl.type = NVME_SGL_FMT_LAST_SEG_DESC << 4;
}

/*
 * __zicio_set_dma_sgl_mapping_to_shared_command_for_md
 *
 * Set dma mapping to command
 */
static void
__zicio_set_dma_sgl_mapping_to_shared_command_for_md(dma_addr_t *sgl_addr,
			void *sgl_data, struct nvme_command *nvme_cmds,
			unsigned mem_produced, unsigned alloc, unsigned int stride,
			unsigned int in_page_offset, short int flags)
{
	dma_addr_t map_data;
	unsigned long sgl_off = in_page_offset + mem_produced / stride;
	unsigned num_pages = (alloc + 1) >> ZICIO_PAGE_TO_SECTOR_SHIFT;
	unsigned sgl_off_len;

	BUG_ON(mem_produced >= (ZICIO_NUM_SGL_ENTRIES_IN_PAGE * stride));

	sgl_off_len = sgl_off * sizeof(struct nvme_sgl_desc);

	map_data = *sgl_addr + sgl_off_len;
	nvme_cmds->rw.flags = flags;

	/*
	 * If mapping using sgl is required, set the following values.
	 */
	nvme_cmds->rw.dptr.sgl.addr = cpu_to_le64(map_data);
	nvme_cmds->rw.dptr.sgl.length =
				cpu_to_le32(num_pages * sizeof(struct nvme_sgl_desc));
	nvme_cmds->rw.dptr.sgl.type = NVME_SGL_FMT_LAST_SEG_DESC << 4;
}

static void
__zicio_set_dma_mapping_for_nvme(zicio_descriptor *desc,
			dma_addr_t *prp_addr, void *prp_data, unsigned long local_page_idx,
			unsigned pagenum_in_buffer, zicio_nvme_cmd_list *nvme_cmd,
			unsigned long buf_start, short int flags)
{
	unsigned long in_buffer_page_offset, page_start;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[cpu %d] __zicio_set_dma_mapping_for_nvme()\n",
			desc->cpu_id);
#endif

	if (nvme_cmd->is_metadata) {
		BUG_ON(buf_start !=
				(unsigned long)desc->metadata_ctrl.inode_buffer +
				(local_page_idx << ZICIO_PAGE_SHIFT));
		in_buffer_page_offset = local_page_idx;
	} else {
		page_start = (unsigned long)desc->buffers.data_buffer[local_page_idx];

		BUG_ON(buf_start < page_start);
		in_buffer_page_offset = ZICIO_START_PAGENUM_IN_CHUNK(local_page_idx)
			+ ((buf_start - page_start) >> ZICIO_PAGE_SHIFT);
	}

	/*
	 * Performs DMA setting in the command using the calculated location
	 * information.
	 */
	__zicio_set_dma_mapping_to_command_for_nvme(prp_addr, prp_data,
				&nvme_cmd->cmd, in_buffer_page_offset,
				le16_to_cpu(nvme_cmd->cmd.rw.length), flags);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[cpu %d] __zicio_set_dma_mapping_to_command_for_nvme()\n",
			desc->cpu_id);
#endif
}

static void
__zicio_set_dma_mapping_for_nvme_shared(zicio_descriptor *desc,
			dma_addr_t *prp_addr, void *prp_data, unsigned long local_page_idx,
			zicio_nvme_cmd_list *nvme_cmd, unsigned long buf_start,
			short int flags)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned long in_buffer_page_offset, page_start;

	if (nvme_cmd->is_on_track_cmd) {
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		page_start = (unsigned long)zicio_spcb->zicio_spcb.chunk_ptr;
	} else {
		page_start = (unsigned long)desc->buffers.data_buffer[local_page_idx];
	}

	BUG_ON((buf_start < page_start) || (buf_start >=
			page_start + ZICIO_DATABUFFER_CHUNK_SIZE));

	in_buffer_page_offset = ((buf_start - page_start) >> ZICIO_PAGE_SHIFT);

	/*
	 * Performs DMA setting in the command using the calculated location
	 * information.
	 */
	__zicio_set_dma_mapping_to_shared_command_for_nvme(prp_addr, prp_data,
			&nvme_cmd->cmd, in_buffer_page_offset,
			le16_to_cpu(nvme_cmd->cmd.rw.length), flags);
}

static void
__zicio_set_dma_prp_mapping_for_md(zicio_descriptor *desc,
			dma_addr_t *prp_addr, void *prp_data, int local_page_idx,
			zicio_nvme_cmd_list *nvme_cmd, unsigned long buf_start,
			unsigned int stride, int in_page_offset, short int flags)
{
	unsigned long in_buffer_page_offset, page_start;

	if (nvme_cmd->is_metadata) {
		BUG_ON(buf_start !=
				(unsigned long)desc->metadata_ctrl.inode_buffer +
				(local_page_idx << ZICIO_PAGE_SHIFT));
		in_buffer_page_offset = local_page_idx;
		/*
		 * Performs DMA setting in the command using the calculated location
		 * information.
		 */
		__zicio_set_dma_mapping_to_command_for_nvme(prp_addr, prp_data,
				&nvme_cmd->cmd, in_buffer_page_offset,
				le16_to_cpu(nvme_cmd->cmd.rw.length), flags);
	} else {
		page_start = (unsigned long)desc->buffers.data_buffer[local_page_idx];
		BUG_ON(buf_start < page_start);
		BUG_ON(buf_start > page_start + ZICIO_CHUNK_SIZE);

		in_buffer_page_offset = ZICIO_START_PAGENUM_IN_CHUNK(local_page_idx)
				+ ((buf_start - page_start) >> (ZICIO_PAGE_SHIFT));
		/*
		 * Performs DMA setting in the command using the calculated location
		 * information.
		 */
		__zicio_set_dma_prp_mapping_to_command_for_md(prp_addr, prp_data,
				&nvme_cmd->cmd, in_buffer_page_offset, le16_to_cpu(
				nvme_cmd->cmd.rw.length), stride, in_page_offset, flags);
	}
}

static void
__zicio_set_dma_prp_mapping_for_md_shared(zicio_descriptor *desc,
		dma_addr_t *prp_addr, void *prp_data, int local_page_idx,
		zicio_nvme_cmd_list *nvme_cmd, unsigned long buf_start,
		unsigned int stride, int in_page_offset, short int flags)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned long in_buffer_page_offset, page_start;

	/* Get spcb from shared pool */
	zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);

	/* Get the start point of target huge page */
	page_start = (unsigned long)zicio_spcb->zicio_spcb.chunk_ptr;

	BUG_ON(buf_start < page_start);
	BUG_ON(buf_start > page_start + ZICIO_CHUNK_SIZE);
	/* Get the in-page offset to NVMe command setting. */
	in_buffer_page_offset = (buf_start - page_start) >> ZICIO_PAGE_SHIFT;
	/*
	 * Performs DMA setting in the command using the calculated location
	 * information.
	 */
	__zicio_set_dma_prp_mapping_to_shared_command_for_md(prp_addr, prp_data,
			&nvme_cmd->cmd, in_buffer_page_offset, le16_to_cpu(
					nvme_cmd->cmd.rw.length), stride, in_page_offset, flags);
}

/*
 * __zicio_set_dma_sgl_mapping_for_md
 *
 * Set DMA mapping through sgl descriptor array for striped device.
 */
static void
__zicio_set_dma_sgl_mapping_for_md(zicio_descriptor *desc,
			dma_addr_t *sgl_addr, void *sgl_data, int local_page_idx,
			zicio_nvme_cmd_list *nvme_cmd, unsigned long buf_start,
			unsigned int stride, unsigned int in_page_offset, short int flags)
{
	unsigned long in_buffer_page_offset, page_start;

	if (nvme_cmd->is_metadata) {
		BUG_ON(buf_start !=
				(unsigned long)desc->metadata_ctrl.inode_buffer +
				(local_page_idx << ZICIO_PAGE_SHIFT));
		in_buffer_page_offset = local_page_idx;
		/*
		 * Performs DMA setting in the command using the calculated location
		 * information.
		 */
		__zicio_set_dma_mapping_to_command_for_nvme(sgl_addr, sgl_data,
				&nvme_cmd->cmd, in_buffer_page_offset,
				le16_to_cpu(nvme_cmd->cmd.rw.length), flags);
	} else {
		page_start = (unsigned long)desc->buffers.data_buffer[local_page_idx];

		BUG_ON(buf_start < page_start);
		BUG_ON(buf_start > page_start + ZICIO_CHUNK_SIZE);
		in_buffer_page_offset = ZICIO_START_PAGENUM_IN_CHUNK(local_page_idx)
				+ ((buf_start - page_start) >> ZICIO_PAGE_SHIFT);
		/*
		 * Performs DMA setting in the command using the calculated location
		 * information.
		 */
		__zicio_set_dma_sgl_mapping_to_command_for_md(sgl_addr, sgl_data,
				&nvme_cmd->cmd, in_buffer_page_offset,
				le16_to_cpu(nvme_cmd->cmd.rw.length), stride, in_page_offset,
				flags);
	}
}

static void
__zicio_set_dma_sgl_mapping_for_md_shared(zicio_descriptor *desc,
			dma_addr_t *sgl_addr, void *sgl_data, int local_page_idx,
			zicio_nvme_cmd_list *nvme_cmd, unsigned long buf_start,
			unsigned int stride, unsigned int in_page_offset, short int flags)
{
	zicio_shared_page_control_block *zicio_spcb;
	unsigned long in_buffer_page_offset, page_start;

	/* Get spcb from shared pool */
	zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);

	/* Get the start point of target huge page */
	page_start = (unsigned long)zicio_spcb->zicio_spcb.chunk_ptr;

	BUG_ON(buf_start < page_start);
	BUG_ON(buf_start > page_start + ZICIO_CHUNK_SIZE);
	/* Get the in-page offset to NVMe command setting. */
	in_buffer_page_offset = ((buf_start - page_start) >> ZICIO_PAGE_SHIFT);

	/*
	 * Performs DMA setting in the command using the calculated location
	 * information.
	 */
	__zicio_set_dma_sgl_mapping_to_shared_command_for_md(sgl_addr, sgl_data,
			&nvme_cmd->cmd, in_buffer_page_offset, le16_to_cpu(
					nvme_cmd->cmd.rw.length), stride, in_page_offset, flags);
}

/*
 * zicio_set_dma_mapping_for_nvme
 *
 * Set DMA address information in command
 */
static void
zicio_set_dma_mapping_for_nvme(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmds, int cmd_flag)
{
	zicio_shared_page_control_block *zicio_spcb;
	int device_idx = nvme_cmds->device_idx, map_idx;
	unsigned int local_page_idx = nvme_cmds->local_huge_page_idx;
	dma_addr_t *prp_addr;
	void *prp_page;
	int num_pages;

	/*
	 * DMA mapping information may vary depending on the buffer and device where
	 * I/O is performed. Set the dma mapping information accordingly.
	 */
	if (nvme_cmds->is_on_track_cmd) {
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		map_idx = zicio_get_dma_map_start_point_shared(desc, device_idx);
		prp_addr = zicio_spcb->zicio_spcb.dev_map[map_idx].prp_data_array;
		prp_page = zicio_spcb->zicio_spcb.dev_map[map_idx].prp_data_address;

		/*
		 * Set the DMA mapping information to the NVMe command using the set DMA
		 * mapping information.
		 */
		__zicio_set_dma_mapping_for_nvme_shared(desc, prp_addr, prp_page,
				local_page_idx, nvme_cmds, nvme_cmds->start_mem, cmd_flag);
		return;
	} else if (!nvme_cmds->is_metadata) {
		prp_addr = desc->dev_maps.dev_node_array[device_idx].dev_map->prp_data_array;
		prp_page = desc->dev_maps.dev_node_array[device_idx].dev_map->prp_data_address;
	} else {
		prp_addr = desc->dev_maps.dev_node_array[device_idx].dev_map->prp_inode_array;
		prp_page = desc->dev_maps.dev_node_array[device_idx].dev_map->prp_inode_address;
	}

	/*
	 * Set the DMA mapping information to the NVMe command using the set DMA
	 * mapping information.
	 */
	if (desc->zicio_shared_pool_desc) {
		num_pages = ZICIO_LOCAL_DATABUFFER_CHUNK_NUM;
	} else if (nvme_cmds->is_metadata) {
		num_pages = ZICIO_INODEBUFFER_PAGENUM;
	} else {
		num_pages = ZICIO_DATABUFFER_PAGENUM;
	}
	__zicio_set_dma_mapping_for_nvme(desc, prp_addr, prp_page,
			local_page_idx, num_pages, nvme_cmds, nvme_cmds->start_mem,
			cmd_flag);
}

/*
 * zicio_set_dma_prp_mapping_for_md
 *
 * Set DMA address information in command
 */
static void
zicio_set_dma_prp_mapping_for_md(zicio_descriptor *desc,
			zicio_nvme_cmd_list * nvme_cmds, int cmd_flag)
{
	zicio_device *zicio_device;
	int device_idx = nvme_cmds->device_idx;
	int mddev_idx = zicio_get_zicio_channel_mddev_idx(desc, device_idx);
	int inner_dev_idx = zicio_get_zicio_channel_innerdev_idx(desc, device_idx);
	unsigned int local_page_idx = nvme_cmds->local_huge_page_idx;
	dma_addr_t *prp_addr;
	void *prp_page;
	int dma_slot;
	int num_inner_dev;
	int in_page_offset;
	unsigned page_interval;
	zicio_dev_map_node *dev_node_map;

	/*
	 * DMA mapping information may vary depending on the buffer and device where
	 * I/O is performed. Set the dma mapping information accordingly.
	 */
	zicio_device = zicio_get_zicio_device_with_desc(desc, mddev_idx);
	num_inner_dev = zicio_get_num_inner_device(zicio_device);
	if (!nvme_cmds->is_metadata) {
		dma_slot = nvme_cmds->start_lpos % num_inner_dev;
		page_interval = zicio_get_dma_map_page_interval(
				ZICIO_DATABUFFER_CHUNK_NUM, dma_slot, num_inner_dev,
				&in_page_offset, false);
		dev_node_map = &desc->dev_maps.dev_node_array[mddev_idx];
		prp_addr = dev_node_map->dev_map[inner_dev_idx].prp_data_array +
			page_interval;
		prp_page = dev_node_map->dev_map[inner_dev_idx].prp_data_address +
			(page_interval * ZICIO_PAGE_SIZE);
	} else {
		dev_node_map = &desc->dev_maps.dev_node_array[mddev_idx];
		prp_addr = dev_node_map->dev_map[inner_dev_idx].prp_inode_array;
		prp_page = dev_node_map->dev_map[inner_dev_idx].prp_inode_address;
	}

	/*
	 * Set the DMA mapping information to the NVMe command using the set DMA
	 * mapping information.
	 */
	__zicio_set_dma_prp_mapping_for_md(desc, prp_addr, prp_page,
			local_page_idx, nvme_cmds, nvme_cmds->start_mem, num_inner_dev,
			in_page_offset, cmd_flag);
}

/*
 * zicio_set_dma_prp_mapping_for_md_shared
 *
 * Set DMA address information in command
 */
static void
zicio_set_dma_prp_mapping_for_md_shared(zicio_descriptor *desc,
			zicio_nvme_cmd_list * nvme_cmds, int cmd_flag)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_device *zicio_device;
	zicio_shared_page_control_block *zicio_spcb;
	zicio_dev_map_node *dev_node_map;
	int device_idx = nvme_cmds->device_idx;
	int mddev_idx = zicio_get_shared_pool_mddev_idx(
			zicio_shared_pool, device_idx);
	int inner_dev_idx = zicio_get_shared_pool_innerdev_idx(
			zicio_shared_pool, device_idx);
	unsigned int local_page_idx = nvme_cmds->local_huge_page_idx;
	dma_addr_t *prp_addr;
	void *prp_page;
	int dma_slot, page_interval, map_idx;
	int num_inner_dev, in_page_offset;

	/*
	 * DMA mapping information may vary depending on the buffer and device where
	 * I/O is performed. Set the dma mapping information accordingly.
	 */

	zicio_device = zicio_get_zicio_device_with_shared_pool(
			zicio_shared_pool, mddev_idx);
	num_inner_dev = zicio_get_num_inner_device(zicio_device);
	dma_slot = nvme_cmds->start_lpos % num_inner_dev;

	if (nvme_cmds->is_on_track_cmd) {
		page_interval = zicio_get_dma_map_page_interval(1, dma_slot,
				num_inner_dev, &in_page_offset, false);
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		map_idx = zicio_get_dma_map_start_point_shared(desc, device_idx);
		prp_addr = zicio_spcb->zicio_spcb.dev_map[map_idx].prp_data_array +
				page_interval;
		prp_page = zicio_spcb->zicio_spcb.dev_map[map_idx].prp_data_address +
				(page_interval + ZICIO_PAGE_SIZE);

		/*
		 * Set the DMA mapping information to the NVMe command using the set DMA
		 * mapping information.
		 */
		__zicio_set_dma_prp_mapping_for_md_shared(desc, prp_addr, prp_page,
				local_page_idx, nvme_cmds, nvme_cmds->start_mem, num_inner_dev,
				in_page_offset, cmd_flag);
	} else {
		page_interval = zicio_get_dma_map_page_interval(
				ZICIO_LOCAL_DATABUFFER_CHUNK_NUM, dma_slot,
				num_inner_dev, &in_page_offset, false);
		dev_node_map = &desc->dev_maps.dev_node_array[mddev_idx];
		prp_addr = dev_node_map->dev_map[inner_dev_idx].prp_data_array +
				page_interval;
		prp_page = dev_node_map->dev_map[inner_dev_idx].prp_data_address +
				page_interval * ZICIO_PAGE_SIZE;

		/*
		 * Set the DMA mapping information to the NVMe command using the set DMA
		 * mapping information.
		 */
		__zicio_set_dma_prp_mapping_for_md(desc, prp_addr, prp_page,
				local_page_idx, nvme_cmds, nvme_cmds->start_mem, num_inner_dev,
				in_page_offset, cmd_flag);
	}
}

/*
 * zicio_set_dma_sgl_mapping_for_md
 *
 * Set DMA address information in command
 */
static void
zicio_set_dma_sgl_mapping_for_md(zicio_descriptor *desc,
			zicio_nvme_cmd_list * nvme_cmds, int cmd_flag)
{
	zicio_device *zicio_device;
	int device_idx = nvme_cmds->device_idx;
	int mddev_idx = zicio_get_zicio_channel_mddev_idx(desc, device_idx);
	int inner_dev_idx = zicio_get_zicio_channel_innerdev_idx(desc, device_idx);
	unsigned int local_page_idx = nvme_cmds->local_huge_page_idx;
	dma_addr_t *sgl_addr;
	void *sgl_page;
	int dma_slot;
	unsigned page_interval;
	zicio_dev_map_node *dev_node_map;
	int num_inner_dev;
	int in_page_offset;

	/*
	 * DMA mapping information may vary depending on the buffer and device where
	 * I/O is performed. Set the dma mapping information accordingly.
	 */
	zicio_device = zicio_get_zicio_device_with_desc(desc, mddev_idx);
	num_inner_dev = zicio_get_num_inner_device(zicio_device);

	if (!nvme_cmds->is_metadata) {
		dma_slot = nvme_cmds->start_lpos % num_inner_dev;
		page_interval = zicio_get_dma_map_page_interval(
				ZICIO_DATABUFFER_CHUNK_NUM, dma_slot, num_inner_dev,
				&in_page_offset, true);
		dev_node_map = &desc->dev_maps.dev_node_array[mddev_idx];
		sgl_addr = dev_node_map->dev_map[inner_dev_idx].sgl_data_array +
			page_interval;
		sgl_page = dev_node_map->dev_map[inner_dev_idx].sgl_data_address +
			(page_interval * ZICIO_PAGE_SIZE);
	} else {
		dev_node_map = &desc->dev_maps.dev_node_array[mddev_idx];
		sgl_addr = dev_node_map->dev_map[inner_dev_idx].prp_inode_array;
		sgl_page = dev_node_map->dev_map[inner_dev_idx].prp_inode_address;
	}

	/*
	 * Set the DMA mapping information to the NVMe command using the set DMA
	 * mapping information.
	 */
	__zicio_set_dma_sgl_mapping_for_md(desc, sgl_addr, sgl_page,
			local_page_idx, nvme_cmds, nvme_cmds->start_mem, num_inner_dev,
			in_page_offset, cmd_flag);
}

/*
 * zicio_set_dma_sgl_mapping_for_md_shared
 *
 * Set DMA address information in command
 */
static void
zicio_set_dma_sgl_mapping_for_md_shared(zicio_descriptor *desc,
		zicio_nvme_cmd_list * nvme_cmds, int cmd_flag)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_device *zicio_device;
	zicio_shared_page_control_block *zicio_spcb;
	zicio_dev_map_node *dev_node_map;
	int device_idx = nvme_cmds->device_idx;
	int mddev_idx = zicio_get_shared_pool_mddev_idx(
			zicio_shared_pool, device_idx);
	int inner_dev_idx = zicio_get_shared_pool_innerdev_idx(
			zicio_shared_pool, device_idx);
	unsigned int local_page_idx = nvme_cmds->local_huge_page_idx;
	dma_addr_t *sgl_addr;
	void *sgl_page;
	int dma_slot, page_interval, map_idx;
	int num_inner_dev;
	int in_page_offset;

	/*
	 * DMA mapping information may vary depending on the buffer and device where
	 * I/O is performed. Set the dma mapping information accordingly.
	 *
	 * The shared data buffer page also works by performing DMA mapping on
	 * memory segments in consideration of device striping and setting the
	 * contents of this in the command.
	 */
	zicio_device = zicio_get_zicio_device_with_shared_pool(
			zicio_shared_pool, mddev_idx);
	num_inner_dev = zicio_get_num_inner_device(zicio_device);
	dma_slot = nvme_cmds->start_lpos % num_inner_dev;

	if (nvme_cmds->is_on_track_cmd) {
		page_interval = zicio_get_dma_map_page_interval(1, dma_slot,
				num_inner_dev, &in_page_offset, true);
		zicio_spcb = zicio_get_spcb_with_id(desc, local_page_idx);
		map_idx = zicio_get_dma_map_start_point_shared(desc, device_idx);
		sgl_addr = zicio_spcb->zicio_spcb.dev_map[map_idx].sgl_data_array +
				page_interval;
		sgl_page = zicio_spcb->zicio_spcb.dev_map[map_idx].sgl_data_address +
				(page_interval * ZICIO_PAGE_SIZE);

		/*
		 * Set the DMA mapping information to the NVMe command using the set DMA
		 * mapping information.
		 */
		__zicio_set_dma_sgl_mapping_for_md_shared(desc, sgl_addr, sgl_page,
				local_page_idx, nvme_cmds, nvme_cmds->start_mem, num_inner_dev,
				in_page_offset, cmd_flag);
	} else {
		page_interval = zicio_get_dma_map_page_interval(
				ZICIO_LOCAL_DATABUFFER_CHUNK_NUM, dma_slot,
				num_inner_dev, &in_page_offset, true);
		dev_node_map = &desc->dev_maps.dev_node_array[mddev_idx];
		sgl_addr = dev_node_map->dev_map[inner_dev_idx].sgl_data_array +
			page_interval;
		sgl_page = dev_node_map->dev_map[inner_dev_idx].sgl_data_address +
			page_interval * ZICIO_PAGE_SIZE;

		/*
		 * Set the DMA mapping information to the NVMe command using the set DMA
		 * mapping information.
		 */
		__zicio_set_dma_sgl_mapping_for_md(desc, sgl_addr, sgl_page,
				local_page_idx, nvme_cmds, nvme_cmds->start_mem,
				num_inner_dev, in_page_offset, cmd_flag);
	}
}

/*
 * zicio_set_dma_mapping_for_md
 *
 * Set DMA address information in command
 */
void
zicio_set_dma_mapping_for_md(zicio_descriptor *desc,
		zicio_nvme_cmd_list * nvme_cmds, int cmd_flag)
{
	if (cmd_flag & NVME_CMD_SGL_METABUF) {
		if (desc->zicio_shared_pool_desc) {
			zicio_set_dma_sgl_mapping_for_md_shared(
					desc, nvme_cmds, cmd_flag);
		} else {
			zicio_set_dma_sgl_mapping_for_md(desc, nvme_cmds, cmd_flag);
		}
	} else if (!cmd_flag) {
		if (desc->zicio_shared_pool_desc) {
			zicio_set_dma_prp_mapping_for_md_shared(
					desc, nvme_cmds, cmd_flag);
		} else {
			zicio_set_dma_prp_mapping_for_md(desc, nvme_cmds, cmd_flag);
		}
	} else {
		printk(KERN_WARNING "Error Unexpected DMA flags\n");
		BUG();
	}
}

void
zicio_set_dma_mapping_to_command(zicio_descriptor *desc,
			zicio_nvme_cmd_list *nvme_cmds, int cmd_flag)
{
	zicio_device *zicio_device = zicio_get_zicio_fs_device_with_desc(desc,
			nvme_cmds->device_idx);
	zicio_command_creator *zicio_cmd_creator =
			zicio_get_command_creator(zicio_device);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("[cpu %d] zicio_set_dma_mapping_to_command()\n",
			desc->cpu_id);
#endif

	zicio_cmd_creator->zicio_set_dma_mapping(desc, nvme_cmds, cmd_flag);
}

/*
 * zicio_create_nvme_command_for_metadata
 *
 * Function called to generate metadata commands.
 */
static zicio_nvme_cmd_list *
zicio_create_nvme_command_for_metadata(zicio_descriptor *desc,
			zicio_nvme_cmd_list **last_meta_cmds,
			zicio_file_struct *current_file_struct,
			unsigned int num_feeding)
{
	zicio_nvme_cmd_list *nvme_meta_cmds;

	/*
	 * Assign commands and set block device location to command
	 */
	nvme_meta_cmds = zicio_set_metadata_block_mapping(desc,
			num_feeding, last_meta_cmds, current_file_struct);

	/*
	 * If there is an assigned command, set the memory information for dma
	 * mapping to the command.
	 */
	if (nvme_meta_cmds) {
		zicio_preset_dma_mapping_for_nvme(desc, nvme_meta_cmds);
	}

	return nvme_meta_cmds;
}

/*
 * zicio_create_md_command_for_metadata
 *
 * Function called to generate metadata commands.
 */
void
zicio_create_md_command_for_metadata(zicio_descriptor *desc,
			zicio_nvme_cmd_list **last_meta_cmd_lists,
			zicio_file_struct *current_file_struct, int num_inner_dev,
			unsigned int num_feeding)
{
	zicio_set_metadata_block_mapping_for_md(desc, num_feeding,
			last_meta_cmd_lists, current_file_struct, num_inner_dev);
	/*
	 * If there is an assigned command, set the memory information for dma
	 * mapping to the command.
	 */
	if (zicio_has_cmd_list_in_set(last_meta_cmd_lists, num_inner_dev)) {
		zicio_preset_dma_mapping_for_md(desc, last_meta_cmd_lists,
				current_file_struct->device_idx_in_channel);
	}
}

/*
 * __zicio_create_nvme_command
 *
 * Create NVMe commands for a single chunk
 *
 * @desc: channel descriptor
 * @zicio_file: zicio file structure
 * @per_cmd_size: the I/O size per-command
 * @current_file_page_idx: file page idx to create command
 * @local_page_idx: local page index to perform I/O
 * @submit_metadata_command: flag to allow creation of metadata commands
 */
static zicio_nvme_cmd_list*
__zicio_create_nvme_command(zicio_descriptor *desc,
			zicio_nvme_cmd_list **start_cmd_lists,
			zicio_file_struct *zicio_file, ssize_t per_cmd_size,
			unsigned long current_file_page_idx, int local_page_idx,
			bool submit_metadata_command)
{
	unsigned need_feed = 0;
	zicio_nvme_cmd_list *nvme_cmds, *nvme_meta_cmds, *last_meta_cmds = NULL;
	int total_command_size;
	int per_req_cnt = per_cmd_size >> 2; /* 256 KiB / 4 KiB */
	int channel_device_idx;
	struct fd fd;
	unsigned long num_file_page;
	unsigned int in_file_page_idx;

	channel_device_idx = zicio_file->device_idx_in_channel;
	fd = zicio_file->fd;

	/* Calculate real command size considering the size of file */
	num_file_page = zicio_file->file_size;
	in_file_page_idx = zicio_get_in_file_page_idx(
				zicio_file, current_file_page_idx);
	total_command_size = min_t(int,
			ZICIO_CHUNK_SIZE >> ZICIO_PAGE_SHIFT,
					num_file_page - in_file_page_idx);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] [ZICIO] Request : %ld(%d/%ld)%%\n",
				desc->cpu_id, 100 * in_file_page_idx / num_file_page,
				in_file_page_idx, num_file_page);
#endif /* CONFIG_ZICIO_DEBUG */

	/*
	 * Set a block device location to command
	 */
	nvme_cmds = zicio_set_block_mapping_for_nvme(desc, start_cmd_lists,
				channel_device_idx, fd, current_file_page_idx, in_file_page_idx,
				total_command_size, per_req_cnt, &need_feed, local_page_idx);

	/*
	 * Set a DMA mapping information to command.
	 */
	if (nvme_cmds && local_page_idx != ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
		zicio_preset_dma_mapping_for_nvme(desc, nvme_cmds);
	}

	/*
	 * If we need to create a metadata command, thenmake sure that only one
	 * worker is doing it at the same time.
	 */
	if (need_feed && submit_metadata_command &&
		!atomic_cmpxchg(&desc->metadata_ctrl.metadata_producing,
		false, true)) {
		/*
		 * Generate metadata commands.
		 */
		nvme_meta_cmds = zicio_create_nvme_command_for_metadata(desc,
				&last_meta_cmds, zicio_file, need_feed);

		if (nvme_meta_cmds) {
			last_meta_cmds->next = nvme_cmds;
			nvme_cmds = nvme_meta_cmds;
		}

		atomic_set(&desc->metadata_ctrl.metadata_producing, false);
	}

	start_cmd_lists[0] = nvme_cmds;
	return nvme_cmds;
}

/*
 * zicio_create_nvme_command
 *
 * Create NVMe commands for a single chunk
 *
 * @desc: channel descriptor
 * @zicio_file: zicio file structure
 * @per_cmd_size: the I/O size per-command
 * @current_file_page_idx: file page idx to create command
 * @local_page_idx: local page index to perform I/O
 * @submit_metadata_command: flag to allow creation of metadata commands
 */
static void*
zicio_create_nvme_command(zicio_descriptor *desc,
			zicio_file_struct *zicio_file, ssize_t per_cmd_size,
			unsigned long current_file_page_idx, int local_page_idx,
			bool submit_metadata_command)
{
	zicio_nvme_cmd_list **start_cmd_lists;

	start_cmd_lists = zicio_alloc_cmd_lists_set_with_desc(desc,
				zicio_file->device_idx_in_channel);
	__zicio_create_nvme_command(desc, start_cmd_lists,
			zicio_file, per_cmd_size, current_file_page_idx, local_page_idx,
					submit_metadata_command);

	if (!start_cmd_lists[0]) {
		zicio_free_cmd_lists_set_with_desc(desc,
					zicio_file->device_idx_in_channel, start_cmd_lists);
		return NULL;
	}

	return start_cmd_lists;
}

/*
 * zicio_link_block_commands
 *
 * Link NVMe commands for a single chunk
 *
 * @start_cmd_lists: start point of command list
 * @num_inner_device: the number of inner device
 */
static void
zicio_link_block_commands(zicio_nvme_cmd_list **start_cmd_lists,
		int num_inner_device)
{
	zicio_nvme_cmd_list **last_data_cmd_lists = start_cmd_lists +
			num_inner_device;
	zicio_nvme_cmd_list **start_metadata_cmd_lists = start_cmd_lists +
			(num_inner_device << 1);
	zicio_nvme_cmd_list **last_metadata_cmd_lists = start_cmd_lists +
			(num_inner_device << 1) + num_inner_device;
	int idx;

	/* Fistly, if there are metadata command lists, then linking it to data
	 * command lists */
	for (idx = 0 ; idx < num_inner_device ; idx++) {
		if (last_metadata_cmd_lists[idx]) {
			last_metadata_cmd_lists[idx]->next = start_cmd_lists[idx];
			start_cmd_lists[idx] = start_metadata_cmd_lists[idx];
		}
	}

	/* Next, linking each other device command lists */
	for (idx = 1 ; idx < num_inner_device ; idx++) {
		last_data_cmd_lists[idx - 1]->next = start_cmd_lists[idx];
	}
}

/*
 * zicio_create_md_command_shared
 *
 * MD command creation function for a channel that operates by attaching to
 * a shared pool
 */
static void *
zicio_create_md_command_shared(zicio_descriptor *desc,
			zicio_file_struct *zicio_file, int current_file_idx,
			ssize_t per_cmd_size, unsigned long file_chunk_idx,
			int local_page_idx, bool is_on_track)
{
	int num_inner_device;
	/* zicio nvme command descriptor to return */
	zicio_nvme_cmd_list **start_cmd_lists;

	/* shared pool and shared pool info for channel */
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;

	/* Request counts */
	int per_req_cnt = per_cmd_size >> 2;
	int tot_req_cnt = ZICIO_CHUNK_SIZE >> ZICIO_PAGE_SHIFT;

	int device_idx;
	struct fd fd;
	unsigned int in_file_chunk_idx;
	unsigned long file_size, requested_size;

	/* The size of cmd should be larger than one page size */
	BUG_ON(per_req_cnt == 0);

    if (file_chunk_idx == UINT_MAX) {
        /* We consumed all of required file chunks. */
        return NULL;
    }

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	BUG_ON(file_chunk_idx >= zicio_shared_pool->shared_files.total_chunk_nums);

	/* Set device idx */
	device_idx = zicio_file->device_idx_in_channel;
	/* Set file Descriptor */
	fd = zicio_file->fd;
	/* Get the total file size of total chunk number */
	file_size = zicio_shared_pool->shared_files.total_chunk_nums;
	/* Get the requested chunk numbers */
	requested_size = atomic_read(&zicio_shared_pool_local->num_mapped);

	/* in-file chunk index */
	in_file_chunk_idx = zicio_get_in_file_chunk_id(zicio_shared_pool,
				zicio_shared_pool_local, current_file_idx, file_chunk_idx);
	/* Last chunk couldn't be aligned to 2MB, set total count of command
	 * considering it */
	tot_req_cnt = zicio_get_current_tot_req_cnt_shared(zicio_shared_pool,
				zicio_shared_pool_local, zicio_file, in_file_chunk_idx, tot_req_cnt);

	/*
	 * Get the number of device consisting of md
	 */
	num_inner_device = zicio_get_num_inner_device_with_id(
			zicio_get_zicio_global_device_idx(desc, device_idx));
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] [ZICIO] Request : %ld(%d/%ld)%%\n",
			desc->cpu_id, 100 * atomic_read(&zicio_shared_pool_local->num_mapped)
			/ file_size, atomic_read(&zicio_shared_pool_local->num_mapped),
			file_size);
#endif /* CONFIG_ZICIO_DEBUG */

	/* Set block mapping information for shared pool */
	start_cmd_lists = zicio_set_block_mapping_shared_for_md(desc,
				device_idx, num_inner_device, fd, current_file_idx,
				in_file_chunk_idx << ZICIO_PAGE_TO_CHUNK_SHIFT,
				tot_req_cnt, per_req_cnt, file_chunk_idx, local_page_idx);

	/* Preset dma mapping information for shared pool */
	if (zicio_has_cmd_list_in_set(start_cmd_lists, num_inner_device)) {
		zicio_preset_dma_mapping_for_md_shared(desc, start_cmd_lists,
				zicio_file->device_idx_in_channel, is_on_track);
	}

	return start_cmd_lists;
}

/*
 * __zicio_create_md_command
 *
 * MD command creation function for a local channel
 */
static void*
zicio_create_md_command(zicio_descriptor *desc,
			zicio_file_struct *zicio_file, ssize_t per_cmd_size,
			unsigned long current_file_page_idx, int local_page_idx,
			bool submit_metadata_command)
{
	int num_inner_device;
	unsigned need_feed = 0;
	zicio_nvme_cmd_list **start_cmd_lists;
	int total_command_size;
	int per_req_cnt = per_cmd_size >> 2;
	int channel_device_idx;
	struct fd fd;
	unsigned long num_file_page;
	unsigned int in_file_page_idx;

	channel_device_idx = zicio_file->device_idx_in_channel;
	fd = zicio_file->fd;

	/*
	 * Calculate real command size considering the size of file.
	 * and get the start page to read.
	 */
	num_file_page = zicio_file->file_size;
	in_file_page_idx = zicio_get_in_file_page_idx(
			zicio_file, current_file_page_idx);
	total_command_size = min_t(int,
			ZICIO_CHUNK_SIZE >> ZICIO_PAGE_SHIFT,
					num_file_page - in_file_page_idx);

	/*
	 * Get the number of device consisting of md
	 */
	num_inner_device = zicio_get_num_inner_device_with_id(
			zicio_get_zicio_global_device_idx(desc, channel_device_idx));

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk(KERN_WARNING "cpu[%d] [ZICIO] Request : %ld(%d/%ld)%%\n",
				desc->cpu_id, 100 * in_file_page_idx / num_file_page,
						in_file_page_idx, num_file_page);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */

	/*
	 * Set a block device location to command
	 */
	start_cmd_lists = zicio_set_block_mapping_for_md(desc,
				channel_device_idx, num_inner_device, fd, current_file_page_idx,
				in_file_page_idx, total_command_size, per_req_cnt, &need_feed,
				local_page_idx);

	/*
	 * Preset a DMA mapping information to command
	 */
	if (zicio_has_cmd_list_in_set(start_cmd_lists, num_inner_device) &&
			local_page_idx != ZICIO_INVALID_LOCAL_HUGE_PAGE_IDX) {
		zicio_preset_dma_mapping_for_md(desc, start_cmd_lists,
				zicio_file->device_idx_in_channel);
	}

	/*
	 * If we need to create a metadata command, thenmake sure that only one
	 * worker is doing it at the same time.
	 */
	if (need_feed && submit_metadata_command &&
		!atomic_cmpxchg(&desc->metadata_ctrl.metadata_producing,
		false, true)) {
		/*
		 * Generate metadata commands.
		 */
		zicio_create_md_command_for_metadata(desc, start_cmd_lists +
				(num_inner_device << 1), zicio_file, num_inner_device, need_feed);
		atomic_set(&desc->metadata_ctrl.metadata_producing, false);
	}

	if (zicio_has_cmd_list_in_set(start_cmd_lists + (num_inner_device << 1),
			num_inner_device)) {
		zicio_link_block_commands(start_cmd_lists, num_inner_device);
	}

	return start_cmd_lists;
}

/*
 * zicio_set_zombie_nvme_command
 *
 * If we can dispatch the zombie command, we need to set the memory 
 * information. This function is called during this process.
 */
static zicio_nvme_cmd_list *
zicio_set_zombie_nvme_command(zicio_descriptor *desc,
	zicio_nvme_cmd_list * zombie_cmd)
{
	/*
	 * Set the memory information for DMA mapping later by using the memory
	 * information located in the cmd of zombie cmd.
	 */
	zicio_preset_dma_mapping_for_nvme(desc, zombie_cmd);

	return zombie_cmd;
}

/*
 * zicio_set_zombie_nvme_command
 *
 * If we can dispatch the zombie command, we need to set the memory 
 * information. This function is called during this process.
 */
static zicio_nvme_cmd_list **
zicio_set_zombie_md_command(zicio_descriptor *desc,
	zicio_nvme_cmd_list **zombie_cmd_lists, int mddev_idx)
{
	/*
	 * Set the memory information for DMA mapping later by using the memory
	 * information located in the cmd of zombie cmd.
	 */
	zicio_preset_dma_mapping_for_md(desc, zombie_cmd_lists, mddev_idx);

	return zombie_cmd_lists;
}

/*
 * __zicio_create_command_shared
 *
 * Create command for channel with shared pool
 */
static zicio_nvme_cmd_list **
__zicio_create_command_shared(zicio_descriptor *desc,
			int local_page_idx, int *num_dev, bool is_on_track)
{
	zicio_shared_pool *zicio_shared_pool;
	zicio_shared_pool_local *zicio_shared_pool_local;
	zicio_device *zicio_device;
	zicio_file_struct *zicio_file;
	zicio_command_creator *cmd_creator;

	int current_file_idx;
	unsigned long file_chunk_idx;
	int per_cmd_size;

	zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local = zicio_get_shared_pool_local(desc);

	/*
	 * Get the file chunk id to read next
	 */
	if (unlikely(zicio_is_first_command_create(zicio_shared_pool_local))) {
		file_chunk_idx = zicio_get_init_chunk_id(desc, local_page_idx);
	} else {
		file_chunk_idx = zicio_get_next_file_chunk_id_shared(desc,
				local_page_idx, is_on_track);
	}

	/*
	 * Check file chunk index for error handling.
	 */
	if (file_chunk_idx == UINT_MAX) {
		return NULL;
	} else if (file_chunk_idx == UINT_MAX - 1) {
		return ERR_PTR(UINT_MAX - 1);
	}

	BUG_ON(local_page_idx == -1);
	BUG_ON(file_chunk_idx >= zicio_shared_pool->shared_files.total_chunk_nums);

	if ((current_file_idx = zicio_get_current_file_id_shared(zicio_shared_pool,
			zicio_shared_pool_local, file_chunk_idx, true, true)) == INT_MAX) {
		return NULL;
	}

	if ((zicio_file = zicio_get_id_file_struct(
			&zicio_shared_pool->shared_files.registered_read_files,
					current_file_idx)) == NULL) {
		return NULL;
	}

	if (!(zicio_device = zicio_get_zicio_device_with_desc(desc,
			zicio_file->device_idx_in_channel))) {
		return NULL;
	}

	*num_dev = zicio_get_num_inner_device(zicio_device);

	cmd_creator = zicio_get_command_creator(zicio_device);

	/*
	 * Determining io size per one command should be generated for the target
	 * device to receive commands. For this, io size is decided, after file and
	 * device is chosen.
	 */
	per_cmd_size = cmd_creator->zicio_get_current_io_size(desc,
			zicio_file->device_idx_in_channel);

	return cmd_creator->zicio_create_device_command_shared(
			desc, zicio_file, current_file_idx, per_cmd_size, file_chunk_idx,
					local_page_idx, is_on_track);
}

/*
 * __zicio_create_command
 *
 * Create command for local channel
 */
static zicio_nvme_cmd_list **
__zicio_create_command(zicio_descriptor *desc, int local_page_idx,
		int *num_dev, bool create_metadata_cmd)
{
	zicio_device *zicio_device;
	zicio_file_struct *zicio_file;
	zicio_command_creator *cmd_creator;
	unsigned long current_file_page_idx;
	int per_cmd_size;

	/*
	 * Get the file chunk id to read next
	 */
	if ((current_file_page_idx = zicio_get_current_file_page_idx(
				&desc->read_files)) == ULONG_MAX) {
		return NULL;
	}

	if (!(zicio_file = zicio_get_file_struct_for_cmd(desc,
				current_file_page_idx, NULL))) {
		return NULL;
	}

	/*
	 * Get the structure of zicio device
	 */
	if (!(zicio_device = zicio_get_zicio_device_with_desc(desc,
			zicio_file->device_idx_in_channel))) {
		return NULL;
	}

	*num_dev = zicio_get_num_inner_device(zicio_device);
	cmd_creator = zicio_get_command_creator(zicio_device);

	/*
	 * Determining io size per one command should be generated for the target
	 * device to receive commands. For this, io size is decided, after file and
	 * device is chosen.
	 */
	per_cmd_size = cmd_creator->zicio_get_current_io_size(desc,
				zicio_file->device_idx_in_channel);


	return cmd_creator->zicio_create_device_command(desc, zicio_file,
			per_cmd_size, current_file_page_idx, local_page_idx,
					create_metadata_cmd);
}

/*
 * zicio_create_command
 *
 * Generic function to Create NVMe commands for a single chunk
 * Identify device and file, and call for the command create function
 *
 * @desc: channel descriptor
 * @local_page_idx: local page index to perform I/O
 * @num_dev: number of device
 * @create_metadata_command: flag to allow creation of metadata commands
 */
zicio_nvme_cmd_list **
zicio_create_command(zicio_descriptor *desc, int local_page_idx,
		int *num_dev, bool create_metadata_cmd)
{
	return __zicio_create_command(desc, local_page_idx, num_dev,
			create_metadata_cmd);
}

/*
 * zicio_create_command_shared
 *
 * Generic function to Create NVMe commands for a single chunk
 * Identify device and file, and call for the command create function
 *
 * @desc: channel descriptor
 * @local_page_idx: local page index to perform I/O
 * @num_dev: number of device
 * @on_track_cmd: flag to allow creation of metadata commands
 */
zicio_nvme_cmd_list **
zicio_create_command_shared(zicio_descriptor *desc, int local_page_idx,
		int *num_dev, bool is_on_track)
{
	return __zicio_create_command_shared(desc, local_page_idx, num_dev,
			is_on_track);
}

/*
 * zicio_set_nvme_zombie_command_list
 *
 * Set dma mapping information for zombie NVMe command list
 *
 * @desc: channel descriptor
 * @cmd_list: zombie command list
 * @local_huge_page_idx: local page index to perform I/O
 */
static void
zicio_set_nvme_zombie_command_list(zicio_descriptor *desc,
		zicio_nvme_cmd_list **cmd_list, int local_huge_page_idx)
{
	zicio_nvme_cmd_list *nvme_cmd_list = cmd_list[0], *tmp;

	tmp = nvme_cmd_list;
	while (tmp != NULL) {
		tmp->local_huge_page_idx = local_huge_page_idx;
		tmp = tmp->next;
	}
	zicio_set_zombie_nvme_command(desc, nvme_cmd_list);
}

/*
 * zicio_set_md_zombie_command_list
 *
 * Set dma mapping information for zombie NVMe command list
 *
 * @desc: channel descriptor
 * @cmd_list: zombie command list
 * @local_huge_page_idx: local page index to perform I/O
 */
static void
zicio_set_md_zombie_command_list(zicio_descriptor *desc,
		zicio_nvme_cmd_list **cmd_lists, int local_huge_page_idx)
{
	zicio_nvme_cmd_list *nvme_cmd_list = cmd_lists[0], *tmp;
	zicio_device *zicio_device = zicio_get_zicio_fs_device_with_desc(desc,
				nvme_cmd_list->device_idx);
	int num_inner_dev = zicio_get_num_inner_device(zicio_device), idx;

	for (idx = 0 ; idx < num_inner_dev ; idx++) {
		tmp = cmd_lists[idx];
		while (tmp != NULL) {
			tmp->local_huge_page_idx = local_huge_page_idx;
			tmp = tmp->next;
		}
	}

	zicio_set_zombie_md_command(desc, cmd_lists,
			zicio_get_zicio_channel_mddev_idx(desc,
					nvme_cmd_list->device_idx));
}

/*
 * zicio_set_zombie_command_list
 *
 * Set dma mapping information for zombie command list
 *
 * @desc: channel descriptor
 * @cmd_list: zombie command list
 * @local_huge_page_idx: local page index to perform I/O
 */
void
zicio_set_zombie_command_list(zicio_descriptor *desc,
			zicio_nvme_cmd_list **cmd_lists, int local_huge_page_idx)
{
	zicio_device *zicio_device;
	zicio_command_creator *cmd_creator;

	/* Get device and its command creator */
	zicio_device = zicio_get_zicio_fs_device_with_desc(desc,
			cmd_lists[0]->device_idx);
	BUG_ON(!zicio_device);

	cmd_creator = zicio_get_command_creator(zicio_device);
	/* Call function pointer to set zombie command */
	cmd_creator->zicio_set_device_zombie_command_list(desc, cmd_lists,
				local_huge_page_idx);
}

/*
 * zicio_set_command_creator
 *
 * Set command creator hook per device
 *
 * @zicio_device - zicio device structure to set command creation hook
 */
void
zicio_set_command_creator(zicio_device *zicio_device)
{
	zicio_command_creator *cmd_creator =
			zicio_get_command_creator(zicio_device);

	switch (zicio_device->device_type) {
		case ZICIO_NVME:
				cmd_creator->zicio_create_device_command =
					zicio_create_nvme_command;
				cmd_creator->zicio_create_device_command_shared =
					zicio_create_nvme_command_shared;
				cmd_creator->zicio_set_device_zombie_command_list =
					zicio_set_nvme_zombie_command_list;
				cmd_creator->zicio_map_dma_buffer =
					zicio_map_dma_buffer_for_nvme;
				cmd_creator->zicio_map_dma_buffer_shared =
					zicio_map_dma_buffer_shared_for_nvme;
				cmd_creator->zicio_unmap_dma_buffer =
					zicio_unmap_dma_buffer_for_nvme;
				cmd_creator->zicio_unmap_dma_buffer_shared =
					zicio_unmap_dma_buffer_shared_for_nvme;
				cmd_creator->zicio_allocate_dev_map =
					zicio_allocate_dev_map_for_nvme;
				cmd_creator->zicio_set_dma_mapping =
					zicio_set_dma_mapping_for_nvme;
				cmd_creator->zicio_get_current_io_size =
					zicio_get_current_io_size;
			break;
		case ZICIO_MD:
				cmd_creator->zicio_create_device_command =
					zicio_create_md_command;
				cmd_creator->zicio_create_device_command_shared =
					zicio_create_md_command_shared;
				cmd_creator->zicio_set_device_zombie_command_list =
					zicio_set_md_zombie_command_list;
				cmd_creator->zicio_map_dma_buffer =
					zicio_map_dma_buffer_for_md;
				cmd_creator->zicio_map_dma_buffer_shared =
					zicio_map_dma_buffer_shared_for_md;
				cmd_creator->zicio_unmap_dma_buffer =
					zicio_unmap_dma_buffer_for_md;
				cmd_creator->zicio_unmap_dma_buffer_shared =
					zicio_unmap_dma_buffer_shared_for_md;
				cmd_creator->zicio_allocate_dev_map =
					zicio_allocate_dev_map_for_md;
				cmd_creator->zicio_set_dma_mapping =
					zicio_set_dma_mapping_for_md;
				cmd_creator->zicio_get_current_io_size =
					zicio_get_current_io_size_for_md;
			break;
		default:
			printk(KERN_WARNING "[ZICIO] unknown device to set command "
								"creator\n");
			BUG_ON(true);
	}
}

/**
 * @brief Create a new zicio_cmd using nvme_cmd_info
 * 
 * @param desc zicio_descriptor*
 * @param chunk_idx index of the current data buffer
 * @param nvme_cmd_info info of the nvme command to be put into zicio_cmd
 * 
 * @return zicio_nvme_cmd_list**
 * 
 * @details contrieved code for using zicio_read_trigger()
 * 
 * TODO: fix during reformatting
 */
zicio_nvme_cmd_list **
zicio_notify_create_command(zicio_descriptor *desc, int chunk_idx,
					 zicio_nvme_cmd_info nvme_cmd_info, unsigned long start_mem)
{
	zicio_notify_descriptor *zicio_notify_desc = (zicio_notify_descriptor*)desc;
	zicio_nvme_cmd_list **start_cmd_lists =
		zicio_alloc_cmd_lists_set_with_desc(desc, 0);
	zicio_nvme_cmd_list *zicio_cmd_head = NULL, *cur_zicio_cmd = NULL; /* only for function call */
	zicio_read_files *zicio_rfile = &(desc->read_files);
	zicio_file_struct* cur_file =
		zicio_get_id_file_struct(zicio_rfile,
			ZICIO_NVME_CMD_INFO_GET_FILE(nvme_cmd_info));

	zicio_allocate_nvme_cmd(&zicio_cmd_head, &cur_zicio_cmd);
	zicio_initialize_nvme_cmd(zicio_cmd_head, 0, cur_file->fd, 0, 0, 0,
								  chunk_idx, false);
	
	zicio_set_nvme_block_mapping_command(zicio_cmd_head, nvme_cmd_read,
		ZICIO_NVME_CMD_INFO_GET_FSBLK(nvme_cmd_info),
		zicio_notify_desc->bd_start_sector,
		ZICIO_NVME_CMD_INFO_GET_LENGTH(nvme_cmd_info) + 1);

	zicio_cmd_head->start_mem = start_mem;

	start_cmd_lists[0] = zicio_cmd_head;

	return start_cmd_lists;
}
