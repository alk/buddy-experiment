#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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

struct free_block {
	struct block parent;
	int order;
};

/* 4 is not enough for 64 bit arches */
#define MIN_ORDER 5
#define MAX_ORDER 20

/*
 * This is heads of free lists for various block order sizes
 */
struct block *blocks_orders[MAX_ORDER+1];

static int max_order_blocks_alloced;

#define USED_MARKER ((struct block *)1)

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
	memset(rv, 0xcc, 1 << MAX_ORDER);
	rv->next = USED_MARKER;
	rv->pprev = 0;
	max_order_blocks_alloced++;
	return rv + 1;
}

static
void enqueue_free(struct block *ptr, int order)
{
	assert(ptr->next == USED_MARKER);
	assert(ptr->pprev == 0);
	struct block *old_front = blocks_orders[order];
	assert(old_front != USED_MARKER);
	ptr->next = old_front;
	ptr->pprev = blocks_orders + order;
	if (old_front) {
		assert(old_front->pprev == blocks_orders + order);
		old_front->pprev = &ptr->next;
	}
	((struct free_block *)ptr)->order = order;
	blocks_orders[order] = ptr;
}

static
void dequeue_free(struct block *ptr)
{
	assert(ptr->pprev != 0);
	assert(*(ptr->pprev) == ptr);
	assert(ptr->next != (struct block *)1);
	struct block *next = ptr->next;
	if (next)
		next->pprev = ptr->pprev;
	*(ptr->pprev) = next;
	ptr->next = USED_MARKER;
	ptr->pprev = 0;
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
		assert(p->pprev == blocks_orders + order);
		dequeue_free(p);
		assert(((struct free_block *)p)->order == order);
		goto out;
	}

	if (order == MAX_ORDER)
		return allocate_max_order_block();

	/*
         * struct block *blocks_orders_snapshot[MAX_ORDER+1];
	 * memcpy(blocks_orders_snapshot, blocks_orders, sizeof(blocks_orders));
         */

	p = (struct block *)allocate_block(order+1) - 1;
	buddy = (struct block *)((char *)p + (1 << order));
	buddy->next = USED_MARKER;
	buddy->pprev = 0;
	enqueue_free(buddy, order);
out:
	return p + 1;
}

static
void free_block(void *ptr, int order)
{
	struct block *p = (struct block *)ptr - 1;
	assert(p->next == USED_MARKER);
	if (order < MAX_ORDER) {
		struct block *buddy = (struct block *)((intptr_t)p ^ (1 << order));
		if (buddy->next != USED_MARKER && (((struct free_block *)buddy)->order == order)) {
			/* if buddy is free as well with same order, we should combine
			 * with it, by first unlinking it from
			 * free-list */
			dequeue_free(buddy);
			if (buddy > p)
				buddy = p;
			free_block(buddy+1, order+1);
			return;
		}
	}

	enqueue_free(p, order);
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

float max_waste;

static
void print_current_stats(void)
{
	int total_ram = max_order_blocks_alloced * (1U << MAX_ORDER);
	float waste = (float)(total_ram - usefully_allocated) * 100 / total_ram;
	if (waste > max_waste)
		max_waste = waste;
	printf("got from OS: %d\nApp allocated: %d\nAllocations count:%d\nwaste %f %f %%\n",
	       total_ram,
	       usefully_allocated,
	       useful_allocations_count,
	       waste, max_waste);
}

#define BLOBS_COUNT (1024*1024)

struct chunked_blob *blobs[BLOBS_COUNT];

int minimal_size = 128;
int size_range = 65536;

static
int parse_int(int *place, char *arg, int min, int max)
{
	char *endptr;
	long val = strtol(arg, &endptr, 10);
	if (!*arg || *endptr)
		return 0;
	if (val < min || val > max)
		return 0;
	*place = val;
	return 1;
}

int main(int argc, char **argv)
{
	int i;
	struct timeval tv;
	int rv;

	while ((i = getopt(argc, argv, "m:r:")) != -1) {
		switch (i) {
		case 'm':
			if (!parse_int(&minimal_size, optarg, 128, 2*1024*1024)) {
				fprintf(stderr, "invalid minimal_size\n");
				return 1;
			}
			break;
		case 'r':
			if (!parse_int(&size_range, optarg, 1, 20*1024*1024)) {
				fprintf(stderr, "invalid size_range\n");
				return 1;
			}
			break;
		case '?':
			fprintf(stderr, "invalid option\n");
			return 1;
		default:
			abort();
		}
	}

	printf("minimal_size = %d\n", minimal_size);
	printf("size_range = %d\n", size_range);

	rv = gettimeofday(&tv, 0);
	if (rv) {
		perror("gettimeofday");
		abort();
	}
	/* srandom((unsigned)tv.tv_sec ^ (unsigned)tv.tv_usec); */
	srandom(0);

	for (int times = 100000000; times >= 0; times--) {
		for (i = 0; i < BLOBS_COUNT; i++) {
			if (blobs[i]) {
				continue;
			}
			unsigned size = minimal_size + random() % size_range;
			blobs[i] = allocate_blob(size);
			assert(blobs[i]->size == size);
			usefully_allocated += size;
			useful_allocations_count++;
			if (usefully_allocated >= (ALLOCATE_UNTIL_MB * 1048576))
				break;
		}

		if (i >= BLOBS_COUNT) {
			fprintf(stderr, "too successful allocation!\n");
			return 1;
		}

		printf("stats:\n");
		print_current_stats();
		printf("\n\n");

		for (; i >= 0; i--) {
			if (blobs[i] && random() % 1000 < 2) {
				usefully_allocated -= blobs[i]->size;
				useful_allocations_count--;
				free_blob(blobs[i]);
				blobs[i] = 0;
			}
		}
	}

	return 0;
}
