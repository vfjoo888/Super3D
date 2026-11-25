// HDR output: an scRGB swapchain (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, RGBA16F) behind a last pass (docs/HDR.md).
//
// The game's window textures are made RGBA16F in this mode (nvn_image.cpp), so values above 1.0 survive. The only
// source of those is the composite replacement (<data root>/spirv-renodx/, tools/renodx_wonder.py): where the game's
// tone curve would clip to white it writes how far past white the scene was. The blit then writes the window texture
// into an image of the swapchain's size (hdr_source) instead of into the swapchain, the F1 overlay is drawn on top of
// it, and this pass puts it on the swapchain: the SDR picture (and UI) at paper white, the highlights from the
// player's Highlights / Highlight start settings, rolled off into the display's peak (runtime/shaders/hdr_present.frag).
//
// Settings: hdr (read once, when the Vulkan instance is made: a restart applies it), hdr_peak, hdr_paper_white,
// hdr_highlights, hdr_highlight_start in pc-settings.ini; the last four apply live (read every frame).
// NX_HDR=0|1, NX_HDR_PEAK, NX_HDR_PAPER_WHITE, NX_HDR_HIGHLIGHTS, NX_HDR_HIGHLIGHT_START override them.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nvn_vk.h"
#include "overlay/overlay.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

const uint32_t kVertex[] = {
#include "hdr_present.vert.inc"
};
const uint32_t kFragment[] = {
#include "hdr_present.frag.inc"
};

// Encoded units per multiple of the white point above 1.0: tools/renodx_wonder.py --scale (its default) writes them.
constexpr float kEncodeScale = 0.25f;
// Highlights 100%: the display-linear light (1.0 = paper white) added at twice the white point's scene brightness
// with the start at the white point (hdr_present.frag: gain * t^2).
constexpr float kGainAt100 = 2.0f;

std::atomic<int> g_setting_on{0}, g_setting_peak{1000}, g_setting_paper{203}, g_setting_highlights{100},
    g_setting_start{100};
std::atomic<int> g_setting_eotf{1};  // set_hdr_sdr_eotf: 0 none, 1 gamma 2.2, 2 gamma 2.4

// Scene grading (set_hdr_grading): all in percent, 100 neutral.
std::atomic<int> g_setting_exposure{100}, g_setting_contrast{100}, g_setting_shadows{100}, g_setting_saturation{100},
    g_setting_highlight_saturation{100};

struct Pass {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImage> overlay_images;  // the source once per swapchain image (overlay::record's frame ring)
    VkExtent2D extent{};
    GpuImage source;  // what the blit writes and the overlay draws on: the swapchain's size, RGBA16F
    bool failed = false;
} g;

VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m);
    return m;
}

// NX_HDR_* overrides: read once, -1 when not set.
int env_int(const char* name) {
    const char* e = std::getenv(name);
    return e && e[0] ? std::atoi(e) : -1;
}
int env_or(int env, const std::atomic<int>& setting) { return env >= 0 ? env : setting.load(std::memory_order_relaxed); }
int peak_nits() {
    static const int env = env_int("NX_HDR_PEAK");
    return std::clamp(env_or(env, g_setting_peak), 100, 10000);
}
int paper_nits() {
    static const int env = env_int("NX_HDR_PAPER_WHITE");
    return std::clamp(env_or(env, g_setting_paper), 80, 1000);
}
int highlights_percent() {
    static const int env = env_int("NX_HDR_HIGHLIGHTS");
    return std::clamp(env_or(env, g_setting_highlights), 0, 500);
}
int start_percent() {
    static const int env = env_int("NX_HDR_HIGHLIGHT_START");
    return std::clamp(env_or(env, g_setting_start), 10, 1000);
}
// The grading overrides may be negative (exposure): INT_MIN when not set.
int env_signed(const char* name) {
    const char* e = std::getenv(name);
    return e && e[0] ? std::atoi(e) : INT_MIN;
}
int env_or_signed(int env, const std::atomic<int>& setting) {
    return env != INT_MIN ? env : setting.load(std::memory_order_relaxed);
}
int exposure_percent() {
    static const int env = env_signed("NX_HDR_EXPOSURE");
    return std::clamp(env_or_signed(env, g_setting_exposure), 25, 300);
}
int contrast_percent() {
    static const int env = env_signed("NX_HDR_CONTRAST");
    return std::clamp(env_or_signed(env, g_setting_contrast), 25, 200);
}
int shadows_percent() {
    static const int env = env_signed("NX_HDR_SHADOWS");
    return std::clamp(env_or_signed(env, g_setting_shadows), 25, 175);
}
int saturation_percent() {
    static const int env = env_signed("NX_HDR_SATURATION");
    return std::clamp(env_or_signed(env, g_setting_saturation), 0, 300);
}
int highlight_saturation_percent() {
    static const int env = env_signed("NX_HDR_HIGHLIGHT_SATURATION");
    return std::clamp(env_or_signed(env, g_setting_highlight_saturation), 0, 300);
}

