# SDRIAK Build System for Third-Party Dependencies

This document describes how the third-party dependency build should be
controlled, using PrusaSlicer's `deps/` build as the closest reference point
and the current SDRIAK `deps/` tree as the implementation base.

The goal here is to document the intended design and the policy currently being
implemented in the `deps/` tree.

## 1. Target Policy

The dependency policy should be driven by the distribution profile, not by a
single global `BUILD_SHARED_LIBS` switch.

### 1.1 Portable builds

Portable builds are:

- Windows bundles
- macOS bundles
- Linux AppImage

For portable builds, the default policy should be:

- build dependencies fresh from source inside `deps/`
- keep bundled dependencies shared unless they have been reviewed and marked as
  static source-build candidates
- statically link those reviewed candidates when they are built from source

Exceptions are allowed for platform ABI libraries that are unsafe or impractical
to bundle. On Linux AppImage this already applies to the host OpenGL, X11, and
glibc stack, and currently also to `libglfw3`.

### 1.2 Distro-packaged Linux builds

For Linux builds intended for distro packages, the default policy should be:

- use system-provided development packages for most dependencies
- resolve them through `find_package()` or `pkg-config`
- bundle and build from source only for rare exceptions where the distro copy is
  too old or otherwise unsuitable

System-provided distro packages are linked in their normal dynamic/shared form.
If a distro-profile dependency is explicitly built from bundled source instead,
it is static only when that package is in the reviewed static-candidate set.

This means Linux distro packaging should default to `system` plus dynamic
linking, while Windows, macOS, AppImage, and Android source builds default to
`bundled` with per-package linkage.

## 2. Current State

### 2.1 What PrusaSlicer already does well

PrusaSlicer already separates two concerns:

- which packages are provided by the platform
- whether dependency builds default to static or shared

Relevant files:

- `deps/CMakeLists.txt`
- `deps/autobuild.cmake`
- `cmake/modules/AddCMakeProject.cmake`
- main `CMakeLists.txt` with `SLIC3R_STATIC`

The important pattern is that the application build owns the high-level linkage
policy, while the `deps/` build owns source-vs-system selection and recipe-level
exceptions.

### 2.2 What SDRIAK used to do

SDRIAK already had a usable `deps/` build, but it was controlled by a small set
of coarse switches:

- global `BUILD_SHARED_LIBS`
- package include/exclude selection
- flat `SYSTEM_PROVIDED_PACKAGES` allowlist

Relevant files:

- `deps/CMakeLists.txt`
- `deps/autobuild.cmake`
- `deps/cmake/AddCMakeProject.cmake`
- `deps/cmake/ValidateDep.cmake`
- `cmake/sdrpp_find_dep.cmake`

That was enough for "build most things here" versus "skip some host-provided
packages", but not enough for the full matrix required by SDRIAK packaging.

## 3. Main Gaps in the Current SDRIAK Design

### 3.1 `BUILD_SHARED_LIBS` is too coarse

One global `BUILD_SHARED_LIBS` option cannot express all of these at once:

- one dependency should come from the system, another from source
- one bundled dependency should be static, another shared
- one dependency should be static when source-built but system-provided and
  shared on distro Linux

Some recipes already override the global default. That is a sign that the real
policy is already per-package, just not modeled centrally.

### 3.2 AppImage uses a dedicated `deps/` preset

The AppImage flow under `docker_builds/appimage/` now builds third-party
dependencies through the dedicated `deps` preset `appimage`, which resolves the
portable profile into a normal deps install prefix and then configures the app
against that prefix.

The one deliberate Linux AppImage exception currently kept host-provided is
`glfw3`, because GLFW couples directly to the user's OpenGL and X11 stack. The
`appimage` preset forces that package to `system` while the rest of the SDR
dependency set is built through `deps/`.

### 3.3 Consumer-side imported targets assume shared linkage

The helper logic that emits imported package configuration should not hardcode a
shared-library model when the resolved package policy says a dependency is
static-only.

In other words, the consumer side must model the same linkage mode that the
recipe actually produced.

## 4. Recommended Control Model

The dependency system should model three orthogonal properties per package.

### 4.1 Source origin

Each dependency should resolve to one of:

- `bundled`
- `system`
- `auto`

Meaning:

- `bundled`: build in `deps/`
- `system`: do not build in `deps/`, require host package discovery
- `auto`: choose from the active build profile

