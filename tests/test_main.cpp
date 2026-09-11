#include "test_framework.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace research_ledger {
namespace test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int& failure_count() {
    static int count = 0;
    return count;
}

int& check_count() {
    static int count = 0;
    return count;
}

std::string& current_test() {
    static std::string name;
    return name;
}

std::vector<std::string>& failure_messages() {
    static std::vector<std::string> messages;
    return messages;
}

void register_test(const char* name, void (*function)()) {
    registry().push_back(TestCase{name, function});
}

void report_failure(const char* file, int line, const std::string& message) {
    failure_count() += 1;
    std::string text = current_test();
    text += " (";
    text += file;
    text += ":";
    text += std::to_string(line);
    text += "): ";
    text += message;
    failure_messages().push_back(text);
    std::printf("FAIL %s\n", text.c_str());
    throw TestFailure{"check failed"};
}

void check(bool condition, const char* expression, const char* file, int line) {
    check_count() += 1;
    if (!condition) {
        report_failure(file, line, expression);
    }
}

void check_message(bool condition, std::string message, const char* file, int line) {
    check_count() += 1;
    if (!condition) {
        report_failure(file, line, message);
    }
}

int run_all(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int executed = 0;
    for (const TestCase& test : registry()) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
            continue;
        }
        current_test() = test.name;
        std::printf("RUN  %s\n", test.name);
        std::fflush(stdout);
        try {
            test.function();
        } catch (const TestFailure&) {
            std::printf("ABORT %s after a failed check\n", test.name);
        } catch (const std::exception& error) {
            report_failure(__FILE__, __LINE__,
                           std::string("unexpected exception: ") + error.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        executed += 1;
    }
    std::printf("\n%d test(s) executed, %d check(s), %d failure(s)\n", executed, check_count(),
                failure_count());
    if (failure_count() != 0) {
        std::printf("\nfailures:\n");
        for (const std::string& message : failure_messages()) {
            std::printf("  %s\n", message.c_str());
        }
        return 1;
    }
    return executed == 0 ? 2 : 0;
}

}  // namespace test
}  // namespace research_ledger

int main(int argc, char** argv) { return research_ledger::test::run_all(argc, argv); }
