# Research Ledger

Research Ledger is an open-source, vendor-neutral C++20 runtime for authoritative
provenance, lineage, and accounting across autonomous research activity. It owns the
durable historical record of what a research process actually did: which hypotheses were
proposed, which experiments and branches addressed them, which attempts really ran, which
models and tools participated, which artifacts were produced or consumed, what was
observed, what failed, what was retried, which branches were abandoned, which results were
rejected, which result was accepted, and which decision authorized that acceptance.

A final result alone is insufficient. Research Ledger keeps enough structured evidence to
reconstruct the whole path, or to state exactly what is missing.

## Core systems question

What happened during this research process, which hypotheses, experiments, calls,
artifacts, observations, failures, and decisions produced each result, what resources were
consumed, and can every accepted result be reconstructed from authoritative evidence?

## Systems boundary

Research Ledger owns the durable historical research record and nothing else.

It owns: research-session identity; hypothesis identity and revision lineage; experiment
identity; branch identity; attempt identity; model-call identity; tool-call identity;
dataset and input references; artifact references; observations; metrics; measurements;
failures; retries; cancellations; decision records; acceptance, rejection, supersession,
invalidation and retraction records; result identity; lineage edges; provenance;
resource-consumption records; externally supplied cost records; authority metadata; append
ordering; immutable committed history; persistence; integrity; deterministic replay;
reconstruction; querying; and explanation.

It does not own: experiment scheduling; accelerator scheduling; model routing; model
execution; tool execution; autonomous hypothesis generation; experiment-control policy;
artifact payload storage; model storage; dataset storage; artifact promotion policy;
scientific truth determination; peer review; general distributed tracing; generic system
observability; or generic workflow execution.

### Adjacent runtimes

| Runtime | Owns | Relationship |
| --- | --- | --- |
| Experiment Fabric | autonomous experiment execution: hypotheses, branches, metrics, rollback, lineage during execution | Research Ledger records what those experiments actually did, durably |
| Lab Scheduler | scheduling experiments across models, GPUs, simulators, datasets and test environments | Research Ledger records the scheduler authority an attempt ran under; it does not schedule |
| Artifact Promotion | deciding whether an artifact becomes trusted enough to promote | consumes evidence from Research Ledger; Research Ledger stores the decision record |
| Autonomous Foundry | coordinating populations of autonomous workers, selection and retention | appends research history through Research Ledger |
| State Provenance | provenance of reusable computational state in general | Research Ledger is specifically the research-history ledger |
| Inference Ledger | exact inference-resource accounting | Research Ledger references externally supplied accounting; it does not re-implement it |
| Efficiency Ledger | classification of useful work, overhead, waste, stranded capacity | Research Ledger may link to that evidence; it does not duplicate the classification |
| Cost Governor | prospective and realized execution economics | Research Ledger stores research economic evidence and references authoritative external cost records |
| Artifact Fabric or another artifact store | artifact bytes | Research Ledger stores immutable identities, digests, metadata, references, relationships and provenance |

## Core doctrine

History is append-only, and the runtime separates mechanically what happened, what was
observed, what was inferred, what was decided and what was accepted.

A failed experiment remains in history. A rejected hypothesis remains in history. An
abandoned branch remains in history. A superseded result remains in history. A model call
that produced an unusable output remains in history. An artifact that was later invalidated
remains part of the lineage. Later conclusions are additional records — ACCEPTED, REJECTED,
SUPERSEDED, INVALIDATED, RETRACTED, RECLASSIFIED — never rewrites of the earlier
occurrence. An observation is not automatically a conclusion, a conclusion is not
automatically accepted, and an accepted result is not proof that every ancestor was
correct.

## Architecture

    include/research_ledger/       public headers (the whole API surface)
    src/                           implementation
    apps/                          coordinator, worker, multiprocess proof, inspection CLI
    examples/                      runnable examples
    benchmarks/                    measured benchmarks
    tests/                         unit, integration, property, adversarial and concurrency tests
    packaging/consumer/            independent downstream consumer of the installed package

Layers:

1. **Foundation** — strong identities (`identity.hpp`), typed errors (`error.hpp`),
   bounded limits (`limits.hpp`), SHA-256 digests (`digest.hpp`), explicit little-endian
   byte codecs (`codec.hpp`), bounded quantities and provenance (`quantity.hpp`,
   `provenance.hpp`).
2. **Model** — entities, payloads and views (`model.hpp`), the record stream
   (`record.hpp`), canonical record encoding (`record_codec.hpp`).
