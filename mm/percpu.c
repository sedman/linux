/*
 * mm/percpu.c - percpu memory allocator
 *
 * Copyright (C) 2009		SUSE Linux Products GmbH
 * Copyright (C) 2009		Tejun Heo <tj@kernel.org>
 *
 * This file is released under the GPLv2.
 *
 * This is percpu allocator which can handle both static and dynamic
 * areas.  Percpu areas are allocated in chunks.  Each chunk is
 * consisted of boot-time determined number of units and the first
 * chunk is used for static percpu variables in the kernel image
 * (special boot time alloc/init handling necessary as these areas
 * need to be brought up before allocation services are running).
 * Unit grows as necessary and all units grow or shrink in unison.
 * When a chunk is filled up, another chunk is allocated.
 *
 *  c0                           c1                         c2
 *  -------------------          -------------------        ------------
 * | u0 | u1 | u2 | u3 |        | u0 | u1 | u2 | u3 |      | u0 | u1 | u
 *  -------------------  ......  -------------------  ....  ------------
 *
 * Allocation is done in offset-size areas of single unit space.  Ie,
 * an area of 512 bytes at 6k in c1 occupies 512 bytes at 6k of c1:u0,
 * c1:u1, c1:u2 and c1:u3.  On UMA, units corresponds directly to
 * cpus.  On NUMA, the mapping can be non-linear and even sparse.
 * Percpu access can be done by configuring percpu base registers
 * according to cpu to unit mapping and pcpu_unit_size.
 *
 * There are usually many small percpu allocations many of them being
 * as small as 4 bytes.  The allocator organizes chunks into lists
 * according to free size and tries to allocate from the fullest one.
 * Each chunk keeps the maximum contiguous area size hint which is
 * guaranteed to be equal to or larger than the maximum contiguous
 * area in the chunk.  This helps the allocator not to iterate the
 * chunk maps unnecessarily.
 *
 * Allocation state in each chunk is kept using an array of integers
 * on chunk->map.  A positive value in the map represents a free
 * region and negative allocated.  Allocation inside a chunk is done
 * by scanning this map sequentially and serving the first matching
 * entry.  This is mostly copied from the percpu_modalloc() allocator.
 * Chunks can be determined from the address using the index field
 * in the page struct. The index field contains a pointer to the chunk.
 *
 * To use this allocator, arch code should do the followings.
 *
 * - define __addr_to_pcpu_ptr() and __pcpu_ptr_to_addr() to translate
 *   regular address to percpu pointer and back if they need to be
 *   different from the default
 *
 * - use pcpu_setup_first_chunk() during percpu area initialization to
 *   setup the first chunk containing the kernel static percpu area
 */

#include <linux/bitmap.h>
#include <linux/bootmem.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/kmemleak.h>

#include <asm/cacheflush.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/io.h>

#define PCPU_SLOT_BASE_SHIFT		5	/* 1-31 shares the same slot */
#define PCPU_DFL_MAP_ALLOC		16	/* start a map with 16 ents */

#ifdef CONFIG_SMP
/* default addr <-> pcpu_ptr mapping, override in asm/percpu.h if necessary */
#ifndef __addr_to_pcpu_ptr
#define __addr_to_pcpu_ptr(addr)					\
	(void __percpu *)((unsigned long)(addr) -			\
			  (unsigned long)pcpu_base_addr	+		\
			  (unsigned long)__per_cpu_start)
#endif
#ifndef __pcpu_ptr_to_addr
#define __pcpu_ptr_to_addr(ptr)						\
	(void __force *)((unsigned long)(ptr) +				\
			 (unsigned long)pcpu_base_addr -		\
			 (unsigned long)__per_cpu_start)
#endif
#else	/* CONFIG_SMP */
/* on UP, it's always identity mapped */
#define __addr_to_pcpu_ptr(addr)	(void __percpu *)(addr)
#define __pcpu_ptr_to_addr(ptr)		(void __force *)(ptr)
#endif	/* CONFIG_SMP */

struct pcpu_chunk {
	struct list_head	list;		/* linked to pcpu_slot lists */
	int			free_size;	/* free bytes in the chunk */
	int			contig_hint;	/* max contiguous size hint */
	void			*base_addr;	/* base address of this chunk */
	int			map_used;	/* # of map entries used */
	int			map_alloc;	/* # of map entries allocated */
	int			*map;		/* allocation map */
	void			*data;		/* chunk data */
	bool			immutable;	/* no [de]population allowed */
	unsigned long		populated[];	/* populated bitmap */
};

static int pcpu_unit_pages __read_mostly;
static int pcpu_unit_size __read_mostly;
static int pcpu_nr_units __read_mostly;
static int pcpu_atom_size __read_mostly;
static int pcpu_nr_slots __read_mostly;
static size_t pcpu_chunk_struct_size __read_mostly;

/* cpus with the lowest and highest unit addresses */
static unsigned int pcpu_low_unit_cpu __read_mostly;
static unsigned int pcpu_high_unit_cpu __read_mostly;

/* the address of the first chunk which starts with the kernel static area */
/** 20130629    
 * pcpu_setup_first_chunk에서 초기값 설정
 **/
void *pcpu_base_addr __read_mostly;
EXPORT_SYMBOL_GPL(pcpu_base_addr);

static const int *pcpu_unit_map __read_mostly;		/* cpu -> unit */
/** 20130629    
 * cpu를 index 했을 때 unit의 위치를 저장한 배열 주소
 * pcpu_setup_first_chunk 에서 설정
 **/
const unsigned long *pcpu_unit_offsets __read_mostly;	/* cpu -> unit offset */

/* group information, used for vm allocation */
static int pcpu_nr_groups __read_mostly;
static const unsigned long *pcpu_group_offsets __read_mostly;
static const size_t *pcpu_group_sizes __read_mostly;

/*
 * The first chunk which always exists.  Note that unlike other
 * chunks, this one can be allocated and mapped in several different
 * ways and thus often doesn't live in the vmalloc area.
 */
/** 20130629    
 * pcpu_setup_first_chunk 에서 first chunk를 bootmem에서 할당받아 설정.
 **/
static struct pcpu_chunk *pcpu_first_chunk;

/*
 * Optional reserved chunk.  This chunk reserves part of the first
 * chunk and serves it for reserved allocations.  The amount of
 * reserved offset is in pcpu_reserved_chunk_limit.  When reserved
 * area doesn't exist, the following variables contain NULL and 0
 * respectively.
 */
static struct pcpu_chunk *pcpu_reserved_chunk;
static int pcpu_reserved_chunk_limit;

/*
 * Synchronization rules.
 *
 * There are two locks - pcpu_alloc_mutex and pcpu_lock.  The former
 * protects allocation/reclaim paths, chunks, populated bitmap and
 * vmalloc mapping.  The latter is a spinlock and protects the index
 * data structures - chunk slots, chunks and area maps in chunks.
 *
 * During allocation, pcpu_alloc_mutex is kept locked all the time and
 * pcpu_lock is grabbed and released as necessary.  All actual memory
 * allocations are done using GFP_KERNEL with pcpu_lock released.  In
 * general, percpu memory can't be allocated with irq off but
 * irqsave/restore are still used in alloc path so that it can be used
 * from early init path - sched_init() specifically.
 *
 * Free path accesses and alters only the index data structures, so it
 * can be safely called from atomic context.  When memory needs to be
 * returned to the system, free path schedules reclaim_work which
 * grabs both pcpu_alloc_mutex and pcpu_lock, unlinks chunks to be
 * reclaimed, release both locks and frees the chunks.  Note that it's
 * necessary to grab both locks to remove a chunk from circulation as
 * allocation path might be referencing the chunk with only
 * pcpu_alloc_mutex locked.
 */
static DEFINE_MUTEX(pcpu_alloc_mutex);	/* protects whole alloc and reclaim */
static DEFINE_SPINLOCK(pcpu_lock);	/* protects index data structures */

static struct list_head *pcpu_slot __read_mostly; /* chunk list slots */

/* reclaim work to release fully free chunks, scheduled from free path */
static void pcpu_reclaim(struct work_struct *work);
static DECLARE_WORK(pcpu_reclaim_work, pcpu_reclaim);

static bool pcpu_addr_in_first_chunk(void *addr)
{
	void *first_start = pcpu_first_chunk->base_addr;

	return addr >= first_start && addr < first_start + pcpu_unit_size;
}

static bool pcpu_addr_in_reserved_chunk(void *addr)
{
	void *first_start = pcpu_first_chunk->base_addr;

	return addr >= first_start &&
		addr < first_start + pcpu_reserved_chunk_limit;
}
/** 20130622
사이즈에 해당하는 slot index반환???
1. size의 값에서 0이 아닌 첫번째 위치를 구해서 highbit에 저장 (지수승을 구하는듯)
	0b10001001 -> 8 
2. highbit 에 PCPU_SLOT_BASE_SHIFT를 빼서 2를 더한다.
	- PCPU_SLOT_BASE_SHIFT는 unit 32(2^5)개가 하나의 slot이므로 비트 단위로 그룹 표현할때 필요한 SHIFT 인듯??? 
	- 여기서 2를 더하는 이유는??? 
- 그런데 어떻게,왜 slot의 인덱스가 구해지는지 모르겠음.....???
max로 인해 slot사이즈는 최소 1이상여야함.
**/
static int __pcpu_size_to_slot(int size)
{
	int highbit = fls(size);	/* size is in bytes */
	return max(highbit - PCPU_SLOT_BASE_SHIFT + 2, 1);
}

/** 20130622
	size와 pcpu_unit_size와 같다면 slot의 마지막 인덱스 반환
	아니면 __pcpu_size_to_slot으로 인덱스 계산
**/
static int pcpu_size_to_slot(int size)
{
	if (size == pcpu_unit_size)
		return pcpu_nr_slots - 1;
	return __pcpu_size_to_slot(size);
}

/** 20130622
	chunk의 free_size(chunk내의 가용 여유 공간의 합)으로 slot index를 구하는 함수 call
**/
static int pcpu_chunk_slot(const struct pcpu_chunk *chunk)
{
	if (chunk->free_size < sizeof(int) || chunk->contig_hint < sizeof(int))
		return 0;

	return pcpu_size_to_slot(chunk->free_size);
}

/* set the pointer to a chunk in a page struct */
static void pcpu_set_page_chunk(struct page *page, struct pcpu_chunk *pcpu)
{
	page->index = (unsigned long)pcpu;
}

/* obtain pointer to a chunk from a page struct */
static struct pcpu_chunk *pcpu_get_page_chunk(struct page *page)
{
	return (struct pcpu_chunk *)page->index;
}

