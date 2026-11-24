cmake_minimum_required(VERSION 3.16...3.24)
project(frequency_manager)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(frequency_manager PRIVATE "src/")

if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(frequency_manager_bookmark_tests
        tests/bookmark_mode_tests.cpp
        src/bookmark.cpp)
    target_include_directories(frequency_manager_bookmark_tests PRIVATE "src/")
    target_link_libraries(frequency_manager_bookmark_tests PRIVATE sdrpp_core)
    add_test(NAME frequency_manager_bookmark_tests
        COMMAND frequency_manager_bookmark_tests)
    if (COMMAND sdrpp_stage_directory_runtime_dlls)
        sdrpp_stage_directory_runtime_dlls()
    endif ()
endif ()
