# skimmer-for-linux (OK1BR) — CW decoder architecture and prior art

*Researched 2026-10-03. Sources: the repository at commit `12154fc` (local clone `D:\src\skimmer-for-linux`), its git history, AG1LE's `morse-wip` source (Bell 1977 port), the fldigi and RSCW sources/docs, the author's public posts on X and LinkedIn, and the literature cited at the end. The Bell thesis and the 1977 Maryland report PDFs on DTIC returned HTTP 403 and were not read directly.*

## Scope

What CW decoders skimmer-for-linux ships, how each works, which models it uses and how CPU vs. CUDA is handled; a deep dive into the default classical decoder ("v2") and how it relates to published work; provenance of the code; and what the author has said publicly about the decoders.

| Project | Author | What it is | License |
|---|---|---|---|
| [OK1BR/skimmer-for-linux](https://github.com/OK1BR/skimmer-for-linux) | Richard Fakenberg, OK1BR | Native GTK4 multi-channel CW/RTTY skimmer, TCI client for [sdr-for-linux](https://github.com/OK1BR/sdr-for-linux); 1536 CW channels from one wideband IQ stream; spots back over TCI + CW-Skimmer-dialect telnet | GPL-3.0-or-later |

Summary: three CW engines — **v1** (classical threshold), **v2** (classical soft-decision semi-Markov Viterbi, the default), **DeepCW** (e04's published Conformer+CTC ONNX model). Exactly **one** neural model is used; CPU vs. CUDA is a user setting with automatic fallback to CPU, not a load-based switch. v2 has no direct published counterpart, but every ingredient has prior art; its closest ancestor is E. L. Bell's 1977 Bayesian manual-Morse decoder, which made the opposite design trade (speed, fading amplitude and Morse grammar inside the hidden state). The code cites no sources. The repository is human-directed agentic development (Claude co-author trailer on 205 of 211 commits, all 17 v2 commits).

---

## 1. Engine selection

`src/engine/pipeline.c:262` (`cw_engine_pick`): config `[decode] engine` (Preferences → Decoding), overridden by `SKIM_CW_ENGINE=v1|v2|deepcw` (legacy `SKIM_CW_V1=1`). If DeepCW is chosen but the ONNX Runtime library or the model file is missing, the pipeline logs a warning and **falls back to v2** — an availability check at pipeline build, not a load-based switch.

| Engine | File | Type |
|---|---|---|
| v1 | `src/engine/decode_cw.c` (583 lines) | Envelope → dual-rate level trackers → Schmitt trigger → run-length dot/dash classification with an adaptive dit EMA → Morse LUT. Kept as fallback. |
| **v2** (default) | `src/engine/decode_cw_v2.c` (1751 lines) | Soft-decision semi-Markov Viterbi (§3). |
| DeepCW | `src/engine/decode_deepcw.c` (1415) + `ort_shim.c` (343) | Neural, ONNX Runtime via `dlopen` (§2). |

No component adapts to CPU load. Cost is bounded by DeepCW's energy pre-gate (≥ 6 dB keyed line over floor, `SKIM_DEEPCW_GATE_DB`) and the bounded ingest queue that drops whole IQ blocks on overload (`pipeline.c:4`).

## 2. DeepCW — one model, CPU or CUDA

### Model

- Single file: e04's `deepcw-engine` `model.onnx` — Conformer, 3.61 M params (15 MB), 3 Conv2d front-end layers + 6 blocks d=192, CTC over 42 classes; input `[batch,1,time,65]` log1p |STFT|, 65 bins × 12.5 Hz (`docs/DEEPCW.md:33`). See also `nn-cw-decoders-denoisers.md` §1.
- Path: `SKIM_DEEPCW_MODEL`, else `~/.local/share/skimmer-for-linux/models/deepcw/model.onnx` (`decode_deepcw.c:176`). Kept out of git (AGPL-3.0); ONNX Runtime is `dlopen`-ed with a vendored MIT C header, never linked.
- e04's web-app models (`en`, `en_narrow`, `ja`, `cw_detect`) are unpublished and not used.
- One refcounted ORT session per process (`decode_deepcw.c:129`).
- Front end in the skimmer: the 250 Hz channel baseband → 20-point Hann DFT every 4 samples (80 ms window, 16 ms hop — a 6.7 % stretch of the trained 15 ms, verified OK 15–35 WPM) → ±5 bins kept per frame → 65-bin tile, peak-normalised (the model's InstanceNorm epsilon otherwise silences weak real-IQ channels), log1p → 10 s sliding window, greedy CTC, own commit rule (word-gap-based) plus gray draft tail.

### Device

- User choice: Preferences → Decoding → Device "CPU" / "GPU (CUDA)", persisted `[decode] device`; precedence `set_device()` > `SKIM_DEEPCW_DEVICE` > `cpu` (`decode_deepcw.c:195`). No auto-detection; default CPU.
- `skim_ort_session_new` appends the CUDA EP (device 0, `CreateCUDAProviderOptions` / `SessionOptionsAppendExecutionProvider_CUDA_V2`). On **any** failure (CPU-only runtime, no GPU, cuDNN unresolved, session refusal) it silently builds a CPU session with the same model and reports `"CPU (cuda unavailable: <reason>)"` (`ort_shim.c:166-214`). Arch's onnxruntime-cuda 1.29 needs `libcudnn.so.9` preloaded `RTLD_GLOBAL` (provider lacks the NEEDED entry).
- **Only behavioural difference:** inference tick per channel — 0.5 s on CUDA, 1.0 s on CPU (`decode_deepcw.c:645-651`, `SKIM_DEEPCW_TICK` overrides). Same 4 intra-op threads, 2 workers, batch 32, gate and commit rules on both.
- Measured (`docs/DEEPCW.md`): 80 m fixture 103 s wall on CUDA:0 vs 190 s on CPU; CPU cost ~2.2–2.8 ms per channel-second batched; all 1536 channels continuously is impossible on CPU, hence the gate.

## 3. v2 — the classical decoder

### Pipeline (from `decode_cw_v2.c`)

1. **Envelope** |IQ| at 250 Hz, 3-tap moving average.
2. **v1 plumbing:** peak tracker (instant attack, 0.8 s release), floor tracker (0.3 s), hysteresis squelch (open at peak > 4× floor, close < 2.6×), keying-duty test.
3. **Per-sample LLR** (`soft_step`), average of two views:
   - *span*: `5·(m − mid)/(µ_mark − µ_space)` clamped ±2.5; µ_mark is a fast decaying peak (0.35 s release) that follows a QSB trough; µ_space learns only from clearly space-like samples.
   - *Rayleigh*: z = m / (µ_space/1.2533); `3.3·(z²/2 − ln z − 2.89)` clamped — zero crossing z≈2.8 (pure noise exceeds it ~2 % of samples); a −18 dB in-dash notch sits at z≈5, i.e. "not noise". The average lets a sub-dit fade inside a dash carry ~zero evidence so the duration prior bridges it.
4. **Semi-Markov Viterbi** (`lattice_step`) over segments DIT, DAH, ESP, CSP, WSP, marks and spaces alternating. Segment score = ±Σ LLR (O(1) via a cumulative-sum ring, W=2048) + duration prior `−α·ln²(d/(ideal·dit))` (log-normal shape; ideals 1/3/1/3/7 dits, α = 10/10/10/6/2.5) − fixed per-segment cost 0.5 ("MDL-style anti-chatter"). Overlapping duration windows (dit 0.6–2.6, dah 1.6–5.0 dits) so a stale clock does not hard-misfile; ESP floor 0.7 dit (0.5 while `elem_err` > 0.2) rejects sub-dit flutter.
5. **Fixed-lag commit** (`lattice_commit`): traceback, commit segments older than 8 dits (4 dits on a healthy signal), every 8 samples.
6. **Live emission:** if the best head hypothesis has a space running since the last commit, emit the char at 0.867·csp dits and the word space at 1.2·√(csp·wsp) — the last char of an over has no closing mark. Over break at 16 dits.
7. **Dit clock:** bootstrap by clustering the first 8 Schmitt marks; EMA on committed elements (gain 0.15, 0.30 while `elem_err` > 0.2); **re-lock** from a 6-mark ring (bimodal → low cluster; unimodal → disambiguated by a space-ring "element-gap witness"); **watchdogs** reset the clock after 16 dahs or 24 dits in a row; carrier detection (> max(8 dits, 0.8 s) solid).
8. **Fist model** (`fist_learn`): last 24 gaps ≥ 1.6 dit, 2-means in log domain seeded from quartiles (label-free, avoids self-reinforcement), accepted if ratio ≥ 1.55; blends char/word gap centres (gain 0.25) which move priors, search windows and live-emission thresholds.
9. **Pre-roll replay:** 2 s ring of envelope samples replayed through the soft path when the channel wakes with a usable clock, recovering the head of an over eaten by the squelch/bootstrap.
10. **Tone offset:** lag-1 conjugate-product phase, EMA 0.01 (an unweighted Kay-type estimator).
11. WPM reported from (dit + mean element space)/2 — cancels the soft-edge bias.

Removed in git (`e10840f`, `ba12f77`) but still compiled in: an optional neural "reader" (`cw_reader.c`) armed only by `SKIM_CW_READER=<blob>`, no blob shipped (§6).

### Prior art

**Bell, NPS 1977** — *Optimal Bayesian estimation of the state of a probabilistically mapped memory-conditional Markov process with application to manual Morse decoding*. Read through AG1LE's faithful C++ transcription of the FORTRAN ([ag1le/morse-wip](https://github.com/ag1le/morse-wip), GPL-3.0):

- 5 ms envelope samples (200 Hz).
- 6 element states: dit, dah, e-spc, chr-spc, wrd-spc, pause; nominal 1/3/1/3/7/14 (`kimap`).
- **Speed is hidden state:** 5 speed-change branches per element boundary with probabilities 0.1/0.2/0.4/0.2/0.1 (0.15/0.2/0.3/0.2/0.15 for char-space/pause), range 10–60 WPM (`spdtr.cxx`, `rtrans`); speed changes only at element boundaries.
- **Grammar:** element transition matrix conditioned on the previous element (`elemtr`; e.g. after a dot: e-spc 0.581, chr-spc 0.335, wrd-spc 0.069, pause 0.015) and a letter state walking the Morse tree (`memfcn`, up to 400 entries) — paths can only spell valid characters.
- Duration enters through a duration-conditioned keystate transition probability with exponential tails (`xtrans.cxx`).
- **Per-path Kalman filter** on signal amplitude (fading model; `kalfil.cxx`, `model.cxx`) — the "exponentially expanding set of Kalman filters on a trellis".
- Sum-product posterior with beam pruning: keep paths until 0.99 cumulative probability, 7–25 paths × 30 successor states (`savep.cxx`); output after a fixed 1 s delay (`NDELAY 200`).
- AG1LE's finding: better than fldigi's legacy decoder in deep QSB and weak signals; noise estimator failed to track real noise ([blog](http://ag1le.blogspot.com/2013/01/towards-bayesian-morse-decoder.html), [part 2](http://ag1le.blogspot.com/2013/12/new-morse-decoder-part-2.html), [part 4](https://ag1le.blogspot.com/2014/06/new-morse-decoder-part-4.html)). LA1K's [cwhorse](https://github.com/la1k/cwhorse) planned to build a skimmer on it.

**University of Maryland TR-554, July 1977** — *Real-Time Recognition of Manual Morse Telegraphy Using Nonlinear Estimation and Viterbi Decoding* (DTIC ADA051001): matched filtering + Viterbi over a Markov model with a-priori and transition probabilities for the whole alphabet, on an LSI-11. Abstract only; author not confirmed.

**Gold 1959** — *Machine recognition of hand-sent Morse code*, IRE Trans. IT 5(1):17–24, MIT Lincoln Lab. Founding work on adaptive timing for hand-sent Morse.

**RSCW (PA3FWM)** — mix to 0 Hz, matched filter, soft values, Viterbi-like pruned ML search over dibits; emits text **when all surviving paths agree on a prefix** (survivor convergence) instead of a fixed lag. Requires known speed and machine timing.

**Johnson & Willsky, UAI 2010 (HDP-HSMM)** — uses learning Morse patterns as a demonstration: an HSMM with explicit durations separates three states where a plain HMM finds two. Direct academic precedent for duration modelling being what disambiguates dit/dah.

**HSMM theory** — Yu, *Hidden semi-Markov models*, Artificial Intelligence 174 (2010) and the 2015 Elsevier book (explicit-duration Viterbi and its cost); Levinson 1986 (continuous duration densities; log-normal is a standard choice, e.g. for word durations in speech recognisers); Rabiner 1989 tutorial (explicit-duration HMMs).

**Changepoint detection** — v2's lattice is Optimal Partitioning (Jackson et al. 2005) with typed segments; SEG_COST is the BIC/MDL per-segment penalty; cumulative-sum segment costs are standard there; PELT (Killick et al. 2012) is the pruned linear-time variant.

**Fixed-lag decision** — Forney, *The Viterbi Algorithm*, Proc. IEEE 61 (1973): survivor truncation depth.

**Tone offset** — Kay, *A fast and accurate single frequency estimator*, IEEE Trans. ASSP 37(12) (1989): weighted phase-difference estimator.

**Classical neighbours** — fldigi (`src/cw_rtty/cw.cxx`): attack/decay averages, hysteresis thresholds at sig − 0.2·Δ / noise + 0.7·Δ, speed from a moving average of (dot+dash)/2 pairs, optional SOM classifier — **v1 is this family**. ggmorse: Goertzel + threshold optimisation. Wang, Sun & Jia 2017 (J. Signal Processing 33(11)): clustering of element durations + binary-tree lookup (cf. v2's bootstrap and fist 2-means).

**CW Skimmer (VE3NEA)** — publicly only "based on Bayesian statistics", per-character error probabilities, error-probability squelch, bitwise merge of call hypotheses. No algorithm details published. VE3NEA's advice to AG1LE — compute the probability that the signal is present rather than a hard decision per sample — is exactly v2's soft-LLR approach.

### What differs from the literature

- **Two-view averaged LLR** (span + Rayleigh surprise) to pass in-element fades — no precedent found. The Rayleigh term is a noise-surprise score, not a true Rician-vs-Rayleigh ratio.
- **Speed outside the model:** EMA + re-lock + space-ratio witness + streak watchdogs exist because speed is not a lattice state; Bell avoids this whole failure class by searching over speed.
- **No grammar:** only mark/space alternation; no gap-type transition priors and no Morse-tree validity, so codes that are not characters can be assembled.
- **Global amplitude tracking** rather than per-hypothesis.
- **Max-product with exact segment durations** over a 5-type state space, so no beam pruning is needed (Bell: sum-product + pruning).
- Engineering-only features with no literature counterpart: pre-roll replay, live emission from the head hypothesis, carrier detection, clock watchdogs.

### Ideas suggested by the prior art (untested)

1. Small parallel speed bank (e.g. ×0.7/×1/×1.4 of the current dit), switch on score margin — could replace much of the re-lock/watchdog logic at ~3× lattice cost on active channels only.
2. Gap-type transition priors (Bell/Mills statistics) and Morse-tree validity (~64-node tree carried along the path, or a penalty at CSP commit).
3. Proper Rician-vs-Rayleigh LLR, with a correction for the MA-induced sample correlation (per-sample LLR summation currently over-counts evidence; the gain K absorbs it empirically).
4. Survivor-convergence commit (RSCW, hardware Viterbi decoders) instead of a fixed 4–8 dit lag — lower latency on clean signals without losing fade protection.

## 4. Documentation and citations in the code

- **No external citations** anywhere in v2, its commits or docs — no papers, URLs, authors or other decoders.
- Method names only: header "soft-decision semi-Markov Viterbi … log-normal duration priors" (`decode_cw_v2.c:1,11-12`), "MDL-style anti-chatter term" (`:92`), "2-means in the log domain, seeded from the ring's QUANTILES" (`:115`), "Rayleigh" (`:1255,1263`), "LLR", "traceback", "lag-committed". Fixed-lag Viterbi and the Kay-type estimator are not named.
- Justifications are internal: dated live incidents (9A170NT "7"→"G", EA1EYL's stretched CQ, 40 m QSO turnaround, UB7M "IISSESS", R3OM "YOTA"→"OTA"), own measurements, test-gate results. Introducing commit `cec6a73` follows the same pattern (gate: QSB 16 dB @ 0.31 Hz — v1 edit distance 14, v2 1; A/B replays).
- Stale: `decode_cw.c:18` and `decode_cw.h:4` still say an "HMM/Bayes" layer is planned — v2 is that layer.
- v2 has no design document (DeepCW has `docs/DEEPCW.md`).

## 5. Provenance

- All 17 commits touching `decode_cw_v2.c` (2026-07-15 → 07-19) carry `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`; 205 of the repo's 211 commits do. `CLAUDE.md` is a Claude Code working brief; comments record the maintainer's directions as dated quotes.
- Read as **human-directed agentic engineering with a strong measurement habit** (test gates with numbers, A/B replays on recorded IQ, live-failure-driven fixes, comments on rejected alternatives), not careless "vibe coding".
- Typical marks of the process: incident-by-incident accretion of heuristics compensating for a structural model gap (speed); first-principles design without prior-art review; many hand-tuned constants justified by single fixtures (LLR_K 5.0, LLR_MAX 2.5, SEG_ALPHA, SEG_COST 0.5, z≈2.8, streak limits 16/24, FIST_SEP 1.55) — overfitting risk; incident-diary comment style; loose ends (stale v1 comments, dead reader code).
- A fork exists: [ok1hra/skimmer-for-linux](https://github.com/ok1hra/skimmer-for-linux) (Dan, OK1HRA; README states its changes were made with Claude Code).

## 6. Author's public statements (X, LinkedIn)

Read 2026-10-03: 17 posts from [x.com/ok1br](https://x.com/ok1br) (Posts tab) and 12 posts from [LinkedIn](https://www.linkedin.com/in/richard-fakenberg/recent-activity/all/) (Czech), back to July 2026. Paraphrased.

- **Background:** LinkedIn — software architect, backend / IoT / GPS telematics, C · GTK4 · MySQL · Linux, 15+ years. X bio — CW, contesting, homebrew; JO60TD; building a native Linux ham stack in C/GTK4 (sdr-for-linux, skimmer-for-linux, log-for-linux, linked over TCI). Tests on an ANAN G2E. One post refers to "nobody of us on the project" owning a radio model — at least sdr-for-linux has more than one contributor.
- **v2 launch** (0.1.0, X 2026-08-05 + LinkedIn): 600 s contest replay — 19 tabled stations vs 12 for v1; v1 lost the strongest signal in the segment, mutated SN1T → IN1T and invented a call not on the band; ~1.5× CPU. Lesson stated: the earliest decision in the pipeline (an up-front threshold) is the most expensive, an irreversible loss of information.
- **Removed neural reader** (X 2026-08-06, LinkedIn): input was symbolic on/off run durations, not audio; features were ratios to neighbouring elements (speed-invariant by design); TCN + CTC, ~310 k parameters, dependency-free C inference. Offline CER 0.13 → 0.03. On air it fixed the hard ~10 % and broke the easy ~90 % — a net loss despite the better average. Deleted: code, weights, a 3.1 GB training environment.
- **Unverified claim:** in the same post, the classical decoder reaches **2× the coverage of a "reference skimmer"** (unnamed, presumably CW Skimmer) with frequency agreement in single-digit Hz. No supporting measurement found in the repo.
- **DeepCW live** (X 2026-09-19/20): SAC CW on an RTX 5070, 1536 channels over 192 kHz of 20 m, ~90 stations tabled while logging; first contest hour exposed 4 bugs the recordings missed. 0.5.0 on a 20 m recording: DeepCW 10 stations vs 7 classical. The 0.5.0 post carries X's "Made with AI" label.
- **Interop** (LinkedIn, 0.4.1): a Swedish user ran it against SunSDR + ExpertSDR3 (works at 156/312 kHz) — first foreign TCI server. Reviewing the client against the spec and Thetis's TCI server source found 5 assumptions copied from his own server (e.g. payload length always equal to the header's); test suite 20 → 32 checks, 5 failing on the old code. Point made: one author writing both sides of a protocol agrees with himself even on his mistakes.
- **Other:** the waterfall + callsign column replaced the station table (190 lines deleted); RTTY 0.3.0 at 768 channels, 11 % CPU (SARTG WW RTTY).
- LinkedIn posts describe the work in the first person and do not mention Claude/AI; no post mentions Bell, HSMMs or other prior art.
- Not read: X Replies tab, LinkedIn activity older than the July sdr-for-linux 0.2.0 post, comments on others' posts.

## References

- E. L. Bell, *Optimal Bayesian estimation of the state of a probabilistically mapped memory-conditional Markov process with application to manual Morse decoding*, NPS 1977 — [Calhoun](https://calhoun.nps.edu/entities/publication/c2ec2718-1416-4c5c-967a-7ee5d75171ad), [DTIC ADA046503](https://apps.dtic.mil/sti/pdfs/ADA046503.pdf); related NPS work: [Processing of the manual Morse signal using optimal linear filtering, smoothing and decoding](https://calhoun.nps.edu/entities/publication/f0209848-4f9d-4ffe-967e-8429ad6ca534)
- *Real-Time Recognition of Manual Morse Telegraphy Using Nonlinear Estimation and Viterbi Decoding*, Univ. of Maryland TR-554, 1977 — [DTIC ADA051001](https://apps.dtic.mil/sti/pdfs/ADA051001.pdf)
- B. Gold, *Machine recognition of hand-sent Morse code*, IRE Trans. IT 5(1):17–24, 1959 — [ETHW](https://ethw.org/Bernard_Gold)
- AG1LE: [ag1le/morse-wip](https://github.com/ag1le/morse-wip), [Towards Bayesian Morse Decoder](http://ag1le.blogspot.com/2013/01/towards-bayesian-morse-decoder.html), [New Morse Decoder Part 1](http://ag1le.blogspot.com/2013/09/new-morse-decoder-part-1.html), [Part 2](http://ag1le.blogspot.com/2013/12/new-morse-decoder-part-2.html), [Part 4](https://ag1le.blogspot.com/2014/06/new-morse-decoder-part-4.html); [la1k/cwhorse](https://github.com/la1k/cwhorse)
- RSCW: [overview](https://pa3fwm.nl/software/rscw/), [algorithm](https://pa3fwm.nl/software/rscw/algorithm.html); PA3FWM, [Timing of Morse code](https://pa3fwm.nl/technotes/tn31-morse-timing.html)
- M. J. Johnson, A. Willsky, *The Hierarchical Dirichlet Process Hidden Semi-Markov Model*, UAI 2010 — [arXiv:1203.3485](https://arxiv.org/abs/1203.3485)
- S.-Z. Yu, *Hidden semi-Markov models*, Artificial Intelligence 174 (2010) — [PDF](https://www.cs.ubc.ca/~murphyk/Teaching/CS540-Spring10/projects/Yu-hsmm09.pdf); *Hidden Semi-Markov Models: Theory, Algorithms and Applications*, Elsevier 2015 — [publisher](https://shop.elsevier.com/books/hidden-semi-markov-models/yu/978-0-12-802767-7); [Inference in HMMs with explicit state duration distributions](https://arxiv.org/pdf/1203.0038)
- R. Killick, P. Fearnhead, I. Eckley, *Optimal detection of changepoints with a linear computational cost* (PELT), 2012 — [ar5iv](https://ar5iv.org/html/1101.1438)
- G. D. Forney, *The Viterbi Algorithm*, Proc. IEEE 61:268–278, 1973
- S. Kay, *A fast and accurate single frequency estimator*, IEEE Trans. ASSP 37(12), 1989 — [URI](https://digitalcommons.uri.edu/ele_facpubs/1387)
- Wang Ya-qi, Sun Zhong-hua, Jia Ke-bin, *Research on Automatic Decoding Algorithm of Morse Telegraph*, J. Signal Processing 33(11), 2017 — [link](https://signal.ejournal.org.cn/en/article/doi/10.16798/j.issn.1003-0530.2017.11.006)
- fldigi [`cw.cxx`](https://github.com/w1hkj/fldigi/blob/master/src/cw_rtty/cw.cxx); [ggmorse](https://github.com/ggerganov/ggmorse); [DeepFist](https://github.com/n9bc/DeepFist); [deepmorse-decoder](https://github.com/ag1le/deepmorse-decoder)
- CW Skimmer: [dxatlas.com](https://dxatlas.com/CwSkimmer/), [Wikipedia](https://en.wikipedia.org/wiki/CW_Skimmer), [Gritty](https://www.dxatlas.com/Gritty)
- skimmer-for-linux: [repo](https://github.com/OK1BR/skimmer-for-linux), [v0.5.0](https://github.com/OK1BR/skimmer-for-linux/releases/tag/v0.5.0), [issue #5 (DeepCW)](https://github.com/OK1BR/skimmer-for-linux/issues/5), [ok1hra fork](https://github.com/ok1hra/skimmer-for-linux); author: [X](https://x.com/ok1br), [LinkedIn](https://www.linkedin.com/in/richard-fakenberg/recent-activity/all/)
- Companion report: `nn-cw-decoders-denoisers.md` (e04 DeepCW, VE3NEA DeepCW, AG1LE, ggmorse, HamNoise)
