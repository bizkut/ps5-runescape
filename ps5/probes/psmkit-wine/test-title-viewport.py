#!/usr/bin/env python3
"""Exercise the existing fullscreen GPU command with RuneScape aspect fitting."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--wine', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
parser.add_argument('--vulkan', action='store_true', help='also run real host CPU Vulkan pixel readback')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('viewport_functions', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
record = extract.definition(args.wine / 'dlls/win32u/vulkan.c', 'record_compute_cmd')
activation = extract.definition(args.wine / 'dlls/win32u/vulkan.c', 'surface_get_fshack_dpi')
pipeline = extract.definition(args.wine / 'dlls/win32u/vulkan.c', 'create_pipeline')
cleanup = '\n'.join(extract.definition(args.wine / 'dlls/win32u/vulkan.c', name) for name in (
    'destroy_fs_hack_image', 'destroy_fs_hack_images'))
driver = args.wine / 'dlls/win32u/driver.c'
driver_text = driver.read_text()
state = driver_text[driver_text.index('static pthread_mutex_t ps5_viewport_mutex'):
                    driver_text.index('void ps5_set_present_viewport')]
state += '\n'.join(extract.definition(driver, name) for name in (
    'ps5_set_present_viewport', 'ps5_clear_present_viewport', 'ps5_get_present_viewport',
    'ps5_viewport_input_point', 'ps5_viewport_cursor_point'))
harness = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include "wine/ps5_viewport.h"
#ifdef TITLE_VIEWPORT_CONTROL
#undef WINE_PS5_RUNESCAPE
#endif
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#include <stdlib.h>
int title_vulkan = 1;
struct fs_hack_image { VkCommandBuffer cmd;VkImage user_image, swapchain_image;VkDescriptorSet descriptor_set;
    VkImageView user_view, blit_view;VkSemaphore blit_finished;uint32_t cmd_queue_idx; };
struct swapchain { VkExtent2D extents, host_extents;VkPipeline pipeline;VkPipelineLayout pipeline_layout;
    int title_fit;struct wine_ps5_viewport title_viewport;uint32_t n_images;
    struct fs_hack_image *fs_hack_images;VkCommandPool *cmd_pools;VkDeviceMemory user_image_memory; };
static float pushed[4];
static unsigned dispatch_x, dispatch_y, begin_count, end_count, barrier_count;
static VkResult begin_result;
static VkResult begin(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info) { begin_count++;return begin_result; }
static VkResult end(VkCommandBuffer cmd) { end_count++;return VK_SUCCESS; }
static unsigned live_handles;
static void release_handle(uintptr_t handle) {
    if (!handle) return;
    assert(handle < 8 && (live_handles & (1u << handle)));live_handles &= ~(1u << handle);
}
static void destroy_view(VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator) { release_handle((uintptr_t)view); }
static void destroy_image(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) { release_handle((uintptr_t)image); }
static void destroy_semaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *allocator) { release_handle((uintptr_t)semaphore); }
static void destroy_pool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *allocator) { release_handle((uintptr_t)pool); }
static void free_memory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator) { release_handle((uintptr_t)memory); }
static void free_commands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer *commands) {
    assert(count == 1 && (uintptr_t)pool == 6);release_handle((uintptr_t)*commands);
}
static void barriers(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
                     uint32_t nmem, const VkMemoryBarrier *mem, uint32_t nbuf, const VkBufferMemoryBarrier *buf,
                     uint32_t nimg, const VkImageMemoryBarrier *images) {
    assert(nimg == 2);barrier_count++;
}
static void pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipe) { assert(point == VK_PIPELINE_BIND_POINT_COMPUTE); }
static void descriptors(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
                        uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t ndynamic, const uint32_t *offsets) {
    assert(count == 1 && !ndynamic);
}
static void constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                      uint32_t offset, uint32_t bytes, const void *data) {
    assert(!offset && bytes == sizeof(pushed));memcpy(pushed, data, bytes);
}
static void dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) { dispatch_x = x;dispatch_y = y;assert(z == 1); }
static int specialized;
static VkResult create_compute(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *result) {
    assert(count == 1);specialized = infos->stage.pSpecializationInfo != NULL;
    if (specialized) {
        const VkSpecializationInfo *info = infos->stage.pSpecializationInfo;
        assert(info->mapEntryCount == 1 && info->pMapEntries[0].constantID == 0 &&
               info->dataSize == sizeof(VkBool32) && *(const VkBool32 *)info->pData == VK_TRUE);
    }
    *result = (VkPipeline)(uintptr_t)1;return VK_SUCCESS;
}
struct vulkan_device {
    PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
    PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
    PFN_vkCmdBindPipeline p_vkCmdBindPipeline;
    PFN_vkCmdBindDescriptorSets p_vkCmdBindDescriptorSets;
    PFN_vkCmdPushConstants p_vkCmdPushConstants;
    PFN_vkCmdDispatch p_vkCmdDispatch;
    struct { VkDevice device; } host;
    PFN_vkCreateComputePipelines p_vkCreateComputePipelines;
    uint32_t queue_count;
    PFN_vkDestroyImageView p_vkDestroyImageView;
    PFN_vkDestroyImage p_vkDestroyImage;
    PFN_vkDestroySemaphore p_vkDestroySemaphore;
    PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
    PFN_vkFreeMemory p_vkFreeMemory;
    PFN_vkFreeCommandBuffers p_vkFreeCommandBuffers;
};
typedef int BOOL;
typedef int32_t LONG;
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
typedef unsigned UINT;
typedef void *HWND;
typedef struct { int32_t x, y; } POINT;
typedef struct { int32_t left, top, right, bottom; } RECT;
#define TRUE 1
#define FALSE 0
#define MDT_RAW_DPI 1
struct surface { HWND hwnd; };
int fshack_enabled = 1;
static int window_valid = 1, client_width = 765, client_height = 503, origin_x = 100, origin_y = 200;
static unsigned thread_dpi = 96, window_dpi = 96, monitor_dpi = 96;
UINT get_thread_dpi(void) { return thread_dpi; }
UINT NtUserGetDpiForWindow(HWND window) { return window_dpi; }
UINT NtUserGetWinMonitorDpi(HWND window, int kind) { return monitor_dpi; }
BOOL NtUserIsWindow(HWND window) { return window_valid; }
static unsigned cursor_warps;
static POINT warped_cursor;
BOOL NtUserSetCursorPos(int32_t x, int32_t y) { cursor_warps++;warped_cursor = (POINT){x,y};return TRUE; }
BOOL NtUserGetClientRect(HWND window, RECT *rect, UINT dpi) {
    assert(dpi == thread_dpi);*rect = (RECT){0, 0, client_width, client_height};return window_valid;
}
int NtUserMapWindowPoints(HWND from, HWND to, POINT *point, UINT count, UINT dpi) {
    assert(!to && count == 1 && dpi == thread_dpi);point->x += origin_x;point->y += origin_y;return 0;
}
RECT map_rect_virt_to_raw(RECT rect, UINT dpi) {
    assert(dpi == thread_dpi);
    rect.left = (int64_t)rect.left * monitor_dpi / dpi;
    rect.right = (int64_t)rect.right * monitor_dpi / dpi;
    rect.top = (int64_t)rect.top * monitor_dpi / dpi;
    rect.bottom = (int64_t)rect.bottom * monitor_dpi / dpi;
    return rect;
}
'''
harness += '#ifdef WINE_PS5_RUNESCAPE\n' + state + '\n#endif\n'
harness += activation
cases = r'''
int main(void) {
    struct vulkan_device device = {begin, end, barriers, pipeline, descriptors, constants, dispatch};
    struct fs_hack_image image = {0};
    struct swapchain swapchain = {.extents = {765,503}, .host_extents = {3840,2160}, .title_fit = 1};
    struct surface surface = {0};
    assert(wine_ps5_viewport_fit(&swapchain.title_viewport, 765, 503, 3840, 2160));
    assert(swapchain.title_viewport.left == 277 && !swapchain.title_viewport.top &&
           swapchain.title_viewport.width == 3285 && swapchain.title_viewport.height == 2160);
#ifdef WINE_PS5_RUNESCAPE
    assert(surface_get_fshack_dpi(&surface, &swapchain.extents, &swapchain.host_extents) == 96);
#else
    assert(!surface_get_fshack_dpi(&surface, &swapchain.extents, &swapchain.host_extents));
#endif
    device.p_vkCreateComputePipelines = create_compute;
    assert(!create_pipeline(&device, &swapchain, VK_NULL_HANDLE));
#ifdef WINE_PS5_RUNESCAPE
    assert(specialized);
#else
    assert(!specialized);
#endif
    assert(!record_compute_cmd(&device, &swapchain, &image));
    assert(begin_count == 1 && end_count == 1 && barrier_count == 2);
    assert(dispatch_x == 480 && dispatch_y == 270); /* write the bars too */
#ifdef WINE_PS5_RUNESCAPE
    assert(fabsf(pushed[2] - 3285.0f) < .001f && pushed[3] == 2160.0f);
    assert(pushed[0] == 276.5f && pushed[1] == -.5f);
#else
    assert(pushed[2] == 3840.0f && pushed[3] == 2160.0f);
    assert(fabsf(pushed[0] - (-.5f * 3840.0f / 765.0f)) < .001f);
#endif
    swapchain.extents = (VkExtent2D){1920,1080};
    assert(wine_ps5_viewport_fit(&swapchain.title_viewport, 1920, 1080, 3840, 2160));
    assert(!record_compute_cmd(&device, &swapchain, &image));
#ifdef WINE_PS5_RUNESCAPE
    assert(pushed[0] == -.5f && pushed[1] == -.5f && pushed[2] == 3840.0f && pushed[3] == 2160.0f);
#else
    assert(pushed[0] == -1.0f && pushed[1] == -1.0f && pushed[2] == 3840.0f && pushed[3] == 2160.0f);
#endif
    unsigned barriers_before = barrier_count, ends_before = end_count;
    begin_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(record_compute_cmd(&device, &swapchain, &image) == begin_result);
    assert(barrier_count == barriers_before && end_count == ends_before);begin_result = VK_SUCCESS;
    device.queue_count = 2;device.p_vkDestroyImageView = destroy_view;device.p_vkDestroyImage = destroy_image;
    device.p_vkDestroySemaphore = destroy_semaphore;device.p_vkDestroyCommandPool = destroy_pool;
    device.p_vkFreeMemory = free_memory;device.p_vkFreeCommandBuffers = free_commands;
    swapchain.fs_hack_images = calloc(3, sizeof(*swapchain.fs_hack_images));
    swapchain.cmd_pools = calloc(2, sizeof(*swapchain.cmd_pools));assert(swapchain.fs_hack_images && swapchain.cmd_pools);
    swapchain.n_images = 3;swapchain.user_image_memory = (VkDeviceMemory)(uintptr_t)7;
    swapchain.cmd_pools[0] = (VkCommandPool)(uintptr_t)6;
    swapchain.fs_hack_images[0] = (struct fs_hack_image){.cmd = (VkCommandBuffer)(uintptr_t)4,
        .user_image = (VkImage)(uintptr_t)1, .user_view = (VkImageView)(uintptr_t)2,
        .blit_finished = (VkSemaphore)(uintptr_t)3};
    swapchain.fs_hack_images[1].blit_finished = (VkSemaphore)(uintptr_t)5; /* partial image setup */
    live_handles = 0xfe;
    destroy_fs_hack_images(&device, &swapchain);
    assert(!live_handles && !swapchain.n_images && !swapchain.fs_hack_images &&
           !swapchain.cmd_pools && !swapchain.user_image_memory);
    destroy_fs_hack_images(&device, &swapchain);assert(!live_handles);
    VkExtent2D native = {3840,2160};
    assert(!surface_get_fshack_dpi(&surface, &native, &native)); /* no avoidable copy at native size */
    fshack_enabled = 0;
    assert(!surface_get_fshack_dpi(&surface, &swapchain.extents, &swapchain.host_extents));fshack_enabled = 1;
#ifdef WINE_PS5_RUNESCAPE
    struct wine_ps5_viewport view;
    struct wine_ps5_viewport_map map;
    assert(!wine_ps5_viewport_fit(&view, 0, 503, 3840, 2160));
    assert(!wine_ps5_viewport_fit(&view, 765, 503, 0, 2160));
    assert(!wine_ps5_viewport_fit(&view, UINT32_MAX, 503, 3840, 2160));
    assert(wine_ps5_viewport_fit(&view, 765, 503, 3840, 2160));
    assert(!ps5_get_present_viewport(&map));
    ps5_set_present_viewport((void *)1, (HWND)2, &view);assert(ps5_get_present_viewport(&map));
    int32_t x, y, rx, ry;
    wine_ps5_viewport_to_client(&map, 0, 0, &x, &y);assert(x == 100 && y == 200);
    wine_ps5_viewport_to_client(&map, 3839, 2159, &x, &y);assert(x == 864 && y == 702);
    for (int row = 0; row < 503; row += 31)
    for (int column = 0; column < 765; column++) {
        wine_ps5_viewport_to_output(&map, column + 100, row + 200, &x, &y);
        wine_ps5_viewport_to_client(&map, x, y, &rx, &ry);
        assert(rx == column + 100 && ry == row + 200);
    }
    /* A moved window and a different GUI-thread DPI use the live client rectangle. */
    origin_x = -300;origin_y = 400;client_width = 1148;client_height = 755;thread_dpi = 144;
    assert(ps5_get_present_viewport(&map));
    wine_ps5_viewport_to_client(&map, 277, 0, &x, &y);assert(x == -300 && y == 400);
    wine_ps5_viewport_to_client(&map, 3561, 2159, &x, &y);assert(x == 847 && y == 1154);
    POINT raw;
    monitor_dpi = 288;
    ps5_viewport_input_point(&map, 277, 0, &raw);assert(raw.x == -600 && raw.y == 800);
    ps5_viewport_input_point(&map, 3561, 2159, &raw);assert(raw.x == 1694 && raw.y == 2308);
    POINT cursor = {-1000, 400};
    ps5_viewport_cursor_point(&map, &cursor);
    assert(cursor_warps == 1 && warped_cursor.x == -300 && warped_cursor.y == 400 &&
           cursor.x == 277 && !cursor.y); /* GUI edge follows the visible cursor; raw deltas are not changed */
    cursor = (POINT){847,1154};ps5_viewport_cursor_point(&map, &cursor);
    assert(cursor_warps == 1 && cursor.x == 3561 && cursor.y == 2159);
    ps5_clear_present_viewport((void *)3);assert(ps5_get_present_viewport(&map));
    window_valid = 0;assert(!ps5_get_present_viewport(&map));window_valid = 1;
    client_width = 0;assert(!ps5_get_present_viewport(&map));client_width = 1148;
    ps5_clear_present_viewport((void *)1);assert(!ps5_get_present_viewport(&map));
    assert(wine_ps5_viewport_fit(&view, 1080, 1920, 3840, 2160));
    assert(view.left == 1312 && !view.top && view.width == 1215 && view.height == 2160);
#endif
    puts("title viewport: existing GPU blit, centered aspect fit, full-output dispatch and non-product stretch preserved PASS");
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
for control in (False, True):
    generated = args.out / ('test-title-viewport' + ('-control' if control else '') + '.c')
    generated.write_text(harness + pipeline + record + cleanup + cases)
    binary = generated.with_suffix('')
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Werror', '-pthread',
        '-D__PROSPERO__', *(['-DTITLE_VIEWPORT_CONTROL'] if control else []),
        '-I' + str(here.parents[2] / 'vkd3d-proton/khronos/Vulkan-Headers/include'),
        '-idirafter', str(args.wine / 'include'), str(generated), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True)

if args.vulkan:
    text = (args.wine / 'dlls/win32u/vulkan.c').read_text()
    array = re.search(r'const uint32_t blit_comp_spv\[\]\s*=\s*\{[^}]*\};', text).group(0)
    (args.out / 'win32u-blit-array.inc').write_text(array + '\n')
    binary = args.out / 'test-title-viewport-vulkan'
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-I' + str(args.out.resolve()),
        '-I' + str(here.parents[2] / 'vkd3d-proton/khronos/Vulkan-Headers/include'),
        '-idirafter', str(args.wine / 'include'), str(here / 'test-title-viewport-vulkan.c'),
        '-o', str(binary), '-l:libvulkan.so.1'], check=True)
    subprocess.run([str(binary)], check=True)
