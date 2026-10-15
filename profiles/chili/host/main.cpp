#include "guest_capture_plan.hpp"
#include "controller_replay.hpp"
#include "diagnostic_live_channel.hpp"
#include "input_pipeline.hpp"
#include "input_title.hpp"
#include "launcher.hpp"
#include "run_options.hpp"
#include "scripted_dialogs.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include <iostream>
#include <algorithm>
#include "rtc_runtime.hpp"
#include "libc_time_runtime.hpp"
#include "savedata_runtime.hpp"
#include "savedata_size_query.hpp"
#include "fixed_file_time.hpp"
#include "native_savedata_backend.hpp"
#include "save_slot_win32.hpp"
#include "message_dialog_runtime.hpp"
#include "message_dialog_win32.hpp"
#include "thread_runtime.hpp"
#include "thread_lifecycle.hpp"
#include "volatile_memory_runtime.hpp"
#include "partition_memory.hpp"
#include "fixed_pool.hpp"
#include "event_flags.hpp"
#include "file_runtime.hpp"
#include "debug_io.hpp"
#include "umd_runtime.hpp"
#include "semaphores.hpp"
#include "network_runtime.hpp"
#include "system_settings.hpp"
#include "controller_runtime.hpp"
#include "controller_sampling_provenance.hpp"
#include "av_runtime.hpp"
#include "sas_runtime.hpp"
#include "atrac_runtime.hpp"
#include "mpeg_runtime.hpp"
#include "audio_runtime.hpp"
#include "audio_settings.hpp"
#include "psmf_runtime.hpp"
#include "ge_runtime.hpp"
#include "power_runtime.hpp"
#include "presentation.hpp"
#include "present_latency.hpp"
#include "frame_probe.hpp"
#include "prompt_style.hpp"
#include "gpu_video.hpp"
#include "overlay_test_page.hpp"
#include "gpu_warm_up.hpp"
#include "pc_input.hpp"
#include "pad_rumble_hook.hpp"
#include "settings_entry.hpp"
#include "settings_entry_legend.hpp"
#include "window_picture.hpp"
#include "fake_xinput.hpp"
#include "dual_analog.hpp"
#include "realtime_pacer.hpp"
#include "performance_telemetry.hpp"
#include "parallel_live_observer.hpp"
#include "sampling_profiler.hpp"
#include "settings_apply.hpp"
#include "settings_page_host.hpp"
#include "test_hook.hpp"
#include <bit>
#include <memory>
#include <string_view>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <fstream>
#include <vector>
#include "display_runtime.hpp"

