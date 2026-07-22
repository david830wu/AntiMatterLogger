cmake_minimum_required(VERSION 3.20)
project(AntiMatterLogger
        VERSION 0.1.0
        DESCRIPTION "AmcLogger - fast, small, pure-C logging for AntiMatterCommon"
        LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AMC_TOP_LEVEL ON)
else()
    set(AMC_TOP_LEVEL OFF)
endif()

option(AMC_BUILD_TESTS "Build the AmcLogger unit tests" ${AMC_TOP_LEVEL})
option(AMC_BUILD_BENCH "Build the benchmark harness" OFF)
option(AMC_BENCH_SPDLOG "Build the optional spdlog comparison benchmark (FetchContent)" OFF)
option(AMC_BUILD_EXAMPLES "Build the example program" OFF)

set(AMC_SOURCES
    src/AmcLoggerCore.c
    src/AmcLoggerConfig.c
    src/AmcLoggerQueue.c
    src/AmcLoggerRender.c
    src/AmcLoggerSink.c)

find_package(Threads REQUIRED)

function(amc_apply_common_props target)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wformat=2 -Werror)
    target_link_libraries(${target} PUBLIC Threads::Threads)
endfunction()

# ---- the library ----
add_library(amc_logger STATIC ${AMC_SOURCES})
target_include_directories(amc_logger
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>)
amc_apply_common_props(amc_logger)
add_library(AntiMatter::AmcLogger ALIAS amc_logger)

install(TARGETS amc_logger ARCHIVE DESTINATION lib)
install(FILES include/AmcLogger.h DESTINATION include)

# ---- tests: the executable user manual (tests/Test01..Test09) ----
if(AMC_BUILD_TESTS)
    enable_testing()

    add_library(unity STATIC third_party/unity/unity.c)
    target_include_directories(unity PUBLIC third_party/unity)

    # library rebuilt with the test seam (amc_internal_test_reset, clock hook)
    add_library(amc_logger_testing STATIC ${AMC_SOURCES})
    target_include_directories(amc_logger_testing PUBLIC include)
    target_compile_definitions(amc_logger_testing PUBLIC AMC_LOGGER_TESTING)
    amc_apply_common_props(amc_logger_testing)

    set(AMC_TEST_CHAPTERS
        Test01_QuickStart
        Test02_LogFormat
        Test03_Levels
        Test04_Configuration
        Test05_FileSink
        Test06_Lifecycle
        Test07_AsyncAndReliability
        Test08_CompileTimeStrip
        Test09_MultiThreaded
        Test10_JsonPayload)

    foreach(chapter ${AMC_TEST_CHAPTERS})
        add_executable(${chapter} tests/${chapter}.c tests/TestHelperModule.c)
        target_link_libraries(${chapter} PRIVATE amc_logger_testing unity)
        set_target_properties(${chapter} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
        target_compile_options(${chapter} PRIVATE -Wall -Wextra)
        # each chapter runs in its own directory so temp files never collide
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run/${chapter})
        add_test(NAME ${chapter}
                 COMMAND ${chapter}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run/${chapter})
    endforeach()

    # chapter 08 demonstrates compile-time stripping: same sources, extra flag
    target_compile_definitions(Test08_CompileTimeStrip
        PRIVATE AMC_LOGGER_ACTIVE_LEVEL=AMC_LOGGER_LEVEL_WARN)
endif()

# ---- example program (docs/Design.md §9); run from the repository root ----
if(AMC_BUILD_EXAMPLES)
    add_executable(amc_example examples/main.c examples/LongTermExtractor.c)
    target_link_libraries(amc_example PRIVATE amc_logger)
    set_target_properties(amc_example PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_options(amc_example PRIVATE -Wall -Wextra)
endif()

# ---- benchmark harness (docs/Architecture.md §13); always built -O2 ----
if(AMC_BUILD_BENCH)
    foreach(b BenchDisabled BenchEnqueue BenchThroughput)
        add_executable(${b} bench/${b}.c)
        target_link_libraries(${b} PRIVATE amc_logger)
        set_target_properties(${b} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
        target_compile_options(${b} PRIVATE -Wall -Wextra -O2)
    endforeach()

    if(AMC_BENCH_SPDLOG)
        # C++ is confined to this one benchmark target; the library stays pure C
        enable_language(CXX)
        include(FetchContent)
        FetchContent_Declare(spdlog
            URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz)
        FetchContent_MakeAvailable(spdlog)
        add_executable(BenchSpdlog bench/BenchSpdlog.cpp)
        target_link_libraries(BenchSpdlog PRIVATE spdlog::spdlog)
        set_target_properties(BenchSpdlog PROPERTIES CXX_STANDARD 17)
        target_compile_options(BenchSpdlog PRIVATE -O2)
    endif()
endif()
