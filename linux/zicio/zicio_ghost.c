// SPDX-License-Identifier: GPL-2.0
#ifdef CONFIG_ZICIO
#include <linux/pgtable.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/zicio_notify.h>
#include <linux/atomic/atomic-instrumented.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <uapi/linux/zicio.h>

#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

#include "zicio_ghost.h"
#include "zicio_desc.h"
#include "zicio_atomic.h"
#include "zicio_shared_pool.h"

/* 
 * TODO: We get redefined error, so just put here. We need to move those macros 
 * to other header files later. 
 */
#define ZICIO_USER_DATABUFFER_SIZE (1 * 1024 * 1024 * 1024ULL) /* 1 GiB */

static inline struct task_struct *zicio_current(zicio_descriptor *desc) {
	struct task_struct *task = current;

	return desc->zicio_current == task ? task : desc->zicio_current;
}

/*
 * zicio_get_pud - get pud entry to access ghost table
 * @mm: current memory management descriptor
 * @user_base_address: user virtual address
 * 
 * It calculates a pud entry to access the ghost table. We assume that the 
 * @user_base_address parameter is algined by @ZICIO_USER_DATABUFFER_SIZE.
 * For now, we save the pud entry inside ghost structure at initialization phase.
 */
static pud_t *zicio_walk_pud(struct mm_struct *mm, 
	unsigned long user_base_address)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	/* Get pgd */
	pgd = pgd_offset(mm, user_base_address);

	/* Get p4d */
	p4d = p4d_alloc(mm, pgd, user_base_address);
	if (!p4d) 
		return NULL;

	/* Get pud */
	pud = pud_alloc(mm, p4d, user_base_address);
	if (!pud)
		return NULL;

	return pud;
}

static inline pud_t *
zicio_get_pud(zicio_descriptor *desc)
{
	zicio_ghost *ghost = &desc->ghost;

	if (unlikely(ghost->pud == NULL))
		return NULL;

	return ghost->pud;
}

static inline unsigned long
zicio_get_user_base_address(zicio_descriptor *desc)
{
	zicio_ghost *ghost = &desc->ghost;

	if (unlikely(ghost->user_base_address == 0))
		return -1;

	if (unlikely(((ghost->user_base_address) % HPAGE_PUD_SIZE) != 0))
		return -1;

	return ghost->user_base_address;
}

/*
 * zicio_ghost_init - do initialization for ghost mapping
 * @desc: zicio descriptor
 * @user_base_address: user address for ghost table
 *
 * We first calculate ghost table's address(=pud) using user base address.
 * Then, We save its address into the zicio descriptor to use later.
 */
int zicio_ghost_init(zicio_descriptor *desc,
	unsigned long user_base_address)
{
	struct mm_struct *mm = zicio_current(desc)->mm;
	struct vm_area_struct *vma;
	zicio_ghost *ghost;
	pmd_t *pmd;
	pmd_t *pmd_start;
	int ret = 0;
	int i;
	
	ghost = &desc->ghost;

	/* Do untagging */
	user_base_address = untagged_addr(user_base_address);

	if (unlikely((user_base_address % HPAGE_PUD_SIZE) != 0)) {
		ret = -1;
		goto l_zicio_ghost_init_out;
	}

	/* Performs the calculation of pud entries used for the ghost table. */
	ghost->pud = zicio_walk_pud(mm, user_base_address);
	if (ghost->pud == NULL) {
		ret = -1;
		goto l_zicio_ghost_init_out;
	}

	ghost->user_base_address = user_base_address;
	atomic64_set(&ghost->premapping_iter, 0);
	atomic64_set(&ghost->unmapping_iter, 0);

	/* Get write lock for vma modification. */
	mmap_write_lock(mm);
	vma = find_vma(mm, user_base_address);
	
	/* Save original VMA flag */
	ghost->orig_vm_flags = vma->vm_flags;
	ghost->orig_prot = vma->vm_page_prot;

	/* Set VMA flags */
	vma->vm_flags &= ~VM_WRITE;
	vma->vm_flags |= 
		(VM_NORESERVE | VM_DONTEXPAND | VM_LOCKED);

	/* Get read lock to read page table entry. */

	/* Get pmd */
	pmd_start = pmd_alloc(mm, ghost->pud, user_base_address);

	/* Init ghost table */
	for (i = 0; i < ZICIO_MAX_NUM_GHOST_ENTRY; ++i) {
		spinlock_t *ptl;

		pmd = pmd_start + i;

		/* Lock pmd entry */
		ptl = pmd_lock(mm, pmd);

		{
				pmd_t entry;

				pmd_clear(&entry);

				/* set flags */
				entry = 
					pmd_set_flags(entry, _PAGE_ZICIO_UNMAPPING | _PAGE_PSE);

				set_pmd(pmd, entry);
		}

		/* Unlock pmd entry */
		spin_unlock(ptl);
	}
	mmap_write_unlock(mm);

l_zicio_ghost_init_out:
	return ret;
}

