#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/codec.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/record_codec.hpp"
#include "research_ledger/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

// Offsets of the snapshot layout documented in research_ledger/persistence.hpp.
//   magic 0..7, format 8..11, major 12..15, minor 16..19, patch 20..23,
//   generation 24..27, epoch 28..31, record_count 32..35, body_length 36..43,
//   header_digest 44..75, body 76..N-32, body_digest N-32..N
inline constexpr std::size_t kSnapshotFormatOffset = 8;
inline constexpr std::size_t kSnapshotGenerationOffset = 24;
inline constexpr std::size_t kSnapshotEpochOffset = 28;
inline constexpr std::size_t kSnapshotRecordCountOffset = 32;
inline constexpr std::size_t kSnapshotBodyLengthOffset = 36;
inline constexpr std::size_t kSnapshotHeaderDigestOffset = 44;
inline constexpr std::size_t kSnapshotHeaderBytes = 76;

// Status (as opposed to Result) needs its own reporting wrapper: it exposes the
// code as a field, not as code(). The expression is evaluated exactly once.
#define RL_REQUIRE_STATUS(EXPRESSION)                                                        \
    do {                                                                                     \
        const ::research_ledger::Status rl_status_value = (EXPRESSION);                       \
        ::research_ledger::test::check_message(                                              \
            rl_status_value.ok(),                                                            \
            std::string(#EXPRESSION " failed: ") +                                           \
                std::string(::research_ledger::error_code_name(rl_status_value.code)) +      \
                " (" + rl_status_value.message + ")",                                        \
            __FILE__, __LINE__);                                                             \
    } while (false)

// A per-test temporary directory that is removed when the test ends, however it
// ends: a failing check throws and unwinding still cleans the directory up.
class TempDirectory {
public:
    explicit TempDirectory(const char* name) {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        path_ = error ? std::filesystem::path(name) : base / name;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        created_ = std::filesystem::exists(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] bool created() const noexcept { return created_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (path_ / name).string();
    }
    [[nodiscard]] std::size_t entry_count() const {
        std::error_code error;
        std::size_t count = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(path_, error)) {
            static_cast<void>(entry);
            count += 1;
        }
        return count;
    }

private:
    std::filesystem::path path_{};
    bool created_ = false;
};

void put_u32(std::vector<std::byte>& image, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        image[offset + index] =
            static_cast<std::byte>((value >> (8u * static_cast<unsigned>(index))) & 0xffu);
    }
}

void put_u64(std::vector<std::byte>& image, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        image[offset + index] =
            static_cast<std::byte>((value >> (8u * static_cast<unsigned>(index))) & 0xffu);
    }
}

// Recomputes the header digest so that a mutated header field is the only thing
// wrong with the image: the test then measures the field check, not the digest.
void refresh_header_digest(std::vector<std::byte>& image) {
    const Digest digest =
        sha256(std::span<const std::byte>(image.data(), kSnapshotHeaderDigestOffset));
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
        image[kSnapshotHeaderDigestOffset + index] = static_cast<std::byte>(digest.bytes()[index]);
    }
}

Result<std::vector<Record>> all_records(const std::shared_ptr<Ledger>& ledger) {
    const RecordSequence last = ledger->last_sequence();
    if (!last.valid()) {
        return std::vector<Record>{};
    }
    return ledger->snapshot().records(RecordSequence::first(), last);
}

Result<std::vector<std::byte>> image_of(const std::shared_ptr<Ledger>& ledger,
                                        const Limits& limits) {
    auto records = all_records(ledger);
    if (!records.ok()) {
        return records.status();
    }
    return serialize_snapshot(records.value(), ledger->generation(), ledger->epoch(), limits);
}

bool same_records(const std::vector<Record>& lhs, const std::vector<Record>& rhs,
                  const Limits& limits) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        auto left = encode_record(lhs[index], limits);
        auto right = encode_record(rhs[index], limits);
        if (!left.ok() || !right.ok() || !(left.value() == right.value())) {
            return false;
        }
    }
    return true;
}