void destroy_targets() {
    VkDevice d = vk_device();
    for (VkFramebuffer f : g.framebuffers) p_vkDestroyFramebuffer(d, f, nullptr);
    for (VkImageView v : g.views) p_vkDestroyImageView(d, v, nullptr);
    g.framebuffers.clear(), g.views.clear(), g.overlay_images.clear();
    if (g.source.view) p_vkDestroyImageView(d, g.source.view, nullptr);
    if (g.source.image) p_vkDestroyImage(d, g.source.image, nullptr);
    if (g.source.memory) p_vkFreeMemory(d, g.source.memory, nullptr);
    g.source = GpuImage{};
    g.extent = {};
}

bool make_pipeline(VkFormat format) {
    VkDevice d = vk_device();
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1, rpi.pAttachments = &color;
    rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1, rpi.pDependencies = &dep;
    if (p_vkCreateRenderPass(d, &rpi, nullptr, &g.render_pass) != VK_SUCCESS) return false;

    if (!g.sampler) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;  // 1:1 with the swapchain
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (p_vkCreateSampler(d, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    }
    if (!g.set_layout) {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                       &g.sampler};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 1, dli.pBindings = &b;
        if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    }
    if (!g.layout) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1, pli.pSetLayouts = &g.set_layout;
        pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
        if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
    }

    VkShaderModule vs = module(kVertex, sizeof kVertex), fs = module(kFragment, sizeof kFragment);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2, ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2, gp.pStages = stages;
    gp.pVertexInputState = &vin, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp;
    gp.pRasterizationState = &rs, gp.pMultisampleState = &ms, gp.pColorBlendState = &cb, gp.pDynamicState = &ds;
    gp.layout = g.layout, gp.renderPass = g.render_pass;
    VkResult r = vs && fs ? p_vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &g.pipeline)
                          : VK_ERROR_INITIALIZATION_FAILED;
    if (vs) p_vkDestroyShaderModule(d, vs, nullptr);
    if (fs) p_vkDestroyShaderModule(d, fs, nullptr);
    g.format = format;
    return r == VK_SUCCESS;
}

bool make_source(VkExtent2D extent) {
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R16G16B16A16_SFLOAT, ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_DST: the blit and the clear; COLOR_ATTACHMENT: the overlay; SAMPLED: this pass.
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &g.source.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, g.source.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || p_vkAllocateMemory(d, &ai, nullptr, &g.source.memory) != VK_SUCCESS)
        return false;
    if (p_vkBindImageMemory(d, g.source.image, g.source.memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g.source.image, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &g.source.view) != VK_SUCCESS) return false;
    g.source.format = ci.format, g.source.width = extent.width, g.source.height = extent.height;
    g.source.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g.source.mutable_format = false;
    return true;
}

}  // namespace

void set_hdr_sdr_eotf(int mode) { g_setting_eotf.store(mode < 0 || mode > 2 ? 1 : mode); }

