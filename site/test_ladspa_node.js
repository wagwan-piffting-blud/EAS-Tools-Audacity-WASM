// Smoke-test the LADSPA path in the WASM emulator: load, prepare, apply macro, process.
const fs = require('fs');
const path = require('path');
const VstEmuModule = require('./vstemu.js');

(async () => {
    const M = await VstEmuModule();
    const dllPath = process.argv[2] || path.join(__dirname, '..', '..', 'sc4_1882.dll');
    const macro = process.argv[3] || '';
    const SR = 44100, BS = 512, N = SR; // 1 second

    const dll = fs.readFileSync(dllPath);
    const dp = M._malloc(dll.length);
    M.HEAPU8.set(dll, dp);
    const rc = M._ladspa_w_load(dp, dll.length, SR);
    M._free(dp);
    console.log(`ladspa_load("${path.basename(dllPath)}") rc=${rc} in=${M._ladspa_w_numinputs()} out=${M._ladspa_w_numoutputs()}`);
    if (rc !== 0) { console.log('LOAD FAILED'); return; }

    M._ladspa_w_prepare(BS);
    if (macro) {
        const len = M.lengthBytesUTF8(macro) + 1;
        const mp = M._malloc(len); M.stringToUTF8(macro, mp, len);
        const applied = M._ladspa_w_apply_macro(mp); M._free(mp);
        console.log(`applied ${applied} params: ${macro}`);
    }

    // build a test tone (220+660 Hz, mono)
    const pcm = new Float32Array(N);
    for (let i = 0; i < N; i++) pcm[i] = 0.5*Math.sin(2*Math.PI*220*i/SR) + 0.3*Math.sin(2*Math.PI*660*i/SR);

    const inP = M._malloc(BS*4), outP = M._malloc(BS*4);
    const out = new Float32Array(N);
    let inRms = 0, outRms = 0, peak = 0, faulted = 0;
    for (let pos = 0; pos < N; pos += BS) {
        const n = Math.min(BS, N - pos);
        const H = M.HEAPF32, fi = inP >> 2;
        for (let i = 0; i < n; i++) H[fi+i] = pcm[pos+i];
        M._ladspa_w_process(inP, outP, n);
        if (M._ladspa_w_faulted()) { faulted = 1; console.log(`** faulted at pos=${pos}`); break; }
        const H2 = M.HEAPF32, fo = outP >> 2;
        for (let i = 0; i < n; i++) { const v = H2[fo+i]; out[pos+i] = v; if (Math.abs(v) > peak) peak = Math.abs(v); }
    }
    M._free(inP); M._free(outP);
    for (let i = 0; i < N; i++) { inRms += pcm[i]*pcm[i]; outRms += out[i]*out[i]; }
    console.log(`in.rms=${Math.sqrt(inRms/N).toFixed(4)} out.rms=${Math.sqrt(outRms/N).toFixed(4)} peak=${peak.toFixed(4)} faulted=${faulted}`);
})();
