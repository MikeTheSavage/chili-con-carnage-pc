#include "native_save_codec.hpp"

#ifndef _WIN32
#error The native save codec requires Windows CNG.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace chili::native_save {

CodecError::CodecError(ErrorCode code, const char* message, std::int32_t native_status)
    : std::runtime_error(message), code_(code), native_status_(native_status) {}

namespace {

constexpr NTSTATUS auth_tag_mismatch = static_cast<NTSTATUS>(0xC000A002u);

[[noreturn]] void fail(ErrorCode code, const char* message, NTSTATUS status = 0) {
    throw CodecError(code, message, static_cast<std::int32_t>(status));
}

void check(NTSTATUS status) {
    if (status < 0) fail(ErrorCode::CryptoFailure, "Windows CNG operation failed", status);
}

struct Algorithm {
    BCRYPT_ALG_HANDLE value{};
    ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    Algorithm() = default;
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
};

struct SymmetricKey {
    BCRYPT_KEY_HANDLE value{};
    ~SymmetricKey() { if (value) BCryptDestroyKey(value); }
    SymmetricKey() = default;
    SymmetricKey(const SymmetricKey&) = delete;
    SymmetricKey& operator=(const SymmetricKey&) = delete;
};

struct SensitiveBuffer {
    Bytes bytes;
    ~SensitiveBuffer() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
    Bytes release() { return std::exchange(bytes, {}); }
};

struct SensitiveKeyCopy {
    Key bytes;
    explicit SensitiveKeyCopy(const Key& source) : bytes(source) {}
    ~SensitiveKeyCopy() { SecureZeroMemory(bytes.data(), bytes.size()); }
};

// Member order ensures the key is destroyed before its object storage is wiped.
struct Cipher {
    Algorithm algorithm;
    SensitiveBuffer object;
    SymmetricKey key;

    explicit Cipher(const Key& source) {
        check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_AES_ALGORITHM, nullptr, 0));
        check(BCryptSetProperty(algorithm.value, BCRYPT_CHAINING_MODE,
              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
              sizeof(BCRYPT_CHAIN_MODE_GCM), 0));

        BCRYPT_AUTH_TAG_LENGTHS_STRUCT tags{};
        ULONG copied{};
        check(BCryptGetProperty(algorithm.value, BCRYPT_AUTH_TAG_LENGTH,
              reinterpret_cast<PUCHAR>(&tags), sizeof(tags), &copied, 0));
        if (copied != sizeof(tags) || tag_bytes < tags.dwMinLength || tag_bytes > tags.dwMaxLength ||
            (tags.dwIncrement != 0 && (tag_bytes - tags.dwMinLength) % tags.dwIncrement != 0))
            fail(ErrorCode::CryptoFailure, "Windows CNG cannot provide the required GCM tag");

        ULONG object_size{};
        check(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
              reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &copied, 0));
        if (copied != sizeof(object_size) || object_size == 0 || object_size > 1024u * 1024u)
            fail(ErrorCode::CryptoFailure, "Windows CNG reported invalid key-object storage");
        object.bytes.resize(object_size);
        SensitiveKeyCopy copy(source);
        check(BCryptGenerateSymmetricKey(algorithm.value, &key.value, object.bytes.data(),
              object_size, copy.bytes.data(), static_cast<ULONG>(copy.bytes.size()), 0));
    }
};

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
}
void put64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<Byte>(value >> (i * 8));
}
std::uint32_t get32(std::span<const Byte> bytes, std::size_t offset) {
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(bytes[offset + i]) << (i * 8);
    return value;
}
std::uint64_t get64(std::span<const Byte> bytes, std::size_t offset) {
    std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}

void validate_sizes(std::uint64_t payload, std::uint64_t sfo) {
    if (payload > max_payload_bytes || sfo > max_sfo_bytes)
        fail(ErrorCode::Bounds, "Native save exceeds the configured size limit");
}

Bytes make_aad(std::span<const Byte> header, std::span<const Byte> raw_sfo) {
    Bytes aad;
    aad.reserve(header_bytes + raw_sfo.size());
    aad.insert(aad.end(), header.begin(), header.end());
    aad.insert(aad.end(), raw_sfo.begin(), raw_sfo.end());
    return aad;
}