static int __maybe_unused pcpu_page_idx(unsigned int cpu, int page_idx)
{
	return pcpu_unit_map[cpu] * pcpu_unit_pages + page_idx;
}

static unsigned long pcpu_chunk_addr(struct pcpu_chunk *chunk,
				     unsigned int cpu, int page_idx)
{
	return (unsigned long)chunk->base_addr + pcpu_unit_offsets[cpu] +
		(page_idx << PAGE_SHIFT);
}

static void __maybe_unused pcpu_next_unpop(struct pcpu_chunk *chunk,
					   int *rs, int *re, int end)
{
	*rs = find_next_zero_bit(chunk->populated, end, *rs);
	*re = find_next_bit(chunk->populated, end, *rs + 1);
}

static void __maybe_unused pcpu_next_pop(struct pcpu_chunk *chunk,
					 int *rs, int *re, int end)
{
	*rs = find_next_bit(chunk->populated, end, *rs);
	*re = find_next_zero_bit(chunk->populated, end, *rs + 1);
}

/*
 * (Un)populated page region iterators.  Iterate over (un)populated
 * page regions between @start and @end in @chunk.  @rs and @re should
 * be integer variables and will be set to start and end page index of
 * the current region.
 */
#define pcpu_for_each_unpop_region(chunk, rs, re, start, end)		    \
	for ((rs) = (start), pcpu_next_unpop((chunk), &(rs), &(re), (end)); \
	     (rs) < (re);						    \
	     (rs) = (re) + 1, pcpu_next_unpop((chunk), &(rs), &(re), (end)))

#define pcpu_for_each_pop_region(chunk, rs, re, start, end)		    \
	for ((rs) = (start), pcpu_next_pop((chunk), &(rs), &(re), (end));   \
	     (rs) < (re);						    \
	     (rs) = (re) + 1, pcpu_next_pop((chunk), &(rs), &(re), (end)))

/**
 * pcpu_mem_zalloc - allocate memory
 * @size: bytes to allocate
 *
 * Allocate @size bytes.  If @size is smaller than PAGE_SIZE,
 * kzalloc() is used; otherwise, vzalloc() is used.  The returned
 * memory is always zeroed.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * Pointer to the allocated area on success, NULL on failure.
 */
static void *pcpu_mem_zalloc(size_t size)
{
	if (WARN_ON_ONCE(!slab_is_available()))
		return NULL;

	if (size <= PAGE_SIZE)
		return kzalloc(size, GFP_KERNEL);
	else
		return vzalloc(size);
}

/**
 * pcpu_mem_free - free memory
 * @ptr: memory to free
 * @size: size of the area
 *
 * Free @ptr.  @ptr should have been allocated using pcpu_mem_zalloc().
 */
static void pcpu_mem_free(void *ptr, size_t size)
{
	if (size <= PAGE_SIZE)
		kfree(ptr);
	else
		vfree(ptr);
}

/**
 * pcpu_chunk_relocate - put chunk in the appropriate chunk slot
 * @chunk: chunk of interest
 * @oslot: the previous slot it was on
 *
 * This function is called after an allocation or free changed @chunk.
 * New slot according to the changed state is determined and @chunk is
 * moved to the slot.  Note that the reserved chunk is never put on
 * chunk slots.
 *
 * CONTEXT:
 * pcpu_lock.
 */
 /** 20130622
	1. chunk의 slot index를 구한다.
	2. 인자의 oslot와 구한 nslot를 비교하여
		oslot이 작으면 chunk를 head의 첫번째 노드에 추가
		아니면 Tail에 추가
 **/
static void pcpu_chunk_relocate(struct pcpu_chunk *chunk, int oslot)
{
	int nslot = pcpu_chunk_slot(chunk);

	if (chunk != pcpu_reserved_chunk && oslot != nslot) {
		if (oslot < nslot)
			list_move(&chunk->list, &pcpu_slot[nslot]);
		else
			list_move_tail(&chunk->list, &pcpu_slot[nslot]);
	}
}

/**
 * pcpu_need_to_extend - determine whether chunk area map needs to be extended
 * @chunk: chunk of interest
 *
 * Determine whether area map of @chunk needs to be extended to
 * accommodate a new allocation.
 *
 * CONTEXT:
 * pcpu_lock.
 *
 * RETURNS:
 * New target map allocation length if extension is necessary, 0
 * otherwise.
 */
static int pcpu_need_to_extend(struct pcpu_chunk *chunk)
{
	int new_alloc;

	if (chunk->map_alloc >= chunk->map_used + 2)
		return 0;

	new_alloc = PCPU_DFL_MAP_ALLOC;
	while (new_alloc < chunk->map_used + 2)
		new_alloc *= 2;

	return new_alloc;
}

/**
 * pcpu_extend_area_map - extend area map of a chunk
 * @chunk: chunk of interest
 * @new_alloc: new target allocation length of the area map
 *
 * Extend area map of @chunk to have @new_alloc entries.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.  Grabs and releases pcpu_lock.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
static int pcpu_extend_area_map(struct pcpu_chunk *chunk, int new_alloc)
{
	int *old = NULL, *new = NULL;
	size_t old_size = 0, new_size = new_alloc * sizeof(new[0]);
	unsigned long flags;

	new = pcpu_mem_zalloc(new_size);
	if (!new)
		return -ENOMEM;

	/* acquire pcpu_lock and switch to new area map */
	spin_lock_irqsave(&pcpu_lock, flags);

	if (new_alloc <= chunk->map_alloc)
		goto out_unlock;

	old_size = chunk->map_alloc * sizeof(chunk->map[0]);
	old = chunk->map;

	memcpy(new, old, old_size);

	chunk->map_alloc = new_alloc;
	chunk->map = new;
	new = NULL;

out_unlock:
	spin_unlock_irqrestore(&pcpu_lock, flags);

	/*
	 * pcpu_mem_free() might end up calling vfree() which uses
	 * IRQ-unsafe lock and thus can't be called under pcpu_lock.
	 */
	pcpu_mem_free(old, old_size);
	pcpu_mem_free(new, new_size);

	return 0;
}

/**
 * pcpu_split_block - split a map block
 * @chunk: chunk of interest
 * @i: index of map block to split
 * @head: head size in bytes (can be 0)
 * @tail: tail size in bytes (can be 0)
 *
 * Split the @i'th map block into two or three blocks.  If @head is
 * non-zero, @head bytes block is inserted before block @i moving it
 * to @i+1 and reducing its size by @head bytes.
 *
 * If @tail is non-zero, the target block, which can be @i or @i+1
 * depending on @head, is reduced by @tail bytes and @tail byte block
 * is inserted after the target block.
 *
 * @chunk->map must have enough free slots to accommodate the split.
 *
 * CONTEXT:
 * pcpu_lock.
 */
static void pcpu_split_block(struct pcpu_chunk *chunk, int i,
			     int head, int tail)
{
	int nr_extra = !!head + !!tail;

	BUG_ON(chunk->map_alloc < chunk->map_used + nr_extra);

	/* insert new subblocks */
	memmove(&chunk->map[i + nr_extra], &chunk->map[i],
		sizeof(chunk->map[0]) * (chunk->map_used - i));
	chunk->map_used += nr_extra;

	if (head) {
		chunk->map[i + 1] = chunk->map[i] - head;
		chunk->map[i++] = head;
	}
	if (tail) {
		chunk->map[i++] -= tail;
		chunk->map[i] = tail;
	}
}

/**
 * pcpu_alloc_area - allocate area from a pcpu_chunk
 * @chunk: chunk of interest
 * @size: wanted size in bytes
 * @align: wanted align
 *
 * Try to allocate @size bytes area aligned at @align from @chunk.
 * Note that this function only allocates the offset.  It doesn't
 * populate or map the area.
 *
 * @chunk->map must have at least two free slots.
 *
 * CONTEXT:
 * pcpu_lock.
 *
 * RETURNS:
 * Allocated offset in @chunk on success, -1 if no matching area is
 * found.
 */
static int pcpu_alloc_area(struct pcpu_chunk *chunk, int size, int align)
{
	int oslot = pcpu_chunk_slot(chunk);
	int max_contig = 0;
	int i, off;

	for (i = 0, off = 0; i < chunk->map_used; off += abs(chunk->map[i++])) {
		bool is_last = i + 1 == chunk->map_used;
		int head, tail;

		/* extra for alignment requirement */
		head = ALIGN(off, align) - off;
		BUG_ON(i == 0 && head != 0);

		if (chunk->map[i] < 0)
			continue;
		if (chunk->map[i] < head + size) {
			max_contig = max(chunk->map[i], max_contig);
			continue;
		}

		/*
		 * If head is small or the previous block is free,
		 * merge'em.  Note that 'small' is defined as smaller
		 * than sizeof(int), which is very small but isn't too
		 * uncommon for percpu allocations.
		 */
		if (head && (head < sizeof(int) || chunk->map[i - 1] > 0)) {
			if (chunk->map[i - 1] > 0)
				chunk->map[i - 1] += head;
			else {
				chunk->map[i - 1] -= head;
				chunk->free_size -= head;
			}
			chunk->map[i] -= head;
			off += head;
			head = 0;
		}

		/* if tail is small, just keep it around */
		tail = chunk->map[i] - head - size;
		if (tail < sizeof(int))
			tail = 0;

		/* split if warranted */
		if (head || tail) {
			pcpu_split_block(chunk, i, head, tail);
			if (head) {
				i++;
				off += head;
				max_contig = max(chunk->map[i - 1], max_contig);
			}
			if (tail)
				max_contig = max(chunk->map[i + 1], max_contig);
		}

		/* update hint and mark allocated */
		if (is_last)
			chunk->contig_hint = max_contig; /* fully scanned */
		else
			chunk->contig_hint = max(chunk->contig_hint,
						 max_contig);

		chunk->free_size -= chunk->map[i];
		chunk->map[i] = -chunk->map[i];

		pcpu_chunk_relocate(chunk, oslot);
		return off;
	}

	chunk->contig_hint = max_contig;	/* fully scanned */
	pcpu_chunk_relocate(chunk, oslot);

	/* tell the upper layer that this chunk has no matching area */
	return -1;
}

/**
 * pcpu_free_area - free area to a pcpu_chunk
 * @chunk: chunk of interest
 * @freeme: offset of area to free
 *
 * Free area starting from @freeme to @chunk.  Note that this function
 * only modifies the allocation map.  It doesn't depopulate or unmap
 * the area.
 *
 * CONTEXT:
 * pcpu_lock.
 */
