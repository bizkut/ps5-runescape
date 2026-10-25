#!/usr/bin/env python3
"""Contracts for the RuneScape title window compositor.

Always: the planner/SPIR-V-layout contract (test-title-compositor.c) and the
win32u glue contract, which runs the actual compositor-mode functions
extracted from Wine's vulkan.c/driver.c against recording fakes. With
--vulkan (host only, needs libvulkan and a CPU driver such as lavapipe):
real composition readback with two producer VkDevices.
"""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
import os
from pathlib import Path
import shlex
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--vulkan', action='store_true', help='run the real host Vulkan readback contract')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('compositor_functions', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
headers = here.parents[2] / 'vkd3d-proton/khronos/Vulkan-Headers/include'
core = args.wine / 'dlls/win32u/ps5_compositor.c'
common = ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-DPS5C_KHRONOS_HEADERS',
          '-I' + str(headers), '-idirafter', str(args.wine / 'include'), '-idirafter', str(args.wine / 'dlls/win32u')]
args.out.mkdir(parents=True, exist_ok=True)

# 1. Planner and shader layout.
binary = args.out / 'test-title-compositor'
subprocess.run(shlex.split(args.cc) + common + [str(here / 'test-title-compositor.c'), str(core), '-o', str(binary)],
               check=True)
spirv = args.out / 'title-compositor.spv'
subprocess.run([str(binary), str(spirv)], check=True)
subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(spirv)], check=True)
print('title compositor SPIR-V: spirv-val --target-env vulkan1.0 PASS')

# 2. The win32u glue, extracted unchanged.
vulkan_c = args.wine / 'dlls/win32u/vulkan.c'
driver_c = args.wine / 'dlls/win32u/driver.c'
glue = '\n'.join(extract.definition(vulkan_c, name) for name in (
    'extents_equals', 'ps5_compositor_surface_formats', 'ps5_compositor_window_state', 'ps5_compositor_acquire',
    'ps5_compositor_signal_acquired', 'ps5_compositor_queue_present'))
glue = '\n'.join(extract.definition(core, name) for name in ('ps5c_rect_empty', 'ps5c_rect_intersect')) + '\n' + glue
text = vulkan_c.read_text()
formats = text[text.index('static const VkSurfaceFormatKHR ps5_compositor_formats[]'):]
formats = formats[:formats.index('};') + 2]
driver_text = driver_c.read_text()
viewport = driver_text[driver_text.index('static pthread_mutex_t ps5_viewport_mutex'):
                       driver_text.index('void ps5_set_present_viewport')]
viewport += '\n'.join(extract.definition(driver_c, name) for name in (
    'ps5_set_present_desktop_viewport', 'ps5_get_present_viewport', 'ps5_viewport_input_point'))

