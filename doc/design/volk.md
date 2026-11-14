# VOLK exact input/output aliasing

Date: 2026-11-14

This document records SDRIAK's position on calling selected VOLK kernels with
the same buffer as an input and the output. It was prompted by the fused DSP
paths, where avoiding an intermediate buffer and copy is part of the reason for
fusion.

The decision is:

> Exact input/output identity is accepted for the kernels listed here. It works
> with the bundled VOLK revision and every relevant implementation inspected,
> and is established practice in GNU Radio. It is nevertheless a de-facto
> compatibility property, not a guarantee in VOLK's public contract.

This is not permission for arbitrary overlapping ranges. `out == in`,
`out == a`, or `out == b` are the only overlap forms covered by this decision.
Shifted or partial overlap such as `out == in + 1` is unsupported.

---

## 1. Correctness conclusion

There are two different meanings of "correct" in this case.

**Operational correctness:** yes. The exact aliases used by the current source
are correct with the bundled VOLK revision and the released/system VOLK
versions and implementations inspected for this review. No current platform
failure was found.

**Contractual portability:** no. VOLK does not document exact input/output
aliasing as a supported property and does not systematically test it for every
kernel and protokernel. A future or custom VOLK implementation could therefore
break this behavior without contradicting the current public documentation.

Consequently, this is not classified as a present correctness defect. It is a
documented compatibility assumption and a forward-portability risk that must be
covered by tests when VOLK versions or supported architectures change.

## 2. What upstream VOLK guarantees

VOLK's v3.3 changelog explicitly says that correct in-place operation is not
tested, notes that GNU Radio relies on it for multiple kernels, and leaves a
way to test and document it as future work:

<https://github.com/gnuradio/volk/blob/061550b3ae0eb4743c122d50b458c24db3e0935c/docs/CHANGELOG.md#L962-L966>

The generated kernel documentation describes distinct input and output
parameters. Its examples allocate separate buffers and make no aliasing
promise. For example:

<https://www.libvolk.org/doxygen/volk_32f_s32f_multiply_32f.html>

The ordinary QA harness similarly allocates separate input and output buffers:

<https://github.com/gnuradio/volk/blob/061550b3ae0eb4743c122d50b458c24db3e0935c/lib/qa_utils.cc#L868-L898>

The current APIs do not use C `restrict`, compiler `noalias` annotations, or an
equivalent promise for these pointer arguments. Exact aliasing is therefore not
automatically C or C++ undefined behavior when the pointers otherwise have the
right type and alignment. The gap is a library-semantics gap: the result for
aliased arguments is not part of the documented API contract.

That distinction matters. The current compiler must preserve possible aliasing
for the current implementations, but a later VOLK implementation is free to
introduce a non-aliasing precondition unless upstream first formalizes the
behavior.

## 3. Source audit

The primary audit target was the bundled revision selected by
`deps/+volk/volk.cmake`:

```
e97e1fb4dadd138a5842a8bcffb38e57b067334d
```

That revision identifies itself as `v3.3.0-13-ge97e1fb` and dates from
2026-05-31. Historical source at VOLK 2.2.1, 2.5.1, 2.5.2, 3.1.2, 3.2.0 and
3.3.0 was also inspected, as was upstream `main` through commit
`061550b3ae0eb4743c122d50b458c24db3e0935c` from 2026-11-03.

The affected kernels are:

| Kernel | Accepted exact alias |
|---|---|
| `volk_32f_s32f_multiply_32f` | output equals input |
| `volk_32f_x2_add_32f` | output equals either or both inputs |
| `volk_32f_x2_subtract_32f` | output equals either or both inputs |
| `volk_32f_x2_multiply_32f` | output equals either or both inputs |
| `volk_32fc_x2_multiply_32fc` | output equals either or both inputs |
| `volk_32fc_conjugate_32fc` | output equals input |
| `volk_32fc_s32fc_x2_rotator_32fc` | output equals input |
| `volk_32fc_s32fc_x2_rotator2_32fc` | output equals input |

The generic, x86 SSE/AVX/AVX-512, ARM NEON/NEONv8 and RISC-V RVV variants
available at those revisions were inspected. Their relevant loops load the
current scalar or SIMD chunk before storing the corresponding output chunk.
The complex multiply and rotator implementations load the values required for
both components before overwriting that element. The inspected RISC-V assembly
also loads an unrolled chunk before its stores.