/*
 * zicio_ghost_close - clean up resources for ghost mapping
 * @desc: zicio descriptor
 *
 * Clean up resources for ghost mapping.
 */
void zicio_ghost_close(zicio_descriptor *desc)
{
	struct mm_struct *mm = zicio_current(desc)->mm;
	struct vm_area_struct *vma;
	unsigned long user_base_address = desc->ghost.user_base_address;
	unsigned i;
	pud_t *pud;
	pmd_t *pmd;
	pmd_t *pmd_start;
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned long flags;
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	mmap_write_lock(mm);
	vma = find_vma(mm, user_base_address);

	/* Get pud entry for ghost table */
	pud = zicio_get_pud(desc);

	/* Get pmd */
	pmd_start = pmd_alloc(mm, pud, user_base_address);

	/* free ghost table */
	for (i = 0; i < ZICIO_MAX_NUM_GHOST_ENTRY; ++i) {
		spinlock_t *ptl;

		pmd = pmd_start + i;

		/* Lock pmd entry */
		ptl = pmd_lock(mm, pmd);

		if (!pmd_none(*pmd) && ((pmd_val(*pmd) & pmd_pfn_mask(*pmd)) != 0)) {
			struct page *page;
			unsigned long haddr;

			/* 
			 * Page exists - we need to clean up 
			 */

			/* Calculate n'th entry of user address */
			haddr = 
				((user_base_address + (i << HPAGE_PMD_SHIFT)) & HPAGE_PMD_MASK);

			/* Get huge page, 2 MiB */
			page = pmd_page(*pmd);

			/* Add rss-stat */
			// DEPRECATED...
			//add_mm_counter(mm, mm_counter_file(page), -HPAGE_PMD_NR);

			// DEPRECATED...
			//page_remove_rmap(page, true);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			local_irq_save(flags);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
			/* TLB flush */
			zicio_flush_tlb_range(vma, haddr, haddr + HPAGE_PMD_SIZE,
						desc->cpu_id);
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			local_irq_restore(flags);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
		}

		/* Set entry to empty */
		pmd_clear(pmd);

		/* Unlock pmd entry */
		spin_unlock(ptl);
	}

	/* Get write lock for vma modification */
	vma->vm_flags = desc->ghost.orig_vm_flags;
	mmap_write_unlock(mm);
}

/*
 * zicio_allocate_hugepage
 *
 * ZicIO use huge page(=Transparent Huge Page) for 2 MiB chunk. 
 */
struct page *
zicio_allocate_hugepage(void)
{
	struct page *zero_hugepage;
l_zicio_allocate_hugepage_retry:
	zero_hugepage = alloc_pages((GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM),
			HPAGE_PMD_ORDER);
	if (!zero_hugepage) {
		goto l_zicio_allocate_hugepage_retry;
	}

	/* Prepare and clear hugepage */
	prep_transhuge_page(zero_hugepage);
	clear_huge_page(zero_hugepage, 0, HPAGE_PMD_NR);

	SetPageReserved(zero_hugepage);

	return zero_hugepage;
}

/*
 * zicio_free_hugepage
 */
void
zicio_free_hugepage(struct page *page)
{
	__free_pages(page, compound_order(page));
}

