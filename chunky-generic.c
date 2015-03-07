#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "common.h"

/* 4 is not enough for 64 bit arches */
#define MIN_ORDER 5
/* #define MAX_ORDER 24 */

#define CHUNKS_COUNT 7

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

static struct chunked_blob *chunky_allocate_blob(size_t);
static void chunky_free_blob(struct chunked_blob *, size_t);
static size_t chunky_get_total_allocated_size(void);

allocation_functions chunky_fns = {
	.name = "chunky_generic:",
	.alloc = (void *(*)(size_t))chunky_allocate_blob,
	.free = (void (*)(void *, size_t))chunky_free_blob,
	.get_total_allocated_size = chunky_get_total_allocated_size
};

allocation_functions *chunky_slave_fns;

/* We need to find at most CHUNKS_COUNT powers of two that cover size
 * + all required metadata (chunked_blob, block headers) with minimal
 * total size. This can be greatly improved, but for now it can remain
 * suboptimal. Unused orders array positions are set to -1. */
static
void value_size_to_block_sizes(unsigned size, int orders[CHUNKS_COUNT])
{
	unsigned original_size = size;
	(void) original_size;
	unsigned thing;
	int i;
	/* we'll need exactly one chunked_blob struct header */
	size += sizeof(struct chunked_blob);
	if (size <= (2U << MIN_ORDER)) {
		if (size <= (1U << MIN_ORDER)) {
			thing = (1U << MIN_ORDER);
			goto skip_upper_bound;
		}
		thing = (2U << MIN_ORDER);
		goto skip_upper_bound;
	}
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
	/* printf("thing/original_size: %f, %d\n", (double)(thing - original_size) / thing, original_size); */
	assert(prop >= 0 && prop < 1.0);
	while (thing) {
		int order = sizeof(unsigned) * 8 - __builtin_clz(thing) - 1;
		orders[i++] = order;
		thing -= (1U << order);
	}
	for (;i < CHUNKS_COUNT; i++)
		orders[i] = -1;
}

static
void *chunky_xmalloc(size_t size)
{
	void * rv = chunky_slave_fns->alloc(size);
	if (!rv) {
		abort();
	}
	return rv;
}

static
void chunky_xfree(void *p, size_t size)
{
	chunky_slave_fns->free(p, size);
}

static
struct chunked_blob *chunky_allocate_blob(size_t size)
{
	int i;
	int orders[CHUNKS_COUNT];
	struct chunked_blob *blob;
	unsigned allocated;
	unsigned subsize;

	value_size_to_block_sizes(size, orders);

	blob = chunky_xmalloc(subsize = (1U << (orders[0])));
	allocated = subsize;
	blob->size = size;

	for (i = 1; i < CHUNKS_COUNT && orders[i] >= 0; i++) {
		blob->other_chunks[i-1] = chunky_xmalloc(subsize = (1U << (orders[i])));
		allocated += subsize;
	}

	assert(allocated > size);

	return blob;
}


static
void chunky_free_blob(struct chunked_blob *blob, size_t dummy)
{
	int i;
	int orders[CHUNKS_COUNT];
	value_size_to_block_sizes(blob->size, orders);

	for (i = CHUNKS_COUNT-1; i > 0; i--) {
		if (orders[i] < 0)
			continue;
		chunky_xfree(blob->other_chunks[i-1], 1U << orders[i]);
	}
	chunky_xfree(blob, 1U << orders[0]);
}

static
size_t chunky_get_total_allocated_size(void)
{
	return chunky_slave_fns->get_total_allocated_size();
}
