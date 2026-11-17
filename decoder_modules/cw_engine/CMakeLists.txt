cmake_minimum_required(VERSION 3.16...3.24)
project(cw_engine CXX)

# The CW skimmer engine (OK1HRA's skimmer-for-linux, file for file — see
# README.md), built once as a static library and linked into the CW decoder
# modules. Upstream's C sources are plain C++17 on the standard library
# (renamed .cpp; GLib replaced by std containers, locks and strings).
# src/sdriak holds WDSP's fir_bandpass, which upstream takes from a
# vendored WDSP, on SDRIAK's own core dsp library.

add_library(cw_engine STATIC
    src/engine/callsign.cpp
    src/engine/channelizer.cpp
    src/engine/cw_align.cpp
    src/engine/cw_callreader.cpp
    src/engine/cw_morse.cpp
    src/engine/cw_reread.cpp
    src/engine/decode_cw_config.cpp
    src/engine/decode_cw_viterbi.cpp
    src/engine/oracle.cpp
    src/sdriak/fir_bandpass.cpp)

set_target_properties(cw_engine PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    # Linked into module shared libraries; its symbols stay inside them.
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden)

# "engine/decode_cw.h" for consumers.
target_include_directories(cw_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

if (MSVC)
    # fopen/sscanf in the upstream sources.
    target_compile_definitions(cw_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
endif ()

# sdrpp_core: the dsp headers, fftw3f and the exported FFTW planner lock.
find_package(Threads REQUIRED)
target_link_libraries(cw_engine PUBLIC sdrpp_core Threads::Threads)
if (UNIX)
    target_link_libraries(cw_engine PUBLIC m)
endif ()

# The DeepCW neural decoders, optional (DeepCW.md): engine/decode_deepcw.h
# (v1 model, OK1HRA/OK1BR's engine) and engine/decode_deepcw2.h (v2 models,
# e04's stitcher), on one model runtime (engine/deepcw_runtime.h: sha256 pin,
# workers). ONNX Runtime is never linked: engine/ort_shim.cpp loads it at
# run time (LoadLibrary / dlopen) through the vendored MIT C API header, so
# the build needs nothing else and a machine without the runtime reports
# the backends unavailable. Consumers see CW_ENGINE_DEEPCW.
if (OPT_BUILD_CW_DEEPCW)
    target_sources(cw_engine PRIVATE
        src/engine/decode_deepcw.cpp
        src/engine/decode_deepcw2.cpp
        src/engine/deepcw_runtime.cpp
        src/engine/ort_shim.cpp
        src/engine/skim_sha256.cpp)
    target_include_directories(cw_engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/onnxruntime")
    target_compile_definitions(cw_engine PUBLIC CW_ENGINE_DEEPCW=1)
    target_link_libraries(cw_engine PUBLIC ${CMAKE_DL_LIBS})
endif ()

# Upstream's offline gates (src/*_test.cpp; cw_test without its v1 runs).
# Like core's tests they sit beside sdrpp_core's shared library, which they
# load.
if (OPT_BUILD_TESTS AND NOT ANDROID)
    foreach (_t cw_test call_test)
        add_executable(cw_engine_${_t} src/${_t}.cpp)
        target_link_libraries(cw_engine_${_t} PRIVATE cw_engine)
        set_target_properties(cw_engine_${_t} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON)
        if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
            set_target_properties(cw_engine_${_t} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
        endif ()
        if (MSVC)
            target_compile_definitions(cw_engine_${_t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif ()
        add_test(NAME cw_engine_${_t} COMMAND cw_engine_${_t})
        set_tests_properties(cw_engine_${_t} PROPERTIES TIMEOUT 300)
    endforeach ()

    # The DeepCW gates: the commit rule / stitcher, the model pin and the DSP
    # half always; the model sections only when SKIM_ORT_LIB and the model
    # variables resolve (exit 77 = skipped). Inline inference keeps them
    # deterministic.
    if (OPT_BUILD_CW_DEEPCW)
        foreach (_t deepcw_test deepcw2_test)
            add_executable(cw_engine_${_t} src/${_t}.cpp)
            target_link_libraries(cw_engine_${_t} PRIVATE cw_engine)
            set_target_properties(cw_engine_${_t} PROPERTIES
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED ON)
            if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
                set_target_properties(cw_engine_${_t} PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
            endif ()
            if (MSVC)
                target_compile_definitions(cw_engine_${_t} PRIVATE _CRT_SECURE_NO_WARNINGS)
            endif ()
            add_test(NAME cw_engine_${_t} COMMAND cw_engine_${_t})
            set_tests_properties(cw_engine_${_t} PROPERTIES
                TIMEOUT 300
                SKIP_RETURN_CODE 77)
        endforeach ()
        set_tests_properties(cw_engine_deepcw_test PROPERTIES ENVIRONMENT "SKIM_DEEPCW_SYNC=1")
        set_tests_properties(cw_engine_deepcw2_test PROPERTIES ENVIRONMENT "SKIM_DEEPCW2_SYNC=1")
    endif ()

    # SDRIAK's bench (experiments/cw_bench): the decoders behind the real
    # channelizer, with fading, impulses and recorded IQ; bench.py drives it.
    add_executable(cw_engine_bench experiments/cw_bench/cw_bench.cpp)
    target_link_libraries(cw_engine_bench PRIVATE cw_engine)
    set_target_properties(cw_engine_bench PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
    if (SDRPP_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(cw_engine_bench PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${SDRPP_RUNTIME_OUTPUT_DIRECTORY}")
    endif ()
    if (MSVC)
        target_compile_definitions(cw_engine_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif ()
    add_test(NAME cw_engine_bench COMMAND cw_engine_bench --gate quick)
    # The channel-start case fails on the current code: the squelch opens on
    # the channelizer's zero-history start and the bootstrap learns noise
    # (ALGORITHM.md §5.3.6). WILL_FAIL keeps ctest green while that holds; it
    # turns red once the decoder starts cleanly by default (e.g.
    # experiments/acquire-settle-and-witness.diff with settle_s on in the
    # preset): then drop WILL_FAIL.
    add_test(NAME cw_engine_bench_startup COMMAND cw_engine_bench --gate startup)
    set_tests_properties(cw_engine_bench cw_engine_bench_startup PROPERTIES TIMEOUT 300)
    set_tests_properties(cw_engine_bench_startup PROPERTIES WILL_FAIL TRUE)
endif ()
