#pragma once

#include <glm/glm.hpp>

namespace vrmod {

// Transient geometry shared by the post-render portal mask and its Slate layer.
// Height is the UI_Size convention; the portal aspect is fixed at 16:9.
struct VolumetricFrameLayout {
    glm::mat4 pose{1.0f};
    glm::vec2 size{0.0f};
    bool active{false};
};

enum class VolumetricFrameAnchorSource { KEEP, UI, HEAD };

inline VolumetricFrameAnchorSource volumetric_frame_anchor_source(
    bool match_ui, bool initial_activation, bool rematch, bool recenter
) {
    // Matching is a reset to the UI's stable placement, not a head recenter.
    if (match_ui && (initial_activation || rematch)) {
        return VolumetricFrameAnchorSource::UI;
    }
    if (initial_activation || recenter) {
        return VolumetricFrameAnchorSource::HEAD;
    }
    return VolumetricFrameAnchorSource::KEEP;
}

inline glm::vec2 volumetric_frame_size(float height) {
    return {height * (16.0f / 9.0f), height};
}

inline glm::mat4 apply_volumetric_frame_offsets(
    glm::mat4 anchor,
    float distance,
    float x_offset,
    float y_offset
) {
    anchor[3] -= anchor[2] * distance;
    anchor[3] += anchor[0] * x_offset;
    anchor[3] += anchor[1] * y_offset;
    anchor[3].w = 1.0f;
    return anchor;
}

} // namespace vrmod
