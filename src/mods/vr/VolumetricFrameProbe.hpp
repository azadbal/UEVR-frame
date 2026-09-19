#pragma once

#include <array>
#include <cstdint>
#include "VolumetricFrameLayout.hpp"
#include "VolumetricFrameCrop.hpp"

namespace vrmod {

enum class VolumetricFrameCropLoss {
    NONE, MISSING_RENDER_ASSOCIATION, QUEUE_SLOT_MISMATCH, REDUCED_WITHOUT_PROJECTION,
    POSE_REPLACED, VIEW_RECT_CHANGED, REDUCTION_DISALLOWED, PROJECTION_INVALID
};

inline const char* frame_crop_loss_name(VolumetricFrameCropLoss reason) {
    switch (reason) {
    case VolumetricFrameCropLoss::NONE: return "none";
    case VolumetricFrameCropLoss::MISSING_RENDER_ASSOCIATION: return "missing-render-association";
    case VolumetricFrameCropLoss::QUEUE_SLOT_MISMATCH: return "queue-slot-mismatch";
    case VolumetricFrameCropLoss::REDUCED_WITHOUT_PROJECTION: return "reduced-without-projection";
    case VolumetricFrameCropLoss::POSE_REPLACED: return "pose-replaced";
    case VolumetricFrameCropLoss::VIEW_RECT_CHANGED: return "view-rect-changed";
    case VolumetricFrameCropLoss::REDUCTION_DISALLOWED: return "reduction-disallowed";
    case VolumetricFrameCropLoss::PROJECTION_INVALID: return "projection-invalid";
    }
    return "unknown";
}

struct VolumetricFrameCropDiagnostic {
    VolumetricFrameCropLoss reason{VolumetricFrameCropLoss::NONE};
    bool render_frame_associated{false};
    uint32_t requested_render_frame{0};
    uint32_t source_pose_frame{0}, source_probe_frame{0};
    uint64_t source_pose_generation{0};
    uint32_t source_cropped_mask{0}, source_reduced_mask{0};
    uint32_t source_projection_mask{0}, source_rect_mask{0};
};

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

    // Recorded only when Native Stereo Fix clones a known source queue entry.
    // Submission must validate the retained source generation before matches can use it.
    struct NativeClone {
        uint32_t render_frame{0};
        uint64_t source_pose_generation{0};
        bool recorded{false};
        bool validated{false};
    } native_clone{};

    bool record_native_clone(uint32_t source_frame, uint64_t source_generation, uint32_t render_frame,
        bool source_pose_valid, bool source_crop_lost) {
        const bool eligible = !native_clone.recorded && prepared && !has_modified_view() &&
            source_pose_valid && !source_crop_lost && projection_mask == 3 && rect_mask == 3 &&
            pose_frame == source_frame && render_frame == source_frame + 1;
        native_clone = {};
        if (eligible) native_clone = {render_frame, source_generation, true, false};
        return eligible;
    }

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
        const bool frame_matches = native_clone.recorded ?
            native_clone.validated && !has_modified_view() && native_clone.render_frame == frame : pose_frame == frame;
        return prepared && layout.active && frame_matches &&
            width == output_width && height == output_height;
    }
};

inline void capture_frame_crop_source(VolumetricFrameCropDiagnostic& diagnostic,
    const VolumetricFrameProbe& probe, uint32_t pose_frame, uint64_t pose_generation) {
    diagnostic.source_pose_frame = pose_frame;
    diagnostic.source_probe_frame = probe.pose_frame;
    diagnostic.source_pose_generation = pose_generation;
    diagnostic.source_cropped_mask = probe.cropped_mask;
    diagnostic.source_reduced_mask = probe.reduced_mask;
    diagnostic.source_projection_mask = probe.projection_mask;
    diagnostic.source_rect_mask = probe.rect_mask;
}

// Called before replacing pose data. Retain the first escaped view's identity
// even though the probe itself must be rebuilt for the replacement pose.
inline void replace_frame_crop_pose(VolumetricFrameProbe& probe, bool same_frame, bool& lost,
    VolumetricFrameCropDiagnostic& diagnostic, uint32_t pose_frame, uint64_t pose_generation) {
    const auto previous = probe;
    if (!same_frame) {
        lost = false;
        diagnostic = {};
    } else if (!lost && previous.has_modified_view()) {
        lost = true;
        capture_frame_crop_source(diagnostic, previous, pose_frame, pose_generation);
        diagnostic.reason = VolumetricFrameCropLoss::POSE_REPLACED;
    }
    probe = {};
    if (same_frame) {
        probe.reduce_pixels_blocked = previous.reduce_pixels_blocked;
        probe.scene_rects = previous.scene_rects;
        probe.rect_mask = previous.rect_mask;
        probe.projection_mask = previous.projection_mask;
        probe.crop_decision_mask = previous.crop_decision_mask;
    }
}

// Returns whether this submission still has a render/pose association. Loss
// suppresses only this frame; a fresh matching queue entry can recover.
inline bool validate_frame_crop_submission(VolumetricFrameProbe& probe, bool& lost,
    VolumetricFrameCropDiagnostic& diagnostic, bool associated, uint32_t render_frame,
    uint32_t pose_frame, uint64_t pose_generation, bool ever_modified) {
    diagnostic.render_frame_associated = associated;
    diagnostic.requested_render_frame = render_frame;
    probe.native_clone.validated = false;
    const bool known_native_clone = probe.native_clone.recorded && !lost && probe.prepared &&
        !probe.has_modified_view() && probe.projection_mask == 3 && probe.rect_mask == 3 && probe.pose_frame == pose_frame &&
        probe.native_clone.source_pose_generation == pose_generation &&
        probe.native_clone.render_frame == render_frame && render_frame == pose_frame + 1;
    const bool frame_matches = probe.native_clone.recorded ? known_native_clone : render_frame == pose_frame;
    if (!associated || !frame_matches) {
        // Missing association can recur after this probe was already cleared.
        if (diagnostic.reason == VolumetricFrameCropLoss::NONE) {
            capture_frame_crop_source(diagnostic, probe, pose_frame, pose_generation);
        }
        diagnostic.reason = associated ? VolumetricFrameCropLoss::QUEUE_SLOT_MISMATCH :
            VolumetricFrameCropLoss::MISSING_RENDER_ASSOCIATION;
        lost = lost || ever_modified || probe.has_modified_view();
        probe = {};
        return false;
    }
    probe.native_clone.validated = known_native_clone;
    if (!lost) {
        capture_frame_crop_source(diagnostic, probe, pose_frame, pose_generation);
        diagnostic.reason = VolumetricFrameCropLoss::NONE;
        if ((probe.reduced_mask & ~probe.cropped_mask) != 0) {
            lost = true;
            diagnostic.reason = VolumetricFrameCropLoss::REDUCED_WITHOUT_PROJECTION;
        }
    }
    return true;
}

} // namespace vrmod
