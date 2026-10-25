#pragma once

// Universal Mod: sun shadows for the game's pre-transformed world (see docs/shadows.md).
// Every depth-writing draw between UmShadowBegin and UmShadowEnd is also kept in a list (vertex / index slices of the UP buffer);
// at the end the list is drawn into a depth-only shadow map seen from the sun (the vertex shader un-projects the screen-space
// vertices back to world space) and a full-screen pass darkens the colour buffer where the sun does not reach.

#include <mutex>
#include <unordered_map>
#include <vector>

#include "dxvk_cmdlist.h"
#include "dxvk_image.h"
#include "dxvk_buffer.h"
#include "dxvk_sampler.h"
#include "dxvk_util.h"

namespace dxvk {

  class DxvkDevice;

  /** Parameters of one frame, filled by the game side (plain data, versioned by size). */
  struct DxvkUmShadowParams {
    uint32_t size            = sizeof(DxvkUmShadowParams);
    uint32_t flags           = 0u;        // bit 0: debug view of the shadow term
    float    screenToWorld[16] = { };     // column-major 4x4: (x * cw, y * cw, z * cw, cw) -> world (homogeneous)
    float    lightDir[3]     = { };       // direction the light TRAVELS (unit)
    float    strength        = 1.0f;      // 0..1
    float    sunColor[3]     = { };
    float    bias            = 0.0015f;   // light depth units (0..1 over the map's depth range)
    float    ambient[3]      = { };
    float    slopeBias       = 0.004f;
    float    depthK          = 0.0f;      // cw = K / (far - z * (far - near)), K = sin(fov / 2) * near * far
    float    depthFar        = 100.0f;
    float    depthNear       = 2.0f;
    float    cameraPos[3]    = { };
    float    center[3]       = { };       // centre of the area to shadow (world)
    float    radius          = 40.0f;     // half extent of the shadowed area (world units)
    float    height          = 60.0f;     // half extent of the casters' range along the light
    uint32_t mapSize         = 2048u;     // shadow map texels per side
    float    extra           = 0.0f;      // extra darkness inside a shadow, on top of the game's own sun / ambient ratio (0 = none)
    float    guard[2]        = { };       // pixels the game's world coordinates are shifted by (guard band): screen pixel + guard = the game's x / y
  };

  /** One recorded caster draw. */
  struct DxvkUmShadowDraw {
    VkBuffer              vertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize          vertexOffset = 0u;
    VkBuffer              indexBuffer  = VK_NULL_HANDLE;
    VkDeviceSize          indexOffset  = 0u;
    VkDeviceSize          indexLength  = 0u;
    VkIndexType           indexType    = VK_INDEX_TYPE_UINT16;
    uint32_t              stride       = 32u;
    uint32_t              indexCount   = 0u;
    uint32_t              alphaMode    = 0u;       // 0 none, 1 vertex alpha, 2 vertex alpha * texture alpha
    float                 alphaRef     = 0.0f;
    Rc<DxvkBuffer>        buffer;                  // keeps the buffer object (and, with `storage`, the memory) alive
    Rc<DxvkResourceAllocation> storage;
    Rc<DxvkImageView>     alphaView;
  };

  class DxvkUmShadowObjects {

  public:

    DxvkUmShadowObjects(DxvkDevice* device);
    ~DxvkUmShadowObjects();

    const DxvkPipelineLayout* depthLayout(bool alpha) const { return alpha ? m_depthAlphaLayout : m_depthLayout; }
    const DxvkPipelineLayout* resolveLayout() const { return m_resolveLayout; }

    VkPipeline getDepthPipeline(bool alpha);
    VkPipeline getResolvePipeline(VkFormat colorFormat, VkSampleCountFlagBits samples, bool opaque, bool edge);

    const Rc<DxvkSampler>& compareSampler() const { return m_compareSampler; }
    const Rc<DxvkSampler>& alphaSampler() const { return m_alphaSampler; }

    /** Creates (or re-creates for another size) the shadow map; returns false when it cannot be made. */
    bool ensureMap(uint32_t size);

    Rc<DxvkImage>     mapImage;
    Rc<DxvkImageView> mapDepthView;     // depth attachment
    Rc<DxvkImageView> mapSampleView;    // sampled

    static constexpr VkFormat MapFormat = VK_FORMAT_D32_SFLOAT;

  private:

    DxvkDevice* m_device = nullptr;

    const DxvkPipelineLayout* m_depthLayout      = nullptr;
    const DxvkPipelineLayout* m_depthAlphaLayout = nullptr;
    const DxvkPipelineLayout* m_resolveLayout    = nullptr;

    Rc<DxvkSampler> m_compareSampler;
    Rc<DxvkSampler> m_alphaSampler;

    VkPipeline m_depthPipeline      = VK_NULL_HANDLE;
    VkPipeline m_depthAlphaPipeline = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, VkPipeline> m_resolvePipelines;

    std::mutex m_mutex;
    bool m_failed = false;   // a pipeline could not be built: no more attempts

    VkPipeline createDepthPipeline(bool alpha);
    VkPipeline createResolvePipeline(VkFormat colorFormat, VkSampleCountFlagBits samples, bool opaque, bool edge);

  };

}