static void pcpu_free_area(struct pcpu_chunk *chunk, int freeme)
{
	int oslot = pcpu_chunk_slot(chunk);
	int i, off;

	for (i = 0, off = 0; i < chunk->map_used; off += abs(chunk->map[i++]))
		if (off == freeme)
			break;
	BUG_ON(off != freeme);
	BUG_ON(chunk->map[i] > 0);

	chunk->map[i] = -chunk->map[i];
	chunk->free_size += chunk->map[i];

	/* merge with previous? */
	if (i > 0 && chunk->map[i - 1] >= 0) {
		chunk->map[i - 1] += chunk->map[i];
		chunk->map_used--;
		memmove(&chunk->map[i], &chunk->map[i + 1],
			(chunk->map_used - i) * sizeof(chunk->map[0]));
		i--;
	}
	/* merge with next? */
	if (i + 1 < chunk->map_used && chunk->map[i + 1] >= 0) {
		chunk->map[i] += chunk->map[i + 1];
		chunk->map_used--;
		memmove(&chunk->map[i + 1], &chunk->map[i + 2],
			(chunk->map_used - (i + 1)) * sizeof(chunk->map[0]));
	}

	chunk->contig_hint = max(chunk->map[i], chunk->contig_hint);
	pcpu_chunk_relocate(chunk, oslot);
}

static struct pcpu_chunk *pcpu_alloc_chunk(void)
{
	struct pcpu_chunk *chunk;

	chunk = pcpu_mem_zalloc(pcpu_chunk_struct_size);
	if (!chunk)
		return NULL;

	chunk->map = pcpu_mem_zalloc(PCPU_DFL_MAP_ALLOC *
						sizeof(chunk->map[0]));
	if (!chunk->map) {
		kfree(chunk);
		return NULL;
	}

	chunk->map_alloc = PCPU_DFL_MAP_ALLOC;
	chunk->map[chunk->map_used++] = pcpu_unit_size;

	INIT_LIST_HEAD(&chunk->list);
	chunk->free_size = pcpu_unit_size;
	chunk->contig_hint = pcpu_unit_size;

	return chunk;
}

static void pcpu_free_chunk(struct pcpu_chunk *chunk)
{
	if (!chunk)
		return;
	pcpu_mem_free(chunk->map, chunk->map_alloc * sizeof(chunk->map[0]));
	kfree(chunk);
}

/*
 * Chunk management implementation.
 *
 * To allow different implementations, chunk alloc/free and
 * [de]population are implemented in a separate file which is pulled
 * into this file and compiled together.  The following functions
 * should be implemented.
 *
 * pcpu_populate_chunk		- populate the specified range of a chunk
 * pcpu_depopulate_chunk	- depopulate the specified range of a chunk
 * pcpu_create_chunk		- create a new chunk
 * pcpu_destroy_chunk		- destroy a chunk, always preceded by full depop
 * pcpu_addr_to_page		- translate address to physical address
 * pcpu_verify_alloc_info	- check alloc_info is acceptable during init
 */
static int pcpu_populate_chunk(struct pcpu_chunk *chunk, int off, int size);
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk, int off, int size);
static struct pcpu_chunk *pcpu_create_chunk(void);
static void pcpu_destroy_chunk(struct pcpu_chunk *chunk);
static struct page *pcpu_addr_to_page(void *addr);
static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai);

#ifdef CONFIG_NEED_PER_CPU_KM
#include "percpu-km.c"
#else
#include "percpu-vm.c"
#endif

/**
 * pcpu_chunk_addr_search - determine chunk containing specified address
 * @addr: address for which the chunk needs to be determined.
 *
 * RETURNS:
 * The address of the found chunk.
 */
static struct pcpu_chunk *pcpu_chunk_addr_search(void *addr)
{
	/* is it in the first chunk? */
	if (pcpu_addr_in_first_chunk(addr)) {
		/* is it in the reserved area? */
		if (pcpu_addr_in_reserved_chunk(addr))
			return pcpu_reserved_chunk;
		return pcpu_first_chunk;
	}

	/*
	 * The address is relative to unit0 which might be unused and
	 * thus unmapped.  Offset the address to the unit space of the
	 * current processor before looking it up in the vmalloc
	 * space.  Note that any possible cpu id can be used here, so
	 * there's no need to worry about preemption or cpu hotplug.
	 */
	addr += pcpu_unit_offsets[raw_smp_processor_id()];
	return pcpu_get_page_chunk(pcpu_addr_to_page(addr));
}

/**
 * pcpu_alloc - the percpu allocator
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 * @reserved: allocate from the reserved chunk if available
 *
 * Allocate percpu area of @size bytes aligned at @align.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * Percpu pointer to the allocated area on success, NULL on failure.
 */
static void __percpu *pcpu_alloc(size_t size, size_t align, bool reserved)
{
	static int warn_limit = 10;
	struct pcpu_chunk *chunk;
	const char *err;
	int slot, off, new_alloc;
	unsigned long flags;
	void __percpu *ptr;

	if (unlikely(!size || size > PCPU_MIN_UNIT_SIZE || align > PAGE_SIZE)) {
		WARN(true, "illegal size (%zu) or align (%zu) for "
		     "percpu allocation\n", size, align);
		return NULL;
	}

	mutex_lock(&pcpu_alloc_mutex);
	spin_lock_irqsave(&pcpu_lock, flags);

	/* serve reserved allocations from the reserved chunk if available */
	if (reserved && pcpu_reserved_chunk) {
		chunk = pcpu_reserved_chunk;

		if (size > chunk->contig_hint) {
			err = "alloc from reserved chunk failed";
			goto fail_unlock;
		}

		while ((new_alloc = pcpu_need_to_extend(chunk))) {
			spin_unlock_irqrestore(&pcpu_lock, flags);
			if (pcpu_extend_area_map(chunk, new_alloc) < 0) {
				err = "failed to extend area map of reserved chunk";
				goto fail_unlock_mutex;
			}
			spin_lock_irqsave(&pcpu_lock, flags);
		}

		off = pcpu_alloc_area(chunk, size, align);
		if (off >= 0)
			goto area_found;

		err = "alloc from reserved chunk failed";
		goto fail_unlock;
	}

restart:
	/* search through normal chunks */
	for (slot = pcpu_size_to_slot(size); slot < pcpu_nr_slots; slot++) {
		list_for_each_entry(chunk, &pcpu_slot[slot], list) {
			if (size > chunk->contig_hint)
				continue;

			new_alloc = pcpu_need_to_extend(chunk);
			if (new_alloc) {
				spin_unlock_irqrestore(&pcpu_lock, flags);
				if (pcpu_extend_area_map(chunk,
							 new_alloc) < 0) {
					err = "failed to extend area map";
					goto fail_unlock_mutex;
				}
				spin_lock_irqsave(&pcpu_lock, flags);
				/*
				 * pcpu_lock has been dropped, need to
				 * restart cpu_slot list walking.
				 */
				goto restart;
			}

			off = pcpu_alloc_area(chunk, size, align);
			if (off >= 0)
				goto area_found;
		}
	}

	/* hmmm... no space left, create a new chunk */
	spin_unlock_irqrestore(&pcpu_lock, flags);

	chunk = pcpu_create_chunk();
	if (!chunk) {
		err = "failed to allocate new chunk";
		goto fail_unlock_mutex;
	}

	spin_lock_irqsave(&pcpu_lock, flags);
	pcpu_chunk_relocate(chunk, -1);
	goto restart;

area_found:
	spin_unlock_irqrestore(&pcpu_lock, flags);

	/* populate, map and clear the area */
	if (pcpu_populate_chunk(chunk, off, size)) {
		spin_lock_irqsave(&pcpu_lock, flags);
		pcpu_free_area(chunk, off);
		err = "failed to populate";
		goto fail_unlock;
	}

	mutex_unlock(&pcpu_alloc_mutex);

	/* return address relative to base address */
	ptr = __addr_to_pcpu_ptr(chunk->base_addr + off);
	kmemleak_alloc_percpu(ptr, size);
	return ptr;

fail_unlock:
	spin_unlock_irqrestore(&pcpu_lock, flags);
fail_unlock_mutex:
	mutex_unlock(&pcpu_alloc_mutex);
	if (warn_limit) {
		pr_warning("PERCPU: allocation failed, size=%zu align=%zu, "
			   "%s\n", size, align, err);
		dump_stack();
		if (!--warn_limit)
			pr_info("PERCPU: limit reached, disable warning\n");
	}
	return NULL;
}

/**
 * __alloc_percpu - allocate dynamic percpu area
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 *
 * Allocate zero-filled percpu area of @size bytes aligned at @align.
 * Might sleep.  Might trigger writeouts.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * Percpu pointer to the allocated area on success, NULL on failure.
 */
void __percpu *__alloc_percpu(size_t size, size_t align)
{
	return pcpu_alloc(size, align, false);
}
EXPORT_SYMBOL_GPL(__alloc_percpu);

/**
 * __alloc_reserved_percpu - allocate reserved percpu area
 * @size: size of area to allocate in bytes
 * @align: alignment of area (max PAGE_SIZE)
 *
 * Allocate zero-filled percpu area of @size bytes aligned at @align
 * from reserved percpu area if arch has set it up; otherwise,
 * allocation is served from the same dynamic area.  Might sleep.
 * Might trigger writeouts.
 *
 * CONTEXT:
 * Does GFP_KERNEL allocation.
 *
 * RETURNS:
 * Percpu pointer to the allocated area on success, NULL on failure.
 */
void __percpu *__alloc_reserved_percpu(size_t size, size_t align)
{
	return pcpu_alloc(size, align, true);
}

/**
 * pcpu_reclaim - reclaim fully free chunks, workqueue function
 * @work: unused
 *
 * Reclaim all fully free chunks except for the first one.
 *
 * CONTEXT:
 * workqueue context.
 */
static void pcpu_reclaim(struct work_struct *work)
{
	LIST_HEAD(todo);
	struct list_head *head = &pcpu_slot[pcpu_nr_slots - 1];
	struct pcpu_chunk *chunk, *next;

	mutex_lock(&pcpu_alloc_mutex);
	spin_lock_irq(&pcpu_lock);

	list_for_each_entry_safe(chunk, next, head, list) {
		WARN_ON(chunk->immutable);

		/* spare the first one */
		if (chunk == list_first_entry(head, struct pcpu_chunk, list))
			continue;

		list_move(&chunk->list, &todo);
	}

	spin_unlock_irq(&pcpu_lock);

	list_for_each_entry_safe(chunk, next, &todo, list) {
		pcpu_depopulate_chunk(chunk, 0, pcpu_unit_size);
		pcpu_destroy_chunk(chunk);
	}

	mutex_unlock(&pcpu_alloc_mutex);
}

