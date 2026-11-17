# vendor/onnxruntime — ONNX Runtime C API header (header only)

- Upstream: https://github.com/microsoft/onnxruntime
- File: `include/onnxruntime/core/session/onnxruntime_c_api.h` at tag
  **v1.21.0** (`ORT_API_VERSION 21`), verbatim, as skimmer-for-linux
  vendors it (`vendor/onnxruntime/`, pinned commit 03beb5b).
- Licence: MIT (`LICENSE` beside it, Microsoft Corporation).

Compiled only with `OPT_BUILD_CW_DEEPCW`. The engine never links ONNX
Runtime: `src/engine/ort_shim.cpp` loads the runtime library at run time
(`LoadLibrary` on Windows, `dlopen` elsewhere), asks
`OrtGetApiBase()->GetApi(21)` and uses only that API table. The build needs
nothing but this header, and a machine without the runtime still runs
SDRIAK: the DeepCW backend then reports why it is not available.

API version 21 is the oldest one a distribution we care about ships (Debian
trixie `libonnxruntime1.21`; Fedora 1.26; Arch 1.29); newer libraries answer
`GetApi(21)`. `GetApi(N)` returns null on a library older than N, so a bump
makes the backend unavailable on those systems.
