#include "message_dialog_runtime.hpp"
#include "message_dialog_win32.hpp"
#include "thread_runtime.hpp"
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Service = chili::MessageDialogRuntime;
constexpr unsigned params = 0x08900000u, span_base = params - 16u, span_size = 752u;
constexpr unsigned invalid_status = 0x80110001u, invalid_address = 0x80110002u,
    invalid_size = 0x80110004u, wrong_type = 0x80110005u;
void require(bool yes, const char *message) { if (!yes) throw std::runtime_error(message); }
bool same_cpu(const psprecomp::AllegrexContext &a, const psprecomp::AllegrexContext &b) {
    return a.gpr == b.gpr && a.pc == b.pc && a.hi == b.hi && a.lo == b.lo &&
        a.fpr == b.fpr && a.fcr31 == b.fcr31 && a.vfpu == b.vfpu && a.vfpu_ctrl == b.vfpu_ctrl;
}
void pattern(psprecomp::AllegrexContext &c) {
    for (unsigned i = 1; i < c.gpr.size(); ++i) c.gpr[i] = 0xF0000000u + i;
    c.gpr[29] = 0x09FFF000u; c.gpr[28] = 0x08D9AB00u; c.gpr[31] = 0x0883172Cu;
    c.pc = 0x08D98DC8u; c.hi = 0x12345678u; c.lo = 0x89ABCDEFu; c.fcr31 = 0x87654321u;
    for (unsigned i = 0; i < c.fpr.size(); ++i) c.fpr[i] = i * 1.25f;
    for (unsigned i = 0; i < c.vfpu.size(); ++i) c.vfpu[i] = -static_cast<float>(i);
    for (unsigned i = 0; i < c.vfpu_ctrl.size(); ++i) c.vfpu_ctrl[i] = 0xF0E0D000u + i;
}
struct Presenter final : chili::MessageDialogPresenter {
    chili::MessageDialogRequest request;
    std::optional<chili::MessageDialogSelection> selection;
    unsigned opens{}, polls{}, closes{};
    bool fail_open{}, fail_poll{}, live{};
    void open(const chili::MessageDialogRequest &r) override {
        ++opens; request = r; live = true;
        if (fail_open) throw std::runtime_error("fixture open failure");
    }
    std::optional<chili::MessageDialogSelection> poll(std::uint64_t generation) override {
        ++polls; require(generation == request.generation, "Presenter poll token differs from open token");
        if (fail_poll) throw std::runtime_error("fixture poll failure");
        return std::exchange(selection, {});
    }
    void close(std::uint64_t generation) noexcept override {
        if (generation == request.generation) { ++closes; live = false; }
    }
    void choose(chili::MessageDialogChoice choice) { selection = chili::MessageDialogSelection{request.generation, choice}; }
};
struct Fixture {
    psprecomp::Runtime rt;
    chili::ThreadRuntime threads{rt};
    chili::UtilityDialogOwner owner;
    Presenter presenter;
    std::unique_ptr<Service> service;
    Fixture(bool with_presenter = true) {
        threads.initialize_bootstrap(); pattern(rt.cpu());
        service = std::make_unique<Service>(rt, with_presenter ? &presenter : nullptr,
            [this] { return threads.now_us(); }, &owner);
    }
    unsigned call(unsigned nid, unsigned a0 = 0u, bool allow_stop = false) {
        auto &c = rt.cpu(); c.gpr[4] = a0; auto expected = c;
        const auto now = threads.now_us(), generation = psprecomp::runtime_thread_switch_generation();
        const auto uid = threads.current_uid();
        rt.invoke_import("sceUtility", nid, c);
        expected.gpr[2] = c.gpr[2];
        require(allow_stop || !rt.stopped(), "Unexpected message service stop");
        require(same_cpu(expected, c), "MsgDialog mutated caller context outside V0");
        require(threads.now_us() == now && threads.current_uid() == uid &&
            psprecomp::runtime_thread_switch_generation() == generation, "MsgDialog changed time or scheduler ownership");
        return c.gpr[2];
    }
    void delay(unsigned us) {
        const auto now = threads.now_us(); const auto uid = threads.current_uid();
        rt.cpu().gpr[4] = us;
        rt.invoke_import("ThreadManForUser", 0xCEADEB47u, rt.cpu());
        require(!rt.stopped() && threads.now_us() == now + us && threads.current_uid() == uid,
            "Actual DelayThread failed to advance scheduler deadline");
    }
    std::vector<std::uint8_t> snapshot() const {
        std::vector<std::uint8_t> bytes(span_size); rt.memory().copy_out(span_base, bytes); return bytes;
    }
    void request(unsigned size = 580u, unsigned mode = 0u, unsigned options = 0u, unsigned code = 0x80010009u) {
        std::array<std::uint8_t, span_size> bytes; bytes.fill(0xA5u); rt.memory().copy_in(span_base, bytes);
        auto &m = rt.memory();
        m.store32(params, size); m.store32(params + 4u, 1u); m.store32(params + 8u, 1u);
        // Exact priorities seen in both original Chili V2 error constructors.
        m.store32(params + 12u, 17u); m.store32(params + 16u, 19u);
        m.store32(params + 20u, 18u); m.store32(params + 24u, 16u);
        m.store32(params + 28u, 0xCAFEBABEu); m.store32(params + 48u, 0x1337u);
        m.store32(params + 52u, mode); m.store32(params + 56u, code);
        m.zero(params + 60u, 512u);
        const std::string body = "A real player must answer.\nNo default response.";
        for (unsigned i = 0; i < body.size(); ++i) m.store8(params + 60u + i, static_cast<std::uint8_t>(body[i]));
        if (size >= 580u) { m.store32(params + 572u, options); m.store32(params + 576u, 0x33445566u); }
        if (size == 708u) { m.zero(params + 580u, 128u); }
    }
    void start() {
        const auto before = snapshot();
        const auto opens_before = presenter.opens;
        require(call(Service::init_nid, params) == 0u && call(Service::status_nid) == 1u && owner.busy(), "INIT contract mismatch");
        require(before == snapshot() && presenter.opens == opens_before, "INIT mutated output or showed a window");
        for (unsigned i = 0; i < 100u; ++i) require(call(Service::status_nid) == 1u, "Status polling invented progress");
        require(call(Service::update_nid, 1u) == invalid_status && call(Service::shutdown_nid) == invalid_status,
            "INIT accepted Update or Shutdown");
        delay(static_cast<unsigned>(Service::init_delay_us - 1u)); service->tick();
        require(call(Service::status_nid) == 1u, "Initialization crossed deadline early");
        delay(1u); service->tick();
        require(call(Service::status_nid) == 2u && before == snapshot(), "tick must only enter Running, without writes");
    }
    void shutdown() {
        require(call(Service::shutdown_nid) == 0u && call(Service::status_nid) == 4u && owner.busy(), "Shutdown status4/busy mismatch");
        require(call(Service::init_nid, 0xDEADBEEFu) == invalid_status, "Busy Shutdown checked pointer before status");
        delay(static_cast<unsigned>(Service::shutdown_delay_us - 1u)); service->tick();
        require(owner.busy(), "tick released ownership before shutdown deadline");
        delay(1u);
        const auto memory = snapshot();
        const auto cpu = rt.cpu();
        const auto now = threads.now_us(), generation = psprecomp::runtime_thread_switch_generation();
        const auto uid = threads.current_uid(); const auto polls = presenter.polls;
        // Important: no MsgDialog GetStatus / Update call before another type acquires.
        service->tick();
        require(!owner.busy() && snapshot() == memory && same_cpu(cpu, rt.cpu()) &&
            now == threads.now_us() && uid == threads.current_uid() &&
            generation == psprecomp::runtime_thread_switch_generation() && polls == presenter.polls,
            "tick wrote guest state, polled input, advanced scheduler or failed owner release");
        require(call(Service::status_nid) == 0u && call(Service::update_nid, 1u) == invalid_status &&
            call(Service::shutdown_nid) == invalid_status, "Inactive last Message type error contract mismatch");
    }
};
void changed_only(const std::vector<std::uint8_t> &before, const std::vector<std::uint8_t> &after, bool button) {
    for (unsigned i = 0; i < before.size(); ++i) {
        const unsigned a = span_base + i;
        const bool output = (a >= params + 48u && a < params + 52u) ||
            (button && a >= params + 576u && a < params + 580u);
        require(output || before[i] == after[i], "Message wrote common.result/input/guard bytes outside documented outputs");
    }
}
void ownership_and_sizes() {
    Fixture f; f.request();
    require(f.call(Service::status_nid) == wrong_type && f.call(Service::update_nid, 1u) == wrong_type &&
        f.call(Service::shutdown_nid) == wrong_type, "Boot calls did not report WRONG_TYPE");
    const auto original = f.snapshot();
    for (unsigned size = 0; size <= 0x800u; ++size) {
        if (size == 572u || size == 580u || size == 708u) continue;
        f.rt.memory().store32(params, size);
        require(f.call(Service::init_nid, params) == invalid_size && !f.owner.busy() &&
            f.call(Service::status_nid) == wrong_type, "Recorded hardware ABI size set/error/type retention differs");
    }
    f.request();
    require(f.call(Service::init_nid, 0u) == invalid_address && f.call(Service::init_nid, params + 1u) == invalid_address,
        "Invalid or unaligned address accepted");
    const unsigned truncated = 0x0A000000u - 48u;
    f.rt.memory().store32(truncated, 580u);
    require(f.call(Service::init_nid, truncated) == invalid_address, "Full request bounds were not checked");
    require(f.owner.try_acquire(chili::UtilityDialogType::Savedata), "Owner acquisition fixture failed");
    require(f.call(Service::init_nid, 0xDEADBEEFu) == invalid_status &&
        f.call(Service::status_nid) == wrong_type && f.call(Service::update_nid, 1u) == wrong_type &&
        f.call(Service::shutdown_nid) == wrong_type, "MsgDialog ignored current Savedata or busy-before-address precedence");
    require(f.owner.release(chili::UtilityDialogType::Savedata), "Owner fixture release failed");
    f.rt.memory().store32(params, 999u);
    require(f.call(Service::init_nid, params) == invalid_size &&
        f.owner.is_current_type(chili::UtilityDialogType::Savedata), "Failed initialization selected Message type");
    require(f.presenter.opens == 0u && f.presenter.polls == 0u, "Invalid input created UI");
    std::cout << "Hardware ABI: all sizes0..2048, mapped bounds, busy/wrong-type precedence PASS\n";
}
void actual_chili_pending() {
    Fixture f; f.request(); const auto before = f.snapshot(); f.start();
    require(f.call(Service::update_nid, 2u) == 0u && f.call(Service::status_nid) == 2u, "Initial Update(2) failed");
    require(f.presenter.opens == 1u && f.presenter.request.mode == 0u && f.presenter.request.size == 580u &&
        f.presenter.request.text.find("80010009") != std::string::npos && !f.presenter.request.yes_no &&
        !f.presenter.request.okay && f.presenter.request.allow_back, "Original Chili V2 error presentation differs");
    require(f.rt.memory().load32(params + 48u) == 0u && f.rt.memory().load32(params + 576u) == 0x33445566u,
        "Pending Update output differs or fabricated button");
    const auto pending = f.snapshot();
    for (unsigned i = 0; i < 128u; ++i) {
        f.delay(16667u); f.service->tick();
        require(f.call(Service::update_nid, (i & 1u) ? 2u : 1u) == 0u &&
            f.call(Service::status_nid) == 2u && f.owner.busy() && f.snapshot() == pending,
            "Time, tick or repeated Update fabricated a dialog response");
    }
    require(f.presenter.opens == 1u && f.presenter.closes == 0u, "Pending dialog reopened or closed itself");
    changed_only(before, f.snapshot(), false);
    require(f.call(Service::shutdown_nid) == invalid_status, "Pending user dialog accepted teardown");
    f.presenter.choose(chili::MessageDialogChoice::Back);
    require(f.call(Service::update_nid, 2u) == 0u && f.call(Service::status_nid) == 3u && f.owner.busy(), "Explicit Back did not finish on Update(2)");
    require(f.rt.memory().load32(params + 576u) == 0u && f.presenter.closes == 1u && !f.presenter.live,
        "V2 error cancel-only Back must report button0");
    changed_only(before, f.snapshot(), true); f.shutdown();
    require(f.owner.try_acquire(chili::UtilityDialogType::Savedata) && f.call(Service::status_nid) == wrong_type,
        "Another utility could not acquire after deadline/tick alone");
    require(f.owner.release(chili::UtilityDialogType::Savedata), "Owner release fixture failed");
    std::cout << "Original Chili V2 error: Update(1)/Update(2), 128 scheduler frames pending, explicit fixture Back, timed teardown PASS\n";
}
void choices_and_versions() {
    struct Case { unsigned size, mode, options; chili::MessageDialogChoice choice; unsigned button; };
    const Case cases[] = {
        {572u, 1u, 0u, chili::MessageDialogChoice::Back, 0u},
        {580u, 1u, 1u, chili::MessageDialogChoice::Back, 0u},
        {580u, 1u, 0x11u, chili::MessageDialogChoice::Yes, 1u},
        {580u, 1u, 0x111u, chili::MessageDialogChoice::No, 2u},
        {580u, 1u, 0x11u, chili::MessageDialogChoice::Back, 3u},
        {708u, 1u, 0u, chili::MessageDialogChoice::Back, 3u},
        {708u, 0u, 0x22u, chili::MessageDialogChoice::Okay, 1u},
        {708u, 1u, 0xA1u, chili::MessageDialogChoice::Okay, 1u},
        {708u, 1u, 0x191u, chili::MessageDialogChoice::No, 2u},
    };
    for (const auto &c : cases) {
        Fixture f; f.request(c.size, c.mode, c.options);
        if (c.size == 708u) {
            const std::string okay = "Accept", back = "Leave";
            for (unsigned i = 0; i < okay.size(); ++i) f.rt.memory().store8(params + 580u + i, okay[i]);
            for (unsigned i = 0; i < back.size(); ++i) f.rt.memory().store8(params + 644u + i, back[i]);
        }
        const auto before = f.snapshot(); f.start(); f.call(Service::update_nid, 1u);
        require(f.presenter.request.default_no == ((c.options & 0x100u) != 0u), "Default No did not select metadata");
        if (c.size == 708u) require(f.presenter.request.okay_label == "Accept" && f.presenter.request.back_label == "Leave",
            "V3 custom captions decoded at wrong offsets");
        f.presenter.choose(c.choice);
        require(f.call(Service::update_nid, 1u) == 0u && f.call(Service::status_nid) == 3u,
            "Explicit action failed to finish supported request");
        require(c.size < 580u || f.rt.memory().load32(params + 576u) == c.button, "Button ABI value differs");
        require(f.rt.memory().load32(params + 28u) == 0xCAFEBABEu && f.rt.memory().load32(params + 48u) == 0u,
            "common.result confused with message result");
        changed_only(before, f.snapshot(), c.size >= 580u); f.shutdown();
    }
    // Nonterminated full fields are bounded, and request input is snapshotted.
    Fixture full; full.request(708u, 1u, 0x20u);
    for (unsigned i = 0; i < 512u; ++i) full.rt.memory().store8(params + 60u + i, 'a');
    for (unsigned i = 0; i < 64u; ++i) full.rt.memory().store8(params + 580u + i, 'b');
    full.start(); full.rt.memory().store8(params + 60u, 'z'); full.call(Service::update_nid, 1u);
    require(full.presenter.request.text.size() == 512u && full.presenter.request.text[0] == 'a' &&
        full.presenter.request.okay_label.size() == 64u, "Text bounds/snapshot semantics differ");
    full.service.reset(); require(!full.owner.busy() && !full.presenter.live, "Destruction retained UI or slot");
    std::cout << "V1/V2/V3 explicit Yes/No/OK/Back, default focus, UTF-8 fields and guarded output PASS\n";
}
void request_error_oracles() {
    struct Case { unsigned size, mode, options, code, result; };
    const Case cases[] = {
        {580u, 1u, 0x201u, 0x80010009u, 0x80110501u},
        {708u, 1u, 0x201u, 0x80010009u, 0x80110501u},
        {708u, 0u, 0x201u, 0x80010009u, 0x80110501u},
        {708u, 1u, 0x100u, 0x80010009u, 0x80110501u},
        {572u, 0u, 0u, 0u, 0x80110502u},
        {708u, 0u, 0x201u, 0u, 0x80110502u},
    };
    for (const auto &c : cases) {
        Fixture f; f.request(c.size, c.mode, c.options, c.code); const auto before = f.snapshot(); f.start();
        require(f.rt.memory().load32(params + 48u) == 0x1337u, "Invalid request published result during INIT");
        require(f.call(Service::update_nid, 1u) == 0u && f.call(Service::status_nid) == 3u &&
            f.rt.memory().load32(params + 48u) == c.result && f.presenter.opens == 0u && f.presenter.polls == 0u,
            "Recorded bad-option/error-code asynchronous result differs");
        require(c.size < 580u || f.rt.memory().load32(params + 576u) == 0x33445566u,
            "Invalid request invented user button");
        changed_only(before, f.snapshot(), false); f.shutdown();
    }
    // Hardware normal-errorcode-v2-bad-options: options outside the text mode
    // mask do not invalidate a V2 error dialog (YESNO is also ignored there).
    Fixture ignored; ignored.request(580u, 0u, 0x211u); ignored.start(); ignored.call(Service::update_nid, 1u);
    require(!ignored.presenter.request.yes_no && ignored.call(Service::status_nid) == 2u,
        "Hardware-proven V2 error option behavior differs");
    ignored.presenter.choose(chili::MessageDialogChoice::Back); ignored.call(Service::update_nid, 1u);
    ignored.shutdown();
    std::cout << "Recorded hardware malformed options/error code: delayed result48, no fabricated button PASS\n";
}
void strict_and_stale() {
    const std::array<std::array<unsigned, 3>, 5> unsupported{{
        {{52u, 2u, 0u}}, {{8u, 2u, 0u}}, {{4u, 12u, 0u}}, {{572u, 0x30u, 0u}}, {{572u, 0x80u, 0u}}
    }};
    for (const auto &c : unsupported) {
        Fixture f; f.request(708u, 1u); f.rt.memory().store32(params + c[0], c[1]); const auto before = f.snapshot();
        f.call(Service::init_nid, params, true);
        require(f.rt.stopped() && !f.owner.busy() && f.snapshot() == before && f.presenter.opens == 0u,
            "Unsupported native request silently succeeded or acquired owner");
    }
    { Fixture f(false); f.request(); f.start(); const auto before = f.snapshot(); f.call(Service::update_nid, 1u, true);
      require(f.rt.stopped() && f.snapshot() == before, "Absent presenter fabricated response"); }
    for (bool open_failure : {false, true}) {
        Fixture f; f.request(); f.start(); const auto before = f.snapshot();
        f.presenter.fail_open = open_failure; f.presenter.fail_poll = !open_failure;
        f.call(Service::update_nid, 1u, true);
        require(f.rt.stopped() && f.snapshot() == before && !f.presenter.live && f.presenter.closes == 1u,
            "Presenter failure published success or left allocated UI");
    }
    { Fixture f; f.request(); f.start(); f.call(Service::update_nid, 1u);
      const auto before = f.snapshot(); f.presenter.choose(chili::MessageDialogChoice::Okay); f.call(Service::update_nid, 1u, true);
      require(f.rt.stopped() && f.snapshot() == before, "Unavailable button action fabricated guest result"); }
    Fixture restart; restart.request(580u, 1u, 0x11u); restart.start(); restart.call(Service::update_nid, 1u);
    const auto old_generation = restart.presenter.request.generation;
    restart.presenter.choose(chili::MessageDialogChoice::Yes); restart.call(Service::update_nid, 1u); restart.shutdown();
    restart.request(580u, 1u, 0x111u); restart.start(); restart.call(Service::update_nid, 1u);
    require(restart.presenter.request.generation != old_generation, "Restart reused dialog generation");
    restart.presenter.selection = chili::MessageDialogSelection{old_generation, chili::MessageDialogChoice::Yes};
    restart.call(Service::update_nid, 1u);
    require(restart.call(Service::status_nid) == 2u && restart.rt.memory().load32(params + 576u) == 0x33445566u,
        "Stale previous-dialog selection answered restarted request");
    restart.presenter.choose(chili::MessageDialogChoice::No); restart.call(Service::update_nid, 1u); restart.shutdown();
    std::cout << "Strict unsupported requests/presenter failure, no auto-choice, stale restart tokens PASS\n";
}
}
int main() {
    try {
        // Construction/destruction compiles the actual native presenter without
        // creating or launching any window. Manual GUI acceptance remains open.
        { chili::Win32MessageDialogPresenter presenter; }
        ownership_and_sizes(); actual_chili_pending(); choices_and_versions(); request_error_oracles(); strict_and_stale();
        std::cout << "MsgDialog fixture PASS: live core + scheduler, four actual Chili NIDs; no GUI launched\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
