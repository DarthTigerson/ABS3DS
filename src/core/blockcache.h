/* Random-access Reader over a fetch callback (e.g. HTTP Range requests) with a
 * small LRU of fixed-size blocks. Lets the MP4 demuxer read its sample tables
 * on demand instead of downloading the whole moov first. Single user. Pure C. */
#ifndef SHELF3DS_BLOCKCACHE_H
#define SHELF3DS_BLOCKCACHE_H
#include <stdint.h>

#include "reader.h"

/* Fills dst with bytes [off, off+len) (fewer only at the end of the file).
 * Returns the byte count, or <0 on error (-3 = interrupted). */
typedef int (*BcFetch)(void *ud, uint64_t off, uint32_t len, uint8_t *dst);

typedef struct BlockCache BlockCache;

BlockCache *bc_new(uint64_t size, uint32_t block, int nblocks, BcFetch fetch, void *ud);
Reader bc_reader(BlockCache *c);
void bc_free(BlockCache *c);
/* fetches made and bytes fetched (for logs and tests) */
void bc_stats(const BlockCache *c, int *fetches, uint64_t *bytes);

#endif
