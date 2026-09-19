// cl /nologo /EHsc /std:c++17 /Idependencies/submodules/glm tests/volumetric_frame_anchor.cpp /Febuild/frame-anchor-test.exe /Fobuild/frame-anchor-test.obj
#include <cstdio>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include "../src/mods/vr/VolumetricFrameLayout.hpp"

using vrmod::VolumetricFrameAnchorSource;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    const auto ui = glm::translate(glm::mat4{1}, glm::vec3{1, 1.6f, -2});
    for (const float yaw : {-0.8f, 0.0f, 1.2f}) {
        auto head = glm::rotate(glm::mat4{1}, yaw, glm::vec3{0,1,0});
        head[3] = glm::vec4{0.4f, 1.7f, 0.3f, 1};
        // Existing frame already active; user switches matching OFF -> ON.
        const auto source = vrmod::volumetric_frame_anchor_source(true, false, true, false);
        const auto selected = source == VolumetricFrameAnchorSource::UI ? ui : head;
        require(selected == ui, "Match Game UI uses current head instead of normal UI placement");
    }
    std::puts("PASS: enabling UI matching is independent of head position and yaw");

    require(vrmod::volumetric_frame_anchor_source(true, true, true, true) == VolumetricFrameAnchorSource::UI,
        "Initial automatic placement must match UI, including an old pending recenter");
    require(vrmod::volumetric_frame_anchor_source(true, false, false, true) == VolumetricFrameAnchorSource::HEAD,
        "Explicit recenter must still use current head");
    require(vrmod::volumetric_frame_anchor_source(true, false, false, false) == VolumetricFrameAnchorSource::KEEP,
        "Ordinary head motion must not reanchor an active matched frame");
    require(vrmod::volumetric_frame_anchor_source(false, false, false, true) == VolumetricFrameAnchorSource::HEAD,
        "Manual recenter must still use current head");
    require(vrmod::volumetric_frame_anchor_source(false, true, false, false) == VolumetricFrameAnchorSource::HEAD,
        "Initial manual placement must use current head");
    require(vrmod::volumetric_frame_anchor_source(false, false, true, false) == VolumetricFrameAnchorSource::KEEP,
        "Switching to manual controls must not recenter the frame");
    std::puts("PASS: initial placement, explicit recenter and anchored motion transitions");
    return 0;
} catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
}
