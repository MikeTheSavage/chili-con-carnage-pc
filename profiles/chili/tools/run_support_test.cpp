#include "input_pipeline.hpp"
#include "frame_timer.hpp"
#include "state_hash.hpp"
#include "png_writer.hpp"
#include "pcm_queue.hpp"
#include "scripted_dialogs.hpp"
#include "run_options.hpp"
#include "pc_window.hpp"
#include "menu_scene.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
namespace lc = chili::live_control;
static unsigned checks;
static void require(bool ok, const char *what) { ++checks; if (!ok) throw std::runtime_error(what); }
static std::string slurp(const fs::path &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void put(const fs::path &p, const std::string &text) {
    const auto staged = fs::path(p.native() + L".stage");
    { std::ofstream f(staged, std::ios::binary | std::ios::trunc); f << text; }
    require(MoveFileExW(staged.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE, "stage move");
}
static bool eq(chili::ControllerInput a, chili::ControllerInput b) { return chili::same_input(a, b); }

int main(int argc, char **argv) {
    try {
        require(argc == 2, "need scratch dir");
        // A directory of its own per run: live captures refuse to overwrite files.
        const fs::path root = fs::absolute(argv[1]) / ("run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(root);

        // xxh64 reference vectors
        const auto bytes = [](const char *s) { return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(s), std::strlen(s)); };
        require(chili::xxh64(bytes("")) == 0xEF46DB3751D8E999ull, "xxh64 empty");
        require(chili::xxh64(bytes("a")) == 0xD24EC4F1A98C6E5Bull, "xxh64 a");
        require(chili::xxh64(bytes("abc")) == 0x44BC2CF5AD770999ull, "xxh64 abc");
        require(chili::xxh64(bytes("Nobody inspects the spammish repetition")) == 0xFBCEA83C8A378BF1ull, "xxh64 39 bytes");
        {
            std::vector<std::uint8_t> big(1000);
            for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7 + 3);
            std::ofstream(root / "xxh_big.bin", std::ios::binary).write(reinterpret_cast<const char *>(big.data()), big.size());
            std::cout << "xxh64_big=" << std::hex << chili::xxh64(big) << " seed1=" << chili::xxh64(big, 1) << std::dec << '\n';
        }

        // state hash: changes with RAM, CPU and clock; ignores VRAM
        {
            psprecomp::GuestMemory memory; psprecomp::AllegrexContext cpu{};
            const auto a = chili::hash_guest_state(memory, cpu, 5);
            memory.store32(0x04000000u, 0x12345678u);
            const auto vram = chili::hash_guest_state(memory, cpu, 5);
            require(a.combined == vram.combined && a.ram == vram.ram, "VRAM changed the state hash");
            memory.store8(0x08900000u, 1);
            const auto ram = chili::hash_guest_state(memory, cpu, 5);
            require(ram.ram != a.ram && ram.combined != a.combined && ram.cpu == a.cpu, "RAM write not hashed");
            cpu.gpr[7] = 9;
            const auto reg = chili::hash_guest_state(memory, cpu, 5);
            require(reg.cpu != ram.cpu && reg.ram == ram.ram && reg.combined != ram.combined, "CPU not hashed");
            require(chili::hash_guest_state(memory, cpu, 6).combined != reg.combined, "clock not hashed");
            memory.store8(0x00010010u, 3);
            require(chili::hash_guest_state(memory, cpu, 5).combined != reg.combined, "scratchpad not hashed");
            {
                // exclusions: a byte inside an excluded range must not change the hash; one outside must
                const std::vector<chili::HashExclusion> skip{{0x08DD0120u, 4u}, {0x08DF711Cu, 16u}};
                const auto base = chili::hash_guest_state(memory, cpu, 5, skip);
                memory.store32(0x08DD0120u, 0xAABBCCDDu); memory.store8(0x08DF712Bu, 0x7Fu);
                require(chili::hash_guest_state(memory, cpu, 5, skip).combined == base.combined, "excluded bytes changed the hash");
                require(chili::hash_guest_state(memory, cpu, 5).combined != chili::hash_guest_state(memory, cpu, 5, skip).combined, "exclusion had no effect");
                memory.store8(0x08DF712Cu, 0x7Fu);
                require(chili::hash_guest_state(memory, cpu, 5, skip).combined != base.combined, "byte after the excluded range not hashed");
                memory.store8(0x08DF712Cu, 0u); memory.store8(0x08DD011Fu, 1u);
                require(chili::hash_guest_state(memory, cpu, 5, skip).combined != base.combined, "byte before the excluded range not hashed");
                memory.store8(0x08DD011Fu, 0u);
                require(chili::hash_guest_state(memory, cpu, 5, skip).combined == base.combined, "hash not restored");
                const auto with = chili::hash_ram_regions(memory, skip), without = chili::hash_ram_regions(memory);
                require(with[13] != without[13] && with[12] == without[12], "region exclusion");
                memory.store32(0x08DD0120u, 0u); memory.store8(0x08DF712Bu, 0u);
            }
            const auto regions = chili::hash_ram_regions(memory);
            require(regions.size() == 32u && regions[0] == regions[2] && regions[9] != regions[0], "region hashes");
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 8; ++i) (void)chili::hash_guest_state(memory, cpu, i);
            std::cout << "state hash ms each=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 8 << '\n';
        }

        // PNG + BMP by extension
        {
            std::vector<unsigned> pixels(480u * 272u);
            for (unsigned y = 0; y < 272; ++y) for (unsigned x = 0; x < 480; ++x)
                pixels[y * 480 + x] = 0xFF000000u | ((x * 255 / 479) << 16) | ((y * 255 / 271) << 8) | ((x ^ y) & 255);
            require(chili::save_display_pixels(root / "gradient.png", pixels), "png write");
            require(chili::save_display_pixels(root / "gradient.bmp", pixels), "bmp write");
            require(fs::file_size(root / "gradient.bmp") == 54u + 480u * 272u * 4u, "bmp size");
            const auto png = slurp(root / "gradient.png");
            require(png.size() > 391952u && png.compare(1, 3, "PNG") == 0, "png header");
            require(!chili::save_display_pixels(root / "bad.png", std::vector<unsigned>(10)), "short frame accepted");
        }

        // recorder / replay round trip
        const auto rec = root / "input.rec";
        {
            chili::InputRecorder recorder(rec);
            recorder.observe(0, {});
            recorder.observe(1, {});
            recorder.observe(2, {0x4000, 128, 128});
            recorder.observe(3, {0x4000, 128, 128});
            recorder.observe(4, {0x4000, 10, 250});
            recorder.observe(5, {});
            recorder.observe(6, {});
        }
        const auto text = slurp(rec);
        require(text == "# chili input v3: sample buttons x y [rx ry [turn pitch]]\n2 0x4000 128 128\n4 0x4000 10 250\n5 0x0 128 128\nend 7\n", "record format");
        {
            chili::InputReplay replay(rec);
            require(replay.enabled() && replay.active(6) && !replay.active(7), "replay end");
            const chili::ControllerInput expect[] = {{}, {}, {0x4000, 128, 128}, {0x4000, 128, 128}, {0x4000, 10, 250}, {}, {}};
            for (std::uint64_t i = 0; i < 7; ++i) require(eq(replay.at(i), expect[i]), "replay state");
        }
        {
            put(root / "noend.rec", "# x\n10 0x10 128 128\n12 0 128 128\n");
            chili::InputReplay replay(root / "noend.rec");
            require(replay.active(12) && !replay.active(13) && eq(replay.at(9), {}) && replay.at(10).buttons == 0x10 && replay.at(12).buttons == 0, "implicit end");
            for (const char *bad : {"5 0x4 128 128\n", "5 0x10 256 0\n", "5 0x10 1 1\n5 0 1 1\n", "5 0x10 1\n", "end\n", "5 0x10 1 1\nend 5\n", "end 3\n4 0 1 1\n", "x 0 1 1\n", "-1 0 1 1\n"}) {
                put(root / "bad.rec", bad);
                bool threw = false;
                try { chili::InputReplay r(root / "bad.rec"); } catch (const psprecomp::Error &) { threw = true; }
                require(threw, "bad replay accepted");
            }
            chili::InputReplay none{fs::path{}};
            require(!none.enabled() && !none.active(0), "disabled replay");
        }
        // second stick and control scheme: written only when used, and a recording
        // made before either existed reads as a centred second stick and no scheme
        {
            const auto dual = root / "dual.rec";
            {
                chili::InputRecorder recorder(dual, "dual_analog");
                recorder.observe(0, {});
                recorder.observe(1, {0x8000, 128, 0});
                recorder.observe(2, {0x8000, 128, 0, 200, 128});
                recorder.observe(3, {0x8000, 128, 0, 200, 128});
                recorder.observe(4, {0x8000, 128, 0, 200, 30});
                recorder.observe(5, {0, 128, 128, 128, 255});
                recorder.observe(6, {});
            }
            require(slurp(dual) == "# chili input v3: sample buttons x y [rx ry [turn pitch]]\nscheme dual_analog\n1 0x8000 128 0\n2 0x8000 128 0 200 128\n"
                                   "4 0x8000 128 0 200 30\n5 0x0 128 128 128 255\n6 0x0 128 128\nend 7\n", "record format with a second stick");
            chili::InputReplay replay(dual);
            require(replay.scheme() == "dual_analog" && replay.active(6) && !replay.active(7), "replay scheme and end");
            const chili::ControllerInput expect[] = {{}, {0x8000, 128, 0}, {0x8000, 128, 0, 200, 128}, {0x8000, 128, 0, 200, 128},
                {0x8000, 128, 0, 200, 30}, {0, 128, 128, 128, 255}, {}};
            for (std::uint64_t i = 0; i < 7; ++i) require(eq(replay.at(i), expect[i]), "replay state with a second stick");
            require(!eq({0, 128, 128, 129, 128}, {}) && !eq({0, 128, 128, 128, 127}, {}), "the second stick is part of the state");

            put(root / "old.rec", "# chili input v1: sample buttons x y\n3 0x4000 1 2\n5 0 128 128\nend 9\n");
            chili::InputReplay old(root / "old.rec");
            require(old.scheme().empty() && eq(old.at(3), {0x4000, 1, 2, 128, 128}) && eq(old.at(5), {}), "old recording: centred second stick, no scheme");
            put(root / "psp.rec", "scheme psp\n3 0x4000 1 2 3 4\n");
            chili::InputReplay psp(root / "psp.rec");
            require(psp.scheme() == "psp" && eq(psp.at(3), {0x4000, 1, 2, 3, 4}), "scheme line and six fields");

            // The mouse's angle of a sample: two more fields, written only when there is one, read back exactly.
            const auto look = root / "look.rec";
            {
                chili::InputRecorder recorder(look, "dual_analog");
                recorder.observe(0, {});
                recorder.observe(1, {0, 128, 128, 128, 128, 2.0f, -0.8f});
                recorder.observe(2, {0, 128, 128, 128, 128, 2.0f, -0.8f});
                recorder.observe(3, {0x8000, 128, 0, 200, 128, 0.1f, 0.0f});
                recorder.observe(4, {0x8000, 128, 0, 200, 128});
                recorder.observe(5, {});
            }
            require(slurp(look) == "# chili input v3: sample buttons x y [rx ry [turn pitch]]\nscheme dual_analog\n1 0x0 128 128 128 128 2 -0.800000012\n"
                                   "3 0x8000 128 0 200 128 0.100000001 0\n4 0x8000 128 0 200 128\n5 0x0 128 128\nend 6\n", "record format with a mouse angle");
            chili::InputReplay looked(look);
            const chili::ControllerInput seen[] = {{}, {0, 128, 128, 128, 128, 2.0f, -0.8f}, {0, 128, 128, 128, 128, 2.0f, -0.8f},
                {0x8000, 128, 0, 200, 128, 0.1f, 0.0f}, {0x8000, 128, 0, 200, 128}, {}};
            for (std::uint64_t i = 0; i < 6; ++i) require(eq(looked.at(i), seen[i]), "replay state with a mouse angle");
            require(!eq({0, 128, 128, 128, 128, 0.5f, 0.0f}, {}) && !eq({0, 128, 128, 128, 128, 0.0f, -0.5f}, {}), "the mouse angle is part of the state");
            put(root / "half.rec", "3 0 128 128 128 128 1.5\n");
            bool refused = false;
            try { chili::InputReplay half(root / "half.rec"); } catch (const std::exception &) { refused = true; }
            require(refused, "a turn without a pitch is refused");
            put(root / "wild.rec", "3 0 128 128 128 128 nan 0\n");
            refused = false;
            try { chili::InputReplay wild(root / "wild.rec"); } catch (const std::exception &) { refused = true; }
            require(refused, "an angle that is not a number is refused");
            for (const char *bad : {"5 0x10 1 1 7\n", "5 0x10 1 1 256 0\n", "5 0x10 1 1 0 256\n", "5 0x10 1 1 2 3 4\n", "scheme\n", "scheme a b\n",
                     "scheme psp\nscheme psp\n", "5 0 1 1\nscheme psp\n", "5 0x10 1 1 x 3\n"}) {
                put(root / "bad.rec", bad);
                bool threw = false;
                try { chili::InputReplay r(root / "bad.rec"); } catch (const psprecomp::Error &) { threw = true; }
                require(threw, "bad second-stick replay accepted");
            }
            // the shipped replays are the old format and must keep loading
            const auto shipped = fs::path(__FILE__).parent_path().parent_path() / "replays" / "mission1_baseline.replay";
            if (fs::exists(shipped)) {
                chili::InputReplay baseline(shipped);
                require(baseline.scheme().empty() && baseline.active(2303) && !baseline.active(2304) &&
                        eq(baseline.at(40), {0x4000, 128, 128}) && eq(baseline.at(233), {0x8000, 128, 0}), "shipped baseline replay");
            }
        }

        // pipeline: live > replay > host(off); live tap counts samples; recorder sees the merge
        {
            std::uint64_t poll_ms = 0, pulse_ms = 0;
            std::vector<std::string> logs;
            lc::Config config{root / "control.txt", root / "frame"};
            config.ack_file = root / "control.txt.ack";
            config.frame_extension = L".png";
            lc::DiagnosticLiveChannel live(config, [&] { return poll_ms; }, [&](const auto &l) { logs.push_back(l); }, [&] { return pulse_ms; });
            bool quit = false;
            live.set_quit_handler([&] { quit = true; });
            chili::InputReplay replay(rec);
            const auto merged = root / "merged.rec";
            std::uint64_t samples_seen = 0;
            {
                chili::InputRecorder recorder(merged);
                chili::InputPipeline input(live, replay, recorder, false);
                live.set_context([&] { return " sample=" + std::to_string(input.samples()); });
                auto pump = [&] { poll_ms += 50; live.host_pump([&](const fs::path &p) { std::ofstream(p) << "img"; return true; }); };
                require(eq(input.sample(), {}) && eq(input.sample(), {}), "replay neutral start");          // 0,1
                require(*input.scripted_buttons() == 0x4000u, "dialog buttons peek replay");
                require(input.sample().buttons == 0x4000u, "replay press");                                 // 2
                put(config.control_file, "1 tap 0x8000 3 128 128\n"); pump();
                require(slurp(config.ack_file) == "1 ok tap sample=3\n", "tap ack");
                for (int i = 0; i < 3; ++i) require(input.sample().buttons == 0x8000u, "tap sample");       // 3,4,5 override replay
                require(eq(input.sample(), {}), "tap ended, replay state");                                 // 6
                require(eq(input.sample(), {}) && input.samples() == 8u, "after replay end neutral");       // 7
                put(config.control_file, "2 hold 0x10 0 255\n"); pump();
                for (int i = 0; i < 5; ++i) require(eq(input.sample(), {0x10, 0, 255}), "hold");            // 8..12
                pulse_ms = 1000000; require(eq(input.sample(), {0x10, 0, 255}), "hold ignores clock");      // 13
                put(config.control_file, "3 release\n"); pump();
                require(slurp(config.ack_file) == "3 ok release sample=14\n" && eq(input.sample(), {}), "release");   // 14
                put(config.control_file, "4 press 0x4000 100 128 128\n"); pump();
                require(input.sample().buttons == 0x4000u, "press active");                                 // 15
                pulse_ms += 99; require(input.sample().buttons == 0x4000u, "press active at 99");           // 16
                pulse_ms += 1; require(eq(input.sample(), {}), "press expired on pulse clock");             // 17
                put(config.control_file, "5 capture shot\n"); pump();
                require(slurp(config.ack_file) == "5 ok capture frame_5_shot.png sample=18\n" && fs::exists(root / "frame_5_shot.png"), "capture ack/png name");
                put(config.control_file, "5 capture again\n"); pump();
                require(slurp(config.ack_file) == "5 stale 5\n", "stale ack");
                put(config.control_file, "6 bogus\n"); pump();
                require(slurp(config.ack_file).rfind("5 rejected ", 0) == 0, "rejected ack");
                fs::remove(config.ack_file);
                put(config.control_file, "6 bogus2\n"); pump();
                require(slurp(config.ack_file).rfind("5 rejected ", 0) == 0, "second rejection with same error text still acknowledged");
                fs::remove(config.ack_file);
                pump(); pump();
                require(!fs::exists(config.ack_file), "unchanged malformed command acknowledged again");
                put(config.control_file, "6 tap 0x20 2 128 128\n"); pump();
                require(*input.scripted_buttons() == 0x20u && *input.scripted_buttons() == 0x20u, "peek does not consume tap");
                require(input.sample().buttons == 0x20u && input.sample().buttons == 0x20u && eq(input.sample(), {}), "tap 2");   // 18,19,20
                put(config.control_file, "7 quit\n"); pump();
                require(quit && slurp(config.ack_file) == "7 ok quit sample=21\n", "quit");
                samples_seen = input.samples();
            }
            require(samples_seen == 21u, "sample count");
            const auto merged_text = slurp(merged);
            const char *expected = "# chili input v3: sample buttons x y [rx ry [turn pitch]]\n2 0x4000 128 128\n3 0x8000 128 128\n6 0x0 128 128\n8 0x10 0 255\n14 0x0 128 128\n"
                                   "15 0x4000 128 128\n17 0x0 128 128\n18 0x20 128 128\n20 0x0 128 128\nend 21\n";
            if (merged_text != expected) std::cout << merged_text;
            require(merged_text == expected, "merged recording");
            // replaying the merged recording reproduces every sample
            lc::DiagnosticLiveChannel off({{}, {}});
            chili::InputReplay again(merged); chili::InputRecorder nothing{fs::path{}};
            chili::InputPipeline input(off, again, nothing, false);
            const unsigned expect[21] = {0, 0, 0x4000, 0x8000, 0x8000, 0x8000, 0, 0, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0, 0x4000, 0x4000, 0, 0x20, 0x20, 0};
            for (unsigned i = 0; i < 21; ++i) require(input.sample().buttons == expect[i], "merged replay sample");
            require(eq(input.sample(), {}) && !input.scripted_buttons(), "past end");
            for (const char *bad : {"1 tap 0x4000 0 128 128", "1 tap 0x4000 3601 128 128", "1 hold 0x4000 128", "1 hold 0x4 128 128", "1 release now", "1 quit now", "1 hold 0x4000 128 128 9"}) {
                bool threw = false; try { (void)lc::parse(bad); } catch (const lc::Error &) { threw = true; }
                require(threw, "bad live command accepted");
            }
            require(lc::parse("9 tap 0x4000 3600 1 2").samples == 3600u && lc::parse("9 hold 0 1 2").kind == lc::Kind::Hold &&
                    lc::parse("9 release\n").kind == lc::Kind::Release && lc::parse("9 quit").kind == lc::Kind::Quit, "new commands parse");
            // second stick on the live channel: optional, both values or neither
            require(eq(lc::parse("9 hold 0x10 1 2 3 4").input, {0x10, 1, 2, 3, 4}) && eq(lc::parse("9 hold 0x10 1 2").input, {0x10, 1, 2, 128, 128}) &&
                    eq(lc::parse("9 tap 0 5 128 128 255 0").input, {0, 128, 128, 255, 0}) && lc::parse("9 tap 0 5 128 128 255 0").samples == 5u &&
                    eq(lc::parse("9 press 0x8000 100 7 8 9 10").input, {0x8000, 7, 8, 9, 10}) && lc::parse("9 press 0x8000 100 7 8 9 10").duration_ms == 100u,
                    "live commands with a second stick");
            for (const char *bad : {"1 hold 0 128 128 256 0", "1 hold 0 128 128 0 256", "1 hold 0 128 128 1 2 3", "1 tap 0 3 128 128 1", "1 tap 0 3 128 128 1 2 3",
                     "1 press 0 100 128 128 1 -2"}) {
                bool threw = false; try { (void)lc::parse(bad); } catch (const lc::Error &) { threw = true; }
                require(threw, "bad second-stick live command accepted");
            }
        }

        // main menu detection from the scene tree: root > Missions > MainMenuScene, loaded while that has a child
        {
            psprecomp::GuestMemory memory;
            using chili::MenuScene;
            std::uint32_t next = 0x08900000u;
            const auto node = [&](const char *name, std::uint32_t parent) {
                const auto at = next; next += 0x80u;
                const auto text = at + 0x60u;
                for (std::size_t i = 0; i <= std::strlen(name); ++i) memory.store8(text + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
                memory.store32(at + MenuScene::name, text); memory.store32(at + MenuScene::parent, parent);
                if (parent) { memory.store32(at + MenuScene::sibling, memory.load32(parent + MenuScene::child)); memory.store32(parent + MenuScene::child, at); }
                return at;
            };
            const auto root = node("Scene", 0u), scope = node("RootScope", root), missions = node("Missions", root);
            (void)node("Sounds", root);
            const auto player = node("PlayerCharacter", scope);
            MenuScene none;
            require(!none.loaded(memory, player), "no MainMenuScene node: not in the menu");
            (void)node("Story", missions);
            const auto menu = node("MainMenuScene", missions);
            (void)node("Bonus", missions);
            MenuScene watch;
            require(!watch.loaded(memory, player), "an empty MainMenuScene: the menu is not up");
            const auto window = node("MenuWindow", menu);
            require(watch.loaded(memory, player) && watch.loaded(memory, root), "MainMenuScene with a child: the menu is up, from any node");
            memory.store32(MenuScene::scene_instance_address, root);
            require(watch.loaded(memory) && MenuScene().loaded(memory), "found from the running scene");
            memory.store32(menu + MenuScene::child, 0u);
            require(!watch.loaded(memory, player), "the mission unloads it");
            memory.store32(menu + MenuScene::child, window);
            memory.store8(memory.load32(menu + MenuScene::name), 'X');
            require(!watch.loaded(memory, player), "a node that is no longer the menu scene is looked up again");
        }

        // frame timer
        {
            const auto csv = root / "frames.csv";
            {
                chili::FrameTimer timer(csv);
                timer.make_active();
                std::uint64_t guest = 1000;
                require(timer.boundary(guest, 0, 0.0333f) == 1u && timer.frames() == 0u, "first boundary");
                for (int i = 0; i < 20; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(i == 10 ? 30 : 5));
                    if (i == 5) chili::FrameTimer::add_idle(std::chrono::milliseconds(3));
                    guest += 33366;
                    timer.boundary(guest, i * 2u, 0.033366f);
                }
                require(timer.frames() == 20u, "frame count");
                std::ostringstream out; timer.report(out);
                std::cout << out.str();
                require(out.str().find("[frame-timer] frames=20 wall_ms_mean=") == 0 && out.str().find("guest_fps=29.971") != std::string::npos, "summary line");
            }
            const auto rows = slurp(csv);
            require(std::count(rows.begin(), rows.end(), '\n') == 21 && rows.rfind("frame,wall_ms,work_ms,guest_ms,", 0) == 0, "csv rows");
            std::cout << rows.substr(0, rows.find('\n', rows.find('\n') + 1) + 1);
            chili::FrameTimer none; std::ostringstream out; none.report(out);
            require(out.str() == "[frame-timer] frames=0\n", "empty summary");
        }

        // guest-clocked audio sink
        {
            unsigned long long now = 0;
            chili::GuestClockPcmSink sink([&] { return now; });
            std::vector<short> block(1024);   // 512 stereo frames = 11609.97 us
            require(sink.submit(block, 0x8000) && sink.submit(block, 0x8000) && !sink.submit(block, 0x8000) && sink.pending() == 2u, "backpressure");
            require(sink.retry_delay_us() == 11609u, "delay to first completion");
            now = 11608; require(sink.pending() == 2u && sink.completed() == 0u, "early completion");
            now = 11609; require(sink.pending() == 1u && sink.completed() == 1u, "first block");
            require(sink.submit(block, 0x8000) && sink.pending() == 2u, "refill");
            now = 23219; require(sink.pending() == 1u, "second block at exact cumulative time");
            now = 34829; require(sink.pending() == 0u && sink.completed() == 3u, "third block, no rounding drift");
            now = 100000; require(sink.submit(block, 0x8000) && sink.retry_delay_us() == 11609u, "underrun restarts stream clock");
        }

        // scripted dialogs
        {
            std::uint64_t now = 100;
            chili::ScriptedSaveSlotPresenter save([&] { return now; }, 500000, {"focus", "1", "cancel"});
            chili::SaveSlotRequest request; request.generation = 7; request.operation = chili::SaveSlotOperation::Load;
            request.slots = {{"DATA00", "", false}, {"DATA01", "", true}, {"DATA02", "", true}}; request.initial_index = 0;
            save.open(request);
            require(!save.poll(7), "answered before delay");
            now += 499999; require(!save.poll(7), "answered early");
            now += 1; const auto first = save.poll(7);
            require(first && first->choice == chili::SaveSlotChoice::Choose && first->index == 1u, "load focus falls to first slot with data");
            require(!save.poll(7), "answered twice");
            request.generation = 8; request.operation = chili::SaveSlotOperation::Save; save.open(request); now += 500000;
            const auto second = save.poll(8); require(second && second->index == 1u && !save.poll(9), "indexed save");
            // A scripted pick stands for the player's answer to the overwrite question too.
            require(second->confirmed && !first->confirmed, "a scripted save over a save is confirmed, a load is not");
            request.generation = 9; save.open(request); now += 500000;
            require(save.poll(9)->choice == chili::SaveSlotChoice::Cancel, "cancel");
            request.generation = 10; save.open(request); now += 500000;
            require(save.poll(10)->choice == chili::SaveSlotChoice::Cancel, "last entry repeats");
            {
                // A save into an empty slot has no question to answer; a delete takes only a save.
                chili::ScriptedSaveSlotPresenter empty([&] { return now; }, 0, {"0", "focus", "0", "cancel"});
                request.generation = 11; request.operation = chili::SaveSlotOperation::Save; empty.open(request);
                const auto into_empty = empty.poll(11);
                require(into_empty && into_empty->choice == chili::SaveSlotChoice::Choose && into_empty->index == 0u && !into_empty->confirmed, "scripted save into an empty slot");
                request.generation = 12; request.operation = chili::SaveSlotOperation::Delete; empty.open(request);
                const auto removed = empty.poll(12);
                require(removed && removed->choice == chili::SaveSlotChoice::Choose && removed->index == 1u && removed->confirmed, "scripted delete: focus on an empty slot falls to the first save, confirmed");
                request.generation = 13; empty.open(request);
                require(empty.poll(13)->choice == chili::SaveSlotChoice::Cancel, "scripted delete of an empty slot is a cancel");
                request.generation = 14; empty.open(request);
                require(empty.poll(14)->choice == chili::SaveSlotChoice::Cancel, "scripted delete cancel");
            }

            chili::ScriptedMessageDialogPresenter message([&] { return now; }, 500000, {});
            chili::MessageDialogRequest m; m.generation = 1; m.yes_no = true; m.allow_back = true; m.text = "Save?\nReally";
            message.open(m); now += 500000; require(message.poll(1)->choice == chili::MessageDialogChoice::Yes, "default accept yes");
            m.generation = 2; m.yes_no = false; m.okay = true; message.open(m); now += 500000; require(message.poll(2)->choice == chili::MessageDialogChoice::Okay, "ok");
            m.generation = 3; m.okay = false; message.open(m); now += 500000; require(message.poll(3)->choice == chili::MessageDialogChoice::Back, "back only");
            chili::ScriptedMessageDialogPresenter decline([&] { return now; }, 0, {"decline"});
            m.generation = 4; m.yes_no = true; decline.open(m); require(decline.poll(4)->choice == chili::MessageDialogChoice::No, "decline no");
        }

        // options parsing
        {
            SetEnvironmentVariableW(L"CHILI_DETERMINISTIC", L"1");
            SetEnvironmentVariableW(L"CHILI_STATE_HASH_EVERY", L"300");
            SetEnvironmentVariableW(L"PSPRECOMP_PROBE_SECONDS", L"7200");
            SetEnvironmentVariableW(L"CHILI_AUTO_MESSAGE", L"accept,decline");
            SetEnvironmentVariableW(L"CHILI_STATE_HASH_EXCLUDE", L"0x08DF711C:16,0x08DD0120:4");
            auto o = chili::RunOptions::from_environment();
            require(o.state_hash_exclude.size() == 2u && o.state_hash_exclude[0].address == 0x08DD0120u && o.state_hash_exclude[1].length == 16u, "exclusions sorted");
            for (const wchar_t *bad : {L"0x08DF711C", L"0x04000000:4", L"0x08DF711C:0", L"0x08DF711C:8,0x08DF7120:8", L"0x09FFFFFF:2", L"x:1", L"0x08DF711C:1x"}) {
                SetEnvironmentVariableW(L"CHILI_STATE_HASH_EXCLUDE", bad);
                bool threw = false; try { (void)chili::RunOptions::from_environment(); } catch (const psprecomp::Error &) { threw = true; }
                require(threw, "bad exclusion accepted");
            }
            SetEnvironmentVariableW(L"CHILI_STATE_HASH_EXCLUDE", nullptr);
            require(o.deterministic && o.null_audio && o.auto_dialogs && o.state_hash_every == 300u && o.probe_seconds == 7200u && o.auto_message.size() == 2u, "options");
            require(o.scripted(), "deterministic run is scripted");
            for (const wchar_t *bad : {L"7201", L"0", L"x", L"-1"}) {
                SetEnvironmentVariableW(L"PSPRECOMP_PROBE_SECONDS", bad);
                bool threw = false; try { (void)chili::RunOptions::from_environment(); } catch (const psprecomp::Error &) { threw = true; }
                require(threw, "bad probe seconds accepted");
            }
            SetEnvironmentVariableW(L"PSPRECOMP_PROBE_SECONDS", nullptr);
            SetEnvironmentVariableW(L"CHILI_DETERMINISTIC", L"yes");
            bool threw = false; try { (void)chili::RunOptions::from_environment(); } catch (const psprecomp::Error &) { threw = true; }
            require(threw, "bad flag accepted");
            SetEnvironmentVariableW(L"CHILI_DETERMINISTIC", nullptr);
            require(!chili::RunOptions::from_environment().deterministic, "default off");
            // Only a plain interactive run may fall back to the real save folder.
            SetEnvironmentVariableW(L"CHILI_AUTO_MESSAGE", nullptr);
            SetEnvironmentVariableW(L"CHILI_STATE_HASH_EVERY", nullptr);
            SetEnvironmentVariableW(L"CHILI_AUTO_DIALOGS", nullptr);
            SetEnvironmentVariableW(L"CHILI_INPUT_REPLAY", nullptr);
            require(!chili::RunOptions::from_environment().scripted(), "plain run is not scripted");
            SetEnvironmentVariableW(L"CHILI_AUTO_DIALOGS", L"1");
            require(chili::RunOptions::from_environment().scripted(), "auto dialogs are scripted");
            SetEnvironmentVariableW(L"CHILI_AUTO_DIALOGS", nullptr);
            SetEnvironmentVariableW(L"CHILI_INPUT_REPLAY", L"some.replay");
            require(chili::RunOptions::from_environment().scripted(), "a replay is scripted");
            SetEnvironmentVariableW(L"CHILI_INPUT_REPLAY", nullptr);
        }
        std::error_code cleanup;
        fs::remove_all(root, cleanup);
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
