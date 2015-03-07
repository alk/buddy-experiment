#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "minimalloc.h"

/* 4 is not enough for 64 bit arches */
#define MIN_ORDER 5
/* #define MAX_ORDER 24 */

#define CHUNKS_COUNT 5

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

static struct mini_state *ms;
static size_t total_allocated;

static
void *mi_mallocer(size_t size)
{
	void *rv = malloc(size);
	if (!rv) {
		abort();
	}
	total_allocated += size;
	return rv;
}


static
void *xmalloc(size_t size)
{
	if (!ms) {
		ms = mini_init(mi_mallocer, 0);
		if (!ms) {
			abort();
		}
	}
	void *rv = mini_malloc(ms, size);
	return rv;
}

static
void xfree(void *p)
{
	assert(ms);
	mini_free(ms, p);
}

struct chunked_blob *allocate_blob(unsigned size)
{
	int i;
	int orders[CHUNKS_COUNT];
	struct chunked_blob *blob;
	value_size_to_block_sizes(size, orders);
	unsigned allocated;
	unsigned subsize;
	unsigned char filler;

	blob = xmalloc(subsize = (1U << (orders[0])));
	allocated = subsize;
	blob->size = size;
	filler = (uintptr_t)blob % 251;
	memset(blob + 1, filler, subsize - sizeof(struct chunked_blob));

	for (i = 1; i < CHUNKS_COUNT && orders[i] >= 0; i++) {
		subsize = 1U << orders[i];
		void *ptr = blob->other_chunks[i-1] = xmalloc(subsize);
		allocated += subsize;
		memset(ptr, filler, subsize);
	}

	assert(allocated > size);

	return blob;
}

static
void memchk(void *_p, unsigned char value, size_t orig_len)
{
	char *p = _p;
	assert(((uintptr_t)p & 3) == 0);
	assert((orig_len & 3) == 0);
	size_t len = orig_len / 4;
	uint32_t acc = 0;
	uint32_t val = value | (value << 8) | (value << 16) | (value << 24);
	for (size_t i = 0; i < len; i++) {
		uint32_t v = ((volatile uint32_t *)p)[i];
		acc |= v ^ val;
	}
	assert(acc == 0);
	if (acc != 0) {
		abort();
	}
	memset(p, 0xff, orig_len);
}


void free_blob(struct chunked_blob *blob, size_t _unused)
{
	int i;
	int orders[CHUNKS_COUNT];
	value_size_to_block_sizes(blob->size, orders);
	unsigned char filler = (uintptr_t)blob % 251;

	for (i = CHUNKS_COUNT-1; i > 0; i--) {
		if (orders[i] < 0)
			continue;
		memchk(blob->other_chunks[i-1], filler, 1U << orders[i]);
		xfree(blob->other_chunks[i-1]);
	}
	memchk(blob + 1, filler, (1U << orders[0]) - sizeof(*blob));
	xfree(blob);
}

size_t get_total_allocated_size(void)
{
	return total_allocated;
}