/**
 * free_percpu - free percpu area
 * @ptr: pointer to area to free
 *
 * Free percpu area @ptr.
 *
 * CONTEXT:
 * Can be called from atomic context.
 */
void free_percpu(void __percpu *ptr)
{
	void *addr;
	struct pcpu_chunk *chunk;
	unsigned long flags;
	int off;

	if (!ptr)
		return;

	kmemleak_free_percpu(ptr);

	addr = __pcpu_ptr_to_addr(ptr);

	spin_lock_irqsave(&pcpu_lock, flags);

	chunk = pcpu_chunk_addr_search(addr);
	off = addr - chunk->base_addr;

	pcpu_free_area(chunk, off);

	/* if there are more than one fully free chunks, wake up grim reaper */
	if (chunk->free_size == pcpu_unit_size) {
		struct pcpu_chunk *pos;

		list_for_each_entry(pos, &pcpu_slot[pcpu_nr_slots - 1], list)
			if (pos != chunk) {
				schedule_work(&pcpu_reclaim_work);
				break;
			}
	}

	spin_unlock_irqrestore(&pcpu_lock, flags);
}
EXPORT_SYMBOL_GPL(free_percpu);

/**
 * is_kernel_percpu_address - test whether address is from static percpu area
 * @addr: address to test
 *
 * Test whether @addr belongs to in-kernel static percpu area.  Module
 * static percpu areas are not considered.  For those, use
 * is_module_percpu_address().
 *
 * RETURNS:
 * %true if @addr is from in-kernel static percpu area, %false otherwise.
 */
bool is_kernel_percpu_address(unsigned long addr)
{
#ifdef CONFIG_SMP
	const size_t static_size = __per_cpu_end - __per_cpu_start;
	void __percpu *base = __addr_to_pcpu_ptr(pcpu_base_addr);
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		void *start = per_cpu_ptr(base, cpu);

		if ((void *)addr >= start && (void *)addr < start + static_size)
			return true;
        }
#endif
	/* on UP, can't distinguish from other static vars, always false */
	return false;
}

/**
 * per_cpu_ptr_to_phys - convert translated percpu address to physical address
 * @addr: the address to be converted to physical address
 *
 * Given @addr which is dereferenceable address obtained via one of
 * percpu access macros, this function translates it into its physical
 * address.  The caller is responsible for ensuring @addr stays valid
 * until this function finishes.
 *
 * percpu allocator has special setup for the first chunk, which currently
 * supports either embedding in linear address space or vmalloc mapping,
 * and, from the second one, the backing allocator (currently either vm or
 * km) provides translation.
 *
 * The addr can be tranlated simply without checking if it falls into the
 * first chunk. But the current code reflects better how percpu allocator
 * actually works, and the verification can discover both bugs in percpu
 * allocator itself and per_cpu_ptr_to_phys() callers. So we keep current
 * code.
 *
 * RETURNS:
 * The physical address for @addr.
 */
phys_addr_t per_cpu_ptr_to_phys(void *addr)
{
	void __percpu *base = __addr_to_pcpu_ptr(pcpu_base_addr);
	bool in_first_chunk = false;
	unsigned long first_low, first_high;
	unsigned int cpu;

	/*
	 * The following test on unit_low/high isn't strictly
	 * necessary but will speed up lookups of addresses which
	 * aren't in the first chunk.
	 */
	first_low = pcpu_chunk_addr(pcpu_first_chunk, pcpu_low_unit_cpu, 0);
	first_high = pcpu_chunk_addr(pcpu_first_chunk, pcpu_high_unit_cpu,
				     pcpu_unit_pages);
	if ((unsigned long)addr >= first_low &&
	    (unsigned long)addr < first_high) {
		for_each_possible_cpu(cpu) {
			void *start = per_cpu_ptr(base, cpu);

			if (addr >= start && addr < start + pcpu_unit_size) {
				in_first_chunk = true;
				break;
			}
		}
	}

	if (in_first_chunk) {
		if (!is_vmalloc_addr(addr))
			return __pa(addr);
		else
			return page_to_phys(vmalloc_to_page(addr)) +
			       offset_in_page(addr);
	} else
		return page_to_phys(pcpu_addr_to_page(addr)) +
		       offset_in_page(addr);
}

/**
 * pcpu_alloc_alloc_info - allocate percpu allocation info
 * @nr_groups: the number of groups
 * @nr_units: the number of units
 *
 * Allocate ai which is large enough for @nr_groups groups containing
 * @nr_units units.  The returned ai's groups[0].cpu_map points to the
 * cpu_map array which is long enough for @nr_units and filled with
 * NR_CPUS.  It's the caller's responsibility to initialize cpu_map
 * pointer of other groups.
 *
 * RETURNS:
 * Pointer to the allocated pcpu_alloc_info on success, NULL on
 * failure.
 */

/** 20130615
1. pcpu_alloc_info+groups+units 의 사이즈 구해서 bootmem으로 할당한다.
2. pcpu_alloc_info의 몇몇 값을 설정하고 리턴.
 **/
struct pcpu_alloc_info * __init pcpu_alloc_alloc_info(int nr_groups,
						      int nr_units)
{
	struct pcpu_alloc_info *ai;
	size_t base_size, ai_size;
	void *ptr;
	int unit;

	/** 20130608    
	 * struct pcpu_alloc_info 크기와 groups 개수만큼 할당하기 위한 struct pcpu_group_info 의 크기를 구하고,
	 * cpu_map[0]의 정렬 단위(unsigned int로 4)로 정렬시킨 값을 base_size.
	 **/
	base_size = ALIGN(sizeof(*ai) + nr_groups * sizeof(ai->groups[0]),
			  __alignof__(ai->groups[0].cpu_map[0]));
	/** 20130608    
	 * unit의 개수만큼 필요한 공간까지 더해 ai_size를 구한다.
	 **/
	ai_size = base_size + nr_units * sizeof(ai->groups[0].cpu_map[0]);

	/** 20130608    
	 * page 단위로 ai_size를 정렬해 memory allocation.
	 **/
	ptr = alloc_bootmem_nopanic(PFN_ALIGN(ai_size));
	if (!ptr)
		return NULL;
	ai = ptr;
	ptr += base_size;

	ai->groups[0].cpu_map = ptr;

	/** 20130608    
	 * units(cpu)의 수만큼 반복해 cpu_map에 NR_CPUS를 넣어준다.
	 **/
	for (unit = 0; unit < nr_units; unit++)
		ai->groups[0].cpu_map[unit] = NR_CPUS;

	/** 20130608    
	 * nr_groups는 1
	 **/
	ai->nr_groups = nr_groups;
	/** 20130608    
	 * 할당한 메모리 크기를 __ai_size에 저장.
	 **/
	ai->__ai_size = PFN_ALIGN(ai_size);

	return ai;
}

/**
 * pcpu_free_alloc_info - free percpu allocation info
 * @ai: pcpu_alloc_info to free
 *
 * Free @ai which was allocated by pcpu_alloc_alloc_info().
 */
/** 20130629    
 * alloc info 자료구조가 사용한 영역 bitmap 해제
 **/
void __init pcpu_free_alloc_info(struct pcpu_alloc_info *ai)
{
	/** 20130629    
	 * ai가 사용한 영역 초기화 (page bitmap 설정)
	 **/
	free_bootmem(__pa(ai), ai->__ai_size);
}

/**
 * pcpu_dump_alloc_info - print out information about pcpu_alloc_info
 * @lvl: loglevel
 * @ai: allocation info to dump
 *
 * Print out information about @ai using loglevel @lvl.
 */
static void pcpu_dump_alloc_info(const char *lvl,
				 const struct pcpu_alloc_info *ai)
{
	int group_width = 1, cpu_width = 1, width;
	char empty_str[] = "--------";
	int alloc = 0, alloc_end = 0;
	int group, v;
	int upa, apl;	/* units per alloc, allocs per line */

	v = ai->nr_groups;
	while (v /= 10)
		group_width++;

	v = num_possible_cpus();
	while (v /= 10)
		cpu_width++;
	empty_str[min_t(int, cpu_width, sizeof(empty_str) - 1)] = '\0';

	upa = ai->alloc_size / ai->unit_size;
	width = upa * (cpu_width + 1) + group_width + 3;
	apl = rounddown_pow_of_two(max(60 / width, 1));

	printk("%spcpu-alloc: s%zu r%zu d%zu u%zu alloc=%zu*%zu",
	       lvl, ai->static_size, ai->reserved_size, ai->dyn_size,
	       ai->unit_size, ai->alloc_size / ai->atom_size, ai->atom_size);

	for (group = 0; group < ai->nr_groups; group++) {
		const struct pcpu_group_info *gi = &ai->groups[group];
		int unit = 0, unit_end = 0;

		BUG_ON(gi->nr_units % upa);
		for (alloc_end += gi->nr_units / upa;
		     alloc < alloc_end; alloc++) {
			if (!(alloc % apl)) {
				printk(KERN_CONT "\n");
				printk("%spcpu-alloc: ", lvl);
			}
			printk(KERN_CONT "[%0*d] ", group_width, group);

			for (unit_end += upa; unit < unit_end; unit++)
				if (gi->cpu_map[unit] != NR_CPUS)
					printk(KERN_CONT "%0*d ", cpu_width,
					       gi->cpu_map[unit]);
				else
					printk(KERN_CONT "%s ", empty_str);
		}
	}
	printk(KERN_CONT "\n");
}

