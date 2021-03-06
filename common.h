#ifndef COMMON_H
#define COMMON_H
#include <sys/types.h>

typedef struct {
	const char *name;
	void *(*alloc)(size_t);
	void (*free)(void *, size_t);
	size_t (*get_total_allocated_size)(void);
	void (*iterate_chunks)(void *p, size_t size, void *data,
			       void (*cb)(void *p, size_t s, void *data));
} allocation_functions;

extern allocation_functions *main_fns;
extern allocation_functions *chunky_slave_fns;
extern allocation_functions chunky_fns;
extern allocation_functions jemalloc_fns;
extern allocation_functions mini_fns;
extern allocation_functions buddy_fns;
extern allocation_functions dl_fns;

void *touch_pages(void *p, size_t size);
size_t rss_allocated();

#endif
