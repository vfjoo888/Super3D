// Post-process anti-aliasing on the game's final image before it is presented:
//   mode 0: Off
//   mode 1: FXAA (1 compute pass: runtime/shaders/post/fxaa.comp)
//   mode 2: SMAA 1x (3 compute passes: smaa_edge.comp -> smaa_weight.comp -> smaa_blend.comp)
// Output is an RGBA8 image of the game image's own size, which the present then upscales (FSR 1, nvn_fsr.cpp)
// or blits instead of the game's image. NX_AA=0|1|2 (or off|fxaa|smaa) or NX_FXAA=0|1 overrides the setting.
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "nvn_vk.h"
#include "post_spv.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

std::atomic<int> g_mode{0};  // 0 Off, 1 FXAA, 2 SMAA
VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;        // 2 bindings: sampler + storage
VkDescriptorSetLayout g_blend_set_layout = VK_NULL_HANDLE;  // 3 bindings: sampler + sampler + storage
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkPipelineLayout g_blend_layout = VK_NULL_HANDLE;
VkPipeline g_fxaa_pipe = VK_NULL_HANDLE;
VkPipeline g_smaa_edge_pipe = VK_NULL_HANDLE;
VkPipeline g_smaa_weight_pipe = VK_NULL_HANDLE;
VkPipeline g_smaa_blend_pipe = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
VkDescriptorPool g_pool = VK_NULL_HANDLE;
constexpr uint32_t kRing = 16;  // sets rewritten one per present (as nvn_fsr.cpp's)
VkDescriptorSet g_sets[kRing] = {};
VkDescriptorSet g_weight_sets[kRing] = {};
VkDescriptorSet g_blend_sets[kRing] = {};
uint32_t g_next = 0;
GpuImage g_out;
GpuImage g_edges;
GpuImage g_weights;
bool g_failed = false;

int forced_mode() {
    static const int f = [] {
        if (const char* e = std::getenv("NX_AA"); e && *e) {
            if (e[0] == '0' || std::strcmp(e, "off") == 0) return 0;
            if (e[0] == '1' || std::strcmp(e, "fxaa") == 0) return 1;
            if (e[0] == '2' || std::strcmp(e, "smaa") == 0) return 2;
        }
        if (const char* e = std::getenv("NX_FXAA"); e && *e)
            return e[0] == '0' ? 0 : 1;
        return -1;
    }();
    return f;
}

bool make_compute_pipe(VkDevice d, VkPipelineLayout layout, const uint32_t* code, size_t size, VkPipeline& out) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = size;
    mi.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule(d, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    bool ok = p_vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &out) == VK_SUCCESS;
    p_vkDestroyShaderModule(d, module, nullptr);
    return ok;
}

