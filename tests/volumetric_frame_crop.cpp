// cl /nologo /EHsc /std:c++17 /Idependencies/submodules/glm tests/volumetric_frame_crop.cpp /Febuild/frame-crop-test.exe /Fobuild/frame-crop-test.obj
#include <cstdio>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include "../src/mods/vr/VolumetricFrameCrop.hpp"
#include "../src/mods/vr/VolumetricFrameProbe.hpp"

using namespace vrmod;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool equal(VolumetricFramePixelRect a, VolumetricFramePixelRect b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

void close(double a, double b, double tolerance = 1e-10) {
    require(std::abs(a - b) < tolerance, "Projection or resolve pixel mapping differs");
}

template<typename T>
void projection_test() {
    // An asymmetric baseline engine projection, deliberately distinct from raw runtime FOV.
    // Infinite reversed-Z also verifies that the crop leaves the depth convention intact.
    const T l = T(-1.4), r = T(1.1), t = T(1.2), b = T(-0.95);
    glm::mat<4, 4, T> baseline{T(0)};
    baseline[0][0] = T(2) / (r-l);
    baseline[1][1] = T(2) / (t-b);
    baseline[2][0] = (r+l) / (r-l);
    baseline[2][1] = (t+b) / (t-b);
    baseline[2][3] = T(-1);
    baseline[3][2] = T(0.01);
    const int width = 2053, height = 1999;
    const auto frame = glm::translate(glm::mat4{1}, glm::vec3{.1f, -.05f, -2});
    const auto crop = calculate_volumetric_frame_crop(frame, {1.1f, .7f}, glm::mat4{1},
        {-1.1f, .9f, 1.0f, -.85f}, {103, 81, 1843, 1796}, width, height);
    require(crop.fallback == VolumetricFrameCropFallback::NONE, "Valid crop rejected");
    const auto rect = crop.rect;
    const auto narrow = crop_volumetric_frame_projection(baseline, rect, width, height);
    const double tolerance = sizeof(T) == sizeof(float) ? 5e-4 : 1e-9;
    for (int column = 0; column < 4; ++column) {
        require(narrow[column][2] == baseline[column][2] && narrow[column][3] == baseline[column][3],
            "Crop changed homogeneous Z/W");
    }
    for (int y = rect.y; y < rect.y + rect.height; y += 19) {
        for (int x = rect.x; x < rect.x + rect.width; x += 17) {
            const T u = (T(x) + T(.5)) / T(width), v = (T(y) + T(.5)) / T(height);
            const glm::vec<4, T> ray{l + (r-l)*u, t + (b-t)*v, T(-1), T(1)};
            const auto clip = narrow * ray;
            const T scene_u = (clip.x / clip.w + T(1)) / T(2);
            const T scene_v = (T(1) - clip.y / clip.w) / T(2);
            close(scene_u * rect.width + rect.x, x + .5, tolerance);
            close(scene_v * rect.height + rect.y, y + .5, tolerance);
            const auto original_clip = baseline * ray;
            require(clip.z == original_clip.z && clip.w == original_clip.w, "Projected depth changed");
        }
    }
    require(crop_volumetric_frame_projection(baseline, {0, 0, width, height}, width, height) == baseline,
        "Full crop must preserve baseline projection");
    require(crop_volumetric_frame_projection(baseline, {0, 0, 0, 1}, width, height) == baseline,
        "Invalid crop must preserve baseline projection");
}

void fixed_view_diagnostic_test() {
    const int width = 2053, height = 1999;
    auto ready = [&](float scale) {
        VolumetricFrameProbe probe{};
        probe.prepared = probe.layout.active = probe.crop_requested = probe.reduce_pixels_requested = true;
        probe.width = width;
        probe.height = height;
        probe.fixed_view_scale = scale;
        probe.crops = {{{{101, 201, 901, 801}, VolumetricFrameCropFallback::NONE},
                        {{82, 212, 887, 793}, VolumetricFrameCropFallback::NONE}}};
        return probe;
    };
    // Successive live crops retain different projections but share a constant source size.
    for (int frame = 0; frame < 4; ++frame) {
        auto probe = ready(0.75f);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            probe.crops[eye].rect.width += frame * 19;
            probe.crops[eye].rect.height += frame * 13;
            probe.crops[eye].rect.x += frame * 7;
            const auto crop = probe.crops[eye].rect;
            bool lost = false;
            const VolumetricFramePixelRect baseline{(int)eye * width, 0, width, height};
            const auto actual = probe.record_view_rect(eye, baseline, lost);
            require(equal(actual, {(int)eye * width, 0, 1540, 1500}), "Fixed shared dimensions changed with live crop");
            require(probe.can_crop(eye) && probe.apply_projection_crop(eye, lost) && !lost,
                "Validated fixed source dimensions rejected live crop");
            require(equal(probe.crops[eye].rect, crop), "Fixed view froze or replaced live crop");
            probe.reduce_pixels_requested = false;
            require(equal(probe.record_view_rect(eye, baseline, lost), actual), "Repeated callback lost fixed source size");
            probe.reduce_pixels_requested = true;
            // A ray at a known full-eye pixel must resolve back to that pixel even
            // when the fixed source and live output crop have different dimensions.
            const double pixel_x = crop.x + crop.width * 0.37;
            const double pixel_y = crop.y + crop.height * 0.61;
            const auto projection = crop_volumetric_frame_projection(glm::dmat4{1}, crop, width, height);
            const auto clip = projection * glm::dvec4{2 * pixel_x / width - 1, 1 - 2 * pixel_y / height, 0, 1};
            const double source_x = actual.x + (clip.x + 1) * actual.width / 2;
            const double source_y = actual.y + (1 - clip.y) * actual.height / 2;
            close(crop.x + (source_x - actual.x) / actual.width * crop.width, pixel_x);
            close(crop.y + (source_y - actual.y) / actual.height * crop.height, pixel_y);
            probe.scene_rects[eye].width -= 1;
            require(!probe.can_crop(eye), "Unexpected fixed source extent accepted");
            probe.scene_rects[eye] = actual;
            probe.scene_rects[eye].x += 1;
            require(!probe.can_crop(eye), "Unexpected fixed source packing accepted");
            probe.scene_rects[eye] = actual;
        }
    }
    for (float scale : {0.0f, -1.0f, 0.05f, 1.01f, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
        auto probe = ready(scale);
        bool lost = false;
        require(equal(probe.record_view_rect(0, {0, 0, width, height}, lost), {0, 0, 901, 801}) &&
            probe.apply_projection_crop(0, lost), "Default/invalid fixed scale changed exact-crop behavior");
    }
    auto full = ready(1.0f);
    bool lost = false;
    require(equal(full.record_view_rect(0, {0, 0, width, height}, lost), {0, 0, width, height}) &&
        full.apply_projection_crop(0, lost) && full.reduced_mask == 0, "Fixed scale 1 lost crop-only comparator");
    auto minimum = ready(0.1f);
    require(equal(minimum.reduced_view_rect(0), {0, 0, 206, 200}), "Minimum fixed scale rounding changed");
    auto disabled = ready(0.75f);
    disabled.reduce_pixels_requested = false;
    require(equal(disabled.record_view_rect(0, {0, 0, width, height}, lost), {0, 0, width, height}),
        "Fixed scale changed views with reduction disabled");
    auto fallback = ready(0.75f);
    fallback.crops[0].fallback = VolumetricFrameCropFallback::EYE_PLANE;
    require(equal(fallback.record_view_rect(0, {0, 0, width, height}, lost), {0, 0, width, height}) &&
        !fallback.apply_projection_crop(0, lost), "Fixed sizing bypassed crop fallback validation");
}

void view_transition_test() {
    const int width = 2053, height = 1999;
    const std::array<VolumetricFramePixelRect, 2> full{{{0, 0, width, height}, {width, 0, width, height}}};
    auto ready = [&]() {
        VolumetricFrameProbe probe{};
        probe.pose_frame = 42;
        probe.prepared = probe.layout.active = probe.crop_requested = true;
        probe.width = width;
        probe.height = height;
        probe.crops = {{{{101, 201, 901, 801}, VolumetricFrameCropFallback::NONE},
                        {{82, 212, 887, 793}, VolumetricFrameCropFallback::NONE}}};
        return probe;
    };
    bool lost = false;
    auto phase2 = ready();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        require(equal(phase2.record_view_rect(eye, full[eye], lost), full[eye]), "Phase 2 changed active dimensions");
        require(phase2.apply_projection_crop(eye, lost), "Phase 2 did not retain crop projection");
    }
    require(!lost && phase2.cropped_mask == 3 && phase2.reduced_mask == 0, "Phase 2 state changed");

    auto phase3 = ready();
    phase3.reduce_pixels_requested = true;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto actual = phase3.record_view_rect(eye, full[eye], lost);
        require(equal(actual, {(int)eye * width, 0, phase3.crops[eye].rect.width, phase3.crops[eye].rect.height}),
            "Reduced view lost integer crop extent or stable full-allocation packing");
        require(phase3.can_crop(eye) && phase3.apply_projection_crop(eye, lost), "Reduced view lost matching crop projection");
        require(equal(phase3.record_view_rect(eye, full[eye], lost), actual), "Repeated view callback changed frozen dimensions");
    }
    require(!lost && phase3.reduced_mask == 3 && phase3.cropped_mask == 3, "Phase 3 did not record both actual reductions");
    require(phase3.width == width && phase3.height == height, "Reduction changed output dimensions");
    require(phase3.has_modified_view(), "Modified frame cannot be detected on pose replacement");
    phase3.reduce_pixels_requested = false;
    require(equal(phase3.record_view_rect(1, full[1], lost), {width, 0, 887, 793}) &&
        phase3.apply_projection_crop(1, lost), "Reduction toggle changed an already returned view");

    auto fallback = ready();
    fallback.reduce_pixels_requested = true;
    fallback.crops[1].fallback = VolumetricFrameCropFallback::EYE_PLANE;
    fallback.record_view_rect(0, full[0], lost);
    require(equal(fallback.record_view_rect(1, full[1], lost), full[1]), "Fallback eye was reduced");
    require(fallback.apply_projection_crop(0, lost) && !fallback.apply_projection_crop(1, lost) &&
        fallback.reduced_mask == 1 && !lost, "Per-eye fallback changed valid eye or blanked frame");

    auto packing = ready();
    packing.reduce_pixels_requested = true;
    const VolumetricFramePixelRect unexpected{0, 0, width, height};
    require(equal(packing.record_view_rect(1, unexpected, lost), unexpected) &&
        !packing.apply_projection_crop(1, lost) && packing.reduced_mask == 0, "Unsupported packing reduced or cropped");

    auto order = ready();
    order.reduce_pixels_requested = true;
    require(!order.apply_projection_crop(0, lost), "Projection before view cropped");
    require(equal(order.record_view_rect(0, full[0], lost), full[0]) && !order.apply_projection_crop(0, lost),
        "Late view changed an already returned full projection decision");

    auto late_pose = ready();
    late_pose.prepared = false;
    late_pose.reduce_pixels_blocked = true;
    late_pose.record_view_rect(0, full[0], lost);
    late_pose.prepared = late_pose.reduce_pixels_requested = true;
    require(equal(late_pose.record_view_rect(1, full[1], lost), full[1]) && late_pose.apply_projection_crop(0, lost) &&
        late_pose.apply_projection_crop(1, lost) && late_pose.reduced_mask == 0, "Late pose failed full-size Phase 2 fallback");

    auto incomplete = ready();
    incomplete.reduce_pixels_requested = true;
    incomplete.record_view_rect(0, full[0], lost);
    require(incomplete.has_modified_view() && (incomplete.reduced_mask & ~incomplete.cropped_mask) == 1,
        "Reduced view without a matching projection cannot be detected at submission");
    incomplete.crops[0].rect.width = 0;
    require(!incomplete.apply_projection_crop(0, lost) && lost, "Lost crop after shrinking did not fail closed");

    lost = false;
    phase3.record_view_rect(1, unexpected, lost);
    require(lost, "Incompatible view overwrite after reducing did not fail closed");
    lost = false;
    auto invalid = ready();
    invalid.reduce_pixels_requested = true;
    invalid.crops[0].rect = {width - 1, 0, 2, 1};
    require(equal(invalid.record_view_rect(0, full[0], lost), full[0]) && !invalid.apply_projection_crop(0, lost),
        "Out-of-bounds crop was accepted");
    require(!lost && invalid.reduced_mask == 0, "Invalid candidate modified the baseline frame");
}