/*
 * zicio_ghost_mapping_hugepage
 * @desc: zicio descriptor
 * @entry_idx: entry idx to map
 * @page: page for mapping
 *
 * ZicIO does mapping using ghost table. In here, @entry_idx is index number 
 * for mapping inside ghost table. @page is the structure of page to insert.
 *
 * Return -1 for an error.
 * Return 0 for success
 */
int zicio_ghost_mapping_hugepage(zicio_descriptor *desc, 
		unsigned long entry_idx, struct page* page)
{
	unsigned long haddr;
	unsigned long user_base_address;
	pmd_t *pmd;
	pud_t *pud;
	int ret = 0;

	user_base_address = zicio_get_user_base_address(desc);

	/* We only get compound page */
	if (unlikely(!PageCompound(page)))
		return -1;

	/* Calculate n'th entry of user address */
	haddr = 
		((user_base_address + (entry_idx << HPAGE_PMD_SHIFT)) & HPAGE_PMD_MASK);

	/* Get pud entry for ghost table */
	pud = zicio_get_pud(desc);

	/* Get pmd */
	pmd = pmd_offset(pud, haddr);
	if (!pmd) {
		ret = -1;
		goto l_zicio_mapping_hugepage_read_unlock;
	}

	page = compound_head(page);
	if (compound_order(page) != HPAGE_PMD_ORDER) {
		ret = -1;
		goto l_zicio_mapping_hugepage_read_unlock;
	}

	/*
	 * Just backoff if any subpage of a THP is corrupted otherwise
	 * the corrupted page may mapped by PMD silently to escape the
	 * check.  This kind of THP just can be PTE mapped.  Access to
	 * the corrupted subpage should trigger SIGBUS as expected.
	 */
	if (unlikely(PageHasHWPoisoned(page))) {
		ret = -1;
		goto l_zicio_mapping_hugepage_read_unlock;
	}

	{
		pmd_t entry;

		entry = mk_huge_pmd(page, (pgprot_t) desc->ghost.orig_prot);

		*pmd = entry;

	}

l_zicio_mapping_hugepage_read_unlock:
	return ret;
}

static int
zicio_ghost_find_empty_entry(zicio_descriptor *desc)
{
	zicio_switch_board *msb = desc->switch_board;
	int mapping_idx = -1;
	unsigned long start_iter = GET_INC_PREMAPPING_ITER(desc);
	unsigned long end_iter =
		GET_UNMAPPING_ITER(desc) + ZICIO_MAX_NUM_GHOST_ENTRY;
	unsigned idx = start_iter % ZICIO_MAX_NUM_GHOST_ENTRY;

	BUG_ON(start_iter > end_iter);

	if (zicio_read_status(msb, idx) == ENTRY_EMPTY) {
		mapping_idx = idx;
	} else {
		DECREMENT_PREMAPPING_ITER(desc);
	}

	return mapping_idx;
}

/*
 * zicio_ghost_premap - do premapping if exists empty entry
 * @desc: zicio descriptor
 * @k_chunk_addr: chunk virtual address
 *
 * We does pre-mapping inside ghost table.
 * 
 * Return -2, it has an error.
 * Return -1, it has no space for premapping anymore.
 * Return 0, success.
 */
int zicio_ghost_premap(zicio_descriptor *desc,
	zicio_shared_page_control_block *zicio_spcb, void *k_chunk_addr,
	unsigned int premap_high_watermark, int distance_idx)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	zicio_switch_board *msb;
	int empty_entry_idx;
	int ret = -1;
	unsigned int current_file_chunk_id;
	unsigned int previous_low_premap_point =
			atomic_read(&zicio_channel->previous_low_premap_point);

	msb = desc->switch_board;
	if (unlikely(!msb))
		return -EINVAL;
	
	/* Find empty entry for insertion */
	empty_entry_idx = zicio_ghost_find_empty_entry(desc);
	if (unlikely(empty_entry_idx < 0)) {
		ret = -1;
		return ret;
	}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("cpu_id[%d] premap try idx: %d file chunk id: %d [%s:%d] [%s]\n",
		desc->cpu_id, empty_entry_idx,zicio_spcb->zicio_spcb.file_chunk_id, __FILE__,
		 __LINE__, __FUNCTION__);