RL_TEST(persistence_round_trip_preserves_committed_history) {
    TempDirectory directory("research-ledger-tests-1");
    RL_CHECK(directory.created());
    const std::string path = directory.file("ledger.rls");

    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    const std::shared_ptr<Ledger>& original = history.value().ledger;
    const std::uint64_t last = original->last_sequence().value();
    RL_CHECK(last > 0u);

    auto original_records = all_records(original);
    RL_CHECK_OK(original_records);
    auto original_stats = original->snapshot().stats();
    RL_CHECK_OK(original_stats);
    const Digest original_chain = original_stats.value().chain_digest;
    const Digest original_logical = original->logical_digest();
    RL_CHECK(!original_chain.is_zero());
    RL_CHECK(!original_logical.is_zero());

    RL_REQUIRE_STATUS(original->save(path));
    RL_CHECK(std::filesystem::exists(path));
    RL_CHECK(std::filesystem::file_size(path) > kSnapshotHeaderBytes);

    Limits limits;
    auto loaded = Ledger::load(path, local_config());
    RL_CHECK_OK(loaded);
    const std::shared_ptr<Ledger>& restored = loaded.value();

    RL_CHECK(restored->generation() == original->generation());
    RL_CHECK(restored->epoch() == original->epoch());
    RL_CHECK_EQ(restored->last_sequence().value(), last);

    auto restored_records = all_records(restored);
    RL_CHECK_OK(restored_records);
    RL_CHECK(same_records(original_records.value(), restored_records.value(), limits));

    auto restored_stats = restored->snapshot().stats();
    RL_CHECK_OK(restored_stats);
    RL_CHECK(restored_stats.value().chain_digest == original_chain);
    RL_CHECK_EQ(restored_stats.value().sessions, original_stats.value().sessions);
    RL_CHECK_EQ(restored_stats.value().hypotheses, original_stats.value().hypotheses);
    RL_CHECK_EQ(restored_stats.value().experiments, original_stats.value().experiments);
    RL_CHECK_EQ(restored_stats.value().attempts, original_stats.value().attempts);
    RL_CHECK_EQ(restored_stats.value().artifacts, original_stats.value().artifacts);
    RL_CHECK_EQ(restored_stats.value().observations, original_stats.value().observations);
    RL_CHECK_EQ(restored_stats.value().decisions, original_stats.value().decisions);
    RL_CHECK_EQ(restored_stats.value().results, original_stats.value().results);
    RL_CHECK(restored->logical_digest() == original_logical);

    auto integrity = restored->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
    RL_CHECK_EQ(integrity.value().checked_records, last);

    // The reconstructed history is still queryable as history.
    auto result = restored->snapshot().result(history.value().result);
    RL_CHECK_OK(result);
    RL_CHECK(result.value().status == ResultStatus::Accepted);
    auto bundle = restored->snapshot().supporting_evidence(history.value().result);
    RL_CHECK_OK(bundle);
    RL_CHECK(bundle.value().complete);
    RL_CHECK(bundle.value().reconstructable);

    // Saving the reloaded ledger again writes an identical image.
    auto first_image = image_of(original, limits);
    RL_CHECK_OK(first_image);
    auto second_image = image_of(restored, limits);
    RL_CHECK_OK(second_image);
    RL_CHECK(first_image.value() == second_image.value());
    RL_CHECK_EQ(directory.entry_count(), 1u);
}

