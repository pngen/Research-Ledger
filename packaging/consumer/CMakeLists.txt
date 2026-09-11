cmake_minimum_required(VERSION 3.20)

project(ResearchLedgerConsumer
    VERSION 1.0.0
    DESCRIPTION "Independent downstream consumer of the installed Research Ledger package"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The consumer is deliberately outside the Research Ledger source tree: it may
# only use the installed package, its exported target and its installed headers.
find_package(ResearchLedger CONFIG REQUIRED)

add_executable(research-ledger-consumer main.cpp)
target_link_libraries(research-ledger-consumer PRIVATE ResearchLedger::research_ledger)
set_target_properties(research-ledger-consumer PROPERTIES WIN32_EXECUTABLE OFF)

if(MSVC)
    target_compile_options(research-ledger-consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
    target_compile_options(research-ledger-consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME research_ledger_consumer COMMAND research-ledger-consumer)