3. **Ledger** — commit path, validation, indexes, snapshots and queries
   (`ledger.hpp`), reconstruction (`evidence.hpp`), persistence (`persistence.hpp`),
   replay (`replay.hpp`).
4. **Reference deployment** — framed bounded protocol (`protocol.hpp`), loopback TCP
   transport (`net.hpp`), real process management (`process.hpp`), coordinator and worker
   (`cluster.hpp`).

## Identity and authority model

Identity domains are separate types. A `HypothesisId` cannot be passed where an
`ExperimentId` is expected, and a serialized identity carries its domain tag, so decoding
one domain as another is an explicit `INVALID_IDENTITY` failure rather than a plausible
value. Research session, hypothesis, hypothesis generation, experiment, experiment
generation, branch, attempt, attempt generation, model call, tool call, dataset, input,
artifact, artifact generation, observation, metric, failure, decision, result, result
generation, worker, worker boot, coordinator epoch, ledger generation, record sequence,
record identity and policy generation are all distinct domains.

Zero is the invalid value in every identity domain: it is never issued, and an identity
that decodes to zero is rejected. Generation zero is likewise never valid authority; the
first committed generation of an entity is generation one. Textual identity forms are
stable and carry the domain (`hypothesis:7`).

Every committed record carries an authority envelope: ledger generation, coordinator
epoch, worker identity and worker boot identity. Authority is never inferred from a
payload.

## Append-only history

A record is submitted, validated, and then committed — or it is rejected. Only committed
records are historical truth.

- `RecordSequence` is monotonic within one ledger history and is the ordering authority.
  Wall-clock timestamps are stored as evidence and are never used for ordering.
- Committed records are immutable. Each carries a digest of its canonical payload and a
  chain digest over the previous chain, its sequence, type, identity, commit timestamp,
  provenance and payload digest. `LedgerSnapshot::verify()` recomputes both.
- Multi-record operations commit atomically. Validation never mutates committed state: a
  batch either commits completely or leaves prior history exactly as it was.
- Record identities may be supplied by the caller. A repeat of a committed identity is
  rejected as `DUPLICATE_RECORD`, or recognized idempotently (`DUPLICATE` outcome)
  when the draft is explicitly idempotent, which is what makes replay-safe ingestion
  possible without double counting.
- Duplicate or replayed records never mutate totals twice.

## Research model

**ResearchSession** — the durable top-level scope: explicit creation, stable identity,
optional label and research question, metadata, and a committed closure state. Closing a
session never deletes history. A closed session accepts only a bounded post-hoc annotation
(`SessionAnnotation`); it never reopens, and it never silently accepts new research.

**Hypothesis** — identity, generation, claim, parent for revisions or forks, owning
session, status (`PROPOSED`, `ACTIVE`, `SUPPORTED`, `NOT_SUPPORTED`, `REJECTED`,
`SUPERSEDED`, `INCONCLUSIVE`), and revision lineage. Status changes produce a new
generation and require the next generation. Absence of supporting evidence is not
rejection, and experiment failure is not hypothesis rejection.

**Experiment** — identity and generation, one or more hypotheses, the branch it belongs to,
an optional parent experiment, an execution-environment reference, declared inputs, expected
outputs, and its attempt history. The runtime does not execute experiments; it records the
externally supplied authority they ran under.

**Branch** — a first-class research branch: root, fork, continuation, retry,
alternate-method, control, ablation, competing hypothesis, merged evidence. A branch may
fail, be abandoned, or remain unresolved without corrupting the rest of the ledger. Root
branches have no parent; every derived branch must name its parent.

**Attempt** — every execution attempt has explicit identity and a generation. A failed
attempt and a successful retry are distinct historical events. Terminal states are
exclusive: a duplicate terminal record is `DUPLICATE_COMPLETION`, a contradictory one is
`ALREADY_TERMINAL`, and a cancelled attempt can never later succeed. Failure evidence alone
does not terminate an attempt — the attempt's own terminal record does.

Because every ancestry edge must reference an entity that already exists, it always points
at a strictly smaller sequence: ancestry is acyclic by construction in a healthy ledger,
and `verify()` re-checks it.

## Provenance: model calls, tool calls, artifacts, observations

**Model calls** are first-class provenance: model identity, revision, provider,
configuration fingerprint, input and output digests, token counts where available, latency,
externally supplied cost, outcome, parent calls, authority. No secrets or credentials are
stored, full prompts and outputs are not required, and model identity is never inferred from
free-form output.