RL_TEST(persistence_snapshot_image_round_trip) {
    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    const std::shared_ptr<Ledger>& ledger = history.value().ledger;
    Limits limits;

    auto records = all_records(ledger);
    RL_CHECK_OK(records);
    auto stats = ledger->snapshot().stats();
    RL_CHECK_OK(stats);

    auto image = serialize_snapshot(records.value(), ledger->generation(), ledger->epoch(), limits);
    RL_CHECK_OK(image);
    RL_CHECK(image.value().size() > kSnapshotHeaderBytes + Digest::kBytes);

    auto decoded = deserialize_snapshot(image.value(), limits);
    RL_CHECK_OK(decoded);
    RL_CHECK(decoded.value().generation == ledger->generation());
    RL_CHECK(decoded.value().epoch == ledger->epoch());
    RL_CHECK_EQ(decoded.value().records.size(), records.value().size());
    RL_CHECK_EQ(decoded.value().bytes, image.value().size());
    RL_CHECK(decoded.value().chain_digest == stats.value().chain_digest);
    RL_CHECK(!decoded.value().body_digest.is_zero());
    RL_CHECK(same_records(records.value(), decoded.value().records, limits));

    // The encoding is canonical: re-encoding decoded records reproduces the same
    // image byte for byte.
    auto again = serialize_snapshot(decoded.value().records, decoded.value().generation,
                                    decoded.value().epoch, limits);
    RL_CHECK_OK(again);
    RL_CHECK(again.value() == image.value());

    // Records recovered from an image are the records that restore accepts.
    auto restored = Ledger::restore(decoded.value().records, local_config());
    RL_CHECK_OK(restored);
    RL_CHECK_EQ(restored.value()->last_sequence().value(), records.value().size());
    RL_CHECK(restored.value()->logical_digest() == ledger->logical_digest());

    // An empty history is a valid image and round trips as empty.
    auto empty =
        serialize_snapshot(std::span<const Record>{}, ledger->generation(), ledger->epoch(), limits);
    RL_CHECK_OK(empty);
    RL_CHECK_EQ(empty.value().size(), kSnapshotHeaderBytes + Digest::kBytes);
    auto empty_decoded = deserialize_snapshot(empty.value(), limits);
    RL_CHECK_OK(empty_decoded);
    RL_CHECK(empty_decoded.value().records.empty());
    RL_CHECK(empty_decoded.value().chain_digest.is_zero());
}

RL_TEST(persistence_corrupt_images_are_rejected) {
    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    Limits limits;
    auto image = image_of(history.value().ledger, limits);
    RL_CHECK_OK(image);
    const std::size_t available = image.value().size() - kSnapshotHeaderBytes - Digest::kBytes;
    RL_CHECK(available > 0u);

    // A flipped byte in the header is caught by the header digest.
    {
        std::vector<std::byte> broken = image.value();
        broken[12] ^= std::byte{0x40};
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // A flipped byte in the record body is caught by the body digest.
    {
        std::vector<std::byte> broken = image.value();
        broken[kSnapshotHeaderBytes + 3] ^= std::byte{0x01};
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // A flipped byte in the stored header digest.
    {
        std::vector<std::byte> broken = image.value();
        broken[kSnapshotHeaderDigestOffset + 5] ^= std::byte{0x80};
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // A flipped byte in the stored body digest.
    {
        std::vector<std::byte> broken = image.value();
        broken[broken.size() - Digest::kBytes + 7] ^= std::byte{0x08};
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // An absurd record count is rejected against the configured maximum.
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotRecordCountOffset, 0xffffffffu);
        refresh_header_digest(broken);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PayloadTooLarge);
    }
    // One record more than the body holds is truncated, not invented.
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotRecordCountOffset,
                static_cast<std::uint32_t>(history.value().ledger->last_sequence().value() + 1u));
        refresh_header_digest(broken);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceTruncated);
    }
    // An absurd body length does not match the file.
    {
        std::vector<std::byte> broken = image.value();
        put_u64(broken, kSnapshotBodyLengthOffset, static_cast<std::uint64_t>(available) + 4096u);
        refresh_header_digest(broken);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceTruncated);
    }
    // A zeroed body length with a non-empty body is likewise rejected.
    {
        std::vector<std::byte> broken = image.value();
        put_u64(broken, kSnapshotBodyLengthOffset, 0u);
        refresh_header_digest(broken);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceTruncated);
    }
    // A zeroed format version and an unknown one are both unsupported.
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotFormatOffset, 0u);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits),
                      ErrorCode::PersistenceUnsupportedVersion);
    }
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotFormatOffset, 2u);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits),
                      ErrorCode::PersistenceUnsupportedVersion);
    }
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotFormatOffset, 0xffffffffu);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits),
                      ErrorCode::PersistenceUnsupportedVersion);
    }
    // Wrong magic.
    {
        std::vector<std::byte> broken = image.value();
        broken[0] = std::byte{'X'};
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // A completely zeroed image is not an empty history.
    {
        std::vector<std::byte> broken(image.value().size(), std::byte{0});
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // A zero ledger generation or coordinator epoch is never valid.
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotGenerationOffset, 0u);
        refresh_header_digest(broken);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotEpochOffset, 0u);
        refresh_header_digest(broken);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    // Trailing garbage is not part of the snapshot.
    {
        std::vector<std::byte> broken = image.value();
        broken.push_back(std::byte{0x5a});
        RL_CHECK_CODE(deserialize_snapshot(broken, limits), ErrorCode::PersistenceTruncated);
    }
    // Every mutation above built its own copy: the original image still decodes.
    RL_CHECK_OK(deserialize_snapshot(image.value(), limits));
}

