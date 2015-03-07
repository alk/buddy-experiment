#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <jemalloc/jemalloc.h>
#include "common.h"

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

void *je_allocate_blob(size_t size)
{
	/* maybe_init(); */
	return malloc(size);
}

void je_free_blob(void *blob, size_t _unused)
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

size_t je_get_total_allocated_size(void)
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

allocation_functions jemalloc_fns = {
	.name = "jemalloc",
	.alloc = je_allocate_blob,
	.free = je_free_blob,
	.get_total_allocated_size = je_get_total_allocated_size
};
