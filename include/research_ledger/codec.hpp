#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/limits.hpp"

namespace research_ledger {

// Explicit little-endian encoding. Integers are written byte by byte: nothing
// in the runtime depends on struct packing, host endianness or alignment, so a
// snapshot or a frame written on one host decodes identically on another.
class ByteWriter {
public:
    explicit ByteWriter(std::size_t reserve = 0);

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void bytes(std::span<const std::byte> value);
    void digest(const Digest& value);
    void boolean(bool value);

    // Length-prefixed string. The declared length is the payload length; no
    // padding, no terminator.
    void string(std::string_view value, std::uint32_t max_length);

    // Counts inside payloads are u32 with an explicit bound; the bound is
    // checked before the count is written so that a decoded payload can never
    // declare more elements than the limit allows.
    void count(std::uint32_t value, std::uint32_t max_value);

    void patch_u32(std::size_t offset, std::uint32_t value);

    [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buffer_); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }

private:
    void fail(ErrorCode code, std::string message);

    std::vector<std::byte> buffer_{};
    Status status_{};
};

// Bounded reader. Every read checks that enough bytes remain and that declared
// lengths are within the configured limits, so a malformed stream produces a
// typed error instead of an over-read or an oversized allocation.
class ByteReader {
public:
    // truncation_code is reported when a declared length exceeds the bytes
    // that remain: a snapshot reports PERSISTENCE_TRUNCATED, a protocol payload
    // reports PROTOCOL_ERROR.
    ByteReader(std::span<const std::byte> bytes, const Limits& limits,
               ErrorCode truncation_code = ErrorCode::PersistenceTruncated);

    [[nodiscard]] bool empty() const noexcept { return offset_ >= bytes_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    // Error code used for malformed content in this reader: PERSISTENCE_* for a
    // snapshot, PROTOCOL_ERROR for a frame payload.
    [[nodiscard]] ErrorCode malformed_code() const noexcept { return truncation_code_; }

    Result<std::uint8_t> u8();
    Result<std::uint16_t> u16();
    Result<std::uint32_t> u32();
    Result<std::uint64_t> u64();
    Result<std::int64_t> i64();
    Result<bool> boolean();
    Result<Digest> digest();
    Result<std::string> string(std::uint32_t max_length);
    Result<std::uint32_t> count(std::uint32_t max_value);
    Result<std::span<const std::byte>> bytes(std::size_t length);

    // Typed identity reads. The stored domain tag must match the requested
    // domain: reading a hypothesis identity as an experiment identity is an
    // INVALID_IDENTITY failure, never a reinterpretation of the same bytes.
    Result<std::uint64_t> identity_value(IdentityDomain expected);
    Result<std::uint32_t> generation_value(IdentityDomain expected);

    // Reads an identity that may legitimately be absent. Zero never becomes a
    // valid identity: it is reported to the caller as "no value", which is what
    // a worker with no previous incarnation sends.
    Result<std::uint64_t> optional_identity_value(IdentityDomain expected);

    template <class Strong>
    Result<Strong> read_strong_id() {
        auto value = identity_value(Strong::domain);
        if (!value.ok()) {
            return value.status();
        }
        Strong identity = Strong::from_value(value.value());
        if (!identity.valid()) {
            return Status(ErrorCode::InvalidIdentity, "identity value zero is never valid");
        }
        return identity;
    }

    template <class Gen>
    Result<Gen> read_generation() {
        auto value = generation_value(Gen::domain);
        if (!value.ok()) {
            return value.status();
        }
        Gen generation = Gen::from_value(value.value());
        if (!generation.valid()) {
            return Status(ErrorCode::InvalidIdentity, "generation zero is never valid authority");
        }
        return generation;
    }

    Status expect_consumed(std::string_view what);
    Status fail(ErrorCode code, std::string message);

private:
    Result<std::span<const std::byte>> take(std::size_t length);

    std::span<const std::byte> bytes_{};
    Limits limits_{};
    std::size_t offset_ = 0;
    Status status_{};
    ErrorCode truncation_code_ = ErrorCode::PersistenceTruncated;
};

// Typed identity writes: the domain tag travels with the value.
void write_strong_id(ByteWriter& writer, IdentityDomain domain, std::uint64_t value);
void write_generation(ByteWriter& writer, IdentityDomain domain, std::uint32_t value);

template <class Strong>
void write_identity(ByteWriter& writer, Strong identity) {
    write_strong_id(writer, Strong::domain, identity.value());
}

template <class Gen>
void write_entity_generation(ByteWriter& writer, Gen generation) {
    write_generation(writer, Gen::domain, generation.value());
}

}  // namespace research_ledger
