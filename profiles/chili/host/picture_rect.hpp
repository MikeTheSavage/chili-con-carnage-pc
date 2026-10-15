#pragma once
// Where the game's 480x272 picture sits in the window's client area. One function for everything that needs
// it: the Direct3D 12 renderer puts the picture there (ge_gpu_backend_dx12.cpp, record_direct_present), the
// overlay pass draws inside it, and the mouse is mapped through it. No Windows header: the client size comes
// from the caller.
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace chili {
inline constexpr std::uint32_t picture_width = 480u, picture_height = 272u;

struct PictureRect {
    std::int32_t x{}, y{}, width{}, height{};
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }
    // Client pixels per PSP pixel.
    [[nodiscard]] constexpr float scale_x() const noexcept { return static_cast<float>(width) / static_cast<float>(picture_width); }
    [[nodiscard]] constexpr float scale_y() const noexcept { return static_cast<float>(height) / static_cast<float>(picture_height); }
    // A client pixel as a place in the picture, in PSP pixels (0..480, 0..272). False outside the picture:
    // the pointer is then over nothing. `px`, `py` are set either way (a drag may leave the picture).
    [[nodiscard]] constexpr bool to_picture(std::int32_t client_x, std::int32_t client_y, float &px, float &py) const noexcept {
        if (empty()) { px = py = 0.0f; return false; }
        // The middle of the client pixel.
        px = (static_cast<float>(client_x - x) + 0.5f) / scale_x();
        py = (static_cast<float>(client_y - y) + 0.5f) / scale_y();
        return client_x >= x && client_y >= y && client_x < x + width && client_y < y + height;
    }
    // A place in the picture as client pixels (not rounded).
    constexpr void to_client(float px, float py, float &client_x, float &client_y) const noexcept {
        client_x = static_cast<float>(x) + px * scale_x();
        client_y = static_cast<float>(y) + py * scale_y();
    }
};

// The picture's rectangle in a client area of `client_width` x `client_height`: the largest one of the
// picture's shape, centred, with black bars beside or above it; with `integer_scale` a whole multiple of
// 480x272 when one fits; without `preserve_aspect` the whole client area. `source_*` is the picture's size
// (the PSP's screen).
[[nodiscard]] inline PictureRect picture_rect(std::uint32_t client_width, std::uint32_t client_height, bool preserve_aspect = true,
                                              bool integer_scale = false, std::uint32_t source_width = picture_width,
                                              std::uint32_t source_height = picture_height) noexcept {
    if (client_width == 0u || client_height == 0u) return {};
    if (!preserve_aspect || source_width == 0u || source_height == 0u)
        return {0, 0, static_cast<std::int32_t>(client_width), static_cast<std::int32_t>(client_height)};
    double scale = std::min(static_cast<double>(client_width) / source_width, static_cast<double>(client_height) / source_height);
    if (integer_scale && scale >= 1.0) scale = std::floor(scale);
    const std::uint32_t width = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::llround(source_width * scale)), 1u, client_width);
    const std::uint32_t height = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::llround(source_height * scale)), 1u, client_height);
    return {static_cast<std::int32_t>((client_width - width) / 2u), static_cast<std::int32_t>((client_height - height) / 2u),
            static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}
}