#endif

	/* Exchange empty entry for ready */
	ret = zicio_ghost_mapping_hugepage(desc, empty_entry_idx, 
						virt_to_page(k_chunk_addr));

	/* Success */
	if (ret == 0) {
		zicio_set_shared_page_control_block(desc, empty_entry_idx, zicio_spcb);
		zicio_set_tracking_ingestion_point(desc, empty_entry_idx,
				premap_high_watermark, distance_idx);

		ret = empty_entry_idx;
		current_file_chunk_id = zicio_get_user_file_chunk_id(desc,
				empty_entry_idx);

		BUG_ON(current_file_chunk_id == UINT_MAX);

		current_file_chunk_id = zicio_convert_chunk_id_to_monotonic_id(
				zicio_shared_pool, zicio_shared_pool_local, current_file_chunk_id);

#if 0
		zicio_set_tracking_user_monotonic_chunk_id(desc,
			empty_entry_idx, current_file_chunk_id);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

		if (previous_low_premap_point > current_file_chunk_id) {
			atomic_set(&zicio_channel->previous_low_premap_point,
				current_file_chunk_id);
		}
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
		zicio_set_turn_on_premap_bitvector(desc, zicio_spcb->chunk_id);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
	} else {
		ret = -2;
	}

	return ret;
}

static bool
zicio_ghost_entry_expired(zicio_descriptor *desc, int entry_idx)
{
	unsigned long expire_jiffies;
	unsigned long current_jiffies = get_jiffies_64();

	/* Get expire time */
	expire_jiffies = zicio_get_shared_page_expiration_jiffies(
				desc, entry_idx);  
 
	if (expire_jiffies < current_jiffies) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk("cpu_id[%d] FOCREFUL UNMAP %d, idx %d, expire jiffies %ld, "
			   "current jiffies %ld\n", desc->cpu_id,
				expire_jiffies < current_jiffies ? 1 : 0, entry_idx,
				expire_jiffies, current_jiffies); 
#endif
		return true;
	} else  {
		return false;
	}
}

static int
zicio_ghost_find_unmappable_entry(zicio_descriptor *desc, 
	int current_user_buffer_idx, int *unmap_idx)
{
	zicio_switch_board *msb = desc->switch_board;
	int expected;
	unsigned long unmappable_idx = -1;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	unsigned long start_iter;
#endif
	int idx;

	/*
	 * The consumption point can be increased by forceful unmap etc. That is,
	 * information on the exact point where the user is currently consuming is
	 * required. Here is the code fix for this.
	 */
	if (current_user_buffer_idx < *unmap_idx)
		return unmappable_idx;

	/* Get expected value */
	idx = *unmap_idx % ZICIO_MAX_NUM_GHOST_ENTRY;
	expected = zicio_read_status(msb, idx); 

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	start_iter = GET_UNMAPPING_ITER(desc);
	idx = start_iter % ZICIO_MAX_NUM_GHOST_ENTRY;
	printk("cpu_id[%d] unmap try iter: %lu, idx %d, status %d [%s:%d] [%s]\n", 
			desc->cpu_id, start_iter, idx, expected, __FILE__, __LINE__,
			__FUNCTION__);
#endif

	if (expected == ENTRY_DONE) {
		/* 
		 * Status change: ENTRY_DONE -> ENTRY_EMPTY
		 *
		 * This code can be executed SoftIRQ Daemon and interrupt handler.
		 * This means this code segment can be executed as if it is in a
		 * parallel program. We should handle it.
		 */
		mb();
		if (!zicio_cmpxchg_status(msb, idx, ENTRY_EMPTY, expected)) {
			return unmappable_idx;
		}

		unmappable_idx = idx;
		INCREMENT_UNMAPPING_ITER(desc);

		(*unmap_idx)++;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk("cpu_id[%d] unmap idx: %d, status %d [%s:%d] [%s]\n", 
				desc->cpu_id, idx, expected, __FILE__, __LINE__,
				__FUNCTION__);
#endif
	} else {
		/* 
		 * Fail: ENTRY_READY -> ENTRY_INUSE (by user) 
		 *
		 * We fail to change the value from ENTRY_READY to ENTRY_EMPTY. 
		 * We assume that the user get this chunk and is reading. 
		 * So, we just go to the next entry. 
		 */
	}

	return unmappable_idx;
}