**Tool calls** are equally explicit: tool identity and version, request and output
references, state (`SUBMITTED`, `ACKNOWLEDGED`, `COMPLETED`, `FAILED`, `CANCELLED`,
`OUTCOME_UNKNOWN`), and accounting evidence. A tool acknowledgment is not completion, and
`OUTCOME_UNKNOWN` never decays into success or failure on its own.

**Artifact references** store identity, generation, content digest, role, media type,
producer (session, experiment, attempt, model call, tool call or artifact), external
location, parents and validation state — never payload bytes. One-to-many and many-to-one
lineage is supported, including chains such as
`dataset → preprocessing tool call → normalized dataset` and
`hypothesis → experiment → model call → generated code → benchmark → result`.

**Observations and metrics** are distinct from decisions. A metric is declared once per
session with a canonical key, a unit and a value kind; an observation carries a typed value
(signed or unsigned integer, decimal, duration, bytes, count, rate, ratio, boolean,
category or digest). Unit and value-kind compatibility is enforced, NaN and infinity are
rejected, and an unknown value stays unknown instead of becoming zero.

## Results and decisions

A result is a research conclusion with its supporting lineage, not merely an artifact. It
carries its session, hypotheses, experiments, artifacts, observations, calls, generation,
content digest, and a lifecycle: `CANDIDATE`, `ACCEPTED`, `REJECTED`, `SUPERSEDED`,
`INVALIDATED`, `RETRACTED`.

A status change requires an explicit committed `DecisionRecord` about that result, with a
consistent type and outcome, and the transition must be permitted by the model. Acceptance
cannot happen through accidental field mutation, a rejected result never becomes accepted
directly (reclassification by a decision, then a new acceptance decision, is required), and
supersession, invalidation and retraction all preserve the original records.

Decisions record the evidence they considered as `(subject, sequence)` pairs. The sequence
must be committed already, it must precede the decision, and the record at that sequence
must concern exactly the cited subject. Evidence added later cannot retroactively appear as
evidence an earlier decision considered — a property the integrity check re-verifies.

## Accounting

Accounting values are typed and checked. Model input and output tokens, model calls, tool
calls, accelerator time, CPU time, wall-clock time, storage bytes, transfer bytes, energy,
attempts, retries and failure overhead are separate measure types; monetary values are
exact integers in micro-units of a stated currency, never binary floating point.

Every measure is either known with provenance (`MEASURED`, `REPORTED`, `DERIVED`,
`ESTIMATED`, `SYNTHETIC`, `RECONSTRUCTED`) or explicitly unknown. Unknown never decays
into zero: an aggregate is complete only when every contributor is fully known, and an
aggregate with no contributors reports itself as empty rather than as a zero total.
Accumulation is checked — overflow is an `ACCOUNTING_OVERFLOW` failure that leaves the
accumulator untouched — and the provenance of a total is never stronger than its weakest
contributor.

Aggregation is available across attempt, experiment, branch, hypothesis, session and result
lineage. The scope of an aggregation is the subject plus everything the ledger records
beneath it, held in a set, so a record shared by several results is counted exactly once.

## Persistence

The snapshot format is versioned, checksummed, bounded and written atomically:

    magic           8 bytes  "RLEDGER1"
    format_version  u32      must equal 1
    runtime version u32 x3
    ledger_gen      u32
    epoch           u32
    record_count    u32      bounded by Limits::max_persistence_records
    body_length     u64      bounded by Limits::max_persistence_bytes
    header_digest   32 bytes SHA-256 over every preceding header byte
    body            record_count length-prefixed canonical records
    body_digest     32 bytes SHA-256 over the body

Saving serializes, validates the produced image by decoding it, writes a temporary file in
the same directory, flushes and closes it, and replaces the target atomically
(`MoveFileEx` with replace-existing on Windows, `rename` elsewhere). A failed save cannot
corrupt the last known-good snapshot.

Loading rejects wrong magic, an unsupported format version before any payload is parsed,
header or body digest mismatch, truncation, trailing bytes, discontinuous record sequences,
records from another ledger generation, payloads whose digest does not match, and chains
that do not recompute. Every declared count is checked against a bound before memory is
reserved.

## Deterministic replay

Restoring a ledger from committed records revalidates each record through the same
validation path used at commit time, recomputes its payload and chain digests, and rebuilds
indexes. Records may be presented in any order: they are ordered by sequence, so a permuted
delivery reconstructs the same state.

`logical_digest()` is a digest of reconstructed logical state, computed from a canonical
rendering of every entity, metric and accounting record sorted by type and identity. It does
not depend on insertion order or on arrival timing. Replaying the same committed bytes
twice produces the same logical digest, and the digest of a reloaded ledger matches the
digest of the ledger that wrote it.

## Distributed reference deployment

