#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <stdbool.h>
#include "common.h"

static void dump_chunks(const char *path);

static
void *allocate_blob(unsigned size)
{
	return main_fns->alloc(size);
}

static
void free_blob(void *blob, size_t size)
{
	main_fns->free(blob, size);
}

static
size_t get_total_allocated_size(void)
{
	return main_fns->get_total_allocated_size();
}

#define ALLOCATE_UNTIL_MB ((1200 + 15) / 16 * 16 - 1)

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
void bump_sizes(void)
{
	for (int k = 0; k < BLOBS_COUNT; k++) {
		if (!blobs[k] || sizes[k] > (minimal_size + size_range / 2)) {
			continue;
		}
		unsigned old_size = sizes[k];
		unsigned new_size = old_size + (old_size >> 8);
		if (new_size > minimal_size + size_range) {
			new_size = minimal_size + size_range;
		}
		if (new_size == old_size) {
			continue;
		}
		free_blob(blobs[k], sizes[k]);
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

static
void do_simulate_dump(const char *path, bool dont_bump)
{
	typedef struct {
		uint32_t sec;
		uint32_t len;
	} sim_evt_t;
	sim_evt_t evt;
	FILE *f;
	f = fopen(path, "rb");
	if (!f) {
		perror("fopen");
		abort();
	}
	while (!feof(f)) {
		int rv = fread(&evt, sizeof(evt), 1, f);
		if (rv != 1) {
			if (rv == 0) {
				break;
			}
			perror("fread");
			abort();
		}
		/* evt.len *= 64; */
		if (evt.len < 128) {
			continue;
		}
		if (blobs[evt.sec]) {
			free_blob(blobs[evt.sec], sizes[evt.sec]);
			usefully_allocated -= sizes[evt.sec];
			/* fprintf(stderr, "overwritten sec: %llu\n", (unsigned long long)evt.sec); */
			useful_allocations_count--;
		}
		sizes[evt.sec] = evt.len;
		blobs[evt.sec] = allocate_blob(evt.len);
		usefully_allocated += evt.len;
		useful_allocations_count++;
	}
	print_current_stats();
	if (!dont_bump) {
		bump_sizes();
	}
	print_current_stats();
}

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

static
void usage_and_exit(int argc, char **argv)
{
	fprintf(stderr,
		"usage: %s [-m minimal_size] [-r size_range] [-c] [-b]"
		"[-t allocator] [-n]\n"
		"\n"
		"  -b dont do bumps\n"
		"  -c wrap with chunky allocator\n"
		"  -n randomize rnd\n"
		"\n"
		"Supported allocator types: dl, mini, je, buddy\n",
		argv[0]);
	exit(1);
}

allocation_functions *main_fns = &dl_fns;

int main(int argc, char **argv)
{
	int i;
	int rv;
	bool dont_bump = false;
	bool use_chunky = false;
	bool randomize = false;
	const char *read_dump = NULL;
	const char *dump_first_path = NULL;

	while ((i = getopt(argc, argv, "bcd:m:np:r:t:")) != -1) {
		switch (i) {
		case 'b':
			dont_bump = true;
			break;
		case 'c':
			use_chunky = true;
			break;
		case 'd':
			read_dump = optarg;
			break;
		case 'm':
			if (!parse_int(&minimal_size, optarg, 128, 2*1024*1024)) {
				fprintf(stderr, "invalid minimal_size\n");
				return 1;
			}
			break;
		case 'n':
			randomize = true;
			break;
		case 'p':
			dump_first_path = optarg;
			break;
		case 'r':
			if (!parse_int(&size_range, optarg, 1, 20*1024*1024)) {
				fprintf(stderr, "invalid size_range\n");
				usage_and_exit(argc, argv);
			}
			break;
		case 't':
			if (strcmp(optarg, "dl") == 0) {
				main_fns = &dl_fns;
			} else if (strcmp(optarg, "mini") == 0) {
				main_fns = &mini_fns;
			} else if (strcmp(optarg, "je") == 0) {
				main_fns = &jemalloc_fns;
			} else if (strcmp(optarg, "buddy") == 0) {
				main_fns = &buddy_fns;
			} else {
				fprintf(stderr, "invalid type: %s\n", optarg);
				usage_and_exit(argc, argv);
			}
			break;
		case '?':
			fprintf(stderr, "invalid option\n");
			usage_and_exit(argc, argv);
		default:
			abort();
		}
	}

	if (use_chunky) {
		chunky_slave_fns = main_fns;
		main_fns = &chunky_fns;
		printf("name = chunky:%s\n", chunky_slave_fns->name);
	} else {
		printf("name = %s\n", main_fns->name);
	}

	printf("minimal_size = %d\n", minimal_size);
	printf("size_range = %d\n", size_range);

	if (read_dump) {
		do_simulate_dump(read_dump, dont_bump);
		return 0;
	}

	if (randomize) {
		struct timeval tv;
		unsigned seed;
		rv = gettimeofday(&tv, 0);
		if (rv) {
			perror("gettimeofday");
			abort();
		}
		seed = (unsigned)tv.tv_sec ^ (unsigned)tv.tv_usec ^ (unsigned)getpid();
		srandom((unsigned)seed);
		printf("seeded random with: 0x%08x\n", seed);
	} else {
		srandom(0);
	}

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

		if (!dont_bump && (times % 100000) == 0) {
			if (dump_first_path) {
				dump_chunks(dump_first_path);
				dump_first_path = NULL;
			}
			bump_sizes();
		}

		if ((times % 100000) == 0) {
			printf("stats (%d):\n", times);
			print_current_stats();
			printf("\n\n");
		}

		for (; i >= 0; i--) {
			if (blobs[i] && random() % 1000 < 5) {
				usefully_allocated -= sizes[i];
				useful_allocations_count--;
				free_blob(blobs[i], sizes[i]);
				blobs[i] = 0;
			}
		}
	}

	return 0;
}


void *touch_pages(void *_p, size_t size)
{
	char *p = _p;
	char *pend = p + size;
	do {
		*p = 0;
		p += ((~(uintptr_t)p) & 4095) + 1;
	} while (p < pend);
	return _p;
}

size_t rss_allocated(void)
{
	unsigned long long dummy, rss_pages = 0;
	FILE *sm = fopen("/proc/self/statm", "r");
	if (!sm) {
		perror("fopen(\"/proc/self/statm\")");
		abort();
	}
	fscanf(sm, "%llu %llu", &dummy, &rss_pages);
	if (rss_pages == 0) {
		perror("fscanf");
		abort();
	}
	fclose(sm);
	return (size_t)rss_pages * 4096;
}

static FILE *diagnose_file;

static
void diagnose_cb(void *ptr, size_t sz, void *dummy)
{
	fprintf(diagnose_file, "%016llx %llx\n", (unsigned long long)(uintptr_t)ptr, (unsigned long long)sz);
}

static
void dump_chunks(const char *path)
{
	int i;
	if (!main_fns->iterate_chunks) {
		return;
	}
	diagnose_file = fopen(path, "w");
	for (i = 0; i < BLOBS_COUNT; i++) {
		if (!blobs[i]) {
			continue;
		}
		main_fns->iterate_chunks(blobs[i], sizes[i], NULL, diagnose_cb);
	}
	fclose(diagnose_file);
}

static
void diagnose_and_exit(void)
{
	dump_chunks("diag_file");
	abort();
}

static
intptr_t total_increment;

void *aasbrk(intptr_t increment)
{
	extern void * __sbrk(intptr_t);
	total_increment += increment;
	if (total_increment >= 1484034048) {
		fprintf(stderr, "last increment: %lld\n", (unsigned long long)increment);
		diagnose_and_exit();
	}
	return __sbrk(increment);
}

static __attribute__((destructor))
void print_ti(void)
{
	fprintf(stderr, "total_increment: %lld\n", (unsigned long long)total_increment);
}
