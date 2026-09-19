#pragma once

#include <array>
#include <cstdint>
#include "VolumetricFrameLayout.hpp"
#include "VolumetricFrameCrop.hpp"

namespace vrmod {

// Diagnostic only: proposals never replace engine projections or scene sizes.
// Stored with OpenXR's pose queue, not in a global "latest frame" variable.
struct VolumetricFrameProbe {
    uint32_t pose_frame{0};
    bool prepared{false};
    VolumetricFrameLayout layout{};
    int width{0}, height{0};
    std::array<glm::vec4, 2> bounds{};
    std::array<VolumetricFrameCrop, 2> crops{};
    std::array<glm::mat4, 2> projections{glm::mat4{1}, glm::mat4{1}};
    std::array<VolumetricFramePixelRect, 2> scene_rects{};
    uint32_t projection_mask{0};
    uint32_t rect_mask{0};

    bool matches(uint32_t frame, int output_width, int output_height) const {
        return prepared && layout.active && pose_frame == frame &&
            width == output_width && height == output_height;
    }
};

} // namespace vrmod
