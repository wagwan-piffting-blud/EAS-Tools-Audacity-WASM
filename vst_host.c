// vst_host.c - host orchestration for a VST2 plugin running under the x86 emulator.
// Loads the DLL, exposes audioMaster as a guest-callable callback, parses the AEffect
// struct from guest memory, and drives dispatch/setParameter/processReplacing.
#include "emu.h"
#include "vst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

int EMU_VERBOSE = 0;
void emu_log(const char* fmt, ...){ if(!EMU_VERBOSE) return; va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }

vst_plugin_t VST;
float VST_SAMPLE_RATE = 44100.0f;
int   VST_BLOCK_SIZE  = 512;

static uint32_t g_audioMaster_va = 0;
// reusable guest-side audio buffers (laid out in VST_SCRATCH)
static uint32_t g_inPtrArr=0, g_outPtrArr=0;     // arrays of channel-buffer VAs
static uint32_t g_inBuf[16], g_outBuf[16];       // per-channel float buffers
static int      g_buf_cap = 0;                   // frames capacity currently allocated

// ---- call a guest function (cdecl/stdcall); returns EAX ----
uint32_t call_guest(uint32_t fn, const uint32_t* args, int n){
    CPU.r[ESP] = STACK_ESP0;
    for(int i=n-1;i>=0;i--) cpu_push32(args[i]);
    cpu_push32(RET_SENTINEL);
    CPU.eip = fn;
    CPU.halted=0; CPU.faulted=0;
    CPU.fpu_top=8; CPU.fpu_sw=0;          // FPU stack empty at call boundary (cdecl)
    int rc = cpu_run(2000000000ULL);
    if(rc==0) emu_log("[call_guest] fn=%08x ran out of instructions\n", fn);
    return CPU.r[EAX];
}
static float call_guest_f(uint32_t fn, const uint32_t* args, int n){
    call_guest(fn, args, n);
    return (float)CPU.st[CPU.fpu_top & 7];   // x87 return value in ST(0)
}

static inline uint32_t f2u(float f){ uint32_t u; memcpy(&u,&f,4); return u; }

// ======================= audioMaster (plugin -> host) =======================
static void s_audioMaster(void){
    uint32_t opcode = arg32(1);
    uint32_t index  = arg32(2);
    uint32_t value  = arg32(3);
    uint32_t ptr    = arg32(4);
    (void)index; (void)value;
    switch(opcode){
        case audioMasterVersion:                  ret_set(2400); return;
        case audioMasterCurrentId:                ret_set(0); return;
        case audioMasterGetSampleRate:            ret_set((uint32_t)VST_SAMPLE_RATE); return;
        case audioMasterGetBlockSize:             ret_set((uint32_t)VST_BLOCK_SIZE); return;
        case audioMasterGetCurrentProcessLevel:   ret_set(2); return;      // realtime
        case audioMasterGetAutomationState:       ret_set(0); return;
        case audioMasterGetVendorVersion:         ret_set(1); return;
        case audioMasterGetLanguage:              ret_set(1); return;      // English
        case audioMasterGetInputLatency:          ret_set(0); return;
        case audioMasterGetOutputLatency:         ret_set(0); return;
        case audioMasterCanDo:                    ret_set(0); return;
        case audioMasterGetTime:                  ret_set(0); return;      // no VstTimeInfo
        case audioMasterIOChanged:                ret_set(1); return;
        case audioMasterSizeWindow:               ret_set(1); return;
        case audioMasterUpdateDisplay:            ret_set(1); return;
        case audioMasterBeginEdit: case audioMasterEndEdit: ret_set(1); return;
        case audioMasterAutomate:                 ret_set(0); return;
        case audioMasterGetVendorString:
        case audioMasterGetProductString:
            if(ptr){ const char* s="x86emu"; int i=0; for(;s[i];i++) wr8(ptr+i,s[i]); wr8(ptr+i,0); }
            ret_set(1); return;
        default:
            emu_log("[audioMaster] unhandled opcode=%u\n", opcode);
            ret_set(0); return;
    }
}

// ======================= load =======================
static uint32_t resolve_entry(void){
    const char* names[] = { "VSTPluginMain", "main", "MAIN", "main_plugin", 0 };
    for(int i=0; names[i]; i++){ uint32_t v=pe_get_export(names[i]); if(v){ emu_log("[vst] entry=%s @%08x\n",names[i],v); return v; } }
    return 0;
}