/*
 * zicio_ghost_unmapping_hugepage
 * @desc: zicio descriptor
 * @unmappable_entry_idx: entry idx to unmap
 *
 * ZicIO does unmapping using ghost table. In here, @unmappable_entry_idx 
 * is index number for mapping inside ghost table. @page is the structure of 
 * page to insert.
 *
 * Return NULL for an error.
 * Return unmapped kernel virtual address
 */
void *
zicio_ghost_unmapping_hugepage(zicio_descriptor *desc, 
	int unmappable_entry_idx)
{
	struct mm_struct *mm = zicio_current(desc)->mm;
	struct vm_area_struct *vma;
	pud_t *pud;
	pmd_t *pmd;
	unsigned long user_base_address;
	unsigned long haddr;
	unsigned long flags;
	int cpu;
	struct page *page = NULL;

	user_base_address = zicio_get_user_base_address(desc);

	/* find vma struct */
	if (in_serving_softirq()) {
		mmap_read_lock(mm);
	}
	vma = zicio_find_vma(mm, user_base_address);

	if (in_serving_softirq()) {
		mmap_read_unlock(mm);
	}

	if (unlikely(!vma))
		goto l__zicio_forceful_unmapping_hugepage_read_unlock;

	if (unlikely(user_base_address < vma->vm_start))
		goto l__zicio_forceful_unmapping_hugepage_read_unlock;

	/* Calculate n'th entry of user address */
	haddr = 
		((user_base_address + (unmappable_entry_idx << HPAGE_PMD_SHIFT)) 
			& HPAGE_PMD_MASK);

	/* Get pud entry for ghost table */
	pud = zicio_get_pud(desc);

	/* Get pud */
	pmd = pmd_offset(pud, haddr);
	
	/* Get huge page, 2 MiB */
	page = pmd_page(*pmd);

	{
		pmd_t entry;

		pmd_clear(&entry);

		/* set flags */
		entry = pmd_set_flags(entry, _PAGE_ZICIO_UNMAPPING | _PAGE_PSE);

		*pmd = entry;

		cpu = get_cpu();

		/*
		 * TLB flush
		 * For tlb flush in softirq in other process don't need to be
		 * executed.
		 * This is because the channel's tlb was flushed during the context
		 * switching process. Also, softIRQ for a channel is done by
		 * specifying the cpu the channel is using.
		 */
		if (in_serving_softirq()) {
			local_irq_save(flags);
			if (current == desc->zicio_current) {
				zicio_flush_tlb_range(vma, haddr, haddr + HPAGE_PMD_SIZE,
						desc->cpu_id);
			}
			local_irq_restore(flags);
		} else {
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			local_irq_save(flags);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
			zicio_flush_tlb_range(vma, haddr, haddr + HPAGE_PMD_SIZE,
					desc->cpu_id);
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
			local_irq_restore(flags);
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */
		}

		put_cpu();
	}
l__zicio_forceful_unmapping_hugepage_read_unlock:

	return page == NULL ? NULL : page_to_virt(page);
}

/*
 * zicio_ghost_unmmaping - do unmapping
 * @desc: zicio descriptor
 * @forceful: do forceful unmapping
 * 
 * ZicIO tries to do forceful unmapping in here.
 *
 * Return @INVALID_FILE_CHUNK_ID, if there is no entry to unmap anymore.
 * Return unmapped file chunk id.
 */
static int zicio_ghost_unmapping(zicio_descriptor *desc,
	int current_user_buffer_idx, int *unmap_idx)
{
	int unmappable_entry_idx = -1;
	void *unmmaped_kern_addr;

	/* Find unmappable entry */
	unmappable_entry_idx =
		zicio_ghost_find_unmappable_entry(desc, current_user_buffer_idx,
				unmap_idx);
	if (unmappable_entry_idx < 0)
		return INVALID_FILE_CHUNK_ID;

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	printk("unmap idx: %d [%s:%d] [%s]\n", unmappable_entry_idx, 
		__FILE__, __LINE__, __FUNCTION__);
#endif

	/* We get unmappable entry here, just do it */
	unmmaped_kern_addr = 
			zicio_ghost_unmapping_hugepage(desc, unmappable_entry_idx);

	return unmappable_entry_idx;
}

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
void zicio_dump_premap_unmap_iter(zicio_descriptor *desc)
{
	unsigned long unmap_iter = GET_UNMAPPING_ITER(desc);
	unsigned long premap_iter = GET_PREMAPPING_ITER(desc);

	printk(KERN_WARNING "cpu_id[%d] unmap_iter: %lu premap_iter: %lu "
						"user_buffer_idx: %d\n", desc->cpu_id,
						unmap_iter, premap_iter,
                        zicio_get_user_buffer_idx(desc));
}
#endif

