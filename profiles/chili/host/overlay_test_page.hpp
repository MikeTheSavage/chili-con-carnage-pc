#pragma once
// A fixed page for testing the overlay pass (gpu/ge_gpu_backend.hpp, page_draw.hpp): nothing a player sees.
// Only a test variable brings it up:
//
//   CHILI_OVERLAY_TEST=<page>[@<first frame>[-<last frame>]]
//       page    a headline, the tilted category line, three rows (plain, selected with a value that waits for
//               a restart, locked), a row with a slider bar, a reason line and three legends with button
//               pictures, on the menu's background: the look of the game's own Settings screen
//       pause   the same rows in the pause menu's black box, from the font sheets alone
//       glyphs  every glyph of the font in rows, the menu's sprites and plain shapes
//       echo    the game's own Profile Options screen drawn again over itself: the background, then every
//               text at the place and in the style its text box has in the menu scene, the legends' sprites.
//               A frame picture at internal scale 1 with it is compared with one without: where the two agree
//               the page's text is the game's text, pixel for pixel
//   CHILI_OVERLAY_SHOTS=<folder>@<frame>[,<frame>...]
//       the window's own picture at those game frames (the back buffer: page, black bars and all, at the
//       client area's size) as <folder>\window_<frame>.png. The frame pictures of CHILI_FRAME_PNG_EVERY show
//       the displayed surface with the page at the internal resolution instead.
//   CHILI_OVERLAY_FULLSCREEN=<frame>
//       borderless full screen from that frame on (a scripted run's window stays behind the others)
//   CHILI_OVERLAY_SIZE=<frame>:<width>x<height>
//       the client area gets that size at that frame
//
// With the variable set the page's textures are watched from the start (they get content keys), in any kind
// of run; a deterministic run stays repeatable, the page reads guest memory and writes none. Without it this
// class does nothing at all.
#include "ge_runtime.hpp"
#include "gpu_video.hpp"
#include "page_draw.hpp"
#include "page_font_guest.hpp"
#include "presentation.hpp"
#include "window_picture.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
namespace chili {
class OverlayTestPage {
public:
    // Call after the texture mods have started and before the first draw. Returns the lines for the log.
    std::vector<std::string> start(bool gpu) {
        std::vector<std::string> lines;
        const auto variable = [](const char *name) { const char *value = std::getenv(name); return std::string(value ? value : ""); };
        const std::string asked = variable("CHILI_OVERLAY_TEST");
        if (asked.empty()) return lines;
        if (!gpu) { lines.push_back("CHILI_OVERLAY_TEST: the software renderer draws no page"); return lines; }
        const auto at = asked.find('@');
        kind_ = asked.substr(0u, at);
        if (kind_ != "page" && kind_ != "pause" && kind_ != "glyphs" && kind_ != "echo") { lines.push_back("CHILI_OVERLAY_TEST must be page, pause, glyphs or echo: '" + kind_ + "' ignored"); return lines; }
        if (at != std::string::npos) {
            const std::string range = asked.substr(at + 1u);
            const auto dash = range.find('-');
            first_ = std::strtoull(range.c_str(), nullptr, 10);
            if (dash != std::string::npos) last_ = std::strtoull(range.c_str() + dash + 1u, nullptr, 10);
        }
        if (const std::string shots = variable("CHILI_OVERLAY_SHOTS"); !shots.empty()) {
            const auto split = shots.rfind('@');
            if (split == std::string::npos) lines.push_back("CHILI_OVERLAY_SHOTS must be <folder>@<frame>[,<frame>...]");
            else {
                shot_folder_ = shots.substr(0u, split);
                for (const char *text = shots.c_str() + split + 1u; *text;) {
                    char *end = nullptr;
                    const auto frame = std::strtoull(text, &end, 10);
                    if (end == text) break;
                    shots_.push_back(frame);
                    text = *end == ',' ? end + 1 : end;
                }
                std::error_code status;
                std::filesystem::create_directories(shot_folder_, status);
            }
        }
        if (const std::string full = variable("CHILI_OVERLAY_FULLSCREEN"); !full.empty()) fullscreen_at_ = std::strtoull(full.c_str(), nullptr, 10);
        if (const std::string size = variable("CHILI_OVERLAY_SIZE"); !size.empty()) {
            unsigned long long frame = 0u; unsigned width = 0u, height = 0u;
            if (std::sscanf(size.c_str(), "%llu:%ux%u", &frame, &width, &height) == 3 && width && height) resize_ = Resize{frame, width, height};
            else lines.push_back("CHILI_OVERLAY_SIZE must be <frame>:<width>x<height>");
        }
        vcs::ge_gpu_backend_watch_overlay_textures(page_textures);
        active_ = true;
        lines.push_back("test page '" + kind_ + "' from frame " + std::to_string(first_) + (last_ == forever ? std::string() : " to " + std::to_string(last_)) +
            (shots_.empty() ? std::string() : ", " + std::to_string(shots_.size()) + " window pictures into " + shot_folder_.string()));
        return lines;
    }
    [[nodiscard]] bool active() const noexcept { return active_; }