static int vst_init_common(void){
    win32_init();
    pe_init_tls();   // implicit TLS (must precede DllMain/ctors that touch thread-locals)
    g_audioMaster_va = host_register_callback(s_audioMaster, 0 /*cdecl*/, "audioMaster");
    emu_log("[vst] running DllMain...\n");
    pe_run_dllmain();
    if(CPU.faulted){ fprintf(stderr,"[vst] DllMain FAULTED @%08x (%s)\n", CPU.fault_addr, CPU.fault_msg?CPU.fault_msg:"?"); return -1; }

    uint32_t entry = resolve_entry();
    if(!entry){ fprintf(stderr,"[vst] no VST entry export found\n"); return -2; }
    uint32_t a[1] = { g_audioMaster_va };
    uint32_t aeff = call_guest(entry, a, 1);
    if(CPU.faulted || !aeff){ fprintf(stderr,"[vst] entry() faulted=%d aeffect=%08x\n", CPU.faulted, aeff); return -3; }

    uint32_t magic = rd32(aeff + AE_magic);
    if(magic != AE_MAGIC){ fprintf(stderr,"[vst] bad AEffect magic %08x (want %08x)\n", magic, AE_MAGIC); return -4; }

    memset(&VST,0,sizeof VST);
    VST.aeffect          = aeff;
    VST.dispatcher       = rd32(aeff + AE_dispatcher);
    VST.setParameter     = rd32(aeff + AE_setParameter);
    VST.getParameter     = rd32(aeff + AE_getParameter);
    VST.processReplacing = rd32(aeff + AE_processReplacing);
    VST.numPrograms      = (int)rd32(aeff + AE_numPrograms);
    VST.numParams        = (int)rd32(aeff + AE_numParams);
    VST.numInputs        = (int)rd32(aeff + AE_numInputs);
    VST.numOutputs       = (int)rd32(aeff + AE_numOutputs);
    VST.flags            = rd32(aeff + AE_flags);
    VST.initialDelay     = rd32(aeff + AE_initialDelay);
    VST.uniqueID         = rd32(aeff + AE_uniqueID);
    VST.version          = rd32(aeff + AE_version);
    VST.canReplacing     = (VST.flags & effFlagsCanReplacing) ? 1 : 0;

    emu_log("[vst] AEffect@%08x in=%d out=%d params=%d progs=%d flags=%08x uid=%08x replacing=%d disp=%08x proc=%08x\n",
        aeff, VST.numInputs, VST.numOutputs, VST.numParams, VST.numPrograms, VST.flags, VST.uniqueID,
        VST.canReplacing, VST.dispatcher, VST.processReplacing);

    // effOpen
    vst_dispatch(effOpen, 0, 0, 0, 0.0f);
    return 0;
}

int vst_load(const char* dll_path){
    mem_init();
    cpu_reset();
    win32_reset();   // fresh import table per load (imports must not accumulate across a macro chain)
    // mem_init wiped/remapped all guest memory, so the cached I/O buffer VAs from a previous
    // plugin in the chain are stale. Force vst_ensure_buffers to re-allocate, else the 2nd
    // plugin reads a zeroed pointer array -> null channel buffer -> fault (dblue_Crusher).
    g_buf_cap = 0; g_inPtrArr = 0; g_outPtrArr = 0;
    for(int i=0;i<16;i++){ g_inBuf[i]=0; g_outBuf[i]=0; }
    if(pe_load(dll_path)!=0){ fprintf(stderr,"[vst] pe_load failed: %s\n", dll_path); return -10; }
    mem_map(VST_SCRATCH, VST_SCRATCH_SZ, "vst_scratch");
    if(getenv("EMU_IATDUMP")){
        for(uint32_t a=0x10015114; a<=0x10015200; a+=4) fprintf(stderr,"  [iat %08x] = %08x\n", a, rd32(a));
    }
    return vst_init_common();
}

