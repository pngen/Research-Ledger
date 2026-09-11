cmake_minimum_required(VERSION 3.22)

project(ResearchLedger
    VERSION 1.0.0
    DESCRIPTION "Vendor neutral C++20 runtime for authoritative provenance, lineage and accounting across autonomous research activity"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RESEARCH_LEDGER_BUILD_TESTS "Build Research Ledger tests" ON)
option(RESEARCH_LEDGER_BUILD_EXAMPLES "Build Research Ledger examples" ON)
option(RESEARCH_LEDGER_BUILD_BENCHMARKS "Build Research Ledger benchmarks" ON)
option(RESEARCH_LEDGER_BUILD_TOOLS "Build the coordinator, worker, multiprocess proof and inspection CLI" ON)
option(RESEARCH_LEDGER_ENABLE_ADDRESS_SANITIZER "Build with AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Every first-party target is compiled with strict warnings and warnings as
# errors. There is no suppression list: a warning is a defect.
function(research_ledger_strict target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

if(RESEARCH_LEDGER_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        # An AddressSanitizer report is only useful with debug information, and
        # the toolchain warns (C5072) when it is missing. Emitting a program
        # database is the fix: nothing is suppressed.
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/INCREMENTAL:NO /DEBUG)
        # The AddressSanitizer runtime ships next to the compiler. The test
        # environment modification below puts it on PATH so that instrumented
        # binaries run from a plain shell as well as from a developer prompt.
        get_filename_component(RESEARCH_LEDGER_ASAN_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
    endif()
endif()

set(RESEARCH_LEDGER_SOURCES
    src/cluster.cpp
    src/codec.cpp
    src/digest.cpp
    src/error.cpp
    src/identity.cpp
    src/ledger.cpp
    src/ledger_aggregate.cpp
    src/ledger_commit.cpp
    src/ledger_query.cpp
    src/ledger_validate.cpp
    src/model.cpp
    src/net.cpp
    src/persistence.cpp
    src/process.cpp
    src/protocol.cpp
    src/quantity.cpp
    src/reconstruction.cpp
    src/record.cpp
    src/record_codec.cpp
    src/replay.cpp
)

add_library(research_ledger ${RESEARCH_LEDGER_SOURCES})
add_library(ResearchLedger::research_ledger ALIAS research_ledger)
target_include_directories(research_ledger
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(research_ledger PUBLIC cxx_std_20)
research_ledger_strict(research_ledger)
if(WIN32)
    target_link_libraries(research_ledger PUBLIC ws2_32)
endif()
set_target_properties(research_ledger PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

install(TARGETS research_ledger
    EXPORT ResearchLedgerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ResearchLedgerTargets
    FILE ResearchLedgerTargets.cmake
    NAMESPACE ResearchLedger::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResearchLedger)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ResearchLedgerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ResearchLedgerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResearchLedger)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ResearchLedgerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ResearchLedgerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ResearchLedgerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResearchLedger)

# --- reference deployment: coordinator, worker, multiprocess proof, CLI ------
if(RESEARCH_LEDGER_BUILD_TOOLS)
    foreach(tool coordinator worker multiprocess-proof cli)
        string(REPLACE "-" "_" tool_source "${tool}")
        add_executable(research-ledger-${tool} apps/${tool_source}_main.cpp)
        target_include_directories(research-ledger-${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
        target_link_libraries(research-ledger-${tool} PRIVATE ResearchLedger::research_ledger)
        research_ledger_strict(research-ledger-${tool})
        set_target_properties(research-ledger-${tool} PROPERTIES WIN32_EXECUTABLE OFF)
        install(TARGETS research-ledger-${tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endforeach()
endif()

# --- tests -------------------------------------------------------------------
if(RESEARCH_LEDGER_BUILD_TESTS)
    enable_testing()

    add_executable(research-ledger-tests
        tests/test_main.cpp
        tests/test_identity.cpp
        tests/test_ledger.cpp
        tests/test_result_lifecycle.cpp
        tests/test_accounting.cpp
        tests/test_persistence.cpp
        tests/test_protocol.cpp
        tests/test_concurrency.cpp
        tests/test_property.cpp
        tests/test_adversarial.cpp)
    target_include_directories(research-ledger-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(research-ledger-tests PRIVATE ResearchLedger::research_ledger)
    research_ledger_strict(research-ledger-tests)
    set_target_properties(research-ledger-tests PROPERTIES WIN32_EXECUTABLE OFF)

    add_test(NAME research_ledger_tests COMMAND research-ledger-tests)

    if(RESEARCH_LEDGER_ENABLE_ADDRESS_SANITIZER AND RESEARCH_LEDGER_ASAN_RUNTIME_DIR)
        set_tests_properties(research_ledger_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${RESEARCH_LEDGER_ASAN_RUNTIME_DIR}")
    endif()

    # The real multiprocess proof: independent coordinator and worker processes
    # over loopback TCP, real process death, real coordinator restart.
    if(RESEARCH_LEDGER_BUILD_TOOLS)
        add_test(NAME research_ledger_multiprocess_proof
            COMMAND research-ledger-multiprocess-proof
                    --tools $<TARGET_FILE_DIR:research-ledger-coordinator>)
        if(RESEARCH_LEDGER_ENABLE_ADDRESS_SANITIZER AND RESEARCH_LEDGER_ASAN_RUNTIME_DIR)
            set_tests_properties(research_ledger_multiprocess_proof PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${RESEARCH_LEDGER_ASAN_RUNTIME_DIR}")
        endif()
    endif()
endif()

# --- AddressSanitizer self test ---------------------------------------------
# Only built for a sanitizer configuration: it is the proof that the sanitizer
# this suite claims to run under is genuinely instrumenting the build.
if(RESEARCH_LEDGER_ENABLE_ADDRESS_SANITIZER)
    add_executable(research-ledger-asan-self-test tests/asan_self_test.cpp)
    research_ledger_strict(research-ledger-asan-self-test)
    set_target_properties(research-ledger-asan-self-test PROPERTIES WIN32_EXECUTABLE OFF)

    if(RESEARCH_LEDGER_BUILD_TESTS)
        add_test(NAME research_ledger_asan_self_test COMMAND research-ledger-asan-self-test)
        # The test passes only when AddressSanitizer reports the deliberate
        # invalid access; a silent exit is a failure.
        set_tests_properties(research_ledger_asan_self_test PROPERTIES
            PASS_REGULAR_EXPRESSION "heap-buffer-overflow"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${RESEARCH_LEDGER_ASAN_RUNTIME_DIR}")
    endif()
endif()

# --- examples and benchmarks -------------------------------------------------
if(RESEARCH_LEDGER_BUILD_EXAMPLES)
    add_executable(research-ledger-examples examples/examples_main.cpp)
    target_include_directories(research-ledger-examples PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(research-ledger-examples PRIVATE ResearchLedger::research_ledger)
    research_ledger_strict(research-ledger-examples)
    set_target_properties(research-ledger-examples PROPERTIES WIN32_EXECUTABLE OFF)

    if(RESEARCH_LEDGER_BUILD_TESTS)
        add_test(NAME research_ledger_examples COMMAND research-ledger-examples)
    endif()
endif()

if(RESEARCH_LEDGER_BUILD_BENCHMARKS)
    add_executable(research-ledger-bench benchmarks/bench_main.cpp)
    target_include_directories(research-ledger-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
    target_link_libraries(research-ledger-bench PRIVATE ResearchLedger::research_ledger)
    research_ledger_strict(research-ledger-bench)
    set_target_properties(research-ledger-bench PROPERTIES WIN32_EXECUTABLE OFF)
endif()
