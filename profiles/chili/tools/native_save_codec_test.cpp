#include "native_save_codec.hpp"
#include "golden_vectors.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

using namespace chili::native_save;

namespace {
std::size_t checks{};
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
Bytes pattern(std::size_t length, unsigned multiply, unsigned add) {
    Bytes result(length);
    for (std::size_t i = 0; i < length; ++i) result[i] = static_cast<Byte>(i * multiply + add);
    return result;
}
Bytes hex(std::string_view text) {
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("invalid fixture hex");
    };
    if (text.size() % 2 != 0) throw std::runtime_error("odd fixture hex");
    Bytes result(text.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<Byte>((digit(text[i * 2]) << 4) | digit(text[i * 2 + 1]));
    return result;
}
void put64(Bytes& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[at + i] = static_cast<Byte>(value >> (i * 8));
}
template<class F> void rejects(F&& f, ErrorCode expected, const char* message) {
    try { f(); }
    catch (const CodecError& error) { require(error.code() == expected, message); return; }
    throw std::runtime_error(message);
}
template<class F> void rejects_any(F&& f, const char* message) {
    try { f(); }
    catch (const CodecError&) { require(true, message); return; }
    throw std::runtime_error(message);
}
Key fixture_key() {
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<Byte>(i);
    return key;
}
Nonce fixture_nonce() {
    Nonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<Byte>(i);
    return nonce;
}

void golden_tests(const Key& key, const Nonce& nonce) {
    for (const auto& v : golden_vectors) {
        const auto plaintext = pattern(v.plaintext_bytes, 37, 11);
        const auto sfo = pattern(v.sfo_bytes, 19, 5);
        const auto expected = hex(v.container_hex);
        require(encode_for_test(plaintext, sfo, key, nonce) == expected,
                "CNG encryption differs from independent OpenSSL native-format golden");
        require(decode(expected, sfo, key) == plaintext, "CNG golden decrypt differs");
        require(expected.size() == plaintext.size() + header_bytes + tag_bytes,
                "native format unexpectedly pads nonaligned plaintext");
    }
    const Bytes empty;
    const auto empty_container = encode_for_test(empty, empty, key, nonce);
    require(decode(empty_container, empty, key).empty(), "empty authenticated container failed");
    auto bad_empty = empty_container;
    bad_empty.back() ^= 1;
    rejects([&] { (void)decode(bad_empty, empty, key); }, ErrorCode::AuthenticationFailed,
            "empty ciphertext tag was not verified");
}

void tamper_tests(const Key& key, const Nonce& nonce) {
    const auto plaintext = pattern(1066, 37, 11);
    const auto sfo = pattern(4912, 19, 5);
    const auto valid = encode_for_test(plaintext, sfo, key, nonce);
    const auto key_before = key;
    const auto nonce_before = nonce;
    auto wrong_key = key;
    wrong_key[7] ^= 0x40;
    rejects([&] { (void)decode(valid, sfo, wrong_key); }, ErrorCode::AuthenticationFailed,
            "wrong key was accepted");

    // Every byte in the header, ciphertext, and tag must be rejected if changed.
    for (std::size_t at = 0; at < valid.size(); ++at) {
        auto bad = valid;
        bad[at] ^= 1;
        rejects_any([&] { (void)decode(bad, sfo, key); }, "single-byte container tamper accepted");
    }
    for (std::size_t at = 0; at < sfo.size(); ++at) {
        auto bad_sfo = sfo;
        bad_sfo[at] ^= 1;
        rejects([&] { (void)decode(valid, bad_sfo, key); }, ErrorCode::AuthenticationFailed,
                "raw SFO byte tamper accepted");
    }
    auto longer_sfo = sfo;
    longer_sfo.push_back(0);
    rejects([&] { (void)decode(valid, longer_sfo, key); }, ErrorCode::AuthenticationFailed,
            "wrong SFO byte length accepted");
    auto changed_sfo_length = valid;
    put64(changed_sfo_length, 24, longer_sfo.size());
    rejects([&] { (void)decode(changed_sfo_length, longer_sfo, key); }, ErrorCode::AuthenticationFailed,
            "rewritten SFO header length was not authenticated");

    // Rebuild a structurally consistent shorter payload. Header is still AAD.
    auto shorter_payload = valid;
    shorter_payload.erase(shorter_payload.begin() + header_bytes);
    put64(shorter_payload, 16, plaintext.size() - 1);
    rejects([&] { (void)decode(shorter_payload, sfo, key); }, ErrorCode::AuthenticationFailed,
            "rewritten payload length was not authenticated");

    // Failure does not assign a caller's existing destination; success is returned by value.
    auto destination = pattern(11, 1, 0x55);
    const auto destination_before = destination;
    auto invalid = valid;
    invalid.back() ^= 1;
    rejects([&] { destination = decode(invalid, sfo, key); }, ErrorCode::AuthenticationFailed,
            "failure did not preserve caller destination");
    require(destination == destination_before, "unauthenticated plaintext escaped through assignment");
    require(plaintext == pattern(1066, 37, 11) && sfo == pattern(4912, 19, 5), "inputs modified");
    require(key == key_before && nonce == nonce_before, "key or nonce input modified");
    require(valid == hex(golden_vectors.back().container_hex), "container input modified");
}

void format_and_bound_tests(const Key& key, const Nonce& nonce) {
    const auto plaintext = pattern(17, 37, 11);
    const auto sfo = pattern(53, 19, 5);
    const auto valid = encode_for_test(plaintext, sfo, key, nonce);
    for (std::size_t length = 0; length < valid.size(); ++length)
        rejects([&] { (void)decode(std::span<const Byte>(valid).first(length), sfo, key); },
                ErrorCode::InvalidFormat, "truncated container accepted");
    for (std::size_t count : {1u, 16u, 64u}) {
        auto extended = valid;
        extended.insert(extended.end(), count, 0);
        rejects([&] { (void)decode(extended, sfo, key); }, ErrorCode::InvalidFormat,
                "trailing bytes accepted");
    }
    // Unknown and PSP-like arbitrary bytes are never dispatched to a PSP fallback.
    for (const auto& unknown : {Bytes(1082, 0), pattern(1082, 29, 3), Bytes(64, 0xff)})
        rejects([&] { (void)decode(unknown, sfo, key); }, ErrorCode::InvalidFormat,
                "unknown or PSP-style bytes accepted");
    for (std::uint64_t oversized : {std::uint64_t(max_payload_bytes) + 1,
                                   std::numeric_limits<std::uint64_t>::max()}) {
        auto bad = valid;
        put64(bad, 16, oversized);
        rejects([&] { (void)decode(bad, sfo, key); }, ErrorCode::Bounds,
                "oversized payload header accepted");
    }
    auto bad_sfo_header = valid;
    put64(bad_sfo_header, 24, std::numeric_limits<std::uint64_t>::max());
    rejects([&] { (void)decode(bad_sfo_header, sfo, key); }, ErrorCode::Bounds,
            "oversized SFO header accepted");
    const Bytes oversized_sfo(max_sfo_bytes + 1);
    rejects([&] { (void)encode(plaintext, oversized_sfo, key); }, ErrorCode::Bounds,
            "oversized SFO input encoded");
    rejects([&] { (void)decode(valid, oversized_sfo, key); }, ErrorCode::Bounds,
            "oversized SFO input decoded");
    const Bytes oversized_payload(max_payload_bytes + 1);
    rejects([&] { (void)encode(oversized_payload, sfo, key); }, ErrorCode::Bounds,
            "oversized plaintext encoded");
    // Maximum supported payload is actual allocated storage, not a forged span.
    const auto maximum = pattern(max_payload_bytes, 37, 11);
    const auto maximum_container = encode_for_test(maximum, sfo, key, nonce);
    require(decode(maximum_container, sfo, key) == maximum, "maximum payload boundary failed");
    const auto maximum_sfo = pattern(max_sfo_bytes, 19, 5);
    const auto maximum_sfo_container = encode_for_test(plaintext, maximum_sfo, key, nonce);
    require(decode(maximum_sfo_container, maximum_sfo, key) == plaintext,
            "maximum raw SFO boundary failed");
}

void production_rng_and_isolation_tests(const Key& key) {
    const auto plaintext = pattern(1066, 37, 11);
    const auto sfo = pattern(4912, 19, 5);
    const auto first = encode(plaintext, sfo, key);
    const auto second = encode(plaintext, sfo, key);
    require(!std::equal(first.begin() + 32, first.begin() + 44, second.begin() + 32),
            "production nonce smoke test repeated a nonce");
    require(decode(first, sfo, key) == plaintext && decode(second, sfo, key) == plaintext,
            "production nonce round trips failed");
    std::array<std::future<bool>, 8> jobs;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
        jobs[i] = std::async(std::launch::async, [key, i] {
            const auto input = pattern(1066 + i, 37, 11);
            const auto metadata = pattern(53 + i, 19, 5);
            const auto container = encode(input, metadata, key);
            return decode(container, metadata, key) == input;
        });
    }
    for (auto& job : jobs) require(job.get(), "independent parallel operation failed");
}

