#pragma once

#include <array>
#include <cstdint>
#include "VolumetricFrameLayout.hpp"
#include "VolumetricFrameCrop.hpp"

namespace vrmod {

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
    bool crop_requested{false};
    bool reduce_pixels_requested{false};
    bool reduce_pixels_blocked{false}; // No usable pose at the first view callback.
    bool green{false};
    uint32_t cropped_mask{0}; // Only eyes whose returned projection was changed.
    uint32_t reduced_mask{0}; // Only eyes whose returned active view was reduced.
    uint32_t crop_decision_mask{0};

    bool has_modified_view() const { return (cropped_mask | reduced_mask) != 0; }

    bool valid_crop(uint32_t eye) const {
        if (eye >= 2 || !prepared || !layout.active || !crop_requested || width <= 0 || height <= 0 ||
            crops[eye].fallback != VolumetricFrameCropFallback::NONE) return false;
        const auto& rect = crops[eye].rect;
        return rect.x >= 0 && rect.y >= 0 && rect.x < width && rect.y < height &&
            rect.width > 0 && rect.height > 0 && rect.width <= width - rect.x && rect.height <= height - rect.y;
    }

    bool baseline_rect(uint32_t eye, VolumetricFramePixelRect rect) const {
        return eye < 2 && rect.x == (int)eye * width && rect.y == 0 && rect.width == width && rect.height == height;
    }

    VolumetricFramePixelRect record_view_rect(uint32_t eye, VolumetricFramePixelRect incoming, bool& lost) {
        if (eye >= 2) return incoming;
        const uint32_t bit = 1u << eye;
        if (rect_mask & bit) {
            // Ordinary repeated callbacks receive the same baseline packing.
            // Any incompatible overwrite after a modified view escapes must blank.
            if (baseline_rect(eye, incoming) && !lost) return scene_rects[eye];
            const auto& previous = scene_rects[eye];
            if (has_modified_view() && (incoming.x != previous.x || incoming.y != previous.y ||
                incoming.width != previous.width || incoming.height != previous.height)) lost = true;
        }
        auto actual = incoming;
        if (!(rect_mask & bit) && !lost && !reduce_pixels_blocked && reduce_pixels_requested && valid_crop(eye) &&
            baseline_rect(eye, incoming) && (!(crop_decision_mask & bit) || (cropped_mask & bit))) {
            actual.width = crops[eye].rect.width;
            actual.height = crops[eye].rect.height;
            if (actual.width != width || actual.height != height) reduced_mask |= bit;
        }
        scene_rects[eye] = actual;
        rect_mask |= bit;
        return actual;
    }

    bool can_crop(uint32_t eye) const {
        if (!valid_crop(eye) || !(rect_mask & (1u << eye))) return false;
        const auto& rect = scene_rects[eye];
        if (!(reduced_mask & (1u << eye))) return baseline_rect(eye, rect);
        return rect.x == (int)eye * width && rect.y == 0 &&
            rect.width == crops[eye].rect.width && rect.height == crops[eye].rect.height;
    }

    bool apply_projection_crop(uint32_t eye, bool& lost) {
        if (eye >= 2 || lost) return false;
        const uint32_t bit = 1u << eye;
        const bool eligible = can_crop(eye);
        if (!eligible && ((cropped_mask | reduced_mask) & bit)) lost = true;
        if (!(crop_decision_mask & bit)) {
            crop_decision_mask |= bit;
            if (eligible) cropped_mask |= bit;
        }
        return !lost && eligible && (cropped_mask & bit);
    }

    bool matches(uint32_t frame, int output_width, int output_height) const {
        return prepared && layout.active && pose_frame == frame &&
            width == output_width && height == output_height;
    }
};

} // namespace vrmod
