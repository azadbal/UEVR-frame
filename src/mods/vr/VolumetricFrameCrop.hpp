#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/glm.hpp>

namespace vrmod {

// Integer pixels in the full output eye; right/bottom edges are exclusive.
struct VolumetricFramePixelRect {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

enum class VolumetricFrameCropFallback { NONE, INVALID_INPUT, EYE_PLANE, OFFSCREEN };

struct VolumetricFrameCrop {
    VolumetricFramePixelRect rect;
    VolumetricFrameCropFallback fallback{VolumetricFrameCropFallback::INVALID_INPUT};
};

inline VolumetricFrameCrop calculate_volumetric_frame_crop(
    const glm::mat4& frame_pose, const glm::vec2& frame_size,
    const glm::mat4& eye_pose, const glm::vec4& raw_fov_tangents,
    VolumetricFramePixelRect submitted, int full_width, int full_height
) {
    VolumetricFrameCrop result{{0, 0, (std::max)(0, full_width), (std::max)(0, full_height)}};
    if (full_width <= 0 || full_height <= 0 || submitted.x < 0 || submitted.y < 0 ||
        submitted.x >= full_width || submitted.y >= full_height ||
        submitted.width <= 0 || submitted.height <= 0 ||
        submitted.width > full_width - submitted.x || submitted.height > full_height - submitted.y ||
        !std::isfinite(frame_size.x) || !std::isfinite(frame_size.y) || frame_size.x <= 0 || frame_size.y <= 0) {
        return result;
    }
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(raw_fov_tangents[i])) return result;
        for (int j = 0; j < 4; ++j) {
            if (!std::isfinite(frame_pose[i][j]) || !std::isfinite(eye_pose[i][j])) return result;
        }
        const float expected = i == 3 ? 1.0f : 0.0f;
        if (std::abs(frame_pose[i][3] - expected) > 1e-5f ||
            std::abs(eye_pose[i][3] - expected) > 1e-5f) return result;
    }
    // Raw runtime FOV is ordered left, right, up, down; +Y is up and -Z forward.
    const glm::dvec4 fov{raw_fov_tangents};
    if (fov.y <= fov.x || fov.z <= fov.w ||
        std::abs(glm::determinant(glm::dmat4{eye_pose})) < 1e-8 ||
        std::abs(glm::determinant(glm::dmat4{frame_pose})) < 1e-8) return result;
    const auto frame_to_eye = glm::inverse(glm::dmat4{eye_pose}) * glm::dmat4{frame_pose};
    glm::dvec2 minimum{std::numeric_limits<double>::infinity()};
    glm::dvec2 maximum{-std::numeric_limits<double>::infinity()};
    for (int corner = 0; corner < 4; ++corner) {
        const auto p = frame_to_eye * glm::dvec4{
            (corner & 1 ? 0.5 : -0.5) * frame_size.x,
            (corner & 2 ? 0.5 : -0.5) * frame_size.y, 0.0, 1.0};
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return result;
        if (p.z >= -1e-5) {
            result.fallback = VolumetricFrameCropFallback::EYE_PLANE;
            return result;
        }
        const glm::dvec2 pixel{
            submitted.x + (p.x / -p.z - fov.x) / (fov.y - fov.x) * submitted.width,
            submitted.y + (p.y / -p.z - fov.z) / (fov.w - fov.z) * submitted.height};
        if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y)) return result;
        minimum = (glm::min)(minimum, pixel);
        maximum = (glm::max)(maximum, pixel);
    }
    if (maximum.x <= submitted.x || maximum.y <= submitted.y ||
        minimum.x >= submitted.x + submitted.width || minimum.y >= submitted.y + submitted.height) {
        result.fallback = VolumetricFrameCropFallback::OFFSCREEN;
        return result;
    }
    // Clamp in floating point before converting, including extremely distant bounds.
    constexpr double guard = 16.0;
    const int x0 = static_cast<int>(std::clamp(std::floor(minimum.x) - guard,
        double(submitted.x), double(submitted.x + submitted.width)));
    const int y0 = static_cast<int>(std::clamp(std::floor(minimum.y) - guard,
        double(submitted.y), double(submitted.y + submitted.height)));
    const int x1 = static_cast<int>(std::clamp(std::ceil(maximum.x) + guard,
        double(submitted.x), double(submitted.x + submitted.width)));
    const int y1 = static_cast<int>(std::clamp(std::ceil(maximum.y) + guard,
        double(submitted.y), double(submitted.y + submitted.height)));
    result.rect = {x0, y0, x1 - x0, y1 - y0};
    result.fallback = VolumetricFrameCropFallback::NONE;
    return result;
}

// GLM uses [column][row] and column vectors. Crop the actual engine projection,
// including its symmetry overrides, without changing homogeneous depth or W.
template<typename T, glm::qualifier Q>
inline glm::mat<4, 4, T, Q> crop_volumetric_frame_projection(
    const glm::mat<4, 4, T, Q>& baseline, VolumetricFramePixelRect rect, int width, int height
) {
    if (width <= 0 || height <= 0 || rect.x < 0 || rect.y < 0 ||
        rect.x >= width || rect.y >= height || rect.width <= 0 || rect.height <= 0 ||
        rect.width > width - rect.x || rect.height > height - rect.y) return baseline;
    const T sx = T(width) / T(rect.width);
    const T sy = T(height) / T(rect.height);
    const T tx = (T(width) - T(2) * T(rect.x) - T(rect.width)) / T(rect.width);
    const T ty = (T(2) * T(rect.y) + T(rect.height) - T(height)) / T(rect.height);
    auto cropped = baseline;
    for (int column = 0; column < 4; ++column) {
        cropped[column][0] = sx * baseline[column][0] + tx * baseline[column][3];
        cropped[column][1] = sy * baseline[column][1] + ty * baseline[column][3];
    }
    return cropped;
}

} // namespace vrmod
