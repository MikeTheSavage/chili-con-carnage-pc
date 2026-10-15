#include "diagnostic_live_channel.hpp"
#include <fstream>
#include <vector>

namespace {
namespace lc = chili::live_control;
namespace fs = std::filesystem;
std::size_t checks{};
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& function, const char* message) {
    try { function(); } catch (const lc::Error&) { require(true, message); return; }
    throw std::runtime_error(message);
}
bool equal(chili::ControllerInput a, chili::ControllerInput b) { return a.buttons == b.buttons && a.x == b.x && a.y == b.y; }
struct Root {
    fs::path parent, path;
    explicit Root(fs::path source) {
        source = fs::weakly_canonical(fs::absolute(source));
        require(source == fs::weakly_canonical(fs::path(__FILE__).parent_path()), "fixture root must be its owned source workspace");
        parent = source / "fixture_roots"; fs::create_directory(parent);
        path = parent / ("case_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        require(fs::create_directory(path), "fixture exclusive root creation failed");
    }
    ~Root() {
        if (path.is_absolute() && path.parent_path() == parent && path.filename().string().starts_with("case_")) {
            std::error_code error; fs::remove_all(path, error);
        }
    }
};
void atomic_command(const fs::path& path, const std::string& text) {
    static unsigned sequence{};
    const auto staged = path.parent_path() / ("writer_" + std::to_string(++sequence) + ".tmp");
    std::ofstream file(staged, std::ios::binary | std::ios::trunc); file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush(); file.close(); require(!file.fail(), "fixture command staging failed");
    bool moved{}; DWORD error{};
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        moved = MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        error = moved ? ERROR_SUCCESS : GetLastError();
        if (moved || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)) break;
        // An external scanner can transiently deny replacement. Retain the
        // closed staging file and retry the same atomic operation, never append.
        Sleep(10);
    }
    require(moved, ("fixture atomic command replacement failed; Win32=" + std::to_string(error) + " staged=" + staged.filename().string()).c_str());
}
void parser_tests() {
    auto c = lc::parse("1 press 0x4000 300 128 128\n");
    require(c.sequence == 1 && c.kind == lc::Kind::Press && c.input.buttons == 0x4000 && c.duration_ms == 300 && c.input.x == 128 && c.input.y == 128, "Cross command fields changed");
    c = lc::parse("18446744073709551615 capture menu_1\r\n");
    require(c.sequence == UINT64_MAX && c.kind == lc::Kind::Capture && c.label == "menu_1", "capture/max sequence parsing failed");
    c = lc::parse("2 press 0 5000 0 255");
    require(c.input.buttons == 0 && c.input.x == 0 && c.input.y == 255 && c.duration_ms == 5000, "valid release/analog boundary rejected");
    require(lc::parse("3 press 62457 1 255 0").input.buttons == 0xF3F9, "decimal PSP buttons rejected");
    for (const auto& bad : std::vector<std::string>{"", "0 capture frame", "-1 capture frame", "+1 capture frame", "18446744073709551616 capture frame", "1 capture ../outside", "1 capture a.bmp", "1 capture a/b", "1 capture a:b", "1 capture " + std::string(49, 'a'), "1 capture two labels", "1 press 0x4000 0 128 128", "1 press 0x4000 5001 128 128", "1 press 0x4 100 128 128", "1 press 0x10000 100 128 128", "1 press -1 100 128 128", "1 press 0x 100 128 128", "1 press 1 100 256 128", "1 press 1 100 128 -1", "1 press 1 100 128 128 extra", "1 fake frame", std::string(257, 'A'), std::string("1 capture a\0b", 13), "1 capture \xC3\xA9"})
        rejects([&] { (void)lc::parse(bad); }, "malformed control command accepted");
}
void channel_tests(Root& root) {
    std::uint64_t now{};
    std::vector<std::string> logs;
    const lc::Config config{root.path / "command.txt", root.path / "frame"};
    lc::DiagnosticLiveChannel channel(config, [&] { return now; }, [&](const auto& s) { logs.push_back(s); });
    unsigned source_calls{}, capture_calls{};
    const chili::ControllerInput ordinary{8, 5, 9};
    auto source = [&] { ++source_calls; return ordinary; };
    auto capture = [&](const fs::path&) { ++capture_calls; return true; };
    channel.host_pump(capture);
    require(channel.enabled() && channel.last_sequence() == 0 && equal(channel.sample(source), ordinary) && capture_calls == 0, "missing control file changed ordinary source");
    atomic_command(config.control_file, "1 press 0x4000 300 0 255");
    now = 49; channel.host_pump(capture);
    require(channel.last_sequence() == 0, "control polling was not host-throttled");
    now = 50; channel.host_pump(capture);
    const auto before = source_calls;
    for (unsigned i = 0; i < 10; ++i) require(equal(channel.sample(source), {0x4000, 0, 255}), "shared pulse sample was consumed or mixed with source");
    require(source_calls == before && capture_calls == 0 && channel.last_sequence() == 1, "active pulse unexpectedly called source/capture");
    now = 349; require(channel.active_sample().has_value(), "pulse ended before exclusive deadline");
    now = 350; require(!channel.active_sample() && equal(channel.sample(source), ordinary), "pulse did not expire/delegate at deadline");
    channel.host_pump(capture);
    require(channel.last_sequence() == 1 && !channel.active_sample(), "unchanged latest command reapplied expired pulse");

    atomic_command(config.control_file, "2 press 0 2000 128 128");
    now = 400; channel.host_pump(capture);
    require(channel.active_sample() && channel.active_sample()->buttons == 0, "explicit release lost valid-zero provenance");
    atomic_command(config.control_file, "3 press 0x4000 400 128 128");
    now = 500; channel.host_pump(capture);
    require(channel.active_sample() && channel.active_sample()->buttons == 0x4000, "release-to-press replacement did not create held sample");
    atomic_command(config.control_file, "4 capture profile_menu");
    now = 550; require(channel.active_sample()->buttons == 0x4000 && capture_calls == 0 && channel.last_sequence() == 3, "provider read/consumed unpumped capture");
    channel.host_pump(capture);
    require(capture_calls == 1 && channel.last_capture()->saved && channel.last_capture()->attempted &&
            channel.last_capture()->path == root.path / "frame_4_profile_menu.bmp" && channel.active_sample()->buttons == 0x4000,
            "capture path or pulse independence changed");
    now = 600; channel.host_pump(capture);
    require(capture_calls == 1, "capture sequence executed twice");
    atomic_command(config.control_file, "3 press 0x8000 400 128 128");
    now = 650; channel.host_pump(capture);
    require(channel.last_sequence() == 4 && channel.active_sample()->buttons == 0x4000, "stale sequence overwrote active input");
    atomic_command(config.control_file, "5 press 0x4 500 128 128");
    now = 700; channel.host_pump(capture);
    require(channel.last_sequence() == 4 && channel.active_sample()->buttons == 0x4000, "malformed command consumed sequence or mutated pulse");
    now = 750; const auto log_count = logs.size(); channel.host_pump(capture);
    require(logs.size() == log_count, "unchanged malformed command spammed logs");
    atomic_command(config.control_file, "5 capture after_failure");
    now = 800; channel.host_pump([](const fs::path&) { return false; });
    require(channel.last_sequence() == 5 && !channel.last_capture()->saved && channel.last_capture()->attempted, "valid retry/failing capture outcome wrong");
    now = 900; require(!channel.active_sample() && equal(channel.sample(source), ordinary), "capture extended pulse lifetime");

    std::ofstream sentinel(root.path / "frame_6_exists.bmp"); sentinel << "owned sentinel"; sentinel.close();
    atomic_command(config.control_file, "6 capture exists");
    now = 950; channel.host_pump(capture);
    require(channel.last_sequence() == 6 && !channel.last_capture()->attempted && !channel.last_capture()->saved && capture_calls == 1,
            "existing capture destination was passed to save_frame");
    std::ifstream sentinel_read(root.path / "frame_6_exists.bmp"); std::string old; std::getline(sentinel_read, old);
    require(old == "owned sentinel", "existing capture was overwritten");
    atomic_command(config.control_file, "7 capture callback_throw");
    now = 1000; channel.host_pump([](const fs::path&) -> bool { throw std::runtime_error("owned fixture callback failure"); });
    require(channel.last_sequence() == 7 && !channel.last_capture()->saved && channel.last_capture()->attempted, "capture callback exception escaped or claimed success");
    atomic_command(config.control_file, std::string(257, 'A'));
    now = 1050; channel.host_pump(capture);
    require(channel.last_sequence() == 7 && capture_calls == 1, "oversized file changed accepted command sequence");
    atomic_command(config.control_file, "8 capture safe");
    now = 1100; channel.host_pump(capture);
    require(channel.last_sequence() == 8 && capture_calls == 2, "oversized malformed file prevented later valid command");

    for (unsigned i = 0; i < 40; ++i) {
        atomic_command(config.control_file, i % 2 ? "0 capture x" : "9 fake x");
        now += 50; channel.host_pump(capture);
    }
    require(channel.last_sequence() == 8 && logs.size() < 40, "malformed diagnostics were not bounded");
    atomic_command(config.control_file, "9 press 1 5 128 128");
    now = UINT64_MAX - 4; channel.host_pump(capture);
    require(channel.last_sequence() == 8 && !channel.active_sample(), "overflowing pulse clock was accepted");
}
void disabled_and_paths(Root& root) {
    unsigned sources{}, captures{};
    lc::DiagnosticLiveChannel disabled({{}, root.path / "nonexistent" / "prefix"}, [] { return 0u; }, [](const auto&) {});
    auto source = [&] { ++sources; return chili::ControllerInput{0x8000, 1, 2}; };
    disabled.host_pump([&](const fs::path&) { ++captures; return true; });
    require(!disabled.enabled() && !disabled.active_sample() && equal(disabled.sample(source), {0x8000, 1, 2}) && sources == 1 && captures == 0,
            "disabled channel changed ordinary source or touched invalid frame parent");
    rejects([&] { lc::DiagnosticLiveChannel bad({"relative.txt", root.path / "frame"}); }, "relative control path accepted");
    rejects([&] { lc::DiagnosticLiveChannel bad({root.path / "control.txt", root.path / "../frame"}); }, "frame traversal accepted");
    rejects([&] { lc::DiagnosticLiveChannel bad({root.path / "control.txt", root.path / "frame.bmp"}); }, "unsupported prefix basename accepted");
    rejects([&] { lc::DiagnosticLiveChannel bad({root.path / "control.txt", root.path / "missing" / "frame"}); }, "missing explicit capture parent accepted");
    fs::create_directory(root.path / "directory_command");
    rejects([&] { (void)lc::read_latest(root.path / "directory_command"); }, "directory input read as command");
}
void environment_tests(Root& root) {
    struct Restore {
        fs::path control = lc::environment_path(L"PSPRECOMP_LIVE_CONTROL");
        fs::path prefix = lc::environment_path(L"PSPRECOMP_LIVE_FRAME_PREFIX");
        ~Restore() {
            SetEnvironmentVariableW(L"PSPRECOMP_LIVE_CONTROL", control.empty() ? nullptr : control.c_str());
            SetEnvironmentVariableW(L"PSPRECOMP_LIVE_FRAME_PREFIX", prefix.empty() ? nullptr : prefix.c_str());
        }
    } restore;
    require(SetEnvironmentVariableW(L"PSPRECOMP_LIVE_CONTROL", nullptr) != FALSE, "fixture environment reset failed");
    require(SetEnvironmentVariableW(L"PSPRECOMP_LIVE_FRAME_PREFIX", L"invalid relative ignored") != FALSE, "fixture environment prefix set failed");
    require(!lc::from_environment().enabled(), "absent live control unexpectedly enabled invalid prefix");
    const auto control = root.path / "env_command.txt", prefix = root.path / "env_frame";
    require(SetEnvironmentVariableW(L"PSPRECOMP_LIVE_CONTROL", control.c_str()) != FALSE, "fixture control environment set failed");
    require(SetEnvironmentVariableW(L"PSPRECOMP_LIVE_FRAME_PREFIX", nullptr) != FALSE, "fixture prefix environment reset failed");
    rejects([] { (void)lc::from_environment(); }, "enabled environment accepted missing explicit capture prefix");
    require(SetEnvironmentVariableW(L"PSPRECOMP_LIVE_FRAME_PREFIX", prefix.c_str()) != FALSE, "fixture capture environment set failed");
    const auto config = lc::from_environment();
    require(config.control_file == control && config.frame_prefix == prefix, "configured environment paths changed");
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected owned fixture source root"); Root root(argv[1]);
        parser_tests(); channel_tests(root); disabled_and_paths(root); environment_tests(root);
        std::cout << "PASS: " << checks << " bounded parser/pulse/shared release/stale/malformed/atomic-file/capture/disabled checks; no guest or OS input, game or real-save writes\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