Bytes encode_with_nonce(std::span<const Byte> plaintext, std::span<const Byte> raw_sfo,
                        const Key& key, const Nonce& nonce) {
    validate_sizes(plaintext.size(), raw_sfo.size());
    Bytes out(header_bytes + plaintext.size() + tag_bytes, 0);
    std::copy(magic.begin(), magic.end(), out.begin());
    put32(out, 8, format_version);
    put32(out, 12, static_cast<std::uint32_t>(header_bytes));
    put64(out, 16, plaintext.size());
    put64(out, 24, raw_sfo.size());
    std::copy(nonce.begin(), nonce.end(), out.begin() + 32);
    // Bytes 44..47 are reserved and remain zero.
    Bytes aad = make_aad(std::span<const Byte>(out).first(header_bytes), raw_sfo);
    Nonce working_nonce = nonce;
    Cipher cipher(key);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = working_nonce.data();
    info.cbNonce = static_cast<ULONG>(working_nonce.size());
    info.pbAuthData = aad.data();
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = out.data() + header_bytes + plaintext.size();
    info.cbTag = static_cast<ULONG>(tag_bytes);
    ULONG written{};
    // Both null pointers invoke CNG's documented zero-data authenticated call.
    PUCHAR input = plaintext.empty() ? nullptr : const_cast<PUCHAR>(plaintext.data());
    PUCHAR output = plaintext.empty() ? nullptr : out.data() + header_bytes;
    check(BCryptEncrypt(cipher.key.value, input, static_cast<ULONG>(plaintext.size()), &info,
          nullptr, 0, output, static_cast<ULONG>(plaintext.size()), &written, 0));
    if (written != plaintext.size())
        fail(ErrorCode::CryptoFailure, "Windows CNG returned an unexpected ciphertext length");
    return out;
}

} // namespace

Bytes encode(std::span<const Byte> plaintext, std::span<const Byte> raw_sfo, const Key& key) {
    validate_sizes(plaintext.size(), raw_sfo.size());
    Nonce nonce{};
    check(BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    return encode_with_nonce(plaintext, raw_sfo, key, nonce);
}

Bytes decode(std::span<const Byte> container, std::span<const Byte> raw_sfo, const Key& key) {
    if (container.size() < header_bytes + tag_bytes ||
        !std::equal(magic.begin(), magic.end(), container.begin()))
        fail(ErrorCode::InvalidFormat, "Unknown native save format");
    if (get32(container, 8) != format_version || get32(container, 12) != header_bytes ||
        get32(container, 44) != 0)
        fail(ErrorCode::InvalidFormat, "Unsupported native save header");
    const auto payload_size = get64(container, 16);
    const auto sfo_size = get64(container, 24);
    validate_sizes(payload_size, sfo_size);
    validate_sizes(payload_size, raw_sfo.size());
    // Bounds above make this addition safe, including on a 32-bit host.
    if (container.size() != header_bytes + static_cast<std::size_t>(payload_size) + tag_bytes)
        fail(ErrorCode::InvalidFormat, "Native save has a noncanonical length");
    if (sfo_size != raw_sfo.size())
        fail(ErrorCode::AuthenticationFailed, "Native save metadata does not match");

    Bytes aad = make_aad(container.first(header_bytes), raw_sfo);
    Nonce nonce{};
    std::copy_n(container.begin() + 32, nonce.size(), nonce.begin());
    std::array<Byte, tag_bytes> tag{};
    std::copy_n(container.end() - tag_bytes, tag.size(), tag.begin());
    SensitiveBuffer plaintext;
    plaintext.bytes.resize(static_cast<std::size_t>(payload_size));
    Cipher cipher(key);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce.data();
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = aad.data();
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());
    ULONG written{};
    PUCHAR input = payload_size == 0 ? nullptr : const_cast<PUCHAR>(container.data() + header_bytes);
    PUCHAR output = payload_size == 0 ? nullptr : plaintext.bytes.data();
    const auto status = BCryptDecrypt(cipher.key.value, input, static_cast<ULONG>(payload_size),
          &info, nullptr, 0, output, static_cast<ULONG>(payload_size), &written, 0);
    if (status == auth_tag_mismatch)
        fail(ErrorCode::AuthenticationFailed, "Native save authentication failed", status);
    check(status);
    if (written != payload_size)
        fail(ErrorCode::CryptoFailure, "Windows CNG returned an unexpected plaintext length");
    return plaintext.release();
}

#ifdef CHILI_NATIVE_SAVE_CODEC_TESTING
Bytes encode_for_test(std::span<const Byte> plaintext, std::span<const Byte> raw_sfo,
                      const Key& key, const Nonce& nonce) {
    return encode_with_nonce(plaintext, raw_sfo, key, nonce);
}
#endif

} // namespace chili::native_save