### 4.2 Linkage mode

Each dependency should resolve to one of:

- `static`
- `shared`
- `header-only`

Meaning:

- `static`: link only into the consuming target
- `shared`: build a runtime library and distribute it where needed
- `header-only`: no runtime binary to package

### 4.3 Usage class

Each dependency should be classified by how it is consumed:

- `module-private`
- `shared-runtime`
- `core`

Meaning:

- `module-private`: used by one module only
- `shared-runtime`: used by multiple modules or recipes
- `core`: used by the core executable/runtime

Usage does not override the default linkage rule by itself. It is still useful
metadata for dependency graph review, packaging manifests, and explicit
shared-only exceptions.

## 5. Build Profiles

Instead of letting `BUILD_SHARED_LIBS` carry the whole design, the top-level
policy should be expressed through a build profile.

Recommended cache variable:

- `SDRPP_DEP_PROFILE`

Suggested values:

- `portable`
- `distro`
- `android`

### 5.1 `portable`

Used for:

- Windows
- macOS
- AppImage

Defaults:

- dependency source: `bundled`
- linkage: per package; the reviewed static-candidate set is static, the rest
  remain shared

Exceptions:

- host graphics stack on Linux AppImage
- any dependency that cannot be safely or usefully redistributed as bundled
- vendor SDKs that are distributed only as shared binaries

### 5.2 `distro`

Used for Linux distro packaging.

Defaults:

- dependency source: `system`
- linkage: `shared`, matching normal distro package consumption

Exception path:

- override selected packages to `bundled` when the distro version is too old or
  missing required functionality
- bundled exceptions are static only when listed as static source-build
  candidates

### 5.3 `android`

Android already behaves differently and should remain a separate profile rather
than being forced into the desktop rules.

Defaults:

- dependency source: `bundled` for packages that are actually built on Android
- linkage: per package; the reviewed static-candidate set is static, the rest
  remain shared

There is intentionally no separate `windows` profile. Windows uses the
`portable` profile, which keeps the profile matrix small and avoids duplicating
policy that also applies to macOS, AppImage, and source-built Android deps.

The static-candidate rows normally encode this as:

- `portable:static distro:shared android:static` for dependencies that use a
  distro/system package in the `distro` profile
- `portable:static distro:static android:static` for dependencies that are
  bundled even in the `distro` profile

The current reviewed static-candidate wave is:

- `libairspy`
- `libairspyhf`
- `libfobos`
- `libhydrasdr`
- `libperseus-sdr`
- `librtlsdr`
- `libad9361`
- `libiio`
- `libxml2`
- `zlib`
- `glfw3`
- `volk`
- `zstd`

## 6. Suggested Per-Package Metadata

Each recipe should declare or inherit central metadata such as:

```cmake
# Example shape only.
sdrpp_register_dep(libusb
    DEFAULT_SOURCE portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:shared distro:shared android:shared
    USAGE shared-runtime)

sdrpp_register_dep(libairspy
    DEFAULT_SOURCE portable:bundled distro:system android:bundled
    DEFAULT_LINKAGE portable:static distro:shared android:static
    USAGE module-private)

sdrpp_register_dep(libfobos
    DEFAULT_SOURCE portable:bundled distro:bundled android:bundled
    DEFAULT_LINKAGE portable:static distro:static android:static
    USAGE module-private)
```

The exact macro name is not important. What matters is that policy resolution
happens once, centrally, and recipes consume the resolved result.

The linkage rule follows the resolved source and the reviewed static-candidate
set:

- `system` dependencies in the `distro` profile are consumed as shared distro
  libraries
- static-candidate dependencies are built static when bundled from source
- other bundled dependencies remain shared until reviewed and intentionally
  moved into the static-candidate set

## 7. Where the Control Should Live

### 7.1 `deps/CMakeLists.txt`

This file should resolve the per-package policy for:

- whether a package is built or treated as system-provided
- which linkage mode a bundled package should use
- which packages are pulled into the top-level `deps` target

The existing `SYSTEM_PROVIDED_PACKAGES` list should evolve into a real per-package
source policy table.

### 7.2 `deps/cmake/AddCMakeProject.cmake`

This helper should stop forwarding one global `BUILD_SHARED_LIBS` value to every
recipe as if all packages shared the same linkage decision.

