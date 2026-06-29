// Smoke-test the WASM emulator in Node: load a plugin, apply a macro by name, process audio.
const factory = require('./vstemu.js');
const fs = require('fs');
const path = require('path');

(async () => {
    const M = await factory();
    const dllPath = process.argv[2] || path.join(__dirname, '..', '..', 'mda Overdrive.dll');
    const macro = process.argv[3] || 'Drive="0.052" Muffle="0.5" Output="0.3"';
    const dll = fs.readFileSync(dllPath);

    // load DLL bytes
    const dp = M._malloc(dll.length);
    M.HEAPU8.set(dll, dp);
    const rc = M._vst_w_load(dp, dll.length);
    M._free(dp);
    console.log(`load("${path.basename(dllPath)}") rc=${rc}  in=${M._vst_w_numinputs()} out=${M._vst_w_numoutputs()} params=${M._vst_w_numparams()} uid=0x${(M._vst_w_uniqueid()>>>0).toString(16)}`);
    if (rc !== 0) { console.log('LOAD FAILED'); return; }

    M._vst_w_samplerate(44100);
    M._vst_w_blocksize(512);
    // apply the Audacity macro param string BY NAME
    const mlen = M.lengthBytesUTF8 ? M.lengthBytesUTF8(macro) : macro.length;
    const mp = M._malloc(mlen + 1);
    M.stringToUTF8(macro, mp, mlen + 1);
    const applied = M._vst_w_apply_macro(mp);
    M._free(mp);
    console.log(`applied ${applied} params from macro: ${macro}`);
    M._vst_w_resume();

    // process a 220 Hz sine block
    const n = 512;
    const inL = M._malloc(n*4), inR = M._malloc(n*4), outL = M._malloc(n*4), outR = M._malloc(n*4);
    for (let i=0;i<n;i++){ const v = 0.5*Math.sin(2*Math.PI*220*i/44100); M.HEAPF32[(inL>>2)+i]=v; M.HEAPF32[(inR>>2)+i]=v; }
    M._vst_w_process(inL, inR, outL, outR, n);
    let peak=0, sum=0, nz=0;
    for (let i=0;i<n;i++){ const o = M.HEAPF32[(outL>>2)+i]; sum+=o*o; if(Math.abs(o)>peak)peak=Math.abs(o); if(Math.abs(o)>1e-6)nz++; }
    console.log(`out: peak=${peak.toFixed(4)} rms=${Math.sqrt(sum/n).toFixed(4)} nonzero=${nz}/${n} faulted=${M._vst_w_faulted()}`);
    console.log(peak>1e-4 && M._vst_w_faulted()===0 ? 'WASM SMOKE TEST: PASS' : 'WASM SMOKE TEST: FAIL');
})();
