#include <stdlib.h>
#include <assert.h>

#include "minimalloc.h"
#include "common.h"


static struct mini_state *ms;
static size_t total_allocated;

static
void *mi_alloc(size_t size)
{
	if (!ms) {
		ms = mini_init(malloc, 0);
		if (!ms) {
			abort();
		}
	}
	void *rv = mini_malloc(ms, size);
	total_allocated += size;
	return rv;
}

static
void mi_free(void *p, size_t size)
{
	assert(ms);
	mini_free(ms, p);
	total_allocated -= size;
}

static
size_t mi_get_total_allocated_size(void)
{
	return total_allocated;
}

allocation_functions mini_fns = {
	.alloc = mi_alloc,
	.free = mi_free,
	.get_total_allocated_size = mi_get_total_allocated_size
};

