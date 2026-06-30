# x86 VST/LADSPA → WASM emulator (`_emu`)

A small **32-bit x86 interpreter + PE32 loader + Win32 shim + VST2/LADSPA host**, in pure C,
that runs unmodified Windows VST2 and LADSPA plugin DLLs **headless** — natively, and via
Emscripten in the browser. It is the plugin engine behind **EAS Tools**' in-browser Audacity
macro renderer: the WASM build runs the real plugin DLLs while a JavaScript engine
(`eas-tools/assets/js/audacity-macro-engine.js`) replays full Audacity 2.4.2 macro chains,
applying the built-in and Nyquist effects as faithful native-DSP ports.

The CPU core (`cpu.c`), memory (`mem.c`) and PE loader (`loader.c`) are shared with the
AcuVoice Roger TTS emulator. The VST2/LADSPA host harness and the Win32 shim are local.

## Status (current)

- **In-browser performance solved.** A runtime **x86→WASM JIT** (the interpreter stays as the
  baseline + deopt fallback; x87 is bit-identical because the JIT emits WASM `f64` and reuses the
  same C libm trampolines) plus a **Web-Worker** parallelism layer cut the headline
  `NWR-KIG76Prototype` macro on a 172 s sample from **~100 s → ~11–13 s** — **faster than native
  Audacity** (~20.7 s) — and stays **bit-exact across ~120 plugins**. See **Performance** below.
- **x87 + SSE/SSE2** implemented. **~326 / 340 plugin DLLs load and produce audio** headless
  (338 VST + 2 LADSPA in the manifest). The remaining few trap in their constructor or hit an
  unmodelled path; a **fault guard** (`vst_w_faulted()` / `ladspa_w_faulted()`) stops a
  misbehaving plugin mid-block instead of crashing the host, so a macro chain always completes.
- **LADSPA host added** (`ladspa_host.c`) — `sc4` (swh RMS compressor) and `chebstortion`
  (= the `ChebyshevDistortion` macro command) run through the same emulator; their `run()` is
  pure float DSP.
- **Shipped in EAS Tools.** The full macro library (**582 macros**) renders end-to-end in the
  browser; a smoke test of all 582 produces audio with **0 silent, 0 crashes**. Headline macros
  (EASyPLUS, NWR-SDR, XJ-FM-v5) were A/B-validated against real Audacity and match (most
  effects bit-exact).
- **Effect coverage is comprehensive (EAS Tools side).** Every built-in and Nyquist effect the
  582-macro library invokes is a native-DSP port in `audacity-macro-engine.js`, A/B-validated
  against real Audacity — bit-exact where the DSP is deterministic (filters, EQ, delays, fades,
  tape/soft-clip limiters, DTMF, etc.), faithful where it isn't (`Clipper` ≈0.994, `PopMute`
  ≈0.9998), and statistically matched for the two intentionally non-deterministic effects
  (`RandomAmplitudeModulation`, `RandomPitchModulation`, which use an unseeded Nyquist `noise`).
  A census of all 90 distinct macro commands flagged the only remaining gaps as **unbundled
  third-party VSTs** (`Broadcast`, `Gverb`, `DynamicMirror`, Thimeo tools), three heavier ports
  (`Reverb` Freeverb, `NoiseReduction` spectral, `Flutter` — source not located), and the
  deliberate length-edit no-ops (`Delete`/`Trim`, single fixed-length-buffer model).

## x87 bug fixes in `cpu.c` (the difference between silence/NaN and correct audio)

These only surface in float-heavy DSP (the MSVC TTS code never emitted these forms). **All
four are backport candidates for `AcuVoiceRoger\web\emu\cpu.c`.**

1. **`FSCALE` (D9 FD)** had a spurious `fpop()` and swapped operands. Correct:
   `ST(0) = ST(0) * 2^trunc(ST(1))`, no pop. Broke `pow`/`exp` and drifted the FPU stack.
