/* Real-Vulkan host contract for the RuneScape title window compositor
 * (Wine dlls/win32u/ps5_compositor.c). It runs the production core against a
 * CPU Vulkan driver (lavapipe): one compositor VkDevice owning an offscreen
 * "output", two separate producer VkDevices whose chain images are shared
 * through VK_EXT_external_memory_host, and readback of every output pixel
 * against an independent painter's-algorithm oracle. Host scope only: no PS5
 * RADV, VideoOut, scanout or Win32 window manager is exercised here. */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "wine/ps5_compositor.h"

#define CHECK(expr) do { VkResult check_res = (expr); if (check_res != VK_SUCCESS) { \
    fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #expr, check_res); abort(); } } while (0)
#define OUT_W 192
#define OUT_H 108

static VkInstance instance;
static VkPhysicalDevice physical;
static VkPhysicalDeviceMemoryProperties memory_props;

/* --- Devices, with vkCreateImage counted per device ------------------------- */

struct test_device
{
    VkDevice device;
    VkQueue queue;
    struct ps5c_device funcs;
    VkCommandPool pool;
    PFN_vkCreateImage real_create_image;
    unsigned created_images;
};

static struct test_device *devices[4];
static unsigned device_count;

static VKAPI_ATTR VkResult VKAPI_CALL counting_create_image( VkDevice device, const VkImageCreateInfo *info,
                                                            const VkAllocationCallbacks *allocator, VkImage *image )
{
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i]->device == device)
        {
            __atomic_add_fetch( &devices[i]->created_images, 1, __ATOMIC_SEQ_CST );
            return devices[i]->real_create_image( device, info, allocator, image );
        }
    abort();
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL test_get_device_proc( VkDevice device, const char *name )
{
    if (!strcmp( name, "vkCreateImage" )) return (PFN_vkVoidFunction)counting_create_image;
    return vkGetDeviceProcAddr( device, name );
}

static void create_device( struct test_device *dev )
{
    float priority = 1;
    const char *extensions[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
            .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {.shaderStorageImageWriteWithoutFormat = VK_TRUE};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue, .enabledExtensionCount = 1, .ppEnabledExtensionNames = extensions,
            .pEnabledFeatures = &features};
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};

    CHECK( vkCreateDevice( physical, &info, NULL, &dev->device ) );
    vkGetDeviceQueue( dev->device, 0, 0, &dev->queue );
    dev->real_create_image = (PFN_vkCreateImage)vkGetDeviceProcAddr( dev->device, "vkCreateImage" );
    devices[device_count++] = dev;
    CHECK( ps5c_device_init( &dev->funcs, dev->device, test_get_device_proc ) );
    CHECK( vkCreateCommandPool( dev->device, &pool, NULL, &dev->pool ) );
}

static uint32_t memory_type( uint32_t bits, VkMemoryPropertyFlags flags )
{
    for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    abort();
}

static VkCommandBuffer begin_commands( struct test_device *dev )
{
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = dev->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkCommandBuffer cmd;
    CHECK( vkAllocateCommandBuffers( dev->device, &alloc, &cmd ) );
    CHECK( vkBeginCommandBuffer( cmd, &begin ) );
    return cmd;
}

static void barrier( VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to )
{
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                          0, NULL, 0, NULL, 1, &b );
}

/* --- The offscreen output (the VideoOut swapchain's stand-in) --------------- */

struct test_output
{
    pthread_mutex_t lock;
    struct test_device *dev;
    VkImage images[3];
    VkDeviceMemory memory[3];
    uint32_t next, presented, acquires, presents;
};

static VkResult output_acquire( void *context, VkSemaphore signal, uint32_t *index )
{
    struct test_output *out = context;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1,
            .pSignalSemaphores = &signal};
    VkResult res;
    pthread_mutex_lock( &out->lock );
    *index = out->next;
    out->next = (out->next + 1) % 3;
    out->acquires++;
    res = vkQueueSubmit( out->dev->queue, 1, &submit, VK_NULL_HANDLE );
    pthread_mutex_unlock( &out->lock );
    return res;
}