/*
 * zicio_unmap_pages_from_local_page_table
 * @desc: zicio descriptor
 */
void zicio_unmap_pages_from_local_page_table(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	unsigned long start_iter = GET_UNMAPPING_ITER(desc);
	int unmap_idx = start_iter % ZICIO_MAX_NUM_GHOST_ENTRY;
	int unmapped_user_buffer_idx;
	int current_user_buffer_idx;
	unsigned int current_file_chunk_id;
	unsigned int new_previous_low_premap_point =
			atomic_read(&zicio_channel->previous_low_premap_point);

	/* 
	 * We need to save current file chunk id read by user, because unmmaping
	 * before premapping may remove the current user buffer index's backpointer.
	 */
	current_user_buffer_idx = zicio_get_user_buffer_idx(desc);
	if (current_user_buffer_idx == INT_MAX) {
		return;
	}

	BUG_ON(current_user_buffer_idx >= ZICIO_MAX_NUM_GHOST_ENTRY);

	current_file_chunk_id =
			zicio_get_user_file_chunk_id(desc, current_user_buffer_idx);

	/*
	 * This chunk is already unmappad.
	 */
	if (current_file_chunk_id == UINT_MAX) {
		/*
		 * Forceful unmapped idx must not be referenced by user buffer index on
		 * the first unmap call.
		 */
		BUG_ON(zicio_read_status(desc->switch_board,
				current_user_buffer_idx) == ENTRY_DONE);
		return;
	}

	/*
	 * The consumption point can be increased by forceful unmap etc. That is,
	 * information on the exact point where the user is currently consuming is
	 * required. Here is the code fix for this.
	 */
	if (current_user_buffer_idx < unmap_idx) {
		/*
		 * Current user consumption point for unmapping.
		 */
		current_user_buffer_idx += ZICIO_MAX_NUM_GHOST_ENTRY;
	}

	while ((unmapped_user_buffer_idx = zicio_ghost_unmapping(desc,
			current_user_buffer_idx, &unmap_idx)) >= 0) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "current user buffer_idx in unmap : %d [%s:%d] "
				"[%s]\n", current_user_buffer_idx, __FILE__, __LINE__,
				__FUNCTION__);
#endif
		BUG_ON(unmapped_user_buffer_idx >= ZICIO_MAX_NUM_GHOST_ENTRY);

		current_file_chunk_id = zicio_get_user_file_chunk_id(desc,
				unmapped_user_buffer_idx);

		if (current_file_chunk_id != UINT_MAX) {
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
			zicio_set_turn_on_unmap_bitvector(desc, current_file_chunk_id);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
			current_file_chunk_id = zicio_convert_chunk_id_to_monotonic_id(
					zicio_shared_pool, zicio_shared_pool_local,
					current_file_chunk_id);
			if (new_previous_low_premap_point < current_file_chunk_id) {
				new_previous_low_premap_point = current_file_chunk_id;
			}
		}

		/* Clear resource of hashtable */
		zicio_clear_shared_page_control_block(desc,
			unmapped_user_buffer_idx);
	}
	/* Set premap low watermark for unmapped pages */
	atomic_set(&zicio_channel->previous_low_premap_point,
			new_previous_low_premap_point);
}

/*
 * zicio_entry_is_forceful_unmappable
 * 
 * Return INT_MAX, no forceful unmappable entry
 * Return idx, unmappable entry index
 */