Instead it should forward a resolved per-package linkage mode, for example by
setting recipe-specific options or a helper variable derived from the package
metadata.

### 7.3 `deps/cmake/ValidateDep.cmake`

Validation should verify the resolved mode for the specific package:

- static archive for `static`
- shared object or DLL plus import library where applicable for `shared`
- config package presence only where appropriate

It should not assume that the entire dependency tree is globally shared or
globally static.

### 7.4 Generated imported package configs

Generated consumer package files must represent the actual built artifact type.

If a package resolved to `static`, its imported target must not be emitted as a
shared target. Otherwise the consumer side and the produced artifacts will drift
out of sync.

#### 7.4.1 Cross-toolchain root-path filtering (`NO_CMAKE_FIND_ROOT_PATH`)

Cross-compilation toolchains restrict CMake's find commands to search only
inside the cross-root. The Android NDK toolchain sets:

- `CMAKE_FIND_ROOT_PATH = <NDK sysroot>` (plus Gradle's prefab dir when invoked
  by the Android Gradle Plugin)
- `CMAKE_FIND_ROOT_PATH_MODE_PACKAGE = ONLY` (some NDK / AGP combinations)
- `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY = ONLY`
- `CMAKE_FIND_ROOT_PATH_MODE_INCLUDE = ONLY`

Under these modes the find commands ignore any path that is not a subtree of
`CMAKE_FIND_ROOT_PATH`, **including paths passed explicitly via `PATHS` and
`HINTS`**. The deps install prefix sits outside the NDK sysroot, so without
intervention every `find_package`, `find_library`, `find_path`, and `find_file`
that targets the deps prefix silently returns `-NOTFOUND`. Downstream symptoms:

- `find_package(CURL CONFIG PATHS deps_prefix NO_DEFAULT_PATH)` reports the
  package as not found even though `CURLConfig.cmake` exists at the path.
- Emitted `<name>Config.cmake` files load successfully but populate the
  imported target with `IMPORTED_LOCATION = "-NOTFOUND"` and
  `INTERFACE_INCLUDE_DIRECTORIES = "_<name>_inc-NOTFOUND"`, which trips
  CMake's generate-time validation that listed paths exist on disk.
- The validator's CONFIG-target probe in `ValidateDepStep.cmake` (which
  re-configures a tiny sub-project under the same toolchain) hits the same
  filtering and falsely reports the install as broken.

The fix for direct lookups is to add `NO_CMAKE_FIND_ROOT_PATH` to every find
command that points at the deps prefix. This is a per-call opt-out from the
root-path restriction and is a no-op on native toolchains where
`CMAKE_FIND_ROOT_PATH` isn't set, so it's safe to apply unconditionally rather
than gating on `if (ANDROID)`.

The direct and transitive lookup sites below must handle the restriction
consistently:

| Location                                                              | Lookups covered                                                                          |
| --------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| `cmake/sdrpp_find_dep.cmake` (consumer side, `sdrpp_link_dep`)        | the strict step-1 `find_package(<pkg> CONFIG PATHS deps_prefix NO_DEFAULT_PATH)`         |
| `deps/cmake/ValidateDepStep.cmake` (validator probe)                  | the probe sub-project's `find_package(<pkg> CONFIG PATHS deps_prefix NO_DEFAULT_PATH)`   |
| consumer and validator lookups above (Android only)                  | transitive `find_dependency` / `find_library` / `find_path` calls made by an upstream Config; prepend the deps prefix to the local `CMAKE_FIND_ROOT_PATH` before loading it |
| `deps/cmake/AddCMakeProject.cmake` (`sdrpp_emit_imported_config`)     | `find_package` for each `PACKAGE_DEPENDENCIES` entry, plus `find_library` / `find_path` / `find_file` that populate the imported target |
| `deps/+librtlsdr/fix_librtlsdr_config.cmake` (custom Config rewriter) | every find call inside the hand-written rtlsdr Config (`find_package(libusb …)`, two `find_library`, two `find_path`, one Windows `find_file`) |

Any new dep that ships its own custom Config rewriter (rather than going
through `sdrpp_emit_imported_config`) must apply the same pattern.