void submission_recovery_test() {
    auto rendered = []() {
        VolumetricFrameProbe probe{};
        probe.pose_frame = 42;
        probe.prepared = probe.layout.active = true;
        probe.cropped_mask = probe.reduced_mask = probe.projection_mask = probe.rect_mask = 3;
        return probe;
    };
    auto probe = rendered();
    bool lost = false;
    VolumetricFrameCropDiagnostic diagnostic{};
    require(!validate_frame_crop_submission(probe, lost, diagnostic, false, 42, 42, 7, true) && lost,
        "Missing render association did not suppress an uncertain image");
    require(!probe.prepared && diagnostic.reason == VolumetricFrameCropLoss::MISSING_RENDER_ASSOCIATION &&
        diagnostic.source_probe_frame == 42 && diagnostic.source_pose_generation == 7 &&
        diagnostic.source_cropped_mask == 3 && diagnostic.source_reduced_mask == 3,
        "Clearing an unassociated probe discarded source evidence");
    validate_frame_crop_submission(probe, lost, diagnostic, false, 42, 42, 7, true);
    require(diagnostic.source_reduced_mask == 3, "Repeated missing association erased original evidence");

    probe = rendered();
    lost = false;
    diagnostic = {};
    require(!validate_frame_crop_submission(probe, lost, diagnostic, true, 48, 42, 7, true) && lost &&
        diagnostic.reason == VolumetricFrameCropLoss::QUEUE_SLOT_MISMATCH &&
        diagnostic.requested_render_frame == 48 && diagnostic.source_pose_frame == 42,
        "Wrapped queue slot accepted or lost its actual identity");

    probe = rendered();
    probe.cropped_mask = 1;
    lost = false;
    diagnostic = {};
    require(validate_frame_crop_submission(probe, lost, diagnostic, true, 42, 42, 7, true) && lost &&
        diagnostic.reason == VolumetricFrameCropLoss::REDUCED_WITHOUT_PROJECTION,
        "Reduced eye without a projection escaped submission validation");

    probe = rendered();
    lost = false;
    diagnostic = {};
    replace_frame_crop_pose(probe, true, lost, diagnostic, 42, 7);
    require(lost && !probe.prepared && diagnostic.reason == VolumetricFrameCropLoss::POSE_REPLACED &&
        diagnostic.source_reduced_mask == 3 && diagnostic.source_pose_generation == 7,
        "Pose replacement discarded escaped crop evidence");
    validate_frame_crop_submission(probe, lost, diagnostic, true, 42, 42, 8, true);
    require(lost && diagnostic.source_pose_generation == 7 && diagnostic.source_reduced_mask == 3,
        "Submission replaced escaped source identity with replacement pose");

    replace_frame_crop_pose(probe, false, lost, diagnostic, 42, 8);
    require(!lost && diagnostic.reason == VolumetricFrameCropLoss::NONE,
        "A fresh pose inherited a transient frame failure");
    probe = rendered();
    probe.pose_frame = 48;
    require(validate_frame_crop_submission(probe, lost, diagnostic, true, 48, 48, 9, true) && !lost &&
        diagnostic.reason == VolumetricFrameCropLoss::NONE && diagnostic.source_pose_frame == 48 &&
        diagnostic.source_reduced_mask == 3, "Fresh matching rendered frame could not recover");

    probe = {};
    lost = false;
    diagnostic = {};
    require(!validate_frame_crop_submission(probe, lost, diagnostic, false, 0, 0, 0, false) && !lost,
        "Unmodified startup frame was unnecessarily marked crop-lost");
}

