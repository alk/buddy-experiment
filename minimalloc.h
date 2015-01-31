#ifndef MINIMALLOC_H
#define MINIMALLOC_H

struct mini_state;

typedef void *(*mini_mallocer)(size_t size);
typedef void (*mini_freer)(void *);

extern struct mini_state *mini_init(mini_mallocer mallocer, mini_freer freer);
extern void mini_deinit(struct mini_state *st);
extern void *mini_malloc(struct mini_state *, size_t size);
extern void mini_free(struct mini_state *, void *);
extern void *mini_realloc(struct mini_state *, void *, size_t);

struct mini_stats {
	unsigned os_chunks_count;
	unsigned free_spans_count;
	size_t free_space;
};

typedef void (*mini_span_cb)(void *span_start, size_t span_size, void *cb_data);

/* fills stats with heap stats (initializing free_space and
 * free_spans_count to 0. Then iterates over all spans calling cb and
 * accumulating free_spans_count and free_space */
extern void mini_get_stats(struct mini_state *st, struct mini_stats *stats, mini_span_cb cb, void *cb_data);

struct mini_spans {
	struct mini_stats stats;
	size_t spans_capacity;
	struct {
		void *at;
		size_t size;
	} spans[1];
};

extern int mini_fill_mini_spans(struct mini_state *st, struct mini_spans *spans);

#endif