/**
 * pcpu_setup_first_chunk - initialize the first percpu chunk
 * @ai: pcpu_alloc_info describing how to percpu area is shaped
 * @base_addr: mapped address
 *
 * Initialize the first percpu chunk which contains the kernel static
 * perpcu area.  This function is to be called from arch percpu area
 * setup path.
 *
 * @ai contains all information necessary to initialize the first
 * chunk and prime the dynamic percpu allocator.
 *
 * @ai->static_size is the size of static percpu area.
 *
 * @ai->reserved_size, if non-zero, specifies the amount of bytes to
 * reserve after the static area in the first chunk.  This reserves
 * the first chunk such that it's available only through reserved
 * percpu allocation.  This is primarily used to serve module percpu
 * static areas on architectures where the addressing model has
 * limited offset range for symbol relocations to guarantee module
 * percpu symbols fall inside the relocatable range.
 *
 * @ai->dyn_size determines the number of bytes available for dynamic
 * allocation in the first chunk.  The area between @ai->static_size +
 * @ai->reserved_size + @ai->dyn_size and @ai->unit_size is unused.
 *
 * @ai->unit_size specifies unit size and must be aligned to PAGE_SIZE
 * and equal to or larger than @ai->static_size + @ai->reserved_size +
 * @ai->dyn_size.
 *
 * @ai->atom_size is the allocation atom size and used as alignment
 * for vm areas.
 *
 * @ai->alloc_size is the allocation size and always multiple of
 * @ai->atom_size.  This is larger than @ai->atom_size if
 * @ai->unit_size is larger than @ai->atom_size.
 *
 * @ai->nr_groups and @ai->groups describe virtual memory layout of
 * percpu areas.  Units which should be colocated are put into the
 * same group.  Dynamic VM areas will be allocated according to these
 * groupings.  If @ai->nr_groups is zero, a single group containing
 * all units is assumed.
 *
 * The caller should have mapped the first chunk at @base_addr and
 * copied static data to each unit.
 *
 * If the first chunk ends up with both reserved and dynamic areas, it
 * is served by two chunks - one to serve the core static and reserved
 * areas and the other for the dynamic area.  They share the same vm
 * and page map but uses different area allocation map to stay away
 * from each other.  The latter chunk is circulated in the chunk slots
 * and available for dynamic allocation like any other chunks.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/** 20130629    
 * percpu first chunk를 설정한다.
 * chunk 자료구조를 할당하고 초기화 한다.
 *
 * first chunk: kernel에서 사용하는 static percpu area를 표현하는 chunk
 **/
int __init pcpu_setup_first_chunk(const struct pcpu_alloc_info *ai,
				  void *base_addr)
{
	static char cpus_buf[4096] __initdata;
	static int smap[PERCPU_DYNAMIC_EARLY_SLOTS] __initdata;
	static int dmap[PERCPU_DYNAMIC_EARLY_SLOTS] __initdata;
	size_t dyn_size = ai->dyn_size;
	size_t size_sum = ai->static_size + ai->reserved_size + dyn_size;
	struct pcpu_chunk *schunk, *dchunk = NULL;
	unsigned long *group_offsets;
	size_t *group_sizes;
	unsigned long *unit_off;
	unsigned int cpu;
	int *unit_map;
	int group, unit, i;
	/** 20130615
	cpu_possible_mask를 문자열로 나타내주기 위한 문자열 버퍼 생성
	**/
	cpumask_scnprintf(cpus_buf, sizeof(cpus_buf), cpu_possible_mask);

#define PCPU_SETUP_BUG_ON(cond)	do {					\
	if (unlikely(cond)) {						\
		pr_emerg("PERCPU: failed to initialize, %s", #cond);	\
		pr_emerg("PERCPU: cpu_possible_mask=%s\n", cpus_buf);	\
		pcpu_dump_alloc_info(KERN_EMERG, ai);			\
		BUG();							\
	}								\
} while (0)
		
	/* sanity checks */
	PCPU_SETUP_BUG_ON(ai->nr_groups <= 0);
#ifdef CONFIG_SMP
	PCPU_SETUP_BUG_ON(!ai->static_size);
	PCPU_SETUP_BUG_ON((unsigned long)__per_cpu_start & ~PAGE_MASK);
#endif
	PCPU_SETUP_BUG_ON(!base_addr);
	PCPU_SETUP_BUG_ON((unsigned long)base_addr & ~PAGE_MASK);
	PCPU_SETUP_BUG_ON(ai->unit_size < size_sum);
	PCPU_SETUP_BUG_ON(ai->unit_size & ~PAGE_MASK);
	PCPU_SETUP_BUG_ON(ai->unit_size < PCPU_MIN_UNIT_SIZE);
	PCPU_SETUP_BUG_ON(ai->dyn_size < PERCPU_DYNAMIC_EARLY_SIZE);
	PCPU_SETUP_BUG_ON(pcpu_verify_alloc_info(ai) < 0);
	
	/** 20130615
	메모리 세팅및 초기화 해준다.
	**/
	/* process group information and build config tables accordingly */
	group_offsets = alloc_bootmem(ai->nr_groups * sizeof(group_offsets[0]));
	group_sizes = alloc_bootmem(ai->nr_groups * sizeof(group_sizes[0]));
	unit_map = alloc_bootmem(nr_cpu_ids * sizeof(unit_map[0]));
	unit_off = alloc_bootmem(nr_cpu_ids * sizeof(unit_off[0]));

	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		unit_map[cpu] = UINT_MAX;

	pcpu_low_unit_cpu = NR_CPUS;
	pcpu_high_unit_cpu = NR_CPUS;

	/** 20130615
	group를 순회 하며 할당 받은 메모리에 값을 아래와같이 세팅해준다.
	**/
	for (group = 0, unit = 0; group < ai->nr_groups; group++, unit += i) {
		const struct pcpu_group_info *gi = &ai->groups[group];
		
		/** 20130615
		각 group의 offset과 size 를 세팅
		**/
		group_offsets[group] = gi->base_offset;
		group_sizes[group] = gi->nr_units * ai->unit_size;
		
		/** 20130615
		group의 unit을 순회하며..	
		**/
		for (i = 0; i < gi->nr_units; i++) {
			cpu = gi->cpu_map[i];
			if (cpu == NR_CPUS)
				continue;

			PCPU_SETUP_BUG_ON(cpu > nr_cpu_ids);
			PCPU_SETUP_BUG_ON(!cpu_possible(cpu));
			PCPU_SETUP_BUG_ON(unit_map[cpu] != UINT_MAX);
			
			/** 20130615
				unit_map,unit_off 배열의 cpu인덱스에 해당하는 위치에 값 세팅
				unit은 하나씩 증가된 값.
			**/
			unit_map[cpu] = unit + i;
			/** 20130629    
			 * group의 base_offset + index * unit 크기를 더해
			 * cpu를 index로 했을 때 참조할 unit의 위치를 저장한다.
			 **/
			unit_off[cpu] = gi->base_offset + i * ai->unit_size;
			
			/** 20130615
			전체 unit에서 가장 작은,가장 큰 unit_off을 가진 cpu를 저장한다.
			**/
			/* determine low/high unit_cpu */
			if (pcpu_low_unit_cpu == NR_CPUS ||
			    unit_off[cpu] < unit_off[pcpu_low_unit_cpu])
				pcpu_low_unit_cpu = cpu;
			if (pcpu_high_unit_cpu == NR_CPUS ||
			    unit_off[cpu] > unit_off[pcpu_high_unit_cpu])
				pcpu_high_unit_cpu = cpu;
		}
	}
	/** 20130615
	전체 unit값을 저장
	**/
	pcpu_nr_units = unit;

	/** 20130615
	위 for에서 설정 안된 unit_map인경우 Bug on!!!
	**/
	for_each_possible_cpu(cpu)
		PCPU_SETUP_BUG_ON(unit_map[cpu] == UINT_MAX);

	/* we're done parsing the input, undefine BUG macro and dump config */
#undef PCPU_SETUP_BUG_ON
	/** 20130615
	다음과 같이 설정된 정보 출력
	pcpu-alloc: s6592 r8192 d13888 u32768 alloc=8*4096
	pcpu-alloc: [0] 0 [0] 1 [0] 2 [0] 3
	**/
	pcpu_dump_alloc_info(KERN_DEBUG, ai);

	/** 20130622
	설정한 값들을 static 변수(전역)들에 넣어준다. 
	**/
	pcpu_nr_groups = ai->nr_groups;
	pcpu_group_offsets = group_offsets;
	pcpu_group_sizes = group_sizes;
	pcpu_unit_map = unit_map;
	pcpu_unit_offsets = unit_off;

	/* determine basic parameters */
	/** 20130622	
	unit_size 를 page단위로 변환후 pcpu_unit_pages에 저장
	**/
	pcpu_unit_pages = ai->unit_size >> PAGE_SHIFT;
	/** 20130622
	unit_size를 PAGE 사이즈 단위로 정렬된 값을 저장 
	**/
	pcpu_unit_size = pcpu_unit_pages << PAGE_SHIFT;
	pcpu_atom_size = ai->atom_size;
	/** 20130622
	chuck struct사이즈를 저장.
	**/
	pcpu_chunk_struct_size = sizeof(struct pcpu_chunk) +
		BITS_TO_LONGS(pcpu_unit_pages) * sizeof(unsigned long);

	/*
	 * Allocate chunk slots.  The additional last slot is for
	 * empty chunks.
	 */
	 /** 20130622
		slot 갯수를 구해서 저장. (여기서 empty chuck 2를 추가적으로 더한다..? 이유는???)
	  **/
		pcpu_nr_slots = __pcpu_size_to_slot(pcpu_unit_size) + 2;
	
	/** 20130622
		총 slot의 리스트의 사이즈를 bootmem에 할당하여 
		각 리스트를 초기화
	**/
	pcpu_slot = alloc_bootmem(pcpu_nr_slots * sizeof(pcpu_slot[0]));
	for (i = 0; i < pcpu_nr_slots; i++)
		INIT_LIST_HEAD(&pcpu_slot[i]);

	/*
	 * Initialize static chunk.  If reserved_size is zero, the
	 * static chunk covers static area + dynamic allocation area
	 * in the first chunk.  If reserved_size is not zero, it
	 * covers static area + reserved area (mostly used for module
	 * static percpu allocation).
	 */

	/** 20130622
	- http://studyfoss.egloos.com/5377666
	- 본 소스 앞쪽 주석 참조.

	static or dynamic chunk를 bootmem에 할당하여 각 변수 초기화
	1. static chunk 할당 및 초기화
		point
		1) 여기서 reserved_size가 있을 경우 
		reserved_size를 static chunk의 free_size(여유 공간)에 저장
		2) 없을 경우
		dyn_size를 free_size 에 저장
	
	2. dynamic chunk 할당 및 초기화
		- 1번 static chunk에서 dyn_size가 free_size로 할당되었거나 dyn_size가 0일 경우에는 
		dynamic chunk를 할당 안한다.
	**/
	schunk = alloc_bootmem(pcpu_chunk_struct_size);
	INIT_LIST_HEAD(&schunk->list);
	/** 20130622	
	할당 모든 unit들중 첫번째 주소를 저장		
	**/
	schunk->base_addr = base_addr;
	schunk->map = smap;
	schunk->map_alloc = ARRAY_SIZE(smap);
	schunk->immutable = true;
	bitmap_fill(schunk->populated, pcpu_unit_pages);

	if (ai->reserved_size) {
		schunk->free_size = ai->reserved_size;
		pcpu_reserved_chunk = schunk;
		pcpu_reserved_chunk_limit = ai->static_size + ai->reserved_size;
	} else {
		schunk->free_size = dyn_size;
		dyn_size = 0;			/* dynamic area covered */
	}
	schunk->contig_hint = schunk->free_size;

	schunk->map[schunk->map_used++] = -ai->static_size;
	if (schunk->free_size)
		schunk->map[schunk->map_used++] = schunk->free_size;

	/* init dynamic chunk if necessary */
	if (dyn_size) {
		dchunk = alloc_bootmem(pcpu_chunk_struct_size);
		INIT_LIST_HEAD(&dchunk->list);
	/** 20130622	
	할당 모든 unit들중 첫번째 주소를 저장		
	**/
		dchunk->base_addr = base_addr;
		dchunk->map = dmap;
		dchunk->map_alloc = ARRAY_SIZE(dmap);
		dchunk->immutable = true;
		bitmap_fill(dchunk->populated, pcpu_unit_pages);

		dchunk->contig_hint = dchunk->free_size = dyn_size;
		/** 20130622
		dynamic_chunk가 사용하는 사이즈가 pcpu_reserved_chunk_limit인 이유???
		**/
		dchunk->map[dchunk->map_used++] = -pcpu_reserved_chunk_limit;
		dchunk->map[dchunk->map_used++] = dchunk->free_size;
	}

	/* link the first chunk in */
	/** 20130622
	dchunk가 있는 경우는 dchunk가 pcpu_first_chunk에 들어간다. 
	**/
	pcpu_first_chunk = dchunk ?: schunk;

	/** 20130622
	할당 첫번째 chunk를
	pcpu_slot 배열의 선택된 slot 자료구조(circular doubly linked list)에
	추가해준다.
	**/
	pcpu_chunk_relocate(pcpu_first_chunk, -1);

	/* we're done */
	/** 20130629    
	 * base_addr (할당된 모든 유닛들의 첫번째 주소)를 전역 변수에 저장
	 **/
	pcpu_base_addr = base_addr;
	return 0;
}

