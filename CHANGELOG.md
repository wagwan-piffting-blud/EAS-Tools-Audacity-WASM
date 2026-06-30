# Changelog — `_emu` x86 VST/LADSPA emulator

Milestones for the C emulator and its integration into EAS Tools' in-browser Audacity macro
renderer. Effect-level DSP fidelity (resampler, filters, compressor, stereo model, Nyquist
ports) lives on the EAS Tools side (`eas-tools/assets/js/audacity-macro-engine.js`), not here.

## Emulator core (`cpu.c` / hosts)

- **FSCALE (D9 FD) fix** — removed a spurious `fpop()` and corrected operand roles to
  `ST(0) = ST(0) * 2^trunc(ST(1))`. Unbroke `pow`/`exp`; took the mda VST suite from ~12/31 to
  ~28/31 producing audio.
- **DC reg-form FSUB/FSUBR/FDIV/FDIVR fix** — the reg-form (dest = ST(i)) uses the DE pop-form
  operand order, not the DC-memory direction (verified vs capstone). Corrupted `exp()`'s
  reduction; gated the `sc4` LADSPA compressor to silence.
- **SSE/SSE2 unit added** — Tier-2 plugins (Airwindows, dblue, AllPassPhase) no longer trap on
  `F3 0F 10` etc. Brought the working set to ~293/308 non-mda DLLs.
- **LADSPA host added** (`ladspa_host.c`, `main_ladspa.c`) — `sc4` and `chebstortion` run
  through the shared cpu/loader; `run()` is pure float DSP. `probe_sc4.c` links the emulator to
  dump `sc4`'s db2lin/lin2db and envelope-coef table for native-vs-emulated diffing.
- **FXAM (D9 E5) fix** — was a no-op; now sets C0–C3 (zero/NaN/inf/denormal/normal + sign) per
  Intel. MSVC `exp`/`pow` dispatch on it via `fxam; fnstsw; xlatb; jmp [table]`, so stale flags
  mis-dispatched for certain inputs. This was the audible **"crunch"** — `sc4`'s attack came out
  ~10× too fast. After the fix, emulated `sc4` is bit-exact to native.
- **FIST/FISTP/FRNDINT rounding fix** — all five x87 integer-store sites truncated toward zero
  via a C cast; x87 rounds per the control-word RC field (default nearest-even). Added
  `fpu_round_rc()`. A bitcrusher (`dblue_Crusher`) quantizes via FISTP, so its quantization
  **residual** was 2× too large and uncorrelated — invisible normally (output corr 0.99996) but
  the **NWR-SDR** macro amplifies it ×316, which is what made that macro's voice sound wrong.
  After the fix the residual is bit-exact (corr 1.0).
- **Fault guard** — `vst_w_faulted()` / `ladspa_w_faulted()` stop a misbehaving plugin
  mid-block instead of crashing the host, so a macro chain always completes. A few plugins still
  trap in their constructor (e.g. `RoughRider2` at `0xc0800008`); non-fatal.
- Also added: `XLAT` (D7), `SHLD`/`SHRD` (0F A4/A5/AC/AD).

## Integration / browser

- **WASM build** (`build_wasm.ps1` → `site/vstemu.js` + `.wasm`, copied into
  `eas-tools/assets/js/`). Exposes `_vst_w_*` / `_ladspa_w_*`; parameters applied **by name**
  from the macro. Replicates Audacity's mono-in-1 / silence-in-2 routing for stereo VSTs.
- **Shipped in EAS Tools.** The full macro library (**582 macros**) renders end-to-end in the
  browser. Smoke test of all 582: **0 silent, 0 crashes**. Headline macros validated A/B vs real
  Audacity 2.4.2.
- **Plugin name resolution** (EAS Tools side) — the plugin manifest norms filenames with a
  `32`/`64` bitness suffix (`Silhouette32.dll`), but macros call plugins by effect name
  (`Silhouette`); resolution now strips the suffix as a fallback (guarded so it can't hijack a
  built-in effect name like `Distortion`). Recovered ~288 previously-skipped plugins.

## 2026-06 — effect coverage complete (EAS Tools side)

Effect-level DSP lives in `eas-tools/assets/js/audacity-macro-engine.js`, not in this repo, but
recorded here because it closes out the in-browser renderer. All items A/B-validated against real
Audacity 2.4.2 via the scripting-pipe harness (`diag_audacity.py` → `compare.py`).

