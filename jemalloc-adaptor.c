#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <jemalloc/jemalloc.h>

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

void *allocate_blob(unsigned size)
{
	/* maybe_init(); */
	return malloc(size);
}

void free_blob(void *blob)
{
	/* maybe_init(); */
	return free(blob);
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
	maybe_init();

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

