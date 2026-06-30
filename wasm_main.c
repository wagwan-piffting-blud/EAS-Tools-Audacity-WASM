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
#include <stddef.h>   // offsetof, for the JIT layout export

// ---------------- JIT layout exports ----------------
// The JS code generator needs the runtime address of the global CPU struct, the page-map base, and the
// byte offset of every cpu_t field it loads/stores. These are baked from offsetof at startup and handed
// to JS so a field reorder in emu.h can never silently corrupt generated code.
extern cpu_t CPU;
extern uint32_t mem_pagemap_base(void);

extern int g_jit_enabled, g_jit_force, g_jit_threshold;
extern unsigned long long g_jit_calls, g_jit_deopts, g_jit_compiles;
extern void jit_reset(void);
EMSCRIPTEN_KEEPALIVE void jit_set_enabled(int on){ g_jit_enabled = on; if(on) jit_reset(); }
EMSCRIPTEN_KEEPALIVE void jit_set_force(int on){ g_jit_force = on; }
EMSCRIPTEN_KEEPALIVE void jit_set_threshold(int n){ g_jit_threshold = n; }
EMSCRIPTEN_KEEPALIVE void jit_stats(void){
    fprintf(stderr,"[jit] compiles=%llu block-calls=%llu deopts=%llu\n",
            (unsigned long long)g_jit_compiles,(unsigned long long)g_jit_calls,(unsigned long long)g_jit_deopts);
}

EMSCRIPTEN_KEEPALIVE uint32_t jit_cpu_addr(void){ return (uint32_t)(uintptr_t)&CPU; }
EMSCRIPTEN_KEEPALIVE uint32_t jit_pagemap_base(void){ return mem_pagemap_base(); }

// Order MUST match the JS side (assets/js/jit/layout indices).
enum {
    JL_R=0, JL_EIP, JL_EFLAGS, JL_FS, JL_GS, JL_ST, JL_TOP, JL_FPUSW, JL_FPUCW,
    JL_XMM, JL_MXCSR, JL_HALTED, JL_FAULTADDR, JL_FAULTED, JL_N
};
static uint32_t g_jit_layout[JL_N];
EMSCRIPTEN_KEEPALIVE uint32_t jit_layout(void){
    g_jit_layout[JL_R]        = offsetof(cpu_t, r);
    g_jit_layout[JL_EIP]      = offsetof(cpu_t, eip);
    g_jit_layout[JL_EFLAGS]   = offsetof(cpu_t, eflags);
    g_jit_layout[JL_FS]       = offsetof(cpu_t, seg_fs_base);
    g_jit_layout[JL_GS]       = offsetof(cpu_t, seg_gs_base);
    g_jit_layout[JL_ST]       = offsetof(cpu_t, st);
    g_jit_layout[JL_TOP]      = offsetof(cpu_t, fpu_top);
    g_jit_layout[JL_FPUSW]    = offsetof(cpu_t, fpu_sw);
    g_jit_layout[JL_FPUCW]    = offsetof(cpu_t, fpu_cw);
    g_jit_layout[JL_XMM]      = offsetof(cpu_t, xmm);
    g_jit_layout[JL_MXCSR]    = offsetof(cpu_t, mxcsr);
    g_jit_layout[JL_HALTED]   = offsetof(cpu_t, halted);
    g_jit_layout[JL_FAULTADDR]= offsetof(cpu_t, fault_addr);
    g_jit_layout[JL_FAULTED]  = offsetof(cpu_t, faulted);
    return (uint32_t)(uintptr_t)g_jit_layout;
}

