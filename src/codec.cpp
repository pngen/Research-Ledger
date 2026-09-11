#include "research_ledger/codec.hpp"

#include <utility>

namespace research_ledger {

ByteWriter::ByteWriter(std::size_t reserve) { buffer_.reserve(reserve); }

void ByteWriter::fail(ErrorCode code, std::string message) {
    if (status_.ok()) {
        status_ = Status(code, std::move(message));
    }
}

void ByteWriter::u8(std::uint8_t value) {
    if (!status_.ok()) {
        return;
    }
    buffer_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xffu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void ByteWriter::u32(std::uint32_t value) {
    u16(static_cast<std::uint16_t>(value & 0xffffu));
    u16(static_cast<std::uint16_t>((value >> 16) & 0xffffu));
}

void ByteWriter::u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value & 0xffffffffull));
    u32(static_cast<std::uint32_t>((value >> 32) & 0xffffffffull));
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::bytes(std::span<const std::byte> value) {
    if (!status_.ok()) {
        return;
    }
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::digest(const Digest& value) {
    bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                     Digest::kBytes));
}

void ByteWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void ByteWriter::string(std::string_view value, std::uint32_t max_length) {
    if (!status_.ok()) {
        return;
    }
    if (value.size() > max_length) {
        fail(ErrorCode::LimitExceeded, "string exceeds the configured maximum length");
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

void ByteWriter::count(std::uint32_t value, std::uint32_t max_value) {
    if (!status_.ok()) {
        return;
    }
    if (value > max_value) {
        fail(ErrorCode::LimitExceeded, "declared element count exceeds the configured maximum");
        return;
    }
    u32(value);
}

void ByteWriter::patch_u32(std::size_t offset, std::uint32_t value) {
    if (!status_.ok() || offset + 4 > buffer_.size()) {
        fail(ErrorCode::InternalError, "patch offset outside the written buffer");
        return;
    }
    buffer_[offset] = static_cast<std::byte>(value & 0xffu);
    buffer_[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
    buffer_[offset + 2] = static_cast<std::byte>((value >> 16) & 0xffu);
    buffer_[offset + 3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

ByteReader::ByteReader(std::span<const std::byte> bytes, const Limits& limits,
                       ErrorCode truncation_code)
    : bytes_(bytes), limits_(limits), truncation_code_(truncation_code) {}

Status ByteReader::fail(ErrorCode code, std::string message) {
    if (status_.ok()) {
        status_ = Status(code, std::move(message));
    }
    return status_;
}

Result<std::span<const std::byte>> ByteReader::take(std::size_t length) {
    if (!status_.ok()) {
        return status_;
    }
    if (length > remaining()) {
        return Status(truncation_code_, "declared length exceeds the remaining bytes");
    }
    const std::span<const std::byte> slice = bytes_.subspan(offset_, length);
    offset_ += length;
    return slice;
}

Result<std::uint8_t> ByteReader::u8() {
    auto slice = take(1);
    if (!slice.ok()) {
        return slice.status();
    }
    return static_cast<std::uint8_t>(slice.value()[0]);
}

Result<std::uint16_t> ByteReader::u16() {
    auto slice = take(2);
    if (!slice.ok()) {
        return slice.status();
    }
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(slice.value()[0]) |
                                      (static_cast<std::uint16_t>(slice.value()[1]) << 8));
}

Result<std::uint32_t> ByteReader::u32() {
    auto slice = take(4);
    if (!slice.ok()) {
        return slice.status();
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(slice.value()[index]) << (8u * index);
    }
    return value;
}

Result<std::uint64_t> ByteReader::u64() {
    auto slice = take(8);
    if (!slice.ok()) {
        return slice.status();
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(slice.value()[index]) << (8u * index);
    }
    return value;
}

Result<std::int64_t> ByteReader::i64() {
    auto value = u64();
    if (!value.ok()) {
        return value.status();
    }
    return static_cast<std::int64_t>(value.value());
}

Result<bool> ByteReader::boolean() {
    auto value = u8();
    if (!value.ok()) {
        return value.status();
    }
    if (value.value() > 1u) {
        return Status(ErrorCode::PersistenceCorrupt, "boolean encoded with a value other than 0 or 1");
    }
    return value.value() == 1u;
}

Result<Digest> ByteReader::digest() {
    auto slice = take(Digest::kBytes);
    if (!slice.ok()) {
        return slice.status();
    }
    std::array<std::uint8_t, Digest::kBytes> bytes{};
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
        bytes[index] = static_cast<std::uint8_t>(slice.value()[index]);
    }
    return Digest(bytes);
}

Result<std::string> ByteReader::string(std::uint32_t max_length) {
    auto length = u32();
    if (!length.ok()) {
        return length.status();
    }
    if (length.value() > max_length) {
        return fail(ErrorCode::LimitExceeded, "declared string length exceeds the configured maximum");
    }
    auto slice = take(length.value());
    if (!slice.ok()) {
        return slice.status();
    }
    return std::string(reinterpret_cast<const char*>(slice.value().data()), slice.value().size());
}

Result<std::uint32_t> ByteReader::count(std::uint32_t max_value) {
    auto value = u32();
    if (!value.ok()) {
        return value.status();
    }
    if (value.value() > max_value) {
        return fail(ErrorCode::LimitExceeded, "declared element count exceeds the configured maximum");
    }
    return value.value();
}

Result<std::span<const std::byte>> ByteReader::bytes(std::size_t length) { return take(length); }

Result<std::uint64_t> ByteReader::identity_value(IdentityDomain expected) {
    auto domain = u8();
    if (!domain.ok()) {
        return domain.status();
    }
    if (static_cast<IdentityDomain>(domain.value()) != expected) {
        return fail(ErrorCode::InvalidIdentity,
                    "identity domain tag does not match the requested domain");
    }
    return u64();
}

Result<std::uint64_t> ByteReader::optional_identity_value(IdentityDomain expected) {
    auto domain = u8();
    if (!domain.ok()) {
        return domain.status();
    }
    if (static_cast<IdentityDomain>(domain.value()) != expected) {
        return fail(ErrorCode::InvalidIdentity,
                    "identity domain tag does not match the requested domain");
    }
    return u64();
}

Result<std::uint32_t> ByteReader::generation_value(IdentityDomain expected) {
    auto domain = u8();
    if (!domain.ok()) {
        return domain.status();
    }
    if (static_cast<IdentityDomain>(domain.value()) != expected) {
        return fail(ErrorCode::InvalidIdentity,
                    "generation domain tag does not match the requested domain");
    }
    return u32();
}

Status ByteReader::expect_consumed(std::string_view what) {
    if (!status_.ok()) {
        return status_;
    }
    if (offset_ != bytes_.size()) {
        return fail(ErrorCode::ProtocolError,
                    std::string("trailing bytes after ") + std::string(what));
    }
    return Status{};
}

void write_strong_id(ByteWriter& writer, IdentityDomain domain, std::uint64_t value) {
    writer.u8(static_cast<std::uint8_t>(domain));
    writer.u64(value);
}

void write_generation(ByteWriter& writer, IdentityDomain domain, std::uint32_t value) {
    writer.u8(static_cast<std::uint8_t>(domain));
    writer.u32(value);
}

}  // namespace research_ledger
