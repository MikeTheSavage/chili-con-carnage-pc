#pragma once
// The control styles: which keys, mouse buttons and pad buttons a player starts with ([Input] Layout of
// ChiliNative.ini). Each goes with one engine path ([Input] Scheme). tod_pc is the default:
// the keys and the mouse Total Overdose had on PC, for every action the two games share.
//
//   tod_pc  "Total Overdose PC"  Scheme dual_analog
//   modern  "Modern"             Scheme dual_analog (the port's own layout before tod_pc)
//   psp     "PSP original"       Scheme psp
//
// No Windows header: the settings table (settings_table.hpp) and the launcher's tests use it too.
#include <cstdint>
#include <optional>
#include <string_view>
namespace chili {
enum class InputLayout : std::uint8_t { TodPc, Modern, Psp };
inline constexpr InputLayout input_layouts[]{InputLayout::TodPc, InputLayout::Modern, InputLayout::Psp};
// The value of [Input] Layout.
constexpr const char *layout_name(InputLayout layout) noexcept {
    return layout == InputLayout::Modern ? "modern" : layout == InputLayout::Psp ? "psp" : "tod_pc";
}
// What the launcher and the game's page call it.
constexpr const char *layout_word(InputLayout layout) noexcept {
    return layout == InputLayout::Modern ? "Modern" : layout == InputLayout::Psp ? "PSP original" : "Total Overdose PC";
}
constexpr std::optional<InputLayout> layout_from_name(std::string_view name) noexcept {
    const auto same = [&](std::string_view word) {
        if (word.size() != name.size()) return false;
        for (std::size_t i = 0; i < word.size(); ++i) {
            const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
            if (c != word[i]) return false;
        }
        return true;
    };
    for (const auto layout : input_layouts) if (same(layout_name(layout))) return layout;
    return std::nullopt;
}
// The engine path a layout goes with: true for [Input] Scheme=psp.
constexpr bool layout_is_psp(InputLayout layout) noexcept { return layout == InputLayout::Psp; }
}
