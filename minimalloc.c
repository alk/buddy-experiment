#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <bsd/sys/tree.h>
#include <assert.h>

#include "minimalloc.h"

struct free_span;

RB_HEAD(mini_rb, free_span);

struct mini_state {
	mini_mallocer mallocer;
	mini_freer freer;
	struct mini_rb head;
	void **next_chunk;
};

struct free_span {
	size_t size;
	RB_ENTRY(free_span) rb_link;
};

#define MIN_SPAN_SIZE (sizeof(struct free_span) + sizeof(intptr_t))

#define SPAN_SIZE_FREE_MASK (~(((size_t)-1) >> 1))
#define SPAN_SIZE_PREV_FREE_MASK (SPAN_SIZE_FREE_MASK >> 1)
#define SPAN_SIZE_VALUE_MASK (SPAN_SIZE_PREV_FREE_MASK - 1)

static inline
int mini_rb_cmp(struct free_span *a, struct free_span *b)
{
	size_t asize = a->size & SPAN_SIZE_VALUE_MASK;
	size_t bsize = b->size & SPAN_SIZE_VALUE_MASK;

	if (asize == bsize) {
		/* here it gets a bit subtle. perfect_fit "span" that we
		 * use in malloc below has arbitrary address which we
		 * don't want to involve at all. It has to be
		 * semantically at NULL, so that span of perfect size
		 * at lowest address is still greater than
		 * perfect_fit */
		/* We detect perfect_fit by lack of
		 * SPAN_SIZE_FREE_MASK. And we know at most one of
		 * <a,b> can be perfect_fit. And normally both are
		 * not. */
		if ((a->size ^ b->size) & SPAN_SIZE_FREE_MASK)
			return a->size - b->size;
		return (intptr_t)a - (intptr_t)b;
	}
	return (int)((ssize_t)(asize) - (ssize_t)(bsize));
}

/* looks like some bug in libbsd I have on my box. It fixes compile
 * error due it's attempt to declare static functions as unused, which
 * is required to prevent compiler warning(s) in case some of
 * functions are not used */
#ifndef __unused
#define __unused __attribute__((unused))
#endif

RB_PROTOTYPE_STATIC(mini_rb, free_span, rb_link, mini_rb_cmp);
RB_GENERATE_STATIC(mini_rb, free_span, rb_link, mini_rb_cmp);

#define CHUNK_SIZE (4*1024*1024)

static inline
size_t min_size(size_t a, size_t b)
{
	return (a > b) ? b : a;
}

static inline
size_t max_size(size_t a, size_t b)
{
	return (a < b) ? b : a;
}

static
void insert_span(struct mini_state *st, void *at, size_t size)
{
	struct free_span *span = (struct free_span *)at;
	size_t *after_span = (size_t *)((char *)at + size);
	span->size = size | SPAN_SIZE_FREE_MASK;
	after_span[0] |= SPAN_SIZE_PREV_FREE_MASK;
	after_span[-1] = size;
	mini_rb_RB_INSERT(&st->head, span);
}

struct mini_state *mini_init(mini_mallocer mallocer, mini_freer freer)
{
	struct initial_stuff {
		struct mini_state state;
		struct free_span first_span;
	};
	struct initial_stuff *first_chunk = mallocer(CHUNK_SIZE);
	char *first_chunk_end;
	if (!first_chunk)
		return 0;
	first_chunk->state.mallocer = mallocer;
	first_chunk->state.freer = freer;
	RB_INIT(&first_chunk->state.head);
	first_chunk->state.next_chunk = 0;
	first_chunk_end = (char *)first_chunk + CHUNK_SIZE - sizeof(size_t);
	insert_span(&first_chunk->state, &first_chunk->first_span,
		    first_chunk_end - (char *)&first_chunk->first_span);
	return &first_chunk->state;
}

void mini_deinit(struct mini_state *st)
{
	void **next = st->next_chunk;
	mini_freer freer = st->freer;
	void **current = (void **)st;
	do {
		freer(current);
		current = next;
		if (!current)
			return;
		next = (void **)*current;
	} while (1);
}

static void *do_malloc_with_fit(struct mini_state *st, size_t sz, struct free_span *fit);

static inline
size_t compute_allocation_sz(size_t size)
{
	return (max_size(size + sizeof(size_t), MIN_SPAN_SIZE) + sizeof(void *) - 1) & (size_t)(-sizeof(void *));
}

static void *mini_malloc_new_chunk(struct mini_state *st, size_t size)
{
	struct next_stuff {
		void **next_chunk;
		struct free_span first_span;
	};
	struct next_stuff *next_chunk;
	size_t chunk_overhead = sizeof(size_t)
		+ offsetof(struct next_stuff, first_span);
	size_t alloc_size = max_size(CHUNK_SIZE, compute_allocation_sz(size) + chunk_overhead);

	next_chunk = st->mallocer(alloc_size);
	if (!next_chunk)
		return 0;
	next_chunk->next_chunk = st->next_chunk;
	st->next_chunk = (void **)next_chunk;
	insert_span(st, &next_chunk->first_span, alloc_size - chunk_overhead);
	return do_malloc_with_fit(st, compute_allocation_sz(size), &next_chunk->first_span);
}