static VkResult output_present( void *context, VkQueue queue, uint32_t index, VkSemaphore wait )
{
    struct test_output *out = context;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &wait, .pWaitDstStageMask = &stage};
    VkResult res;
    pthread_mutex_lock( &out->lock );
    assert( queue == out->dev->queue );
    out->presented = index;
    out->presents++;
    res = vkQueueSubmit( queue, 1, &submit, VK_NULL_HANDLE );
    pthread_mutex_unlock( &out->lock );
    return res;
}

static void create_output( struct test_output *out, struct test_device *dev )
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {OUT_W, OUT_H, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    pthread_mutex_init( &out->lock, NULL );
    out->dev = dev;
    for (int i = 0; i < 3; i++)
    {
        VkMemoryRequirements reqs;
        VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        CHECK( vkCreateImage( dev->device, &info, NULL, &out->images[i] ) );
        vkGetImageMemoryRequirements( dev->device, out->images[i], &reqs );
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
        CHECK( vkAllocateMemory( dev->device, &alloc, NULL, &out->memory[i] ) );
        CHECK( vkBindImageMemory( dev->device, out->images[i], out->memory[i], 0 ) );
    }
}

/* Reads the last presented output image as packed B,G,R,A bytes. */
static void read_output( struct test_output *out, uint8_t *pixels )
{
    struct test_device *dev = out->dev;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = OUT_W * OUT_H * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {OUT_W, OUT_H, 1}};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryRequirements reqs;
    VkDeviceMemory memory;
    VkCommandBuffer cmd;
    VkBuffer buffer;
    void *data;

    CHECK( vkCreateBuffer( dev->device, &info, NULL, &buffer ) );
    vkGetBufferMemoryRequirements( dev->device, buffer, &reqs );
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    CHECK( vkAllocateMemory( dev->device, &alloc, NULL, &memory ) );
    CHECK( vkBindBufferMemory( dev->device, buffer, memory, 0 ) );
    pthread_mutex_lock( &out->lock );
    cmd = begin_commands( dev );
    barrier( cmd, out->images[out->presented], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
    vkCmdCopyImageToBuffer( cmd, out->images[out->presented], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region );
    barrier( cmd, out->images[out->presented], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );
    CHECK( vkEndCommandBuffer( cmd ) );
    submit.pCommandBuffers = &cmd;
    CHECK( vkQueueSubmit( dev->queue, 1, &submit, VK_NULL_HANDLE ) );
    CHECK( vkQueueWaitIdle( dev->queue ) );
    pthread_mutex_unlock( &out->lock );
    CHECK( vkMapMemory( dev->device, memory, 0, VK_WHOLE_SIZE, 0, &data ) );
    memcpy( pixels, data, OUT_W * OUT_H * 4 );
    vkUnmapMemory( dev->device, memory );
    vkFreeCommandBuffers( dev->device, dev->pool, 1, &cmd );
    vkDestroyBuffer( dev->device, buffer, NULL );
    vkFreeMemory( dev->device, memory, NULL );
}

/* --- Producers ---------------------------------------------------------------- */

struct producer
{
    struct test_device *dev;
    struct ps5c_chain *chain;
    VkImage images[PS5C_MAX_CHAIN_IMAGES];
    uint32_t width, height;
};

