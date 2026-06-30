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

## Files

| File | Role |
|------|------|
| `emu.h` / `vst.h`   | shared types; VST2 AEffect offsets + opcodes |
| `mem.c`             | region-based guest VA→host memory (shared) |
| `cpu.c`             | 32-bit x86 interpreter: integer + string + x87 + SSE/SSE2 (shared, +fixes) |
| `loader.c`          | minimal PE32 loader: sections, relocs, imports→pseudo-VAs, TEB/PEB (shared) |
| `win32_vst.c`       | guest heap + import dispatch + ~120 CRT/Win32 shims |
| `vst_host.c`        | VST2 host: load, audioMaster callback, AEffect parse, dispatch/process |
| `ladspa_host.c`     | LADSPA host: descriptor parse, port wiring, `run()` (shares cpu/loader) |
| `main_native.c`     | native VST test driver (WAV in/out, param dump, stats) |
| `main_ladspa.c`     | native LADSPA test driver |
| `macrorun.c`        | native end-to-end macro replayer (early prototype; the browser engine is primary) |
| `probe_sc4.c`       | links the emulator to call `sc4`'s db2lin/lin2db and dump its coef table (debug) |
| `wasm_main.c`       | Emscripten-exported API (`_vst_w_*`, `_ladspa_w_*`) for the browser |
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
- **Backport the four `cpu.c` x87 fixes to AcuVoice** — they are latent there too.

## License

GPLv3, see `LICENSE`.

## GenAI Disclosure Notice: Portions of this repository have been generated using Generative AI tools (Claude Code).