void native_eye_crop_test() {
    VolumetricFrameProbe probe{};
    probe.pose_frame = 41;
    probe.prepared = probe.layout.active = probe.crop_requested = true;
    probe.separate_eye_sources = true;
    probe.width = 2000;
    probe.height = 1800;
    probe.crops = {{{{100, 200, 900, 800}, VolumetricFrameCropFallback::NONE},
                    {{80, 210, 880, 790}, VolumetricFrameCropFallback::NONE}}};
    bool lost = false;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        probe.record_view_rect(eye, {0, 0, 2000, 1800}, lost);
        require(probe.apply_projection_crop(eye, lost), "Native eye-local view was not cropped");
    }
    require(probe.cropped_mask == 3 && !lost, "Native eyes did not retain independent crop decisions");
    require(probe.packed_scene_rect(0).x == 0 && probe.packed_scene_rect(1).x == 2000,
        "Native eye-local sources lost eye identity after assembly");
    probe.projection_mask = 3;
    require(probe.record_native_clone(41, 7, 42, true, false), "Native crop clone was rejected");
    VolumetricFrameCropDiagnostic diagnostic{};
    require(validate_frame_crop_submission(probe, lost, diagnostic, true, 42, 41, 7, true) &&
        !lost && probe.matches(42, 2000, 1800), "Native crop clone lost source pose association");
    auto corrupt = probe;
    corrupt.cropped_mask = 1;
    require(!validate_frame_crop_submission(corrupt, lost, diagnostic, true, 42, 41, 7, true) && lost,
        "Changed native crop decisions survived clone validation");
    corrupt = probe;
    lost = false;
    require(!validate_frame_crop_submission(corrupt, lost, diagnostic, true, 42, 41, 8, true) && lost,
        "Changed native crop source generation survived validation");
    corrupt = probe;
    corrupt.native_clone = {};
    corrupt.rect_mask = corrupt.cropped_mask = corrupt.crop_decision_mask = 0;
    corrupt.reduce_pixels_requested = true;
    lost = false;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const auto rect = corrupt.record_view_rect(eye, {0, 0, 2000, 1800}, lost);
        require(rect.width == 2000 && rect.height == 1800 && corrupt.apply_projection_crop(eye, lost),
            "Native pixel request changed full-size crop behavior");
    }
    require(corrupt.reduced_mask == 0, "Native crop enabled pixel reduction");
    corrupt.reduced_mask = 1;
    require(!corrupt.record_native_clone(41, 7, 42, true, false), "Reduced native view acquired a clone token");
    corrupt = probe;
    corrupt.scene_rects[1].x = 2000;
    lost = false;
    require(!validate_frame_crop_submission(corrupt, lost, diagnostic, true, 42, 41, 7, true) && lost,
        "Packed rect was accepted for an eye-local native source");
    corrupt = probe;
    lost = false;
    corrupt.record_view_rect(1, {0, 0, 1900, 1800}, lost);
    require(lost, "Changed native eye rect after projection did not fail closed");
}