static bool
zicio_ghost_find_forceful_unmappable_entry_idx(zicio_descriptor *desc,
	int *idx)
{
	zicio_switch_board *sb = desc->switch_board;
	int expected, new_val;
	int target_idx;

l_retry_zicio_ghost_find_forceful_unmappable_entry_idx:
	target_idx = *idx;

	BUG_ON(target_idx < 0 || ZICIO_MAX_NUM_GHOST_ENTRY <= target_idx);

	/* Get expected value */
	expected = zicio_read_status(sb, target_idx); 

	if (expected == ENTRY_DONE) {
		/* Move to next index */
		*idx = (*idx + 1) % ZICIO_MAX_NUM_GHOST_ENTRY;

		goto l_retry_zicio_ghost_find_forceful_unmappable_entry_idx;
	} else if (expected == ENTRY_READY && 
			zicio_ghost_entry_expired(desc, target_idx)) {

		/* Is this entry ready and expired? */
		new_val = ENTRY_DONE;

		mb();
		/* Change the status of chunk inside memory switchboard atomically */
		if (zicio_cas_status(sb, target_idx, expected, new_val) == 0) {
			/* 
		 	 * Success: ENTRY_READY -> ENTRY_DONE 
		 	 */
			return true;
		}

		/* Fail to compare and swap */
		goto l_retry_zicio_ghost_find_forceful_unmappable_entry_idx;
	} 

	return false;
}

/*
 * zicio_ghost_forceful_unmap - do forceful unmapping
 * @desc: zicio descriptor
 *
 * Return # of unmapped chunks
 */