The reference deployment is a coordinator that owns the committed ledger and worker
processes that ingest records over loopback TCP with a bounded framed protocol.

Frame layout:

    magic             u32   "RLP1"
    protocol_version  u16   must match
    message_type      u16   must be known
    flags             u16   must be zero
    payload_length    u32   bounded by Limits::max_frame_size
    correlation_id    u64   echoed by the reply
    coordinator_epoch u32   authority claim of the sender
    worker            u64   authority claim of the sender
    worker_boot       u64   authority claim of the sender
    checksum          u32   CRC-32 over every preceding header byte
    payload           payload_length bytes

A frame is rejected when it is truncated, oversized, carries an unknown message type or
non-zero flags, declares a payload length that does not match the frame, fails its
checksum, or when a typed payload is not consumed exactly. Integers are decoded explicitly
and portably; a peer-controlled length never drives an allocation before it is bounded.

Authority in the reference deployment:

- A worker never invents its own boot identity. The coordinator mints one per admitted
  incarnation (`admit_worker_incarnation`), derived from the coordinator epoch and a
  counter, and returns it in the hello acknowledgement.
- Admitting a new incarnation of a worker fences the previous one. A connection that ends
  fences the incarnation it was admitted under, so process death removes authority without
  waiting for a replacement.
- Appends carry the epoch and incarnation; a fenced incarnation is rejected with
  `STALE_WORKER` and a superseded epoch with `STALE_EPOCH`, checked before any ledger
  state is touched.
- A restarted coordinator loads committed history and advances the coordinator epoch.
  Historical records keep their original authority as history — a worker that produced
  records yesterday is historical truth — but that does not authorize its old incarnation to
  append today. Every incarnation must re-handshake after a restart.

## Build

    cmake -S . -B build
    cmake --build build --config Release

Requirements: CMake 3.20 or newer and a C++20 compiler. On Windows the Visual Studio 2022
generator is used directly (`-G "Visual Studio 17 2022" -A x64`); the library links
`ws2_32` for the reference deployment's loopback transport. First-party code is compiled
with `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc` on MSVC and
`-Wall -Wextra -Wpedantic -Werror` elsewhere. There is no warning suppression list.

Build options:

    RESEARCH_LEDGER_BUILD_TESTS            ON
    RESEARCH_LEDGER_BUILD_EXAMPLES         ON
    RESEARCH_LEDGER_BUILD_BENCHMARKS       ON
    RESEARCH_LEDGER_BUILD_TOOLS            ON
    RESEARCH_LEDGER_ENABLE_ADDRESS_SANITIZER  OFF

## Tests

    ctest --test-dir build -C Release

The suite contains no timeouts, no watchdogs and no time-based success criteria: a test
either runs to completion and reports, or it fails. A failed check aborts the failing test
through the framework's reporting path rather than letting it continue into undefined
behaviour. Randomized property tests print their seed and reproduce exactly from it.

Test groups: identity and digest, ledger commit semantics, result lifecycle, accounting,
persistence, protocol framing, concurrency, property tests, adversarial hardening, and the
real multiprocess coordinator/worker proof.

## Examples

    build/Release/research-ledger-examples

## Inspection CLI

    research-ledger-cli --state session.rls <command>

Commands include `init`, `create-session`, `create-hypothesis`, `create-branch`,
`create-experiment`, `create-attempt`, `complete-attempt`, `fail-attempt`,
`create-result`, `accept`, `reject`, `inspect-session`, `inspect-hypothesis`,
`inspect-experiment`, `inspect-attempt`, `inspect-result`, `lineage`, `evidence`,
`explain`, `failures`, `decisions`, `accounting`, `verify`, `replay`, `digest`
and `stats`. The CLI uses the same production API as the library: it loads a real ledger,
appends real records and reports typed errors.

## Package installation and consumption

    cmake --install build --config Release --prefix <prefix>

Installs the headers, the library, the exported target `ResearchLedger::research_ledger`
and the CMake package configuration. An independent consumer outside the source tree:

    cmake -S packaging/consumer -B _consumer -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build _consumer --config Release
    _consumer/Release/research-ledger-consumer

## Reference deployment proof

`research-ledger-multiprocess-proof` starts real operating-system processes and drives the
whole boundary over framed TCP:

1. A worker process creates a research session and commits the main scenario, a failed
   branch (failure record plus a terminal failed attempt) and a successful retry branch.
2. A second worker process, with its own worker identity, accepts the retry result with an
   explicit decision and its status change committed in one atomic batch.
3. The first worker's process is killed with real OS termination. Its committed records
   remain, and the committed chain digest is unchanged.
