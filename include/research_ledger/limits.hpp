#pragma once

#include <cstdint>

namespace research_ledger {

// Every externally controlled surface is bounded here once, so that the codec,
// the ledger, the persistence layer, the protocol and the tests agree on the
// same bounds. Nothing in the runtime reserves memory from an untrusted count
// before checking it against one of these limits.
struct Limits {
    // Text and metadata.
    std::uint32_t max_string_length = 512;
    std::uint32_t max_label_length = 128;
    std::uint32_t max_explanation_length = 1024;
    std::uint32_t max_failure_message_length = 512;
    std::uint32_t max_location_length = 512;
    std::uint32_t max_metadata_entries = 16;
    std::uint32_t max_metadata_key_length = 64;
    std::uint32_t max_metadata_value_length = 256;

    // Entity relationships.
    std::uint32_t max_hypotheses_per_experiment = 8;
    std::uint32_t max_inputs = 64;
    std::uint32_t max_expected_outputs = 64;
    std::uint32_t max_parents_per_artifact = 16;
    std::uint32_t max_evidence_refs_per_decision = 256;
    std::uint32_t max_lineage_depth = 512;
    std::uint32_t max_lineage_nodes = 200000;
    std::uint32_t max_artifacts_per_result = 64;
    std::uint32_t max_observations_per_result = 256;
    std::uint32_t max_experiments_per_result = 64;
    std::uint32_t max_calls_per_attempt = 4096;
    std::uint32_t max_attempts_per_experiment = 4096;
    std::uint32_t max_query_results = 65536;

    // Ledger.
    std::uint32_t max_records_per_batch = 256;
    std::uint64_t max_records = 4000000;
    std::uint32_t max_explanation_lines = 4096;

    // Transport.
    std::uint32_t max_frame_size = 1u << 20;  // 1 MiB
    std::uint32_t max_records_per_append_frame = 64;
    std::uint32_t max_worker_connections = 64;
    std::uint32_t max_workers = 256;
    std::uint32_t max_inflight_requests_per_worker = 256;

    // Persistence.
    std::uint32_t max_persistence_records = 4000000;
    std::uint64_t max_persistence_bytes = 512ull << 20;  // 512 MiB
};

}  // namespace research_ledger