2. **`DC` reg-form `FSUB`/`FSUBR`/`FDIV`/`FDIVR`** — the `DC` (dest = ST(i)) operand order
   matches the `DE` pop-forms (minus the pop), NOT the `DC`-memory direction. Verified vs
   capstone (`dc e1` = `fsubr st(1),st(0)`). Corrupted the fraction in `exp()`'s reduction →
   `sc4` envelope-coef table filled with garbage → compressor gated to silence.
3. **`FXAM` (D9 E5) was a no-op** — it never set C0–C3. MSVC `exp`/`pow` do
   `fxam; fnstsw; xlatb; jmp [table]` (a computed jump on the classification), so with stale
   flags they dispatched to the wrong handler for certain inputs. This was the audible
   **"crunch"**: `sc4`'s attack coefficients came out ~10× too fast. Now classifies
   zero/NaN/inf/denormal/normal + sign per Intel.
4. **`FIST`/`FISTP`/`FRNDINT` truncated instead of rounding per the control-word RC field.**
   All five integer-store sites used a C cast `(int)*st(0)` (truncate toward zero); x87
   rounds to nearest-even by default. A bitcrusher (`dblue_Crusher`) quantizes via `FISTP`, so
   its quantization **residual** came out exactly 2× too large and uncorrelated — invisible in
   normal use (the crushed output still matched at corr 0.99996) but the **NWR-SDR** macro
   amplifies that residual ×316, so it dominated the output and made the voice "sound wrong."
   Added `fpu_round_rc()` (RC = `cw>>10 & 3`: nearest / floor / ceil / trunc).

Also added to `cpu.c`: `XLAT` (D7), `SHLD`/`SHRD` (0F A4/A5/AC/AD), and the SSE/SSE2 unit.

## Performance: x86→WASM JIT + Web-Worker parallelism

VST-heavy macros were slow — a 172 s sample rendered `NWR-KIG76Prototype` in ~100 s (vs ~20.7 s
in native Audacity). The bottleneck is the interpreter itself (~80 M guest-instr/s; x87 ≈ 44 % of
executed instructions, MOV ≈ 34 %); the contained interpreter optimizations were exhausted. The
fix is a **runtime x86→WASM JIT** (v86-style) plus a **Web-Worker** layer. The JavaScript codegen
lives in EAS Tools (`eas-tools/assets/js/jit-wasm-encoder.js`, `jit-compiler.js`,
`worker-pool.js`, `resample-worker.js`, `plugin-worker.js`); the C side here provides the
dispatch, the trampolines and the layout exports.

**JIT — bit-exact by construction.** `cpu_run` stays as baseline + deopt fallback. A C hotness
map (`g_jitmap`, keyed `(eip>>1)&mask`) counts basic-block hits; past a threshold it calls
`emu_jit_request(eip)` (an `EM_JS` into `Module.__jit_compile`), which translates a region of
guest blocks into **one** WASM module that shares the global `CPU` and the linear memory and is
installed into `__indirect_function_table`. `cpu_run` then dispatches a hot `eip` by calling its
compiled block by table index; anything untranslatable **deopts** (stores `CPU.eip`, returns a
status) and the interpreter resumes — correctness never depends on JIT coverage. Because x87 is
emulated as C `double` + libm, the JIT emits WASM `f64` and calls the **same** C trampolines
(`jit_f2i` / `jit_frndint` / `jit_f2xm1` / `jit_fyl2x` / `jit_ftan` / `jit_fpatan` / `jit_fsin` /
`jit_fcos` / `jit_fscale`) → bit-identical to the interpreter, not an approximation. Integer
flags are eager-inlined, memory goes through the same page-map, SSE scalar promotes
`f32→f64→op→demote` to match the interpreter's rounding. (C side: `g_jitmap`/`jit_find`/
`jit_reset`/`emu_jit_request`/`emu_jit_flush` + the 9 trampolines in `cpu.c`; `jit_set_enabled`/
`jit_set_force`/`jit_set_threshold`/`jit_stats`/`jit_helpers`/`jit_cpu_addr`/`jit_layout`/
`jit_pagemap_base`/`jit_jitmap_base` exports in `wasm_main.c`; `ALLOW_TABLE_GROWTH` + exposed
`wasmMemory`/`wasmTable` + the `_jit_*` exports in `build_wasm.ps1`.)