static void create_producer( struct ps5c_compositor *comp, struct producer *p, struct test_device *dev,
                             uint64_t key, uint32_t width, uint32_t height, VkFormat format )
{
    struct ps5c_chain_desc desc = {.image_count = 3, .format = format, .extent = {width, height},
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    p->dev = dev;
    p->width = width;
    p->height = height;
    CHECK( ps5c_chain_create( comp, key, &dev->funcs, &desc, &p->chain ) );
    assert( ps5c_chain_images( p->chain, p->images, PS5C_MAX_CHAIN_IMAGES ) == 3 );
}

/* Colours are byte quadruples B,G,R,A with 0/255 channels (exact through the
 * sRGB round trip). block > 0 draws four quadrants of the given colours. */
struct paint
{
    uint8_t quad[4][4]; /* TL, TR, BL, BR */
};

static struct paint solid( uint8_t b, uint8_t g, uint8_t r )
{
    struct paint p;
    for (int i = 0; i < 4; i++) { p.quad[i][0] = b; p.quad[i][1] = g; p.quad[i][2] = r; p.quad[i][3] = 255; }
    return p;
}

static void present_frame( struct producer *p, const struct paint *paint )
{
    struct test_device *dev = p->dev;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = p->width * p->height * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {p->width, p->height, 1}};
    VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
            .signalSemaphoreCount = 1};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryRequirements reqs;
    VkDeviceMemory memory;
    VkCommandBuffer cmd;
    VkSemaphore rendered;
    VkBuffer buffer;
    VkFence done;
    uint32_t index;
    uint8_t *data;

    CHECK( ps5c_chain_acquire( p->chain, UINT64_MAX, &index ) );
    CHECK( vkCreateBuffer( dev->device, &info, NULL, &buffer ) );
    vkGetBufferMemoryRequirements( dev->device, buffer, &reqs );
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = memory_type( reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    CHECK( vkAllocateMemory( dev->device, &alloc, NULL, &memory ) );
    CHECK( vkBindBufferMemory( dev->device, buffer, memory, 0 ) );
    CHECK( vkMapMemory( dev->device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&data ) );
    for (uint32_t y = 0; y < p->height; y++)
        for (uint32_t x = 0; x < p->width; x++)
            memcpy( data + (y * p->width + x) * 4,
                    paint->quad[(y >= p->height / 2) * 2 + (x >= p->width / 2)], 4 );
    vkUnmapMemory( dev->device, memory );

    cmd = begin_commands( dev );
    barrier( cmd, p->images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
    vkCmdCopyBufferToImage( cmd, buffer, p->images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
    barrier( cmd, p->images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR );
    CHECK( vkEndCommandBuffer( cmd ) );
    CHECK( vkCreateSemaphore( dev->device, &sem_info, NULL, &rendered ) );
    CHECK( vkCreateFence( dev->device, &fence_info, NULL, &done ) );
    submit.pCommandBuffers = &cmd;
    submit.pSignalSemaphores = &rendered;
    CHECK( vkQueueSubmit( dev->queue, 1, &submit, done ) );
    CHECK( ps5c_chain_present( p->chain, dev->queue, 0, index, 1, &rendered ) );
    /* The producer's own queue is used only by this thread. */
    CHECK( vkWaitForFences( dev->device, 1, &done, VK_TRUE, UINT64_MAX ) );
    CHECK( vkQueueWaitIdle( dev->queue ) );
    vkDestroyFence( dev->device, done, NULL );
    vkDestroySemaphore( dev->device, rendered, NULL );
    vkFreeCommandBuffers( dev->device, dev->pool, 1, &cmd );
    vkDestroyBuffer( dev->device, buffer, NULL );
    vkFreeMemory( dev->device, memory, NULL );
}

/* --- Oracle --------------------------------------------------------------------- */

struct oracle_window
{
    const struct ps5c_window_state *state;
    const struct paint *paint;
    uint32_t image_width, image_height;
};

static int z_less( const struct ps5c_window_state *a, const struct ps5c_window_state *b )
{
    uint32_t depth = a->z_depth < b->z_depth ? a->z_depth : b->z_depth;
    for (uint32_t i = 0; i < depth; i++) if (a->z[i] != b->z[i]) return a->z[i] < b->z[i];
    if (a->z_depth != b->z_depth) return a->z_depth < b->z_depth;
    return a->layer < b->layer;
}

/* Independent of the planner: the view is the union of visible top-level
 * clients, letterboxed with wine_ps5_viewport_fit; the topmost window whose
 * (clip, client, view) contains the pixel centre wins. Returns the number of
 * pixels checked; ambiguous pixels (sample point within 1e-3 desktop pixels of
 * an edge, or a bilinear footprint across a quadrant edge) are counted. */
static unsigned check_output( const uint8_t *pixels, struct oracle_window *windows, unsigned count,
                              unsigned *ambiguous, const char *label )
{
    struct ps5c_rect view = {0};
    struct wine_ps5_viewport fit;
    int have = 0;
    unsigned checked = 0, order[16];
    double sx, sy, ox, oy;

    *ambiguous = 0;
    for (unsigned i = 0; i < count; i++)
    {
        const struct ps5c_window_state *s = windows[i].state;
        /* The visible client area (a GDI surface includes its frame). */
        struct ps5c_rect v = {s->client.left > s->clip.left ? s->client.left : s->clip.left,
                              s->client.top > s->clip.top ? s->client.top : s->clip.top,
                              s->client.right < s->clip.right ? s->client.right : s->clip.right,
                              s->client.bottom < s->clip.bottom ? s->client.bottom : s->clip.bottom};
        if (!s->visible || !s->top_level) continue;
        if (!have) view = v;
        else
        {
            if (v.left < view.left) view.left = v.left;
            if (v.top < view.top) view.top = v.top;
            if (v.right > view.right) view.right = v.right;
            if (v.bottom > view.bottom) view.bottom = v.bottom;
        }
        have = 1;
    }
    for (unsigned i = 0; i < count; i++) order[i] = i;
    for (unsigned i = 1; i < count; i++)
        for (unsigned j = i; j && z_less( windows[order[j]].state, windows[order[j - 1]].state ); j--)
        { unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    if (have) assert( wine_ps5_viewport_fit( &fit, view.right - view.left, view.bottom - view.top, OUT_W, OUT_H ) );
    sx = have ? (double)(view.right - view.left) / fit.width : 1;
    sy = have ? (double)(view.bottom - view.top) / fit.height : 1;
    ox = have ? view.left - fit.left * sx : 0;
    oy = have ? view.top - fit.top * sy : 0;

    for (int y = 0; y < OUT_H; y++)
        for (int x = 0; x < OUT_W; x++)
        {
            const uint8_t *got = pixels + (y * OUT_W + x) * 4;
            uint8_t expect[4] = {0, 0, 0, 255};
            double dx = (x + 0.5) * sx + ox, dy = (y + 0.5) * sy + oy;
            int skip = 0;

            for (int k = (int)count - 1; have && k >= 0 && !skip; k--)
            {
                const struct oracle_window *w = &windows[order[k]];
                const struct ps5c_window_state *s = w->state;
                double l, t, r, b;
                if (!s->visible) continue;
                l = fmax( fmax( s->clip.left, s->client.left ), view.left );
                t = fmax( fmax( s->clip.top, s->client.top ), view.top );
                r = fmin( fmin( s->clip.right, s->client.right ), view.right );
                b = fmin( fmin( s->clip.bottom, s->client.bottom ), view.bottom );
                if (r <= l || b <= t) continue;
                if (fabs( dx - l ) < 1e-3 || fabs( dx - r ) < 1e-3 || fabs( dy - t ) < 1e-3 || fabs( dy - b ) < 1e-3)
                {
                    skip = 1;
                    break;
                }
                if (dx < l || dx >= r || dy < t || dy >= b) continue;
                {
                    /* Bilinear footprint in texel space must stay in one quadrant. */
                    double tx = (dx - s->client.left) / (s->client.right - s->client.left) * w->image_width - 0.5;
                    double ty = (dy - s->client.top) / (s->client.bottom - s->client.top) * w->image_height - 0.5;
                    int x0 = (int)floor( tx ), y0 = (int)floor( ty ), x1 = x0 + 1, y1 = y0 + 1;
                    int hx = w->image_width / 2, hy = w->image_height / 2;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 >= (int)w->image_width) x1 = w->image_width - 1;
                    if (y1 >= (int)w->image_height) y1 = w->image_height - 1;
                    if ((x0 >= hx) != (x1 >= hx) || (y0 >= hy) != (y1 >= hy))
                    {
                        if (memcmp( w->paint->quad[0], w->paint->quad[1], 4 ) ||
                            memcmp( w->paint->quad[0], w->paint->quad[2], 4 ) ||
                            memcmp( w->paint->quad[0], w->paint->quad[3], 4 ))
                        {
                            skip = 1;
                            break;
                        }
                    }
                    memcpy( expect, w->paint->quad[(y0 >= hy) * 2 + (x0 >= hx)], 4 );
                }
                break;
            }
            if (skip)
            {
                (*ambiguous)++;
                continue;
            }
            if (memcmp( got, expect, 4 ))
            {
                fprintf( stderr, "%s: pixel %d,%d got %u,%u,%u,%u expected %u,%u,%u,%u\n", label, x, y,
                         got[0], got[1], got[2], got[3], expect[0], expect[1], expect[2], expect[3] );
                abort();
            }
            checked++;
        }
    return checked;
}

/* --- Tests ----------------------------------------------------------------------- */

static struct ps5c_view last_view;
static unsigned view_changes;

static void view_changed( void *context, const struct ps5c_view *view )
{
    (void)context;
    last_view = *view;
    view_changes++;
}

static struct ps5c_window_state window( uint64_t key, int l, int t, int r, int b, uint32_t top_level,
                                        uint32_t depth, uint16_t z0, uint16_t z1 )
{
    struct ps5c_window_state s = {.key = key, .visible = 1, .top_level = top_level, .client = {l, t, r, b},
            .clip = {l, t, r, b}, .z_depth = depth, .z = {z0, z1}};
    return s;
}

static unsigned total_checked, total_ambiguous;

static void verify( struct ps5c_compositor *comp, struct test_output *out, struct oracle_window *w, unsigned n,
                    const char *label )
{
    static uint8_t pixels[OUT_W * OUT_H * 4];
    unsigned checked, ambiguous;
    CHECK( ps5c_compositor_flush( comp ) );
    read_output( out, pixels );
    checked = check_output( pixels, w, n, &ambiguous, label );
    assert( checked + ambiguous == OUT_W * OUT_H );
    assert( ambiguous * 20 < OUT_W * OUT_H ); /* < 5%: edge samples and bilinear seams only */
    total_checked += checked;
    total_ambiguous += ambiguous;
    printf( "%s: %u pixels exact, %u edge pixels ambiguous\n", label, checked, ambiguous );
}

struct stress
{
    struct producer *producer;
    struct paint paints[2];
    int frames;
};

static void *stress_thread( void *arg )
{
    struct stress *s = arg;
    for (int i = 0; i < s->frames; i++) present_frame( s->producer, &s->paints[i & 1] );
    return NULL;
}

int main( void )
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    static struct test_device compositor_dev, producer_a, producer_b;
    static struct test_output out;
    struct ps5c_compositor_desc desc = {0};
    struct ps5c_compositor *comp;
    struct ps5c_stats stats;
    VkPhysicalDeviceProperties props;
    uint32_t count = 1;

    setvbuf( stdout, NULL, _IOLBF, 0 );
    CHECK( vkCreateInstance( &instance_info, NULL, &instance ) );
    CHECK( vkEnumeratePhysicalDevices( instance, &count, &physical ) );
    vkGetPhysicalDeviceProperties( physical, &props );
    vkGetPhysicalDeviceMemoryProperties( physical, &memory_props );
    printf( "device: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR( props.apiVersion ),
            VK_VERSION_MINOR( props.apiVersion ), VK_VERSION_PATCH( props.apiVersion ) );
    create_device( &compositor_dev );
    create_device( &producer_a );
    create_device( &producer_b );
    create_output( &out, &compositor_dev );

    desc.device = &compositor_dev.funcs;
    desc.queue = compositor_dev.queue;
    desc.queue_family = 0;
    desc.output.context = &out;
    desc.output.image_count = 3;
    memcpy( desc.output.images, out.images, sizeof(out.images) );
    desc.output.extent = (VkExtent2D){OUT_W, OUT_H};
    desc.output.format = VK_FORMAT_B8G8R8A8_UNORM;
    desc.output.acquire = output_acquire;
    desc.output.present = output_present;
    desc.view_changed = view_changed;
    CHECK( ps5c_compositor_create( &desc, &comp ) );
    /* The compositor makes no image of its own: no desktop/intermediate copy. */
    assert( compositor_dev.created_images == 0 );

    /* 1. Single window, one pass, no extra image: a 96x54 client fills the
     *    192x108 output exactly (scale 2). */
    {
        static struct producer w1;
        struct paint quads = {{{0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}, {255, 255, 255, 255}}};
        struct ps5c_window_state s1 = window( 0x100, 100, 100, 196, 154, 1, 1, 1, 0 );
        struct oracle_window ow[] = {{&s1, &quads, 96, 54}};

        create_producer( comp, &w1, &producer_a, 0x100, 96, 54, VK_FORMAT_B8G8R8A8_UNORM );
        assert( compositor_dev.created_images == 3 && producer_a.created_images == 3 );
        ps5c_scene_update( comp, &s1, 1 );
        present_frame( &w1, &quads );
        verify( comp, &out, ow, 1, "single-window" );
        ps5c_get_stats( comp, &stats );
        assert( stats.last_dispatches == 1 && stats.last_clears == 0 );
        assert( compositor_dev.created_images == 3 ); /* only the shared chain images */
        assert( last_view.valid && last_view.fit.width == OUT_W && last_view.fit.height == OUT_H );
        assert( last_view.desktop.left == 100 && last_view.desktop.right == 196 );

        /* A narrow window letterboxes: 48x54 -> 96x108 centred, bars black;
         * its 96x54 image is now squeezed into the client (image != client). */
        s1 = window( 0x100, 100, 100, 148, 154, 1, 1, 1, 0 );
        ow[0].state = &s1;
        ps5c_scene_update( comp, &s1, 1 );
        verify( comp, &out, ow, 1, "single-window-moved-stretched-letterbox" );
        assert( last_view.fit.left == 48 && last_view.fit.width == 96 );
        ps5c_get_stats( comp, &stats );
        assert( stats.last_dispatches == 1 && stats.last_clears == 0 );

        ps5c_chain_destroy( w1.chain );
        ps5c_scene_update( comp, NULL, 0 );
        verify( comp, &out, ow, 0, "single-window-gone" );
        ps5c_get_stats( comp, &stats );
        assert( stats.live_chains == 0 && stats.host_allocations == 0 && stats.live_images == 0 && stats.last_clears == 1 );
    }

    /* 2. Multiple windows on two producer VkDevices: z-order, child clip,
     *    visibility. W1 (device A) with child W3 (device A, its own chain)
     *    partly outside W1's client; W2 (device B) top-level overlapping both
     *    and extending the view, so W3's clipped-off part lies over W2. */
    {
        static struct producer w1, w2, w3;
        struct paint red = solid( 0, 0, 255 ), green = solid( 0, 255, 0 ), blue = solid( 255, 0, 0 );
        struct ps5c_window_state s[3];
        struct oracle_window ow[3] = {{&s[0], &red, 96, 54}, {&s[1], &green, 36, 26}, {&s[2], &blue, 30, 20}};

        s[0] = window( 0x200, 100, 100, 196, 154, 1, 1, 1, 0 );
        s[1] = window( 0x300, 150, 120, 214, 164, 1, 1, 2, 0 );
        s[2] = window( 0x400, 180, 140, 210, 160, 0, 2, 1, 0 );
        s[2].clip = (struct ps5c_rect){180, 140, 196, 154}; /* clipped by parent W1's client */
        compositor_dev.created_images = 0;
        create_producer( comp, &w1, &producer_a, 0x200, 96, 54, VK_FORMAT_B8G8R8A8_UNORM );
        create_producer( comp, &w2, &producer_b, 0x300, 36, 26, VK_FORMAT_B8G8R8A8_SRGB );
        create_producer( comp, &w3, &producer_a, 0x400, 30, 20, VK_FORMAT_B8G8R8A8_UNORM );
        ps5c_scene_update( comp, s, 3 );
        present_frame( &w1, &red );
        present_frame( &w2, &green );
        present_frame( &w3, &blue );
        verify( comp, &out, ow, 3, "multi-window-two-devices" );
        ps5c_get_stats( comp, &stats );
        assert( stats.last_dispatches == 3 );
        assert( compositor_dev.created_images == 9 ); /* 3 chains x 3 shared images, nothing else */

        /* Raise W1 (and its child) above W2: W3 must stay clipped to W1's
         * client, leaving W2 visible where W3's unclipped part would be. */
        s[0].z[0] = 3;
        s[2].z[0] = 3;
        ps5c_scene_update( comp, s, 3 );
        verify( comp, &out, ow, 3, "multi-window-raised-child-clipped" );

        /* Hide W2: two windows remain; the view shrinks to W1. */
        s[1].visible = 0;
        ps5c_scene_update( comp, s, 3 );
        verify( comp, &out, ow, 3, "multi-window-hidden" );
        ps5c_get_stats( comp, &stats );
        assert( stats.last_dispatches == 2 );

        /* Hide W1 (its child follows); show W2 alone: view becomes W2 only. */
        s[0].visible = s[2].visible = 0;
        s[1].visible = 1;
        ps5c_scene_update( comp, s, 3 );
        verify( comp, &out, ow, 3, "multi-window-only-w2" );
        assert( last_view.desktop.left == 150 && last_view.desktop.right == 214 );

        /* A presentation by a hidden window changes nothing on screen. */
        ps5c_get_stats( comp, &stats );
        {
            uint64_t composes = stats.composes;
            present_frame( &w1, &red );
            CHECK( ps5c_compositor_flush( comp ) );
            ps5c_get_stats( comp, &stats );
            assert( stats.composes == composes && stats.skipped >= 1 );
        }

        /* Concurrent producers on both devices while the scene changes. */
        s[0].visible = s[2].visible = 1;
        ps5c_scene_update( comp, s, 3 );
        {
            struct stress a = {&w1, {red, blue}, 41}, b = {&w2, {green, red}, 41};
            pthread_t ta, tb;
            pthread_create( &ta, NULL, stress_thread, &a );
            pthread_create( &tb, NULL, stress_thread, &b );
            for (int i = 0; i < 20; i++)
            {
                s[0].z[0] = s[2].z[0] = (i & 1) ? 3 : 1;
                ps5c_scene_update( comp, s, 3 );
            }
            pthread_join( ta, NULL );
            pthread_join( tb, NULL );
            /* Frame 40 is each thread's paints[0]. */
            ow[0].paint = &red;
            verify( comp, &out, ow, 3, "multi-window-after-concurrent-producers" );
        }

        ps5c_chain_destroy( w1.chain );
        ps5c_chain_destroy( w2.chain );
        ps5c_chain_destroy( w3.chain );
        ps5c_scene_update( comp, NULL, 0 );
        CHECK( ps5c_compositor_flush( comp ) );
        ps5c_get_stats( comp, &stats );
        assert( stats.live_chains == 0 && stats.host_allocations == 0 && stats.live_images == 0 );
    }

    /* 3. Resize and destroy lifetime: the old chain keeps the window's content
     *    until the replacement presents; its memory goes afterwards. */
    {
        static struct producer old_chain, new_chain;
        struct paint red = solid( 0, 0, 255 ), white = solid( 255, 255, 255 );
        struct ps5c_window_state s = window( 0x500, 0, 0, 96, 54, 1, 1, 1, 0 );
        struct oracle_window ow[] = {{&s, &red, 96, 54}};

        create_producer( comp, &old_chain, &producer_b, 0x500, 96, 54, VK_FORMAT_B8G8R8A8_UNORM );
        ps5c_scene_update( comp, &s, 1 );
        present_frame( &old_chain, &red );
        verify( comp, &out, ow, 1, "resize-before" );

        /* The window grows; the app makes a new chain then destroys the old. */
        s = window( 0x500, 0, 0, 192, 108, 1, 1, 1, 0 );
        ps5c_scene_update( comp, &s, 1 );
        create_producer( comp, &new_chain, &producer_b, 0x500, 192, 108, VK_FORMAT_B8G8R8A8_UNORM );
        ps5c_chain_destroy( old_chain.chain );
        verify( comp, &out, ow, 1, "resize-old-content-retained" );
        ps5c_get_stats( comp, &stats );
        /* Only the old chain's displayed image survives its destroy. */
        assert( stats.live_chains == 2 && stats.host_allocations == 3 + 1 );

        present_frame( &new_chain, &white );
        ow[0] = (struct oracle_window){&s, &white, 192, 108};
        verify( comp, &out, ow, 1, "resize-new-chain" );
        ps5c_get_stats( comp, &stats );
        assert( stats.live_chains == 1 && stats.host_allocations == 3 && stats.last_dispatches == 1 );

        ps5c_chain_destroy( new_chain.chain );
        ps5c_scene_update( comp, NULL, 0 );
        verify( comp, &out, ow, 0, "destroyed-window-cleared" );
        ps5c_get_stats( comp, &stats );
        assert( stats.live_chains == 0 && stats.host_allocations == 0 && stats.host_bytes == 0 &&
                stats.live_images == 0 );
    }

    /* 4. RuneLite shape: a frame painted only through GDI (sidebar, title area)
     *    as a CPU layer whose surface includes the non-client frame, with the
     *    game canvas child on its own swapchain above it. The view is the
     *    frame's client area; the non-client part is never shown. */
    {
        static struct producer canvas;
        static uint8_t bits[120 * 70 * 4];
        struct paint frame_paint = {{{0, 255, 255, 255}, {255, 0, 255, 255}, {0, 255, 0, 255}, {255, 255, 0, 255}}};
        struct paint frame_paint2 = solid( 255, 255, 255 ), canvas_paint = solid( 0, 0, 255 );
        struct ps5c_window_state s[2];
        struct oracle_window ow[2] = {{&s[0], &frame_paint, 120, 70}, {&s[1], &canvas_paint, 64, 40}};
        struct ps5c_chain *gdi;

        /* Frame surface 120x70 at 500,300; client 104x52 at 508,314; canvas 64x40 at 510,316. */
        s[0] = window( 0x600 | (1ull << 63), 500, 300, 620, 370, 1, 1, 4, 0 );
        s[0].clip = (struct ps5c_rect){508, 314, 612, 366};
        s[0].layer = 0;
        s[1] = window( 0x700, 510, 316, 574, 356, 0, 2, 4, 0 );
        s[1].clip = s[1].client;
        s[1].layer = 1;
        compositor_dev.created_images = 0;
        CHECK( ps5c_chain_create_cpu( comp, s[0].key, 120, 70, &gdi ) );
        create_producer( comp, &canvas, &producer_a, 0x700, 64, 40, VK_FORMAT_B8G8R8A8_UNORM );
        assert( compositor_dev.created_images == 3 + 3 ); /* GDI images + shared canvas images only */
        ps5c_scene_update( comp, s, 2 );
        for (int y = 0; y < 70; y++)
            for (int x = 0; x < 120; x++)
            {
                memcpy( bits + (y * 120 + x) * 4, frame_paint.quad[(y >= 35) * 2 + (x >= 60)], 4 );
                bits[(y * 120 + x) * 4 + 3] = 0; /* GDI's X byte is ignored */
            }
        CHECK( ps5c_chain_upload( gdi, bits, 120 * 4 ) );
        present_frame( &canvas, &canvas_paint );
        verify( comp, &out, ow, 2, "gdi-frame-with-canvas" );
        assert( last_view.desktop.left == 508 && last_view.desktop.right == 612 &&
                last_view.desktop.top == 314 && last_view.desktop.bottom == 366 );
        ps5c_get_stats( comp, &stats );
        assert( stats.last_dispatches == 2 );

        /* Repaint through GDI several times: each upload replaces the frame. */
        for (int i = 0; i < 5; i++)
        {
            memset( bits, i & 1 ? 0 : 255, sizeof(bits) );
            CHECK( ps5c_chain_upload( gdi, bits, 120 * 4 ) );
        }
        ow[0].paint = &frame_paint2;
        verify( comp, &out, ow, 2, "gdi-frame-repainted" );

        ps5c_chain_destroy( canvas.chain );
        ps5c_chain_destroy( gdi );
        ps5c_scene_update( comp, NULL, 0 );
        verify( comp, &out, ow, 0, "gdi-frame-destroyed" );
        ps5c_get_stats( comp, &stats );
        assert( stats.live_chains == 0 && stats.host_allocations == 0 && stats.live_images == 0 );
    }

    ps5c_get_stats( comp, &stats );
    printf( "composes=%llu dispatches=%llu clears=%llu skipped=%llu view_changes=%u output_acquires=%u presents=%u\n",
            (unsigned long long)stats.composes, (unsigned long long)stats.dispatches,
            (unsigned long long)stats.clears, (unsigned long long)stats.skipped, view_changes, out.acquires,
            out.presents );
    ps5c_compositor_destroy( comp );
    for (int i = 0; i < 3; i++)
    {
        vkDestroyImage( compositor_dev.device, out.images[i], NULL );
        vkFreeMemory( compositor_dev.device, out.memory[i], NULL );
    }
    for (unsigned i = 0; i < device_count; i++)
    {
        vkDestroyCommandPool( devices[i]->device, devices[i]->pool, NULL );
        vkDestroyDevice( devices[i]->device, NULL );
    }
    vkDestroyInstance( instance, NULL );
    printf( "TITLE_COMPOSITOR_VULKAN_PASS scope=HOST_LAVAPIPE checked=%u ambiguous_edges=%u\n",
            total_checked, total_ambiguous );
    return 0;
}