RL_TEST(persistence_truncation_at_every_offset_is_rejected) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RL_CHECK_OK(ledger.value()->append_batch(std::vector<RecordDraft>{
        session_opened(1), hypothesis_declared(1, 1, "hypothesis"), branch_declared(1, 1)}));
    Limits limits;
    auto image = image_of(ledger.value(), limits);
    RL_CHECK_OK(image);
    RL_CHECK(image.value().size() > kSnapshotHeaderBytes + Digest::kBytes);

    for (std::size_t length = 0; length < image.value().size(); ++length) {
        const std::span<const std::byte> prefix(image.value().data(), length);
        RL_CHECK_CODE(deserialize_snapshot(prefix, limits), ErrorCode::PersistenceTruncated);
    }
    // The whole image is accepted, so the loop above only ever removed bytes.
    RL_CHECK_OK(deserialize_snapshot(image.value(), limits));
}

RL_TEST(persistence_a_failed_save_leaves_the_last_good_file_intact) {
    TempDirectory directory("research-ledger-tests-4");
    RL_CHECK(directory.created());
    const std::string good = directory.file("ledger.rls");
    const std::string absent = (directory.path() / "absent-directory" / "ledger.rls").string();

    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    const std::shared_ptr<Ledger>& ledger = history.value().ledger;
    const std::uint64_t last = ledger->last_sequence().value();
    auto stats = ledger->snapshot().stats();
    RL_CHECK_OK(stats);

    RL_REQUIRE_STATUS(ledger->save(good));
    auto before = Ledger::load(good, local_config());
    RL_CHECK_OK(before);
    RL_CHECK_EQ(before.value()->last_sequence().value(), last);

    // A save into a directory that does not exist fails ...
    const Status failed = ledger->save(absent);
    RL_CHECK(!failed.ok());
    RL_CHECK(failed.code == ErrorCode::IoFailure);
    RL_CHECK(!std::filesystem::exists(absent));
    RL_CHECK_EQ(directory.entry_count(), 1u);

    // ... and the last known-good file is untouched.
    auto after = Ledger::load(good, local_config());
    RL_CHECK_OK(after);
    RL_CHECK_EQ(after.value()->last_sequence().value(), last);
    auto after_stats = after.value()->snapshot().stats();
    RL_CHECK_OK(after_stats);
    RL_CHECK(after_stats.value().chain_digest == stats.value().chain_digest);
    RL_CHECK(after.value()->logical_digest() == ledger->logical_digest());
    RL_CHECK_EQ(directory.entry_count(), 1u);

    // A snapshot file corrupted in place does not load, and the in-memory ledger
    // keeps its history.
    {
        std::vector<std::byte> bytes;
        std::ifstream input(good, std::ios::binary | std::ios::ate);
        RL_CHECK(input.good());
        const std::streamoff size = input.tellg();
        RL_CHECK(size > 0);
        bytes.resize(static_cast<std::size_t>(size));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()), size);
        input.close();
        RL_CHECK(bytes.size() > kSnapshotHeaderBytes + Digest::kBytes);
        bytes[kSnapshotHeaderBytes + 1] ^= std::byte{0x11};
        std::ofstream output(good, std::ios::binary | std::ios::trunc);
        RL_CHECK(output.good());
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        RL_CHECK(output.good());
    }
    auto corrupted = Ledger::load(good, local_config());
    RL_CHECK(!corrupted.ok());
    RL_CHECK(corrupted.code() == ErrorCode::PersistenceCorrupt);
    RL_CHECK_EQ(ledger->last_sequence().value(), last);
    auto still_intact = ledger->snapshot().verify();
    RL_CHECK_OK(still_intact);
    RL_CHECK(still_intact.value().ok);

    // Writing the ledger again repairs the file.
    RL_REQUIRE_STATUS(ledger->save(good));
    auto repaired = Ledger::load(good, local_config());
    RL_CHECK_OK(repaired);
    RL_CHECK_EQ(repaired.value()->last_sequence().value(), last);
    RL_CHECK_EQ(directory.entry_count(), 1u);
}