namespace {
constexpr char probe_deadline_reason[] = "Diagnostic wall-clock deadline reached";
constexpr char live_quit_reason[] = "Live control requested stop";
constexpr char window_closed_reason[] = "Native window closed";
std::filesystem::path executable_directory() {
    std::wstring path(32768u, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}
// The settings file with every section's defaults. A file that is there keeps what it has; a section
// it lacks is added commented out.
void write_default_settings(const std::filesystem::path &ini) {
    std::vector<std::string> problems;
    (void)chili::PcInputConfig::load(ini, problems, true);
    chili::add_default_mod_settings(ini);
    chili::add_default_render_settings(ini);
    chili::add_default_audio_settings(ini);
    chili::launcher::add_default_disc_settings(ini);
}

// Strict bootstrap: missing firmware semantics stop execution with an exact
// import/PC diagnostic. No placeholder success responses are used.
// Returns the exit code; `start` says afterwards how the program was started and why it failed.
int run(chili::launcher::Start &start) {
    try {
        // With no positional argument this is a player's start (launcher.hpp): the folders beside the
        // executable and the player's own disc. With two it is a scripted or developer run on the named
        // BOOT.BIN and disc folder, as before.
        start = chili::launcher::parse_command_line();
        if (start.usage) {
            std::cerr << chili::launcher::usage_text << '\n';
            return 2;
        }
        if (!start.write_ini.empty()) {
            write_default_settings(start.write_ini);
            return std::filesystem::is_regular_file(start.write_ini) ? 0 : 1;
        }
        const auto &positional = start.positional;
        const auto &ms0_argument = start.ms0;
        const auto options = chili::RunOptions::from_environment();
        if (options.log_flush) std::cout.setf(std::ios::unitbuf);
        chili::launcher::Disc disc;
        if (start.player) {
            // False: the game is already running and its window was brought to the front.
            if (!chili::launcher::begin(start, options.scripted())) return 0;
            if (!options.deterministic && options.config_file.empty()) write_default_settings(start.ini);
            auto chosen = chili::launcher::choose_disc(start);
            // The player closed the disc question: nothing to run, nothing failed.
            if (!chosen) return 0;
            disc = std::move(*chosen);
            chili::disc_iso = disc.image;
        }
        psprecomp::Runtime runtime;
        auto elf = start.player ? psprecomp::Elf32Image::from_bytes(std::move(disc.boot), "PSP_GAME/SYSDIR/BOOT.BIN")
                                : psprecomp::Elf32Image::from_file(positional[0]);
        const auto relocations = elf.load_and_relocate(runtime.memory());
        if (relocations.invalid || relocations.unsupported)
            throw psprecomp::Error("Invalid or unsupported relocations");
        const auto module = elf.find_module_info(runtime.memory());
        if (!module) throw psprecomp::Error("Missing PSP module metadata");
        runtime.register_hle("ModuleMgrForUser", 0xD8B73127u,
            [&elf](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                const auto address = psprecomp::GuestMemory::canonical(ctx.gpr[4]);
                std::uint32_t uid = 0x800200CBu;
                for (std::size_t i = 0; i < elf.segments().size(); ++i) {
                    const auto &segment = elf.segments()[i];
                    if (segment.type != 1u) continue;
                    const auto start = elf.segment_runtime_address(i);
                    if (address >= start && address - start < segment.memory_size) { uid = 0x100000u; break; }
                }
                ctx.set_gpr(2u, uid);
            });
        runtime.set_game_root(start.player ? disc.path : std::filesystem::path(positional[1]));
        // The disc folder is never written to: every place this run writes must lie outside it.
        const auto keep_out = [&](const std::filesystem::path &path, const char *what) {
            if (!path.empty() && chili::vfs::inside(path, runtime.game_root()))
                throw psprecomp::Error(std::string(what) + " is inside the disc folder, which is never written to: " + path.string());
        };
        keep_out(ms0_argument, "--ms0");
        for (const wchar_t *name : {L"CHILI_MS0_DIR", L"CHILI_CONFIG", L"CHILI_INPUT_RECORD", L"CHILI_FRAME_LOG", L"CHILI_FRAME_PNG_DIR",
                 L"CHILI_GE_CAPTURE_DIR", L"CHILI_GE_TRACE", L"CHILI_SAMPLER", L"CHILI_AUDIO_WAV", L"CHILI_AUDIO_TRACE", L"PSPRECOMP_FRAME_DUMP", L"PSPRECOMP_GUEST_DUMP",
                 L"PSPRECOMP_GUEST_DUMP_PREFIX", L"PSPRECOMP_LIVE_FRAME_PREFIX"}) {
            const auto value = chili::environment_text(name);
            std::string label;
            for (const wchar_t *c = name; *c; ++c) label.push_back(static_cast<char>(*c));
            keep_out(value, label.c_str());
        }
        // The memory stick: a player's saves folder beside the executable; for a run on a named disc
        // folder the ms0 folder beside disc0. --ms0 or CHILI_MS0_DIR points a run at a copy so the real
        // save stays untouched.
        auto ms0_root = start.player ? start.saves : runtime.game_root().parent_path() / "ms0";
        if (!ms0_argument.empty() || !options.ms0_dir.empty()) {
            ms0_root = std::filesystem::absolute(ms0_argument.empty() ? options.ms0_dir : ms0_argument);
            std::filesystem::create_directories(ms0_root);
            std::cout << "[options] memory stick root " << ms0_root.string() << '\n';
        } else if (options.scripted()) {
            // Scripted dialogs answer the save prompts by themselves: never on the real save.
            throw psprecomp::Error("CHILI_DETERMINISTIC, CHILI_AUTO_DIALOGS and CHILI_INPUT_REPLAY runs save without asking: "
                "give --ms0 or CHILI_MS0_DIR (a copy of the save folder), the real save in " + ms0_root.string() + " is not used");
        }
        if (options.deterministic) {
            std::cout << "[options] deterministic mode: guest-clocked RTC and audio, scripted dialogs, no host input\n";
            // Host disk free space and file times would otherwise reach guest RAM.
            chili::savedata_size_prep::set_fixed_volume(chili::savedata_size_prep::Volume{8ull << 30, 16ull << 30, 8ull << 30, 4096u});
            chili::fixed_file_time = static_cast<__time64_t>(chili::deterministic_epoch_unix_us / 1000000u);
        }
        chili::FileRuntime files(runtime, ms0_root);
        // Mods and engine settings: [Mods], [Game] and [Game.Config] of ChiliNative.ini (mod_files.hpp).
        const auto mods_ini = options.config_file.empty() ? executable_directory() / "ChiliNative.ini" : options.config_file;
        auto mods = chili::ModSetup::load(mods_ini);
        for (const auto &problem : mods.problems) std::cerr << "[mods] " << mods_ini.filename().string() << ' ' << problem << '\n';
        // 60 game frames per second when someone plays, unless the ini says otherwise ([Game] FrameLock). A
        // scripted run keeps the game's own 30: its replays were recorded at that rate.
        {
            const bool given = std::any_of(mods.game_config.begin(), mods.game_config.end(),
                [](const auto &entry) { return chili::ModSetup::same_text(entry.first, "frame_lock"); });
            const bool player = !options.scripted() && chili::environment_text(L"PSPRECOMP_LIVE_CONTROL").empty();
            if (!given && player) mods.game_config.emplace_back("frame_lock", "1");
            const auto lock = std::find_if(mods.game_config.begin(), mods.game_config.end(),
                [](const auto &entry) { return chili::ModSetup::same_text(entry.first, "frame_lock"); });
            std::cout << "[game] frame rate: " << (lock != mods.game_config.end() && lock->second == "1" ? "60" : "30")
                      << " game frames per second ([Game] FrameLock=1|2" << (given ? "" : player ? ", default for a player" : ", default for a scripted run") << ")\n";
        }
        // Control scheme and bindings: the ini file's. A deterministic run reads an ini only when
        // CHILI_CONFIG names it, so replays made before the scheme existed keep playing as psp.
        auto input_config = chili::PcInputConfig::defaults(chili::InputScheme::Psp);
        if (!options.deterministic || !options.config_file.empty()) {
            std::vector<std::string> problems, notes;
            input_config = chili::PcInputConfig::load(mods_ini, problems, !options.deterministic, &notes);
            std::cout << "[input] bindings: " << mods_ini.string() << '\n';
            for (const auto &note : notes) std::cout << "[input] " << note << '\n';
            for (const auto &problem : problems) std::cerr << "[input] " << mods_ini.filename().string() << ' ' << problem << '\n';
            // An interactive run adds the [Mods], [Game], [Rendering] and [Audio] sections to a file that has none.
            if (!options.deterministic) { chili::add_default_mod_settings(mods_ini); chili::add_default_render_settings(mods_ini); chili::add_default_audio_settings(mods_ini); }
        }
        // [Game] Camera: the tight on-foot camera goes with the ini's two-stick scheme unless the file says
        // otherwise. A replay that names its own scheme does not change it, so scripted runs keep the game's.
        const bool tight_camera = mods.tight_camera(input_config.scheme == chili::InputScheme::DualAnalog);
        if (tight_camera) {
            mods.builtin_properties = chili::tight_camera_properties;
            std::cout << "[game] camera: tight ([Game] Camera=original gives the game's own)\n";
        }
        // The "Settings" entries in the game's own menus (settings_entry.hpp): in a player's run, and in a
        // deterministic, replay or script run only with CHILI_PAGE=1. The blocks that carry them are changed
        // where the mod layer serves them (menu_entry_blocks.hpp); CHILI_PAGE_ENTRY picks another way in.
        // The player's switch for all of it: [Game] SettingsInGame=false (or CHILI_PAGE=0 for one run) leaves the
        // game's menus as the PSP game has them and puts no page in the run, should a menu or the page ever
        // misbehave. CHILI_PAGE=1 asks for the feature whatever the file says (tests).
        const bool settings_off = chili::settings_switched_off(mods.settings_in_game);
        const bool page_entries = !settings_off && chili::settings_entry_enabled(options.scripted() || !chili::environment_text(L"PSPRECOMP_LIVE_CONTROL").empty());
        const auto entry_route = page_entries ? chili::SettingsEntry::route_from_environment() : chili::SettingsEntry::Route{};
        if (!entry_route.problem.empty()) std::cerr << "[menu] " << entry_route.problem << '\n';
        mods.menu_entry_main = page_entries && !entry_route.hook;
        mods.menu_entry_pause = page_entries && !entry_route.legend;
        // Renderer choice: [Rendering] of the same file, then CHILI_RENDER_* (render_settings.hpp). The mount
        // wants it: what goes with a replacement picture is only for the renderer that shows one.
        const auto render = chili::RenderSettings::load(mods_ini);
        mods.replace_textures = render.backend == chili::RenderSettings::Backend::D3D12;
        files.mount_mods(mods);
        for (const auto &problem : render.problems) std::cerr << "[render] " << mods_ini.filename().string() << ' ' << problem << '\n';
        chili::register_load_exec_exit(runtime);
        std::uint32_t compiled_sdk_version = 0u;
        std::uint32_t compiler_version = 0u;
        runtime.register_hle("SysMemUserForUser", 0x7591C7DBu,
            [&compiled_sdk_version, &files](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                compiled_sdk_version = ctx.gpr[4];
                files.set_compiled_sdk(compiled_sdk_version);
                std::cout << "[sysmem] compiled SDK " << psprecomp::hex32(compiled_sdk_version) << '\n';
                ctx.set_gpr(2u, 0u);
            });
        runtime.register_hle("SysMemUserForUser", 0xF77D77CBu,
            [&compiler_version](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                compiler_version = ctx.gpr[4];
                std::cout << "[sysmem] compiler version " << psprecomp::hex32(compiler_version) << '\n';
                ctx.set_gpr(2u, 0u);
            });
        for(const unsigned cache_nid:{0x79D1C3FAu,0xB435DEC5u})
        runtime.register_hle("UtilsForUser", cache_nid,
            [](auto &rt,auto &ctx) {
                // Guest RAM has one coherent backing store shared by CPU and GE.
                rt.memory().memory_barrier();ctx.set_gpr(2u,0u);
            });
        psprecomp::register_generated_functions(runtime);
        chili::TestHook::install(runtime);
        // The game's vibration calls go to the pad in use (pad_rumble_hook.hpp). Not in a deterministic run:
        // a host function over guest code moves its guest clock, and it reads no pad.
        if (!options.deterministic) chili::PadRumbleHook::install(runtime);
        // The entries' part while the game runs: a look at the game's menu with every pad sample, and the host
        // functions it cannot do without (the network start answered false; the main menu's line by two more
        // when its block does not carry the entry).
        std::unique_ptr<chili::SettingsEntry> settings_entry, network_answer;
        if (settings_off && !options.scripted() && chili::environment_text(L"PSPRECOMP_LIVE_CONTROL").empty()) {
            // Switched off in a player's run: Fiesta and Sync Highscores are the game's own lines again, and
            // neither may end the program, so the network start is still answered. Nothing else is put in place
            // and the game's menu is never looked at.
            std::cout << "[menu] no Settings line in the game's menus and no settings page in this run (" << (mods.settings_in_game ? "CHILI_PAGE=0" : "[Game] SettingsInGame=false") << ")\n";
            network_answer = std::make_unique<chili::SettingsEntry>(chili::SettingsEntry::Options{});
            for (const auto &line : network_answer->install(runtime)) std::cout << "[menu] " << line << '\n';
        }
        if (page_entries) {
            const bool in_block = files.menu_entries().main;
            settings_entry = std::make_unique<chili::SettingsEntry>(chili::SettingsEntry::Options{in_block, !in_block, entry_route.sync});
            for (const auto &line : settings_entry->install(runtime)) std::cout << "[menu] " << line << '\n';
            // Two of the window's messages are the entries' own, ahead of the input layer: F2, the hidden door
            // (bit 30 is set on auto-repeat), and a left click on the pause menu's Settings legend, which is that
            // legend's button and not the menu's Cross. Not in a deterministic run: it takes nothing from the window.
            if (!options.deterministic) chili::PcWindow::set_first_hook([](HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
                if (message == WM_KEYDOWN && w == VK_F2 && !(l & (LPARAM{1} << 30))) chili::SettingsEntry::hotkey_pressed();
                if (message == WM_LBUTTONDOWN) {
                    float x = 0.0f, y = 0.0f;
                    if (chili::window_pointer_in_picture(window, l, x, y) && chili::SettingsEntry::legend_click(x, y)) { result = 0; return true; }
                }
                return false;
            });
        }
        runtime.cpu().gpr[28] = module->gp;
        runtime.cpu().vfpu_ctrl[0] = 0xE4u;
        runtime.cpu().vfpu_ctrl[1] = 0xE4u;
        runtime.cpu().gpr[29] = 0x09FFF000u;
        runtime.cpu().gpr[31] = chili::ThreadRuntime::return_trampoline;
        const std::string argument = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const std::uint32_t argument_address = 0x09FFE000u;
        for (std::size_t i = 0; i <= argument.size(); ++i)
            runtime.memory().store8(argument_address + static_cast<std::uint32_t>(i),
                i == argument.size() ? 0u : static_cast<std::uint8_t>(argument[i]));
        runtime.cpu().gpr[4] = static_cast<std::uint32_t>(argument.size() + 1);
        runtime.cpu().gpr[5] = argument_address;
        std::uint32_t arena_begin = 0x08804000u;
        for (std::size_t i = 0; i < elf.segments().size(); ++i) {
            const auto &segment = elf.segments()[i];
            if (segment.type == 1u)
                arena_begin = std::max(arena_begin, elf.segment_runtime_address(i) + segment.memory_size);
        }
        chili::PartitionMemory partition(runtime, arena_begin, 0x09FF0000u);
        chili::ThreadRuntime scheduler(runtime,
            [&partition](std::uint32_t size) { return partition.reserve_stack(size); });
        scheduler.initialize_bootstrap();
        // Guest wall time. Deterministic mode derives it from the scheduler
        // clock and a fixed epoch; otherwise both services read the host clock.
        chili::UnixMicrosecondsSource guest_wall_clock;
        if (options.deterministic)
            guest_wall_clock = chili::make_scheduler_time_source(scheduler, chili::deterministic_epoch_unix_us);
        chili::register_rtc(runtime, guest_wall_clock);
        chili::LibcTimeRuntime libc_time(runtime, guest_wall_clock);
        chili::ThreadLifecycle lifecycle(runtime,scheduler,
            [&partition](std::uint32_t base){return partition.release_region(base);},
            &compiled_sdk_version);
        chili::VolatileMemoryRuntime volatile_memory(runtime,&scheduler,[&scheduler] {
            using Context = chili::VolatileMemoryRuntime::Context;
            if (scheduler.in_interrupt()) return Context::Interrupt;
            if (!scheduler.interrupts_enabled()) return Context::DispatchDisabled;
            return Context::Normal;
        });
        chili::UtilityDialogOwner utility_owner;
        chili::FixedPools fixed_pools(runtime, partition, scheduler);
        chili::EventFlags event_flags(runtime, scheduler);
        chili::DebugIO debug_io(runtime);
        chili::UmdRuntime umd(runtime, files, scheduler);
        chili::Semaphores semaphores(runtime, scheduler);
        chili::NetworkRuntime network(runtime, partition);
        if (options.deterministic) {
            // The network runtime draws a random MAC address; pin it.
            runtime.register_hle("sceWlanDrv", 0x0C622081u, [](auto &rt, auto &c) {
                if (!c.gpr[4] || !rt.memory().contains(c.gpr[4], 6u)) { c.set_gpr(2, 0x800200D3u); return; }
                const std::array<std::uint8_t, 6> mac{0x02u, 0x43u, 0x48u, 0x49u, 0x4Cu, 0x49u};
                rt.memory().copy_in(c.gpr[4], mac); c.set_gpr(2, 0u);
            });
        }
        chili::SystemSettings settings(runtime,ms0_root/"pc-system-settings.txt");
        std::unique_ptr<chili::ControllerSamplingProvenance> controller_sampling;
        if (const char *setting = std::getenv("PSPRECOMP_CONTROLLER_SAMPLING_PROVENANCE")) {
            if (*setting) {
                if (std::string_view(setting) != "1")
                    throw psprecomp::Error("PSPRECOMP_CONTROLLER_SAMPLING_PROVENANCE must be 1 or unset");
                controller_sampling = std::make_unique<chili::ControllerSamplingProvenance>(std::cout);
            }
        }
        const auto guest_now_us = [&scheduler] { return static_cast<std::uint64_t>(scheduler.now_us()); };
        // Input: live control channel, then the sample-indexed replay, then the
        // host devices. The recorder sees whatever the guest was given. In
        // deterministic mode live presses are timed on the guest clock.
        chili::InputReplay input_replay(options.input_replay);
        chili::live_control::DiagnosticLiveChannel live_control(chili::live_control::from_environment(), {}, {},
            options.deterministic ? chili::live_control::DiagnosticLiveChannel::Clock([&scheduler] { return static_cast<std::uint64_t>(scheduler.now_us() / 1000u); })
                                  : chili::live_control::DiagnosticLiveChannel::Clock{});
        // A run nobody steers by hand must not take the focus from the window the person is using.
        chili::background_windows = (options.scripted() || live_control.enabled() || start.unattended) && !options.watch;
        if (chili::background_windows) std::cout << "[options] scripted run: window behind the others, not activated (CHILI_WATCH=1 for an ordinary window)\n";
        // Control scheme: the replay's own when it names one, otherwise the ini file's.
        if (!input_replay.scheme().empty()) {
            const auto named = chili::scheme_from_name(input_replay.scheme());
            if (!named) throw psprecomp::Error("Input replay names an unknown control scheme: " + input_replay.scheme());
            input_config.scheme = *named;
        }
        std::cout << "[input] scheme " << chili::scheme_name(input_config.scheme) << '\n';
        // Whether this run has the game's own settings page and what is put in place for it (settings_apply.hpp):
        // a player's run has; a deterministic, replay or script run only when a test variable asks (CHILI_PAGE=1,
        // CHILI_SETTINGS_AT), so such a run is otherwise exactly what it was before the page existed.
        const bool settings_live = !settings_off && chili::settings::feature_enabled(options.scripted() || live_control.enabled());
        chili::InputRecorder input_recorder(options.input_record, chili::scheme_name(input_config.scheme));
        chili::InputPipeline input(live_control, input_replay, input_recorder, !options.deterministic);
        // dual_analog puts host functions over the game's control queries; psp leaves the game alone.
        std::unique_ptr<chili::DualAnalog> dual_analog;
        if (input_config.scheme == chili::InputScheme::DualAnalog) {
            dual_analog = std::make_unique<chili::DualAnalog>(runtime, options.input_trace);
            // The stick's turn as an angle and turning on the ground after a dive: the ini's choice. A
            // deterministic run that names no ini keeps the game's own turning, which its replays were
            // recorded with.
            dual_analog->set_exact_turn(input_config.exact_turn && (!options.deterministic || !options.config_file.empty()));
            // The settings page can switch ExactTurn while the game runs: the host function that takes is put
            // in place now, whatever the setting says. Not in a deterministic run (it moves the guest clock):
            // there a change to "on" waits for a restart.
            if (settings_live && !options.deterministic) dual_analog->prepare_exact_turn();
            dual_analog->set_tight_view(tight_camera);
        }
        // The bindings and the Prompts and Glyphs settings, in every kind of run: the on-screen prompts are
        // drawn from them, also in a deterministic run that names an ini (which reads no keyboard, mouse or pad).
        chili::PcInput::instance().configure(input_config);
        if (!options.deterministic) {
            // Keyboard, mouse and gamepads; the window feeds the input layer.
            // dual_analog knows when the player is being steered and says so with every sample; until the
            // first one the game is starting up, and a click then must not capture the mouse.
            chili::PcInput::instance().set_look_active(dual_analog == nullptr);
            // A session with the script channel never captures the mouse. A replay keeps it free
            // while it plays; when it has ended the player has the input and a click captures.
            if (live_control.enabled()) chili::PcInput::instance().forbid_capture();
            // CHILI_FAKE_XINPUT=<file>: Xbox-style pads made up from a text file, for checks of the real program on a
            // PC without a controller (fake_xinput.hpp).
            if (const auto fake = chili::environment_text(L"CHILI_FAKE_XINPUT"); !fake.empty())
                chili::install_fake_xinput(chili::PcInput::instance(), std::filesystem::path(fake));
            // An open settings page takes the pointer, the mouse buttons and the wheel before the input layer does.
            chili::PcWindow::set_input_hook({
                [](HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
                    if (chili::page_window_message(window, message, w, l, result)) return true;
                    return chili::PcInput::instance().window_message(window, message, w, l, result); },
                [](HWND window) { chili::PcInput::instance().attach(window); }});
        } else if (settings_live) {
            // A deterministic run reads no device; with the page asked for (CHILI_PAGE=1) the window's mouse
            // messages still reach the page, for a test that posts them.
            chili::PcWindow::set_input_hook({
                [](HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) { return chili::page_window_message(window, message, w, l, result); }, nullptr});
        }
        chili::InputTitle input_title;
        // psp: the game is not asked about any control, so only the main menu can be told from play.
        chili::MenuScene psp_menu;
        // Interactive runs: how long a pad read takes to reach the window (present_latency.hpp), for the
        // pictures of a mission and for the ones a menu or a movie draws straight to the screen.
        chili::PresentLatency present_latency, menu_latency;
        // The on-screen prompts' controller, once it has started (below): the pad-sample path tells it of every
        // sample the player owns, for the screens that end no game frame.
        chili::PromptController *prompt_samples = nullptr;
        chili::ControllerRuntime controller(runtime,scheduler,[&runtime,&input,&dual_analog,&input_title,&psp_menu,&present_latency,&menu_latency,&prompt_samples,&settings_entry,host_input=!options.deterministic] {
            // The mouse is an angle per frame; the engine's own frame time turns it into the rate it asks for.
            // This is the real frame time. Not checked: whether the player script turns by the real or by the game
            // frame time in slow motion; if by the game one, mouse turning slows down with the world there.
            auto &pc = chili::PcInput::instance();
            if (host_input) {
                // The mouse as it is now, not as it was at the last timer pump.
                pc.pump_mouse();
                pc.set_frame_seconds(std::bit_cast<float>(runtime.memory().load32(chili::real_delta_time_address)));
                // While a replay or a script command owns the samples the player's devices are set aside.
                pc.set_scripted(input.next_source() != chili::InputSource::Host);
            }
            // While the settings page is open the sample is the page's and the game gets a neutral one; what
            // was held when it closed stays out until it is let go. Nothing in a run without the page.
            const auto sample = chili::page_sample(input.sample());
            if (host_input) { present_latency.sampled(); menu_latency.sampled(); }
            // The briefing's Continue and screens like it read the pad without ending a frame: the device used
            // last still decides the prompt pictures there (prompt_style.hpp). Not for a replay's or a script's
            // sample, and never in a deterministic run.
            if (host_input && prompt_samples && input.source() == chili::InputSource::Host) (void)prompt_samples->sampled();
            chili::TestHook::mark(input.samples() - 1u);
            // The ways into the settings page from the game's own menus (settings_entry.hpp): a look at the menu
            // the game has up, and what opened the page is kept from the game, so the game gets `given`. After
            // the page's own part: an open page has taken the sample before this.
            auto given = sample;
            if (settings_entry) settings_entry->sampled(runtime.memory(), given.buttons, input.samples() - 1u);
            if (dual_analog) {
                dual_analog->sampled(given);
                if (host_input) {
                    pc.set_look_active(dual_analog->look_active());
                    dual_analog->set_hold_pitch(pc.mouse_captured() && pc.config().mouse_hold_pitch);
                }
            } else if (host_input) pc.set_look_active(!psp_menu.loaded(runtime.memory()));
            // The title bar says what the mouse is doing and, with the input test on, what the game was given.
            if (host_input) input_title.update(pc.window(), pc, input.source(), given, pc.now());
            return given;
        });
        controller.set_sampling_observer(controller_sampling.get());
        // Interactive runs take one pad sample per vblank; deterministic runs keep
        // the fixed sample period their replays and reference hashes were made with.
        controller.set_vblank_latch(!options.deterministic);
        auto utility_foreground_owned = [] {
            DWORD foreground_pid{};
            const auto foreground_window = GetForegroundWindow();
            return foreground_window && GetWindowThreadProcessId(foreground_window,&foreground_pid) &&
                   foreground_pid == GetCurrentProcessId();
        };
        auto utility_buttons = [&]() -> std::optional<std::uint32_t> {
            // Live commands and an active replay are explicit input sources.
            if (const auto scripted = input.scripted_buttons()) return scripted;
            if (!utility_foreground_owned()) return std::nullopt;
            // The gamepad only: the dialogs take the keyboard and the mouse themselves.
            const auto host = chili::PcInput::instance().peek_pad();
            if (!utility_foreground_owned()) return std::nullopt;
            return host.buttons;
        };
        chili::SasRuntime sas(runtime);
        chili::AvRuntime av(runtime,&sas);
        chili::AtracRuntime atrac(runtime,av,&scheduler);
        chili::MpegRuntime mpeg(runtime,av,&scheduler);
        // Sound output: [Audio] of the same ini, then CHILI_AUDIO_* (audio_settings.hpp).
        const auto audio_settings = chili::AudioSettings::load(mods_ini);
        for (const auto &problem : audio_settings.problems) std::cerr << "[audio] " << mods_ini.filename().string() << ' ' << problem << '\n';
        chili::AudioOptions audio_options{options.null_audio,options.audio_wav,options.audio_trace};
        audio_settings.apply_to(audio_options);
        chili::AudioRuntime audio(runtime,&scheduler,audio_options);
        chili::PsmfRuntime psmf(runtime);
        chili::PowerRuntime power(runtime);
        // Destroy telemetry and GE before their optional observer storage.
        std::unique_ptr<chili::ParallelLiveObserver> parallel_observation;
        std::unique_ptr<chili::PerformanceTelemetry> performance;
        if (const char *setting = std::getenv("PSPRECOMP_PERFORMANCE_TELEMETRY")) {
            if (*setting) {
                if (std::string_view(setting) != "1")
                    throw psprecomp::Error("PSPRECOMP_PERFORMANCE_TELEMETRY must be 1 or unset");
                performance = std::make_unique<chili::PerformanceTelemetry>(std::cout);
            }
        }
        if (const char *setting = std::getenv("PSPRECOMP_PARALLEL_SHADE_OBSERVATION")) {
            if (*setting) {
                if (std::string_view(setting) != "1")
                    throw psprecomp::Error("PSPRECOMP_PARALLEL_SHADE_OBSERVATION must be 1 or unset");
                if (!chili::kParallelShadeObservationCompiled)
                    throw psprecomp::Error("Parallel shading observation requires CHILI_PARALLEL_SHADE_OBSERVATION=ON at build time");
                if (!performance)
                    throw psprecomp::Error("Parallel shading observation requires PSPRECOMP_PERFORMANCE_TELEMETRY=1");
                parallel_observation = std::make_unique<chili::ParallelLiveObserver>();
                performance->set_parallel_live_observer(parallel_observation.get());
            }
        }
        scheduler.set_pacing_observer(performance.get());
        audio.set_pacing_observer(performance.get());
        chili::GeRuntime ge(runtime,&scheduler,partition.reserve_stack(16384u),&compiled_sdk_version,performance.get());
        ge.set_parallel_live_observer(parallel_observation.get());
        chili::DisplayRuntime display(runtime,scheduler,performance.get());
        display.set_pacing_observer(performance.get());
        // Frame timer, state hash, frame images and the frame limit all hang off
        // the engine's once-per-frame vblank wait. Destroyed before the display.
        chili::FrameProbe frame_probe(runtime,scheduler,display,options,[&input] { return input.samples(); });
        live_control.set_context([&input,&frame_probe,&scheduler] {
            return " sample=" + std::to_string(input.samples()) + " frame=" + std::to_string(frame_probe.frame()) +
                   " guest_us=" + std::to_string(scheduler.now_us());
        });
        live_control.set_quit_handler([&runtime] { runtime.stop(live_quit_reason); });
        chili::Presentation *presentation_parent = nullptr;
        const auto presentation_window = [&presentation_parent] {
            return presentation_parent ? presentation_parent->window_handle() : nullptr;
        };
        // Scripted presenters answer on guest time and never open a window;
        // the Win32 ones wait for the player.
        std::unique_ptr<chili::SaveSlotPresenter> save_presenter;
        std::unique_ptr<chili::MessageDialogPresenter> message_presenter;
        if (options.auto_dialogs) {
            save_presenter = std::make_unique<chili::ScriptedSaveSlotPresenter>(guest_now_us,options.auto_dialog_delay_us,options.auto_save_slot);
            message_presenter = std::make_unique<chili::ScriptedMessageDialogPresenter>(guest_now_us,options.auto_dialog_delay_us,options.auto_message);
        } else {
            save_presenter = std::make_unique<chili::Win32SaveSlotPresenter>(presentation_window, true, utility_buttons);
            message_presenter = std::make_unique<chili::Win32MessageDialogPresenter>(presentation_window, true, utility_buttons);
        }
        chili::NativeSavedataBackend native_savedata(runtime.memory(),ms0_root,
            [](const chili::savedata_storage::CommitResult &result) {
                std::cerr << "[savedata] committed profile retained transaction cleanup: "
                          << result.recovery_directory << "; backup=" << result.backup_directory
                          << "; staging=" << result.staged_directory << '\n';
            });
        chili::SavedataRuntime savedata(runtime,ms0_root,&scheduler,
            &utility_owner,save_presenter.get(),&native_savedata);
        chili::MessageDialogRuntime message_dialog(runtime,message_presenter.get(),guest_now_us,&utility_owner);
        // Optional host-time bound for unattended runs (PSPRECOMP_PROBE_SECONDS).
        // Unset means normal interactive lifetime; no deadline is imposed.
        const auto probe_seconds=options.probe_seconds;
        const auto probe_started=std::chrono::steady_clock::now();
        chili::Presentation presentation(runtime,display,scheduler,true,
            [&savedata,&message_dialog] { savedata.tick(); message_dialog.tick(); },performance.get());
        // The displayed buffer; the game sets it once and keeps it.
        unsigned display_address = psprecomp::GuestMemory::kVramPhysicalBase;
        // The surface whose picture the window shows: the displayed buffer, or the scene surface after
        // an early present (below). Screenshots and frame images read this one.
        unsigned shown_address = display_address;
        bool gpu = false;
        presentation.set_host_tick([&runtime,&display,&live_control,&presentation_parent,&gpu,&shown_address,&ge,probe_seconds,probe_started] {
            live_control.host_pump([&](const std::filesystem::path &path) {
                // The hardware renderer's shown surface, read back from the GPU.
                if (gpu) {
                    bool saved = false;
                    ge.render_call([&saved, &shown_address, &path] { saved = chili::save_gpu_display(shown_address, path); }, true);
                    return saved;
                }
                // The displayed guest framebuffer as it stands now; the window's
                // last presented copy is the fallback before the display is set up.
                const auto frame=display.snapshot();
                if (frame.configured && chili::save_display_frame(runtime.memory(),frame.address,frame.stride,frame.format,path)) return true;
                return presentation_parent && presentation_parent->save_frame(path);
            });
            if (probe_seconds && std::chrono::steady_clock::now()-probe_started>=std::chrono::seconds(probe_seconds))
                runtime.stop(probe_deadline_reason);
        });
        presentation_parent = &presentation;
        // Renderer. Direct3D 12 keeps its own surfaces and puts the displayed one in
        // the game window; guest VRAM is then left alone, apart from movie frames.
        // The software rasterizer writes guest VRAM, which GDI shows. Either way a
        // picture is shown when a list ends on the displayed buffer: the menu list,
        // or the list that copies a finished scene to the screen. Direct3D 12 also
        // shows a finished scene straight from its own surface (early present).
        std::string render_report;
        // CHILI_GE_NULL_DRAW has installed a backend that draws nothing; it stays,
        // and no renderer is started over it.
        if (ge.draw_backend) render_report = "draws are consumed without drawing (CHILI_GE_NULL_DRAW)";
        else gpu = chili::start_gpu_backend(render, render.resolved_scale(static_cast<unsigned>(GetSystemMetrics(SM_CYSCREEN)), options.scripted()), render_report);
        std::cout << "[render] " << render_report << '\n';
        if (gpu) std::cout << "[render] preset " << render.preset_name() << " ([Rendering] Preset=low|medium|high|custom)\n";
        // The pipeline states and surfaces earlier runs had to create, created now instead of in the
        // first frames that use them (gpu_warm_up.hpp).
        std::unique_ptr<chili::GpuWarmUp> warm_up;
        if (gpu) {
            warm_up = std::make_unique<chili::GpuWarmUp>(executable_directory() / "ChiliNative.gpucache");
            std::cout << "[render] " << warm_up->apply() << '\n';
        }
        // The slow renderer says so in the title bar: after a failed Direct3D 12 start nothing else
        // tells a player why the game crawls.
        if (!gpu && !ge.draw_backend) SetWindowTextW(presentation.ensure_window(), L"Chili Con Carnage - Native PC (software renderer)");
        // A pack's letter spacing was put in force for the Direct3D 12 renderer, which then did not start.
        if (!gpu && files.font_spacing().applied)
            std::cout << "[mods] font advances: in force, but the renderer that shows " << files.font_spacing().pack << "'s font sheet did not start\n";
        // Declared after the window so the renderer lets go of it first.
        struct GpuShutdown {
            chili::GeRuntime &ge; const bool &active;
            ~GpuShutdown() {
                ge.stop_worker();
                ge.list_end = {};
                if (!active) return;
                ge.draw_backend = {};
                for (const auto &line : chili::texture_mods_summary()) std::cout << "[mods] " << line << '\n';
                vcs::ge_gpu_backend_set_native_window(nullptr);
                vcs::shutdown_ge_gpu_backend();
                vcs::ge_texture_mods_stop();
            }
        } gpu_shutdown{ge, gpu};
        std::uint64_t gpu_frames = 0u;
        bool gpu_window = gpu;
        if (gpu) {
            // Texture replacement and dump: mods\<name>\textures\<key>.png and [Mods] DumpTextures.
            for (const auto &line : chili::start_texture_mods(mods)) std::cout << "[mods] " << line << '\n';
            // The pictures of texture packs are decoded on their own threads while the game boots. A
            // deterministic run waits for them here: its clock is the guest's, so the wait moves nothing in
            // the game, and every one of its frames is then drawn with the same pictures in every run.
            if (options.deterministic) vcs::ge_texture_mods_wait_idle();
            chili::attach_gpu_draw_backend(ge, render.strict);
            presentation.set_gpu_presents(true);
            vcs::ge_gpu_backend_set_native_window(presentation.ensure_window());
            vcs::ge_gpu_backend_set_display_framebuffer(display_address);
            // Decoded movie pictures are the one thing the CPU writes into VRAM.
            mpeg.set_frame_observer([&ge](std::uint32_t destination, std::uint32_t, std::uint32_t width, std::uint32_t height,
                                       std::span<const std::uint8_t> rgba) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::cout << "[render] movie pictures " << width << 'x' << height << " go to the surface at " << psprecomp::hex32(destination) << '\n';
                }
                // In order with the lists: where they run. The decoder's buffer is its own, so the picture is copied.
                auto picture = std::make_shared<std::vector<std::uint8_t>>(rgba.begin(), rgba.end());
                ge.render_call([destination, width, height, picture] {
                    if (!vcs::ge_gpu_backend_write_surface_rgba(destination, width, height, std::as_bytes(std::span<const std::uint8_t>(*picture)), vcs::ge_gpu_config().internal_scale > 1u))
                        (void)vcs::runtime_log_once("movie picture not copied to its surface");
                }, false);
            });
            frame_probe.set_frame_saver([&ge, &shown_address](const std::filesystem::path &path) {
                bool saved = false;
                ge.render_call([&saved, &shown_address, &path] { saved = chili::save_gpu_display(shown_address, path); }, true);
                return saved;
            });
        }
        // On-screen button prompts that show the input in use (prompt_style.hpp): pictures of the bound keys or
        // pad buttons over the PSP ones in seven of the game's textures, the style decided at every frame start.
        // After the texture mods have started and before the first draw.
        chili::PromptController prompts;
        // A deterministic run's clock is its frame count; a scripted run in real time that follows the input
        // (CHILI_PROMPTS=auto) goes by the real clock, like a player's.
        // With the settings page the pictures are painted also when the file says Prompts=psp: the setting can
        // leave psp while the game runs.
        for (const auto &line : prompts.start(input_config, gpu ? vcs::ge_gpu_config().internal_scale : 0u, options.scripted() || live_control.enabled(), options.deterministic, settings_live))
            std::cout << "[prompts] " << line << '\n';
        // CHILI_OVERLAY_TEST: a fixed page for the overlay pass (overlay_test_page.hpp). Without the variable it
        // watches nothing and its frame() returns at once.
        chili::OverlayTestPage overlay_test;
        for (const auto &line : overlay_test.start(gpu)) std::cout << "[overlay-test] " << line << '\n';
        // The pause menu's legend for the settings page (settings_entry_legend.hpp): drawn while a pause window
        // that has no Settings line has the focus. Its guard goes first, so no sample finds a painter that is gone.
        chili::PauseLegend pause_legend;
        struct LegendGuard { chili::SettingsEntry *entry; ~LegendGuard() { if (entry) entry->set_legend({}); } } legend_guard{settings_entry.get()};
        if (settings_entry) {
            pause_legend.start(gpu);
            settings_entry->set_legend([&pause_legend, &runtime, entry = settings_entry.get()](chili::SettingsEntry::Legend state) {
                pause_legend.set(state, runtime.memory());
                // A little more than the legend covers: a pointer is not placed to the pixel.
                const auto &box = pause_legend.box();
                entry->set_legend_area(box.x0 - 3.0f, box.y0 - 3.0f, box.x1 + 3.0f, box.y1 + 3.0f);
            });
        }
        // The settings while the game runs (settings_apply.hpp): what the settings page reads, changes and writes.
        // Declared after everything it works on (the input layer, the two-stick layer) so that it goes first.
        std::unique_ptr<chili::settings::Live> live_settings;
        chili::settings::Schedule settings_steps;
        struct LiveSettingsSlot { ~LiveSettingsSlot() { chili::settings::live_slot() = nullptr; } } live_settings_slot;
        if (settings_live) {
            const bool scripted_run = options.scripted() || live_control.enabled();
            chili::settings::Host host;
            // A deterministic run that names no ini has read none for its input: it has no file here either.
            if (!options.deterministic || !options.config_file.empty()) host.ini = mods_ini;
            // A scripted run writes only to a file of its own that CHILI_CONFIG names; scripts\unattended.ini is
            // every scripted run's settings and is read, never written.
            host.writable = !host.ini.empty() && !(scripted_run && (options.config_file.empty() || chili::settings::same_name(mods_ini.filename().string(), "unattended.ini")));
            host.input = &chili::PcInput::instance();
            if (dual_analog) host.exact_turn = [&dual_analog](bool on) { return dual_analog->switch_exact_turn(on); };
            // Screen: the window's own state and its switch (what F11 does), so the row changes it at once.
            host.fullscreen = [&presentation] { return presentation.fullscreen(); };
            host.set_fullscreen = [&presentation](bool on) { presentation.set_fullscreen(on); };
            host.mods_folder = mods.folder;
            const auto lock = std::find_if(mods.game_config.begin(), mods.game_config.end(), [](const auto &entry) { return chili::ModSetup::same_text(entry.first, "frame_lock"); });
            host.started = chili::settings::started_values(render, mods, audio_settings, tight_camera, lock != mods.game_config.end() ? std::string_view(lock->second) : std::string_view("2"),
                mods.raised_priority(!scripted_run && !start.unattended), render.fullscreen && !options.scripted() && !chili::background_windows);
            std::cout << "[settings] the settings can change while the game runs: " << (host.ini.empty() ? std::string("no settings file in this run") : host.ini.string())
                      << (host.writable ? "" : " (read, not written: a scripted run writes only to a file of its own named by CHILI_CONFIG)") << '\n';
            live_settings = std::make_unique<chili::settings::Live>(std::move(host));
            chili::settings::live_slot() = live_settings.get();
            std::vector<std::string> step_problems;
            settings_steps = chili::settings::Schedule(chili::settings::environment_value("CHILI_SETTINGS_AT"), step_problems);
            for (const auto &problem : step_problems) std::cerr << "[settings] " << problem << '\n';
        }
        // The settings page itself (settings_page.cpp): drawn by the overlay pass, worked by the pad samples and
        // the window's mouse messages, opened by the game's menus (settings_page_api.hpp) or by a test step.
        // Declared after the settings and the renderer's shutdown so that it lets go of both first.
        struct SettingsPageSlot { ~SettingsPageSlot() { for (const auto &line : chili::page_report()) std::cout << "[settings] " << line << '\n'; chili::page_stop(); } } settings_page_slot;
        if (live_settings) {
            chili::PageStart page;
            page.live = live_settings.get();
            page.gpu = gpu;
            page.frame_clock = options.deterministic;
            page.scripted = options.scripted() || live_control.enabled();
            page.window = [&presentation] { return presentation.window_handle(); };
            page.set_fullscreen = [&presentation](bool on) { presentation.set_fullscreen(on); };
            page.save_frame = [&ge, &shown_address](const std::filesystem::path &path) {
                bool saved = false;
                ge.render_call([&saved, &shown_address, &path] { saved = chili::save_gpu_display(shown_address, path); }, true);
                return saved;
            };
            page.request_window_capture = [&ge] { ge.render_call([] { vcs::ge_gpu_backend_request_window_capture(); }, false); };
            page.take_window_capture = [&ge](vcs::GeGpuWindowCapture &capture) {
                bool taken = false;
                ge.render_call([&capture, &taken] { taken = vcs::ge_gpu_backend_take_window_capture(capture); }, true);
                return taken;
            };
            // The Change legend: one D-pad with both arms lit where the pictures of D-pad left and right are
            // the same D-pad (2: the PSP's own; 1: an Xbox or PlayStation pad's as the prompts paint it), two
            // keys side by side otherwise (0).
            page.dpad_pictures = [&prompts] {
                if (!prompts.active() || prompts.shown() == chili::PromptStyle::Psp) return 2;
                const auto &config = chili::PcInput::instance().config();
                return chili::prompt_dpad_pair(chili::prompt_glyph(config, prompts.shown(), chili::PadTarget::DpadLeft),
                                               chili::prompt_glyph(config, prompts.shown(), chili::PadTarget::DpadRight)) ? 1 : 0;
            };
            for (const auto &line : chili::page_start(std::move(page))) std::cout << "[settings] " << line << '\n';
        }
        frame_probe.set_frame_start([&prompts, &live_settings, &settings_steps, &overlay_test, &runtime, &ge, &presentation](std::uint64_t frame) {
            // CHILI_SETTINGS_AT: values set as the page would set them, before the prompts look at their settings.
            if (live_settings && !settings_steps.empty()) for (const auto &line : settings_steps.frame(frame, *live_settings)) std::cout << "[settings] " << line << '\n';
            // The page's test steps and its draw list for the frame that starts; a row may change what the prompts show.
            chili::page_frame(frame, runtime.memory());
            prompts.frame(frame);
            overlay_test.frame(frame, runtime.memory(), ge, presentation);
        });
        // From here on the pad-sample path looks at the style too; not after `prompts` is gone.
        struct PromptSamples {
            chili::PromptController *&slot;
            ~PromptSamples() { slot = nullptr; }
        } prompt_samples_guard{prompt_samples};
        prompt_samples = &prompts;
        display.set_framebuffer_observer([&gpu, &ge, &display_address, &shown_address](unsigned address) {
            if (!address || address == display_address) return;
            display_address = address;
            // shown_address and the renderer belong to the thread that runs the lists.
            ge.render_call([&gpu, &shown_address, address] {
                shown_address = address;
                if (gpu) vcs::ge_gpu_backend_set_display_framebuffer(address);
            }, false);
        });
        // Early present. In a mission the game draws each scene, HUD and fades included, into a surface of
        // its own (scene_surface) at the end of its frame-end function, and only the next frame end copies
        // that to the displayed buffer: a finished picture waits one game frame. The game says a copy is
        // due with the byte it tests before it enqueues that copy (GfxInternal_PSP::EndFrame, 0x08825700;
        // set at 0x08825788 right before the scene list goes out, cleared while a movie plays). When a list
        // with draws ends on the scene surface and the byte is set, the surface is put in the window at once.
        // The copy still happens a frame later and shows the same picture again. Nothing the game reads
        // changes. Menus, loading screens and movies never meet the three conditions and keep the old path.
        constexpr unsigned scene_surface = 0x040D8000u;
        constexpr std::uint32_t scene_copy_due_address = 0x08DD03E0u;
        const bool early_present = gpu && render.early_present;
        std::uint64_t early_presents = 0u;
        // The window shows a scene the game has not copied to the displayed buffer yet. That copy is the
        // same picture again: its two draws stay queued and go out with the next scene, without a
        // submission and a present of their own (one of each per game frame instead of two).
        bool scene_on_screen = false;
        if (gpu) std::cout << "[render] early present " << (early_present ? "on" : "off") << '\n';
        // What the end of a list needs from the game's side, taken when the game hands the list over: the
        // displayed buffer (low word), the copy-due byte (bit 32) and the number of the frame's pad read
        // (bits 33 up; present_latency.hpp).
        ge.list_plan = [&runtime, &display_address, &present_latency] {
            return std::uint64_t{display_address} | (runtime.memory().load8(scene_copy_due_address) != 0u ? std::uint64_t{1} << 32u : 0u) |
                   std::uint64_t{present_latency.stamp()} << 33u;
        };
        ge.list_end = [&gpu, &gpu_window, &gpu_frames, &shown_address, &presentation, &early_presents, &present_latency, &menu_latency, &scene_on_screen, early_present](unsigned framebuffer, unsigned draws, std::uint64_t plan) {
            const unsigned display_address = static_cast<unsigned>(plan);
            const bool displayed = !((framebuffer ^ display_address) & 0x1FFFF0u);
            const bool copy_due = ((plan >> 32u) & 1u) != 0u;
            const bool scene = !displayed && early_present && draws && !((framebuffer ^ scene_surface) & 0x1FFFF0u) && copy_due;
            if (!displayed && !scene) return;
            if (!gpu) { presentation.frame_complete(); return; }
            if (displayed && copy_due && scene_on_screen) { scene_on_screen = false; return; }
            // The player closed the window: the swapchain goes with it, and the
            // lists keep being drawn until the game has taken its exit callback.
            if (gpu_window && !presentation.window_handle()) { gpu_window = false; vcs::ge_gpu_backend_set_native_window(nullptr); }
            if (scene) vcs::ge_gpu_backend_set_display_framebuffer(scene_surface);
            // Never a blocking sync interval here: the game thread is paced on guest time.
            if (vcs::ge_gpu_backend_finish_color_frame(++gpu_frames)) presentation.count_frame();
            if (scene) { vcs::ge_gpu_backend_set_display_framebuffer(display_address); ++early_presents; }
            scene_on_screen = scene;
            shown_address = scene ? scene_surface : display_address;
            // A scene shown early and a menu's own list are the picture of the frame that read the pad; the
            // game's copy shows the scene of the frame before (again, when that was already shown early).
            const auto pad_read = static_cast<std::uint32_t>(plan >> 33u);
            if (scene) present_latency.presented(true, pad_read);
            else if (!copy_due) menu_latency.presented(true, pad_read);
            else if (!early_present) present_latency.presented(false, pad_read);
        };
        // Early draw. The game closes a frame's scene list before the frame-end vblank wait and enqueues it
        // after the wait (after two at 30 frames per second), so the whole wait sat between the pad read and
        // the picture. At the frame boundary the list the game is about to enqueue is already complete: it is
        // run there (GeRuntime::run_early), which with early present puts the picture in the window a frame
        // period sooner. The game's enqueue then finds it done. The list is the one EndFrame picks at
        // 0x088257B8: the pointer at +12 or +16 of the list pair, by bit 0 of the byte at +11. Only while the
        // copy-due byte is set: menus, loading screens and movies keep the game's own order.
        constexpr std::uint32_t gfx_internal_address = 0x08DF2658u;
        if (early_present) frame_probe.set_frame_draw([&runtime, &ge] {
            const auto &memory = runtime.memory();
            if (memory.load8(scene_copy_due_address) == 0u) return;
            // EndFrame's own test before its enqueue (0x08825780 to 0x088257B4): with the object at 0x08DF2708
            // in the state where it only waits for the GE, the game enqueues nothing this frame.
            const std::uint32_t halt = memory.load32(0x08DF2708u);
            if (halt && memory.contains(halt, 400u) && memory.load8(halt + 394u) != 0u && memory.load8(halt + 324u) == 0u &&
                memory.load8(0x08DF2678u) == 0u) return;
            const std::uint32_t self = memory.load32(gfx_internal_address);
            if (!self || !memory.contains(self, 4u)) return;
            const std::uint32_t pair = memory.load32(self);
            if (!pair || !memory.contains(pair, 20u)) return;
            (void)ge.run_early(runtime, memory.load32(pair + 12u + 4u * (memory.load8(pair + 11u) & 1u)));
        });
        // Interactive runs hold guest time to real time; deterministic runs go
        // as fast as the host allows.
        std::unique_ptr<chili::RealTimePacer> pacer;
        if (!options.deterministic && !options.unpaced) {
            // The game thread one step above normal priority: for a player, not for a scripted run.
            const bool raised = mods.raised_priority(!options.scripted() && !live_control.enabled() && !start.unattended);
            std::cout << "[options] game thread priority: " << (raised ? "above normal" : "normal") << " ([Game] Priority=high|normal)\n";
            pacer = std::make_unique<chili::RealTimePacer>([&presentation] { presentation.pump_window(); }, raised);
            scheduler.set_idle_hook([&pacer](std::uint64_t wake_us) { pacer->idle_until(wake_us); });
        }
        presentation.set_paced(pacer != nullptr);
        // The display lists on a thread of their own ([Rendering] GeThread, ge_runtime.hpp). Deterministic
        // runs, the software renderer and runs that capture or trace lists draw where the game hands a list over.
        if (gpu && render.ge_thread && (!options.deterministic || render.ge_thread_forced) && ge.can_use_worker()) {
            const bool raised = pacer != nullptr && mods.raised_priority(!options.scripted() && !live_control.enabled());
            ge.wait_pump = [&presentation] { presentation.pump_window(); };
            ge.start_worker(runtime, [raised] { if (raised) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); });
        }
        if (gpu) std::cout << "[render] display lists " << (ge.worker_running() ? "on their own thread" : "on the game thread") << " ([Rendering] GeThread=true|false)\n";
        // Unattended runs stay in their window; a player's run may start in full screen.
        if (render.fullscreen && !options.scripted() && !chili::background_windows) presentation.set_fullscreen(true);
        // Report the stock virtual PSP clocks. Chili imports only the integer
        // queries; these values do not describe the host CPU's clock rate.
        runtime.register_hle("scePower",0xFDB5BFE9u,[](auto &,auto &c) { c.set_gpr(2,222u); });
        runtime.register_hle("scePower",0xBD681969u,[](auto &,auto &c) { c.set_gpr(2,111u); });
        std::cout << "Chili native bootstrap: module=" << module->name
                  << " entry=" << psprecomp::hex32(elf.runtime_entry())
                  << " functions=" << runtime.function_count() << '\n';
        // Explicit limits remain available for repeatable bring-up probes.
        // Interactive launches run until the game exits or an error occurs.
        const auto limit = positional.size() == 3u ? std::stoull(positional[2]) : std::numeric_limits<std::uint64_t>::max();
        // CHILI_SAMPLER=<file>: sample this thread while the guest runs (sampling_profiler.hpp).
        auto sampler = chili::SamplingProfiler::from_environment([&frame_probe] { return frame_probe.frame(); });
        runtime.run(elf.runtime_entry(), limit);
        sampler.reset();
        // The lists still queued are drawn; the renderer is this thread's again.
        ge.stop_worker();
        scheduler.set_idle_hook({});
        runtime.report_hle_histogram(194u);
        presentation.tick(true);
        if (performance) {
            performance->poll(chili::PerformanceTelemetry::Clock::now(),scheduler.now_us(),true);
            if (performance->output_failed())
                std::cerr << "[perf] telemetry output failed\n";
        }
        std::cout << "[display] native presented frames=" << presentation.presented_frames() << '\n';
        if (early_present) std::cout << "[render] scenes shown early: " << early_presents << ", drawn at the frame boundary ahead of the game's enqueue: " << ge.early_lists() << '\n';
        if (present_latency.count()) std::cout << present_latency.report("mission") << '\n';
        if (menu_latency.count()) std::cout << menu_latency.report("menu and movie") << '\n';
        if (warm_up) std::cout << "[render] " << warm_up->save() << '\n';
        if (gpu) {
            // What the graphics card needed per submission (one per game frame in a mission): the figure that
            // says how much slower a card may be.
            const auto gpu_time = vcs::ge_gpu_backend_gpu_time();
            if (gpu_time.submissions) {
                char line[200];
                std::snprintf(line, sizeof(line), "[render] gpu time per submission at scale %u: count=%llu mean=%.2f ms p50=%.2f p99=%.2f max=%.1f",
                    vcs::ge_gpu_config().internal_scale, static_cast<unsigned long long>(gpu_time.submissions), gpu_time.mean_ms, gpu_time.p50_ms, gpu_time.p99_ms, gpu_time.max_ms);
                std::cout << line << '\n';
            }
            const auto report = vcs::ge_gpu_backend_report();
            std::cout << "[render] d3d12 draws=" << report.game_draw_calls << " pipelines=" << report.unique_pipeline_keys
                      << " texture_uploads=" << report.texture_image_uploads << " surfaces=" << report.dx12_native_framebuffer_targets
                      << " feedback_snapshots=" << report.dx12_self_feedback_snapshots << " fxaa_passes=" << report.dx12_fxaa_passes << " ao_passes=" << report.dx12_ao_passes
                      << (report.perf_finish_frame_calls ? " gpu_ms_mean=" + std::to_string(static_cast<double>(report.perf_finish_frame_ns) / 1.0e6 / static_cast<double>(report.perf_finish_frame_calls)) : std::string()) << " submissions=" << report.transfer_submissions << " dropped_geometry=" << report.game_vertex_overflows
                      << " approximated: " << (report.fallbacks_seen ? vcs::ge_gpu_fallback_names(report.fallbacks_seen) : std::string("nothing")) << '\n';
            // The glow drawn at the PSP's size ([Rendering] Glow=smooth or off); with Glow=game there is no such line.
            if (report.dx12_glow_blocks)
                std::cout << "[render] glow: " << report.dx12_glow_blocks << " blocks drawn at the PSP's size, " << report.dx12_glow_readers
                          << " draws read them" << (report.dx12_glow_left_out ? ", " + std::to_string(report.dx12_glow_left_out) + " left out (Glow=off)" : std::string()) << '\n';
        }
        for (const auto &line : overlay_test.report()) std::cout << "[overlay-test] " << line << '\n';
        frame_probe.report();
        audio.report(std::cout);
        chili::TestHook::report();
        if (settings_entry) std::cout << "[menu] " << settings_entry->report() << "; the pause legend drawn " << pause_legend.shown() << " time" << (pause_legend.shown() == 1u ? "" : "s") << '\n';
        std::cout << "[input] controller samples=" << input.samples() << " guest_us=" << scheduler.now_us() << '\n';
        if (dual_analog && dual_analog->turn_frames()) std::cout << dual_analog->turn_report() << '\n';
        if (!options.deterministic) {
            // What the keyboard and the mouse delivered, and the cursor back to Windows.
            std::cout << chili::input_summary(chili::PcInput::instance()) << '\n';
            chili::PcInput::instance().release();
        }
        if(const char *capture=std::getenv("PSPRECOMP_FRAME_DUMP")){if(*capture){
            // ".png" writes a PNG, anything else the 32-bit BMP.
            const auto frame=display.snapshot();
            if((gpu && chili::save_gpu_display(shown_address,capture)) ||
               (!gpu && ((frame.configured && chili::save_display_frame(runtime.memory(),frame.address,frame.stride,frame.format,capture)) ||
               presentation.save_frame(capture))))std::cout << "[display] saved framebuffer " << capture << '\n';
            else std::cerr << "[display] framebuffer capture failed\n";
        }}
        std::cout << "Execution ended at " << psprecomp::hex32(runtime.cpu().pc)
                  << ": " << runtime.stop_reason() << '\n';
        // Opt-in bounded snapshots let bring-up inspect actual guest request data.
        if(const char *dump=std::getenv("PSPRECOMP_GUEST_DUMP")){if(*dump){
            const char *address_text=std::getenv("PSPRECOMP_GUEST_DUMP_ADDRESS");
            const char *size_text=std::getenv("PSPRECOMP_GUEST_DUMP_SIZE");
            const auto address=address_text?std::stoull(address_text,nullptr,0):0ull;
            const auto size=size_text?std::stoull(size_text,nullptr,0):0ull;
            if(address<=0xFFFFFFFFull && size>0 && size<=4u*1024u*1024u &&
               runtime.memory().contains(static_cast<std::uint32_t>(address),static_cast<std::size_t>(size))){
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                runtime.memory().copy_out(static_cast<std::uint32_t>(address),bytes);
                std::ofstream output(dump,std::ios::binary|std::ios::trunc);
                output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
                output.close();
                std::cout << "[guest-dump] address=" << psprecomp::hex32(static_cast<std::uint32_t>(address))
                          << " bytes=" << size << " saved=" << !output.fail() << '\n';
            }else std::cerr << "[guest-dump] invalid range or size limit exceeded\n";
        }}

