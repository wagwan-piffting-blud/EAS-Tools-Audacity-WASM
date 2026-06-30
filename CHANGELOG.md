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

## Backport note

The four `cpu.c` x87 fixes (FSCALE, DC reg-form, FXAM, FIST/FISTP) are latent in
`AcuVoiceRoger\web\emu\cpu.c` too and should be backported.