RL_TEST(persistence_history_is_not_live_authority) {
    TempDirectory directory("research-ledger-tests-6");
    RL_CHECK(directory.created());
    const std::string path = directory.file("ledger.rls");

    LedgerConfig writer_config;
    writer_config.require_worker_admission = true;
    auto created = Ledger::create(writer_config);
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& writer = created.value();

    const WorkerId worker = WorkerId::from_value(7);
    std::uint32_t counter = 1;
    auto admitted = writer->admit_worker_incarnation(worker, WorkerBootId{}, counter++);
    RL_CHECK_OK(admitted);
    const AuthorityEnvelope committed_by = admitted.value();
    RL_CHECK(committed_by.worker == worker);
    RL_CHECK(committed_by.worker_boot.valid());

    RL_CHECK_OK(writer->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(1), hypothesis_declared(1, 1, "h")}, committed_by));
    RL_REQUIRE_STATUS(writer->save(path));

    LedgerConfig reader_config;
    reader_config.require_worker_admission = true;
    auto loaded = Ledger::load(path, reader_config);
    RL_CHECK_OK(loaded);
    const std::shared_ptr<Ledger>& reader = loaded.value();

    // History is intact ...
    RL_CHECK_EQ(reader->last_sequence().value(), 2u);
    auto integrity = reader->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
    RL_CHECK_OK(reader->snapshot().session(ResearchSessionId::from_value(1)));
    RL_CHECK_OK(reader->snapshot().hypothesis(HypothesisId::from_value(1)));

    // ... but the worker that produced it is historical, not live.
    auto workers = reader->workers();
    RL_CHECK_OK(workers);
    bool found = false;
    for (const WorkerAuthorityView& view : workers.value()) {
        if (view.worker == worker) {
            found = true;
            RL_CHECK(!view.live);
            RL_CHECK_EQ(view.committed_records, 2u);
            RL_CHECK(view.boot == committed_by.worker_boot);
            RL_CHECK(view.epoch == committed_by.epoch);
        }
    }
    RL_CHECK(found);
    RL_CHECK(!reader->worker_is_live(worker, committed_by.worker_boot));

    // An append carrying the authority that wrote yesterday's records is
    // rejected, and it commits nothing.
    auto stale = reader->append_batch_with_authority(
        std::vector<RecordDraft>{hypothesis_declared(2, 1, "rewritten today")}, committed_by);
    RL_CHECK_CODE(stale, ErrorCode::StaleWorker);
    RL_CHECK_EQ(reader->last_sequence().value(), 2u);
    RL_CHECK_CODE(reader->snapshot().hypothesis(HypothesisId::from_value(2)), ErrorCode::NotFound);

    // A forged boot identity for the same worker is refused as well.
    AuthorityEnvelope forged = committed_by;
    forged.worker_boot = WorkerBootId::from_value(committed_by.worker_boot.value() + 1u);
    RL_CHECK_CODE(reader->append_batch_with_authority(
                      std::vector<RecordDraft>{hypothesis_declared(3, 1, "forged")}, forged),
                  ErrorCode::StaleWorker);

    // The restarted coordinator re-admits the worker as a new incarnation, and
    // only then may that worker append again.
    auto readmitted = reader->admit_worker_incarnation(worker, WorkerBootId{}, counter++);
    RL_CHECK_OK(readmitted);
    RL_CHECK(readmitted.value().worker_boot != committed_by.worker_boot);
    RL_CHECK_OK(reader->append_batch_with_authority(
        std::vector<RecordDraft>{hypothesis_declared(2, 1, "re-admitted today")},
        readmitted.value()));
    RL_CHECK_EQ(reader->last_sequence().value(), 3u);
    RL_CHECK_OK(reader->snapshot().hypothesis(HypothesisId::from_value(2)));
}