// ======================= control =======================
int64_t vst_dispatch(int opcode, int index, int32_t value, uint32_t ptr, float opt){
    if(!VST.dispatcher) return 0;
    uint32_t a[6] = { VST.aeffect, (uint32_t)opcode, (uint32_t)index, (uint32_t)value, ptr, f2u(opt) };
    return (int32_t)call_guest(VST.dispatcher, a, 6);
}
void vst_set_param(int index, float value){
    if(!VST.setParameter) return;
    uint32_t a[3] = { VST.aeffect, (uint32_t)index, f2u(value) };
    call_guest(VST.setParameter, a, 3);
}
float vst_get_param(int index){
    if(!VST.getParameter) return 0.0f;
    uint32_t a[2] = { VST.aeffect, (uint32_t)index };
    return call_guest_f(VST.getParameter, a, 2);
}
void vst_set_samplerate(float sr){ VST_SAMPLE_RATE = sr; vst_dispatch(effSetSampleRate, 0, 0, 0, sr); }
void vst_set_blocksize(int bs){ VST_BLOCK_SIZE = bs; vst_dispatch(effSetBlockSize, 0, bs, 0, 0.0f); }
void vst_resume(void){ vst_dispatch(effMainsChanged, 0, 1, 0, 0.0f); }
void vst_suspend(void){ vst_dispatch(effMainsChanged, 0, 0, 0, 0.0f); }

// ---- parameter-by-name (Audacity macros reference params by name) ----
int vst_get_param_name(int index, char* out, int outsz){
    uint32_t buf = VST_SCRATCH + VST_SCRATCH_SZ - 0x280;
    for(int i=0;i<32;i+=4) wr32(buf+i,0);
    vst_dispatch(effGetParamName, index, 0, buf, 0.0f);
    int i=0; for(; i<outsz-1; i++){ char c=(char)rd8(buf+i); out[i]=c; if(!c) break; } out[i<outsz?i:outsz-1]=0;
    while(i>0 && out[i-1]==' ') out[--i]=0;          // trim trailing pad
    int lead=0; while(out[lead]==' ') lead++;         // mda right-justifies names with leading spaces
    if(lead){ int j=0; while(out[lead+j]){ out[j]=out[lead+j]; j++; } out[j]=0; i=j; }
    return i;
}
// Case-insensitive compare that ignores non-alphanumeric characters. Audacity rewrites a
// plugin's param name into the macro key by replacing spaces/punctuation with '_' (e.g.
// plugin "Bit 1" / "Make-Up Gain" -> macro "Bit_1" / "Make-Up_Gain"), so a strict compare
// would drop every param whose name contains a space or dash.
static int ci_eq(const char* a, const char* b){
    for(;;){
        while(*a && !((*a>='a'&&*a<='z')||(*a>='A'&&*a<='Z')||(*a>='0'&&*a<='9'))) a++;
        while(*b && !((*b>='a'&&*b<='z')||(*b>='A'&&*b<='Z')||(*b>='0'&&*b<='9'))) b++;
        if(!*a || !*b) return *a==*b;
        char x=*a,y=*b; if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32;
        if(x!=y) return 0;
        a++; b++;
    }
}
int vst_param_index(const char* name){
    char pn[80];
    for(int i=0;i<VST.numParams;i++){ vst_get_param_name(i,pn,sizeof pn); if(ci_eq(pn,name)) return i; }
    return -1;
}
int vst_set_param_by_name(const char* name, float v){ int i=vst_param_index(name); if(i>=0) vst_set_param(i,v); return i; }

// Apply an Audacity macro parameter string:  Name="value" Name2="value2" ...
// (also tolerates Name=value without quotes, comma or space separated). Returns count applied.
int vst_apply_macro(const char* s){
    int applied=0; char key[80]; char val[48];
    while(*s){
        while(*s==' '||*s==','||*s=='\t') s++;
        if(!*s) break;
        int k=0; while(*s && *s!='=' && k<79){ key[k++]=*s++; } key[k]=0;
        while(k>0 && key[k-1]==' ') key[--k]=0;
        if(*s!='=') break; s++;
        if(*s=='"') s++;
        int v=0; while(*s && *s!='"' && *s!=' ' && *s!=',' && v<47){ val[v++]=*s++; } val[v]=0;
        if(*s=='"') s++;
        if(k>0){ int idx; float fv=(float)atof(val);
            if(key[0]>='0'&&key[0]<='9'){ idx=atoi(key); vst_set_param(idx,fv); applied++; }
            else { idx=vst_set_param_by_name(key,fv); if(idx>=0) applied++; else emu_log("[macro] no param '%s'\n",key); }
        }
    }
    return applied;
}

