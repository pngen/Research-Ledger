#pragma once

// Argument parsing and deterministic rendering helpers for research-ledger-cli.
//
// The CLI never reinterprets a value across identity domains: every argument
// that names an entity is parsed against the domain its command position
// requires, so a hypothesis identity can never be accepted where an artifact
// identity is required. Output is derived from committed state only: nothing
// printed by the CLI depends on wall-clock time, address layout or iteration
// order of a hash container.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "research_ledger/error.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/quantity.hpp"

namespace research_ledger {
namespace cli {

inline constexpr int kExitSuccess = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;

// One parsed invocation: the option, the command and its operands.
struct Invocation {
    bool help = false;
    std::string state_path{};
    std::string command{};
    std::vector<std::string> operands{};
};

inline std::string_view text_of(const char* argument) noexcept {
    return argument == nullptr ? std::string_view{} : std::string_view(argument);
}

// A decimal identity value. Zero is never issued by the runtime, so it is not
// accepted as an argument either.
inline std::optional<std::uint64_t> parse_decimal(std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (0xffffffffffffffffull - digit) / 10ull) {
            return std::nullopt;
        }
        value = value * 10ull + digit;
    }
    if (value == kInvalidIdentityValue) {
        return std::nullopt;
    }
    return value;
}

// Accepts "<domain>:<value>" or a bare "<value>". The expected domain comes
// from the command position, never from the shape of the text.
inline std::optional<std::uint64_t> parse_identity_argument(std::string_view text,
                                                           std::string_view domain) {
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos) {
        return parse_decimal(text);
    }
    if (text.substr(0, separator) != domain) {
        return std::nullopt;
    }
    return parse_decimal(text.substr(separator + 1));
}

// Parses "<option> [--state <file>] <command> [arguments...]". Returns false
// when the invocation cannot be used; "problem" then describes why.
inline bool parse_invocation(int argc, char** argv, Invocation& invocation, std::string& problem) {
    invocation = Invocation{};
    problem.clear();
    bool state_seen = false;
    int index = 1;
    for (; index < argc; ++index) {
        const std::string_view argument = text_of(argv[index]);
        if (argument == "--help" || argument == "-h") {
            invocation.help = true;
            return true;
        }
        if (argument == "--state") {
            if (state_seen) {
                problem = "--state was given more than once";
                return false;
            }
            if (index + 1 >= argc) {
                problem = "--state requires a file path";
                return false;
            }
            ++index;
            invocation.state_path = std::string(text_of(argv[index]));
            if (invocation.state_path.empty()) {
                problem = "--state requires a file path";
                return false;
            }
            state_seen = true;
            continue;
        }
        if (argument.size() > 8 && argument.substr(0, 8) == "--state=") {
            if (state_seen) {
                problem = "--state was given more than once";
                return false;
            }
            invocation.state_path = std::string(argument.substr(8));
            state_seen = true;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            problem = "unknown option: " + std::string(argument);
            return false;
        }
        break;
    }
    if (index >= argc) {
        problem = "no command was given";
        return false;
    }
    invocation.command = std::string(text_of(argv[index]));
    for (++index; index < argc; ++index) {
        invocation.operands.emplace_back(text_of(argv[index]));
    }
    if (invocation.state_path.empty()) {
        problem = "--state <file> is required";
        return false;
    }
    return true;
}

// Checks the operand count against the shape the command documents.
inline bool require_operands(std::string_view command, std::string_view shape,
                             const std::vector<std::string>& operands, std::size_t minimum,
                             std::size_t maximum, std::string& problem) {
    if (operands.size() < minimum || operands.size() > maximum) {
        problem = std::string(command) + " expects " + std::string(shape);
        return false;
    }
    return true;
}

// Joins operands [from, size) with single spaces; used for free text such as a
// hypothesis claim or a failure message.
inline std::string join_operands(const std::vector<std::string>& operands, std::size_t from) {
    std::string text;
    for (std::size_t index = from; index < operands.size(); ++index) {
        if (!text.empty()) {
            text += ' ';
        }
        text += operands[index];
    }
    return text;
}