RL_TEST(persistence_restart_advances_the_epoch_and_keeps_history) {
    TempDirectory directory("research-ledger-tests-7");
    RL_CHECK(directory.created());
    const std::string path = directory.file("ledger.rls");

    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    const std::shared_ptr<Ledger>& first = history.value().ledger;
    const std::uint64_t last = first->last_sequence().value();
    const AuthorityEnvelope previous = first->local_authority();
    const CoordinatorEpoch previous_epoch = first->epoch();
    auto first_stats = first->snapshot().stats();
    RL_CHECK_OK(first_stats);
    RL_REQUIRE_STATUS(first->save(path));

    auto loaded = Ledger::load(path, local_config());
    RL_CHECK_OK(loaded);
    const std::shared_ptr<Ledger>& restarted = loaded.value();
    RL_CHECK(restarted->epoch() == previous_epoch);
    RL_CHECK_EQ(restarted->last_sequence().value(), last);

    auto advanced = restarted->advance_epoch();
    RL_CHECK_OK(advanced);
    RL_CHECK(advanced.value().value() > previous_epoch.value());
    RL_CHECK(restarted->epoch() == advanced.value());

    // Prior-epoch authority is rejected by epoch, before any worker bookkeeping.
    auto stale = restarted->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(2)}, previous);
    RL_CHECK_CODE(stale, ErrorCode::StaleEpoch);
    RL_CHECK_EQ(restarted->last_sequence().value(), last);

    // The history itself did not change.
    auto records = all_records(restarted);
    RL_CHECK_OK(records);
    RL_CHECK_EQ(records.value().size(), static_cast<std::size_t>(last));
    auto restarted_stats = restarted->snapshot().stats();
    RL_CHECK_OK(restarted_stats);
    RL_CHECK(restarted_stats.value().chain_digest == first_stats.value().chain_digest);
    RL_CHECK_EQ(restarted_stats.value().sessions, first_stats.value().sessions);
    RL_CHECK_EQ(restarted_stats.value().hypotheses, first_stats.value().hypotheses);
    RL_CHECK_EQ(restarted_stats.value().results, first_stats.value().results);
    RL_CHECK_EQ(restarted_stats.value().decisions, first_stats.value().decisions);
    auto integrity = restarted->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);

    // The restarted coordinator is the live authority and appends new records.
    const AuthorityEnvelope current = restarted->local_authority();
    RL_CHECK(current.epoch == advanced.value());
    RL_CHECK(restarted->worker_is_live(current.worker, current.worker_boot));
    RL_CHECK_OK(restarted->append(session_opened(2)));
    RL_CHECK_EQ(restarted->last_sequence().value(), last + 1u);

    RL_REQUIRE_STATUS(restarted->save(path));
    auto reloaded = Ledger::load(path, local_config());
    RL_CHECK_OK(reloaded);
    RL_CHECK_EQ(reloaded.value()->last_sequence().value(), last + 1u);
    RL_CHECK(reloaded.value()->epoch() == advanced.value());
    RL_CHECK_OK(reloaded.value()->snapshot().session(ResearchSessionId::from_value(2)));
}