void set_hdr(bool on, int peak_nits, int paper_white_nits, int highlights_percent, int highlight_start_percent) {
    g_setting_on.store(on ? 1 : 0), g_setting_peak.store(peak_nits), g_setting_paper.store(paper_white_nits);
    g_setting_highlights.store(highlights_percent), g_setting_start.store(highlight_start_percent);
}

void set_hdr_grading(int exposure_percent, int contrast_percent, int shadows_percent, int saturation_percent,
                     int highlight_saturation_percent) {
    g_setting_exposure.store(exposure_percent), g_setting_contrast.store(contrast_percent);
    g_setting_shadows.store(shadows_percent), g_setting_saturation.store(saturation_percent);
    g_setting_highlight_saturation.store(highlight_saturation_percent);
}

// The scene grading the composite replacement reads (tools/renodx_wonder.py, hdr_grade): two vec4 in the support
// buffer's transform feedback slots (0x550, 0x560), which no translated Wonder shader reads (Wonder has no transform
// feedback; a scan of every spirv/*.spv finds reads of frag_scale_count and render_scale only). The replacement is the
// only program that looks at them, and it runs only for the 3D scene: the HUD, the menus and the 2D art are drawn
// after it (or without it) and are not graded.
//   a: x 1 = grading on (0: the replacement skips it, the game's picture bit for bit), y exposure multiplier,
//      z contrast exponent, w shadows exponent        b: x saturation (OKLab chroma scale), yzw 0
// Neutral settings leave all eight zero: the support buffer is then byte for byte what it was without grading.
bool hdr_grading(float out[8]) {
    if (!hdr_output()) return false;
    const int exposure = exposure_percent(), contrast = contrast_percent(), shadows = shadows_percent(),
              saturation = saturation_percent(), highlight_saturation = highlight_saturation_percent();
    if (exposure == 100 && contrast == 100 && shadows == 100 && saturation == 100 && highlight_saturation == 100)
        return false;
    out[0] = 1.0f;
    out[1] = float(exposure) / 100.0f;
    out[2] = float(contrast) / 100.0f;
    out[3] = 2.0f - float(shadows) / 100.0f;  // RenoDX: pow(y / mid, 2 - shadows) below mid grey; 1.0 neutral
    out[4] = float(saturation) / 100.0f;
    out[5] = float(highlight_saturation) / 100.0f;
    out[6] = out[7] = 0.0f;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) == 0)
        log("nvn/hdr: scene grading on: exposure %d%%, contrast %d%%, shadows %d%%, saturation %d%%, highlight saturation %d%%",
            exposure, contrast, shadows, saturation, highlight_saturation);
    return true;
}

// Latched at the first call (when the Vulkan instance is made, init in nvn_vk.cpp): the instance extension, the
// surface format and the window textures' format all follow it, so a change in the menu waits for a restart.
bool hdr_wanted() {
    static const bool on = [] {
        const char* e = std::getenv("NX_HDR");
        if (e && e[0]) return e[0] != '0';
        return g_setting_on.load() != 0;
    }();
    return on;
}

bool hdr_active() { return g.pipeline && g.source.image && !g.failed; }

std::string hdr_spirv_override(const std::string& hash) {
    if (!hdr_output()) return {};
    const std::string path = plat::data_root() + "/spirv-renodx/" + hash + ".spv";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return {};
    std::fclose(f);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 16) log("nvn/hdr: shader %s: HDR replacement", hash.c_str());
    return path;
}