- **Built-in effects ported** (bit-exact unless noted): Phaser, Loudness Normalization (full EBU
  R128 K-weighting + gated blocks), Auto Duck, Echo, Fade In/Out, Repeat — plus the earlier audit
  pass (resampler, HP/LP Butterworth, BassTreble, ChangeSpeed, NoiseGate, MultibandEq,
  HarmonicEnhancer, Compressor, real stereo model).
- **DTMF Tones** fixed to bit-exact (corr 1.0): the generator now spreads Audacity's leftover
  `diff` samples (+1 to the first `diff` tone/silence blocks) and uses the float `fs/250` fade —
  was 0.74 (mis-timed tones) before.
- **Nyquist `.ny` effects ported by usage** (bit-exact vs Audacity): ParametricEq, Notch Filter,
  Chebyshev Type I Filter (note: Nyquist's `biquad` negates a1/a2 vs `snd-biquad`, else the
  16th-order cascade goes unstable → NaN), Comb Filter (delayed feedback form `y[n]=x[n-D]+g·y[n-D]`),
  Tape Saturation Limiter, Studio Fade Out, Delay (multi-tap), Flanger (linear, resample-based).
  `Clipper` (≈0.994) and `Pop Mute` (≈0.9998) are faithful but not bit-exact;
  `RandomAmplitudeModulation` / `RandomPitchModulation` are structural ports — corr 1.0 is
  impossible because Nyquist's `noise` is unseeded (two identical Audacity runs correlate ~0.79),
  so they're matched on statistics/energy instead.
- **Talkbox**: added `mda Talkbox.dll` to the plugin manifest (`norm:"talkbox"`); emulates
  bit-exact. Manifest is now 340 entries.
- **Census** of all 90 distinct macro commands across the 582 macros: confirmed every invoked
  built-in/Nyquist effect and bundled plugin is handled. Remaining gaps are unbundled VSTs
  (`Broadcast` 46×, `BroadcastLimiterIii`, `Gverb`, `DynamicMirror`, `ThimeoStereoTool`),
  `Reverb`/`NoiseReduction`/`Flutter` (need native ports or sources), and length-changing edit
  ops (`Delete`/`Trim`/`SetTrack`/`TrackMove`) which are deliberate no-ops in the single-buffer
  model. `ChannelMixer` is a correct no-op (all macro uses are on mono tracks).

## 2026-06-30 — performance: x86→WASM JIT + Web-Worker parallelism

The renderer was *correct* but slow: a 172 s sample took ~100 s on the `NWR-KIG76Prototype`
macro (vs ~20.7 s in native Audacity). Profiling put the cost in the interpreter itself
(~80 M guest-instr/s; x87 ≈ 44 % of executed instructions, MOV ≈ 34 %), and the contained
interpreter optimizations were exhausted. The multiplicative win is a runtime JIT plus
parallelism. The JavaScript codegen lives in EAS Tools (`jit-wasm-encoder.js`, `jit-compiler.js`,
`worker-pool.js`, `resample-worker.js`, `plugin-worker.js`); the C side here gained the dispatch,
trampolines and layout exports. Recorded here because it closes out the in-browser renderer.

- **Runtime x86→WASM JIT (v86-style), bit-exact by construction.** `cpu_run` stays as baseline +
  deopt fallback. New in `cpu.c`: a hotness map `g_jitmap` (`jitent_t{eip,slot,count}`, keyed
  `(eip>>1)&mask`), `jit_find`/`jit_reset`, an `emu_jit_request(eip)` `EM_JS` that calls
  `Module.__jit_compile` (synchronous so the new table slot is live this `cpu_run`), the dispatch
  hook at the top of the `cpu_run` loop, and **9 libm trampolines** (`jit_f2i`, `jit_frndint`,
  `jit_f2xm1`, `jit_fyl2x`, `jit_ftan`, `jit_fpatan`, `jit_fsin`, `jit_fcos`, `jit_fscale`). New
  in `wasm_main.c`: `jit_set_enabled`/`jit_set_force`/`jit_set_threshold`/`jit_stats` and the
  layout exports `jit_helpers`/`jit_cpu_addr`/`jit_layout`/`jit_pagemap_base`/`jit_jitmap_base`.
  `build_wasm.ps1`: `ALLOW_TABLE_GROWTH`, exposed `wasmMemory`/`wasmTable`, the `_jit_*` exports.
  Hot blocks compile to WASM functions that share the global `CPU` + linear memory; untranslatable
  ops deopt to the interpreter, so correctness never depends on coverage. **x87 stays bit-exact**
  because the JIT emits WASM `f64` and calls the *same* C trampolines the interpreter uses (the
  x87 stack is already C `double` + libm), not a re-derivation. P0–P5 (integer/MOV/LEA/branch,
  x87, SSE/SSE2 scalar, string ops) verified bit-exact via a forced-compile differential harness.
