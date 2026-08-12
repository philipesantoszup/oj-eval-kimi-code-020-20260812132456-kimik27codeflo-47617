#include "buddy.h"

#include <stdint.h>
#include <stdlib.h>

#define MIN_RANK 1
#define MAX_RANK 16

static void *pool_base = NULL;
static int pool_pages = 0;

/* Metadata for each page.  These arrays are allocated separately from the
 * managed memory so that we never touch the pages given to us by the caller. */
static int *blk_rank = NULL;   /* rank of the block starting at this page; 0 if none */
static int *blk_free = NULL;   /* 1 if the block starting here is free */
static int *next_idx = NULL;   /* next page index in the rank's free list */
static int *prev_idx = NULL;   /* previous page index in the rank's free list */

static int free_head[MAX_RANK + 1];
static int free_count[MAX_RANK + 1];

static inline int valid_rank(int rank) {
    return rank >= MIN_RANK && rank <= MAX_RANK;
}

static inline int pages_in_rank(int rank) {
    return 1 << (rank - 1);
}

static inline int page_index(void *p) {
    return (int)(((uintptr_t)p - (uintptr_t)pool_base) >> 12);
}

static inline int in_pool(void *p) {
    if (!p)
        return 0;
    uintptr_t addr = (uintptr_t)p;
    uintptr_t base = (uintptr_t)pool_base;
    uintptr_t end = base + (uintptr_t)pool_pages * 4096ULL;
    return addr >= base && addr < end;
}

static void clear_metadata(void) {
    for (int i = 0; i < pool_pages; i++) {
        blk_rank[i] = 0;
        blk_free[i] = 0;
        next_idx[i] = -1;
        prev_idx[i] = -1;
    }
    for (int r = 1; r <= MAX_RANK; r++) {
        free_head[r] = -1;
        free_count[r] = 0;
    }
}

/* Maximum rank of a block that can start at page idx while staying inside
 * the pool.  Rank is determined by offset from pool_base, not absolute address. */
static int max_rank_at(int idx) {
    int rank = 1;
    int pages = 1;
    while (rank < MAX_RANK) {
        int next_pages = pages << 1;
        if ((idx & (next_pages - 1)) != 0)
            break;
        if (idx + next_pages > pool_pages)
            break;
        pages = next_pages;
        rank++;
    }
    return rank;
}

static void add_free(int idx, int rank) {
    blk_rank[idx] = rank;
    blk_free[idx] = 1;
    next_idx[idx] = free_head[rank];
    prev_idx[idx] = -1;
    if (free_head[rank] != -1)
        prev_idx[free_head[rank]] = idx;
    free_head[rank] = idx;
    free_count[rank]++;
}

static void remove_free(int idx) {
    int rank = blk_rank[idx];
    int n = next_idx[idx];
    int p = prev_idx[idx];
    if (n != -1)
        prev_idx[n] = p;
    if (p != -1)
        next_idx[p] = n;
    else
        free_head[rank] = n;
    free_count[rank]--;
    blk_free[idx] = 0;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0)
        return -EINVAL;

    if (pool_pages > 0) {
        free(blk_rank);
        free(blk_free);
        free(next_idx);
        free(prev_idx);
    }

    pool_base = p;
    pool_pages = pgcount;

    blk_rank = (int *)calloc(pgcount, sizeof(int));
    blk_free = (int *)calloc(pgcount, sizeof(int));
    next_idx = (int *)malloc(pgcount * sizeof(int));
    prev_idx = (int *)malloc(pgcount * sizeof(int));
    if (!blk_rank || !blk_free || !next_idx || !prev_idx)
        return -ENOSPC;

    clear_metadata();

    /* Decompose the whole pool into power-of-two aligned free blocks. */
    int idx = 0;
    while (idx < pool_pages) {
        int rank = max_rank_at(idx);
        add_free(idx, rank);
        idx += pages_in_rank(rank);
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (!valid_rank(rank))
        return ERR_PTR(-EINVAL);

    int r;
    for (r = rank; r <= MAX_RANK; r++) {
        if (free_head[r] != -1)
            break;
    }
    if (r > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    int idx = free_head[r];
    remove_free(idx);

    /* Split down to the requested rank.  Keep the lower half as the block
     * we are going to allocate and put the upper halves back into the lists. */
    while (r > rank) {
        r--;
        int buddy = idx ^ pages_in_rank(r);
        add_free(buddy, r);
    }

    blk_rank[idx] = rank;
    blk_free[idx] = 0;
    return (void *)((uintptr_t)pool_base + ((uintptr_t)idx << 12));
}

int return_pages(void *p) {
    if (!p || !in_pool(p))
        return -EINVAL;

    if ((((uintptr_t)p - (uintptr_t)pool_base) & 4095) != 0)
        return -EINVAL;

    int idx = page_index(p);
    if (idx < 0 || idx >= pool_pages)
        return -EINVAL;

    int rank = blk_rank[idx];
    if (!valid_rank(rank) || blk_free[idx])
        return -EINVAL;

    while (rank < MAX_RANK) {
        int buddy = idx ^ pages_in_rank(rank);
        if (buddy < 0 || buddy >= pool_pages)
            break;
        if (!blk_free[buddy] || blk_rank[buddy] != rank)
            break;

        remove_free(buddy);
        blk_rank[idx] = 0;
        blk_rank[buddy] = 0;
        if (buddy < idx)
            idx = buddy;
        rank++;
    }

    add_free(idx, rank);
    return OK;
}

int query_ranks(void *p) {
    if (!p || !in_pool(p))
        return -EINVAL;

    if ((((uintptr_t)p - (uintptr_t)pool_base) & 4095) != 0)
        return -EINVAL;

    int idx = page_index(p);
    if (idx < 0 || idx >= pool_pages)
        return -EINVAL;

    /* If this is the start of an allocated block, report its rank. */
    if (blk_rank[idx] > 0 && !blk_free[idx])
        return blk_rank[idx];

    /* Otherwise report the maximum rank the address could have as a free block. */
    return max_rank_at(idx);
}

int query_page_counts(int rank) {
    if (!valid_rank(rank))
        return -EINVAL;

    return free_count[rank];
}
