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
    std::puts("PASS: crop containment, asymmetric FOV/subimages, guard/fallback, float/double projection and pixel-center resolve");
    return 0;
} catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