// JIT x87 trampolines (cpu.c). Taking their addresses forces a function-table slot; the returned indices
// are what compiled blocks call_indirect. Order MUST match jit-compiler.js helper indexing.
extern int32_t jit_f2i(double);
extern double  jit_frndint(double), jit_f2xm1(double), jit_ftan(double), jit_fsin(double), jit_fcos(double);
extern double  jit_fyl2x(double,double), jit_fpatan(double,double), jit_fscale(double,double);
static uint32_t g_jit_helpers[16];
EMSCRIPTEN_KEEPALIVE uint32_t jit_helpers(void){
    g_jit_helpers[0] = (uint32_t)(uintptr_t)&jit_f2i;
    g_jit_helpers[1] = (uint32_t)(uintptr_t)&jit_frndint;
    g_jit_helpers[2] = (uint32_t)(uintptr_t)&jit_f2xm1;
    g_jit_helpers[3] = (uint32_t)(uintptr_t)&jit_fyl2x;
    g_jit_helpers[4] = (uint32_t)(uintptr_t)&jit_ftan;
    g_jit_helpers[5] = (uint32_t)(uintptr_t)&jit_fpatan;
    g_jit_helpers[6] = (uint32_t)(uintptr_t)&jit_fsin;
    g_jit_helpers[7] = (uint32_t)(uintptr_t)&jit_fcos;
    g_jit_helpers[8] = (uint32_t)(uintptr_t)&jit_fscale;
    return (uint32_t)(uintptr_t)g_jit_helpers;
}

extern uint32_t jit_jitmap_addr(void);
EMSCRIPTEN_KEEPALIVE uint32_t jit_jitmap_base(void){ return jit_jitmap_addr(); }

extern unsigned long long g_ophist[1024];
extern unsigned long long g_insns;
static const char* opname(int idx){
    int o = idx & 0xff; int two = idx >= 256;
    if(two){
        if(o>=0x10&&o<=0x17) return "0F SSE mov";
        if(o>=0x28&&o<=0x2F) return "0F SSE mov/cvt";
        if(o>=0x51&&o<=0x5F) return "0F SSE arith";
        if(o>=0x54&&o<=0x57) return "0F SSE logic";
        if(o>=0x80&&o<=0x8F) return "0F Jcc";
        if(o>=0x90&&o<=0x9F) return "0F SETcc";
        if(o>=0x40&&o<=0x4F) return "0F CMOVcc";
        if(o==0xB6||o==0xB7) return "0F MOVZX";
        if(o==0xBE||o==0xBF) return "0F MOVSX";
        if(o==0xAF) return "0F IMUL";
        return "0F other";
    }
    if(o>=0xD8&&o<=0xDF) return "x87 FPU";
    if((o&0xC0)==0x00 && (o&0x07)<=5 && o<0x40) return "ALU r/m (add/or/adc/sbb/and/sub/xor/cmp)";
    if(o>=0x88&&o<=0x8B) return "MOV r/m";
    if(o>=0xB8&&o<=0xBF) return "MOV imm";
    if(o>=0x50&&o<=0x5F) return "PUSH/POP r";
    if(o>=0x70&&o<=0x7F) return "Jcc rel8";
    if(o>=0x40&&o<=0x4F) return "INC/DEC r";
    if(o==0xE8) return "CALL";
    if(o==0xC3||o==0xC2) return "RET";
    if(o==0xE9||o==0xEB) return "JMP";
    if(o==0xFF) return "grp5 (call/jmp/push r/m)";
    if(o==0x83||o==0x81) return "grp1 ALU r/m,imm";
    if(o==0x8D) return "LEA";
    if(o==0x84||o==0x85) return "TEST";
    if(o==0xC7||o==0xC6) return "MOV r/m,imm";
    return "other";
}
EMSCRIPTEN_KEEPALIVE void emu_ophist_dump(void){
    int idx[1024]; for(int i=0;i<1024;i++) idx[i]=i;
    for(int i=0;i<1024;i++) for(int j=i+1;j<1024;j++) if(g_ophist[idx[j]]>g_ophist[idx[i]]){int t=idx[i];idx[i]=idx[j];idx[j]=t;}
    fprintf(stderr,"=== opcode histogram (total %llu insns) ===\n",(unsigned long long)g_insns);
    unsigned long long tot=0; for(int i=0;i<1024;i++) tot+=g_ophist[i]; if(!tot)tot=1;
    for(int k=0;k<25;k++){ int i=idx[k]; if(!g_ophist[i])break;
        fprintf(stderr,"  %5.1f%%  %12llu  %s %02X  %s\n", 100.0*g_ophist[i]/tot,(unsigned long long)g_ophist[i], (i>=256?"0F":"  "), i&0xff, opname(i)); }
}
EMSCRIPTEN_KEEPALIVE void emu_ophist_reset(void){ for(int i=0;i<1024;i++) g_ophist[i]=0; g_insns=0; }
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
