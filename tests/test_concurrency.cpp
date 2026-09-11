#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/cluster.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/process.hpp"
#include "research_ledger/replay.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

// Every query is read through this helper so that a failed query is reported
// through the framework instead of being dereferenced.
template <class T>
inline T expect(Result<T> result, const char* what) {
    RL_CHECK_MESSAGE(result.ok(), std::string(what) + ": " +
                                      std::string(error_code_name(result.code())) + " (" +
                                      result.message() + ")");
    return result.take();
}

// A Status is not a Result: it is reported through the same controlled path.
inline void require_ok(const Status& status, const char* what) {
    RL_CHECK_MESSAGE(status.ok(), std::string(what) + ": " +
                                      std::string(error_code_name(status.code)) + " (" +
                                      status.message + ")");
}

inline std::string temp_path(const char* name) {
    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) {
        root = std::filesystem::path(".");
    }
    root /= "rlagent";
    std::filesystem::create_directories(root, error);
    root /= name;
    return root.string();
}

inline bool same_file_bytes(const std::string& lhs, const std::string& rhs) {
    std::ifstream left(lhs, std::ios::binary);
    std::ifstream right(rhs, std::ios::binary);
    if (!left || !right) {
        return false;
    }
    const std::string first((std::istreambuf_iterator<char>(left)),
                            std::istreambuf_iterator<char>());
    const std::string second((std::istreambuf_iterator<char>(right)),
                             std::istreambuf_iterator<char>());
    return !first.empty() && first == second;
}

// --- disjoint writers -------------------------------------------------------

