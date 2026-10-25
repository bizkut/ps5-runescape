/* Host-only real Vulkan readback of the shared Wine blit, not PS5 qualification. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include <wine/ps5_viewport.h>
#include "win32u-blit-array.inc"

#define VK_CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s failed: %d\n", #call, r);exit(1); } } while (0)
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties memory_properties;
static uint32_t family;
static VkCommandPool command_pool;
static VkDescriptorSetLayout descriptor_layout;
static VkPipelineLayout pipeline_layout;
static VkPipeline pipeline;
static VkSampler sampler;

static VkDeviceMemory memory_for(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags)
{
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) { type = i;break; }
    assert(type != UINT32_MAX);
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type};
    VkDeviceMemory memory;VK_CHECK(vkAllocateMemory(device, &info, NULL, &memory));return memory;
}
static VkImage make_image(uint32_t width, uint32_t height, VkImageUsageFlags usage, VkDeviceMemory *memory)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {width,height,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VkImage image;VK_CHECK(vkCreateImage(device, &info, NULL, &image));
    VkMemoryRequirements requirements;vkGetImageMemoryRequirements(device, image, &requirements);
    *memory = memory_for(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkBindImageMemory(device, image, *memory, 0));return image;
}
static VkImageView make_view(VkImage image, VkFormat format)
{
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view;VK_CHECK(vkCreateImageView(device, &info, NULL, &view));return view;
}
static void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout old, VkImageLayout next,
                       VkAccessFlags src_access, VkAccessFlags dst_access,
                       VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old, .newLayout = next, .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}
static void check_case(uint32_t width, uint32_t height)
{
    const uint32_t output_width = 192, output_height = 108;
    struct wine_ps5_viewport viewport;
    assert(wine_ps5_viewport_fit(&viewport, width, height, output_width, output_height));
    VkDeviceMemory source_memory, target_memory;
    VkImage source = make_image(width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &source_memory);
    VkImage target = make_image(output_width, output_height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &target_memory);
    VkImageView source_view = make_view(source, VK_FORMAT_B8G8R8A8_SRGB);
    VkImageView target_view = make_view(target, VK_FORMAT_B8G8R8A8_UNORM);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = output_width * output_height * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;VK_CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkDeviceMemory buffer_memory = memory_for(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    VkDescriptorPool pool;VK_CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &descriptor_layout};
    VkDescriptorSet set;VK_CHECK(vkAllocateDescriptorSets(device, &set_info, &set));
    VkDescriptorImageInfo images[] = {{sampler,source_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {VK_NULL_HANDLE,target_view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &images[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &images[1]}};
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;VK_CHECK(vkAllocateCommandBuffers(device, &allocate, &cmd));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    transition(cmd, source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue red = {.float32 = {1,0,0,1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
    float constants[] = {viewport.left - .5f, viewport.top - .5f, viewport.width, viewport.height};
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
    vkCmdDispatch(cmd, (output_width + 7) / 8, (output_height + 7) / 8, 1);
    transition(cmd, target, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}, .imageExtent = {output_width,output_height,1}};
    vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));VK_CHECK(vkQueueWaitIdle(queue));
    uint32_t *pixels;VK_CHECK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));
    for (uint32_t y = 0; y < output_height; y++) for (uint32_t x = 0; x < output_width; x++) {
        int inside = x >= viewport.left && x < viewport.left + viewport.width &&
                     y >= viewport.top && y < viewport.top + viewport.height;
        uint32_t expected = inside ? 0xffff0000u : 0xff000000u;
        if (pixels[y * output_width + x] != expected) {
            fprintf(stderr, "pixel %u,%u case %ux%u actual=%08x expected=%08x\n",
                    x, y, width, height, pixels[y * output_width + x], expected);exit(1);
        }
    }
    vkUnmapMemory(device, buffer_memory);
    printf("VIEWPORT_VULKAN_PIXELS image=%ux%u output=%ux%u fit=%u,%u %ux%u checked=%u PASS\n",
           width,height,output_width,output_height,viewport.left,viewport.top,viewport.width,viewport.height,output_width*output_height);
    vkFreeCommandBuffers(device, command_pool, 1, &cmd);vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);vkFreeMemory(device, buffer_memory, NULL);
    vkDestroyImageView(device, source_view, NULL);vkDestroyImageView(device, target_view, NULL);
    vkDestroyImage(device, source, NULL);vkDestroyImage(device, target, NULL);
    vkFreeMemory(device, source_memory, NULL);vkFreeMemory(device, target_memory, NULL);
}
int main(void)
{
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    VkInstance instance;VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    VkPhysicalDevice devices[8];uint32_t count = 8;VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDeviceProperties properties;
    for (uint32_t i = 0; i < count; i++) {
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { physical = devices[i];break; }
    }
    assert(physical);vkGetPhysicalDeviceProperties(physical, &properties);
    printf("VIEWPORT_VULKAN_DEVICE name=%s api=%u.%u.%u scope=HOST_ONLY\n", properties.deviceName,
           VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion));
    VkQueueFamilyProperties queues[32];count = 32;vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
    family = UINT32_MAX;for (uint32_t i = 0; i < count; i++) if (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i;break; }
    assert(family != UINT32_MAX);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(physical, &features);assert(features.shaderStorageImageWriteWithoutFormat);
    features = (VkPhysicalDeviceFeatures){.shaderStorageImageWriteWithoutFormat = VK_TRUE};
    float priority = 1;VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info, .pEnabledFeatures = &features};
    VK_CHECK(vkCreateDevice(physical, &device_info, NULL, &device));vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VK_CHECK(vkCreateCommandPool(device, &commands, NULL, &command_pool));
    VkDescriptorSetLayoutBinding bindings[] = {{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
                                              {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VK_CHECK(vkCreateDescriptorSetLayout(device, &descriptors, NULL, &descriptor_layout));
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT,0,16};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &descriptor_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK_CHECK(vkCreatePipelineLayout(device, &layout, NULL, &pipeline_layout));
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(blit_comp_spv), .pCode = blit_comp_spv};
    VkShaderModule shader;VK_CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader));
    VkBool32 enabled = VK_TRUE;VkSpecializationMapEntry entry = {0,0,sizeof(enabled)};
    VkSpecializationInfo specialization = {1,&entry,sizeof(enabled),&enabled};
    VkComputePipelineCreateInfo compute = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = shader, .pName = "main", .pSpecializationInfo = &specialization}, .layout = pipeline_layout};
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, NULL, &pipeline));
    VkSamplerCreateInfo sample = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VK_CHECK(vkCreateSampler(device, &sample, NULL, &sampler));
    check_case(3,2);check_case(2,1);check_case(1,2);check_case(1,1);check_case(765,503);check_case(1920,1080);
    vkDestroySampler(device, sampler, NULL);vkDestroyPipeline(device, pipeline, NULL);vkDestroyShaderModule(device, shader, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);vkDestroyDevice(device, NULL);vkDestroyInstance(instance, NULL);
    return 0;
}