RL_TEST(persistence_replay_is_deterministic_and_permutation_invariant) {
    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    const std::shared_ptr<Ledger>& ledger = history.value().ledger;

    auto records = all_records(ledger);
    RL_CHECK_OK(records);
    RL_CHECK(records.value().size() > 4u);

    auto first = replay_committed(records.value(), local_config());
    RL_CHECK_OK(first);
    auto second = replay_committed(records.value(), local_config());
    RL_CHECK_OK(second);
    RL_CHECK_EQ(first.value().records, records.value().size());
    RL_CHECK(first.value().logical_digest == second.value().logical_digest);
    RL_CHECK(first.value().chain_digest == second.value().chain_digest);
    RL_CHECK(!first.value().logical_digest.is_zero());
    RL_CHECK(first.value().integrity.ok);
    RL_CHECK_EQ(first.value().integrity.checked_records, records.value().size());
    RL_CHECK(first.value().logical_digest == ledger->logical_digest());
    RL_CHECK(first.value().last_sequence == ledger->last_sequence());

    // The same records delivered in a different order reconstruct the same
    // logical state and the same committed chain.
    std::vector<Record> permuted = records.value();
    std::reverse(permuted.begin(), permuted.end());
    auto replayed = replay_committed(permuted, local_config());
    RL_CHECK_OK(replayed);
    RL_CHECK(replayed.value().logical_digest == first.value().logical_digest);
    RL_CHECK(replayed.value().chain_digest == first.value().chain_digest);
    RL_CHECK(replayed.value().integrity.ok);

    // A tampered history never replays.
    {
        std::vector<Record> broken = records.value();
        broken[1].header.sequence = broken[0].header.sequence;
        RL_CHECK_CODE(replay_committed(broken, local_config()), ErrorCode::IntegrityFailure);
    }
    {
        std::vector<Record> broken = records.value();
        broken[2].header.payload_digest = sha256("tampered-payload");
        RL_CHECK_CODE(replay_committed(broken, local_config()), ErrorCode::IntegrityFailure);
    }
    {
        std::vector<Record> broken = records.value();
        broken[broken.size() - 1].header.chain_digest = sha256("broken-chain");
        RL_CHECK_CODE(replay_committed(broken, local_config()), ErrorCode::IntegrityFailure);
    }
    {
        std::vector<Record> broken = records.value();
        broken[1].header.record_id = broken[0].header.record_id;
        RL_CHECK_CODE(replay_committed(broken, local_config()), ErrorCode::DuplicateRecord);
    }
    {
        std::vector<Record> broken = records.value();
        broken.erase(broken.begin() + 3);
        RL_CHECK_CODE(replay_committed(broken, local_config()), ErrorCode::IntegrityFailure);
    }
    {
        std::vector<Record> broken = records.value();
        broken[0].header.authority.ledger = LedgerGeneration::from_value(9);
        RL_CHECK_CODE(replay_committed(broken, local_config()), ErrorCode::StaleGeneration);
    }
    // Replaying a prefix is replaying a shorter committed history.
    std::vector<Record> prefix(records.value().begin(), records.value().begin() + 5);
    auto shorter = replay_committed(prefix, local_config());
    RL_CHECK_OK(shorter);
    RL_CHECK_EQ(shorter.value().records, 5u);
    RL_CHECK(shorter.value().logical_digest != first.value().logical_digest);
}

}  // namespace
}  // namespace research_ledger