bool hdr_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent) {
    if (g.failed) return false;
    destroy_targets();
    if (g.format != format && g.pipeline) {
        VkDevice d = vk_device();
        p_vkDestroyPipeline(d, g.pipeline, nullptr);
        p_vkDestroyRenderPass(d, g.render_pass, nullptr);
        g.pipeline = VK_NULL_HANDLE, g.render_pass = VK_NULL_HANDLE;
    }
    if (!g.pipeline && !make_pipeline(format)) {
        log("nvn/hdr: the pass could not be made");
        g.failed = true;
        return false;
    }
    if (!make_source(extent)) {
        log("nvn/hdr: the %ux%u source image could not be made", extent.width, extent.height);
        destroy_targets();
        g.failed = true;
        return false;
    }
    VkDevice d = vk_device();
    for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (p_vkCreateImageView(d, &vi, nullptr, &v) == VK_SUCCESS) {
            g.views.push_back(v);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = g.render_pass, fi.attachmentCount = 1, fi.pAttachments = &v;
            fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
            if (p_vkCreateFramebuffer(d, &fi, nullptr, &fb) != VK_SUCCESS) fb = VK_NULL_HANDLE;
        }
        if (!fb) {
            log("nvn/hdr: no framebuffer for a swapchain image");
            destroy_targets();
            g.failed = true;
            return false;
        }
        g.framebuffers.push_back(fb);
    }
    g.overlay_images.assign(images.size(), g.source.image);
    g.extent = extent;
    log("nvn/hdr: HDR output on: %ux%u, peak %d nits, paper white %d nits, highlights %d%%, highlight start %d%%",
        extent.width, extent.height, peak_nits(), paper_nits(), highlights_percent(), start_percent());
    return true;
}

GpuImage* hdr_source() { return hdr_active() ? &g.source : nullptr; }

void hdr_release() { destroy_targets(); }  // before the swapchain whose images they view is destroyed

void hdr_clear_source(VkCommandBuffer cb, float r, float gr, float b) {
    if (!hdr_active()) return;
    transition(cb, g.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue v{{r, gr, b, 1.0f}};
    VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdClearColorImage(cb, g.source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
}

bool hdr_record(VkCommandBuffer cb, uint32_t index) {
    if (!hdr_active() || index >= g.framebuffers.size()) return false;
    // The overlay first, on the source: in the same linear SDR range as the game's UI, so it comes out at
    // paper white like the rest of the SDR picture. Its render pass takes and leaves the image PRESENT_SRC.
    transition(cb, g.source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    overlay::record(cb, index, g.overlay_images, g.source.format, g.extent);
    transition(cb, g.source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorSet set = descriptor_set(g.set_layout);
    if (!set) return false;
    VkDescriptorImageInfo ii{g.sampler, g.source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = &ii;
    p_vkUpdateDescriptorSets(vk_device(), 1, &w, 0, nullptr);

    const float peak = float(peak_nits()), paper = float(paper_nits());
    // SDR EOTF emulation: what a real SDR display makes of the game's picture is gamma(sRGB-encode(light)): the GPU
    // encodes with the sRGB curve on write, a display decodes with a plain gamma power. Without it the linear light is
    // shown as is, which lifts the shadows and looks flatter.
    static const int eotf_env = [] { const char* e = std::getenv("NX_HDR_SDR_EOTF"); return e && e[0] ? std::atoi(e) : -1; }();
    const int eotf_mode = eotf_env >= 0 ? std::min(eotf_env, 2) : g_setting_eotf.load();
    const float eotf = eotf_mode == 1 ? 2.2f : eotf_mode == 2 ? 2.4f : 0.0f;
    const float push[8] = {paper / 80.0f, peak / paper, kGainAt100 * float(highlights_percent()) / 100.0f,
                           // 1.0: no decode. The game draws into the window textures through sRGB views, the GPU encodes on
                           // write; this image is float, nothing encodes, so what is in it is already linear.
                           float(start_percent()) / 100.0f, 1.0f, 1.0f / kEncodeScale, eotf, 0.0f};

    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.render_pass, rb.framebuffer = g.framebuffers[index], rb.renderArea = {{0, 0}, g.extent};
    p_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    VkRect2D sc{{0, 0}, g.extent};
    p_vkCmdSetViewport(cb, 0, 1, &v);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
    p_vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    return true;
}

}  // namespace nx::nvn
