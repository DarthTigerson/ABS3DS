/* Block cache over a fetch callback: MP4 sample tables read on demand. */
#include <stdlib.h>

#include "blockcache.h"
#include "mp4.h"
#include "test.h"

typedef struct { const uint8_t *data; uint64_t size; int calls; uint64_t bytes; int fail_next; } Src;

static int fetch(void *ud, uint64_t off, uint32_t len, uint8_t *dst) {
    Src *s = ud;
    s->calls++;
    if (s->fail_next) { s->fail_next--; return -1; }
    if (off >= s->size) return 0;
    if (off + len > s->size) len = (uint32_t)(s->size - off);
    memcpy(dst, s->data + off, len);
    s->bytes += len;
    return (int)len;
}

TEST(reads_span_blocks_and_cache) {
    uint8_t *data = malloc(300000);
    for (int i = 0; i < 300000; i++) data[i] = (uint8_t)(i * 7 + (i >> 9));
    Src src = {data, 300000, 0, 0, 0};
    BlockCache *bc = bc_new(300000, 65536, 2, fetch, &src);
    Reader r = bc_reader(bc);
    uint8_t buf[100000];
    /* a read crossing a block boundary */
    CHECK_EQ_INT(reader_read_full(&r, 65000, buf, 1000), 0);
    CHECK(memcmp(buf, data + 65000, 1000) == 0);
    CHECK_EQ_INT(src.calls, 2);
    /* same blocks again: no new fetches */
    CHECK_EQ_INT(reader_read_full(&r, 65100, buf, 500), 0);
    CHECK_EQ_INT(src.calls, 2);
    /* the tail block is short; reads past the end stop there */
    CHECK_EQ_INT(r.read_at(r.ctx, 299990, buf, 100), 10);
    CHECK(memcmp(buf, data + 299990, 10) == 0);
    CHECK_EQ_INT(r.read_at(r.ctx, 300000, buf, 10), 0);
    /* LRU of 2: the oldest block was evicted */
    int before = src.calls;
    CHECK_EQ_INT(reader_read_full(&r, 1, buf, 10), 0);
    CHECK_EQ_INT(src.calls, before + 1);
    /* a failed fetch surfaces as an error, and the next read retries */
    src.fail_next = 1;
    CHECK(r.read_at(r.ctx, 131072 + 5, buf, 10) < 0);
    CHECK_EQ_INT(r.read_at(r.ctx, 131072 + 5, buf, 10), 10);
    CHECK(memcmp(buf, data + 131072 + 5, 10) == 0);
    int calls;
    uint64_t bytes;
    bc_stats(bc, &calls, &bytes);
    CHECK(calls > 0 && bytes > 0);
    bc_free(bc);
    free(data);
}

TEST(mp4_tables_without_whole_moov) {
    FILE *f = fopen("tests/fixtures/media/aac_lc.m4b", "rb");
    if (!f) { printf("  (fixture missing, skipped)\n"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(n);
    if (fread(data, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    Src src = {data, (uint64_t)n, 0, 0, 0};
    BlockCache *bc = bc_new((uint64_t)n, 4096, 8, fetch, &src);
    Reader r = bc_reader(bc);
    uint64_t off, size;
    CHECK_EQ_INT(mp4_find_moov(&r, &off, &size), 0);
    static Mp4 m;
    CHECK_EQ_INT(mp4_open_at(&m, &r, off, size), 0);
    CHECK_EQ_INT(m.codec, MP4_AAC);
    double actual;
    CHECK_EQ_INT(mp4_seek(&m, 12.0, &actual), 0);
    CHECK_NEAR(actual, 12.0, 0.05);
    printf("  moov %llu bytes; fetched %llu bytes in %d requests\n", (unsigned long long)size,
           (unsigned long long)src.bytes, src.calls);
    CHECK(src.bytes < (uint64_t)n);
    bc_free(bc);
    free(data);
}

void suite_blockcache(void) {
    RUN(reads_span_blocks_and_cache);
    RUN(mp4_tables_without_whole_moov);
}
