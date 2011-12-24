#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/uio.h>

/*
 * Block starts with following struct.
 *
 * Allocated blocks contain 1 in next field as marker.
 *
 * Free blocks are linked via next/pprev fields to free blocks of same
 * size.
 */
struct block {
	struct block *next;
	struct block **pprev;
};

#define MIN_ORDER 4
#define MAX_ORDER 20

/*
 * This is heads of free lists for various block order sizes
 */
struct block *blocks_orders[MAX_ORDER+1];

static int max_order_blocks_alloced;

static
void *allocate_max_order_block(void)
{
	struct block *rv;
	int error;
	error = posix_memalign((void **)&rv, 1 << MAX_ORDER, 1 << MAX_ORDER);
	if (error) {
		errno = error;
		perror("posix_memalign");
		abort();
	}
	rv->next = 0;
	max_order_blocks_alloced++;
	return rv + 1;
}

static
void *allocate_block(int order)
{
	struct block *buddy;
	struct block *p;

	if (order > MAX_ORDER)
		abort();

	p = blocks_orders[order];
	if (p) {
		struct block *next = blocks_orders[order] = p->next;
		if (next)
			next->pprev = blocks_orders + order;
		p->next = (void *)1;
		return p + 1;
	}

	if (order == MAX_ORDER)
		return allocate_max_order_block();

	p = (struct block *)allocate_block(order+1) - 1;
	buddy = (struct block *)((char *)p + (1 << order));
	buddy->next = 0;	/* NOTE: block_orders[order] is 0 */
	buddy->pprev = blocks_orders + order;
	blocks_orders[order] = buddy;
	return p + 1;
}

static
void free_block(void *ptr, int order)
{
	struct block *p = (struct block *)ptr - 1;
	assert(p->next == (struct block *)1);
	if (order < MAX_ORDER) {
		struct block *buddy = (struct block *)((intptr_t)p ^ (1 << order));
		if ((intptr_t)buddy->next & 1) {
			/* if buddy is free as well, we should combine
			 * with it, by first unlinking it from
			 * free-list */
			struct block *next = *(buddy->pprev) = buddy->next;
			if (next)
				next->pprev = buddy->pprev;
			if (buddy > p)
				buddy = p;
			else
				buddy->next = (struct block *)1;
			free_block(buddy+1, order+1);
			return;
		}
	}

	struct block *next = blocks_orders[order];
	if (next)
		next->pprev = &p->next;
	p->next = next;
	p->pprev = blocks_orders + order;
	blocks_orders[order] = p;
}

#define CHUNKS_COUNT 6

/*
 * Chunked blob is split into contiguous power of 2 chunks. Up to
 * CHUNKS_COUNT chunks.
 *
 * 'size' determines chunks count and sizes (via
 * value_size_to_block_sizes). Biggest chunk -> smallest chunk. Chunk
 * 0 (biggest one) starts right after end of chunked_blob structure
 */
struct chunked_blob {
	unsigned size;
	void *other_chunks[CHUNKS_COUNT-1];
};

#define BLOBS_COUNT (1024*1024)

struct chunked_blob *blobs[BLOBS_COUNT];

/* We need to find at most CHUNKS_COUNT powers of two that cover size
 * + all required metadata (chunked_blob, block headers) with minimal
 * total size. This can be greatly improved, but for now it can remain
 * suboptimal. Unused orders array positions are set to -1. */
static
void value_size_to_block_sizes(unsigned size, int orders[CHUNKS_COUNT])
{
	unsigned original_size = size;
	unsigned thing;
	int i;
	/* we'll need at least one block and exactly one chunked_blob
	 * struct */
	size += sizeof(struct chunked_blob) + sizeof(struct block);
	if (size <= (2U << MIN_ORDER)) {
		if (size <= (1U << MIN_ORDER)) {
			thing = (1U << MIN_ORDER);
			goto skip_upper_bound;
		}
		thing = (2U << MIN_ORDER);
		goto skip_upper_bound;
	}
	/* we're adding all possible block sizes here which is clearly
	 * suboptimal, but leaving it as is for now */
	size += sizeof(struct block) * (CHUNKS_COUNT - 1);
	i = CHUNKS_COUNT;
	thing = 0;
	while (--i >= 0 && thing != size) {
		/* count leading zeros of remaining difference and
		 * find highest set bit of it */
		int order = sizeof(unsigned) * 8 - __builtin_clz(size - thing) - 1;
		if (order < MIN_ORDER) {
			thing += (1U << MIN_ORDER);
			goto skip_upper_bound;
		}
		thing |= (1U << order);
	}
	/* size is still smaller then thing. Add into smallest set bit
	 * of thing to make it larger then size. */
	if (thing != size)
		thing += (1U << __builtin_ctz(thing));
skip_upper_bound:
	i = 0;
	double prop = (double)(thing-size)/thing;
	printf("thing/original_size: %f, %d\n", (double)(thing - original_size) / thing, original_size);
	assert(prop >= 0 && prop < 1.0);
	while (thing) {
		int order = sizeof(unsigned) * 8 - __builtin_clz(thing) - 1;
		orders[i++] = order;
		thing -= (1U << order);
	}
	for (;i < CHUNKS_COUNT; i++)
		orders[i] = -1;
}