ORC recipes for the affected arithmetic are element-wise, same-index
operations. That makes exact identity compatible with their dataflow, although
the generated ORC backend is still not covered by a documented VOLK aliasing
contract.

This audit supports only exact identity. It does not establish a safe traversal
direction for partially overlapping ranges.

### 3.1 VOLK relies on the behavior internally

VOLK's power-spectrum implementation calls
`volk_32f_s32f_multiply_32f` twice with the same buffer as input and output,
then calls `volk_32f_log2_32f` in place:

<https://github.com/gnuradio/volk/blob/e97e1fb4dadd138a5842a8bcffb38e57b067334d/kernels/volk/volk_32fc_s32f_power_spectrum_32f.h#L81-L91>

This gives scalar multiply especially strong de-facto compatibility evidence.
The composite power-spectrum test may also catch a regression incidentally on
the selected implementation. It is not a deliberate alias test for every
scalar-multiply implementation, alignment and tail length.

## 4. GNU Radio compatibility evidence

GNU Radio deliberately uses exact aliases for several of the same kernels. Its
frequency-translating FIR implementation explains that reusing the same input
and output buffer is safe for many VOLK functions and avoids a temporary copy,
then passes `out` as both arguments to its VOLK-backed rotator:

<https://github.com/gnuradio/gnuradio/blob/3d33e04a249b31ef32106d275b927a7c4cf86425/gr-filter/lib/freq_xlating_fir_filter_impl.cc#L168-L172>

Other direct dependencies in the inspected GNU Radio revision include:

* add:
  <https://github.com/gnuradio/gnuradio/blob/3d33e04a249b31ef32106d275b927a7c4cf86425/gr-blocks/lib/add_blk_impl.cc#L28-L38>
* subtract:
  <https://github.com/gnuradio/gnuradio/blob/3d33e04a249b31ef32106d275b927a7c4cf86425/gr-blocks/lib/sub_impl.cc#L42-L51>
* real and complex multiply:
  <https://github.com/gnuradio/gnuradio/blob/3d33e04a249b31ef32106d275b927a7c4cf86425/gr-blocks/lib/multiply_impl.cc#L38-L73>
* scalar multiply:
  <https://github.com/gnuradio/gnuradio/blob/3d33e04a249b31ef32106d275b927a7c4cf86425/gr-blocks/lib/nlog10_ff_impl.cc#L43-L55>

This makes an upstream behavioral break unlikely: it would cause widespread
GNU Radio regressions. It still does not turn that behavior into a formal API
promise.

No equivalent explicit GNU Radio exact-alias dependency was found for
`volk_32fc_conjugate_32fc`. That kernel is safe in the inspected source, but
has weaker external compatibility evidence than the others.

## 5. Version and platform scope

Portable builds use the bundled revision above. Distro builds may use the
system package. The repository's Linux build matrix covers x86-64 and ARM64 on
Debian bookworm, trixie and sid and Ubuntu focal, jammy, noble and resolute.

The relevant package set spans multiple VOLK generations. Examples at the
time of this review include:

| Distribution | VOLK version |
|---|---:|
| Debian bookworm | 2.5.2 |
| Debian trixie | 3.2.0 |
| Debian sid | 3.3.0 |
| Ubuntu noble | 3.1.2 |
| Ubuntu resolute | 3.3.0 |

Package references:

* <https://packages.debian.org/source/volk>
* <https://packages.ubuntu.com/libvolk-dev>

The old rotator API is used before VOLK 3.1. The pointer-scalar `rotator2` API
is used from VOLK 3.1 onward. Both forms were included in the audit.

## 6. SDRIAK dependency sites

The fused DSP kernels intentionally allow their destination to be one of their
sources. The central sites are:

* `core/src/dsp/channel/frequency_xlator.h`
* `core/src/dsp/audio/volume.h`
* `core/src/dsp/math/add.h`
* `core/src/dsp/math/subtract.h`
* `core/src/dsp/math/multiply.h`
* `core/src/dsp/math/conjugate.h`
* `core/src/dsp/channel/rx_vfo.h`

At the 2026-11-14 source snapshot, direct exact-alias calls also exist outside
those wrappers:

