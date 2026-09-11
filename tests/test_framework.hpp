#pragma once

// Minimal deterministic test framework. It has no timeouts, no watchdogs and no
// way to skip work silently: a test either runs and reports, or it fails loudly.
//
// Checks go through functions rather than through an inline if-statement so
// that a constant condition (a compile-time property of the identity model, for
// instance) is checked without tripping the compiler's constant-condition
// warning. Nothing is suppressed.

#include <cstdint>
#include <string>
#include <vector>

namespace research_ledger {
namespace test {

struct TestCase {
    const char* name;
    void (*function)();
};

std::vector<TestCase>& registry();
int& failure_count();
int& check_count();
std::string& current_test();
std::vector<std::string>& failure_messages();

// Thrown after a failure is recorded. It aborts the failing test through a
// controlled reporting path instead of letting the test continue into
// undefined behaviour (dereferencing a failed result, for instance).
struct TestFailure {
    const char* what = "";
};

void register_test(const char* name, void (*function)());
void report_failure(const char* file, int line, const std::string& message);
void check(bool condition, const char* expression, const char* file, int line);
void check_message(bool condition, std::string message, const char* file, int line);

// Deterministic pseudo random generator: property tests print their seed and
// reproduce exactly from it.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

    std::uint64_t next() noexcept {
        state_ += 0x9e3779b97f4a7c15ull;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    }

    std::uint64_t range(std::uint64_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

    std::uint32_t u32(std::uint32_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(range(bound));
    }

    bool chance(unsigned numerator, unsigned denominator) noexcept {
        return denominator != 0 && u32(denominator) < numerator;
    }

    [[nodiscard]] std::uint64_t seed() const noexcept { return state_; }

private:
    std::uint64_t state_ = 0;
};

int run_all(int argc, char** argv);

}  // namespace test
}  // namespace research_ledger

#define RL_TEST(NAME)                                                                        \
    static void NAME();                                                                      \
    static const bool NAME##_registered = []() {                                             \
        ::research_ledger::test::register_test(#NAME, &NAME);                                \
        return true;                                                                         \
    }();                                                                                     \
    static void NAME()

#define RL_CHECK(CONDITION)                                                                  \
    ::research_ledger::test::check(static_cast<bool>(CONDITION), #CONDITION, __FILE__,       \
                                   __LINE__)

#define RL_CHECK_EQ(ACTUAL, EXPECTED)                                                        \
    do {                                                                                     \
        const auto rl_actual_value = (ACTUAL);                                               \
        const auto rl_expected_value = (EXPECTED);                                           \
        ::research_ledger::test::check(rl_actual_value == rl_expected_value,                 \
                                       #ACTUAL " != " #EXPECTED, __FILE__, __LINE__);        \
    } while (false)

#define RL_CHECK_MESSAGE(CONDITION, MESSAGE)                                                 \
    ::research_ledger::test::check_message(static_cast<bool>(CONDITION),                     \
                                           std::string(#CONDITION " :: ") + (MESSAGE),       \
                                           __FILE__, __LINE__)

#define RL_CHECK_OK(EXPRESSION)                                                              \
    do {                                                                                     \
        const auto rl_result_value = (EXPRESSION);                                           \
        ::research_ledger::test::check_message(                                              \
            rl_result_value.ok(),                                                            \
            std::string(#EXPRESSION " failed: ") +                                           \
                std::string(::research_ledger::error_code_name(rl_result_value.code())) +    \
                " (" + rl_result_value.message() + ")",                                      \
            __FILE__, __LINE__);                                                             \
    } while (false)

#define RL_CHECK_CODE(EXPRESSION, EXPECTED_CODE)                                             \
    do {                                                                                     \
        const auto rl_result_value = (EXPRESSION);                                           \
        ::research_ledger::test::check_message(                                              \
            rl_result_value.code() == (EXPECTED_CODE),                                       \
            std::string(#EXPRESSION " produced ") +                                          \
                std::string(::research_ledger::error_code_name(rl_result_value.code())) +    \
                " (" + rl_result_value.message() + "), expected " +                          \
                std::string(::research_ledger::error_code_name(EXPECTED_CODE)),              \
            __FILE__, __LINE__);                                                             \
    } while (false)
