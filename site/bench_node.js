// WASM benchmark in Node: load a plugin, process a buffer, report xRealtime.
const factory = require('./vstemu.js');
const fs = require('fs'), path = require('path');
(async () => {
    const M = await factory();
    const dll = fs.readFileSync(process.argv[2] || path.join(__dirname, '..', '..', 'AllPassPhase.dll'));
    const dp = M._malloc(dll.length); M.HEAPU8.set(dll, dp);
    const rc = M._vst_w_load(dp, dll.length); M._free(dp);
    if (rc !== 0) { console.log('load rc=', rc); return; }
    const SR = 44100, BS = 512, N = SR; // 1s
    M._vst_w_samplerate(SR); M._vst_w_blocksize(BS); M._vst_w_resume();
    const inL = M._malloc(BS*4), inR = M._malloc(BS*4), outL = M._malloc(BS*4), outR = M._malloc(BS*4);
    const reps = 5;
    const t0 = Date.now();
    for (let r=0;r<reps;r++) for (let pos=0;pos<N;pos+=BS) {
        const n = Math.min(BS, N-pos), H = M.HEAPF32, fi=inL>>2, fr=inR>>2;
        for (let i=0;i<n;i++){ const s=0.4*Math.sin(2*Math.PI*220*(pos+i)/SR); H[fi+i]=s; H[fr+i]=s; }
        M._vst_w_process(inL, inR, outL, outR, n);
    }
    const ms = Date.now()-t0, audioSec = N*reps/SR;
    console.log(`${path.basename(process.argv[2]||'AllPassPhase.dll')}: ${ms}ms for ${audioSec}s audio = ${(audioSec/(ms/1000)).toFixed(2)}x realtime`);
})();