void *mini_malloc(struct mini_state *st, size_t size)
{
	size_t sz = compute_allocation_sz(size);
	struct free_span perfect_fit = {.size = sz};
	struct free_span *fit;

	fit = mini_rb_RB_NFIND(&st->head, &perfect_fit);

	if (!fit)
		return mini_malloc_new_chunk(st, size);

	return do_malloc_with_fit(st, sz, fit);
}

static
void *do_malloc_with_fit(struct mini_state *st, size_t sz, struct free_span *fit)
{
	ssize_t remaining_space;
	size_t *hdr;

	mini_rb_RB_REMOVE(&st->head, fit);

	remaining_space = (fit->size & SPAN_SIZE_VALUE_MASK) - sz;

	assert(remaining_space >= 0);

	if (remaining_space >= MIN_SPAN_SIZE) {
		char *hole = (char *)fit + sz;
		insert_span(st, hole, remaining_space);
	} else {
		sz = fit->size & SPAN_SIZE_VALUE_MASK;
	}

	hdr = (size_t *)fit;
	*hdr = sz;

	*((size_t *)(((char *)hdr) + sz)) &= ~SPAN_SIZE_PREV_FREE_MASK;

	return (void *)(hdr+1);
}

static void do_mini_free(struct mini_state *st, void *_ptr);

void mini_free(struct mini_state *st, void *_ptr)
{
	if (!_ptr) {
		return;
	}

	do_mini_free(st, _ptr);
}

static void do_mini_free(struct mini_state *st, void *_ptr)
{
	size_t *hdr = ((size_t *)_ptr) - 1;
	size_t raw_sz = hdr[0];
	size_t sz = raw_sz & SPAN_SIZE_VALUE_MASK;
	size_t *next_span = (size_t *)((char *)hdr + sz);

	if ((*next_span) & SPAN_SIZE_FREE_MASK) {
		struct free_span *real_next_span = (struct free_span *)next_span;
		mini_rb_RB_REMOVE(&st->head, real_next_span);
		sz += real_next_span->size & SPAN_SIZE_VALUE_MASK;
	}

	if (raw_sz & SPAN_SIZE_PREV_FREE_MASK) {
		size_t prev_size = hdr[-1];
		struct free_span *prev_span = (struct free_span *)((char *)hdr - prev_size);
		mini_rb_RB_REMOVE(&st->head, prev_span);
		sz += prev_size;
		hdr = (size_t *)prev_span;
	}

	insert_span(st, hdr, sz);
}

void *mini_realloc(struct mini_state *st, void *p, size_t new_size)
{
	void *new_p;
	if (new_size == 0) {
		mini_free(st, p);
		return 0;
	}
	new_p = mini_malloc(st, new_size);
	if (!new_p) {
		return new_p;
	}
	memcpy(new_p, p, min_size((((size_t *)p)[-1] & SPAN_SIZE_VALUE_MASK) - sizeof(size_t), new_size));
	mini_free(st, p);
	return new_p;
}

void mini_get_stats(struct mini_state *st, struct mini_stats *stats, mini_span_cb cb, void *cb_data)
{
	unsigned chunks_count = 1;
	void **chunk = st->next_chunk;
	struct free_span *span;

	while (chunk) {
		chunks_count++;
		chunk = (void **)*chunk;
	}

	stats->free_spans_count = 0;
	stats->free_space = 0;
	stats->os_chunks_count = chunks_count;

	RB_FOREACH(span, mini_rb, &st->head) {
		size_t size = span->size & SPAN_SIZE_VALUE_MASK;
		stats->free_spans_count++;
		stats->free_space = size;
		if (cb) {
			cb((void *)span, size, cb_data);
		}
	}
}

struct fill_mini_spans_st {
	struct mini_spans *spans;
	size_t idx;
};

static
void mini_fill_mini_spans_cb(void *at, size_t size, void *_cbst)
{
	struct fill_mini_spans_st *cbst = (struct fill_mini_spans_st *)_cbst;
	if (cbst->idx >= cbst->spans->spans_capacity)
		return;
	cbst->spans->spans[cbst->idx].at = at;
	cbst->spans->spans[cbst->idx].size = size;
	cbst->idx++;
}

int mini_fill_mini_spans(struct mini_state *st, struct mini_spans *spans)
{
	struct fill_mini_spans_st cbst = {.spans = spans,
					  .idx = 0};
	mini_get_stats(st, &spans->stats, mini_fill_mini_spans_cb, &cbst);
	if (spans->stats.free_spans_count > spans->spans_capacity)
		return -(int)(spans->stats.free_spans_count);
	return (int)spans->stats.free_spans_count;
}
