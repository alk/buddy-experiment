#include "common.h"

extern void *dlmalloc(size_t size);
extern void dlfree(void *);

static size_t dl_total_allocated;

static
void *dl_alloc(size_t size)
{
	void *rv = dlmalloc(size);
	dl_total_allocated += size;
	return rv;
}

static
void dl_free(void *p, size_t size)
{
	dlfree(p);
	dl_total_allocated -= size;
}

static
size_t dl_get_total_allocated_size(void)
{
	return dl_total_allocated;
}

allocation_functions dl_fns = {
	.name = "dl",
	.alloc = dl_alloc,
	.free = dl_free,
	.get_total_allocated_size = dl_get_total_allocated_size
};