bool init() {
    if (g_fxaa_pipe) return true;
    if (g_failed) return false;
    VkDevice d = vk_device();
    VkDescriptorSetLayoutBinding b2[2]{};
    b2[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b2[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li2.bindingCount = 2;
    li2.pBindings = b2;

    VkDescriptorSetLayoutBinding b3[3]{};
    b3[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b3[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b3[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li3{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li3.bindingCount = 3;
    li3.pBindings = b3;

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl2{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl2.setLayoutCount = 1;
    pl2.pSetLayouts = &g_set_layout;
    pl2.pushConstantRangeCount = 1;
    pl2.pPushConstantRanges = &range;

    VkPipelineLayoutCreateInfo pl3{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl3.setLayoutCount = 1;
    pl3.pSetLayouts = &g_blend_set_layout;
    pl3.pushConstantRangeCount = 1;
    pl3.pPushConstantRanges = &range;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRing * 4},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kRing * 3}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kRing * 3;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;

    bool ok = p_vkCreateDescriptorSetLayout(d, &li2, nullptr, &g_set_layout) == VK_SUCCESS &&
              p_vkCreateDescriptorSetLayout(d, &li3, nullptr, &g_blend_set_layout) == VK_SUCCESS &&
              p_vkCreatePipelineLayout(d, &pl2, nullptr, &g_layout) == VK_SUCCESS &&
              p_vkCreatePipelineLayout(d, &pl3, nullptr, &g_blend_layout) == VK_SUCCESS &&
              p_vkCreateSampler(d, &si, nullptr, &g_sampler) == VK_SUCCESS &&
              p_vkCreateDescriptorPool(d, &pi, nullptr, &g_pool) == VK_SUCCESS;
    if (ok) {
        ok = make_compute_pipe(d, g_layout, k_fxaa_comp, sizeof k_fxaa_comp, g_fxaa_pipe) &&
             make_compute_pipe(d, g_layout, k_smaa_edge_comp, sizeof k_smaa_edge_comp, g_smaa_edge_pipe) &&
             make_compute_pipe(d, g_layout, k_smaa_weight_comp, sizeof k_smaa_weight_comp, g_smaa_weight_pipe) &&
             make_compute_pipe(d, g_blend_layout, k_smaa_blend_comp, sizeof k_smaa_blend_comp, g_smaa_blend_pipe);
        VkDescriptorSetLayout layouts2[kRing], layouts3[kRing];
        for (uint32_t i = 0; i < kRing; i++) layouts2[i] = g_set_layout, layouts3[i] = g_blend_set_layout;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_pool;
        ai.descriptorSetCount = kRing;
        ai.pSetLayouts = layouts2;
        ok = ok && p_vkAllocateDescriptorSets(d, &ai, g_sets) == VK_SUCCESS;
        ok = ok && p_vkAllocateDescriptorSets(d, &ai, g_weight_sets) == VK_SUCCESS;
        ai.pSetLayouts = layouts3;
        ok = ok && p_vkAllocateDescriptorSets(d, &ai, g_blend_sets) == VK_SUCCESS;
    }
    if (!ok) {
        log("nvn/vk: AA unavailable (pipeline creation failed)");
        g_failed = true;
        return false;
    }
    log("nvn/vk: FXAA + SMAA ready");
    return true;
}

void drop_img(GpuImage& im) {
    VkDevice d = vk_device();
    if (im.view) p_vkDestroyImageView(d, im.view, nullptr);
    if (im.image) p_vkDestroyImage(d, im.image, nullptr);
    if (im.memory) p_vkFreeMemory(d, im.memory, nullptr);
    im = GpuImage{};
}

void drop_output() {
    drop_img(g_out);
    drop_img(g_edges);
    drop_img(g_weights);
}

bool make_rgba8(GpuImage& dst, uint32_t w, uint32_t h) {
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &dst.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, dst.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &dst.memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, dst.image, dst.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = dst.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &dst.view) != VK_SUCCESS) return false;
    dst.format = ci.format;
    dst.width = w, dst.height = h;
    dst.mutable_format = false;
    return true;
}

bool ensure_output(uint32_t w, uint32_t h, bool need_smaa) {
    const bool out_ok = g_out.image && g_out.width == w && g_out.height == h;
    const bool smaa_ok = !need_smaa || (g_edges.image && g_edges.width == w && g_edges.height == h && g_weights.image);
    if (out_ok && smaa_ok) return true;
    if (g_out.image || g_edges.image) p_vkDeviceWaitIdle(vk_device());
    if (!out_ok) drop_output();
    if (!g_out.image && !make_rgba8(g_out, w, h)) return false;
    if (need_smaa) {
        if (!g_edges.image && !make_rgba8(g_edges, w, h)) return false;
        if (!g_weights.image && !make_rgba8(g_weights, w, h)) return false;
    }
    log("nvn/vk: %s output %ux%u", need_smaa ? "SMAA" : "FXAA", w, h);
    return true;
}

void write_set2(VkDescriptorSet set, VkImageView src_view, VkImageView dst_view) {
    VkDescriptorImageInfo in{g_sampler, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out{VK_NULL_HANDLE, dst_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = w[1].dstSet = set;
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &in;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &out;
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);
}

void write_set3(VkDescriptorSet set, VkImageView src0, VkImageView src1, VkImageView dst_view) {
    VkDescriptorImageInfo in0{g_sampler, src0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo in1{g_sampler, src1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo out{VK_NULL_HANDLE, dst_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = uint32_t(i);
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[0].pImageInfo = &in0;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[1].pImageInfo = &in1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[2].pImageInfo = &out;
    p_vkUpdateDescriptorSets(vk_device(), 3, w, 0, nullptr);
}

}  // namespace

void set_fxaa(bool on) { g_mode.store(on ? 1 : 0, std::memory_order_relaxed); }

void set_aa_mode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    g_mode.store(mode, std::memory_order_relaxed);
}

GpuImage* fxaa_record(VkCommandBuffer cb, GpuImage& src) {
    const int mode = forced_mode() >= 0 ? forced_mode() : g_mode.load(std::memory_order_relaxed);
    if (mode == 0) return nullptr;
    // RGBA8 sources only (the game's final image); sRGB ones are left alone rather than filtered in the wrong space.
    if (src.format != VK_FORMAT_R8G8B8A8_UNORM && src.format != VK_FORMAT_B8G8R8A8_UNORM) return nullptr;
    const bool smaa = mode == 2;
    if (!init() || !ensure_output(src.width, src.height, smaa)) return nullptr;

    const uint32_t slot = g_next++ % kRing;
    const float inv[2] = {1.0f / float(src.width), 1.0f / float(src.height)};
    const uint32_t gx = (src.width + 7) / 8, gy = (src.height + 7) / 8;

    if (!smaa) {
        VkDescriptorSet set = g_sets[slot];
        write_set2(set, src.view, g_out.view);
        transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cb, g_out, VK_IMAGE_LAYOUT_GENERAL);
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_fxaa_pipe);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &set, 0, nullptr);
        p_vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof inv, inv);
        p_vkCmdDispatch(cb, gx, gy, 1);
    } else {
        VkDescriptorSet s_edge = g_sets[slot];
        VkDescriptorSet s_weight = g_weight_sets[slot];
        VkDescriptorSet s_blend = g_blend_sets[slot];
        write_set2(s_edge, src.view, g_edges.view);
        write_set2(s_weight, g_edges.view, g_weights.view);
        write_set3(s_blend, src.view, g_weights.view, g_out.view);

        // Pass 1: Edge detection (src -> g_edges)
        transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cb, g_edges, VK_IMAGE_LAYOUT_GENERAL);
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_smaa_edge_pipe);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &s_edge, 0, nullptr);
        p_vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof inv, inv);
        p_vkCmdDispatch(cb, gx, gy, 1);

        // Pass 2: Blending weights (g_edges -> g_weights)
        transition(cb, g_edges, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cb, g_weights, VK_IMAGE_LAYOUT_GENERAL);
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_smaa_weight_pipe);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_layout, 0, 1, &s_weight, 0, nullptr);
        p_vkCmdPushConstants(cb, g_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof inv, inv);
        p_vkCmdDispatch(cb, gx, gy, 1);

        // Pass 3: Neighborhood blending (src + g_weights -> g_out)
        transition(cb, g_weights, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cb, g_out, VK_IMAGE_LAYOUT_GENERAL);
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_smaa_blend_pipe);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_blend_layout, 0, 1, &s_blend, 0, nullptr);
        p_vkCmdPushConstants(cb, g_blend_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof inv, inv);
        p_vkCmdDispatch(cb, gx, gy, 1);
    }

    // Read next by FSR (sampled) or the blit (transfer): the transition there orders it.
    g_out.logical_width = src.logical_width, g_out.logical_height = src.logical_height;
    g_out.uw = src.uw;
    static int logged_mode = 0;
    if (logged_mode != mode) {
        logged_mode = mode;
        log("nvn/vk: %s on the presented image (%ux%u)", smaa ? "SMAA" : "FXAA", src.width, src.height);
    }
    return &g_out;
}

void fxaa_release() {
    if (!g_out.image && !g_edges.image && !g_weights.image) return;
    p_vkDeviceWaitIdle(vk_device());
    drop_output();
}

}  // namespace nx::nvn