- **JS-heap OOM fix (V8 table-import retention).** The first integration OOM'd after a few macros:
  V8 permanently retains WASM instances that *import a table*. Fixed by importing only the memory
  and passing the 9 trampolines as **function imports** (no table import).
- **Region batching (Firefox exec-memory OOM #1).** Per-block modules meant thousands of tiny
  WASM modules → "failed to allocate executable memory." DSP code is call-heavy, so following only
  direct branches gave ~1 block/region (useless). Fixed with **linear batching**: a compact
  instruction-length decoder (`insnLen`) lets the compiler skip the call/ret/untranslatable
  terminator and batch a contiguous run of blocks into **one** module — **2.3–5.3× fewer modules**
  (dblue 3057→575, RoughRider2 893→173). Safe even if the length decoder is wrong: every compiled
  block is keyed to its own `eip`, so a bad skip only wastes a compile, never corrupts output
  (self-check: `insnLen` == decoder length for every translatable instruction, 0 mismatches).
- **Module cache (Firefox exec-memory OOM #2 — "OOM after a few macros in a row").** Because the
  JIT flushes on every plugin load, the same regions were re-compiled every run and Firefox didn't
  reclaim the executable memory fast enough → it accumulated until `new WebAssembly.Module` threw
  `out of memory`. Fixed with a **byte-keyed LRU module cache**: a `WebAssembly.Module`'s compiled
  code is shared across its instances, so identical module bytes reuse the cached module. Measured
  on the NWR macro run 3×: **run 1 = 511 modules created, runs 2 & 3 = 0**, all bit-exact — re-runs
  allocate zero new executable memory. A per-instance **module budget** and an **`oomHalt`** valve
  cap the footprint and degrade gracefully (decline new modules → interpret) if the ceiling is hit.
- **Web-Worker parallelism.** A persistent pool of full emulator instances (each with its own JIT +
  cache): the Kaiser resampler is split across workers (~6×, bit-exact); long single-plugin passes
  are time-chunked with a per-plugin state-warmup prefix (bit-exact for the chunk-safe set, serial
  fallback otherwise). Worker-side JIT is opt-in (`cfg.workerJit`).
- **Load-once: investigated, rejected.** Keeping warmed plugins resident to skip re-compilation
  measured at **1.9 %** of macro time (region-batched compile is a fixed ~0.1–0.2 s/plugin startup
  that amortizes away; DSP dominates) — and `resume()` doesn't reset plugin state, so naive reuse
  wouldn't even be bit-exact. The part of the idea that mattered (don't *re-create* modules) is the
  module cache above.
- **Results (172 s sample, every path bit-exact vs the serial interpreter).** JIT'd PB workers:
  RoughRider2 4.85×, dblue 5.1–5.3×, mda Combo 3.7×. Full `NWR-KIG76Prototype`:
  **~100 s (interpreter) → ~23 s (serial JIT) → ~11–13 s (JIT'd workers)** — **faster than native
  Audacity** (~20.7 s). Verified bit-exact across **~120 plugins** (Airwindows + mda + LADSPA).
  Pre-existing interpreter hangs `mda Tracker` / `librnnoise_vst` excluded from the sweeps.

## Backport note

The four `cpu.c` x87 fixes (FSCALE, DC reg-form, FXAM, FIST/FISTP) are latent in
`AcuVoiceRoger\web\emu\cpu.c` too and should be backported. The JIT trampoline/dispatch scaffold
(`g_jitmap` + `emu_jit_request` + the 9 libm trampolines + the `wasm_main.c` layout exports) is a
further candidate if that emulator ever needs the same multiplicative speedup.