struct chunked_blob *allocate_blob(unsigned size)
{
	int i;
	int orders[CHUNKS_COUNT];
	struct chunked_blob *blob;
	value_size_to_block_sizes(size, orders);

	blob = allocate_block(orders[0]);
	blob->size = size;

	for (i = 1; i < CHUNKS_COUNT && orders[i] >= 0; i++)
		blob->other_chunks[i-1] = allocate_block(orders[i]);

	return blob;
}


void free_blob(struct chunked_blob *blob)
{
	int i;
	int orders[CHUNKS_COUNT];
	value_size_to_block_sizes(blob->size, orders);

	for (i = CHUNKS_COUNT-1; i > 0; i--) {
		if (orders[i] < 0)
			continue;
		free_block(blob->other_chunks[i-1], orders[i]);
	}
	free_block(blob, orders[0]);
}

// NOTE: returns freshly malloced array of iovec-s
/* 
 * static
 * struct iovec *blob_to_iovecs(struct chunked_blob *blob, int *iovecs_count)
 * {
 * 	struct iovec *rv = malloc(sizeof(struct iovec) * CHUNKS_COUNT);
 * 	if (!rv) {
 * 		perror("malloc");
 * 		abort();
 * 	}
 * 	int orders[CHUNKS_COUNT];
 * 	undefined size = blob->size;
 * 	int i = 0;
 * 	value_size_to_block_sizes(size, orders);
 * 
 * 	while (size > 0) {
 * 		unsigned current_size = orders[i];
 * 		char *p = (i == 0) ? (char *)(blob + 1) : blob->other_chunks[i-1];
 * 		i++;
 * 		for (; current_size > 0; current_size--) {
 *			// HERE
 * 		}
 * 	}
 * }
 */

/* 
 * static
 * void fill_block(int block_number)
 * {
 * 	struct random_data rdata;
 * 	srandom_r(block_number, &rdata);
 * 
 * 	struct chunked_blob *blob = blobs[block_number];
 * 	int orders[CHUNKS_COUNT];
 * 	undefined size = blob->size;
 * 	int i = 0;
 * 	value_size_to_block_sizes(size, orders);
 * 
 * 	while (size > 0) {
 * 		unsigned current_size = orders[i];
 * 		char *p = (i == 0) ? (char *)(blob + 1) : blob->other_chunks[i-1];
 * 		i++;
 * 		for (; current_size > 0; current_size--) {
 * 			// HERE
 * 		}
 * 	}
 * }
 */

#define ALLOCATE_UNTIL_MB 384

static int usefully_allocated;
static int useful_allocations_count;

int main(void)
{
	int i;
	struct timeval tv;
	int rv;

	rv = gettimeofday(&tv, 0);
	if (rv) {
		perror("gettimeofday");
		abort();
	}
	srandom((unsigned)tv.tv_sec ^ (unsigned)tv.tv_usec);
	/* srandom(0); */

	for (i = 0; i < BLOBS_COUNT; i++) {
		unsigned size = 128 + random() % (128*1024);
		allocate_blob(size);
		usefully_allocated += size;
		useful_allocations_count++;
		if (usefully_allocated >= (ALLOCATE_UNTIL_MB * 1048576))
			break;
	}
	if (i >= BLOBS_COUNT) {
		fprintf(stderr, "too successful allocation!\n");
		return 1;
	}

	int total_ram = max_order_blocks_alloced * (1U << MAX_ORDER);
	printf("got from OS: %d\nApp allocated: %d\nAllocations count:%d\nwaste %f %%\n",
	       total_ram,
	       usefully_allocated,
	       useful_allocations_count,
	       (float)(total_ram - usefully_allocated) * 100 / total_ram);

	return 0;
}