#ifdef CONFIG_SMP

const char *pcpu_fc_names[PCPU_FC_NR] __initdata = {
	[PCPU_FC_AUTO]	= "auto",
	[PCPU_FC_EMBED]	= "embed",
	[PCPU_FC_PAGE]	= "page",
};

enum pcpu_fc pcpu_chosen_fc __initdata = PCPU_FC_AUTO;

static int __init percpu_alloc_setup(char *str)
{
	if (0)
		/* nada */;
#ifdef CONFIG_NEED_PER_CPU_EMBED_FIRST_CHUNK
	else if (!strcmp(str, "embed"))
		pcpu_chosen_fc = PCPU_FC_EMBED;
#endif
#ifdef CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK
	else if (!strcmp(str, "page"))
		pcpu_chosen_fc = PCPU_FC_PAGE;
#endif
	else
		pr_warning("PERCPU: unknown allocator %s specified\n", str);

	return 0;
}
early_param("percpu_alloc", percpu_alloc_setup);

/*
 * pcpu_embed_first_chunk() is used by the generic percpu setup.
 * Build it if needed by the arch config or the generic setup is going
 * to be used.
 */
#if defined(CONFIG_NEED_PER_CPU_EMBED_FIRST_CHUNK) || \
	!defined(CONFIG_HAVE_SETUP_PER_CPU_AREA)
#define BUILD_EMBED_FIRST_CHUNK
#endif

/* build pcpu_page_first_chunk() iff needed by the arch config */
#if defined(CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK)
#define BUILD_PAGE_FIRST_CHUNK
#endif

/* pcpu_build_alloc_info() is used by both embed and page first chunk */
#if defined(BUILD_EMBED_FIRST_CHUNK) || defined(BUILD_PAGE_FIRST_CHUNK)
/**
 * pcpu_build_alloc_info - build alloc_info considering distances between CPUs
 * @reserved_size: the size of reserved percpu area in bytes
 * @dyn_size: minimum free size for dynamic allocation in bytes
 * @atom_size: allocation atom size
 * @cpu_distance_fn: callback to determine distance between cpus, optional
 *
 * This function determines grouping of units, their mappings to cpus
 * and other parameters considering needed percpu size, allocation
 * atom size and distances between CPUs.
 *
 * Groups are always mutliples of atom size and CPUs which are of
 * LOCAL_DISTANCE both ways are grouped together and share space for
 * units in the same group.  The returned configuration is guaranteed
 * to have CPUs on different nodes on different groups and >=75% usage
 * of allocated virtual address space.
 *
 * RETURNS:
 * On success, pointer to the new allocation_info is returned.  On
 * failure, ERR_PTR value is returned.
 */

/** 20130615
1. 모든 cpu를 순회 하면서 근접한 cpu를 그룹핑한다.
2. upa (unit per alloc)을 구한다.
3. pcpu_alloc_info를 할당하고 초기화 한다.
**/
static struct pcpu_alloc_info * __init pcpu_build_alloc_info(
				size_t reserved_size, size_t dyn_size,
				size_t atom_size,
				pcpu_fc_cpu_distance_fn_t cpu_distance_fn)
{
	static int group_map[NR_CPUS] __initdata;
	static int group_cnt[NR_CPUS] __initdata;
	/** 20130608    
		 . = ALIGN((1 << 12));
		 .data..percpu :
			AT(ADDR(.data..percpu) - 0) {
				__per_cpu_load = .;
				__per_cpu_start = .;
				*(.data..percpu..first)
				. = ALIGN((1 << 12));
				*(.data..percpu..page_aligned)
				. = ALIGN((1 << 6));
				*(.data..percpu..readmostly)
				. = ALIGN((1 << 6));
				*(.data..percpu)
				*(.data..percpu..shared_aligned)
				__per_cpu_end = .;
			}
	 **/
	const size_t static_size = __per_cpu_end - __per_cpu_start;
	int nr_groups = 1, nr_units = 0;
	size_t size_sum, min_unit_size, alloc_size;
	int upa, max_upa, uninitialized_var(best_upa);	/* units_per_alloc */
	int last_allocs, group, unit;
	unsigned int cpu, tcpu;
	struct pcpu_alloc_info *ai;
	unsigned int *cpu_map;

	/* this function may be called multiple times */
	memset(group_map, 0, sizeof(group_map));
	memset(group_cnt, 0, sizeof(group_cnt));

	/* calculate size_sum and ensure dyn_size is enough for early alloc */
	/** 20130608    
	 * size_sum = 
	 * static_size + reserved_size + dyn_size(또는 PERCPU_DYNAMIC_EARLY_SIZE)
	 **/
	size_sum = PFN_ALIGN(static_size + reserved_size +
			    max_t(size_t, dyn_size, PERCPU_DYNAMIC_EARLY_SIZE));
	/** 20130608    
	 * dyn_size를 정렬된 크기에서 다시 구해옴.
	 **/
	dyn_size = size_sum - static_size - reserved_size;

	/*
	 * Determine min_unit_size, alloc_size and max_upa such that
	 * alloc_size is multiple of atom_size and is the smallest
	 * which can accommodate 4k aligned segments which are equal to
	 * or larger than min_unit_size.
	 */
	/** 20130608    
	 * size_sum과 PCPU_MIN_UNIT_SIZE (32K) 중 큰 값으로 min_unit_size를 정함
	 **/	
	min_unit_size = max_t(size_t, size_sum, PCPU_MIN_UNIT_SIZE);

	/** 20130608    
	 * min_unit_size 를 atom_size 단위로 roundup해 alloc_size를 구함
	 **/
	alloc_size = roundup(min_unit_size, atom_size);

	/** 20130608    
	 * min_unit_size가 32KB이고,
	 * atom_size가 4KB인 경우 alloc_size가 min_unit_size와 같으므로 upa는 1
	 * atom_size가 2MB인 경우 alloc_size는 2MB. upa는 2MB / 32KB이므로 upa는 64
	 **/

	/** 20130608    
	 * e.g.
	 * size_sum : 28672. 0x7000. 28K
	 * min_unit_size : 32K
	 * upa : 1
	 * max_upa : 1
	 **/
	upa = alloc_size / min_unit_size;
	while (alloc_size % upa || ((alloc_size / upa) & ~PAGE_MASK))
		upa--;
	max_upa = upa;

	/* group cpus according to their proximity */
	for_each_possible_cpu(cpu) {
		group = 0;
	next_group:
		for_each_possible_cpu(tcpu) {
			/** 20130608    
			 * cpu, tcpu 가 0으로 같으면 break;
			 * cpu: 1, tcpu: 0일 때 다음 라인 수행
			 **/
			if (cpu == tcpu)
				break;
			/** 20130608    
			 * cpu_distance_fn는 ACPI spec에 명시되어 있음.
			 * UMA인 ARM에서는 NULL fn.
			 *
			 * LOCAL_DISTANCE보다 클 때 별도의 group에 들어가도록 함.
			 **/
			if (group_map[tcpu] == group && cpu_distance_fn &&
			    (cpu_distance_fn(cpu, tcpu) > LOCAL_DISTANCE ||
			     cpu_distance_fn(tcpu, cpu) > LOCAL_DISTANCE)) {
				group++;
				nr_groups = max(nr_groups, group + 1);
				goto next_group;
			}
		}
		/** 20130608    
		 * group_map[0, 1, 2, 3] = 0
		 * group_cnt[0]          = nr_cpu_ids
		 **/
		group_map[cpu] = group;
		group_cnt[group]++;
	}

	/*
	 * Expand unit size until address space usage goes over 75%
	 * and then as much as possible without using more address
	 * space.
	 */
	last_allocs = INT_MAX;
	for (upa = max_upa; upa; upa--) {
		int allocs = 0, wasted = 0;

		if (alloc_size % upa || ((alloc_size / upa) & ~PAGE_MASK))
			continue;

		/** 20130608    
		 * nr_groups 초기 수행시 1.
		 *
		 * atom_size가 4KB인 경우 (upa:1)
		 * allocs : 4
		 * wasted : 0
		 *
		 * atom_size가 2MB인 경우 (upa:64)
		 * allocs : 1
		 * wasted : 60
		 **/
		for (group = 0; group < nr_groups; group++) {
			int this_allocs = DIV_ROUND_UP(group_cnt[group], upa);
			allocs += this_allocs;
			wasted += this_allocs * upa - group_cnt[group];
		}

		/*
		 * Don't accept if wastage is over 1/3.  The
		 * greater-than comparison ensures upa==1 always
		 * passes the following check.
		 */
		/** 20130608    
		 * 낭비되는 자원의 수가 가용한 cpus의 1/3을 넘는다면 continue.
		 * 여기서 낭비되는 자원의 수란???
		 **/
		if (wasted > num_possible_cpus() / 3)
			continue;

		/* and then don't consume more memory */
		/** 20130608    
		 * 예를 들어 this_allocs가 1에서 2로 넘어갈 경우,
		 * 이전에 저장한 last_allocs은 1, allocs가 2인 경우가 발생한다.
		 * best_upa는 last_allocs가 2로 넘어가기 전의 값.
		 *
		 * atom_size가 4KB인 경우 best_upa는 1
		 * atom_size가 2MB인 경우 best_upa는 4
		 **/
		if (allocs > last_allocs)
			break;
		last_allocs = allocs;
		best_upa = upa;
	}
	upa = best_upa;

	/* allocate and fill alloc_info */
	/** 20130608    
	 * atom_size 4KB인 경우 upa : 1
	 * nr_units는 4.
	 **/
	for (group = 0; group < nr_groups; group++)
		nr_units += roundup(group_cnt[group], upa);

	ai = pcpu_alloc_alloc_info(nr_groups, nr_units);
	if (!ai)
		return ERR_PTR(-ENOMEM);
	/** 20130615
	pcpu_alloc_info 의 첫번째 그룹의 cpu_map을 cpu_map에 저장
	**/
	cpu_map = ai->groups[0].cpu_map;
	/** 20130615
	nr_groups만큼을 돌면서 각 그룹의 cpu_map을 세팅해준다. 
	**/
	for (group = 0; group < nr_groups; group++) {
		ai->groups[group].cpu_map = cpu_map;
		cpu_map += roundup(group_cnt[group], upa);
	}

	/** 20130615
	pcpu_alloc_info 의 값을 채운다.
	**/
	ai->static_size = static_size;
	ai->reserved_size = reserved_size;
	ai->dyn_size = dyn_size;
	ai->unit_size = alloc_size / upa;
	ai->atom_size = atom_size;
	ai->alloc_size = alloc_size;

	/** 20130615
	각 group를 순회하면서
	pcpu_group_info을설정한다.**/	
	for (group = 0, unit = 0; group_cnt[group]; group++) {
		struct pcpu_group_info *gi = &ai->groups[group];

		/*
		 * Initialize base_offset as if all groups are located
		 * back-to-back.  The caller should update this to
		 * reflect actual allocation.
		 */
		/** 20130615
		각 그룹의 base_offset을 설정해준다.
		**/
		gi->base_offset = unit * ai->unit_size;

		/** 20130615
		각 possible cpu를 순회하면서 같은 group일경우
		pcpu_group_info의 cpu_map의 nr_units 인덱스에
		cpu 번호를 넣어준다
		**/
		for_each_possible_cpu(cpu)
			if (group_map[cpu] == group)
				gi->cpu_map[gi->nr_units++] = cpu;

		/** 20130615
		nr_units을 upa 단위로 올림한다.
		**/
		gi->nr_units = roundup(gi->nr_units, upa);
		/** 20130615
		다음 base_offset을 계산하기 위해서nr_units만큰 unit 업데이트
		**/
		unit += gi->nr_units;
	}
	BUG_ON(unit != nr_units);

	return ai;
}
#endif /* BUILD_EMBED_FIRST_CHUNK || BUILD_PAGE_FIRST_CHUNK */

