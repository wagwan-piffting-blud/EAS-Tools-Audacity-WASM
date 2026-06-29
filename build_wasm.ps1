# build_wasm.ps1 - compile the x86 VST emulator to WebAssembly (run from PowerShell in _emu).
# Requires the Emscripten SDK to be active in this shell:  emsdk_env.ps1  (or .bat)
# Output: ./site/vstemu.js + vstemu.wasm  (no preloaded DLL; the host passes bytes at runtime).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force -Path .\site | Out-Null

$exportedFns = "['_vst_w_load','_vst_w_numparams','_vst_w_numinputs','_vst_w_numoutputs'," +
               "'_vst_w_uniqueid','_vst_w_samplerate','_vst_w_blocksize','_vst_w_resume','_vst_w_suspend'," +
               "'_vst_w_setparam','_vst_w_getparam','_vst_w_param_index','_vst_w_setparam_byname','_vst_w_apply_macro','_vst_w_process'," +
               "'_vst_w_effectname','_vst_w_vendor','_vst_w_paramname','_vst_w_paramdisplay','_vst_w_faulted'," +
               "'_ladspa_w_load','_ladspa_w_prepare','_ladspa_w_numinputs','_ladspa_w_numoutputs'," +
               "'_ladspa_w_apply_macro','_ladspa_w_process','_ladspa_w_faulted'," +
               "'_malloc','_free']"
$exportedRt  = "['ccall','cwrap','UTF8ToString','stringToUTF8','lengthBytesUTF8','HEAPU8','HEAPF32','HEAP32']"

# ladspa_host.c uses -DLADSPA_STANDALONE_LOG so it shares vst_host.c's call_guest/emu_log/EMU_VERBOSE
# (no duplicate symbols) instead of defining its own.
emcc mem.c cpu.c loader.c win32_vst.c vst_host.c ladspa_host.c wasm_main.c `
  -DLADSPA_STANDALONE_LOG `
  -O3 -flto -lm `
  -o .\site\vstemu.js `
  -s MODULARIZE=1 -s "EXPORT_NAME=VstEmuModule" `
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=33554432 -s MAXIMUM_MEMORY=536870912 `
  -s STACK_SIZE=2097152 `
  -s "EXPORTED_FUNCTIONS=$exportedFns" `
  -s "EXPORTED_RUNTIME_METHODS=$exportedRt" `
  -s "ENVIRONMENT=web,worker,node" `
  -s "EXPORT_ES6=0"

if ($LASTEXITCODE -ne 0) { Write-Error "emcc build failed"; exit 1 }
Write-Host "built site/vstemu.js  ($((Get-Item .\site\vstemu.wasm).Length) bytes wasm)"
