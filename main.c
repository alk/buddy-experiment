#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/uio.h>


extern void *allocate_blob(unsigned size);
extern void free_blob(void *blob);

extern size_t get_total_allocated_size(void);

#define ALLOCATE_UNTIL_MB 384

static int usefully_allocated;
static int useful_allocations_count;

float max_waste;

static
void print_current_stats(void)
{
	int total_ram = get_total_allocated_size();
	float waste = (float)(total_ram - usefully_allocated) * 100 / total_ram;
	if (waste > max_waste) {
		max_waste = waste;
	}
	printf("got from OS: %d\nApp allocated: %d\nAllocations count:%d\nwaste %f %f %%\n",
	       total_ram,
	       usefully_allocated,
	       useful_allocations_count,
	       waste, max_waste);
}

#define BLOBS_COUNT (1024*1024)

void *blobs[BLOBS_COUNT];
size_t sizes[BLOBS_COUNT];

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
			sizes[i] = size;
			usefully_allocated += size;
			useful_allocations_count++;
			if (usefully_allocated >= (ALLOCATE_UNTIL_MB * 1048576))
				break;
		}

		if (i >= BLOBS_COUNT) {
			fprintf(stderr, "too successful allocation!\n");
			return 1;
		}

		if ((times % 100000) == 0) {
			for (int k = 0; k < BLOBS_COUNT; k++) {
				if (!blobs[k]) {
					continue;
				}
				unsigned old_size = sizes[k];
				unsigned new_size = old_size + (old_size >> 2);
				if (new_size > minimal_size + size_range) {
					new_size = minimal_size + size_range;
				}
				if (new_size == old_size) {
					continue;
				}
				free_blob(blobs[k]);
				blobs[k] = 0;
				usefully_allocated -= old_size;
				useful_allocations_count--;
				if (usefully_allocated < (ALLOCATE_UNTIL_MB * 1048576)) {
					blobs[k] = allocate_blob(new_size);
					sizes[k] = new_size;
					usefully_allocated += new_size;
					useful_allocations_count++;
				}
			}
		}

		if ((times % 100000) == 0) {
			printf("stats:\n");
			print_current_stats();
			printf("\n\n");
		}

		for (; i >= 0; i--) {
			if (blobs[i] && random() % 1000 < 5) {
				usefully_allocated -= sizes[i];
				useful_allocations_count--;
				free_blob(blobs[i]);
				blobs[i] = 0;
			}
		}
	}

	return 0;
}