void native_clone_submission_test() {
    VolumetricFrameProbe source{};
    source.pose_frame = 2045;
    source.prepared = source.layout.active = true;
    source.width = 2688;
    source.height = 2880;
    source.rect_mask = source.projection_mask = 3;
    source.scene_rects = {{{0, 0, 2688, 2880}, {0, 0, 2688, 2880}}};
    auto cloned = source;
    require(cloned.record_native_clone(2045, 330, 2046, true, false), "Known native clone was not recorded");
    require(!cloned.matches(2046, 2688, 2880), "Unvalidated clone matched submission");
    bool lost = false;
    VolumetricFrameCropDiagnostic diagnostic{};
    require(validate_frame_crop_submission(cloned, lost, diagnostic, true, 2046, 2045, 330, false),
        "Known native clone lost its render/source association");
    require(!lost && cloned.matches(2046, 2688, 2880) && cloned.pose_frame == 2045 &&
        source.pose_frame == 2045 && !source.native_clone.recorded &&
        diagnostic.requested_render_frame == 2046 && diagnostic.source_pose_frame == 2045 &&
        diagnostic.source_probe_frame == 2045 && diagnostic.source_pose_generation == 330,
        "Native clone relabeled source identity or lost matching geometry");
    require(!cloned.can_crop(1), "Native clone enabled eye-local cropping");
    require(!cloned.matches(2046, 2689, 2880), "Resized native clone matched");

    auto reject = [&](VolumetricFrameProbe probe, uint32_t render, uint32_t pose, uint64_t generation, bool associated) {
        bool rejected_lost = false;
        VolumetricFrameCropDiagnostic rejected_diagnostic{};
        require(!validate_frame_crop_submission(probe, rejected_lost, rejected_diagnostic,
            associated, render, pose, generation, false) && !probe.prepared && !probe.native_clone.validated,
            "Unproven native clone survived submission validation");
    };
    reject(source, 2046, 2045, 330, true); // One-frame mismatch alone is insufficient.
    reject(cloned, 2052, 2045, 330, true); // Six-slot ring reuse must not match.
    reject(cloned, 2046, 2045, 331, true); // Same source frame, replaced pose generation.
    reject(cloned, 2046, 2039, 330, true); // Replaced source identity.
    reject(cloned, 2046, 2045, 330, false);
    auto modified = cloned;
    modified.cropped_mask = 1;
    reject(modified, 2046, 2045, 330, true);
    modified = source;
    modified.reduced_mask = 1;
    require(!modified.record_native_clone(2045, 330, 2046, true, false), "Modified view acquired a native clone token");
    auto invalid_source = source;
    require(!invalid_source.record_native_clone(2039, 330, 2046, true, false), "Stale source acquired a native clone token");
    for (bool valid_pose : {false, true}) {
        auto invalid = source;
        require(!invalid.record_native_clone(2045, 330, 2046, valid_pose, true), "Lost source acquired a native clone token");
    }
    invalid_source = source;
    require(!invalid_source.record_native_clone(2045, 330, 2046, false, false), "Invalid pose acquired a native clone token");
    for (bool incomplete_rects : {false, true}) {
        auto partial = source;
        (incomplete_rects ? partial.rect_mask : partial.projection_mask) = 1;
        require(!partial.record_native_clone(2045, 330, 2046, true, false), "Partial source acquired a native clone token");
        partial = cloned;
        (incomplete_rects ? partial.rect_mask : partial.projection_mask) = 1;
        reject(partial, 2046, 2045, 330, true);
    }

    for (bool same_frame : {false, true}) {
        auto replaced = cloned;
        replace_frame_crop_pose(replaced, same_frame, lost, diagnostic, 2045, 330);
        require(!replaced.native_clone.recorded && !replaced.native_clone.validated &&
            !replaced.matches(2046, 2688, 2880), "Pose replacement retained a native clone token");
    }
}