int vst_get_string(int opcode, char* out, int outsz){
    uint32_t buf = VST_SCRATCH + VST_SCRATCH_SZ - 0x100;  // scratch tail
    for(int i=0;i<64;i+=4) wr32(buf+i,0);
    vst_dispatch(opcode, 0, 0, buf, 0.0f);
    int i=0; for(; i<outsz-1; i++){ char c=(char)rd8(buf+i); out[i]=c; if(!c) break; } out[i<outsz?i:outsz-1]=0;
    return i;
}

// ======================= process =======================
static void ensure_buffers(int nframes){
    int ni = VST.numInputs  < 16 ? VST.numInputs  : 16;
    int no = VST.numOutputs < 16 ? VST.numOutputs : 16;
    if(nframes <= g_buf_cap && g_inPtrArr) return;
    g_buf_cap = nframes;
    g_inPtrArr  = guest_alloc((ni?ni:1)*4, 1);
    g_outPtrArr = guest_alloc((no?no:1)*4, 1);
    for(int c=0;c<ni;c++){ g_inBuf[c]  = guest_alloc(nframes*4, 1); wr32(g_inPtrArr  + c*4, g_inBuf[c]); }
    for(int c=0;c<no;c++){ g_outBuf[c] = guest_alloc(nframes*4, 1); wr32(g_outPtrArr + c*4, g_outBuf[c]); }
}

void vst_process(const float* const* in, float* const* out, int nframes){
    if(!VST.processReplacing || nframes<=0) return;
    ensure_buffers(nframes);
    int ni = VST.numInputs  < 16 ? VST.numInputs  : 16;
    int no = VST.numOutputs < 16 ? VST.numOutputs : 16;
    // host -> guest input
    for(int c=0;c<ni;c++){
        const float* src = in[c];
        uint32_t dst = g_inBuf[c];
        for(int i=0;i<nframes;i++) wr32(dst + i*4, f2u(src[i]));
    }
    for(int c=0;c<no;c++){ uint32_t b=g_outBuf[c]; for(int i=0;i<nframes;i++) wr32(b+i*4, 0); }

    uint32_t a[4] = { VST.aeffect, g_inPtrArr, g_outPtrArr, (uint32_t)nframes };
    static int dbg=-1; if(dbg<0) dbg = getenv("EMU_PROCDBG")?1:0;
    if(dbg==1){ dbg=2;
        uint32_t obj=rd32(VST.aeffect+0x40), vt=obj?rd32(obj):0, dsp=vt?rd32(vt+0x14):0;
        emu_log("[proc] procVA=%08x object=%08x vtable=%08x vtable[5]=%08x  in0[0..2]=%08x %08x %08x\n",
            VST.processReplacing, obj, vt, dsp, rd32(g_inBuf[0]), rd32(g_inBuf[0]+4), rd32(g_inBuf[0]+8));
        emu_log("[proc] obj coeffs @+0xb0:"); for(uint32_t o=0xb0;o<=0xc8;o+=4){ uint32_t u=rd32(obj+o); float f; memcpy(&f,&u,4); emu_log(" [%02x]=%g",o,f);} emu_log("\n");
        call_guest(VST.processReplacing, a, 4);
        emu_log("[proc] faulted=%d fpu_top=%d out0[0..2]=%08x %08x %08x  out1[0..2]=%08x %08x %08x\n",
            CPU.faulted, CPU.fpu_top, rd32(g_outBuf[0]), rd32(g_outBuf[0]+4), rd32(g_outBuf[0]+8),
            no>1?rd32(g_outBuf[1]):0, no>1?rd32(g_outBuf[1]+4):0, no>1?rd32(g_outBuf[1]+8):0);
    } else
    call_guest(VST.processReplacing, a, 4);

    // guest -> host output
    for(int c=0;c<no;c++){
        float* d = out[c];
        uint32_t s = g_outBuf[c];
        for(int i=0;i<nframes;i++){ uint32_t u=rd32(s+i*4); memcpy(&d[i],&u,4); }
    }
}
