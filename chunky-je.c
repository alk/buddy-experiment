#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <jemalloc/jemalloc.h>

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

static bool inited;
static void do_init(void);

static inline
void maybe_init()
{
	if (!inited) {
		do_init();
		inited = true;
	}
}

static
void *xmalloc(size_t size)
{
	void *rv = malloc(size);
	/* memset(rv, 0, size); */
	return rv;
}

struct chunked_blob *allocate_blob(unsigned size)
{
	/* maybe_init(); */

	int i;
	int orders[CHUNKS_COUNT];
	struct chunked_blob *blob;
	value_size_to_block_sizes(size, orders);
	unsigned allocated;
	unsigned subsize;

	blob = xmalloc(subsize = (1U << (orders[0])));
	allocated = subsize;
	blob->size = size;

	for (i = 1; i < CHUNKS_COUNT && orders[i] >= 0; i++) {
		blob->other_chunks[i-1] = xmalloc(subsize = (1U << (orders[i])));
		allocated += subsize;
	}

	assert(allocated > size);

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
		free(blob->other_chunks[i-1]);
	}
	free(blob);
}

static void do_init(void)
{
	bool old;
	size_t size = sizeof(old);
	bool enable = false;
	int err = mallctl("thread.tcache.enabled", &old, &size, &enable,
			  sizeof(enable));
	if (err) {
		errno = err;
		perror("init:mallctl");
		abort();
	}
}

size_t get_total_allocated_size(void)
{
	do_init();

	size_t rv;
	size_t rvlen = sizeof(rv);
	int err = mallctl("stats.active", &rv, &rvlen, NULL, 0);
	if (err) {
		errno = err;
		perror("mallctl");
		abort();
	}

	return rv;
}

static void malloc_stats_printer_cb(void *opaq, const char *data)
{
	fputs(data, stderr);
}

void dump_malloc_stats(void)
{
	malloc_stats_print(malloc_stats_printer_cb, 0, "");
}