**The two hard browser problems (both solved).**
1. *Too many tiny modules* → Firefox "failed to allocate executable memory." DSP code is
   call-heavy, so naive block-following produced ~1 module per block. Fixed by **linear region
   batching**: a compact instruction-length decoder (`insnLen`, JS) lets the compiler skip the
   call/ret/untranslatable terminator and batch a contiguous run of blocks into one module
   (2.3–5.3× fewer modules). Safe even if the length decoder is wrong — every compiled block is
   keyed to its own `eip`, so a bad skip only wastes a compile, it can't corrupt output.
2. *Executable-memory accumulation across runs* ("OOM after a few macros in a row"). The JIT
   flushes on every plugin load, so the same regions were re-compiled every run and Firefox
   didn't reclaim the exec memory fast enough. Fixed by a **byte-keyed LRU module cache**: a
   `WebAssembly.Module`'s compiled code is shared across its instances, so identical module bytes
   reuse the cached module → re-runs allocate **zero** new exec memory (measured: run 1 = 511
   modules created, runs 2 & 3 = 0, all bit-exact). A per-instance **module budget** and an
   **`oomHalt`** valve cap the footprint and degrade gracefully (decline new modules → interpret)
   if the ceiling is ever reached. (An earlier JS-heap OOM was traced to V8 retaining WASM
   instances that *import a table* — fixed by importing only the memory and passing the
   trampolines as function imports.)

**Parallelism.** A persistent pool of Web Workers, each a full emulator instance with its own
JIT + cache. The Kaiser resampler is split across workers (bit-exact, ~6×); long single-plugin
passes are time-chunked with a per-plugin state-warmup prefix (bit-exact for the chunk-safe set,
serial fallback otherwise).