    // At every frame start, on the game thread, with the number of the frame that ended.
    void frame(std::uint64_t frame, const psprecomp::GuestMemory &memory, GeRuntime &ge, Presentation &presentation) {
        if (!active_) return;
        // The window's picture asked for a frame ago: there once a present has happened.
        if (pending_shot_) {
            vcs::GeGpuWindowCapture capture;
            bool taken = false;
            ge.render_call([&capture, &taken] { taken = vcs::ge_gpu_backend_take_window_capture(capture); }, true);
            if (taken) { save_window(*pending_shot_, frame, capture); pending_shot_.reset(); }
            else if (++shot_waits_ > 30u) { std::printf("[overlay-test] frame %llu: no present came for the window picture of frame %llu\n",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(*pending_shot_)); pending_shot_.reset(); }
        }
        // The glyph table, once the game has drawn text (its font is loaded then).
        if (!font_.loaded() && frame >= next_scan_ && vcs::ge_gpu_backend_overlay_texture_ready(page_texture::font_fill)) {
            next_scan_ = frame + 15u;
            constexpr std::uint32_t base = page_font_memory_base;
            const auto started = std::chrono::steady_clock::now();
            if (load_page_font(memory, font_)) {
                unsigned pictures = 0u;
                for (const auto &glyph : font_.glyphs()) pictures += glyph.picture ? 1u : 0u;
                std::printf("[overlay-test] frame %llu: font table at 0x%08X: %zu glyphs (%u pictures), cell height %.0f, %s; found in %.1f ms\n",
                    static_cast<unsigned long long>(frame), base + static_cast<std::uint32_t>(font_.table_offset()), font_.glyphs().size(), pictures,
                    font_.cell_height(), font_.header_found() ? "length from the font record" : "no font record found, length by the entries",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
                std::fflush(stdout);
            }
        }
        const bool show = frame >= first_ && frame <= last_ && font_.loaded();
        if (show) {
            const auto started = std::chrono::steady_clock::now();
            PageDraw draw;
            if (kind_ == "pause") build_pause(draw, frame);
            else if (kind_ == "glyphs") build_glyphs(draw);
            else if (kind_ == "echo") build_echo(draw);
            else build_page(draw, frame);
            const std::size_t quads = draw.quads();
            auto list = draw.finish();
            const std::size_t runs = list->runs.size(), vertices = list->vertices.size();
            vcs::ge_gpu_backend_set_overlay(std::move(list));
            const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
            build_ns_ += ns; build_max_ns_ = std::max(build_max_ns_, ns); ++builds_;
            const std::uint32_t ready = ready_mask();
            if (!shown_ || ready != ready_) {
                std::printf("[overlay-test] frame %llu: page '%s' %s: %zu rectangles in %zu runs, %zu vertices; in the cache:%s\n",
                    static_cast<unsigned long long>(frame), kind_.c_str(), shown_ ? "(textures changed)" : "up", quads, runs, vertices, ready_text(ready).c_str());
                std::fflush(stdout);
            }
            shown_ = true; ready_ = ready;
        } else if (shown_) {
            vcs::ge_gpu_backend_set_overlay(nullptr);
            shown_ = false;
            std::printf("[overlay-test] frame %llu: page down\n", static_cast<unsigned long long>(frame));
            std::fflush(stdout);
        }
        if (fullscreen_at_ && frame == *fullscreen_at_) {
            presentation.set_fullscreen(true);
            RECT client{};
            if (const HWND window = presentation.window_handle(); window && GetClientRect(window, &client)) {
                const PictureRect picture = window_picture_rect(window);
                std::printf("[overlay-test] frame %llu: borderless full screen, client area %ldx%ld, the picture for the pointer at (%d, %d) %dx%d; the window in front is %s\n",
                    static_cast<unsigned long long>(frame), client.right, client.bottom, picture.x, picture.y, picture.width, picture.height,
                    GetForegroundWindow() == window ? "this one" : "another one, as before");
            }
        }
        if (resize_ && frame == resize_->frame) {
            if (const HWND window = presentation.window_handle()) {
                RECT size{0, 0, static_cast<LONG>(resize_->width), static_cast<LONG>(resize_->height)};
                AdjustWindowRectExForDpi(&size, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                    static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
                SetWindowPos(window, nullptr, 0, 0, size.right - size.left, size.bottom - size.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                RECT client{};
                GetClientRect(window, &client);
                const PictureRect picture = window_picture_rect(window);
                std::printf("[overlay-test] frame %llu: client area %ldx%ld, the picture for the pointer at (%d, %d) %dx%d\n",
                    static_cast<unsigned long long>(frame), client.right, client.bottom, picture.x, picture.y, picture.width, picture.height);
            }
        }
        // The picture the window gets in the frame that starts now.
        if (!pending_shot_ && std::find(shots_.begin(), shots_.end(), frame + 1u) != shots_.end()) {
            ge.render_call([] { vcs::ge_gpu_backend_request_window_capture(); }, false);
            pending_shot_ = frame + 1u; shot_waits_ = 0u;
        }
    }

    // The lines for the end of the log: what the pass did and what it cost.
    std::vector<std::string> report() {
        std::vector<std::string> lines;
        if (!active_ || reported_) return lines;
        reported_ = true;
        const auto stats = vcs::ge_gpu_backend_overlay_stats();
        // What watching the page's textures cost, page or no page: the content key of every texture of their shapes.
        const auto keyed = vcs::ge_texture_mods_report();
        char text[640];
        std::snprintf(text, sizeof(text), "presents with the page: %llu (%llu runs, %llu vertices, %llu runs left out for a texture not in the cache); "
            "recording a page: mean %.1f us, longest %.1f us; the card's time for a page: %llu samples, mean %.3f ms, longest %.3f ms; "
            "building a list on the game thread: %llu times, mean %.1f us, longest %.1f us; window pictures written: %u; "
            "textures keyed because the page's shapes are watched: %llu in %.2f ms (longest %.3f ms)",
            static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(stats.runs), static_cast<unsigned long long>(stats.vertices),
            static_cast<unsigned long long>(stats.runs_without_texture),
            stats.presents ? static_cast<double>(stats.record_ns) / 1000.0 / static_cast<double>(stats.presents) : 0.0, static_cast<double>(stats.record_max_ns) / 1000.0,
            static_cast<unsigned long long>(stats.gpu_samples), stats.gpu_mean_ms, stats.gpu_max_ms,
            static_cast<unsigned long long>(builds_), builds_ ? static_cast<double>(build_ns_) / 1000.0 / static_cast<double>(builds_) : 0.0,
            static_cast<double>(build_max_ns_) / 1000.0, shots_written_,
            static_cast<unsigned long long>(keyed.keyed_textures), static_cast<double>(keyed.keying_ns) / 1.0e6, static_cast<double>(keyed.keying_max_ns) / 1.0e6);
        lines.emplace_back(text);
        return lines;
    }

private:
    static constexpr std::uint64_t forever = ~std::uint64_t{0};
    struct Resize { std::uint64_t frame; unsigned width, height; };

    static bool ready(std::uint64_t key) { return vcs::ge_gpu_backend_overlay_texture_ready(key); }
    static std::uint32_t ready_mask() {
        std::uint32_t mask = 0u;
        for (std::size_t i = 0; i < page_textures.size(); ++i) if (ready(page_textures[i].key)) mask |= 1u << i;
        return mask;
    }
    static std::string ready_text(std::uint32_t mask) {
        static constexpr const char *names[]{"font", "font outline", "BackGround01", "BackGround03", "GameLogo", "white", "ButtonCross", "ButtonCircle",
            "ButtonSquare", "ButtonTriangle", "ButtonDpadLR"};
        std::string in, out;
        for (std::size_t i = 0; i < page_textures.size(); ++i) ((mask >> i) & 1u ? in : out) += std::string(" ") + names[i] + ",";
        if (!in.empty()) in.pop_back();
        if (!out.empty()) { out.pop_back(); in += "; not in the cache:" + out; }
        return in;
    }
    // The selected line's colour: between dark and bright red, one round in two seconds as in the game's menu,
    // counted in frames of a thirtieth of a second so that a deterministic run repeats.
    static PageColour pulse(std::uint64_t frame) {
        const float phase = static_cast<float>(frame % 60u) / 60.0f, t = 0.5f - 0.5f * std::cos(phase * 6.2831853f);
        return {page_colour::selected_dark.r + (page_colour::selected_bright.r - page_colour::selected_dark.r) * t, 0.0f, 0.0f, 1.0f};
    }
    // A legend: the 32 x 32 sprite when the cache has it, else the font's picture of the same button.
    void button(PageDraw &draw, std::uint64_t sprite, char32_t glyph, float x, float y) const {
        if (ready(sprite)) draw.sprite(sprite, {x, y, x + 32.0f, y + 32.0f});
        else draw.glyph(font_, glyph, {x, y + 4.0f, x + 32.0f, y + 28.0f});
    }

    void build_page(PageDraw &draw, std::uint64_t frame) const {
        // Behind: the menu's background and the small logo, as the game's Settings screen has them.
        if (ready(page_texture::background_page)) draw.sprite(page_texture::background_page, {0.0f, 0.0f, 480.0f, 272.0f});
        else if (ready(page_texture::background_main)) draw.sprite(page_texture::background_main, {0.0f, 0.0f, 480.0f, 272.0f});
        else draw.rect({0.0f, 0.0f, 480.0f, 272.0f}, {0.16f, 0.05f, 0.02f});
        if (ready(page_texture::logo)) draw.sprite(page_texture::logo, {-23.0f, 0.0f, 157.0f, 90.0f});
        draw.text(font_, "Display", 120.0f, 20.0f, PageTextStyle::headline());
        draw.text(font_, "Settings", 20.0f, 50.0f, PageTextStyle::category());

        const float name_x = 74.5f, value_right = 391.0f;
        const auto row_y = [](unsigned row) { return 72.0f + 21.0f * static_cast<float>(row); };
        const auto value_at = [&](std::string_view text, unsigned row, PageTextStyle style) {
            style.align = PageAlign::Right; style.width = 200.0f;
            draw.text(font_, text, value_right - style.width, row_y(row), style);
        };
        // A plain row.
        draw.text(font_, "Screen", name_x, row_y(0u), PageTextStyle::line());
        value_at("Window", 0u, PageTextStyle::value());
        // The selected row, 10 pixels to the right, its value waiting for a restart: hot orange with the chili.
        PageTextStyle selected = PageTextStyle::line();
        selected.fill = pulse(frame);
        draw.text(font_, "Resolution", name_x + 10.0f, row_y(1u), selected);
        PageTextStyle pending = PageTextStyle::value();
        pending.fill = page_colour::headline;
        value_at("1920 x 1088 ~", 1u, pending);
        // A locked row: every colour halved, the closed lock after the name.
        PageTextStyle locked = PageTextStyle::line();
        locked.fill = locked.fill.dimmed(0.5f); locked.outline = locked.outline.dimmed(0.5f); locked.picture_tint = 0.5f;
        draw.text(font_, "Frame rate }", name_x, row_y(2u), locked);
        PageTextStyle locked_value = PageTextStyle::value();
        locked_value.fill = locked_value.fill.dimmed(0.5f);
        value_at("60 fps", 2u, locked_value);
        // A slider: the number at the right, the bar in front of it.
        draw.text(font_, "Turn speed", name_x, row_y(3u), PageTextStyle::line());
        value_at("0.35", 3u, PageTextStyle::value());
        const float bar_left = 236.0f, bar_right = 336.0f, bar_y = row_y(3u) + 9.0f, share = 0.35f;
        draw.rect({bar_left - 1.0f, bar_y - 1.0f, bar_right + 3.0f, bar_y + 7.0f}, page_colour::black);                       // shadow
        draw.rect({bar_left - 1.0f, bar_y - 1.0f, bar_right + 1.0f, bar_y + 5.0f}, page_colour::line_outline);                // edge
        draw.rect({bar_left, bar_y, bar_right, bar_y + 4.0f}, page_colour::line.dimmed(0.25f));                                // track
        draw.rect({bar_left, bar_y, bar_left + (bar_right - bar_left) * share, bar_y + 4.0f}, page_colour::line);             // filled part
        const float knob = bar_left + (bar_right - bar_left) * share;
        draw.rect({knob - 2.0f, bar_y - 4.0f, knob + 4.0f, bar_y + 10.0f}, page_colour::black);
        draw.rect({knob - 3.0f, bar_y - 5.0f, knob + 3.0f, bar_y + 9.0f}, page_colour::line_outline);
        draw.rect({knob - 2.0f, bar_y - 4.0f, knob + 2.0f, bar_y + 8.0f}, page_colour::white);
        // The list goes on: the pointers at the right edge.
        const PageColour gold{1.0f, 0.82f, 0.2f};
        draw.triangle({398.0f, 76.0f}, {410.0f, 76.0f}, {404.0f, 68.0f}, page_colour::black);
        draw.triangle({399.5f, 75.0f}, {408.5f, 75.0f}, {404.0f, 69.5f}, gold);
        draw.triangle({398.0f, 156.0f}, {410.0f, 156.0f}, {404.0f, 164.0f}, page_colour::black);
        draw.triangle({399.5f, 157.0f}, {408.5f, 157.0f}, {404.0f, 162.5f}, gold);
        // The reason of a locked row.
        PageTextStyle reason = PageTextStyle::legend();
        reason.fill = page_colour::headline; reason.outline = page_colour::headline_outline;
        draw.text(font_, "Needs a restart from the main menu.", name_x, 214.0f, reason);
        // Legends: Back at the left, Restart in the middle, Change at the right.
        button(draw, page_texture::button_circle, page_glyph::circle, 8.0f, 232.0f);
        draw.text(font_, "Back", 8.0f + 36.0f, 232.0f + 7.0f, PageTextStyle::legend());
        button(draw, page_texture::button_triangle, page_glyph::triangle, 200.0f, 232.0f);
        draw.text(font_, "Restart", 200.0f + 36.0f, 232.0f + 7.0f, PageTextStyle::legend());
        button(draw, page_texture::button_dpad_lr, page_glyph::dpad_right, 442.0f, 234.0f);
        PageTextStyle right = PageTextStyle::legend();
        right.align = PageAlign::Right; right.width = 96.0f;
        draw.text(font_, "Change", 442.0f - 102.0f, 234.0f + 5.0f, right);
    }

    void build_pause(PageDraw &draw, std::uint64_t frame) const {
        // The pause menu's box, opaque: the game's own lines are under it.
        const PageBox box{111.0f, 16.5f, 111.0f + 272.0f, 16.5f + 218.0f};
        draw.rect(box, page_colour::black);
        PageTextStyle headline;
        headline.scale = 0.9f; headline.lift = 6.0f; headline.fill = page_colour::pause_headline; headline.outlined = false;
        headline.shadow_x = headline.shadow_y = 3.0f; headline.align = PageAlign::Centre; headline.width = box.width();
        draw.text(font_, "Controls", box.x0, 28.0f, headline);
        const float name_x = 128.0f, value_right = 366.0f;
        const auto row_y = [](unsigned row) { return 56.0f + 19.0f * static_cast<float>(row); };
        PageTextStyle name = PageTextStyle::legend();
        PageTextStyle value = PageTextStyle::value();
        value.scale = 0.7f; value.lift = 7.0f; value.align = PageAlign::Right; value.width = 150.0f;
        static constexpr std::pair<const char *, const char *> rows[]{{"Turn speed", "0.35"}, {"Invert up/down", "off"}, {"Hold the aim", "on"},
            {"Precise aim", "on"}, {"Smoothing", "0.25"}, {"Grab the mouse", "On click"}, {"Defaults", ""}};
        for (unsigned row = 0; row < std::size(rows); ++row) {
            PageTextStyle style = name;
            if (row == 1u) style.fill = pulse(frame);
            draw.text(font_, rows[row].first, name_x + (row == 1u ? 10.0f : 0.0f), row_y(row), style);
            if (*rows[row].second) draw.text(font_, rows[row].second, value_right - value.width, row_y(row), value);
        }
        // Two legends at the pause menu's own places, their buttons from the font's pictures.
        std::string back = page_text(page_glyph::circle) + " Back", change = page_text(page_glyph::dpad_left) + page_text(page_glyph::dpad_right) + " Change";
        draw.text(font_, back, 117.0f, 197.0f + 7.0f, PageTextStyle::legend());
        PageTextStyle right = PageTextStyle::legend();
        right.align = PageAlign::Right; right.width = 140.0f;
        draw.text(font_, change, 376.0f - right.width, 197.0f + 7.0f, right);
    }

    void build_glyphs(PageDraw &draw) const {
        draw.rect({0.0f, 0.0f, 480.0f, 272.0f}, {0.10f, 0.10f, 0.12f});
        // Every glyph in table order, 26 to a row, at the menu line's scale.
        PageTextStyle style = PageTextStyle::line();
        style.spacing = -2.0f;
        std::string line;
        unsigned in_line = 0u, row = 0u;
        for (const auto &glyph : font_.glyphs()) {
            page_append(line, glyph.code);
            if (++in_line == 26u) { draw.text(font_, line, 6.0f, 6.0f + 24.0f * static_cast<float>(row++), style); line.clear(); in_line = 0u; }
        }
        if (!line.empty()) draw.text(font_, line, 6.0f, 6.0f + 24.0f * static_cast<float>(row++), style);
        // The sprites and the other textures a page may draw with, each where the cache has it, and plain shapes.
        const float y = 6.0f + 24.0f * static_cast<float>(row) + 4.0f;
        float x = 6.0f;
        for (const std::uint64_t key : {page_texture::button_cross, page_texture::button_circle, page_texture::button_square,
                                        page_texture::button_triangle, page_texture::button_dpad_lr, page_texture::white}) {
            if (ready(key)) draw.sprite(key, {x, y, x + 32.0f, y + 32.0f});
            else draw.rect({x + 8.0f, y + 8.0f, x + 24.0f, y + 24.0f}, {0.5f, 0.0f, 0.5f});
            x += 36.0f;
        }
        if (ready(page_texture::logo)) draw.sprite(page_texture::logo, {x, y, x + 64.0f, y + 32.0f});
        x += 68.0f;
        if (ready(page_texture::background_page)) draw.sprite(page_texture::background_page, {x, y, x + 56.0f, y + 32.0f});
        x += 60.0f;
        if (ready(page_texture::background_main)) draw.sprite(page_texture::background_main, {x, y, x + 56.0f, y + 32.0f});
        x += 60.0f;
        draw.rect({x, y, x + 20.0f, y + 32.0f}, {1.0f, 1.0f, 1.0f, 0.5f});
        draw.triangle({x + 24.0f, y + 32.0f}, {x + 44.0f, y + 32.0f}, {x + 34.0f, y}, {0.2f, 0.8f, 0.3f});
        // Clipped: only the middle of the word shows.
        draw.clip({400.0f, y + 6.0f, 440.0f, y + 20.0f});
        draw.text(font_, "Clipped text", 380.0f, y + 6.0f, PageTextStyle::line());
        draw.unclip();
    }

    // Profile Options as the menu scene has it (SubMenu_Profile: positions, sizes and styles of its text boxes).
    void build_echo(PageDraw &draw) const {
        draw.sprite(page_texture::background_page, {0.0f, 0.0f, 480.0f, 272.0f});
        // The logo where the game's draw puts it: a pixel wider on each side than its node's 180 x 90 at (-23, 0).
        draw.sprite(page_texture::logo, {-24.0f, 0.0f, 158.0f, 90.0f});
        draw.text(font_, "Profile Options", 120.0f, 20.0f, PageTextStyle::headline());
        draw.text(font_, "Profile: RAM", 20.0f, 50.0f, PageTextStyle::category());
        static constexpr struct { const char *text; float x, y; } lines[]{{"Create new profile", 208.0f, 58.0f}, {"Save profile", 195.3f, 82.67f},
            {"Load profile", 183.0f, 107.7f}, {"Delete save games", 169.0f, 133.0f}, {"Sync Highscores", 156.0f, 158.0f}, {"Trophy room", 143.0f, 183.0f},
            {"Settings", 130.0f, 208.0f}};
        for (const auto &line : lines) draw.text(font_, line.text, line.x, line.y, PageTextStyle::line());
        draw.sprite(page_texture::button_circle, {8.0f, 232.0f, 40.0f, 264.0f});
        draw.text(font_, "Back", 8.0f + 36.0f, 232.0f + 7.0f, PageTextStyle::legend());
        draw.sprite(page_texture::button_cross, {440.0f, 232.0f, 472.0f, 264.0f});
        PageTextStyle right = PageTextStyle::legend();
        right.align = PageAlign::Right; right.width = 96.0f;
        draw.text(font_, "Select", 440.0f - 100.0f, 232.0f + 7.0f, right);
    }

    void save_window(std::uint64_t wanted, std::uint64_t frame, const vcs::GeGpuWindowCapture &capture) {
        std::vector<unsigned> pixels(static_cast<std::size_t>(capture.width) * capture.height);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const std::byte *p = capture.rgba8.data() + i * 4u;
            pixels[i] = 0xFF000000u | (std::to_integer<unsigned>(p[0]) << 16u) | (std::to_integer<unsigned>(p[1]) << 8u) | std::to_integer<unsigned>(p[2]);
        }
        char name[48];
        std::snprintf(name, sizeof(name), "window_%08llu.png", static_cast<unsigned long long>(wanted));
        const bool written = write_png(shot_folder_ / name, capture.width, capture.height, pixels);
        shots_written_ += written ? 1u : 0u;
        std::printf("[overlay-test] frame %llu: window picture of frame %llu %s: %ux%u, the game's picture at (%d, %d) %dx%d\n",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(wanted), written ? "written" : "NOT written", capture.width, capture.height,
            capture.picture_x, capture.picture_y, capture.picture_width, capture.picture_height);
        std::fflush(stdout);
    }

    PageFont font_;
    std::string kind_;
    std::uint64_t first_{}, last_{forever}, next_scan_{};
    std::filesystem::path shot_folder_;
    std::vector<std::uint64_t> shots_;
    std::optional<std::uint64_t> pending_shot_, fullscreen_at_;
    std::optional<Resize> resize_;
    std::uint64_t build_ns_{}, build_max_ns_{}, builds_{};
    std::uint32_t ready_{};
    unsigned shot_waits_{}, shots_written_{};
    bool active_{}, shown_{}, reported_{};
};
}
