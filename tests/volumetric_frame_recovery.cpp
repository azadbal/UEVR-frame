#include <cstdio>
#include <stdexcept>
#include "../src/mods/vr/VolumetricFrameResolve.hpp"

using Result = vrmod::VolumetricFrameResolveResult;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    bool failed = false;
    auto submit = [&](bool modified, bool lost, bool snapshot, bool resources) {
        const auto result = vrmod::frame_resolve_result(modified, lost, snapshot, resources, failed);
        if (result == Result::FAILED) failed = true;
        return result;
    };
    require(submit(true, false, true, true) == Result::RESOLVED, "Initial crop did not resolve");
    // Hogwarts log: a prior crop, then lost=true/mask=0 during loading.
    require(submit(false, true, false, false) == Result::SUPPRESSED, "Unassociated pixels escaped");
    require(!failed, "Loading permanently disabled cropping");
    require(submit(false, true, false, false) == Result::SUPPRESSED, "Repeated loading frame escaped");
    require(submit(true, false, true, true) == Result::RESOLVED, "Fresh crop failed to recover");
    require(submit(false, false, true, true) == Result::BASELINE, "Known full view failed to recover");
    require(submit(true, false, false, true) == Result::SUPPRESSED, "Invalid modified snapshot escaped");
    require(submit(true, false, true, false) == Result::FAILED, "Resource failure did not latch");
    require(submit(true, false, true, true) == Result::FAILED, "Resource failure recovered without reset");
    require(submit(false, true, false, false) == Result::FAILED, "Latched resource failure mislabeled transient");
    require(submit(false, false, true, true) == Result::BASELINE, "Known baseline blocked by crop resource error");
    failed = false; // Explicit component reset permits reinitialization.
    require(submit(true, false, true, true) == Result::RESOLVED, "Reset failed to recover");
    std::puts("PASS: loading suppression, fresh-frame recovery, persistent resource failure and reset");
    return 0;
} catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
