#pragma once

namespace vrmod {

enum class VolumetricFrameResolveResult { BASELINE, RESOLVED, SUPPRESSED, FAILED };

// Missing association cannot establish what is in the source image. Suppress
// that submission, but do not confuse it with a persistent resource failure.
inline VolumetricFrameResolveResult frame_resolve_result(bool modified, bool lost,
    bool snapshot_valid, bool resources_valid, bool resource_failure_latched) {
    if (!modified && !lost) return VolumetricFrameResolveResult::BASELINE;
    if (resource_failure_latched) return VolumetricFrameResolveResult::FAILED;
    if (lost || !snapshot_valid) return VolumetricFrameResolveResult::SUPPRESSED;
    if (!resources_valid) return VolumetricFrameResolveResult::FAILED;
    return VolumetricFrameResolveResult::RESOLVED;
}

} // namespace vrmod
