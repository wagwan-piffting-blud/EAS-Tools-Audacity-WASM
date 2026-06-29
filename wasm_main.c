// wasm_main.c - Emscripten-exported API for hosting an x86 VST2 plugin in the browser.
// Build with build_wasm.ps1 (emcc). The DLL bytes are handed in from JS, written to
// Emscripten MEMFS, then loaded via the same pe_load path used natively.
//
// JS flow:
//   const ok   = Module._vst_w_load(ptr, len);          // ptr = malloc'd DLL bytes
//   Module._vst_w_samplerate(44100); Module._vst_w_blocksize(512);
//   for(p=0;p<Module._vst_w_numparams();p++) Module._vst_w_setparam(p, v);
//   Module._vst_w_resume();
//   Module._vst_w_process(inLptr,inRptr,outLptr,outRptr, n);  // Float32 heap views
#include <emscripten.h>
#include "emu.h"
#include "vst.h"
#include <stdio.h>
#include <string.h>

EMSCRIPTEN_KEEPALIVE int vst_w_load(const unsigned char* data, int len){
    FILE* f = fopen("/plugin.dll","wb");
    if(!f) return -100;
    fwrite(data,1,len,f); fclose(f);
    return vst_load("/plugin.dll");   // 0 on success
}
EMSCRIPTEN_KEEPALIVE int   vst_w_numparams(void){ return VST.numParams; }
EMSCRIPTEN_KEEPALIVE int   vst_w_numinputs(void){ return VST.numInputs; }
EMSCRIPTEN_KEEPALIVE int   vst_w_numoutputs(void){ return VST.numOutputs; }
EMSCRIPTEN_KEEPALIVE unsigned vst_w_uniqueid(void){ return VST.uniqueID; }
EMSCRIPTEN_KEEPALIVE void  vst_w_samplerate(float sr){ vst_set_samplerate(sr); }
EMSCRIPTEN_KEEPALIVE void  vst_w_blocksize(int bs){ vst_set_blocksize(bs); }
EMSCRIPTEN_KEEPALIVE void  vst_w_resume(void){ vst_resume(); }
EMSCRIPTEN_KEEPALIVE void  vst_w_suspend(void){ vst_suspend(); }
EMSCRIPTEN_KEEPALIVE void  vst_w_setparam(int i, float v){ vst_set_param(i,v); }
EMSCRIPTEN_KEEPALIVE float vst_w_getparam(int i){ return vst_get_param(i); }
// macro support: resolve params by name / apply a raw Audacity macro param string
EMSCRIPTEN_KEEPALIVE int   vst_w_param_index(const char* name){ return vst_param_index(name); }
EMSCRIPTEN_KEEPALIVE int   vst_w_setparam_byname(const char* name, float v){ return vst_set_param_by_name(name,v); }
EMSCRIPTEN_KEEPALIVE int   vst_w_apply_macro(const char* s){ return vst_apply_macro(s); }

// Stereo process. JS passes four Float32 heap pointers (mono buffers per channel)
// and the frame count. Mono plugins/inputs are handled by vst_process's channel clamps.
EMSCRIPTEN_KEEPALIVE void vst_w_process(float* inL, float* inR, float* outL, float* outR, int n){
    const float* in[2]  = { inL,  inR  };
    float*       out[2] = { outL, outR };
    vst_process(in, out, n);
}

// String getters write a NUL-terminated ASCII string into the caller's heap buffer.
EMSCRIPTEN_KEEPALIVE int vst_w_effectname(char* out, int outsz){ return vst_get_string(effGetEffectName, out, outsz); }
EMSCRIPTEN_KEEPALIVE int vst_w_vendor(char* out, int outsz){ return vst_get_string(effGetVendorString, out, outsz); }
EMSCRIPTEN_KEEPALIVE int vst_w_paramname(int idx, char* out, int outsz){
    uint32_t buf = VST_SCRATCH + VST_SCRATCH_SZ - 0x300;
    for(int i=0;i<64;i+=4) wr32(buf+i,0);
    vst_dispatch(effGetParamName, idx, 0, buf, 0.0f);
    int i=0; for(; i<outsz-1; i++){ char c=(char)rd8(buf+i); out[i]=c; if(!c) break; } out[i<outsz?i:outsz-1]=0;
    return i;
}
EMSCRIPTEN_KEEPALIVE int vst_w_paramdisplay(int idx, char* out, int outsz){
    uint32_t buf = VST_SCRATCH + VST_SCRATCH_SZ - 0x300;
    for(int i=0;i<64;i+=4) wr32(buf+i,0);
    vst_dispatch(effGetParamDisplay, idx, 0, buf, 0.0f);
    int i=0; for(; i<outsz-1; i++){ char c=(char)rd8(buf+i); out[i]=c; if(!c) break; } out[i<outsz?i:outsz-1]=0;
    return i;
}
// Did the last operation fault? (CPU trap / unmapped memory / unhandled opcode)
EMSCRIPTEN_KEEPALIVE int vst_w_faulted(void){ return CPU.faulted; }

// ============================ LADSPA (sc4, chebstortion) ============================
// LADSPA plugins are pure-C DSP (no GUI/AEffect). The engine routes here when the plugin
// manifest marks a plugin kind="ladspa". Mono I/O: the engine's buffer is mono; ladspa_run
// feeds it to every audio-input port and reads output port 0 (stereo plugins -> dual-mono).
EMSCRIPTEN_KEEPALIVE int ladspa_w_load(const unsigned char* data, int len, float sr){
    FILE* f = fopen("/plugin.dll","wb");
    if(!f) return -100;
    fwrite(data,1,len,f); fclose(f);
    return ladspa_load("/plugin.dll", sr);   // 0 on success
}
EMSCRIPTEN_KEEPALIVE void ladspa_w_prepare(int cap){ ladspa_prepare(cap); }
EMSCRIPTEN_KEEPALIVE int  ladspa_w_numinputs(void){ return ladspa_numinputs(); }
EMSCRIPTEN_KEEPALIVE int  ladspa_w_numoutputs(void){ return ladspa_numoutputs(); }
EMSCRIPTEN_KEEPALIVE int  ladspa_w_apply_macro(const char* s){ return ladspa_apply_macro(s); }
EMSCRIPTEN_KEEPALIVE void ladspa_w_process(float* in, float* out, int n){ ladspa_run(in, out, n); }
EMSCRIPTEN_KEEPALIVE int  ladspa_w_faulted(void){ return CPU.faulted; }
