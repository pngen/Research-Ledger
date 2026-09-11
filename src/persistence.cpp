#include "research_ledger/persistence.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include "ledger_state.hpp"
#include "research_ledger/codec.hpp"
#include "research_ledger/record_codec.hpp"
#include "research_ledger/version.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace research_ledger {
namespace {

inline constexpr std::size_t kHeaderSize = 76;

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

Result<std::vector<std::byte>> read_file(const std::string& path, const Limits& limits) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return Status(ErrorCode::IoFailure, "cannot open snapshot file for reading: " + path);
    }
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return Status(ErrorCode::IoFailure, "cannot determine snapshot file size: " + path);
    }
    if (static_cast<std::uint64_t>(size) > limits.max_persistence_bytes) {
        return Status(ErrorCode::PayloadTooLarge, "snapshot file exceeds the configured maximum size");
    }
    if (static_cast<std::uint64_t>(size) < kHeaderSize + Digest::kBytes) {
        return Status(ErrorCode::PersistenceTruncated, "snapshot file is shorter than its header");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        return Status(ErrorCode::IoFailure, "cannot read snapshot file: " + path);
    }
    return bytes;
}

Status write_temp_and_replace(const std::string& path, std::span<const std::byte> image) {
    const std::string temporary =
        path + ".tmp" + std::to_string(static_cast<unsigned long long>(current_process_id()));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return Status(ErrorCode::IoFailure, "cannot open temporary snapshot file for writing");
        }
        stream.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::remove(temporary.c_str());
            return Status(ErrorCode::IoFailure, "cannot write temporary snapshot file");
        }
        stream.close();
        if (!stream) {
            std::remove(temporary.c_str());
            return Status(ErrorCode::IoFailure, "cannot close temporary snapshot file");
        }
    }
#ifdef _WIN32
    if (!MoveFileExA(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(temporary.c_str());
        return Status(ErrorCode::IoFailure, "atomic replacement of the snapshot failed");
    }
#else
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return Status(ErrorCode::IoFailure, "atomic replacement of the snapshot failed");
    }
#endif
    return Status{};
}

}  // namespace

Result<std::vector<std::byte>> serialize_snapshot(std::span<const Record> records,
                                                  LedgerGeneration generation,
                                                  CoordinatorEpoch epoch, const Limits& limits) {
    if (records.size() > limits.max_persistence_records) {
        return Status(ErrorCode::PayloadTooLarge,
                      "snapshot record count exceeds the configured maximum");
    }
    ByteWriter body(records.size() * 128);
    for (const Record& record : records) {
        auto encoded = encode_record(record, limits);
        if (!encoded.ok()) {
            return encoded.status();
        }
        if (encoded.value().size() > limits.max_persistence_bytes) {
            return Status(ErrorCode::PayloadTooLarge, "encoded record exceeds the configured maximum");
        }
        body.u32(static_cast<std::uint32_t>(encoded.value().size()));
        body.bytes(encoded.value());
        if (!body.ok()) {
            return body.status();
        }
    }
    const std::vector<std::byte> body_bytes = body.take();
    if (body_bytes.size() > limits.max_persistence_bytes) {
        return Status(ErrorCode::PayloadTooLarge, "snapshot body exceeds the configured maximum size");
    }

    ByteWriter header(kHeaderSize);
    header.bytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(kPersistenceMagic), sizeof(kPersistenceMagic)));
    header.u32(kPersistenceFormatVersion);
    header.u32(kVersionMajor);
    header.u32(kVersionMinor);
    header.u32(kVersionPatch);
    header.u32(generation.value());
    header.u32(epoch.value());
    header.u32(static_cast<std::uint32_t>(records.size()));
    header.u64(static_cast<std::uint64_t>(body_bytes.size()));
    if (!header.ok() || header.size() != kHeaderSize - Digest::kBytes) {
        return Status(ErrorCode::InternalError, "snapshot header layout is inconsistent");
    }
    const Digest header_digest = sha256(header.buffer());
    header.digest(header_digest);

    std::vector<std::byte> image = header.take();
    image.insert(image.end(), body_bytes.begin(), body_bytes.end());
    const Digest body_digest = sha256(body_bytes);
    image.insert(image.end(), reinterpret_cast<const std::byte*>(body_digest.data()),
                 reinterpret_cast<const std::byte*>(body_digest.data()) + Digest::kBytes);
    return image;
}

