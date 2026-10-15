#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace chili::native_save {

using Byte = std::uint8_t;
using Key = std::array<Byte, 16>;
using Nonce = std::array<Byte, 12>;
using Bytes = std::vector<Byte>;

// This is a native PC container. It does not read or write PSP SECURE.BIN.
inline constexpr std::array<Byte, 8> magic{'C', 'H', 'L', 'S', 'A', 'V', 'E', 0};
inline constexpr std::uint32_t format_version = 1;
inline constexpr std::size_t header_bytes = 48;
inline constexpr std::size_t tag_bytes = 16;
inline constexpr std::size_t max_payload_bytes = 32u * 1024u * 1024u;
inline constexpr std::size_t max_sfo_bytes = 1024u * 1024u;

enum class ErrorCode { InvalidFormat, Bounds, AuthenticationFailed, CryptoFailure };

class CodecError final : public std::runtime_error {
public:
    CodecError(ErrorCode code, const char* message, std::int32_t native_status = 0);
    ErrorCode code() const noexcept { return code_; }
    std::int32_t native_status() const noexcept { return native_status_; }
private:
    ErrorCode code_;
    std::int32_t native_status_;
};

// raw_sfo must be the exact original bytes stored with the native container.
// No disk I/O, guest writes, key logging, or scheduler changes occur here.
Bytes encode(std::span<const Byte> plaintext, std::span<const Byte> raw_sfo, const Key& key);
Bytes decode(std::span<const Byte> container, std::span<const Byte> raw_sfo, const Key& key);

#ifdef CHILI_NATIVE_SAVE_CODEC_TESTING
// Fixture-only API. Production builds must not define this macro.
Bytes encode_for_test(std::span<const Byte> plaintext, std::span<const Byte> raw_sfo,
                      const Key& key, const Nonce& nonce);
#endif

} // namespace chili::native_save