* `core/src/dsp/demod/broadcast_fm.h`
* `core/src/gui/widgets/waterfall.cpp`
* `decoder_modules/atv_decoder/src/main.cpp`
* `decoder_modules/atv_decoder/src/amplitude.h`
* `decoder_modules/dab_decoder/src/dab_dsp.h`
* `decoder_modules/vor_receiver/src/vor_receiver.h`
* `misc_modules/noise_reduction_logmmse/src/logmmse.h`

Therefore, changing only the new fused paths would not remove the project-wide
dependency. Any policy change away from VOLK aliasing must begin with a fresh
repository-wide call-site audit.

## 7. Required boundary conditions

Code depending on this decision must obey all of the following:

1. Aliased pointers have exact identity. Partial overlap is forbidden.
2. The normal kernel type, size and alignment requirements still apply.
3. Binary kernels may alias the output to the left input, right input, or both
   only where this document lists that form.
4. A new VOLK kernel is not assumed alias-safe merely because it is
   element-wise. It must be inspected and added to the table deliberately.
5. A new architecture, hand-written assembly implementation, ORC change, or
   major VOLK update triggers a renewed audit or explicit alias tests.
6. This decision does not resolve separate type-punning questions caused by
   casting SDRIAK sample structures to VOLK complex types. Those casts require
   their own layout and aliasing justification.

## 8. Regression testing policy

An alias test must compute an out-of-place reference first and compare it with
the exact-alias result. A sanitizer-only test is insufficient: an alias-unsafe
kernel can silently produce wrong samples without accessing invalid memory.

Tests should cover:

* output aliased to each compatible input separately;
* output and both inputs identical for binary arithmetic;
* aligned and deliberately unaligned buffers;
* lengths `0`, `1`, SIMD-width minus one, SIMD width, SIMD-width plus one,
  `511`, `512`, `513`, and a representative large buffer;
* real, complex and stereo wrapper instantiations;
* rotator output and final phase;
* rotator processing as one buffer versus several consecutive buffers;
* both pre-3.1 and 3.1-or-newer rotator APIs where those versions remain
  supported.

A dispatcher test covers only the implementation selected for that machine and
VOLK profile. When possible, use VOLK's manual dispatch entry points and
function descriptors to enumerate every implementation compiled into the
test binary. The CI matrix should include at least x86-64 and ARM64 and both a
bundled build and representative system-VOLK builds.

## 9. Alternatives if formal guarantees become mandatory

If SDRIAK adopts a policy that permits only behavior guaranteed in upstream
documentation, the available replacements are:

### 9.1 Scalar exact-alias fallback

Detect exact identity once per buffer and execute a normal C++ element-wise
loop for the aliased case. This is straightforward for add, subtract,
multiply, scalar gain and conjugate. It must be benchmarked; auto-vectorization
is compiler- and alias-analysis-dependent.

### 9.2 Scratch output buffer

Run VOLK out of place into reusable storage, then copy or transfer the result.
This retains VOLK's selected implementation and numerical behavior but adds
memory traffic and weakens the purpose of fusing the path.

### 9.3 Rotator-specific caution

Do not replace the VOLK rotator with a casually written scalar recurrence.
Newer optimized rotators use different phase-renormalization strategies,
including Kahan/sine-cosine resynchronization. A fallback can therefore differ
in throughput, numerical drift and final phase even when every output is within
a loose sample tolerance. Any rotator replacement needs a phase-continuity
test and a benchmark.

### 9.4 Runtime compatibility test

A process-start or first-use test can compare in-place and out-of-place results
for the implementation selected on the host, then choose VOLK or a fallback.
Such a test must exercise aligned, unaligned and tail cases. It is more complex
than a direct fallback and still tests only the selected host implementations.

## 10. Preferred upstream resolution

The best long-term resolution is an upstream VOLK contract for exact aliasing,
not general overlap. A useful upstream change would provide:

* per-kernel metadata declaring which output/input identities are supported;
* generated documentation for that metadata;
* QA runs with the output aliased to every declared-compatible input;
* tests of aligned, unaligned and tail paths for every compiled protokernel.

Until that exists, SDRIAK retains exact aliasing as a tested compatibility
assumption. A new failure is a reason to select a fallback or constrain the
affected VOLK version; the absence of an upstream promise alone is not evidence
that the current implementation is producing incorrect samples.