Result<LoadedSnapshot> deserialize_snapshot(std::span<const std::byte> image, const Limits& limits) {
    if (image.size() < kHeaderSize + Digest::kBytes) {
        return Status(ErrorCode::PersistenceTruncated, "snapshot image is shorter than its header");
    }
    if (image.size() > limits.max_persistence_bytes) {
        return Status(ErrorCode::PayloadTooLarge, "snapshot image exceeds the configured maximum size");
    }
    ByteReader reader(image, limits, ErrorCode::PersistenceTruncated);
    auto magic = reader.bytes(sizeof(kPersistenceMagic));
    if (!magic.ok()) {
        return magic.status();
    }
    if (std::memcmp(magic.value().data(), kPersistenceMagic, sizeof(kPersistenceMagic)) != 0) {
        return Status(ErrorCode::PersistenceCorrupt, "snapshot magic does not match");
    }
    auto format = reader.u32();
    if (!format.ok()) {
        return format.status();
    }
    if (format.value() != kPersistenceFormatVersion) {
        return Status(ErrorCode::PersistenceUnsupportedVersion,
                      "snapshot format version is not supported by this runtime");
    }
    auto major = reader.u32();
    auto minor = reader.u32();
    auto patch = reader.u32();
    auto generation = reader.u32();
    auto epoch = reader.u32();
    auto record_count = reader.u32();
    auto body_length = reader.u64();
    if (!major.ok() || !minor.ok() || !patch.ok() || !generation.ok() || !epoch.ok() ||
        !record_count.ok() || !body_length.ok()) {
        return Status(ErrorCode::PersistenceTruncated, "snapshot header is truncated");
    }
    auto header_digest = reader.digest();
    if (!header_digest.ok()) {
        return header_digest.status();
    }
    if (!(sha256(image.subspan(0, kHeaderSize - Digest::kBytes)) == header_digest.value())) {
        return Status(ErrorCode::PersistenceCorrupt, "snapshot header digest does not match");
    }
    if (record_count.value() > limits.max_persistence_records) {
        return Status(ErrorCode::PayloadTooLarge,
                      "snapshot declares more records than the configured maximum");
    }
    const std::uint64_t available = static_cast<std::uint64_t>(image.size()) - kHeaderSize -
                                    Digest::kBytes;
    if (body_length.value() != available) {
        return Status(ErrorCode::PersistenceTruncated,
                      "snapshot body length does not match the file length");
    }
    const std::span<const std::byte> body = image.subspan(kHeaderSize, static_cast<std::size_t>(available));
    const std::span<const std::byte> stored_digest =
        image.subspan(image.size() - Digest::kBytes, Digest::kBytes);
    std::array<std::uint8_t, Digest::kBytes> digest_bytes{};
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
        digest_bytes[index] = static_cast<std::uint8_t>(stored_digest[index]);
    }
    const Digest computed = sha256(body);
    if (!(computed == Digest(digest_bytes))) {
        return Status(ErrorCode::PersistenceCorrupt, "snapshot body digest does not match");
    }
    if (generation.value() == 0 || epoch.value() == 0) {
        return Status(ErrorCode::PersistenceCorrupt, "snapshot generation or epoch is zero");
    }

    LoadedSnapshot snapshot;
    snapshot.generation = LedgerGeneration::from_value(generation.value());
    snapshot.epoch = CoordinatorEpoch::from_value(epoch.value());
    snapshot.body_digest = computed;
    snapshot.bytes = static_cast<std::uint64_t>(image.size());
    snapshot.records.reserve(record_count.value());

    ByteReader body_reader(body, limits, ErrorCode::PersistenceTruncated);
    Digest chain{};
    for (std::uint32_t index = 0; index < record_count.value(); ++index) {
        auto length = body_reader.u32();
        if (!length.ok()) {
            return length.status();
        }
        if (length.value() > limits.max_persistence_bytes) {
            return Status(ErrorCode::PayloadTooLarge, "record length exceeds the configured maximum");
        }
        auto encoded = body_reader.bytes(length.value());
        if (!encoded.ok()) {
            return encoded.status();
        }
        auto record = decode_record(encoded.value(), limits);
        if (!record.ok()) {
            return record.status();
        }
        if (record.value().header.sequence.value() != index + 1) {
            return Status(ErrorCode::PersistenceCorrupt, "snapshot record sequences are not contiguous");
        }
        if (!(record.value().header.authority.ledger == snapshot.generation)) {
            return Status(ErrorCode::PersistenceCorrupt,
                          "snapshot record belongs to a different ledger generation");
        }
        auto payload_digest = record_body_digest(record.value().body, limits);
        if (!payload_digest.ok() || !(payload_digest.value() == record.value().header.payload_digest)) {
            return Status(ErrorCode::PersistenceCorrupt,
                          "snapshot record payload digest does not match its payload");
        }
        const Digest expected_chain = next_chain_digest(chain, record.value().header);
        if (!(expected_chain == record.value().header.chain_digest)) {
            return Status(ErrorCode::IntegrityFailure, "snapshot record chain digest does not match");
        }
        chain = expected_chain;
        snapshot.records.push_back(record.take());
    }
    if (!body_reader.empty()) {
        return Status(ErrorCode::PersistenceCorrupt, "snapshot body has trailing bytes");
    }
    snapshot.chain_digest = chain;
    return snapshot;
}

Result<LoadedSnapshot> load_snapshot_file(const std::string& path, const Limits& limits) {
    auto image = read_file(path, limits);
    if (!image.ok()) {
        return image.status();
    }
    return deserialize_snapshot(image.value(), limits);
}

Status save_snapshot_file(const std::string& path, std::span<const Record> records,
                          LedgerGeneration generation, CoordinatorEpoch epoch, const Limits& limits) {
    if (path.empty()) {
        return Status(ErrorCode::InvalidArgument, "snapshot path must not be empty");
    }
    auto image = serialize_snapshot(records, generation, epoch, limits);
    if (!image.ok()) {
        return image.status();
    }
    // Validate the image before it can replace anything: a snapshot that does
    // not decode is never written over the last known-good file.
    auto decoded = deserialize_snapshot(image.value(), limits);
    if (!decoded.ok()) {
        return decoded.status();
    }
    if (decoded.value().records.size() != records.size()) {
        return Status(ErrorCode::InternalError, "snapshot round trip changed the record count");
    }
    return write_temp_and_replace(path, image.value());
}

}  // namespace research_ledger