#if defined(BUILD_EMBED_FIRST_CHUNK)
/**
 * pcpu_embed_first_chunk - embed the first percpu chunk into bootmem
 * @reserved_size: the size of reserved percpu area in bytes
 * @dyn_size: minimum free size for dynamic allocation in bytes
 * @atom_size: allocation atom size
 * @cpu_distance_fn: callback to determine distance between cpus, optional
 * @alloc_fn: function to allocate percpu page
 * @free_fn: function to free percpu page
 *
 * This is a helper to ease setting up embedded first percpu chunk and
 * can be called where pcpu_setup_first_chunk() is expected.
 *
 * If this function is used to setup the first chunk, it is allocated
 * by calling @alloc_fn and used as-is without being mapped into
 * vmalloc area.  Allocations are always whole multiples of @atom_size
 * aligned to @atom_size.
 *
 * This enables the first chunk to piggy back on the linear physical
 * mapping which often uses larger page size.  Please note that this
 * can result in very sparse cpu->unit mapping on NUMA machines thus
 * requiring large vmalloc address space.  Don't use this allocator if
 * vmalloc space is not orders of magnitude larger than distances
 * between node memory addresses (ie. 32bit NUMA machines).
 *
 * @dyn_size specifies the minimum dynamic area size.
 *
 * If the needed size is smaller than the minimum or specified unit
 * size, the leftover is returned using @free_fn.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
/** 20130629    
 * reserved_size, dynamic_size, atom_size를 받아와
 * first_chunk 자료구조를 bootmem에 할당하고 초기화
 **/
int __init pcpu_embed_first_chunk(size_t reserved_size, size_t dyn_size,
				  size_t atom_size,
				  pcpu_fc_cpu_distance_fn_t cpu_distance_fn,
				  pcpu_fc_alloc_fn_t alloc_fn,
				  pcpu_fc_free_fn_t free_fn)
{
	void *base = (void *)ULONG_MAX;
	/** 20130629    
	 * group의 위치를 가리키는 포인터 배열을 나타냄
	 **/
	void **areas = NULL;
	struct pcpu_alloc_info *ai;
	size_t size_sum, areas_size, max_distance;
	int group, i, rc;
	
	/** 20130615
	pcpu_alloc_info 구조체 할당하고 초기화
	**/
	ai = pcpu_build_alloc_info(reserved_size, dyn_size, atom_size,
				   cpu_distance_fn);
	if (IS_ERR(ai))
		return PTR_ERR(ai);

	/** 20130615
	총 사이즈를 구한다.
	**/
	size_sum = ai->static_size + ai->reserved_size + ai->dyn_size;
	
	/** 20130615
	각 그룹을 가르키기위한 포인터의 총 사이즈를 구함.
	**/
	areas_size = PFN_ALIGN(ai->nr_groups * sizeof(void *));

	/** 20130615
	areas_size 만큼 메모리 할당
	**/
	areas = alloc_bootmem_nopanic(areas_size);
	if (!areas) {
		rc = -ENOMEM;
		goto out_free;
	}

	/* allocate, copy and determine base address */
	for (group = 0; group < ai->nr_groups; group++) {
		struct pcpu_group_info *gi = &ai->groups[group];
		unsigned int cpu = NR_CPUS;
		void *ptr;
	
		/** 20130615
			각 pcpu_group_info의 nr_units 만큼 돌면서 cpu_map의
			cpu번호 가져온다. 
			단, cpu번호가 for이 돌때마다 바뀔텐데 그럼 항상 두번째 for문 에서
			false로 나갈듯???
		**/
		for (i = 0; i < gi->nr_units && cpu == NR_CPUS; i++)
			cpu = gi->cpu_map[i];
		BUG_ON(cpu == NR_CPUS);
		
		/** 20130615
			atomic_size단위로 해당 group의 총 unit사이즈를
			할당한다.
			cpu :: ???	

			unit을 할당한다.
		**/
		/* allocate space for the whole group */
		ptr = alloc_fn(cpu, gi->nr_units * ai->unit_size, atom_size);
		if (!ptr) {
			rc = -ENOMEM;
			goto out_free_areas;
		}
		/* kmemleak tracks the percpu allocations separately */
		kmemleak_free(ptr);
	
		/** 20130615
		각 그룹 포인터 설정
		**/
		areas[group] = ptr;

		/** 20130615
		ptr과 base중 작은 값을 다시base에 대입		
		**/
		base = min(ptr, base);
	}

	/*
	 * Copy data and free unused parts.  This should happen after all
	 * allocations are complete; otherwise, we may end up with
	 * overlapping groups.
	 */
	/** 20130615
	모든 group를 순회
		각 group의 cpu를 순회
			1.areas[group] 내에서 해당 cpu의 공간에 static size만큼의 데이타를 ptr로 복사
			2.size_sum영역을 제외한 나머지 영역을 free

	20130629
		--> vmlinux.lds에서 지정된 .data..percpu 영역을 각 group의 unit 공간에 복사해 percpu용 데이터로 만드는 함수.
		    이후 각 group마다의 offset 값을 저장해 둔 다음, 그 offset을 percpu 변수 주소에 더해 실제 위치를 구한다.
	**/
	for (group = 0; group < ai->nr_groups; group++) {
		struct pcpu_group_info *gi = &ai->groups[group];
		void *ptr = areas[group];

		for (i = 0; i < gi->nr_units; i++, ptr += ai->unit_size) {
			if (gi->cpu_map[i] == NR_CPUS) {
				/* unused unit, free whole */
				free_fn(ptr, ai->unit_size);
				continue;
			}
			/* copy and return the unused part */
			memcpy(ptr, __per_cpu_load, ai->static_size);
			free_fn(ptr + size_sum, ai->unit_size - size_sum);
		}
	}

	/* base address is now known, determine group base offsets */
	/** 20130615
	group를 순회 하면서 base offset을 구해서 그중 가장 큰 값을
	max_distance에 넣는다.
	**/
	max_distance = 0;
	for (group = 0; group < ai->nr_groups; group++) {
		ai->groups[group].base_offset = areas[group] - base;
		max_distance = max_t(size_t, max_distance,
				     ai->groups[group].base_offset);
	}
	/** 20130615
	max_distance에 unit_size만큼을 더한다.
	**/	
	max_distance += ai->unit_size;

	/** 20130615
		max_distance가 vmalloc space의 75%보다 크다면 warning!!
	**/
	/* warn if maximum distance is further than 75% of vmalloc space */
	if (max_distance > (VMALLOC_END - VMALLOC_START) * 3 / 4) {
		pr_warning("PERCPU: max_distance=0x%zx too large for vmalloc "
			   "space 0x%lx\n", max_distance,
			   (unsigned long)(VMALLOC_END - VMALLOC_START));
#ifdef CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK
		/* and fail if we have fallback */
		rc = -EINVAL;
		goto out_free;
#endif
	}

	pr_info("PERCPU: Embedded %zu pages/cpu @%p s%zu r%zu d%zu u%zu\n",
		PFN_DOWN(size_sum), base, ai->static_size, ai->reserved_size,
		ai->dyn_size, ai->unit_size);

	/** 20130629    
	 * first chuck 자료구조 초기화.
	 **/
	rc = pcpu_setup_first_chunk(ai, base);
	goto out_free;

/** 20130629    
 * ai alloc이 실패하면 이동
 **/
out_free_areas:
	for (group = 0; group < ai->nr_groups; group++)
		free_fn(areas[group],
			ai->groups[group].nr_units * ai->unit_size);
out_free:
	/** 20130629    
	 * ai 자료구조 해제
	 **/
	pcpu_free_alloc_info(ai);
	/** 20130629    
	 * area 자료구조 해제
	 **/
	if (areas)
		free_bootmem(__pa(areas), areas_size);
	return rc;
}
#endif /* BUILD_EMBED_FIRST_CHUNK */

