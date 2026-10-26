#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define __PROSPERO__ 1
#define BOOL int
#define UINT unsigned int
#define UINT_PTR uintptr_t
#define NTSTATUS int
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_NO_MEMORY 1
#define STATUS_INVALID_PARAMETER 2
#define STATUS_INVALID_PARAMETER_3 3
#define STATUS_CONFLICTING_ADDRESSES 4
#define VPROT_PLACEHOLDER 0x400
#define VPROT_FREE_PLACEHOLDER 0x800
#define VPROT_PS5_DIRECT 0x2000
#define VPROT_PS5_PLACEHOLDER 0x4000
#define VPROT_WRITEWATCH 0x40
#define VPROT_COMMITTED 0x20
#define MEM_REPLACE_PLACEHOLDER 0x4000
#define PS5_KERNEL_MAP_FIXED 0x10
#define PS5_KERNEL_PAGE_SIZE 0x4000
#define PAGE PS5_KERNEL_PAGE_SIZE
#define SPAN ((size_t)512 << 20)
#define ORIGIN ((uintptr_t)64 << 30)
#define ROUND_SIZE(base,size,mask) (((uintptr_t)(base) + (size) + (mask)) & ~(uintptr_t)(mask))
#define TRACE(...) do {} while (0)
#define ERR(...) do {} while (0)
#define VIRTUAL_DEBUG_DUMP_VIEW(v) do {} while (0)
static const size_t host_page_mask = PAGE - 1;
#define max(a,b) ((a) > (b) ? (a) : (b))
static int direct_memory_enabled = 1, direct_chunked_reserve = 1;
static unsigned char pages[SPAN / PAGE]; /* 0=free, 1=native reservation, 2=flexible */
static int reserve_calls, mmap_calls, fail_mmap, fail_reserve, displace;
static size_t flexible;
struct rb_entry { int unused; };
struct file_view { struct rb_entry entry; void *base; size_t size; unsigned protect, map_profile; void *direct; int live; };
static struct file_view views[12];
static size_t index_of(void *base) { assert((uintptr_t)base >= ORIGIN); return ((uintptr_t)base - ORIGIN) / PAGE; }
static int32_t sceKernelReserveVirtualRange(void **base, size_t size, int flags, size_t alignment)
{
    size_t first = index_of(*base), count = size / PAGE;
    assert(alignment == PAGE && !(size % PAGE) && first + count <= sizeof(pages));
    reserve_calls++;
    if (fail_reserve) return (int32_t)0x8002000c;
    if (displace) { *base = (char *)*base + SPAN / 2; first = index_of(*base); }
    assert(first + count <= sizeof(pages));
    for (size_t i = first; i < first + count; i++)
    {
        if (!flags) assert(!pages[i]);
        if (pages[i] == 2) flexible -= PAGE;
        pages[i] = 1;
    }
    return 0;
}
static int32_t sceKernelMunmap(void *base, size_t size)
{
    size_t first = index_of(base);
    for (size_t i = first; i < first + size / PAGE; i++)
    { if (pages[i] == 2) flexible -= PAGE; pages[i] = 0; }
    return 0;
}
static int32_t sceKernelAvailableFlexibleMemorySize(size_t *free_bytes)
{ *free_bytes = ((size_t)1 << 30) - flexible; return 0; }
static void ps5_direct_fail_fast(void) { assert(!"native reset must not fail"); }
static void direct_log(const char *format, ...) { (void)format; }
enum { PS5_FLEX_REPLACE, PS5_FLEX_FILEMAP, PS5_FLEX_ANON, PS5_FLEX_KINDS };
static unsigned flex_notes;
static void ps5_direct_flex_note(unsigned kind, void *base, size_t size)
{ (void)base; assert(kind == PS5_FLEX_REPLACE && size); flex_notes++; }
static int get_unix_prot(unsigned vprot) { return vprot & VPROT_COMMITTED ? PROT_READ | PROT_WRITE : PROT_NONE; }
static void *anon_mmap_fixed(void *base, size_t size, int prot, int flags)
{
    (void)prot; (void)flags; mmap_calls++;
    if (fail_mmap) return MAP_FAILED;
    size_t first = index_of(base);
    for (size_t i = first; i < first + size / PAGE; i++)
    { if (pages[i] != 2) flexible += PAGE; pages[i] = 2; }
    return base;
}
static struct file_view *alloc_view(void)
{ for (unsigned i = 0; i < 12; i++) if (!views[i].live) { views[i].live = 1; return &views[i]; } return NULL; }
static void free_view(struct file_view *view) { view->live = 0; }
static void unregister_view(struct file_view *view) { (void)view; }
static void register_view(struct file_view *view) { (void)view; }
static NTSTATUS create_view(struct file_view **ret, void *base, size_t size, unsigned protect)
{ struct file_view *v = alloc_view(); if (!v) return STATUS_NO_MEMORY; v->base = base; v->size = size; v->protect = protect; *ret = v; return 0; }
static struct file_view *find_view(void *base, size_t size)
{ (void)size; for (unsigned i = 0; i < 12; i++) if (views[i].live && views[i].base == base) return &views[i]; return NULL; }
static struct rb_entry *rb_next(struct rb_entry *entry)
{
    struct file_view *v = (struct file_view *)entry, *next = NULL;
    for (unsigned i = 0; i < 12; i++) if (views[i].live && views[i].base > v->base && (!next || views[i].base < next->base)) next = &views[i];
    return next ? &next->entry : NULL;
}
#define RB_ENTRY_VALUE(entry,type,member) ((type *)(entry))
static NTSTATUS ps5_free_pages(struct file_view *view, void *base, size_t size)
{ (void)view; (void)base; (void)size; assert(!"no direct metadata on placeholders"); return 1; }
static void ps5_direct_retain_view(void *view) { (void)view; assert(0); }
static int title_profile_is_enabled(void) { return 0; }
static void title_map_removed(unsigned slot, size_t size) { (void)slot; (void)size; }
static void title_map_unmapped(struct file_view *v) { (void)v; }
static void set_page_vprot(void *base, size_t size, unsigned vprot) { (void)base; (void)size; (void)vprot; }
static void set_vprot(struct file_view *view, void *base, size_t size, unsigned vprot) { (void)view; set_page_vprot(base,size,vprot); }
static void kernel_writewatch_register_range(struct file_view *view, void *base, size_t size) { (void)view; (void)base; (void)size; }
static void reset_write_watches(void *base, size_t size) { (void)base; (void)size; }
#include "wine-placeholder.inc"
int main(void)
{
    void *base = (void *)ORIGIN;
    struct file_view *whole, *subset;
    assert(check_span((void *)0x10000, (void *)((uintptr_t)512 << 30),
                      (size_t)0x15800000000) == STATUS_NO_MEMORY);
    assert(check_span((void *)0x10000, (void *)((uintptr_t)512 << 30),
                      (size_t)512 << 30) == STATUS_NO_MEMORY);
    assert(check_span((void *)0x10000, (void *)((uintptr_t)512 << 30),
                      (size_t)256 << 30) == STATUS_SUCCESS);
    assert(!reserve_calls && !mmap_calls);
    assert(placeholder_floor((char *)0x10000, SPAN, TRUE) == (char *)ORIGIN);
    assert(placeholder_floor((char *)0x10000, SPAN - PAGE, TRUE) == (char *)0x10000);
    assert(placeholder_floor((char *)0x10000, SPAN, FALSE) == (char *)0x10000);
    assert(placeholder_floor((char *)(ORIGIN + SPAN), SPAN, TRUE) == (char *)(ORIGIN + SPAN));
    direct_chunked_reserve = 0; assert(!ps5_direct_reserve_placeholders());
    direct_chunked_reserve = 1; direct_memory_enabled = 0; assert(!ps5_direct_reserve_placeholders());
    direct_memory_enabled = 1; assert(ps5_direct_reserve_placeholders());
    fail_reserve = 1; assert(ps5_direct_placeholder_reserve(base, SPAN, FALSE) == MAP_FAILED && errno == ENOMEM);
    fail_reserve = 0;
    displace = 1; assert(ps5_direct_placeholder_reserve(base, PAGE, FALSE) == MAP_FAILED && errno == EEXIST);
    assert(!pages[SPAN / 2 / PAGE]); displace = 0;
    assert(ps5_direct_placeholder_reserve(base, SPAN, FALSE) == base);
    assert(!flexible && !mmap_calls);
    assert(!create_view(&whole, base, SPAN, VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER | VPROT_PS5_PLACEHOLDER));
    assert(free_pages_preserve_placeholder(whole, base, SPAN) == STATUS_CONFLICTING_ADDRESSES);
    /* Real V8 split/replace/preserve/merge sequence, at a legal host-page
     * boundary which is deliberately NOT direct-memory-unit aligned. */
    assert(!free_pages_preserve_placeholder(whole, (char *)base + PAGE, 2 * PAGE));
    subset = find_view((char *)base + PAGE, 0); assert(subset && whole->size == PAGE);
    assert(!flexible && !mmap_calls);
    fail_mmap = 1;
    assert(replace_placeholder(&subset, subset->base, subset->size, VPROT_COMMITTED) == STATUS_NO_MEMORY);
    assert(subset->protect & VPROT_FREE_PLACEHOLDER); assert(!flexible);
    fail_mmap = 0; assert(!flex_notes);
    assert(!replace_placeholder(&subset, subset->base, subset->size, VPROT_COMMITTED));
    assert(flex_notes == 1);
    assert(flexible == 2 * PAGE && !(subset->protect & VPROT_PS5_PLACEHOLDER));
    assert(pages[0] == 1 && pages[1] == 2 && pages[2] == 2 && pages[3] == 1);
    assert(!free_pages_preserve_placeholder(subset, subset->base, subset->size));
    assert(!flexible && (subset->protect & VPROT_PS5_PLACEHOLDER));
    assert(!coalesce_placeholders(whole, base, SPAN));
    assert(whole->size == SPAN && whole->protect & VPROT_PS5_PLACEHOLDER);
    assert(!sceKernelMunmap(base, SPAN)); free_view(whole);
    for (size_t i = 0; i < sizeof(pages); i++) assert(!pages[i]);
    assert(reserve_calls == 5 && mmap_calls == 2);
    puts("PASS native placeholder: gate, exact reserve, ENOMEM, displacement rollback, 512MiB no flex, 16KiB split, replacement failure, subset backing, preserve, coalesce, free");
    return 0;
}