**Boundary:** direct lookups that we own use `NO_CMAKE_FIND_ROOT_PATH`. Many
upstream-provided Configs bake in their target paths, but that is not a safe
assumption: curl 8.22's `CURLConfig.cmake`, for example, calls
`find_dependency(MbedTLS MODULE)`, and that module performs its own
`find_library` / `find_path` calls. Transitive calls do not inherit
`NO_CMAKE_FIND_ROOT_PATH` from the outer `find_package(CURL)`. On Android,
`sdrpp_link_dep` and the validator therefore prepend the explicit deps prefix
to a normal, locally scoped `CMAKE_FIND_ROOT_PATH` variable immediately before
loading the Config. This permits only that Config's dependency closure to find
the same bundled prefix.

**Why not centralize via a cached `CMAKE_FIND_ROOT_PATH` extension?** An
earlier attempt appended the deps prefix in `deps/autobuild.cmake`. That did
not survive because the Android Gradle Plugin re-passes
`CMAKE_FIND_ROOT_PATH` with its prefab value on every reconfigure, overriding
the cached append. The current extension is deliberately different: it is a
normal variable set at the point of use, inside the `sdrpp_link_dep` function
or validator probe. AGP never sees or overwrites it, and its scope ends with
the lookup. Direct calls still opt out per-call with
`NO_CMAKE_FIND_ROOT_PATH`, preserving the toolchain's intended behavior for
unrelated searches.

### 7.5 `cmake/sdrpp_find_dep.cmake`

This file is already close to the right consumer behavior for distro Linux:

- try `find_package(CONFIG)` first
- fall back to `pkg-config`

That should stay. The missing control is upstream of it: deciding when a package
should be expected from the system at all.

## 8. Packaging-Specific Guidance

### 8.1 Windows

Preferred policy:

- build all third-party dependencies from source in `deps/`
- use static linkage for the reviewed static-candidate set
- keep other bundled dependencies shared until they are reviewed

### 8.2 macOS

Preferred policy:

- same overall rules as Windows
- use the `deps/` tree as the authoritative source of bundled third-party
  libraries
- use static linkage for the reviewed static-candidate set

### 8.3 Linux AppImage

Preferred policy:

- use the same source-first dependency strategy as other portable builds
- build portable third-party libraries in `deps/`
- use static linkage for the reviewed static-candidate set
- keep platform ABI libraries host-provided where bundling is known to be
  harmful or fragile

This implies the current AppImage Docker image should eventually stop being the
primary dependency builder and should instead become a host environment for
running the `deps/` portable profile.

### 8.4 Linux distro packages

Preferred policy:

- use distro packages for the large majority of dependencies
- reserve bundled builds for explicit compatibility exceptions
- link distro packages dynamically
- build bundled exceptions static only when they are in the reviewed
  static-candidate set

This keeps security updates, ABI tracking, and maintenance aligned with distro
policy.

## 9. Staging and Deployment Model

The build should not assume one staging strategy for all use cases.

There are three distinct consumers of build outputs:

- development-time runnable staging
- system install staging
- bundle packaging staging

These consumers should share one CMake-owned artifact registry, but they should
not be forced into one common filesystem layout.

### 9.1 Development staging

Development staging exists to make a locally built tree runnable under an IDE or
from the command line without pretending that it is a final package.

Typical requirements are:

- copy or symlink runtime libraries next to the executable or into a known dev
  runtime tree
- expose plugins in a predictable location for debugger launches
- stage resources and configuration defaults needed for interactive testing
- preserve a fast edit-build-run cycle

This should be modeled as a dedicated CMake deploy target, for example
`sdrpp_stage_dev`, not as a side effect of `install()`.

### 9.2 System install staging

System install staging is the normal `install()` surface consumed by distro
packaging and local installs.

This is the correct place for:

- Linux FHS-style paths
- `DESTDIR` staging for distro packages
- generator-agnostic `cmake --install`

This path should stay authoritative for files that belong to a system install,
but it should not be treated as the only runtime layout used by the project.

### 9.3 Bundle packaging staging

Bundle packaging for Windows, macOS, and AppImage has different layout rules and
should remain separate from system install staging.

Typical requirements are:

- Windows ZIP layout with the executable, plugins, and runtime DLLs arranged for
  portable use
- macOS `.app` bundle layout with `MacOS`, `Frameworks`, and plugin/resource
  directories
- AppImage `AppDir` layout under `usr/`

These packaging layouts should be driven by CMake-known artifact metadata, but
they do not need to reuse the exact directory structure from `install()`.

### 9.4 Single source of truth, multiple projections

