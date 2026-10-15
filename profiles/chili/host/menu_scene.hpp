#pragma once
#include "psprecomp/guest_memory.hpp"
#include <cstdint>
#include <string_view>
namespace chili {
// Whether the main menu (profile, mode and mission choice) is up, read from the game's scene tree: the
// root has a node Missions, whose child MainMenuScene has children only while the menu is loaded; in a
// mission it is empty. Scene nodes (Kapow's Node) keep their parent at +0x3C, first child at +0x40, next
// sibling at +0x44 and name at +0x48. A few guest loads per call.
class MenuScene {
public:
    // The running scene (Scene::SceneInstance); it is the root of the tree, or a node below it.
    static constexpr std::uint32_t scene_instance_address = 0x08DFAE8Cu;
    // `node`: any node of the tree, 0 for the running scene.
    bool loaded(const psprecomp::GuestMemory &memory, std::uint32_t node = 0u) {
        if (!scene_ || !named(memory, scene_, "MainMenuScene")) {
            scene_ = 0u;
            auto root = node ? node : word(memory, scene_instance_address);
            for (unsigned depth = 0; root && depth < 12u && word(memory, root + parent); ++depth) root = word(memory, root + parent);
            auto missions = root ? word(memory, root + child) : 0u;
            for (unsigned count = 0; missions && count < 64u && !named(memory, missions, "Missions"); ++count) missions = word(memory, missions + sibling);
            auto scene = missions && named(memory, missions, "Missions") ? word(memory, missions + child) : 0u;
            for (unsigned count = 0; scene && count < 16u && !named(memory, scene, "MainMenuScene"); ++count) scene = word(memory, scene + sibling);
            if (!scene || !named(memory, scene, "MainMenuScene")) return false;
            scene_ = scene;
        }
        return word(memory, scene_ + child) != 0u;
    }
    static constexpr std::uint32_t parent = 0x3Cu, child = 0x40u, sibling = 0x44u, name = 0x48u;
    static std::uint32_t word(const psprecomp::GuestMemory &memory, std::uint32_t address) {
        return memory.contains(address, 4u) ? memory.load32(address) : 0u;
    }
    static bool named(const psprecomp::GuestMemory &memory, std::uint32_t node, std::string_view text) {
        const auto at = word(memory, node + name);
        if (!at || !memory.contains(at, text.size() + 1u)) return false;
        for (std::size_t i = 0; i <= text.size(); ++i)
            if (memory.load8(at + static_cast<std::uint32_t>(i)) != (i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u)) return false;
        return true;
    }
private:
    std::uint32_t scene_{};
};
}