Bytes read_bounded(const char* path, std::size_t maximum) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.is_open(), "cannot open capture fixture file");
    const auto length = file.tellg();
    require(length >= 0 && static_cast<std::uint64_t>(length) <= maximum,
            "capture fixture file exceeds bound");
    Bytes bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(file.good(), "cannot read capture fixture file");
    return bytes;
}

void captured_tests(const char* payload_path, const char* request_path, const char* oracle_path) {
    struct PrivateCapture {
        Bytes payload, request;
        Key key{};
        ~PrivateCapture() {
            if (!payload.empty()) SecureZeroMemory(payload.data(), payload.size());
            if (!request.empty()) SecureZeroMemory(request.data(), request.size());
            SecureZeroMemory(key.data(), key.size());
        }
    } capture;
    capture.payload = read_bounded(payload_path, 1066);
    capture.request = read_bounded(request_path, 65536);
    const auto oracle = read_bounded(oracle_path, 1130);
    require(capture.payload.size() == 1066 && capture.request.size() == 65536 && oracle.size() == 1130,
            "unexpected actual capture size");
    constexpr std::size_t params = 0x68c0;
    auto request32 = [&](std::size_t offset) {
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i)
            value |= std::uint32_t(capture.request[params + offset + i]) << (i * 8);
        return value;
    };
    require(request32(0) == 1536 && request32(48) == 5 && request32(1516) == 0 &&
            request32(116) == 0x09cb35c0 && request32(120) == 1066 && request32(124) == 1066,
            "capture is not the reviewed pointer-followed LISTSAVE request");
    std::copy_n(capture.request.begin() + params + 1500, capture.key.size(), capture.key.begin());
    // Synthetic opaque AAD; no claim that these are an actual saved PARAM.SFO.
    const auto sfo = pattern(4912, 19, 5);
    const auto nonce = fixture_nonce();
    require(encode_for_test(capture.payload, sfo, capture.key, nonce) == oracle,
            "actual payload/private key differs from independent OpenSSL oracle");
    auto decoded = decode(oracle, sfo, capture.key);
    require(decoded == capture.payload, "actual payload/private key decrypt differs");
    SecureZeroMemory(decoded.data(), decoded.size());
    auto bad = oracle;
    bad[header_bytes + 37] ^= 0x80;
    rejects([&] { (void)decode(bad, sfo, capture.key); }, ErrorCode::AuthenticationFailed,
            "actual capture ciphertext tamper accepted");
    const auto production = encode(capture.payload, sfo, capture.key);
    decoded = decode(production, sfo, capture.key);
    require(decoded == capture.payload, "actual capture production nonce round trip failed");
    SecureZeroMemory(decoded.data(), decoded.size());
    std::cout << "PASS: validated captured 1,066-byte payload/private key with independent OpenSSL oracle; "
                 "synthetic metadata AAD; no key or game bytes logged\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto key = fixture_key();
        const auto nonce = fixture_nonce();
        golden_tests(key, nonce);
        tamper_tests(key, nonce);
        format_and_bound_tests(key, nonce);
        production_rng_and_isolation_tests(key);
        if (argc != 1) {
            require(argc == 5 && std::string_view(argv[1]) == "--captured", "invalid fixture arguments");
            captured_tests(argv[2], argv[3], argv[4]);
        }
        std::cout << "PASS: " << checks << " native save codec checks; 6 independent OpenSSL golden vectors; "
                     "1,066-byte synthetic payload; all container/SFO bytes tampered; no real save writes or key logging\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