        chili::dump_guest_capture_plan(runtime);

        if(const char *diagnostic=std::getenv("PSPRECOMP_STOP_CONTEXT")){if(*diagnostic){
            for(unsigned i=0;i<32;++i)std::cout << "[stop-context] r" << i << "=" << psprecomp::hex32(runtime.cpu().gpr[i]) << '\n';
        }}
        // Requested stops are clean exits; anything else that left the guest
        // mid-flight is a failure.
        const auto &reason = runtime.stop_reason();
        const bool requested = reason == probe_deadline_reason || reason == live_quit_reason ||
            reason == window_closed_reason || reason == chili::FrameProbe::frame_limit_reason;
        if (runtime.cpu().pc == 0u || requested) return 0;
        start.failure = "The game stopped at " + psprecomp::hex32(runtime.cpu().pc) + ": " + reason;
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "Chili bootstrap failure: " << error.what() << '\n';
        start.failure = error.what();
        return 1;
    }
}
}

int main() {
    // Real pixels on a scaled desktop: the swapchain is sized from the client
    // rectangle, and a virtualised one would be stretched a second time.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    chili::launcher::Start start;
    // Everything of the run (window, renderer, sound) is gone when run() returns; a player is then
    // told in a message box why it failed.
    const int code = run(start);
    return chili::launcher::finish(start, code);
}
