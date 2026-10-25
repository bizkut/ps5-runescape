/* Kernel model retains ownership at the same 64-KiB granularity as Wine. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stddef.h>
#include <setjmp.h>
#define BOOL int
#define BYTE unsigned char
#define TRUE 1
#define FALSE 0
#define DIRECT_UNIT_SIZE 65536
#define PS5_KERNEL_MAP_FIXED 0x10
#define PS5_KERNEL_PROT_CPU_READ 1
#define PS5_KERNEL_PROT_CPU_WRITE 2
#define PS5_KERNEL_PROT_CPU_EXEC 4
/* Extracted operations compare nonnegative byte counts, including ptrdiff_t. */
#define min(a,b) ((size_t)(a) < (size_t)(b) ? (size_t)(a) : (size_t)(b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define __PROSPERO__ 1
#define UINT_PTR uintptr_t
#define ROUND_ADDR(a,mask) ((void *)((uintptr_t)(a) & ~(uintptr_t)(mask)))
#define TRACE(...) do {} while (0)
#define ERR(...) direct_log(__VA_ARGS__)
#define VPROT_PS5_DIRECT 0x1000
static const uintptr_t granularity_mask = DIRECT_UNIT_SIZE - 1;
static const ptrdiff_t max_try_map_step = (ptrdiff_t)1 << 30;
static int increase_try_map_step = 1;
struct alloc_area;
static void *map_alloc_area(const struct alloc_area *area, void *base);
#define GIB (UINT64_C(1) << 30)
#define ORIGIN (UINT64_C(8) << 30)
#define UNITS ((128 * GIB) / DIRECT_UNIT_SIZE)
static BYTE native[UNITS];
static int direct_memory_enabled = 1, direct_chunked_reserve = 1;
static int calls, fail_call, displace_call, fail_unmap, fatal;
static int32_t large_error = (int32_t)0x8002000c;
static unsigned physical_releases, protects;
static jmp_buf failure;
static void direct_log(const char *format, ...) { (void)format; }
static int32_t direct_error(const char *a, const char *b, int32_t r) { (void)a; (void)b; return r; }
static int32_t direct_wrong_address(const char *a, const char *b, void *e, void *p)
{ (void)a; (void)b; (void)e; (void)p; return -1; }
static void ps5_direct_fail_fast(void) { fatal = 1; longjmp(failure, 1); }
static size_t index_of(void *base)
{
    assert((uintptr_t)base >= ORIGIN && (uintptr_t)base < ORIGIN + sizeof(native) * DIRECT_UNIT_SIZE);
    return ((uintptr_t)base - ORIGIN) / DIRECT_UNIT_SIZE;
}
static int32_t sceKernelReserveVirtualRange(void **base, size_t size, int flags, size_t alignment)
{
    size_t first, units = size / DIRECT_UNIT_SIZE;
    assert(!flags || flags == PS5_KERNEL_MAP_FIXED);
    assert(alignment == DIRECT_UNIT_SIZE && size && !(size % DIRECT_UNIT_SIZE));
    calls++;
    if (size > 8 * GIB) return large_error;
    if (calls == fail_call) return (int32_t)0x8002000c;
    if (calls == displace_call) *base = (void *)(uintptr_t)(ORIGIN + 96 * GIB);
    first = index_of(*base);
    assert(first + units <= UNITS);
    for (size_t i = first; i < first + units; ++i)
        if (native[i]) return (int32_t)0x8002000c;
    memset(native + first, 1, units);
    return 0;
}
static int32_t sceKernelMunmap(void *base, size_t size)
{
    size_t first = index_of(base), units = size / DIRECT_UNIT_SIZE;
    if (fail_unmap) return -1;
    for (size_t i = first; i < first + units; ++i) assert(native[i]);
    memset(native + first, 0, units);
    return 0;
}
static int32_t sceKernelReleaseDirectMemory(int64_t p, size_t size)
{
    assert(p >= 0 && size == DIRECT_UNIT_SIZE);
    physical_releases++;
    return 0;
}
static int32_t sceKernelMprotect(const void *base, size_t size, int prot)
{
    assert(size == 2 * DIRECT_UNIT_SIZE && prot == PS5_KERNEL_PROT_CPU_READ);
    assert(native[index_of((void *)base)]);
    protects++;
    return 0;
}
#include "wine-direct-reserve.inc"
static void *map_alloc_area(const struct alloc_area *area, void *base)
{
    assert(area->direct);
    return ps5_direct_reserve(base, area->size);
}
static void *search_range(uintptr_t low, uintptr_t high, size_t size, size_t alignment, BOOL top_down)
{
    struct alloc_area area = {.size = size, .step = top_down ? -(ptrdiff_t)alignment : (ptrdiff_t)alignment,
        .direct = TRUE, .align_mask = alignment - 1, .top_down = top_down};
    char *start = ps5_direct_reserve_floor((char *)low, size, TRUE);
    return try_map_free_area_range(&area, start, (char *)high);
}
static void empty(void)
{
    for (size_t i = 0; i < UNITS; ++i) assert(!native[i]);
}
static void reset(void)
{
    memset(native, 0, sizeof(native));
    calls = fail_call = displace_call = fail_unmap = fatal = 0;
    physical_releases = protects = 0;
    direct_chunked_reserve = 1;
    large_error = (int32_t)0x8002000c;
}
static struct ps5_direct_view *view_at(void *base, size_t size)
{
    size_t units = size / DIRECT_UNIT_SIZE, capacity = (units + 63) & ~(size_t)63;
    struct ps5_direct_view *v = calloc(1, sizeof(*v) + capacity * sizeof(int64_t) + 3 * capacity / 8);
    assert(v);
    v->base = base; v->units = units; v->capacity = capacity; v->refs = 1; v->failed_unit = units;
    memset(v->physical, 0xff, capacity * sizeof(int64_t));
    memset(direct_bitmap(v, 0), 0xff, capacity / 8);
    return v;
}
static void release_view(struct ps5_direct_view *v)
{
    for (size_t i = 0; i < v->units; ++i)
        assert(ps5_direct_drop_unit(v, (char *)v->base + i * DIRECT_UNIT_SIZE, FALSE));
    free(v);
}
int main(void)
{
    void *base = (void *)(uintptr_t)ORIGIN;
    const size_t oversized = 64 * GIB - DIRECT_UNIT_SIZE;
    reset(); direct_chunked_reserve = 0;
    assert(ps5_direct_reserve(base, oversized) == MAP_FAILED && errno == ENOMEM && calls == 1); empty();
    reset(); large_error = (int32_t)0x80020016;
    assert(ps5_direct_reserve(base, oversized) == MAP_FAILED && errno == EEXIST && calls == 1); empty();
    reset(); assert(ps5_direct_reserve(base, 4 * GIB) == base && calls == 1);
    assert(!sceKernelMunmap(base, 4 * GIB)); empty();
    reset(); assert(ps5_direct_reserve(base, 8 * GIB) == base && calls == 1);
    assert(!sceKernelMunmap(base, 8 * GIB)); empty();
    assert(!ps5_direct_chunked_reserve_enabled(8 * GIB));
    assert(ps5_direct_chunked_reserve_enabled(8 * GIB + DIRECT_UNIT_SIZE));
    /* Every possible failure position, including the short final chunk. */
    for (int n = 2; n <= 65; ++n)
    {
        reset(); fail_call = n;
        assert(ps5_direct_reserve(base, oversized) == MAP_FAILED && errno == EEXIST); empty();
    }
    reset(); displace_call = 3;
    assert(ps5_direct_reserve(base, oversized) == MAP_FAILED && errno == EEXIST && calls == 3); empty();
    reset(); native[2 * GIB / DIRECT_UNIT_SIZE] = 1;
    assert(ps5_direct_reserve(base, oversized) == MAP_FAILED && errno == EEXIST);
    assert(native[2 * GIB / DIRECT_UNIT_SIZE]); native[2 * GIB / DIRECT_UNIT_SIZE] = 0; empty();
    /* Wine's caller retries the next candidate after an occupied candidate. */
    assert(ps5_direct_reserve(base, oversized) == base);
    struct ps5_direct_view *v = view_at(base, oversized);
    /* Partial decommit/restore and split release cross a native chunk boundary. */
    void *edge = (char *)base + GIB - DIRECT_UNIT_SIZE;
    size_t first = direct_unit_index(v, edge);
    direct_set_bit(v, 1, first, TRUE);
    direct_set_bit(v, 1, first + 1, TRUE);
    v->physical[first] = 100;
    v->physical[first + 1] = 200;
    assert(!ps5_direct_protect(v, edge, 2 * DIRECT_UNIT_SIZE, PROT_READ) && protects == 1);
    assert(ps5_direct_drop_unit(v, edge, TRUE));
    assert(ps5_direct_drop_unit(v, edge, FALSE));
    assert(ps5_direct_drop_unit(v, (char *)edge + DIRECT_UNIT_SIZE, FALSE));
    assert(physical_releases == 2);
    release_view(v); empty();
    /* PartitionAlloc: oversized reserve, whole MEM_RELEASE, exact aligned reserve. */
    reset(); assert(ps5_direct_reserve(base, oversized) == base && calls == 65);
    release_view(view_at(base, oversized)); empty();
    void *aligned = (void *)(uintptr_t)(32 * GIB);
    assert(ps5_direct_reserve(aligned, 32 * GIB) == aligned);
    release_view(view_at(aligned, 32 * GIB)); empty();
    /* Use the console-proven high range, not the occupied low native window. */
    reset();
    assert(ps5_direct_reserve_floor((char *)ORIGIN, oversized, TRUE) == (char *)(64 * GIB));
    assert(ps5_direct_reserve_floor((char *)(96 * GIB), oversized, TRUE) == (char *)(96 * GIB));
    assert(ps5_direct_reserve_floor((char *)ORIGIN, 8 * GIB, TRUE) == (char *)ORIGIN);
    assert(ps5_direct_reserve_floor((char *)ORIGIN, oversized, FALSE) == (char *)ORIGIN);
    direct_chunked_reserve = 0;
    assert(ps5_direct_reserve_floor((char *)ORIGIN, oversized, TRUE) == (char *)ORIGIN);
    direct_chunked_reserve = 1;
    assert(search_range(ORIGIN, 128 * GIB, oversized, DIRECT_UNIT_SIZE, FALSE) == (void *)(64 * GIB));
    release_view(view_at((void *)(64 * GIB), oversized)); empty();
    /* The exact-address aligned re-reserve does not move the caller's base. */
    assert(ps5_direct_reserve((void *)(64 * GIB), 32 * GIB) == (void *)(64 * GIB));
    release_view(view_at((void *)(64 * GIB), 32 * GIB)); empty();
    reset(); displace_call = 2; /* First chunk mismatch: only two calls at this candidate. */
    assert(search_range(ORIGIN, 128 * GIB, 32 * GIB, 32 * GIB, FALSE) == (void *)(96 * GIB));
    assert(calls == 35); /* 2 at 64 GiB, then the single failure + 32 chunks at 96 GiB. */
    release_view(view_at((void *)(96 * GIB), 32 * GIB)); empty();
    reset();
    assert(search_range(ORIGIN, 128 * GIB, 32 * GIB, 32 * GIB, TRUE) == (void *)(96 * GIB));
    release_view(view_at((void *)(96 * GIB), 32 * GIB)); empty();
    reset();
    assert(!search_range(ORIGIN, 96 * GIB, oversized, DIRECT_UNIT_SIZE, FALSE) && !calls);
    assert(!search_range(ORIGIN, 32 * GIB, oversized, DIRECT_UNIT_SIZE, TRUE) && !calls);
    /* A cleanup error must never be reported as a clean allocation miss. */
    reset(); fail_call = 3; fail_unmap = 1;
    if (!setjmp(failure)) { ps5_direct_reserve(base, oversized); assert(!"must fail fast"); }
    assert(fatal);
    puts("PASS direct reserve: default gate, threshold, errno, all rollback positions, exact placement, ownership, partial/whole release, PartitionAlloc trim, fatal rollback");
    return 0;
}
