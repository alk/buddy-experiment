#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/uio.h>

struct block {
	struct block *next;
	struct block **pprev;
};

#define MIN_ORDER 4
#define MAX_ORDER 20

struct block *blocks_orders[MAX_ORDER+1];

static int max_order_blocks_left;

static void on_memory_exhausted(void);

static
void *allocate_max_order_block(void)
{
	struct block *rv;
	int error;
	if (--max_order_blocks_left < 0) {
		on_memory_exhausted();
		abort();
	}
	error = posix_memalign((void **)&rv, 1 << MAX_ORDER, 1 << MAX_ORDER);
	if (error) {
		errno = error;
		perror("posix_memalign");
		abort();
	}
	rv->next = 0;
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
		p->next = 0;
		return p + 1;
	}

	if (order == MAX_ORDER)
		return allocate_max_order_block();

	p = (struct block *)allocate_block(order+1) - 1;
	buddy = (struct block *)((char *)p + (1 << order));
	buddy->next = 0;
	buddy->pprev = blocks_orders + order;
	blocks_orders[order] = buddy;
	return p + 1;
}

static
void free_block(void *ptr, int order)
{
	struct block *p = (struct block *)ptr - 1;
	assert(p->next == 0);
	if (order < MAX_ORDER) {
		struct block *buddy = (struct block *)((intptr_t)p ^ (1 << order));
		if (buddy->next) {
			/* if buddy is free as well, we should combine
			 * with it, by first unlinking it from
			 * freelist */
			*(buddy->pprev) = buddy->next;
			if (buddy > p)
				buddy = p;
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

struct chunked_blob {
	unsigned size;
	void *other_chunks[CHUNKS_COUNT-1];
};

#define BLOBS_COUNT (1024*1024)

struct chunked_blob *blobs[BLOBS_COUNT];

static
void value_size_to_block_sizes(unsigned size, int orders[CHUNKS_COUNT])
{
	size += sizeof(struct chunked_blob) + sizeof(struct block) * CHUNKS_COUNT;
	int i = CHUNKS_COUNT;
	unsigned thing = 0;
	while (--i >= 0 && thing != size) {
		int order = sizeof(unsigned) * 8 - __builtin_clz(size - thing) - 1;
		if (order < MIN_ORDER) {
			thing += (1U << MIN_ORDER);
			goto skip_upper_bound;
		}
		thing |= (1U << order);
	}
	if (thing != size)
		thing += (1U << __builtin_ctz(thing));
skip_upper_bound:
	i = 0;
	double prop = (double)(thing-size)/thing;
	printf("thing/size: %f\n", prop);
	if (prop > 1.0) {
		abort();
	}
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

static int usefully_allocated;
static int useful_allocations_count;
static int initial_max_order_blocks_left;

static void on_memory_exhausted(void)
{
	int total_ram = initial_max_order_blocks_left * (1 << MAX_ORDER);
	printf("%d %d %d %f\n",
	       total_ram,
	       usefully_allocated,
	       useful_allocations_count,
	       (float)(total_ram - usefully_allocated) * 100 / total_ram);
	exit(0);
}

// NOTE: returns freshly malloced array of iovec-s
static
struct iovec *blob_to_iovecs(struct chunked_blob *blob, int *iovecs_count)
{
	struct iovec *rv = malloc(sizeof(struct iovec) * CHUNKS_COUNT);
	if (!rv) {
		perror("malloc");
		abort();
	}
	int orders[CHUNKS_COUNT];
	undefined size = blob->size;
	int i = 0;
	value_size_to_block_sizes(size, orders);

	while (size > 0) {
		unsigned current_size = orders[i];
		char *p = (i == 0) ? (char *)(blob + 1) : blob->other_chunks[i-1];
		i++;
		for (; current_size > 0; current_size--) {
			
		}
	}
}

static
void fill_block(int block_number)
{
	struct random_data rdata;
	srandom_r(block_number, &rdata);

	struct chunked_blob *blob = blobs[block_number];
	int orders[CHUNKS_COUNT];
	undefined size = blob->size;
	int i = 0;
	value_size_to_block_sizes(size, orders);

	while (size > 0) {
		unsigned current_size = orders[i];
		char *p = (i == 0) ? (char *)(blob + 1) : blob->other_chunks[i-1];
		i++;
		for (; current_size > 0; current_size--) {
			
		}
	}
}

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

	initial_max_order_blocks_left = 512U*1024U*1024U / (1U << MAX_ORDER);
	max_order_blocks_left = initial_max_order_blocks_left;

	for (i = 0; i < BLOBS_COUNT; i++) {
		unsigned size = 128 + random() % (128*1024);
		allocate_blob(size);
		usefully_allocated += size;
		useful_allocations_count++;
	}
	fprintf(stderr, "too successfull allocation!\n");
	return 1;
}
