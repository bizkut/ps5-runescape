#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ps5platform/kernel.h>
static unsigned live, singles, aligned_singles, chunks, checks, releases;
static uintptr_t next_chunk;
static size_t current_chunk;
static void io_log(const char *format, ...) { (void)format; }
int64_t sceKernelGetDirectMemorySize(void) { return INT64_C(12) << 30; }
int32_t sceKernelAvailableDirectMemorySize(int64_t a, int64_t b, size_t c, int64_t *p, size_t *s)
{ (void)c; *p = a; *s = b - a; return 0; }
int32_t sceKernelAvailableFlexibleMemorySize(size_t *s) { *s = (size_t)256 << 20; return 0; }
int32_t sceKernelReserveVirtualRange(void **base, size_t size, int flags, size_t alignment)
{
    assert(!flags);
    if (size == PS5_KERNEL_DIRECT_ALIGNMENT)
    {
        assert(!live); checks++;
        if (!*base) *base = (void *)(uintptr_t)(UINT64_C(64) << 30);
        live++; return 0;
    }
    if (!*base)
    {
        assert(!live);
        singles++;
        if (alignment == (UINT64_C(32) << 30)) aligned_singles++;
        else assert(alignment == PS5_KERNEL_DIRECT_ALIGNMENT);
        if (size > (UINT64_C(16) << 30)) return (int32_t)0x8002000c;
        *base = (void *)(uintptr_t)(UINT64_C(64) << 30); live++; return 0;
    }
    chunks++;
    if (!live) { next_chunk = (uintptr_t)*base; current_chunk = size; }
    assert(size == current_chunk && (uintptr_t)*base == next_chunk);
    if (live == 5) { *base = (void *)(uintptr_t)(UINT64_C(400) << 30); live++; return 0; }
    live++; next_chunk += size; return 0;
}
int32_t sceKernelMunmap(void *base, size_t size)
{ (void)base; (void)size; assert(live); live--; releases++; return 0; }
#include "runescape-vaprobe.h"
int main(void)
{
    assert(!rs_va_probe());
    assert(!live && singles == 12 && aligned_singles == 6 && chunks == 12 && checks == 3);
    unsigned previous = releases;
    assert(!rs_va_probe() && releases == previous);
    puts("PASS vaprobe: size/alignment matrix, adjacent 1/4-GiB probes, unexpected-placement cleanup, final small checks, one-shot");
    return 0;
}