inline std::string unstated(const std::string& text) {
    return text.empty() ? std::string("(unstated)") : text;
}

inline const char* boolean_text(bool value) noexcept { return value ? "true" : "false"; }

// A measure is rendered with its provenance, or as UNKNOWN. Unknown is never
// rendered as zero.
template <class Tag>
inline std::string measure_text(const Measure<Tag>& measure) {
    if (!measure.is_known()) {
        return "UNKNOWN";
    }
    std::string text = std::to_string(measure.units());
    text += " (";
    text += provenance_name(measure.provenance());
    text += ")";
    return text;
}

inline std::string monetary_text(const MonetaryMeasure& measure) {
    if (!measure.known) {
        return "UNKNOWN";
    }
    std::string text = std::to_string(measure.micro_units);
    text += " micro-";
    text += measure.currency;
    text += " (";
    text += provenance_name(measure.provenance);
    text += ")";
    return text;
}

// A list of identities, comma separated, or "(none)".
template <class Id>
inline std::string identity_list_text(const std::vector<Id>& values) {
    if (values.empty()) {
        return "(none)";
    }
    std::string text;
    for (const Id& value : values) {
        if (!text.empty()) {
            text += ',';
        }
        text += to_string(value);
    }
    return text;
}

template <class Id>
inline std::string optional_identity_text(const std::optional<Id>& value) {
    return value.has_value() ? to_string(*value) : std::string("none");
}

inline const char* usage_text() noexcept {
    return
        "research-ledger-cli --state <file> <command> [arguments]\n"
        "\n"
        "The state file is a Research Ledger snapshot. A mutating command loads it,\n"
        "appends real records through the public API in one atomic batch and writes the\n"
        "snapshot back; a read-only command reports committed history and writes nothing.\n"
        "\n"
        "Identities are accepted as <value> or as <domain>:<value>, for example \"1\" or\n"
        "\"research-session:1\". The domain a command position expects is fixed by that\n"
        "position and is never guessed from the text.\n"
        "\n"
        "Exit codes\n"
        "  0  the command completed\n"
        "  1  the command failed; \"ERROR <ERROR_CODE_NAME>: <detail>\" is written to stderr\n"
        "  2  the invocation was not understood; this usage is written to stderr\n"
        "\n"
        "Commands\n"
        "  init\n"
        "  create-session <id> [label] [question]\n"
        "  create-hypothesis <session> <id> <claim...>\n"
        "  create-branch <session> <id> [root|fork|retry|alternate-method|control|ablation] [parent]\n"
        "  create-experiment <session> <id> <hypothesis> <branch>\n"
        "  create-attempt <experiment> <id> <branch>\n"
        "  complete-attempt <attempt> [reference]\n"
        "  fail-attempt <attempt> <failure-id> <message...>\n"
        "  create-result <session> <id> <hypothesis> <experiment> [artifact-id]...\n"
        "  accept <result> <decision-id>\n"
        "  reject <result> <decision-id>\n"
        "  inspect-session <id>\n"
        "  inspect-hypothesis <id>\n"
        "  inspect-experiment <id>\n"
        "  inspect-attempt <id>\n"
        "  inspect-result <id>\n"
        "  lineage <hypothesis|branch|artifact> <id>\n"
        "  evidence <result>\n"
        "  explain <result>\n"
        "  failures\n"
        "  decisions <type> <id>\n"
        "  accounting <type> <id>\n"
        "  verify\n"
        "  replay\n"
        "  digest\n"
        "  stats\n"
        "\n"
        "<type> is one of session, hypothesis, experiment, branch, attempt, model-call,\n"
        "tool-call, artifact, observation, failure, decision, result.\n"
        "\n"
        "init refuses to overwrite a non-empty state file: remove the file to start a new\n"
        "ledger. accept and reject commit the decision and the result status change in one\n"
        "atomic batch, citing the committed (subject, sequence) pairs of the result's\n"
        "experiments, artifacts and hypotheses. fail-attempt records an EXECUTION failure\n"
        "scoped to the attempt and the attempt's terminal record in the same batch.\n";
}

}  // namespace cli
}  // namespace research_ledger