**Results — 172 s sample, every path bit-exact vs the serial interpreter.** JIT'd PB workers:
RoughRider2 **4.85×**, dblue_Crusher **5.1–5.3×**, mda Combo **3.7×**. Full `NWR-KIG76Prototype`:
**~100 s (interpreter) → ~23 s (serial JIT) → ~11–13 s (JIT'd workers)** — faster than native
Audacity (~20.7 s). Verified bit-exact across ~120 plugins (Airwindows + mda + LADSPA). Kill-
switches everywhere: `window.EAS_JIT_DISABLE`, `cfg.disablePluginParallel` / `cfg.disableParallel`,
`cfg.workerJit` (worker-side JIT, opt-in), `cfg.workerModuleBudget` / `workerModuleCacheMax` /
`workerRegionMax`, `EAS_JIT_REGION_MAX` / `EAS_JIT_MODULE_BUDGET` / `EAS_JIT_MODULE_CACHE_MAX` /
`EAS_JIT_NO_MODULE_CACHE`, and `AudacityMacroEngine.resetPluginPool()`.

> **Load-once, investigated & rejected.** Keeping warmed plugins resident to skip re-compilation
> was measured at only **1.9 %** of macro time (region-batched compilation is already a fixed
> ~0.1–0.2 s/plugin startup that amortizes away over real audio; DSP dominates). The part of
> "load once, use many" that actually *mattered* was not re-creating modules — which is exactly
> what the byte-keyed module cache above does, and that fixed the OOM.

## Files

| File | Role |
|------|------|
| `emu.h` / `vst.h`   | shared types; VST2 AEffect offsets + opcodes |
| `mem.c`             | region-based guest VA→host memory (shared) |
| `cpu.c`             | 32-bit x86 interpreter: integer + string + x87 + SSE/SSE2 (shared, +fixes); **+ JIT dispatch** (`g_jitmap`, `cpu_run` hook, `emu_jit_request`/`emu_jit_flush`, 9 libm trampolines) |
| `loader.c`          | minimal PE32 loader: sections, relocs, imports→pseudo-VAs, TEB/PEB (shared) |
| `win32_vst.c`       | guest heap + import dispatch + ~120 CRT/Win32 shims |
| `vst_host.c`        | VST2 host: load, audioMaster callback, AEffect parse, dispatch/process |
| `ladspa_host.c`     | LADSPA host: descriptor parse, port wiring, `run()` (shares cpu/loader) |
| `main_native.c`     | native VST test driver (WAV in/out, param dump, stats) |
| `main_ladspa.c`     | native LADSPA test driver |
| `macrorun.c`        | native end-to-end macro replayer (early prototype; the browser engine is primary) |
| `probe_sc4.c`       | links the emulator to call `sc4`'s db2lin/lin2db and dump its coef table (debug) |
| `wasm_main.c`       | Emscripten-exported API (`_vst_w_*`, `_ladspa_w_*`) for the browser; **+ JIT exports** (`_jit_set_enabled/force/threshold`, `_jit_helpers/cpu_addr/layout/pagemap_base/jitmap_base`) |
| `eas-tools/assets/js/jit-*.js`, `*-worker.js` | **JIT codegen + parallelism (JS, not in this repo):** WASM encoder, region compiler + module cache, worker pool, resampler/plugin workers |
| `build.bat`         | native build (MSVC, run from PowerShell) |
| `build_wasm.ps1`    | WASM build (emcc; needs an active emsdk) → `site/vstemu.js` + `.wasm` |
| `site/`             | built WASM (copied into `eas-tools/assets/js/`) |

## Native usage

```powershell
.\build.bat
.\vstemu.exe "..\mda Overdrive.dll" out.wav            # synth a sine, write WAV, print stats
$env:EMU_PARAMS="0=0.052,1=0.5,2=0.3"; .\vstemu.exe "..\mda Overdrive.dll" out.wav  # params by index
.\vstemu.exe "..\mda Delay.dll" out.wav in.wav         # process a real WAV
```

Diagnostics (env-gated): `EMU_VERBOSE=1`, `EMU_PROCDBG=1`, `EMU_IATDUMP=1`,
`EMU_FPUTRACE=lo,hi,max` (+ `EMU_FT8=1` to dump all 8 st regs), `EMU_TESTFN=hexaddr,d1,d2`.

## Browser / EAS Tools path

`build_wasm.ps1` compiles `mem/cpu/loader/win32_vst/vst_host/ladspa_host/wasm_main` to
`site/vstemu.js` + `.wasm`. The JS macro engine hands DLL bytes in (`_vst_w_load` /
`_ladspa_w_load`), sets sample rate / block size / parameters **by name** from the macro
(`MdaCombo:Drive="0.5" …` via `_vst_w_apply_macro`), then renders block-by-block on Float32
heap views — offline rendering, which matches Audacity's "apply effect to selection" model.
Audacity feeds a mono track to a stereo VST's in1 with **silence in in2**; the engine
replicates that routing.

To rebuild and deploy:
```powershell
.\build_wasm.ps1
Copy-Item .\site\vstemu.js,.\site\vstemu.wasm "eas-tools\assets\js\" -Force
```

## Known limitations / TODO

- A handful of plugins still trap in their constructor (e.g. `RoughRider2` ctor MEM FAULT at
  `0xc0800008`) — non-fatal (the fault guard absorbs it), not chased.
- No MIDI/event input → instrument/trigger plugins stay silent.
- No GUI/editor (`effEditOpen`) — headless processing only.
- `pow`/`exp` have negligible residual inaccuracy on some CRT special-case paths (stable, not NaN).
- **Worker-side JIT (`cfg.workerJit`) is opt-in**, pending broad in-browser confirmation across
  browsers; main-thread JIT is on by default. The byte-keyed module cache bounds executable
  memory across runs, but a single very heavy *first* run can still approach the browser's exec
  arena — `oomHalt` then degrades to interpreting the overflow instead of crashing.
- `mda Tracker` and `librnnoise_vst` hang/fault during processing in the **interpreter** itself
  (pre-existing, not JIT-related) — excluded from the bit-exact sweeps.
- **Backport the four `cpu.c` x87 fixes to AcuVoice** — they are latent there too. (The JIT
  trampoline/dispatch scaffold is also a candidate if AcuVoice ever needs the same speedup.)

## License

GPLv3, see `LICENSE`.

## GenAI Disclosure Notice: Portions of this repository have been generated using Generative AI tools (Claude Code).