int main() try {
    const int width = 2053, height = 1999;
    const VolumetricFramePixelRect submitted{103, 81, 1843, 1796};
    const glm::vec4 fov{-1.1f, .9f, 1.0f, -.85f};
    for (const float eye_x : {-.032f, .032f}) {
        for (const float yaw : {-.6f, 0.0f, .8f}) {
            auto frame = glm::translate(glm::mat4{1}, glm::vec3{.17f, .13f, -1.8f});
            frame = glm::rotate(frame, .45f, glm::vec3{0, 0, 1});
            frame = glm::rotate(frame, yaw, glm::vec3{0, 1, 0});
            auto eye = glm::translate(glm::mat4{1}, glm::vec3{eye_x, .02f, .03f});
            eye = glm::rotate(eye, -.12f, glm::vec3{0, 1, 0});
            const auto crop = calculate_volumetric_frame_crop(frame, {2, 1.125f}, eye, fov,
                submitted, width, height);
            require(crop.fallback == VolumetricFrameCropFallback::NONE, "Oblique crop rejected");
            require(crop.rect.x >= submitted.x && crop.rect.y >= submitted.y &&
                crop.rect.x + crop.rect.width <= submitted.x + submitted.width &&
                crop.rect.y + crop.rect.height <= submitted.y + submitted.height,
                "Crop escaped integer submission rectangle");
            for (int ix = 0; ix <= 40; ++ix) for (int iy = 0; iy <= 22; ++iy) {
                const auto p = glm::inverse(eye) * frame * glm::vec4{-1 + ix / 20.0f,
                    -.5625f + iy * 1.125f / 22, 0, 1};
                const float u = (p.x / -p.z - fov.x) / (fov.y - fov.x);
                const float v = (p.y / -p.z - fov.z) / (fov.w - fov.z);
                if (u < 0 || u > 1 || v < 0 || v > 1) continue;
                const float x = submitted.x + u * submitted.width, y = submitted.y + v * submitted.height;
                require(x >= crop.rect.x && x <= crop.rect.x + crop.rect.width &&
                    y >= crop.rect.y && y <= crop.rect.y + crop.rect.height,
                    "Crop lost visible aperture interior");
            }
        }
    }
    const glm::mat4 eye{1};
    auto frame = glm::translate(glm::mat4{1}, glm::vec3{0, 0, -2});
    const auto centered = calculate_volumetric_frame_crop(frame, {2, 1}, eye, {-1, 1, 1, -1},
        {10, 20, 200, 160}, 240, 220);
    require(equal(centered.rect, {44, 64, 132, 72}), "Crop lost subimage offset or exact 16px guard");
    auto fallback = [&](glm::mat4 pose, glm::vec4 tangents, VolumetricFrameCropFallback reason) {
        const auto crop = calculate_volumetric_frame_crop(pose, {2, 1}, eye, tangents, submitted, width, height);
        require(equal(crop.rect, {0, 0, width, height}) && crop.fallback == reason, "Incorrect full-view fallback");
    };
    fallback(glm::mat4{1}, fov, VolumetricFrameCropFallback::EYE_PLANE);
    fallback(glm::translate(eye, {0, 0, 1}), fov, VolumetricFrameCropFallback::EYE_PLANE);
    fallback(glm::rotate(glm::translate(eye, {0, 0, -.2f}), .8f, glm::vec3{0, 1, 0}),
        fov, VolumetricFrameCropFallback::EYE_PLANE);
    fallback(glm::translate(eye, {100, 0, -1}), fov, VolumetricFrameCropFallback::OFFSCREEN);
    fallback(frame, {1, 1, 1, -1}, VolumetricFrameCropFallback::INVALID_INPUT);
    frame[3].x = std::numeric_limits<float>::quiet_NaN();
    fallback(frame, fov, VolumetricFrameCropFallback::INVALID_INPUT);
    const auto huge = calculate_volumetric_frame_crop(glm::translate(eye, {0, 0, -1}),
        {1e30f, 1e30f}, eye, fov, submitted, width, height);
    require(huge.fallback == VolumetricFrameCropFallback::NONE && equal(huge.rect, submitted),
        "Extreme projected bounds must clamp before integer conversion");
    const auto invalid_eye = calculate_volumetric_frame_crop(glm::translate(eye, {0, 0, -1}),
        {2, 1}, glm::mat4{0}, fov, submitted, width, height);
    require(invalid_eye.fallback == VolumetricFrameCropFallback::INVALID_INPUT &&
        equal(invalid_eye.rect, {0, 0, width, height}), "Invalid eye transform must fall back");
    VolumetricFrameProbe probe{};
    require(!probe.matches(100, width, height), "Empty snapshot accepted");
    probe.pose_frame = 100;
    probe.width = width;
    probe.height = height;
    probe.prepared = true;
    probe.layout.active = true;
    require(probe.matches(100, width, height), "Matching snapshot rejected");
    require(!probe.matches(106, width, height), "Wrapped frame slot accepted");
    require(!probe.matches(100, width + 1, height), "Resized output accepted");
    require(!probe.matches(100, width, height + 1), "Resized output height accepted");
    require(!probe.can_crop(0), "Disabled experiment changed a projection");
    probe.crop_requested = true;
    probe.scene_rects = {{{0, 0, width, height}, {width, 0, width, height}}};
    probe.crops[0] = {{100, 200, 900, 800}, VolumetricFrameCropFallback::NONE};
    probe.crops[1] = {{80, 210, 900, 800}, VolumetricFrameCropFallback::NONE};
    require(!probe.can_crop(0), "Missing view callback accepted for cropping");
    probe.rect_mask = 1;
    require(probe.can_crop(0) && !probe.can_crop(1), "Per-eye view readiness lost");
    probe.rect_mask = 3;
    require(probe.can_crop(0) && probe.can_crop(1), "Valid packed stereo rejected");
    probe.scene_rects[1].x = 0;
    require(!probe.can_crop(1), "Native-stereo-fix packing accepted");
    probe.scene_rects[1].x = width;
    probe.scene_rects[1].height -= 1;
    require(!probe.can_crop(1), "Unexpected view size accepted");
    probe.scene_rects[1].height = height;
    probe.crops[1].fallback = VolumetricFrameCropFallback::EYE_PLANE;
    require(!probe.can_crop(1) && probe.can_crop(0), "Crossing fallback contaminated other eye");
    require(!probe.can_crop(2), "Non-eye projection accepted");
    probe = {};
    require(!probe.matches(100, width, height) && !probe.can_crop(0), "Invalidated snapshot accepted");
    projection_test<float>();
    projection_test<double>();
    view_transition_test();
    fixed_view_diagnostic_test();
    submission_recovery_test();
    native_eye_crop_test();
    native_clone_submission_test();
    std::puts("PASS: crop geometry, view decisions, fail-closed submission, source identity, native clone validation and recovery");
    return 0;
} catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
