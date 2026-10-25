/* Reserve-only title experiment. No physical memory is committed. */
#ifndef RUNESCAPE_VAPROBE_H
#define RUNESCAPE_VAPROBE_H
#include <ps5platform/kernel.h>

/* The native query returns the largest hole, not the sum of free ranges. */
static uint64_t rs_va_direct_free(int64_t from, int64_t to)
{
    uint64_t total = 0;
    while (from < to)
    {
        int64_t start = -1;
        size_t bytes = 0;
        if (sceKernelAvailableDirectMemorySize(from, to, PS5_KERNEL_DIRECT_ALIGNMENT,
                                               &start, &bytes) || !bytes) break;
        if (start < from || start >= to || bytes > (uint64_t)(to - start)) break;
        int64_t end = start + bytes;
        total += bytes;
        if (start - from < to - end)
        {
            total += rs_va_direct_free(from, start);
            from = end;
        }
        else
        {
            total += rs_va_direct_free(end, to);
            to = start;
        }
    }
    return total;
}

static void rs_va_pools(const char *stage)
{
    size_t flexible = 0, largest = 0;
    int64_t physical = 0;
    int32_t dr = sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(),
                         PS5_KERNEL_DIRECT_ALIGNMENT, &physical, &largest);
    int32_t fr = sceKernelAvailableFlexibleMemorySize(&flexible);
    uint64_t direct_free = rs_va_direct_free(0, sceKernelGetDirectMemorySize());
    io_log("runescape: vaprobe pools stage=%s direct_status=0x%08x direct_largest=%zu direct_free=%llu "
           "flexible_status=0x%08x flexible=%zu", stage, (unsigned)dr, largest,
           (unsigned long long)direct_free, (unsigned)fr, flexible);
}

static int rs_va_release(void *base, size_t size)
{
    int32_t status = sceKernelMunmap(base, size);
    io_log("runescape: vaprobe release base=%p size=%zu status=0x%08x", base, size, (unsigned)status);
    return status != 0;
}

static int rs_va_small_check(void *hint)
{
    void *base = hint;
    int32_t status = sceKernelReserveVirtualRange(&base, PS5_KERNEL_DIRECT_ALIGNMENT, 0,
                                                 PS5_KERNEL_DIRECT_ALIGNMENT);
    io_log("runescape: vaprobe small-check hint=%p base=%p size=%u status=0x%08x exact=%d",
           hint, base, (unsigned)PS5_KERNEL_DIRECT_ALIGNMENT, (unsigned)status, base == hint);
    if (status) return 1;
    int failed = rs_va_release(base, PS5_KERNEL_DIRECT_ALIGNMENT);
    return failed || (hint && base != hint);
}

static int rs_va_probe(void)
{
    static int ran;
    static const unsigned sizes[] = {4, 8, 16, 32, 48, 64};
    const uint64_t gib = UINT64_C(1) << 30;
    const uintptr_t start = UINT64_C(0x1000000000), limit = UINT64_C(0x8000000000);
    int failed = 0;
    if (ran) return 0;
    ran = 1;
    io_log("runescape: vaprobe begin usable_limit=%p chunks_base=%p", (void *)limit, (void *)start);
    rs_va_pools("begin");
    for (unsigned aligned = 0; aligned < 2; ++aligned)
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        {
            size_t size = sizes[i] * gib;
            size_t alignment = aligned ? 32 * gib : PS5_KERNEL_DIRECT_ALIGNMENT;
            void *base = NULL;
            rs_va_pools("single-before");
            int32_t status = sceKernelReserveVirtualRange(&base, size, 0, alignment);
            io_log("runescape: vaprobe single hint=0 size=%zu alignment=%zu base=%p status=0x%08x",
                   size, alignment, base, (unsigned)status);
            rs_va_pools("single-held");
            if (!status && rs_va_release(base, size)) return 1;
            rs_va_pools("single-after");
        }
    for (unsigned chunk_gib = 1; chunk_gib <= 4; chunk_gib *= 4)
    {
        size_t chunk = chunk_gib * gib, owned = 0;
        rs_va_pools("chunks-before");
        while (owned < limit - start)
        {
            void *hint = (void *)(start + owned), *base = hint;
            int32_t status = sceKernelReserveVirtualRange(&base, chunk, 0, PS5_KERNEL_DIRECT_ALIGNMENT);
            io_log("runescape: vaprobe chunk hint=%p size=%zu base=%p status=0x%08x owned=%zu",
                   hint, chunk, base, (unsigned)status, owned);
            if (status) break;
            if (base != hint)
            {
                if (rs_va_release(base, chunk)) failed = 1;
                break;
            }
            owned += chunk;
        }
        io_log("runescape: vaprobe adjacent base=%p chunk=%zu total=%zu limit_reached=%d",
               (void *)start, chunk, owned, owned == limit - start);
        rs_va_pools("chunks-held");
        while (owned)
        {
            owned -= chunk;
            if (rs_va_release((void *)(start + owned), chunk)) failed = 1;
        }
        rs_va_pools("chunks-after");
        if (rs_va_small_check((void *)start)) failed = 1;
        if (failed) return 1; /* Never enter Wine with unaccounted ownership. */
    }
    failed = rs_va_small_check(NULL);
    rs_va_pools("end");
    io_log("runescape: vaprobe end cleanup_ok=%d", !failed);
    return failed;
}
#endif
