#include "display_runtime.hpp"
#include "ge_runtime.hpp"
#include "presentation.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
unsigned checks{};
void require(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
using Telemetry = chili::PerformanceTelemetry;
class ThrowingBuffer final : public std::streambuf {
    std::streamsize xsputn(const char *, std::streamsize) override {
        throw std::runtime_error("intentional telemetry write failure");
    }
    int_type overflow(int_type) override {
        throw std::runtime_error("intentional telemetry write failure");
    }
};
void set_buffer(psprecomp::Runtime &runtime, unsigned address, unsigned sync) {
    auto &c = runtime.cpu();
    c.gpr[4] = address; c.gpr[5] = 512; c.gpr[6] = 3; c.gpr[7] = sync;
    runtime.invoke_import("sceDisplay", 0x289D82FEu, c);
}
void enqueue(psprecomp::Runtime &runtime, unsigned address) {
    auto &c = runtime.cpu();
    c.gpr[4] = address; c.gpr[5] = 0; c.gpr[6] = 0xFFFFFFFFu; c.gpr[7] = 0;
    runtime.invoke_import("sceGe_user", 0xAB49E76Au, c);
}
}

int main() {
    try {
        const auto epoch = Telemetry::Clock::now();
        std::ostringstream output;
        Telemetry telemetry(output, epoch);
        {
            psprecomp::Runtime runtime;
            chili::ThreadRuntime threads(runtime); threads.initialize_bootstrap();
            chili::DisplayRuntime display(runtime, threads, &telemetry);
            set_buffer(runtime, 0x04000000u, 1);
            require(runtime.cpu().gpr[2] == 0, "first deferred request rejected");
            require(display.snapshot().address == 0, "deferred request applied early");
            set_buffer(runtime, 0x04000000u, 1);
            require(telemetry.counters()[Telemetry::Requests] == 2, "same-pointer requests collapsed");
            require(telemetry.counters()[Telemetry::ReplacedPendingRequests] == 1, "pending replacement missing");
            set_buffer(runtime, 0x04000001u, 1);
            require(runtime.cpu().gpr[2] == 0x80000103u, "invalid pointer accepted");
            require(telemetry.counters()[Telemetry::Requests] == 2, "rejected pointer counted");
            require(telemetry.counters()[Telemetry::DeferredApplies] == 0, "rejected request caused apply");
            runtime.register_function(0x08804000u, [](auto &, auto &c) { c.pc = 0; }, "telemetry_wait_done");
            runtime.cpu().gpr[31] = 0x08804000u;
            runtime.invoke_import("sceDisplay", 0x46F186C3u, runtime.cpu());
            runtime.run(runtime.cpu().pc, 100);
            require(threads.now_us() == 16683, "observer changed scheduler deadline");
            require(display.snapshot().address == 0x04000000u, "deferred latch missing");
            require(telemetry.counters()[Telemetry::DeferredApplies] == 1, "latch counted incorrectly");
            display.snapshot(); display.snapshot();
            require(telemetry.counters()[Telemetry::DeferredApplies] == 1, "snapshot invented latches");
            set_buffer(runtime, 0x04080000u, 0);
            require(runtime.cpu().gpr[2] == 0 && display.snapshot().address == 0x04080000u, "immediate apply changed");
            require(telemetry.counters()[Telemetry::ImmediateApplies] == 1, "immediate apply missing");
            require(telemetry.counters()[Telemetry::AppliedTupleChanges] == 1, "metadata change count incorrect");
        }

        // Scope events are synthetic observer tests; no window is created and
        // no programmatic event is presented as real gameplay or human input.
        { Telemetry::PresentScope event(&telemetry, false); event.finish(true); event.finish(true); }
        { Telemetry::PresentScope event(&telemetry, false); event.finish(true); }
        { Telemetry::PresentScope event(&telemetry, true); event.finish(true); }
        { Telemetry::PresentScope event(&telemetry, false); }
        require(telemetry.counters()[Telemetry::NormalPresents] == 2, "normal presentation count incorrect");
        require(telemetry.counters()[Telemetry::ForcedPresents] == 1, "forced presentation not separate");
        require(telemetry.counters()[Telemetry::PresentsWithoutNewApply] == 2, "same-apply presentation classification incorrect");
        require(telemetry.counters()[Telemetry::PresentFailures] == 1, "failed presentation counted successful");

        { Telemetry::DrawScope event(&telemetry, 4, true); event.finish(true, 12); event.finish(true, 90); }
        { Telemetry::DrawScope event(&telemetry, 2, false); event.finish(true, 999); }
        { Telemetry::DrawScope event(&telemetry, 99, true); }
        require(telemetry.counters()[Telemetry::DrawAttempts] == 3, "draw attempts incorrect");
        require(telemetry.counters()[Telemetry::DrawSuccesses] == 2 && telemetry.counters()[Telemetry::DrawFailures] == 1, "draw outcome count incorrect");
        require(telemetry.counters()[Telemetry::TriangleVertices] == 4 && telemetry.counters()[Telemetry::OtherVertices] == 2, "failed or repeated vertices charged");
        require(telemetry.counters()[Telemetry::TriangleCommittedFragments] == 12, "sprite/duplicate/failed fragments charged");

        {
            psprecomp::Runtime runtime; chili::GeRuntime ge(runtime, nullptr, 0, nullptr, &telemetry);
            runtime.memory().store32(0x08804000u, 0x0F000001u);
            runtime.memory().store32(0x08804004u, 0x0C000000u);
            enqueue(runtime, 0x08804000u);
            require(runtime.cpu().gpr[2] == 1 && ge.completed.contains(1), "observer changed valid stream completion");
            require(telemetry.counters()[Telemetry::GeListsStarted] == 1 && telemetry.counters()[Telemetry::GeListsCompleted] == 1, "completed stream telemetry incorrect");
            require(telemetry.counters()[Telemetry::GeWordsRead] == 2, "valid stream words incorrect");
        }
        {
            psprecomp::Runtime runtime; chili::GeRuntime ge(runtime, nullptr, 0, nullptr, &telemetry);
            runtime.memory().store32(0x08804000u, 0x0C000000u);
            enqueue(runtime, 0x08804000u);
            require(!runtime.stop_reason().empty() && ge.completed.empty(), "invalid stream was acknowledged");
            require(telemetry.counters()[Telemetry::GeListsStarted] == 2 && telemetry.counters()[Telemetry::GeListsCompleted] == 1, "failed stream counted completed");
            require(telemetry.counters()[Telemetry::GeWordsRead] == 3, "early-exit words lost");
        }
        {
            psprecomp::Runtime runtime; chili::GeRuntime ge(runtime, nullptr, 0, nullptr, &telemetry);
            runtime.memory().store32(0x08804000u, 0x04060002u);
            enqueue(runtime, 0x08804000u);
            require(!runtime.stop_reason().empty() && ge.completed.empty(), "unsupported real PRIM acknowledged");
            require(telemetry.counters()[Telemetry::DrawAttempts] == 4 && telemetry.counters()[Telemetry::DrawFailures] == 2, "real failed draw event missing");
            require(telemetry.counters()[Telemetry::TriangleVertices] == 4 && telemetry.counters()[Telemetry::OtherVertices] == 2, "real failure charged vertices");
        }
        {
            psprecomp::Runtime runtime; chili::ThreadRuntime threads(runtime); threads.initialize_bootstrap();
            chili::DisplayRuntime display(runtime, threads); // Production-default null observer.
            set_buffer(runtime, 0x04000000u, 0);
            require(runtime.cpu().gpr[2] == 0 && display.snapshot().address == 0x04000000u, "default display semantics changed");
            chili::GeRuntime ge(runtime);
            runtime.memory().store32(0x08804000u, 0x0F000001u);
            runtime.memory().store32(0x08804004u, 0x0C000000u);
            enqueue(runtime, 0x08804000u);
            require(runtime.cpu().gpr[2] == 1 && ge.completed.contains(1), "default GE semantics changed");
        }

        Telemetry::Sample sample; sample.wall_us = 99;
        require(!telemetry.sample(epoch + std::chrono::milliseconds(999), 16683, sample), "output interval too frequent");
        require(sample.wall_us == 99, "not-due sample altered output");
        require(telemetry.sample(epoch + std::chrono::seconds(1), 16683, sample), "one-second sample missing");
        require(sample.delta_wall_us == 1'000'000 && sample.delta_guest_us == 16683, "wall/guest time mixed");
        require(sample.delta[Telemetry::Requests] == 3 && sample.delta[Telemetry::NormalPresents] == 2 && sample.delta[Telemetry::ForcedPresents] == 1, "sample event counts incorrect");
        require(!telemetry.sample(epoch + std::chrono::seconds(2), 16682, sample), "backward guest clock accepted");
        output << std::hex; output.width(13); const auto flags = output.flags();
        telemetry.poll(epoch + std::chrono::seconds(2), 16683);
        require(output.flags() == flags, "telemetry changed shared stream formatting");
        require(output.width() == 13, "telemetry consumed caller pending width");
        require(output.str().rfind("[perf]", 0) == 0, "caller pending width padded telemetry prefix");
        require(output.str().find("[perf] wall_us=2000000 guest_us=16683 d_wall_us=1000000 d_guest_us=0") != std::string::npos, "raw units/output cadence incorrect");
        require(output.str().find("fps") == std::string::npos, "invented FPS field emitted");
        const auto size = output.str().size();
        telemetry.poll(epoch + std::chrono::milliseconds(2999), 16683);
        require(output.str().size() == size, "more than one row per second");
        require(output.width() == 13, "not-due poll altered pending width");
        require(!telemetry.output_failed(), "healthy output reported failure");
        {
            ThrowingBuffer buffer;
            std::ostream throwing_output(&buffer);
            throwing_output.exceptions(std::ios::badbit | std::ios::failbit);
            throwing_output << std::hex;
            throwing_output.width(17);
            const auto throwing_flags = throwing_output.flags();
            Telemetry throwing_telemetry(throwing_output, epoch);
            throwing_telemetry.poll(epoch + std::chrono::seconds(1), 0);
            require(throwing_telemetry.output_failed(), "throwing output failure not recorded");
            require(throwing_output.bad(), "throwing buffer did not exercise write failure");
            require(throwing_output.flags() == throwing_flags, "write exception lost caller flags");
            require(throwing_output.width() == 17, "write exception consumed caller pending width");
        }
        std::cout << "PASS " << checks << " telemetry assertions: actual display/GE effects and default-null path, synthetic presentation classification; no window/game/FPS acceptance claimed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
