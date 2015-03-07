#include "minimalloc.h"
#include "common.h"

static struct mini_state *ms;
static size_t total_allocated;

static
void *mini_alloc(size_t size)
{
	if (!ms) {
		ms = mini_init(mi_mallocer, 0);
		if (!ms) {
			abort();
		}
	}
	void *rv = mini_malloc(ms, size);
	total_allocated += size;
	return rv;
}

static
void mini_free(void *p, size_t size)
{
	assert(ms);
	mini_free(ms, p);
	total_allocated -= size;
}

static
size_t mini_get_total_allocated_size(void)
{
	return total_allocated;
}

allocation_functions mini_fns = {
	.alloc = mini_alloc,
	.free = mini_free,
	.get_total_allocated_size = .mini_get_total_allocated_size
};

