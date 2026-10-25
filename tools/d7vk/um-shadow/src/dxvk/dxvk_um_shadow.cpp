#include "dxvk_device.h"
#include "dxvk_um_shadow.h"

#include <dxvk_present_vert.h>
#include <dxvk_um_shadow_depth_vert.h>
#include <dxvk_um_shadow_depth_frag.h>
#include <dxvk_um_shadow_resolve_frag.h>
#include <dxvk_um_shadow_resolve_ms_frag.h>
#include <dxvk_um_shadow_resolve_edge_frag.h>

namespace dxvk {

  struct UmShadowDepthPush {
    float    matrix[16];
    uint32_t samplerIndex;
    float    alphaRef;
    uint32_t alphaMode;
    uint32_t pad;
  };

  DxvkUmShadowObjects::DxvkUmShadowObjects(DxvkDevice* device)
  : m_device(device) {
    // Pipeline layouts: the depth pass has push constants only (plus one texture for alpha-tested casters), the resolve pass reads two images
    m_depthLayout = m_device->createBuiltInPipelineLayout(0u,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80u, 0u, nullptr);

    static const std::array<DxvkDescriptorSetLayoutBinding, 1> alphaBindings = {{
      { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    }};

    m_depthAlphaLayout = m_device->createBuiltInPipelineLayout(DxvkPipelineLayoutFlag::UsesSamplerHeap,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 80u, alphaBindings.size(), alphaBindings.data());

    static const std::array<DxvkDescriptorSetLayoutBinding, 2> resolveBindings = {{
      { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
      { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    }};

    m_resolveLayout = m_device->createBuiltInPipelineLayout(DxvkPipelineLayoutFlag::UsesSamplerHeap,
      VK_SHADER_STAGE_FRAGMENT_BIT, 224u, resolveBindings.size(), resolveBindings.data());

    // Samplers: hardware depth compare with a white border (outside the map = lit), and a plain one for the alpha-tested casters' textures
    DxvkSamplerKey samplerInfo = { };
    samplerInfo.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
    samplerInfo.setAddressModes(
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    samplerInfo.setDepthCompare(true, VK_COMPARE_OP_LESS_OR_EQUAL);
    samplerInfo.setUsePixelCoordinates(false);
    samplerInfo.borderColor = { };
    samplerInfo.borderColor.float32[0] = 1.0f;
    samplerInfo.borderColor.float32[1] = 1.0f;
    samplerInfo.borderColor.float32[2] = 1.0f;
    samplerInfo.borderColor.float32[3] = 1.0f;
    m_compareSampler = m_device->createSampler(samplerInfo);

    DxvkSamplerKey alphaInfo = { };
    alphaInfo.setFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
    alphaInfo.setAddressModes(
      VK_SAMPLER_ADDRESS_MODE_REPEAT,
      VK_SAMPLER_ADDRESS_MODE_REPEAT,
      VK_SAMPLER_ADDRESS_MODE_REPEAT);
    alphaInfo.setUsePixelCoordinates(false);
    m_alphaSampler = m_device->createSampler(alphaInfo);
  }


  DxvkUmShadowObjects::~DxvkUmShadowObjects() {
    auto vk = m_device->vkd();

    if (m_depthPipeline)      vk->vkDestroyPipeline(vk->device(), m_depthPipeline, nullptr);
    if (m_depthAlphaPipeline) vk->vkDestroyPipeline(vk->device(), m_depthAlphaPipeline, nullptr);

    for (const auto& p : m_resolvePipelines)
      vk->vkDestroyPipeline(vk->device(), p.second, nullptr);
  }


  bool DxvkUmShadowObjects::ensureMap(uint32_t size) {
    if (mapImage != nullptr && mapImage->info().extent.width == size)
      return true;

    DxvkImageCreateInfo imageInfo = { };
    imageInfo.type = VK_IMAGE_TYPE_2D;
    imageInfo.format = MapFormat;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent = { size, size, 1u };
    imageInfo.numLayers = 1u;
    imageInfo.mipLevels = 1u;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    imageInfo.access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                     | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                     | VK_ACCESS_SHADER_READ_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imageInfo.debugName = "Universal Mod shadow map";

    mapImage = m_device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    DxvkImageViewKey viewInfo = { };
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = MapFormat;
    viewInfo.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.mipIndex = 0u;
    viewInfo.mipCount = 1u;
    viewInfo.layerIndex = 0u;
    viewInfo.layerCount = 1u;

    viewInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    mapDepthView = mapImage->createView(viewInfo);

    viewInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    mapSampleView = mapImage->createView(viewInfo);

    return mapImage != nullptr;
  }


  VkPipeline DxvkUmShadowObjects::getDepthPipeline(bool alpha) {
    std::lock_guard lock(m_mutex);
    VkPipeline& pipeline = alpha ? m_depthAlphaPipeline : m_depthPipeline;

    if (!pipeline && !m_failed) {
      try {
        pipeline = createDepthPipeline(alpha);
      } catch (const DxvkError& e) {
        Logger::err(str::format("UmShadow: ", e.message()));
        m_failed = true;
      }
    }

    return pipeline;
  }


  VkPipeline DxvkUmShadowObjects::getResolvePipeline(VkFormat colorFormat, VkSampleCountFlagBits samples, bool opaque, bool edge) {
    std::lock_guard lock(m_mutex);
    const uint64_t key = (uint64_t(edge) << 41) | (uint64_t(opaque) << 40) | (uint64_t(samples) << 32) | uint64_t(colorFormat);
    auto entry = m_resolvePipelines.find(key);

    if (entry != m_resolvePipelines.end())
      return entry->second;

    if (m_failed)
      return VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;

    try {
      pipeline = createResolvePipeline(colorFormat, samples, opaque, edge);
    } catch (const DxvkError& e) {
      Logger::err(str::format("UmShadow: ", e.message()));
      m_failed = true;
      return VK_NULL_HANDLE;
    }

    m_resolvePipelines.insert({ key, pipeline });
    return pipeline;
  }


  VkPipeline DxvkUmShadowObjects::createDepthPipeline(bool alpha) {
    // The game's pre-transformed vertex: x, y, z, rhw, diffuse, specular, u, v (32 bytes)
    VkVertexInputBindingDescription binding = { 0u, 32u, VK_VERTEX_INPUT_RATE_VERTEX };

    std::array<VkVertexInputAttributeDescription, 3> attributes = {{
      { 0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT,  0u },
      { 1u, 0u, VK_FORMAT_R8G8B8A8_UNORM,      16u },
      { 2u, 0u, VK_FORMAT_R32G32_SFLOAT,       24u },
    }};

    VkPipelineVertexInputStateCreateInfo viState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    viState.vertexBindingDescriptionCount = 1u;
    viState.pVertexBindingDescriptions = &binding;
    viState.vertexAttributeDescriptionCount = attributes.size();
    viState.pVertexAttributeDescriptions = attributes.data();

    VkPipelineRasterizationStateCreateInfo rsState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsState.cullMode = VK_CULL_MODE_NONE;
    rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.polygonMode = VK_POLYGON_MODE_FILL;
    rsState.depthClampEnable = VK_TRUE;
    rsState.depthBiasEnable = VK_TRUE;
    rsState.depthBiasConstantFactor = 2.0f;
    rsState.depthBiasSlopeFactor = 2.5f;
    rsState.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo dsState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsState.depthTestEnable = VK_TRUE;
    dsState.depthWriteEnable = VK_TRUE;
    dsState.depthCompareOp = VK_COMPARE_OP_LESS;

    util::DxvkBuiltInGraphicsState state = { };
    state.vs = util::DxvkBuiltInShaderStage(dxvk_um_shadow_depth_vert, nullptr);

    if (alpha)
      state.fs = util::DxvkBuiltInShaderStage(dxvk_um_shadow_depth_frag, nullptr);

    state.depthFormat = MapFormat;
    state.viState = &viState;
    state.rsState = &rsState;
    state.dsState = &dsState;

    return m_device->createBuiltInGraphicsPipeline(depthLayout(alpha), state);
  }


  VkPipeline DxvkUmShadowObjects::createResolvePipeline(VkFormat colorFormat, VkSampleCountFlagBits samples, bool opaque, bool edge) {
    // dst = dst * factor
    VkPipelineColorBlendAttachmentState cbAttachment = { };
    cbAttachment.blendEnable = opaque ? VK_FALSE : VK_TRUE;   // opaque: the debug views write their colour as it is
    cbAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    cbAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    cbAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    cbAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;

    util::DxvkBuiltInGraphicsState state = { };
    state.vs = util::DxvkBuiltInShaderStage(dxvk_present_vert, nullptr);
    // the number of samples goes in as a specialization constant (the edge test loops over them)
    static const VkSpecializationMapEntry specEntry = { 0u, 0u, sizeof(int32_t) };
    int32_t specData = int32_t(samples);

    VkSpecializationInfo specInfo = { };
    specInfo.mapEntryCount = 1u;
    specInfo.pMapEntries = &specEntry;
    specInfo.dataSize = sizeof(specData);
    specInfo.pData = &specData;

    if (samples == VK_SAMPLE_COUNT_1_BIT)
      state.fs = util::DxvkBuiltInShaderStage(dxvk_um_shadow_resolve_frag, &specInfo);
    else if (edge)
      state.fs = util::DxvkBuiltInShaderStage(dxvk_um_shadow_resolve_edge_frag, &specInfo);
    else
      state.fs = util::DxvkBuiltInShaderStage(dxvk_um_shadow_resolve_ms_frag, &specInfo);
    state.colorFormats[0] = colorFormat;
    state.sampleCount = samples;
    state.cbAttachment = &cbAttachment;

    return m_device->createBuiltInGraphicsPipeline(m_resolveLayout, state);
  }

}