#ifdef BUILD_PAGE_FIRST_CHUNK
/**
 * pcpu_page_first_chunk - map the first chunk using PAGE_SIZE pages
 * @reserved_size: the size of reserved percpu area in bytes
 * @alloc_fn: function to allocate percpu page, always called with PAGE_SIZE
 * @free_fn: function to free percpu page, always called with PAGE_SIZE
 * @populate_pte_fn: function to populate pte
 *
 * This is a helper to ease setting up page-remapped first percpu
 * chunk and can be called where pcpu_setup_first_chunk() is expected.
 *
 * This is the basic allocator.  Static percpu area is allocated
 * page-by-page into vmalloc area.
 *
 * RETURNS:
 * 0 on success, -errno on failure.
 */
int __init pcpu_page_first_chunk(size_t reserved_size,
				 pcpu_fc_alloc_fn_t alloc_fn,
				 pcpu_fc_free_fn_t free_fn,
				 pcpu_fc_populate_pte_fn_t populate_pte_fn)
{
	static struct vm_struct vm;
	struct pcpu_alloc_info *ai;
	char psize_str[16];
	int unit_pages;
	size_t pages_size;
	struct page **pages;
	int unit, i, j, rc;

	snprintf(psize_str, sizeof(psize_str), "%luK", PAGE_SIZE >> 10);

	ai = pcpu_build_alloc_info(reserved_size, 0, PAGE_SIZE, NULL);
	if (IS_ERR(ai))
		return PTR_ERR(ai);
	BUG_ON(ai->nr_groups != 1);
	BUG_ON(ai->groups[0].nr_units != num_possible_cpus());

	unit_pages = ai->unit_size >> PAGE_SHIFT;

	/* unaligned allocations can't be freed, round up to page size */
	pages_size = PFN_ALIGN(unit_pages * num_possible_cpus() *
			       sizeof(pages[0]));
	pages = alloc_bootmem(pages_size);

	/* allocate pages */
	j = 0;
	for (unit = 0; unit < num_possible_cpus(); unit++)
		for (i = 0; i < unit_pages; i++) {
			unsigned int cpu = ai->groups[0].cpu_map[unit];
			void *ptr;

			ptr = alloc_fn(cpu, PAGE_SIZE, PAGE_SIZE);
			if (!ptr) {
				pr_warning("PERCPU: failed to allocate %s page "
					   "for cpu%u\n", psize_str, cpu);
				goto enomem;
			}
			/* kmemleak tracks the percpu allocations separately */
			kmemleak_free(ptr);
			pages[j++] = virt_to_page(ptr);
		}

	/* allocate vm area, map the pages and copy static data */
	vm.flags = VM_ALLOC;
	vm.size = num_possible_cpus() * ai->unit_size;
	vm_area_register_early(&vm, PAGE_SIZE);

	for (unit = 0; unit < num_possible_cpus(); unit++) {
		unsigned long unit_addr =
			(unsigned long)vm.addr + unit * ai->unit_size;

		for (i = 0; i < unit_pages; i++)
			populate_pte_fn(unit_addr + (i << PAGE_SHIFT));

		/* pte already populated, the following shouldn't fail */
		rc = __pcpu_map_pages(unit_addr, &pages[unit * unit_pages],
				      unit_pages);
		if (rc < 0)
			panic("failed to map percpu area, err=%d\n", rc);

		/*
		 * FIXME: Archs with virtual cache should flush local
		 * cache for the linear mapping here - something
		 * equivalent to flush_cache_vmap() on the local cpu.
		 * flush_cache_vmap() can't be used as most supporting
		 * data structures are not set up yet.
		 */

		/* copy static data */
		memcpy((void *)unit_addr, __per_cpu_load, ai->static_size);
	}

	/* we're ready, commit */
	pr_info("PERCPU: %d %s pages/cpu @%p s%zu r%zu d%zu\n",
		unit_pages, psize_str, vm.addr, ai->static_size,
		ai->reserved_size, ai->dyn_size);

	rc = pcpu_setup_first_chunk(ai, vm.addr);
	goto out_free_ar;

enomem:
	while (--j >= 0)
		free_fn(page_address(pages[j]), PAGE_SIZE);
	rc = -ENOMEM;
out_free_ar:
	free_bootmem(__pa(pages), pages_size);
	pcpu_free_alloc_info(ai);
	return rc;
}
#endif /* BUILD_PAGE_FIRST_CHUNK */

#ifndef	CONFIG_HAVE_SETUP_PER_CPU_AREA
/*
 * Generic SMP percpu area setup.
 *
 * The embedding helper is used because its behavior closely resembles
 * the original non-dynamic generic percpu area setup.  This is
 * important because many archs have addressing restrictions and might
 * fail if the percpu area is located far away from the previous
 * location.  As an added bonus, in non-NUMA cases, embedding is
 * generally a good idea TLB-wise because percpu area can piggy back
 * on the physical linear memory mapping which uses large page
 * mappings on applicable archs.
 */
/** 20130831    
 * 각 cpu가 setup_per_cpu_areas 에서 채워준다.
 **/
unsigned long __per_cpu_offset[NR_CPUS] __read_mostly;
EXPORT_SYMBOL(__per_cpu_offset);

/** 20130608    
 * pcpu_dfl_fc_alloc, pcpu_dfl_fc_free는 pcpu_embed_first_chunk에 Callback 함수로 전달
 **/
 /** 20130615
 PA(MAX_DMA_ADDRESS)이후의 공간에서 align단위의 size만큼 할당. 
 **/
static void * __init pcpu_dfl_fc_alloc(unsigned int cpu, size_t size,
				       size_t align)
{
	return __alloc_bootmem_nopanic(size, align, __pa(MAX_DMA_ADDRESS));
}

static void __init pcpu_dfl_fc_free(void *ptr, size_t size)
{
	free_bootmem(__pa(ptr), size);
}

/** 20130629    
 * first chunk를 생성하고,
 * 각 cpu들이 percpu내의 자원을 사용할 수 있도록 시작 위치 및 offset을 구해준다.
 **/
void __init setup_per_cpu_areas(void)
{
	unsigned long delta;
	unsigned int cpu;
	int rc;

	/*
	 * Always reserve area for module percpu variables.  That's
	 * what the legacy allocator did.
	 */
	/** 20130608    
	 * PERCPU_MODULE_RESERVE  : 8K
	 * PERCPU_DYNAMIC_RESERVE : 12K
	 * PAGE_SIZE              : 4K
	 **/
	rc = pcpu_embed_first_chunk(PERCPU_MODULE_RESERVE,
				    PERCPU_DYNAMIC_RESERVE, PAGE_SIZE, NULL,
				    pcpu_dfl_fc_alloc, pcpu_dfl_fc_free);
	if (rc < 0)
		panic("Failed to initialize percpu areas.");

	/** 20130629    
	 * pcpu_base_addr  : group이 할당받은 bootmem에서 할당 받은 공간 중 가장 작은 값
	 * __per_cpu_start : .data..percpu 영역의 시작 주소.
	 *
	 * delta는 둘 사이의 offset 값.
	 **/
	delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
	/** 20130629    
	 * cpu들의 unit offset 주소를 가져와 delta와 더해 __per_cpu_offset에 저장
	 *     -> 이것은 __per_cpu_start 에서부터 offset 값이다.
	 *
	 * 아래 MACRO에서 사용
	 * #define per_cpu_offset(x) (__per_cpu_offset[x])
	 * #define per_cpu(var, cpu) \
	 *    (*SHIFT_PERCPU_PTR(&(var), per_cpu_offset(cpu)))
	 **/
	for_each_possible_cpu(cpu)
		__per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
}
#endif	/* CONFIG_HAVE_SETUP_PER_CPU_AREA */

#else	/* CONFIG_SMP */

/*
 * UP percpu area setup.
 *
 * UP always uses km-based percpu allocator with identity mapping.
 * Static percpu variables are indistinguishable from the usual static
 * variables and don't require any special preparation.
 */
void __init setup_per_cpu_areas(void)
{
	const size_t unit_size =
		roundup_pow_of_two(max_t(size_t, PCPU_MIN_UNIT_SIZE,
					 PERCPU_DYNAMIC_RESERVE));
	struct pcpu_alloc_info *ai;
	void *fc;

	ai = pcpu_alloc_alloc_info(1, 1);
	fc = __alloc_bootmem(unit_size, PAGE_SIZE, __pa(MAX_DMA_ADDRESS));
	if (!ai || !fc)
		panic("Failed to allocate memory for percpu areas.");
	/* kmemleak tracks the percpu allocations separately */
	kmemleak_free(fc);

	ai->dyn_size = unit_size;
	ai->unit_size = unit_size;
	ai->atom_size = unit_size;
	ai->alloc_size = unit_size;
	ai->groups[0].nr_units = 1;
	ai->groups[0].cpu_map[0] = 0;

	if (pcpu_setup_first_chunk(ai, fc) < 0)
		panic("Failed to initialize percpu areas.");
}

#endif	/* CONFIG_SMP */

/*
 * First and reserved chunks are initialized with temporary allocation
 * map in initdata so that they can be used before slab is online.
 * This function is called after slab is brought up and replaces those
 * with properly allocated maps.
 */
void __init percpu_init_late(void)
{
	struct pcpu_chunk *target_chunks[] =
		{ pcpu_first_chunk, pcpu_reserved_chunk, NULL };
	struct pcpu_chunk *chunk;
	unsigned long flags;
	int i;

	for (i = 0; (chunk = target_chunks[i]); i++) {
		int *map;
		const size_t size = PERCPU_DYNAMIC_EARLY_SLOTS * sizeof(map[0]);

		BUILD_BUG_ON(size > PAGE_SIZE);

		map = pcpu_mem_zalloc(size);
		BUG_ON(!map);

		spin_lock_irqsave(&pcpu_lock, flags);
		memcpy(map, chunk->map, size);
		chunk->map = map;
		spin_unlock_irqrestore(&pcpu_lock, flags);
	}
}