RL_TEST(concurrency_disjoint_appends_commit_or_fail_with_a_typed_error) {
    constexpr unsigned kThreads = 8;
    constexpr unsigned kAppendsPerThread = 200;

    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    RL_CHECK_OK(book->append(session_opened(1)));

    std::vector<std::vector<RecordSequence>> sequences(kThreads);
    std::vector<std::uint64_t> successes(kThreads, 0);
    std::vector<std::uint64_t> refusals(kThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (unsigned index = 0; index < kThreads; ++index) {
        threads.emplace_back([&book, &sequences, &successes, &refusals, index]() {
            sequences[index].reserve(kAppendsPerThread);
            for (unsigned attempt = 0; attempt < kAppendsPerThread; ++attempt) {
                const std::uint64_t identity =
                    static_cast<std::uint64_t>(index) * kAppendsPerThread + attempt + 1;
                auto outcome = book->append(hypothesis_declared(identity, 1, "disjoint claim"));
                if (!outcome.ok()) {
                    refusals[index] += 1;
                    continue;
                }
                if (outcome.value().state == CommitState::Committed) {
                    sequences[index].push_back(outcome.value().sequence);
                    successes[index] += 1;
                } else {
                    refusals[index] += 1;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::uint64_t committed = 0;
    std::uint64_t refused = 0;
    std::vector<RecordSequence> all;
    for (unsigned index = 0; index < kThreads; ++index) {
        committed += successes[index];
        refused += refusals[index];
        all.insert(all.end(), sequences[index].begin(), sequences[index].end());
    }
    std::printf("concurrency_disjoint: threads=%u appends=%u committed=%llu refused=%llu\n",
                kThreads, kThreads * kAppendsPerThread,
                static_cast<unsigned long long>(committed),
                static_cast<unsigned long long>(refused));

    // Every append committed: disjoint writers cannot refuse each other.
    RL_CHECK_EQ(committed, static_cast<std::uint64_t>(kThreads) * kAppendsPerThread);
    RL_CHECK_EQ(refused, 0u);
    RL_CHECK_EQ(all.size(), committed);

    // Sequences are unique and contiguous, with no gap left by a writer.
    std::sort(all.begin(), all.end());
    for (std::size_t index = 0; index < all.size(); ++index) {
        RL_CHECK_EQ(all[index].value(), index + 2);
    }
    RL_CHECK_EQ(book->last_sequence().value(), committed + 1);
    const LedgerStats stats = expect(book->snapshot().stats(), "stats");
    RL_CHECK_EQ(stats.hypotheses, committed);
    RL_CHECK_EQ(stats.count_of(RecordType::HypothesisDeclared), committed);
    const IntegrityReport integrity = expect(book->snapshot().verify(), "verify");
    RL_CHECK(integrity.ok);
    RL_CHECK_EQ(integrity.issues.size(), 0u);
}

// --- readers while writers append -------------------------------------------

RL_TEST(concurrency_readers_never_observe_a_partial_batch) {
    constexpr unsigned kWriters = 4;
    constexpr unsigned kBatchesPerWriter = 60;
    constexpr std::uint64_t kPairs = kWriters * kBatchesPerWriter;
    constexpr unsigned kReaders = 4;
    constexpr unsigned kReaderIterations = 200;

    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    RL_CHECK_OK(book->append(session_opened(1)));
    RL_CHECK_OK(book->append(hypothesis_declared(1, 1, "shared claim")));
    RL_CHECK_OK(book->append(branch_declared(1, 1, BranchKind::Root)));

    struct ReaderReport {
        std::uint64_t iterations = 0;
        std::uint64_t queries = 0;
        std::uint64_t partial_batches = 0;
        std::uint64_t unstable_watermarks = 0;
        std::uint64_t unexpected_codes = 0;
        std::uint64_t future_visibility = 0;
        RecordSequence first_watermark{};
        RecordSequence last_watermark{};
    };

    const auto branch_of = [](std::uint64_t index) {
        return BranchId::from_value(100 + index);
    };
    const auto experiment_of = [](std::uint64_t index) {
        return ExperimentId::from_value(100 + index);
    };

    std::vector<std::uint64_t> writer_refusals(kWriters, 0);
    std::vector<ReaderReport> reports(kReaders);
    std::vector<std::thread> threads;

    for (unsigned writer = 0; writer < kWriters; ++writer) {
        threads.emplace_back([&book, &writer_refusals, writer]() {
            for (unsigned batch = 0; batch < kBatchesPerWriter; ++batch) {
                const std::uint64_t index = static_cast<std::uint64_t>(writer) * kBatchesPerWriter +
                                            batch;
                std::vector<RecordDraft> drafts{
                    branch_declared(100 + index, 1, BranchKind::Fork, 1),
                    experiment_declared(100 + index, 1, 1, 100 + index)};
                auto outcomes = book->append_batch(drafts);
                if (!outcomes.ok()) {
                    writer_refusals[writer] += 1;
                }
            }
        });
    }
    for (unsigned reader = 0; reader < kReaders; ++reader) {
        threads.emplace_back([&book, &reports, reader, &branch_of, &experiment_of]() {
            ReaderReport& report = reports[reader];
            std::uint64_t cursor = reader;
            for (unsigned iteration = 0; iteration < kReaderIterations; ++iteration) {
                report.iterations += 1;
                auto snapshot = book->snapshot();
                const RecordSequence watermark = snapshot.watermark();
                if (!report.first_watermark.valid()) {
                    report.first_watermark = watermark;
                }
                report.last_watermark = watermark;
                for (unsigned step = 0; step < 16; ++step) {
                    const std::uint64_t index = (cursor + step * 13) % kPairs;
                    auto experiment = snapshot.experiment(experiment_of(index));
                    auto branch = snapshot.branch(branch_of(index));
                    report.queries += 2;
                    if (!experiment.ok() && experiment.code() != ErrorCode::NotFound) {
                        report.unexpected_codes += 1;
                    }
                    if (!branch.ok() && branch.code() != ErrorCode::NotFound) {
                        report.unexpected_codes += 1;
                    }
                    if (experiment.ok()) {
                        if (experiment.value().declared_at.value() > watermark.value()) {
                            report.future_visibility += 1;
                        }
                        // A batch is all or nothing: the experiment of a batch is
                        // never visible without the branch committed with it.
                        if (!branch.ok()) {
                            report.partial_batches += 1;
                        } else if (!(branch.value().declared_at < experiment.value().declared_at)) {
                            report.partial_batches += 1;
                        }
                    }
                }
                if (!(snapshot.watermark() == watermark)) {
                    report.unstable_watermarks += 1;
                }
                cursor += 1;
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::uint64_t refused : writer_refusals) {
        RL_CHECK_EQ(refused, 0u);
    }

    std::uint64_t iterations = 0;
    std::uint64_t queries = 0;
    std::uint64_t readers_that_saw_growth = 0;
    for (const ReaderReport& report : reports) {
        iterations += report.iterations;
        queries += report.queries;
        RL_CHECK_EQ(report.partial_batches, 0u);
        RL_CHECK_EQ(report.unstable_watermarks, 0u);
        RL_CHECK_EQ(report.unexpected_codes, 0u);
        RL_CHECK_EQ(report.future_visibility, 0u);
        RL_CHECK_EQ(report.iterations, kReaderIterations);
        // The readers really ran while the ledger was being extended.
        if (report.last_watermark > report.first_watermark) {
            readers_that_saw_growth += 1;
        }
    }
    RL_CHECK(readers_that_saw_growth > 0u);
    std::printf("concurrency_readers: writers=%u readers=%u pairs=%llu reader-iterations=%llu "
                "queries=%llu readers-observing-growth=%llu\n",
                kWriters, kReaders, static_cast<unsigned long long>(kPairs),
                static_cast<unsigned long long>(iterations),
                static_cast<unsigned long long>(queries),
                static_cast<unsigned long long>(readers_that_saw_growth));

    // After the writers stop every pair is committed together.
    auto snapshot = book->snapshot();
    std::uint64_t visible = 0;
    for (std::uint64_t index = 0; index < kPairs; ++index) {
        const ExperimentView experiment = expect(snapshot.experiment(experiment_of(index)),
                                                 "experiment");
        const BranchView branch = expect(snapshot.branch(branch_of(index)), "branch");
        RL_CHECK(branch.declared_at < experiment.declared_at);
        visible += 1;
    }
    RL_CHECK_EQ(visible, kPairs);
    RL_CHECK_EQ(book->last_sequence().value(), 3u + kPairs * 2u);
    const IntegrityReport integrity = expect(book->snapshot().verify(), "verify");
    RL_CHECK(integrity.ok);
}

// --- persistence while writers append ---------------------------------------

RL_TEST(concurrency_save_reload_and_replay_during_appends) {
    constexpr unsigned kWriters = 3;
    constexpr unsigned kAppendsPerWriter = 150;
    constexpr unsigned kSaveRounds = 20;

    const std::string first_path = temp_path("concurrency-live.bin");
    const std::string second_path = temp_path("concurrency-reloaded.bin");
    std::remove(first_path.c_str());
    std::remove(second_path.c_str());

    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    RL_CHECK_OK(book->append(session_opened(1)));

    struct SaveReport {
        std::uint64_t rounds = 0;
        std::uint64_t records_seen = 0;
        std::uint64_t smallest_image = 0;
        std::uint64_t largest_image = 0;
        std::uint64_t save_failures = 0;
        std::uint64_t load_failures = 0;
        std::uint64_t record_failures = 0;
        std::uint64_t digest_mismatches = 0;
        std::uint64_t image_mismatches = 0;
        std::uint64_t integrity_failures = 0;
        std::uint64_t append_failures = 0;
        ErrorCode last_error = ErrorCode::Ok;
    };
    SaveReport save_report;
    std::vector<std::thread> threads;

    for (unsigned writer = 0; writer < kWriters; ++writer) {
        threads.emplace_back([&book, &save_report, writer]() {
            for (unsigned attempt = 0; attempt < kAppendsPerWriter; ++attempt) {
                const std::uint64_t identity =
                    static_cast<std::uint64_t>(writer) * kAppendsPerWriter + attempt + 1;
                auto outcome = book->append(hypothesis_declared(identity, 1, "written while saved"));
                if (!outcome.ok()) {
                    save_report.append_failures += 1;
                }
            }
        });
    }
    // The saver keeps working while the writers append: every image it writes
    // must load, replay to the same logical state and re-save identically.
    threads.emplace_back([&book, &save_report, &first_path, &second_path]() {
        for (unsigned round = 0; round < kSaveRounds; ++round) {
            save_report.rounds += 1;
            const Status saved = book->save(first_path);
            if (!saved.ok()) {
                save_report.save_failures += 1;
                save_report.last_error = saved.code;
                continue;
            }
            auto reloaded = Ledger::load(first_path, local_config());
            if (!reloaded.ok()) {
                save_report.load_failures += 1;
                save_report.last_error = reloaded.code();
                continue;
            }
            const std::shared_ptr<Ledger>& restored = reloaded.value();
            const RecordSequence last = restored->last_sequence();
            std::vector<Record> records;
            if (last.valid()) {
                auto range = restored->snapshot().records(RecordSequence::from_value(1), last);
                if (!range.ok()) {
                    save_report.record_failures += 1;
                    save_report.last_error = range.code();
                    continue;
                }
                records = range.take();
            }
            save_report.records_seen += records.size();
            if (save_report.rounds == 1u || records.size() < save_report.smallest_image) {
                save_report.smallest_image = records.size();
            }
            if (records.size() > save_report.largest_image) {
                save_report.largest_image = records.size();
            }
            auto replay = replay_committed(records, local_config());
            if (!replay.ok()) {
                save_report.digest_mismatches += 1;
                save_report.last_error = replay.code();
                continue;
            }
            if (!(replay.value().logical_digest == restored->logical_digest())) {
                save_report.digest_mismatches += 1;
            }
            auto integrity = restored->snapshot().verify();
            if (!integrity.ok() || !integrity.value().ok) {
                save_report.integrity_failures += 1;
            }
            // Saving the reloaded ledger again produces the same image.
            const Status resaved = restored->save(second_path);
            if (!resaved.ok() || !same_file_bytes(first_path, second_path)) {
                save_report.image_mismatches += 1;
            }
        }
    });

    for (std::thread& thread : threads) {
        thread.join();
    }

    std::printf("concurrency_persistence: writers=%u appends=%u save-rounds=%llu records=%llu "
                "image-size-range=%llu..%llu\n",
                kWriters, kWriters * kAppendsPerWriter,
                static_cast<unsigned long long>(save_report.rounds),
                static_cast<unsigned long long>(save_report.records_seen),
                static_cast<unsigned long long>(save_report.smallest_image),
                static_cast<unsigned long long>(save_report.largest_image));
    RL_CHECK_EQ(save_report.rounds, kSaveRounds);
    RL_CHECK_EQ(save_report.append_failures, 0u);
    RL_CHECK_EQ(save_report.save_failures, 0u);
    RL_CHECK_EQ(save_report.load_failures, 0u);
    RL_CHECK_EQ(save_report.record_failures, 0u);
    RL_CHECK_EQ(save_report.digest_mismatches, 0u);
    RL_CHECK_EQ(save_report.image_mismatches, 0u);
    RL_CHECK_EQ(save_report.integrity_failures, 0u);
    // The images grew while they were being written: the saver really overlapped
    // the appenders instead of running after them.
    RL_CHECK(save_report.largest_image > save_report.smallest_image);

    // The last image always loads and reproduces the live logical digest.
    const Status final_save = book->save(first_path);
    require_ok(final_save, "final save");
    auto final_load = Ledger::load(first_path, local_config());
    RL_CHECK_OK(final_load);
    RL_CHECK(final_load.value()->logical_digest() == book->logical_digest());
    const std::vector<Record> final_records =
        expect(final_load.value()->snapshot().records(RecordSequence::from_value(1),
                                                      final_load.value()->last_sequence()),
               "final records");
    auto final_replay = replay_committed(final_records, local_config());
    RL_CHECK_OK(final_replay);
    RL_CHECK(final_replay.value().logical_digest == book->logical_digest());
    RL_CHECK_EQ(final_load.value()->last_sequence().value(),
                static_cast<std::uint64_t>(1) + kWriters * kAppendsPerWriter);

    std::remove(first_path.c_str());
    std::remove(second_path.c_str());
}

// --- terminal races ---------------------------------------------------------

RL_TEST(concurrency_terminal_races_choose_exactly_one_winner) {
    constexpr unsigned kThreads = 12;

    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    std::vector<RecordDraft> setup{session_opened(1), hypothesis_declared(1, 1, "raced claim"),
                                   branch_declared(1, 1, BranchKind::Root),
                                   experiment_declared(1, 1, 1, 1), attempt_started(1, 1, 1)};
    RL_CHECK_OK(book->append_batch(setup));
    RL_CHECK_OK(book->append(failure_recorded(1, SubjectId::of(AttemptId::from_value(1)),
                                              FailureCategory::Execution, "raced failure")));

    std::vector<ErrorCode> codes(kThreads, ErrorCode::Ok);
    std::vector<bool> accepted(kThreads, false);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (unsigned index = 0; index < kThreads; ++index) {
        threads.emplace_back([&book, &codes, &accepted, index]() {
            const unsigned kind = index % 3u;
            Result<AppendOutcome> outcome =
                kind == 0u ? book->append(attempt_completed(1))
                           : (kind == 1u ? book->append(attempt_failed(1, 1))
                                         : book->append(attempt_cancelled(1, "raced cancellation")));
            codes[index] = outcome.code();
            accepted[index] = outcome.ok();
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    unsigned winners = 0;
    unsigned typed_losers = 0;
    for (unsigned index = 0; index < kThreads; ++index) {
        if (accepted[index]) {
            winners += 1;
            RL_CHECK(codes[index] == ErrorCode::Ok);
            continue;
        }
        typed_losers += 1;
        RL_CHECK(codes[index] == ErrorCode::AlreadyTerminal ||
                 codes[index] == ErrorCode::DuplicateCompletion ||
                 codes[index] == ErrorCode::Cancelled);
    }
    std::printf("concurrency_terminal_race: threads=%u winners=%u typed-losers=%u\n", kThreads,
                winners, typed_losers);
    RL_CHECK_EQ(winners, 1u);
    RL_CHECK_EQ(typed_losers, kThreads - 1u);

    auto snapshot = book->snapshot();
    const AttemptView attempt = expect(snapshot.attempt(AttemptId::from_value(1)), "attempt 1");
    RL_CHECK(attempt.state == AttemptState::Completed || attempt.state == AttemptState::Failed ||
             attempt.state == AttemptState::Cancelled);
    RL_CHECK(attempt.terminated_at.valid());
    RL_CHECK_EQ(attempt.generation.value(), 1u);
    // Exactly one terminal record was committed.
    const LedgerStats stats = expect(snapshot.stats(), "stats");
    const std::uint64_t terminal_records =
        stats.count_of(RecordType::AttemptCompleted) + stats.count_of(RecordType::AttemptFailed) +
        stats.count_of(RecordType::AttemptCancelled);
    RL_CHECK_EQ(terminal_records, 1u);
    const IntegrityReport integrity = expect(snapshot.verify(), "verify");
    RL_CHECK(integrity.ok);

    // The attempt stays terminal: a later contradictory transition is refused.
    const ErrorCode late = book->append(attempt_completed(1)).code();
    RL_CHECK(late == ErrorCode::AlreadyTerminal || late == ErrorCode::DuplicateCompletion ||
             late == ErrorCode::Cancelled);
}

// --- snapshot lifetime -------------------------------------------------------

RL_TEST(concurrency_snapshot_outlives_the_ledger) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    std::shared_ptr<Ledger> book = created.value();
    RL_CHECK_OK(book->append_batch(std::vector<RecordDraft>{
        session_opened(1), hypothesis_declared(1, 1, "handle lifetime"),
        branch_declared(1, 1, BranchKind::Root), experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1)}));

    std::vector<LedgerSnapshot> snapshots;
    snapshots.push_back(book->snapshot());
    snapshots.push_back(book->snapshot());
    const RecordSequence watermark = snapshots[0].watermark();
    const Digest chain = expect(snapshots[0].stats(), "stats").chain_digest;
    const Digest logical = book->logical_digest();

    // The ledger object is destroyed while snapshots are still alive.
    book.reset();

    for (const LedgerSnapshot& snapshot : snapshots) {
        RL_CHECK(snapshot.valid());
        RL_CHECK(snapshot.watermark() == watermark);
        const SessionView session = expect(snapshot.session(ResearchSessionId::from_value(1)),
                                           "session");
        RL_CHECK(session.state == SessionState::Open);
        const HypothesisView hypothesis = expect(snapshot.hypothesis(HypothesisId::from_value(1)),
                                                 "hypothesis");
        RL_CHECK_EQ(hypothesis.claim, std::string("handle lifetime"));
        const LedgerStats stats = expect(snapshot.stats(), "stats");
        RL_CHECK(stats.chain_digest == chain);
        const IntegrityReport integrity = expect(snapshot.verify(), "verify");
        RL_CHECK(integrity.ok);
        RL_CHECK_EQ(integrity.checked_records, watermark.value());
    }

    // Reading the same state through a fresh ledger reconstructs the same digest.
    auto restored = Ledger::restore(
        expect(snapshots[0].records(RecordSequence::from_value(1), watermark), "records"),
        local_config());
    RL_CHECK_OK(restored);
    RL_CHECK(restored.value()->logical_digest() == logical);
    snapshots.clear();
}

// --- in-process coordinator -------------------------------------------------

RL_TEST(concurrency_coordinator_accounts_for_every_committed_record) {
    constexpr unsigned kWorkers = 4;
    constexpr unsigned kAppendsPerWorker = 12;

    const std::string path = temp_path("concurrency-coordinator.bin");
    std::remove(path.c_str());

    CoordinatorConfig config;
    config.persistence_path = path;
    config.identity = "concurrency-test-coordinator";
    config.port = 0;
    std::unique_ptr<Coordinator> coordinator = expect(Coordinator::start(config),
                                                      "Coordinator::start");
    const std::uint16_t port = coordinator->port();
    RL_CHECK(port != 0);
    RL_CHECK(coordinator->epoch().valid());

    Status serve_status{};
    std::thread server([&coordinator, &serve_status]() { serve_status = coordinator->serve(); });

    struct WorkerReport {
        std::uint64_t committed = 0;
        std::uint64_t refused = 0;
        ErrorCode last_error = ErrorCode::Ok;
    };
    std::vector<WorkerReport> reports(kWorkers);
    std::vector<std::unique_ptr<WorkerClient>> clients(kWorkers);
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (unsigned index = 0; index < kWorkers; ++index) {
        threads.emplace_back([&reports, &clients, port, index]() {
            WorkerConfig worker;
            worker.port = port;
            worker.worker = WorkerId::from_value(index + 1);
            worker.authority = "worker";
            auto connected = WorkerClient::connect(worker);
            if (!connected.ok()) {
                reports[index].refused += 1;
                reports[index].last_error = connected.code();
                return;
            }
            clients[index] = connected.take();
            for (unsigned attempt = 0; attempt < kAppendsPerWorker; ++attempt) {
                const std::uint64_t identity =
                    static_cast<std::uint64_t>(index) * kAppendsPerWorker + attempt + 1;
                auto outcomes =
                    clients[index]->append(std::vector<RecordDraft>{session_opened(identity)});
                if (!outcomes.ok()) {
                    reports[index].refused += 1;
                    reports[index].last_error = outcomes.code();
                    continue;
                }
                for (const AppendOutcome& outcome : outcomes.value()) {
                    if (outcome.state == CommitState::Committed) {
                        reports[index].committed += 1;
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::uint64_t committed = 0;
    std::uint64_t refused = 0;
    for (const WorkerReport& report : reports) {
        committed += report.committed;
        refused += report.refused;
        RL_CHECK(report.committed == kAppendsPerWorker);
        RL_CHECK(report.refused == 0u);
    }
    std::printf("concurrency_coordinator: workers=%u appends-per-worker=%u committed=%llu "
                "refused=%llu port=%u\n",
                kWorkers, kAppendsPerWorker, static_cast<unsigned long long>(committed),
                static_cast<unsigned long long>(refused), static_cast<unsigned>(port));
    RL_CHECK_EQ(committed, static_cast<std::uint64_t>(kWorkers) * kAppendsPerWorker);
    RL_CHECK_EQ(coordinator->ledger()->last_sequence().value(), committed);

    auto snapshot = coordinator->ledger()->snapshot();
    const LedgerStats stats = expect(snapshot.stats(), "stats");
    RL_CHECK_EQ(stats.sessions, committed);
    RL_CHECK_EQ(stats.count_of(RecordType::SessionOpened), committed);
    RL_CHECK(stats.live_workers >= kWorkers);
    const IntegrityReport integrity = expect(snapshot.verify(), "verify");
    RL_CHECK(integrity.ok);
    // Every worker incarnation is accounted for.
    const std::vector<WorkerAuthorityView> workers = expect(coordinator->ledger()->workers(),
                                                            "workers");
    RL_CHECK(workers.size() >= kWorkers);

    // The coordinator shuts down cleanly while the clients are still connected.
    require_ok(coordinator->request_shutdown("the test is complete"), "request_shutdown");
    server.join();
    RL_CHECK(serve_status.ok());

    // A client that is still open observes the shutdown as a typed error rather
    // than hanging.
    for (unsigned index = 0; index < kWorkers; ++index) {
        if (clients[index] == nullptr) {
            continue;
        }
        auto after_shutdown = clients[index]->append(std::vector<RecordDraft>{session_opened(9000)});
        RL_CHECK(!after_shutdown.ok());
        RL_CHECK(after_shutdown.code() != ErrorCode::Ok);
        clients[index]->close();
    }
    RL_CHECK_EQ(coordinator->ledger()->last_sequence().value(), committed);

    std::remove(path.c_str());
    const std::string temp_file = path + ".tmp" + std::to_string(current_process_id());
    std::remove(temp_file.c_str());
}

}  // namespace
}  // namespace research_ledger