The safe shared abstraction is not one universal staging directory.

The safe shared abstraction is one authoritative registry of built artifacts and
their roles, with separate projections for:

- development staging
- system install
- bundle packaging

That means the packaging scripts should stop hardcoding module DLL and dylib
lists, but it does not mean the package builders must all call the same install
command into the same tree.

## 10. Proposed CMake Artifact Model

The build should declare artifact role once, near the target definition, and
then reuse that information in the staging flows.

Suggested roles:

- `runtime-executable`
- `runtime-library`
- `plugin`
- `resource-tree`
- `bundle-framework`

Example shape only:

```cmake
sdrpp_register_runtime_artifact(sdrpp
    ROLE runtime-executable)

sdrpp_register_runtime_artifact(sdrpp_core
    ROLE runtime-library)

sdrpp_register_runtime_artifact(${PROJECT_NAME}
    ROLE plugin)

sdrpp_register_runtime_resource(root/res
    ROLE resource-tree)
```

The exact API can differ, but the important rule is that the target-owning CMake
file declares the role and the packagers consume that declaration.

### 10.1 Registration points

Natural registration points are:

- top-level `CMakeLists.txt` for the main executable and shared resources
- `core/CMakeLists.txt` for `sdrpp_core`
- `sdrpp_module.cmake` for plugins

This avoids a second hand-maintained inventory inside shell or PowerShell
packaging scripts.

### 10.2 Generated manifest

CMake should generate a machine-readable manifest for the configured build.

That manifest should contain resolved build outputs and metadata such as:

- target file path
- artifact role
- destination class
- whether the artifact participates in dev staging, system install, bundle
  staging, or multiple of them

Possible formats:

- CMake include fragment
- JSON file
- plain newline-delimited manifest consumed by scripts

The format matters less than the ownership boundary. CMake should own artifact
discovery. Package scripts should consume discovered artifacts rather than naming
them manually.

### 10.3 Relationship to `install()`

`install()` should remain the canonical definition for system installation.

It should not be overloaded to cover every deploy scenario. Instead:

- use `install()` and `cmake --install` for system installs and distro staging
- use a dedicated deploy target for development staging
- use a bundle-staging manifest or bundle-specific staging helpers for Windows,
  macOS, and AppImage

This keeps the current Linux install assumptions intact while still removing the
manual target lists from the bundle scripts.

## 11. Migration Plan

Recommended implementation order:

1. Add this policy layer without changing every recipe immediately.
2. Introduce `SDRPP_DEP_PROFILE` and per-package source resolution.
3. Introduce per-package linkage resolution instead of relying on a single
   `BUILD_SHARED_LIBS` default.
4. Update validation and generated imported configs to reflect resolved package
   type.
5. Convert AppImage to consume a dedicated `deps/` profile.
6. Introduce a CMake-owned runtime artifact registry and manifest generation.
7. Add a dedicated development staging target.
8. Rework Windows, macOS, and AppImage packaging scripts to consume the
   generated artifact manifest instead of hardcoded file lists.
9. Classify packages one by one into `module-private`, `shared-runtime`, or
   `core`.

## 12. Practical Classification Heuristic

The first decision is source origin:

- for distro Linux packaging, prefer `system`
- for portable bundles and Android, prefer `bundled`
- for distro-profile exceptions, use `bundled` only when the system package is
  unavailable, too old, or otherwise unsuitable

Once a dependency resolves to `bundled`, review it before changing linkage. Move
it to `static` only when the static artifact builds cleanly on the relevant
platforms and does not create duplicate-runtime or ABI problems for consumers.
Until that review happens, keep bundled dependencies shared.

## 13. Summary

The key design decision is to stop treating dependency control as one global
`BUILD_SHARED_LIBS` choice.

SDRIAK needs a package-aware policy layer that decides, per dependency:

- build from source or use system package
- static, shared, or header-only consumption
- portable-profile behavior versus distro-profile behavior

It also needs one CMake-owned view of runtime artifacts that can be projected
into three separate staging models:

- development runnable staging
- system install staging
- bundle packaging staging

PrusaSlicer's build shows the right structural split between application policy,
dependency orchestration, and recipe exceptions. SDRIAK should keep its current
`deps/` foundation, but extend it with per-package source and linkage policy so
the same tree can support Windows, macOS, AppImage, and distro Linux cleanly
without keeping brittle per-script artifact inventories.
