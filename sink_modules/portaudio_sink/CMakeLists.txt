cmake_minimum_required(VERSION 3.16...3.24)
project(audio_sink)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

sdrpp_link_dep(audio_sink portaudio PC_NAME portaudio-2.0
	TARGETS portaudio portaudio_static
	SHARED_TARGETS portaudio
	STATIC_TARGETS portaudio_static)

if (OPT_BUILD_TESTS AND NOT ANDROID)
    add_executable(portaudio_bridge_tests tests/portaudio_bridge_tests.cpp)
    target_include_directories(portaudio_bridge_tests PRIVATE src)
    target_link_libraries(portaudio_bridge_tests PRIVATE sdrpp_core)
    if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(portaudio_bridge_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
    endif ()
    add_test(NAME portaudio_bridge_tests COMMAND portaudio_bridge_tests)
endif ()
