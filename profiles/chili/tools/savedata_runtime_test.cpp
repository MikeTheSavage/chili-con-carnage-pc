#include "../host/savedata_runtime.hpp"
#include "../host/thread_runtime.hpp"
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/utime.h>
#include <vector>

namespace {
constexpr unsigned init_nid = 0x50C4CD57u, update_nid = 0xD4B95FFBu,
    status_nid = 0x8874DBE0u, shutdown_nid = 0x9790B33Cu;
constexpr unsigned capture_base = 0x08DF0000u, params = 0x08DF68C0u,
    id_list = 0x08DF712Cu, entries = 0x08DF7138u;
constexpr unsigned invalid_status = 0x80110001u, invalid_address = 0x80110002u,
    invalid_size = 0x80110004u, wrong_type = 0x80110005u;

void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
std::vector<std::uint8_t> read_capture(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot open actual savedata request RAM capture");
    std::vector<std::uint8_t> result{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    require(result.size() == 65536u, "Savedata RAM capture has wrong length");
    return result;
}
void pattern(psprecomp::AllegrexContext &c) {
    for (unsigned i = 1; i < c.gpr.size(); ++i) c.gpr[i] = 0xF0000000u + i;
    c.gpr[29] = 0x09FFF000u; c.gpr[28] = 0x08D9AB00u; c.gpr[31] = 0x08830178u;
    c.pc = 0x08D98D98u; c.hi = 0x12345678u; c.lo = 0x89ABCDEFu; c.fcr31 = 0x87654321u;
    for (unsigned i = 0; i < c.fpr.size(); ++i) c.fpr[i] = i * 1.25f;
    for (unsigned i = 0; i < c.vfpu.size(); ++i) c.vfpu[i] = -static_cast<float>(i);
    for (unsigned i = 0; i < c.vfpu_ctrl.size(); ++i) c.vfpu_ctrl[i] = 0xF0E0D000u + i;
}
bool same_cpu(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.pc == b.pc && a.hi == b.hi && a.lo == b.lo &&
        a.fpr == b.fpr && a.fcr31 == b.fcr31 && a.vfpu == b.vfpu && a.vfpu_ctrl == b.vfpu_ctrl;
}
struct Fixture {
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads{rt};
    chili::UtilityDialogOwner owner;
    std::unique_ptr<chili::SavedataRuntime> savedata;
    Fixture(const std::vector<std::uint8_t> &capture, const std::filesystem::path &root) {
        threads.initialize_bootstrap(); pattern(rt.cpu());
        rt.memory().copy_in(capture_base, capture);
        savedata = std::make_unique<chili::SavedataRuntime>(rt, root, &threads, &owner);
    }
    unsigned call(unsigned nid, unsigned a0 = 0u, bool allow_stop = false) {
        auto &c = rt.cpu(); c.gpr[4] = a0;
        auto expected = c;
        const auto uid = psprecomp::runtime_thread_uid();
        const auto generation = psprecomp::runtime_thread_switch_generation();
        const auto now = threads.now_us();
        rt.invoke_import("sceUtility", nid, c);
        expected.gpr[2] = c.gpr[2];
        require(allow_stop || !rt.stopped(), "Savedata call stopped unexpectedly");
        require(same_cpu(c, expected), "Savedata call changed caller CPU outside V0");
        require(psprecomp::runtime_thread_uid() == uid &&
            psprecomp::runtime_thread_switch_generation() == generation && threads.now_us() == now,
            "Savedata call advanced time or changed guest scheduling ownership");
        return c.gpr[2];
    }
    void delay(unsigned us) {
        const auto now = threads.now_us(); const auto uid = threads.current_uid();
        rt.cpu().gpr[4] = us; rt.cpu().gpr[31] = 0x08830178u;
        rt.invoke_import("ThreadManForUser", 0xCEADEB47u, rt.cpu());
        require(!rt.stopped() && threads.now_us() == now + us && threads.current_uid() == uid,
            "Actual DelayThread did not preserve guest UID and advance scheduler time");
    }
    std::vector<std::uint8_t> snapshot() const {
        std::vector<std::uint8_t> bytes(65536u); rt.memory().copy_out(capture_base, bytes); return bytes;
    }
    void start() {
        require(call(init_nid, params) == 0u && call(status_nid) == 1u, "Captured LIST Init/status mismatch");
        delay(static_cast<unsigned>(chili::SavedataRuntime::init_delay_us));
        require(call(status_nid) == 2u, "LIST did not become Running after scheduler delay");
    }
    void finish() {
        require(call(update_nid, 1u) == 0u && call(status_nid) == 3u, "LIST Update/status mismatch");
    }
    void shutdown() {
        require(call(shutdown_nid) == 0u && call(status_nid) == 4u, "LIST shutdown/status mismatch");
        delay(static_cast<unsigned>(chili::SavedataRuntime::shutdown_delay_us));
        require(call(status_nid) == 0u && !owner.busy(), "LIST did not release utility ownership after teardown");
    }
};
void put_string(psprecomp::GuestMemory &memory, unsigned address, unsigned limit, const std::string &value) {
    require(value.size() <= limit, "Test guest string is too long");
    memory.zero(address, limit);
    for (unsigned i = 0; i < value.size(); ++i) memory.store8(address + i, static_cast<std::uint8_t>(value[i]));
}
void unchanged_except(const std::vector<std::uint8_t> &before, const std::vector<std::uint8_t> &after,
    unsigned written_count) {
    for (unsigned i = 0; i < before.size(); ++i) {
        const auto a = capture_base + i;
        const bool output = (a >= params + 28u && a < params + 32u) ||
            (a >= id_list + 4u && a < id_list + 8u) ||
            (a >= entries && a < entries + 72u * written_count);
        require(output || before[i] == after[i], "LIST changed unrelated captured guest memory or secure key");
    }
}
void date_equals(psprecomp::GuestMemory &memory, unsigned address, const std::filesystem::path &path) {
    struct _stat64 st{}; require(_wstat64(path.c_str(), &st) == 0, "Cannot independently stat fixture timestamp source");
    const __time64_t times[] = {st.st_ctime, st.st_atime, st.st_mtime};
    for (unsigned i = 0; i < 3u; ++i) {
        std::tm d{}; require(_localtime64_s(&d, &times[i]) == 0, "Cannot convert fixture host timestamp");
        const unsigned values[] = {static_cast<unsigned>(d.tm_year + 1900), static_cast<unsigned>(d.tm_mon + 1),
            static_cast<unsigned>(d.tm_mday), static_cast<unsigned>(d.tm_hour),
            static_cast<unsigned>(d.tm_min), static_cast<unsigned>(d.tm_sec)};
        for (unsigned j = 0; j < 6u; ++j)
            require(memory.load16(address + 4u + 16u * i + 2u * j) == values[j], "PSP date fields/offsets differ from real host stat");
        require(memory.load32(address + 16u + 16u * i) == 0u, "Second-resolution native stat invented microseconds");
    }
}
void captured_empty(const std::vector<std::uint8_t> &capture, const std::filesystem::path &root) {
    std::filesystem::create_directories(root);
    Fixture f(capture, root); auto &m = f.rt.memory();
    require(m.load32(params) == 1536u && m.load32(params + 48u) == 11u &&
        m.load32(params + 1524u) == id_list && m.load32(id_list) == 20u && m.load32(id_list + 8u) == entries,
        "Actual captured guest ABI changed");
    require(m.read_c_string(params + 60u, 13u) == "ULUS10216" && m.read_c_string(params + 76u, 20u) == "DATA*",
        "Actual captured game/save-name request changed");
    const auto before = f.snapshot();
    require(f.call(status_nid) == wrong_type && f.call(update_nid) == wrong_type && f.call(shutdown_nid) == wrong_type,
        "Unselected savedata utility did not report hardware wrong-type error");
    require(f.call(init_nid, params) == 0u && f.call(status_nid) == 1u && f.owner.busy(), "Actual Init failed");
    require(f.call(init_nid, 0xDEADBEEFu) == invalid_status && f.call(update_nid) == invalid_status &&
        f.call(shutdown_nid) == invalid_status, "Invalid lifecycle ordering did not fail before pointer access");
    for (unsigned i = 0; i < 100u; ++i) require(f.call(status_nid) == 1u, "Status polling fabricated initialization progress");
    require(f.snapshot() == before, "Init or polling wrote captured guest memory");
    f.delay(199999u); require(f.call(status_nid) == 1u, "Init deadline crossed too early");
    f.delay(1u); require(f.call(status_nid) == 2u, "Init did not reach Running at deadline");
    f.delay(2000000u);
    require(f.call(status_nid) == 2u && f.snapshot() == before, "GetStatus finished LIST without Update");
    f.finish(); require(m.load32(id_list + 4u) == 0u && m.load32(params + 28u) == 0u,
        "Missing SAVEDATA folder did not produce genuine successful empty LIST");
    unchanged_except(before, f.snapshot(), 0u);
    require(!std::filesystem::exists(root / "PSP"), "LIST created or saved host files");
    require(f.call(update_nid) == invalid_status, "Finished LIST accepted another Update");
    require(f.call(shutdown_nid) == 0u && f.call(status_nid) == 4u, "Shutdown did not begin");
    f.delay(1999u); require(f.call(status_nid) == 4u, "Shutdown deadline crossed too early");
    f.delay(1u); require(f.call(status_nid) == 0u && !f.owner.busy(), "Shutdown did not release shared utility slot");
    require(f.call(update_nid) == invalid_status && f.call(shutdown_nid) == invalid_status,
        "Inactive last savedata type did not report invalid status");
    f.start(); f.finish(); f.shutdown();
    require(f.owner.try_acquire(chili::UtilityDialogType::Message), "Message could not acquire idle savedata slot");
    require(f.call(status_nid) == wrong_type && f.call(update_nid) == wrong_type && f.call(shutdown_nid) == wrong_type &&
        f.call(init_nid, params) == invalid_status, "Savedata ignored another active utility type");
    require(f.call(init_nid, 0xDEADBEEFu) == invalid_status && f.owner.busy() &&
        f.owner.is_current_type(chili::UtilityDialogType::Message), "Utility busy did not win over an invalid Init pointer");
    require(f.owner.release(chili::UtilityDialogType::Message), "Message fixture release failed");
    require(f.call(status_nid) == wrong_type, "Last utility type was lost at teardown");
    f.start(); f.finish(); require(f.call(shutdown_nid) == 0u, "Pump shutdown setup failed");
    f.delay(2000u);
    const auto pump_cpu = f.rt.cpu(); const auto pump_uid = f.threads.current_uid();
    const auto pump_now = f.threads.now_us(); const auto pump_memory = f.snapshot();
    f.savedata->tick();
    require(!f.owner.busy() && same_cpu(pump_cpu, f.rt.cpu()) && f.threads.current_uid() == pump_uid &&
        f.threads.now_us() == pump_now && f.snapshot() == pump_memory,
        "Post-dispatch pump changed CPU/UID/time/output or failed to release completed shutdown");
    require(f.owner.try_acquire(chili::UtilityDialogType::Message), "Pump release required guest status polling");
    require(f.call(status_nid) == wrong_type, "Savedata did not observe a utility acquired after pump teardown");
    require(f.owner.release(chili::UtilityDialogType::Message), "Pump message fixture release failed");
    f.start(); require(!f.owner.try_acquire(chili::UtilityDialogType::Message), "Concurrent utilities silently overlapped");
    f.savedata.reset(); require(!f.owner.busy(), "Destroyed savedata helper retained an active shared slot");
    std::cout << "Captured LIST: empty filesystem, 1536/12/72-byte ABI, lifecycle, shared ownership, full CPU/UID PASS\n";
}
void create_save_fixtures(const std::filesystem::path &root) {
    const auto path = root / "PSP" / "SAVEDATA";
    for (const auto name : {"ULUS10216DATA", "ULUS10216DATA0", "ULUS10216DATAAC", "ulus10216dataB",
        "ULUS10216OTHER", "ULES77777DATA0"}) std::filesystem::create_directories(path / name);
    std::ofstream(path / "ULUS10216DATA-file", std::ios::binary) << "not a save directory";
    const auto sfo = path / "ULUS10216DATA0" / "PARAM.SFO";
    std::ofstream(sfo, std::ios::binary) << "Owned native fixture; LIST does not parse SFO contents";
    __utimbuf64 times{1590000000ll, 1600000000ll};
    require(_wutime64(sfo.c_str(), &times) == 0, "Cannot establish real PARAM.SFO timestamp fixture");
}
void enumeration(const std::vector<std::uint8_t> &capture, const std::filesystem::path &root) {
    create_save_fixtures(root);
    Fixture f(capture, root); const auto before = f.snapshot(); f.start(); f.finish();
    auto &m = f.rt.memory();
    require(m.load32(id_list + 4u) == 4u && m.load32(id_list) == 20u && m.load32(id_list + 8u) == entries,
        "LIST count/maxCount/entries pointer ABI mismatch");
    const std::array<const char *, 4> names{"DATA", "DATA0", "DATAAC", "dataB"};
    const auto save_root = root / "PSP" / "SAVEDATA";
    for (unsigned i = 0; i < names.size(); ++i) {
        const auto a = entries + 72u * i;
        require(m.load32(a) == 0x11FFu && m.read_c_string(a + 52u, 20u) == names[i], "LIST entry mode/name/stride mismatch");
        const auto path = i == 1u ? save_root / "ULUS10216DATA0" / "PARAM.SFO" :
            save_root / (i == 3u ? "ulus10216dataB" : std::string("ULUS10216") + names[i]);
        date_equals(m, a, path);
    }
    require(m.load16(entries + 72u + 36u) == 2020u, "LIST did not prefer real PARAM.SFO modification timestamp");
    unchanged_except(before, f.snapshot(), 4u); f.shutdown();

    for (const auto &[pattern_text, expected] : std::array<std::pair<const char *, unsigned>, 5>{
        std::pair{"DATA?", 2u}, {"data??", 1u}, {"*A?", 3u}, {"NO_MATCH*", 0u}, {"DATA", 1u}}) {
        put_string(m, params + 76u, 20u, pattern_text);
        f.start(); f.finish(); require(m.load32(id_list + 4u) == expected, "Wildcard/FAT case matching mismatch"); f.shutdown();
    }
    put_string(m, params + 76u, 20u, "DATA*"); m.store32(id_list, 2u);
    const auto limited = f.snapshot(); f.start(); f.finish();
    require(m.load32(id_list + 4u) == 2u && m.read_c_string(entries + 52u, 20u) == "DATA" &&
        m.read_c_string(entries + 72u + 52u, 20u) == "DATA0", "maxCount did not bound returned entries/count");
    unchanged_except(limited, f.snapshot(), 2u); f.shutdown();
    m.store32(id_list, 0u); m.store32(id_list + 8u, 0u); f.start(); f.finish();
    require(m.load32(id_list + 4u) == 0u && m.load32(id_list + 8u) == 0u, "Zero-count NULL entry buffer was not accepted"); f.shutdown();
    std::cout << "Filesystem LIST: native folders, case/wildcards, PARAM.SFO/directory times, maxCount, untouched tail PASS\n";
}
void pointer_errors(const std::vector<std::uint8_t> &capture, const std::filesystem::path &root) {
    Fixture f(capture, root); auto &m = f.rt.memory(); const auto before = f.snapshot();
    for (const auto address : {0u, 0xDEADBEEFu, params + 1u, params | 0x80000000u, 0x09FFFFFCu}) {
        if (address == 0x09FFFFFCu) m.store32(address, 1536u);
        require(f.call(init_nid, address) == invalid_address && !f.owner.busy(), "Invalid params pointer acquired utility ownership");
        require(f.snapshot() == before, "Invalid params pointer partially wrote request");
    }
    m.store32(params, 1535u); auto bad = f.snapshot();
    require(f.call(init_nid, params) == invalid_size && f.snapshot() == bad && !f.owner.busy(), "Unknown param size accepted");
    for (const auto old_size : {0x5C8u, 0x5DCu}) {
        m.store32(params, old_size); bad = f.snapshot();
        require(f.call(init_nid, params) == invalid_size && f.snapshot() == bad, "Old params size accepted out-of-struct LIST field");
    }
    m.store32(params, 1536u);
    for (const auto address : {0u, 0x09FFFFFCu, id_list + 1u, params, id_list | 0x80000000u}) {
        m.store32(params + 1524u, address); bad = f.snapshot();
        require(f.call(init_nid, params) == invalid_address && f.snapshot() == bad && !f.owner.busy(), "Invalid/overlapping idList accepted");
    }
    m.store32(params + 1524u, id_list);
    for (const auto address : {0u, 0x09FFFFFCu, entries + 1u, entries | 0x80000000u, params, id_list}) {
        m.store32(id_list + 8u, address); bad = f.snapshot();
        require(f.call(init_nid, params) == invalid_address && f.snapshot() == bad && !f.owner.busy(), "Invalid/overlapping output buffer accepted");
    }
    m.store32(id_list + 8u, entries);
    for (const auto count : {0xFFFFFFFFu, 0x7FFFFFFFu, 0x04000000u}) {
        m.store32(id_list, count); bad = f.snapshot();
        require(f.call(init_nid, params) == invalid_address && f.snapshot() == bad && !f.owner.busy(), "Overflowing/negative entry count accepted");
    }
    m.store32(id_list, 20u); f.start(); m.store32(id_list + 8u, 0x09FFFFFCu); bad = f.snapshot();
    require(f.call(update_nid) == invalid_address && f.snapshot() == bad && f.call(status_nid) == 2u,
        "Changed output pointer caused partial results or finished an invalid query");
    m.store32(id_list + 8u, entries); f.finish(); f.shutdown();
    require(f.call(init_nid, params | 0x40000000u) == 0u, "Valid uncached PSP user alias rejected");
    f.delay(200000u); f.finish(); f.shutdown();
    std::cout << "Pointer/errors: guest span/alias/overlap/count validation, no partial writes, retry PASS\n";
}
void strict_failures(const std::vector<std::uint8_t> &capture, const std::filesystem::path &root) {
    // Without a backend the modes that need one still stop: that is a program
    // put together wrongly, not something a player can reach.
    for (const auto mode : {0u, 1u, 4u, 5u, 7u}) {
        Fixture f(capture, root); f.rt.memory().store32(params + 48u, mode); const auto before = f.snapshot();
        f.call(init_nid, params, true);
        require(f.rt.stopped() && f.rt.stop_reason().find("requires a complete native save/load backend") != std::string::npos &&
            f.snapshot() == before && !f.owner.busy(), "Backend-less mode faked success/cancel, wrote output or held utility ownership");
    }
    // This expectation changed on purpose. A mode the port does not implement
    // used to stop the runtime ("Native savedata mode N is not implemented"),
    // and the game's "Delete save games" menu (mode 7) ended the whole game that
    // way. A menu choice must never end the game: such a request now runs
    // through the utility's life cycle and ends with the result a PSP gives for
    // a cancelled dialog (common.result 1), and nothing else is written.
    for (const auto mode : {2u, 3u, 6u, 22u, 15u, 999u}) {
        Fixture f(capture, root); f.rt.memory().store32(params + 48u, mode); const auto before = f.snapshot();
        require(f.call(init_nid, params) == 0u && !f.rt.stopped() && f.owner.busy() && f.call(status_nid) == 1u,
            "A mode the port does not implement stopped the runtime or was refused at Init");
        require(f.snapshot() == before, "A declined mode wrote guest memory at Init");
        f.delay(200000u); require(f.call(status_nid) == 2u, "A declined mode did not reach Running");
        require(f.call(update_nid, 1u) == 0u && f.call(status_nid) == 3u && !f.rt.stopped(), "A declined mode did not finish");
        auto expected = before; const auto at = params + 28u - capture_base;
        expected[at] = 1u; expected[at + 1u] = expected[at + 2u] = expected[at + 3u] = 0u;
        require(f.snapshot() == expected, "A declined mode wrote more than the cancelled result");
        f.shutdown(); require(!f.owner.busy() && !f.rt.stopped(), "A declined mode kept the utility after Shutdown");
    }
    {
        Fixture invalid_name(capture, root); put_string(invalid_name.rt.memory(), params + 76u, 20u, "../*");
        const auto before_name = invalid_name.snapshot(); invalid_name.call(init_nid, params, true);
        require(invalid_name.rt.stopped() && invalid_name.snapshot() == before_name && !invalid_name.owner.busy(),
            "Unsafe/unsupported save-name pattern silently succeeded");
    }
    const auto malformed_root = root / "malformed";
    std::filesystem::create_directories(malformed_root); std::ofstream(malformed_root / "PSP") << "not a directory";
    Fixture f(capture, malformed_root); const auto before = f.snapshot(); f.start(); f.call(update_nid, 1u, true);
    require(f.rt.stopped() && f.rt.stop_reason().find("filesystem query failed") != std::string::npos &&
        f.snapshot() == before, "Filesystem failure became empty success or partially wrote guest output");
    f.savedata.reset(); require(!f.owner.busy(), "Failure teardown retained utility slot");
    std::cout << "Strict failures: missing native backend, unsafe names and native filesystem failure stop; modes the port does not implement (2, 3, 6, 15, 22, 999) end as cancelled PASS\n";
}
struct Scratch {
    std::filesystem::path parent, path;
    explicit Scratch(std::filesystem::path root) : parent(std::filesystem::absolute(root).lexically_normal()) {
        std::filesystem::create_directories(parent);
        path = parent / ("savedata-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(path.parent_path() == parent && !std::filesystem::exists(path), "Unsafe or reused savedata scratch path");
        require(std::filesystem::create_directory(path), "Could not create isolated savedata scratch directory");
    }
    ~Scratch() {
        if (path.parent_path() == parent && path.filename().string().starts_with("savedata-fixture-")) {
            std::error_code ec; std::filesystem::remove_all(path, ec);
        }
    }
};
}

int main(int argc, char **argv) {
    try {
        const auto capture_path = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path().parent_path() / "analysis" / "savedata_request_ram.bin";
        const auto capture = read_capture(capture_path);
        Scratch scratch(argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::temp_directory_path() / "chili-native-tests");
        captured_empty(capture, scratch.path / "empty-ms0");
        enumeration(capture, scratch.path / "populated-ms0");
        pointer_errors(capture, scratch.path / "populated-ms0");
        strict_failures(capture, scratch.path / "failure-ms0");
        std::cout << "PASS: native Chili Savedata LIST runtime fixture\n"; return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