4. A fresh incarnation of that worker is admitted with a **different** `WorkerBootId` and
   appends successfully.
5. A frame claiming the killed incarnation is rejected with `STALE_WORKER`, and a raw frame
   claiming the pre-restart coordinator epoch is rejected with `STALE_EPOCH`.
6. The coordinator process is killed and restarted on the same state file. The epoch
   advances, the committed history is intact, the logical digest is unchanged, a fresh
   worker appends under the new epoch, and the accepted result still reconstructs to the
   same lineage.
7. After a graceful shutdown the proof reloads the persisted snapshot: integrity verifies,
   the accepted result reconstructs with the failed attempt still in history, and its cited
   evidence resolves to the sequences the loaded ledger reports.

Run it directly, or through `ctest`, which passes the tool directory automatically:

    build/Release/research-ledger-multiprocess-proof --tools build/Release

## Measured performance

`research-ledger-bench` measures completed operations through the public API on
committed state. The figures below are from one local synthetic run on one machine (no
network, no hardware counters): Windows x64, MSVC 19.44, Release, single process. Every
measured operation is checked for success, and each benchmark warms up before its timed
loop.

| Operation | Scale | Time | Throughput |
| --- | --- | --- | --- |
| single-record append | 10,000 records | 3.36 µs/record | 297,351 records/s |
| batched append (256 per batch) | 10,000 records | 3.10 µs/record | 323,038 records/s |
| indexed point lookup | 20,000-record ledger | 0.12 µs/lookup | 8,668,892 lookups/s |
| hypothesis ancestry | chain depth 500 | 18.56 µs | 53,878 reconstructions/s |
| branch ancestry | chain depth 500 | 20.32 µs | 49,205 reconstructions/s |
| artifact ancestry | chain depth 1000 | 178.56 µs | 5,600 traversals/s |
| artifact descendants | fan-out 1000 | 177.54 µs | 5,633 traversals/s |
| accepted-result reconstruction | 42-record history, 24 closure nodes | 27.32 µs | 36,599 reconstructions/s |
| result explanation | 36 explanation lines | 35.67 µs | 28,034 explanations/s |
| accounting aggregation | 220 contributions | 40.22 µs | 24,863 aggregations/s |
| snapshot save | 20,000 records, 3,970,108 bytes | 59.93 ms | 16.7 saves/s |
| snapshot load | 20,000 records, 3,970,108 bytes | 129.18 ms | 7.7 loads/s |
| replay with integrity re-verification | 20,000 records | 5.02 µs/record | 199,144 records/s |
| logical state digest | 20,000-record ledger | 10.31 ms | 97.0 digests/s |

Appends, point lookups, ancestry queries and aggregation are index-backed: no operation
scans the whole ledger. Snapshot save and load are linear in the number of committed
records, and load revalidates every payload digest and chain link, which is what makes a
corrupt or tampered image a rejection rather than a silent success.

## Limitations

- Research Ledger owns the historical record only. It does not schedule, execute, route
  models, store artifact bytes, promote artifacts, evaluate scientific truth, or perform
  general distributed tracing.
- The reference deployment is single-node and loopback-only: one coordinator process owns
  one committed ledger. There is no clustering, consensus, replication or failover, and no
  transport authentication or encryption — the trust boundary is the coordinator's own
  machine.
- The coordinator persists a full snapshot image after each committed batch, so ingestion
  cost per batch is linear in the committed history. An append-only log with periodic
  compaction is the production shape; the snapshot format here favours verifiability.
- Persistence format 1 accepts only its own layout. A change that makes an older payload
  ambiguous bumps the version, and older versions are rejected deterministically.
- Accounting reconciles externally supplied or directly observable values. Energy,
  accelerator time and monetary cost are recorded only when they are supplied; they are
  never inferred from unrelated evidence.
- Research Ledger records accelerator use as referenced evidence. It performs no GPU work
  of its own and claims no accelerator validation.
- AddressSanitizer validation is genuine for Windows x64 with MSVC 19.44: a deliberate
  sanitizer self-test (registered as a test whose pass criterion is the sanitizer report)
  proves the instrumentation is active, and the complete 96-test unit, integration,
  property, adversarial and concurrency suite runs clean under it. The instrumented
  multiprocess proof was not observed to completion in the sanitizer configuration in this
  environment, so no sanitizer result is claimed for it. No other sanitizer result is
  claimed, and no sanitizer result is claimed for platforms that were not exercised.
- The Unix code paths are written and compile-guarded but were not exercised in the
  released validation, which was performed on Windows x64.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