unsigned int
zicio_ghost_forceful_unmap(zicio_descriptor *desc)
{
	zicio_shared_pool *zicio_shared_pool = zicio_get_shared_pool(desc);
	zicio_shared_pool_local *zicio_shared_pool_local =
			zicio_get_shared_pool_local(desc);
	zicio_attached_channel *zicio_channel = zicio_shared_pool_local->zicio_channel;
	int forceful_unmappable_idx;
	bool is_unmappable;
	int current_user_buffer_idx = zicio_get_user_buffer_idx(desc);
	unsigned unmmaped_cnt = 0;
	unsigned int file_chunk_id;
	unsigned int forcefully_unmapped_file_chunk_id =
			atomic_read(&zicio_channel->last_forcefully_unmapped_file_chunk_id);
	unsigned int unmapped_file_chunk_id, unmapped_idx_distance;
	zicio_bitvector *local_bitvector;
	unsigned int new_previous_low_premap_point =
			atomic_read(&zicio_channel->previous_low_premap_point);
#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	unsigned int cur_forcefully_unmapped_monotonic_chunk_id =
			UINT_MAX;
	unsigned int last_forcefully_unmapped_monotonic_chunk_id = 
			UINT_MAX;
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
	uint64_t exp_time = 0;
	zicio_shared_page_control_block *zicio_spcb = NULL;
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

	if (new_previous_low_premap_point == UINT_MAX) {
		return 0;
	}

	/* There is no user buffer ingestion point */
	if (current_user_buffer_idx == INT_MAX)
		return 0;

	/* Get user buffer ingesion point's next */
	forceful_unmappable_idx = 
		(current_user_buffer_idx + 1) % ZICIO_MAX_NUM_GHOST_ENTRY;

	local_bitvector = zicio_get_local_bitvector(desc);

	for (;;) {
		/* Find unmappable entry and set status to DONE */
		is_unmappable =
			zicio_ghost_find_forceful_unmappable_entry_idx(desc, 
				&forceful_unmappable_idx);
		if (!is_unmappable)
			break;

		mb();
		file_chunk_id = zicio_get_user_file_chunk_id(desc,
					forceful_unmappable_idx);

		BUG_ON(file_chunk_id == UINT_MAX);

		BUG_ON(!zicio_get_bit_status(local_bitvector, file_chunk_id, false,
				0));
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 3)
		zicio_set_turn_on_unmap_bitvector(desc, file_chunk_id);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
		/* Clear bit vector, because user don't read forceful unmapped chunk */
		zicio_clear_bitvector_atomic(local_bitvector, file_chunk_id,
			ZICIO_BIT_REF, false);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		zicio_spcb = zicio_get_shared_page_control_block(desc,
				forceful_unmappable_idx);
		exp_time = atomic64_read(&zicio_spcb->zicio_spcb.exp_jiffies);
		mb();
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */
#endif /* CONFIG_ZICIO_OPTIMIZE_SHARED_MODE */

		zicio_clear_shared_page_control_block(desc,
					forceful_unmappable_idx);
		zicio_dec_mapped_chunks_num(desc);

		file_chunk_id = zicio_convert_chunk_id_to_monotonic_id(
				zicio_shared_pool, zicio_shared_pool_local, file_chunk_id);

		/*
		 * When performing a forceful unmap, we update the low premap watermark
		 * for chunks with larger ids. This is because chunks smaller than the
		 * corresponding id will have expired.
		 */
		if (new_previous_low_premap_point < file_chunk_id) {
			new_previous_low_premap_point = file_chunk_id;
		}

		zicio_get_tracking_ingestion_point(desc, forceful_unmappable_idx,
				&unmapped_file_chunk_id, &unmapped_idx_distance);
		unmapped_file_chunk_id = zicio_convert_chunk_id_to_monotonic_id(
				zicio_shared_pool, zicio_shared_pool_local, unmapped_file_chunk_id);

		if (forcefully_unmapped_file_chunk_id < unmapped_file_chunk_id ||
			forcefully_unmapped_file_chunk_id == UINT_MAX) {
			forcefully_unmapped_file_chunk_id = unmapped_file_chunk_id;
		}

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
		cur_forcefully_unmapped_monotonic_chunk_id
			= zicio_get_tracking_ingestion_monotonic_chunk_id(desc,
				forceful_unmappable_idx);

#if (CONFIG_ZICIO_DEBUG_LEVEL >= 2)
		printk(KERN_WARNING "[ZICIO] cpu[%d] un-mapping, chunk id: %u, current jiffies: %lu, user buffer idx: %d, forceful unmap idx: %d, last_forcefully_unmapped_monotonic_chunk_id: %d, cur_forcefully_unmapped_monotonic_chunk_id: %d, exp time: %lu\n",
				desc->cpu_id,
				file_chunk_id % zicio_shared_pool->shared_files.total_chunk_nums,
				get_jiffies_64(),
				zicio_get_user_buffer_idx(desc),
				forceful_unmappable_idx,
				last_forcefully_unmapped_monotonic_chunk_id,
				cur_forcefully_unmapped_monotonic_chunk_id,
				exp_time);
#endif /* CONFIG_ZICIO_DEBUG_LEVEL */

		if ((last_forcefully_unmapped_monotonic_chunk_id <
				cur_forcefully_unmapped_monotonic_chunk_id) ||
			last_forcefully_unmapped_monotonic_chunk_id == UINT_MAX) {
			last_forcefully_unmapped_monotonic_chunk_id
				= cur_forcefully_unmapped_monotonic_chunk_id;
		}
#endif /* CONFIG_ZICIO_OPTMIZE_SHARED_MODE */

		/* Move next index */
		forceful_unmappable_idx = 
			(forceful_unmappable_idx + 1) % ZICIO_MAX_NUM_GHOST_ENTRY;
#ifdef CONFIG_ZICIO_STAT
		/* Update stat board */
		zicio_dec_endowed_pages(desc);
		zicio_add_forcefully_unmapped_pages(desc);
#endif
		unmmaped_cnt += 1;
	}

	/* Set premap low watermark for forcefully unmapped pages */
	atomic_set(&zicio_channel->previous_low_premap_point,
			new_previous_low_premap_point);
	atomic_set(&zicio_channel->last_forcefully_unmapped_file_chunk_id,
			forcefully_unmapped_file_chunk_id);

#ifdef CONFIG_ZICIO_OPTIMIZE_SHARED_MODE
	if (last_forcefully_unmapped_monotonic_chunk_id != UINT_MAX)
		atomic_set(&zicio_channel->last_forcefully_unmapped_monotonic_chunk_id,
				last_forcefully_unmapped_monotonic_chunk_id);
#endif /* CONFIG_ZICIO_OPTMIZE_SHARED_MODE */

	return unmmaped_cnt;
}

#endif /* CONFIG_ZICIO */
