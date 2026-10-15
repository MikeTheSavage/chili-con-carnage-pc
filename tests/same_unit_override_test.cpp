// A host function registered over a guest address has to run for every call of
// that address, including the calls made from inside the function's own
// generated unit, which generated code otherwise turns into native jumps.
//
// The build compiles this file twice. With PSPRECOMP_WRITE_FIXTURE it is the
// tool that writes the guest program below as an ELF; psp_recomp --auto then
// turns that into one generated unit, and the second build links the unit and
// runs it under the runtime with and without a host function over the callee.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint32_t kCaller = 0x08804000u;
constexpr std::uint32_t kCallee = 0x08804020u;
constexpr std::uint32_t kCode[] = {
    0x27BDFFF0u, // caller: addiu sp, sp, -16
    0xAFBF0000u, //         sw    ra, 0(sp)
    0x0E201008u, //         jal   callee
    0x00000000u, //         nop
    0x8FBF0000u, //         lw    ra, 0(sp)
    0x03E00008u, //         jr    ra
    0x27BD0010u, //         addiu sp, sp, 16
    0x00000000u, //         nop
    0x27BDFFF0u, // callee: addiu sp, sp, -16
    0x24020007u, //         addiu v0, zero, 7
    0x03E00008u, //         jr    ra
    0x27BD0010u, //         addiu sp, sp, 16
};
}

#ifdef PSPRECOMP_WRITE_FIXTURE

static void put16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
}

static void put32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4u; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8u));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output.elf>\n";
        return 2;
    }
    constexpr std::uint32_t code_bytes = static_cast<std::uint32_t>(sizeof(kCode));
    std::vector<std::uint8_t> bytes(0x80u + code_bytes, 0u);
    bytes[0] = 0x7Fu; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 1u; bytes[5] = 1u; bytes[6] = 1u;
    put16(bytes, 16u, 2u);          // ET_EXEC
    put16(bytes, 18u, 8u);          // EM_MIPS
    put32(bytes, 20u, 1u);
    put32(bytes, 24u, kCaller);
    put32(bytes, 28u, 52u);
    put16(bytes, 40u, 52u);
    put16(bytes, 42u, 32u);
    put16(bytes, 44u, 1u);
    put16(bytes, 46u, 40u);

    put32(bytes, 52u, 1u);          // PT_LOAD
    put32(bytes, 56u, 0x80u);
    put32(bytes, 60u, kCaller);
    put32(bytes, 64u, kCaller);
    put32(bytes, 68u, code_bytes);
    put32(bytes, 72u, code_bytes);
    put32(bytes, 76u, 5u);
    put32(bytes, 80u, 16u);
    for (std::size_t i = 0; i < std::size(kCode); ++i) put32(bytes, 0x80u + i * 4u, kCode[i]);

    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out ? 0 : 1;
}

#else

#include "psprecomp/runtime.hpp"

namespace {
constexpr std::uint32_t kStop = 0x08900000u;
constexpr std::uint32_t kStack = 0x09F00000u;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void stop(psprecomp::Runtime &rt, psprecomp::AllegrexContext &) { rt.stop("returned"); }

int replacement_calls = 0;
// Replaces the callee: answers 42 where the guest code answers 7.
void replacement(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ++replacement_calls;
    ctx.gpr[2] = 42u;
    ctx.pc = ctx.gpr[31];
}

int observer_calls = 0;
psprecomp::Runtime::RecompiledFunction observed{};
// Counts the call and runs the code it was registered over.
void observer(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    ++observer_calls;
    observed(rt, ctx);
}

std::uint32_t run_caller(psprecomp::Runtime &runtime) {
    runtime.cpu().gpr[2] = 0u;
    runtime.cpu().gpr[29] = kStack;
    runtime.cpu().gpr[31] = kStop;
    runtime.run(kCaller, 64u);
    require(runtime.stop_reason() == "returned", "The caller did not return to its return address");
    require(runtime.cpu().gpr[29] == kStack, "The caller left the stack pointer moved");
    return runtime.cpu().gpr[2];
}
}

int main() {
    try {
        {
            psprecomp::Runtime runtime;
            psprecomp::register_generated_functions(runtime);
            runtime.register_function(kStop, &stop, "test_stop");
            require(run_caller(runtime) == 7u, "The generated callee did not run");
        }
        {
            psprecomp::Runtime runtime;
            psprecomp::register_generated_functions(runtime);
            runtime.register_function(kStop, &stop, "test_stop");
            runtime.register_function(kCallee, &replacement, "test_replacement");
            const std::uint32_t result = run_caller(runtime);
            require(replacement_calls == 1, "A call from inside the unit did not reach the host function over the callee");
            require(result == 42u, "The guest callee ran although a host function replaces it");
        }
        {
            psprecomp::Runtime runtime;
            psprecomp::register_generated_functions(runtime);
            runtime.register_function(kStop, &stop, "test_stop");
            observed = runtime.function_at(kCallee);
            require(observed != nullptr, "No generated code is registered at the callee");
            runtime.register_function(kCallee, &observer, "test_observer");
            const std::uint32_t result = run_caller(runtime);
            require(observer_calls == 1, "A host function that runs the original was not called exactly once");
            require(result == 7u, "The original callee did not run from the host function");
        }
        std::puts("same-unit override test passed");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "same-unit override test failed: " << e.what() << '\n';
        return 1;
    }
}

#endif