gdi_harness = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *HBITMAP;
typedef struct { uint8_t rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { uint32_t biSize; int32_t biWidth, biHeight; uint16_t biPlanes, biBitCount; uint32_t biCompression,
                 biSizeImage; int32_t biXPelsPerMeter, biYPelsPerMeter; uint32_t biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
#define FIELD_OFFSET(type, field) ((long)(intptr_t)&(((type *)0)->field))
#define BI_RGB 0
#define TRACE(...) ((void)0)
#define wine_dbgstr_rect(r) "rect"
static char gdi_log[16384];
static void gdi_message(const char *format, ...)
{
    size_t used = strlen(gdi_log);
    va_list args;
    va_start(args, format);
    vsnprintf(gdi_log + used, sizeof(gdi_log) - used, format, args);
    va_end(args);
}
#define MESSAGE(...) gdi_message(__VA_ARGS__)
static LONG InterlockedExchange(LONG *target, LONG value) { LONG old = *target; *target = value; return old; }
static LONG InterlockedExchangeAdd(LONG *target, LONG value) { LONG old = *target; *target += value; return old; }
static BOOL EqualRect(const RECT *a, const RECT *b)
{ return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }

struct window_surface;
struct window_surface_funcs
{
    void (*set_clip)(struct window_surface *, const RECT *, UINT);
    BOOL (*flush)(struct window_surface *, const RECT *, const RECT *, const BITMAPINFO *, const void *, BOOL,
                  const BITMAPINFO *, const void *);
    void (*destroy)(struct window_surface *);
};
struct window_surface { const struct window_surface_funcs *funcs; HWND hwnd; RECT rect; BITMAPINFO info; int ref; };
static UINT get_dib_image_size(const BITMAPINFO *info)
{ return info->bmiHeader.biWidth * (info->bmiHeader.biHeight < 0 ? -info->bmiHeader.biHeight : info->bmiHeader.biHeight) * 4; }
static struct window_surface *window_surface_create(UINT size, const struct window_surface_funcs *funcs, HWND hwnd,
                                                    const RECT *rect, BITMAPINFO *info, HBITMAP bitmap)
{
    struct window_surface *surface = calloc(1, size);
    assert(size == sizeof(*surface) && !bitmap);
    surface->funcs = funcs; surface->hwnd = hwnd; surface->rect = *rect; surface->info = *info; surface->ref = 1;
    return surface;
}
static void window_surface_add_ref(struct window_surface *surface) { surface->ref++; }
static void window_surface_release(struct window_surface *surface)
{
    if (--surface->ref) return;
    surface->funcs->destroy(surface);
    free(surface);
}

static unsigned chains_created, chains_destroyed;
static uint32_t gdi_bits[1152 * 640];
static struct ps5c_compositor *ps5_compositor;
static BOOL ps5_compositor_mode;
VkResult ps5c_chain_create_cpu(struct ps5c_compositor *compositor, uint64_t key, uint32_t width, uint32_t height,
                               struct ps5c_chain **out)
{
    struct ps5c_chain *chain = calloc(1, sizeof(*chain));
    assert(compositor == ps5_compositor && key == ((uint64_t)0x300aa | (1ull << 63)));
    chain->width = width; chain->height = height;
    chains_created++;
    *out = chain;
    return VK_SUCCESS;
}
VkResult ps5c_chain_upload(struct ps5c_chain *chain, const void *bits, uint32_t stride)
{
    assert(bits == gdi_bits && stride == chain->width * 4);
    chain->uploads++;
    return VK_SUCCESS;
}
void ps5c_chain_destroy(struct ps5c_chain *chain) { chains_destroyed++; free(chain); }
VkExtent2D ps5c_chain_size(struct ps5c_chain *chain) { return (VkExtent2D){chain->width, chain->height}; }
'''
vulkan_text = vulkan_c.read_text()
dce_c = args.wine / 'dlls/win32u/dce.c'
dce_text = dce_c.read_text()
gdi_glue = vulkan_text[vulkan_text.index('#define PS5_GDI_KEY'):vulkan_text.index('#define PS5_GDI_LOG_LIMIT 24') +
                       len('#define PS5_GDI_LOG_LIMIT 24')] + '\n'
gdi_glue += '\n'.join(extract.definition(vulkan_c, name) for name in (
    'ps5_compositor_surface_flush', 'ps5_compositor_surface_destroy')) + '\n'
gdi_glue += extract.definition(vulkan_c, 'surface_sets_pixel_format') + '\n'
offscreen_table = dce_text[dce_text.index('static const struct window_surface_funcs offscreen_window_surface_funcs'):]
offscreen_table = offscreen_table[:offscreen_table.index('};') + 2]
gdi_glue += '\n'.join(extract.definition(dce_c, name) for name in (
    'offscreen_window_surface_set_clip', 'offscreen_window_surface_flush', 'offscreen_window_surface_destroy'))
gdi_glue += '\n' + offscreen_table + '\n' + extract.definition(dce_c, 'create_offscreen_window_surface') + '\n'

harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "wine/ps5_compositor.h"
typedef int BOOL;
typedef unsigned UINT;
typedef int32_t LONG;
typedef void *HWND;
typedef struct { int32_t x, y; } POINT;
typedef struct { int32_t left, top, right, bottom; } RECT;
#define TRUE 1
#define FALSE 0
#define GW_HWNDNEXT 2
#define COORDS_SCREEN 3
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define UINT_PTR uintptr_t

/* --- Win32 window tree fake: desktop 1, top-levels 10/20, child 11 of 10. --- */
struct fake_window { HWND hwnd, parent, next; RECT client; BOOL visible, iconic; };
static struct fake_window windows[] = {
    {(HWND)10, (HWND)1, (HWND)20, {100, 100, 900, 600}, TRUE, FALSE},
    {(HWND)11, (HWND)10, (HWND)12, {850, 500, 1000, 700}, TRUE, FALSE},
    {(HWND)12, (HWND)10, NULL, {0, 0, 10, 10}, TRUE, FALSE},
    {(HWND)20, (HWND)1, (HWND)30, {0, 0, 50, 50}, TRUE, FALSE},
    {(HWND)30, (HWND)1, NULL, {0, 0, 1, 1}, FALSE, FALSE},
};
static struct fake_window *find_window(HWND hwnd)
{
    for (unsigned i = 0; i < ARRAY_SIZE(windows); i++) if (windows[i].hwnd == hwnd) return &windows[i];
    return NULL;
}
HWND get_desktop_window(void) { return (HWND)1; }
HWND get_parent(HWND hwnd) { struct fake_window *w = find_window(hwnd); return w ? w->parent : NULL; }
HWND get_window_relative(HWND hwnd, UINT rel) { assert(rel == GW_HWNDNEXT); return find_window(hwnd)->next; }
BOOL is_iconic(HWND hwnd) { return find_window(hwnd)->iconic; }
BOOL is_window_visible(HWND hwnd)
{
    for (struct fake_window *w = find_window(hwnd); w; w = find_window(w->parent)) if (!w->visible) return FALSE;
    return TRUE;
}
BOOL get_client_rect_rel(HWND hwnd, int rel, RECT *rect, UINT dpi)
{
    struct fake_window *w = find_window(hwnd);
    assert(rel == COORDS_SCREEN && !dpi);
    if (!w) return FALSE;
    *rect = w->client;
    return TRUE;
}

/* --- Vulkan object fakes ---------------------------------------------------- */
struct semaphore { struct { struct { VkSemaphore semaphore; } host; } obj; BOOL ps5_acquired; };
static struct semaphore *semaphore_from_handle(VkSemaphore handle) { return (struct semaphore *)(uintptr_t)handle; }
struct vulkan_fence { struct { VkFence fence; } host; };
static struct vulkan_fence *vulkan_fence_from_handle(VkFence handle) { return (struct vulkan_fence *)(uintptr_t)handle; }
struct vulkan_device;
struct vulkan_queue { struct vulkan_device *device; struct { VkQueue queue; } host; struct { uint32_t queueFamilyIndex; } info; };
struct vulkan_device { PFN_vkQueueSubmit p_vkQueueSubmit; struct vulkan_queue queues[1]; };
struct surface { HWND hwnd; };
struct swapchain { struct ps5c_chain *ps5_chain; struct surface *surface; VkExtent2D extents; };
static struct swapchain *swapchain_from_handle(VkSwapchainKHR handle) { return (struct swapchain *)(uintptr_t)handle; }

static unsigned submits, submit_signals;
static VkSemaphore signalled[8];
static VkFence submitted_fence;
static VkQueue submitted_queue;
static VKAPI_ATTR VkResult VKAPI_CALL fake_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence)
{
    submits++;
    submitted_queue = queue;
    submitted_fence = fence;
    for (uint32_t i = 0; i < count; i++)
    {
        assert(!info[i].waitSemaphoreCount && !info[i].commandBufferCount);
        for (uint32_t j = 0; j < info[i].signalSemaphoreCount; j++) signalled[submit_signals++] = info[i].pSignalSemaphores[j];
    }
    return VK_SUCCESS;
}

/* --- Core fakes recording what the glue hands the compositor ---------------- */
struct ps5c_chain { VkResult acquire_result; uint32_t next, released; unsigned presents;
                    VkQueue queue; uint32_t family, index, wait_count; VkSemaphore waits[16]; VkResult present_result;
                    uint32_t width, height; unsigned uploads; };
VkResult ps5c_chain_acquire(struct ps5c_chain *chain, uint64_t timeout, uint32_t *index)
{
    (void)timeout;
    if (chain->acquire_result) return chain->acquire_result;
    *index = chain->next;
    return VK_SUCCESS;
}
void ps5c_chain_release(struct ps5c_chain *chain, uint32_t index) { chain->released = index + 1; }
VkResult ps5c_chain_present(struct ps5c_chain *chain, VkQueue queue, uint32_t family, uint32_t index,
                            uint32_t wait_count, const VkSemaphore *waits)
{
    chain->presents++;
    chain->queue = queue;
    chain->family = family;
    chain->index = index;
    chain->wait_count = wait_count;
    memcpy(chain->waits, waits, wait_count * sizeof(*waits));
    return chain->present_result;
}
static RECT client_rect = {0, 0, 765, 503};
static BOOL window_exists = TRUE;
static unsigned checked_windows;
UINT NtUserGetDpiForWindow(HWND hwnd) { (void)hwnd; return 96; }
static BOOL get_surface_rect(HWND hwnd, RECT *rect, UINT dpi)
{
    (void)hwnd; assert(dpi == 96);
    if (!window_exists) return FALSE;
    *rect = client_rect;
    return TRUE;
}
static void ps5_compositor_check_window(HWND hwnd) { (void)hwnd; checked_windows++; }

/* --- driver.c viewport fakes ------------------------------------------------- */
UINT get_thread_dpi(void) { return 96; }
BOOL NtUserIsWindow(HWND hwnd) { (void)hwnd; return FALSE; }
BOOL NtUserGetClientRect(HWND hwnd, RECT *rect, UINT dpi) { (void)hwnd; (void)rect; (void)dpi; return FALSE; }
int NtUserMapWindowPoints(HWND from, HWND to, POINT *points, UINT count, UINT dpi)
{ (void)from; (void)to; (void)points; (void)count; (void)dpi; return 0; }
static RECT map_rect_virt_to_raw(RECT rect, UINT dpi) { assert(dpi == 96); return rect; }
'''

cases = r'''
static struct semaphore sem(uintptr_t host) { struct semaphore s = {{{(VkSemaphore)host}}, FALSE}; return s; }
#define H(p) ((VkSemaphore)(uintptr_t)(p))

static void test_surface_formats(void)
{
    VkSurfaceFormatKHR formats[4];
    VkSurfaceFormat2KHR formats2[4] = {{0}};
    uint32_t count = 0;
    assert(!ps5_compositor_surface_formats(&count, NULL, NULL) && count == 2);
    count = 1;
    assert(ps5_compositor_surface_formats(&count, formats, NULL) == VK_INCOMPLETE && count == 1);
    assert(formats[0].format == VK_FORMAT_B8G8R8A8_UNORM && formats[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    count = 4;
    assert(!ps5_compositor_surface_formats(&count, NULL, formats2) && count == 2);
    assert(formats2[1].surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB);
}

static void test_window_state(void)
{
    struct ps5c_window_state s;
    ps5_compositor_window_state((HWND)10, &s);
    assert(s.key == 10 && s.top_level && s.visible && s.client.left == 100 && s.clip.right == 900);
    assert(s.z_depth == 1 && s.z[0] == 2); /* two top-level windows below it */
    ps5_compositor_window_state((HWND)11, &s);
    assert(!s.top_level && s.visible && s.client.right == 1000);
    assert(s.clip.left == 850 && s.clip.top == 500 && s.clip.right == 900 && s.clip.bottom == 600);
    assert(s.z_depth == 2 && s.z[0] == 2 && s.z[1] == 1); /* above its sibling 12, under parent 10 */
    windows[0].iconic = TRUE;   /* a minimized root hides its children */
    ps5_compositor_window_state((HWND)11, &s);
    assert(!s.visible);
    windows[0].iconic = FALSE;
    windows[0].visible = FALSE; /* hidden parent */
    ps5_compositor_window_state((HWND)11, &s);
    assert(!s.visible);
    windows[0].visible = TRUE;
    ps5_compositor_window_state((HWND)30, &s);
    assert(!s.visible && s.z[0] == 0);
}

static void test_acquire(void)
{
    struct ps5c_chain chain = {.next = 2};
    struct surface surface = {(HWND)10};
    struct swapchain swapchain = {&chain, &surface, {765, 503}};
    struct vulkan_device device = {fake_submit};
    struct semaphore acquire = sem(0x51);
    struct vulkan_fence fence = {{(VkFence)(uintptr_t)0xf1}};
    uint32_t index = 99;

    device.queues[0].host.queue = (VkQueue)(uintptr_t)0xa0;
    submits = 0;
    assert(!ps5_compositor_acquire(&device, &swapchain, UINT64_MAX, (VkSemaphore)(uintptr_t)&acquire,
                                   VK_NULL_HANDLE, &index));
    assert(index == 2 && acquire.ps5_acquired && !submits); /* no queue touched for a semaphore */
    acquire.ps5_acquired = FALSE;
    assert(!ps5_compositor_acquire(&device, &swapchain, 0, VK_NULL_HANDLE, (VkFence)(uintptr_t)&fence, &index));
    assert(submits == 1 && submitted_fence == fence.host.fence && submitted_queue == device.queues[0].host.queue);
    client_rect.right = 800;      /* resized window: image still handed out, but suboptimal */
    assert(ps5_compositor_acquire(&device, &swapchain, 0, (VkSemaphore)(uintptr_t)&acquire, VK_NULL_HANDLE,
                                  &index) == VK_SUBOPTIMAL_KHR && acquire.ps5_acquired);
    client_rect.right = 765;
    acquire.ps5_acquired = FALSE;
    chain.acquire_result = VK_TIMEOUT;
    assert(ps5_compositor_acquire(&device, &swapchain, 5, (VkSemaphore)(uintptr_t)&acquire, VK_NULL_HANDLE,
                                  &index) == VK_TIMEOUT && !acquire.ps5_acquired);
}

static void test_signal_acquired(void)
{
    struct vulkan_device device = {fake_submit};
    struct vulkan_queue queue = {&device, {(VkQueue)(uintptr_t)0xb0}, {0}};
    struct semaphore a = sem(0x61), b = sem(0x62);
    VkSemaphore waits[2] = {(VkSemaphore)(uintptr_t)&a, (VkSemaphore)(uintptr_t)&b};

    submits = submit_signals = 0;
    assert(!ps5_compositor_signal_acquired(&queue, waits, 2) && !submits);
    b.ps5_acquired = TRUE;
    assert(!ps5_compositor_signal_acquired(&queue, waits, 2));
    assert(submits == 1 && submit_signals == 1 && signalled[0] == H(0x62) && !b.ps5_acquired);
    assert(submitted_queue == queue.host.queue && submitted_fence == VK_NULL_HANDLE);
    assert(!ps5_compositor_signal_acquired(&queue, waits, 2) && submits == 1); /* signalled once only */
}

static void test_present(void)
{
    struct ps5c_chain first = {0}, second = {0};
    struct surface surface = {(HWND)10};
    struct swapchain a = {&first, &surface, {765, 503}}, b = {&second, &surface, {765, 503}};
    struct vulkan_device device = {fake_submit};
    struct vulkan_queue queue = {&device, {(VkQueue)(uintptr_t)0xc0}, {3}};
    struct semaphore rendered = sem(0x71), acquired = sem(0x72);
    VkSemaphore waits[2] = {(VkSemaphore)(uintptr_t)&acquired, (VkSemaphore)(uintptr_t)&rendered};
    VkSwapchainKHR chains[2] = {(VkSwapchainKHR)(uintptr_t)&a, (VkSwapchainKHR)(uintptr_t)&b};
    uint32_t indices[2] = {1, 2};
    VkResult results[2];
    VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 2,
            .pWaitSemaphores = waits, .swapchainCount = 2, .pSwapchains = chains, .pImageIndices = indices,
            .pResults = results};

    acquired.ps5_acquired = TRUE; /* presented without rendering: consumed, never signalled */
    checked_windows = 0;
    assert(!ps5_compositor_queue_present(&queue, &info));
    assert(!acquired.ps5_acquired && first.presents == 1 && second.presents == 1);
    assert(first.wait_count == 1 && first.waits[0] == H(0x71) && !second.wait_count);
    assert(first.queue == queue.host.queue && first.family == 3 && first.index == 1 && second.index == 2);
    assert(!results[0] && !results[1] && checked_windows == 2);

    b.extents.width = 640;        /* stale extent: suboptimal, still presented */
    assert(ps5_compositor_queue_present(&queue, &info) == VK_SUBOPTIMAL_KHR);
    assert(!results[0] && results[1] == VK_SUBOPTIMAL_KHR && second.presents == 2);
    window_exists = FALSE;        /* destroyed window: out of date */
    assert(ps5_compositor_queue_present(&queue, &info) == VK_ERROR_OUT_OF_DATE_KHR);
    assert(results[0] == VK_ERROR_OUT_OF_DATE_KHR && results[1] == VK_ERROR_OUT_OF_DATE_KHR);
    window_exists = TRUE;
    second.present_result = VK_ERROR_SURFACE_LOST_KHR;
    assert(ps5_compositor_queue_present(&queue, &info) == VK_ERROR_SURFACE_LOST_KHR);
    assert(!results[0] && results[1] == VK_ERROR_SURFACE_LOST_KHR);
}

/* GDI window surfaces: dce.c's offscreen surface creation/flush/destroy with the real
 * compositor GDI-layer glue. A resized window gets a new layer of the new size, and a
 * replaced surface released late never removes its successor's layer. */
static unsigned gdi_count(const char *needle)
{
    unsigned n = 0;
    for (const char *p = gdi_log; (p = strstr(p, needle)); p++) n++;
    return n;
}

static void gdi_flush(struct window_surface *surface)
{
    assert(surface->funcs->flush(surface, &surface->rect, &surface->rect, &surface->info, gdi_bits, FALSE, NULL, NULL));
}

static void test_gdi_surfaces(void)
{
    HWND hwnd = (HWND)(uintptr_t)0x300aa;
    RECT small = {0, 0, 896, 640}, large = {0, 0, 1152, 640};
    struct window_surface *surface = NULL, *first, *second;

    ps5_compositor = (struct ps5c_compositor *)1;
    /* A Vulkan surface on a GDI window (Java2D's Direct3D 9 GDI fallback on the frame) must not
     * set the pixel format that clips the client area out of GDI drawing: run 5 lost every
     * frame-window GDI flush right after DXVK created its surface on the frame. */
    ps5_compositor_mode = FALSE;
    assert(surface_sets_pixel_format(FALSE) && !surface_sets_pixel_format(TRUE));
    ps5_compositor_mode = TRUE;
    assert(!surface_sets_pixel_format(FALSE) && !surface_sets_pixel_format(TRUE));
    assert(gdi_count("compositor surface pixel_format=skipped readback=0") == 1);
    create_offscreen_window_surface(hwnd, &small, &surface);
    first = surface;
    assert(first && EqualRect(&first->rect, &small) && first->info.bmiHeader.biHeight == -640);
    gdi_flush(first);
    assert(chains_created == 1 && ps5_gdi_layers[0].hwnd == hwnd && ps5_gdi_layers[0].owner == first);
    assert(ps5c_chain_size(ps5_gdi_layers[0].chain).width == 896);
    assert(gdi_count("gdi hwnd=0x300aa surface=896x640 result=0 replaced=0") == 1);
    assert(gdi_count("gdi flush hwnd=0x300aa surface=896x640 slot=0 upload=0") == 1);

    surface = first;                       /* same size: the surface and layer are reused */
    create_offscreen_window_surface(hwnd, &small, &surface);
    assert(surface == first && chains_created == 1 && !chains_destroyed);

    /* The window grew while its offscreen surface was current: a new surface, a new layer. */
    create_offscreen_window_surface(hwnd, &large, &surface);
    second = surface;
    assert(second != first && EqualRect(&second->rect, &large) && chains_destroyed == 1);
    gdi_flush(second);
    assert(chains_created == 2 && ps5_gdi_layers[0].owner == second);
    assert(ps5c_chain_size(ps5_gdi_layers[0].chain).width == 1152 && ps5_gdi_layers[0].chain->uploads == 1);
    assert(gdi_count("gdi hwnd=0x300aa surface=1152x640 result=0 replaced=0") == 1);

    /* The superseded surface is still referenced (a DC) when its successor flushes first. */
    window_surface_add_ref(second);
    create_offscreen_window_surface(hwnd, &small, &surface);   /* the window shrank again */
    first = surface;
    assert(first != second && second->ref == 1 && chains_destroyed == 1);
    gdi_flush(first);
    assert(chains_destroyed == 2 && ps5_gdi_layers[0].owner == first);
    assert(ps5c_chain_size(ps5_gdi_layers[0].chain).width == 896);
    assert(gdi_count("gdi hwnd=0x300aa surface=896x640 result=0 replaced=1") == 1);
    window_surface_release(second);          /* late release of the superseded surface */
    assert(ps5_gdi_layers[0].hwnd == hwnd && ps5_gdi_layers[0].owner == first && ps5_gdi_layers[0].chain);
    assert(gdi_count("gdi destroy hwnd=0x300aa owned=0 stale=1") == 1);

    for (unsigned i = 0; i < 100; i++) gdi_flush(first);   /* logging stays bounded */
    assert(gdi_count("gdi flush") <= PS5_GDI_LOG_LIMIT && gdi_count("gdi flush") > 1);

    window_surface_release(first);           /* the owner going removes the layer */
    assert(!ps5_gdi_layers[0].hwnd && !ps5_gdi_layers[0].chain && chains_created - chains_destroyed == 0);
    ps5_compositor_mode = FALSE;             /* direct mode: no layers at all */
    surface = NULL;
    create_offscreen_window_surface(hwnd, &small, &surface);
    gdi_flush(surface);
    assert(!ps5_gdi_layers[0].hwnd);
    window_surface_release(surface);
}

/* Input maps through the composed desktop rectangle, not one window. */
static void test_desktop_input(void)
{
    struct wine_ps5_viewport fit;
    struct wine_ps5_viewport_map map;
    POINT point;

    assert(wine_ps5_viewport_fit(&fit, 765, 503, 3840, 2160));
    ps5_set_present_desktop_viewport((void *)1, 1537, 828, 765, 503, &fit);
    assert(ps5_get_present_viewport(&map));
    assert(map.origin_x == 1537 && map.origin_y == 828 && map.client_width == 765 && map.client_height == 503);
    ps5_viewport_input_point(&map, fit.left, 0, &point);
    assert(point.x == 1537 && point.y == 828);
    ps5_viewport_input_point(&map, fit.left + fit.width - 1, 2159, &point);
    assert(point.x == 1537 + 764 && point.y == 828 + 502);
    ps5_viewport_input_point(&map, 0, 1080, &point);  /* black bar clamps to the view edge */
    assert(point.x == 1537);
    ps5_set_present_desktop_viewport((void *)1, 0, 0, 0, 0, &fit);
    assert(!ps5_get_present_viewport(&map));
}

int main(void)
{
    test_surface_formats();
    test_window_state();
    test_acquire();
    test_signal_acquired();
    test_present();
    test_desktop_input();
    test_gdi_surfaces();
    puts("TITLE_COMPOSITOR_GLUE_PASS scope=EXTRACTED_WIN32U_FUNCTIONS_WITH_FAKES");
    return 0;
}
'''
generated = args.out / 'test-title-compositor-glue.c'
generated.write_text(harness + formats + '\n' + glue + '\n' + viewport + gdi_harness + gdi_glue + cases)
binary = generated.with_suffix('')
# Extracted Wine code builds with -Wall -Werror, as Wine's own flags (no -Wextra).
subprocess.run(shlex.split(args.cc) + [flag for flag in common if flag != '-Wextra'] +
               ['-Wno-unused-function', '-Wno-unused-variable', '-D__PROSPERO__', '-DWINE_PS5_RUNESCAPE',
                str(generated), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)

# 3. Real Vulkan readback (host only).
if args.vulkan:
    binary = args.out / 'test-title-compositor-vulkan'
    subprocess.run(shlex.split(args.cc) + common + [str(here / 'test-title-compositor-vulkan.c'), str(core),
                   '-o', str(binary), '-l:libvulkan.so.1', '-lm'], check=True)
    env = dict(os.environ)
    lavapipe = Path('/usr/share/vulkan/icd.d/lvp_icd.json')
    if 'VK_ICD_FILENAMES' not in env and lavapipe.exists():
        env['VK_ICD_FILENAMES'] = str(lavapipe)
    subprocess.run([str(binary)], check=True, env=env)
