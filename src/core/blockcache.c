#include "blockcache.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t index; /* UINT64_MAX when empty */
    uint32_t len;
    uint32_t used;  /* LRU stamp */
    uint8_t *data;
} Block;

struct BlockCache {
    uint64_t size;
    uint32_t bsize;
    int n;
    Block *blocks;
    uint32_t clock;
    BcFetch fetch;
    void *ud;
    int fetches;
    uint64_t bytes;
};

/* The block holding index, fetched if needed; NULL with *err set on failure. */
static Block *get(BlockCache *c, uint64_t index, int *err) {
    Block *victim = &c->blocks[0];
    for (int i = 0; i < c->n; i++) {
        if (c->blocks[i].index == index) {
            c->blocks[i].used = ++c->clock;
            return &c->blocks[i];
        }
        if (c->blocks[i].used < victim->used) victim = &c->blocks[i];
    }
    uint64_t off = index * c->bsize;
    uint32_t len = c->bsize;
    if (c->size && off + len > c->size) len = (uint32_t)(c->size - off);
    victim->index = UINT64_MAX; /* invalid until the fetch succeeds */
    int k = c->fetch(c->ud, off, len, victim->data);
    c->fetches++;
    if (k <= 0) { *err = k < 0 ? k : -1; return NULL; }
    c->bytes += (uint64_t)k;
    victim->index = index;
    victim->len = (uint32_t)k;
    victim->used = ++c->clock;
    return victim;
}

static int bc_read(void *ctx, uint64_t off, void *buf, uint32_t n) {
    BlockCache *c = ctx;
    if (c->size && off >= c->size) return 0;
    if (c->size && off + n > c->size) n = (uint32_t)(c->size - off);
    uint32_t done = 0;
    while (done < n) {
        uint64_t at = off + done;
        int err = 0;
        Block *b = get(c, at / c->bsize, &err);
        if (!b) return done ? (int)done : err;
        uint32_t in = (uint32_t)(at % c->bsize);
        if (in >= b->len) break;
        uint32_t k = b->len - in;
        if (k > n - done) k = n - done;
        memcpy((uint8_t *)buf + done, b->data + in, k);
        done += k;
    }
    return (int)done;
}

BlockCache *bc_new(uint64_t size, uint32_t block, int nblocks, BcFetch fetch, void *ud) {
    BlockCache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->size = size;
    c->bsize = block ? block : 65536;
    c->n = nblocks > 0 ? nblocks : 8;
    c->fetch = fetch;
    c->ud = ud;
    c->blocks = calloc(c->n, sizeof(Block));
    if (!c->blocks) { free(c); return NULL; }
    for (int i = 0; i < c->n; i++) {
        c->blocks[i].index = UINT64_MAX;
        if (!(c->blocks[i].data = malloc(c->bsize))) { bc_free(c); return NULL; }
    }
    return c;
}

Reader bc_reader(BlockCache *c) {
    Reader r = {c, bc_read, c->size};
    return r;
}

void bc_stats(const BlockCache *c, int *fetches, uint64_t *bytes) {
    if (fetches) *fetches = c->fetches;
    if (bytes) *bytes = c->bytes;
}

void bc_free(BlockCache *c) {
    if (!c) return;
    if (c->blocks)
        for (int i = 0; i < c->n; i++) free(c->blocks[i].data);
    free(c->blocks);
    free(c);
}
