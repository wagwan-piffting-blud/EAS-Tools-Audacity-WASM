// ladspa_host.c - host a LADSPA plugin DLL under the x86 emulator. LADSPA is a pure-C
// DSP API (no GUI): ladspa_descriptor(i) -> descriptor with instantiate/connect_port/run.
// Reuses mem/cpu/loader/win32_vst. Self-contained call_guest + logging so it can build
// without vst_host.c.
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#ifndef LADSPA_STANDALONE_LOG
int EMU_VERBOSE = 0;
void emu_log(const char* fmt, ...){ if(!EMU_VERBOSE) return; va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
uint32_t call_guest(uint32_t fn, const uint32_t* args, int n){
    CPU.r[ESP] = STACK_ESP0;
    for(int i=n-1;i>=0;i--) cpu_push32(args[i]);
    cpu_push32(RET_SENTINEL);
    CPU.eip = fn; CPU.halted=0; CPU.faulted=0; CPU.fpu_top=8; CPU.fpu_sw=0;
    cpu_run(2000000000ULL);
    return CPU.r[EAX];
}
static float call_guest_f(uint32_t fn, const uint32_t* args, int n){ call_guest(fn,args,n); return (float)CPU.st[CPU.fpu_top&7]; }
#endif

// LADSPA_Descriptor field offsets (32-bit)
#define LD_UID            0x00
#define LD_Label          0x04
#define LD_Name           0x0C
#define LD_PortCount      0x18
#define LD_PortDescriptors 0x1C
#define LD_PortNames      0x20
#define LD_PortRangeHints 0x24
#define LD_instantiate    0x2C
#define LD_connect_port   0x30
#define LD_activate       0x34
#define LD_run            0x38
#define LD_deactivate     0x44
#define LD_cleanup        0x48

#define LADSPA_PORT_INPUT   1
#define LADSPA_PORT_OUTPUT  2
#define LADSPA_PORT_CONTROL 4
#define LADSPA_PORT_AUDIO   8
// PortRangeHint.HintDescriptor bits we use for defaults
#define HINT_BOUNDED_BELOW 0x1
#define HINT_BOUNDED_ABOVE 0x2
#define HINT_DEFAULT_MASK  0x3C0
#define HINT_DEFAULT_0     0x40
#define HINT_DEFAULT_1     0x80
#define HINT_DEFAULT_100   0xC0
#define HINT_DEFAULT_440   0x100
#define HINT_DEFAULT_MIN   0x140
#define HINT_DEFAULT_LOW   0x180
#define HINT_DEFAULT_MID   0x1C0
#define HINT_DEFAULT_HIGH  0x200
#define HINT_DEFAULT_MAX   0x240
#define HINT_SAMPLE_RATE   0x8
#define HINT_LOGARITHMIC   0x10

typedef struct {
    uint32_t desc, handle, instantiate, connect, run, activate;
    int nports;
    uint32_t portDesc, portNames, portHints;
    int nin, nout, nctrl;
    int inPort[16], outPort[16];      // audio port indices
    uint32_t inBuf[16], outBuf[16];   // guest float buffers per audio port
    uint32_t ctrlVA;                  // guest float[nports] for control values
    int cap;
    float SR;
} ladspa_t;
static ladspa_t L;

static float hint_default(uint32_t hint, float lo, float hi, float sr){
    uint32_t d = hint & HINT_DEFAULT_MASK;
    if(hint & HINT_SAMPLE_RATE){ lo*=sr; hi*=sr; }
    switch(d){
        case HINT_DEFAULT_0: return 0; case HINT_DEFAULT_1: return 1;
        case HINT_DEFAULT_100: return 100; case HINT_DEFAULT_440: return 440;
        case HINT_DEFAULT_MIN: return lo; case HINT_DEFAULT_MAX: return hi;
        case HINT_DEFAULT_LOW:  return (hint&HINT_LOGARITHMIC)? (float)exp(log(lo)*0.75+log(hi)*0.25) : lo*0.75f+hi*0.25f;
        case HINT_DEFAULT_MID:  return (hint&HINT_LOGARITHMIC)? (float)exp((log(lo)+log(hi))*0.5) : (lo+hi)*0.5f;
        case HINT_DEFAULT_HIGH: return (hint&HINT_LOGARITHMIC)? (float)exp(log(lo)*0.25+log(hi)*0.75) : lo*0.25f+hi*0.75f;
        default: return (hint&HINT_BOUNDED_BELOW)? lo : 0;
    }
}

int ladspa_load(const char* dll, float sr){
    memset(&L,0,sizeof L); L.SR=sr;
    mem_init(); cpu_reset(); win32_reset();
    if(pe_load(dll)!=0){ fprintf(stderr,"[ladspa] pe_load failed\n"); return -1; }
    mem_map(VST_SCRATCH, VST_SCRATCH_SZ, "scratch");
    win32_init();
    pe_run_dllmain();
    if(CPU.faulted){ fprintf(stderr,"[ladspa] DllMain faulted\n"); return -2; }
    uint32_t fn = pe_get_export("ladspa_descriptor");
    if(!fn){ fprintf(stderr,"[ladspa] no ladspa_descriptor export\n"); return -3; }
    uint32_t a0[1]={0};
    L.desc = call_guest(fn, a0, 1);
    if(!L.desc || CPU.faulted){ fprintf(stderr,"[ladspa] descriptor(0)=%08x faulted=%d\n",L.desc,CPU.faulted); return -4; }

    L.nports     = (int)rd32(L.desc+LD_PortCount);
    L.portDesc   = rd32(L.desc+LD_PortDescriptors);
    L.portNames  = rd32(L.desc+LD_PortNames);
    L.portHints  = rd32(L.desc+LD_PortRangeHints);
    L.instantiate= rd32(L.desc+LD_instantiate);
    L.connect    = rd32(L.desc+LD_connect_port);
    L.run        = rd32(L.desc+LD_run);
    L.activate   = rd32(L.desc+LD_activate);

    for(int p=0;p<L.nports;p++){
        uint32_t pd = rd32(L.portDesc + p*4);
        if((pd&LADSPA_PORT_AUDIO)){
            if(pd&LADSPA_PORT_INPUT){ if(L.nin<16) L.inPort[L.nin++]=p; }
            else { if(L.nout<16) L.outPort[L.nout++]=p; }
        } else if((pd&LADSPA_PORT_CONTROL)&&(pd&LADSPA_PORT_INPUT)) L.nctrl++;
    }
    char nm[64]={0}; uint32_t lab=rd32(L.desc+LD_Label); for(int i=0;i<63;i++){ char c=(char)rd8(lab+i); nm[i]=c; if(!c)break; }
    emu_log("[ladspa] '%s' uid=%u ports=%d (in=%d out=%d ctrl=%d)\n", nm, rd32(L.desc+LD_UID), L.nports, L.nin, L.nout, L.nctrl);

    uint32_t ia[2]={ L.desc, (uint32_t)sr };
    L.handle = call_guest(L.instantiate, ia, 2);
    if(!L.handle || CPU.faulted){ fprintf(stderr,"[ladspa] instantiate handle=%08x faulted=%d\n",L.handle,CPU.faulted); return -5; }
    return 0;
}

// connect ports + set control defaults; allocate guest buffers sized for `cap` frames.
void ladspa_prepare(int cap){
    L.cap = cap;
    L.ctrlVA = guest_alloc(L.nports*4, 1);
    for(int i=0;i<L.nin;i++)  L.inBuf[i]  = guest_alloc(cap*4, 1);
    for(int i=0;i<L.nout;i++) L.outBuf[i] = guest_alloc(cap*4, 1);
    // control inputs -> default from range hints
    for(int p=0;p<L.nports;p++){
        uint32_t pd = rd32(L.portDesc+p*4);
        if((pd&LADSPA_PORT_CONTROL)&&(pd&LADSPA_PORT_INPUT)){
            uint32_t h = rd32(L.portHints + p*12); uint32_t loB=rd32(L.portHints+p*12+4), hiB=rd32(L.portHints+p*12+8);
            float lo,hi; memcpy(&lo,&loB,4); memcpy(&hi,&hiB,4);
            float dv = hint_default(h, lo, hi, L.SR);
            uint32_t u; memcpy(&u,&dv,4); wr32(L.ctrlVA+p*4, u);
        }
        // connect every port to its data location
        uint32_t dataVA;
        if((pd&LADSPA_PORT_AUDIO)&&(pd&LADSPA_PORT_INPUT)){ int k=0; for(;k<L.nin;k++) if(L.inPort[k]==p)break; dataVA=L.inBuf[k]; }
        else if((pd&LADSPA_PORT_AUDIO)){ int k=0; for(;k<L.nout;k++) if(L.outPort[k]==p)break; dataVA=L.outBuf[k]; }
        else dataVA = L.ctrlVA + p*4;
        uint32_t ca[3]={ L.handle, (uint32_t)p, dataVA };
        call_guest(L.connect, ca, 3);
    }
    if(L.activate){ uint32_t aa[1]={L.handle}; call_guest(L.activate, aa, 1); }
}

// set a control-input port value by its (case-insensitive) name; returns port or -1
int ladspa_set_param(const char* name, float v){
    for(int p=0;p<L.nports;p++){
        uint32_t pd=rd32(L.portDesc+p*4); if(!((pd&LADSPA_PORT_CONTROL)&&(pd&LADSPA_PORT_INPUT))) continue;
        uint32_t np=rd32(L.portNames+p*4); char pn[64]={0}; for(int i=0;i<63;i++){ char c=(char)rd8(np+i); pn[i]=c; if(!c)break; }
        // normalize compare (case-insensitive, ignore non-alnum)
        const char *a=pn,*b=name; int match=1;
        while(*a||*b){ char x=*a,y=*b;
            while(x && !((x>='a'&&x<='z')||(x>='A'&&x<='Z')||(x>='0'&&x<='9'))){a++;x=*a;}
            while(y && !((y>='a'&&y<='z')||(y>='A'&&y<='Z')||(y>='0'&&y<='9'))){b++;y=*b;}
            if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; if(x!=y){match=0;break;} if(x)a++; if(y)b++; }
        if(match){ uint32_t u; memcpy(&u,&v,4); wr32(L.ctrlVA+p*4,u); return p; }
    }
    return -1;
}
void ladspa_get_param_name(int ctrlIdx, char* out, int osz){ /* (unused helper) */ (void)ctrlIdx;(void)out;(void)osz; }

// Apply an Audacity macro param string ('Name="v" Name2="v2" ...') to control-input ports
// by name. Mirrors vst_apply_macro's tolerant parser. Returns the count applied.
int ladspa_apply_macro(const char* s){
    int applied=0; char key[80]; char val[48];
    while(*s){
        while(*s==' '||*s==','||*s=='\t') s++;
        if(!*s) break;
        int k=0; while(*s && *s!='=' && k<79){ key[k++]=*s++; } key[k]=0;
        while(k>0 && key[k-1]==' ') key[--k]=0;
        if(*s!='=') break; s++;
        int quoted=0; if(*s=='"'){ quoted=1; s++; }
        int v=0;
        if(quoted){ while(*s && *s!='"' && v<47) val[v++]=*s++; if(*s=='"') s++; }
        else { while(*s && *s!=' ' && *s!=',' && v<47) val[v++]=*s++; }
        val[v]=0;
        if(k>0){ float fv=(float)atof(val); if(ladspa_set_param(key,fv)>=0) applied++; }
    }
    return applied;
}

// process one mono block: in[n] -> out[n]
void ladspa_run(const float* in, float* out, int n){
    if(n>L.cap) n=L.cap;
    // feed input to all audio-input ports; zero outputs
    for(int c=0;c<L.nin;c++){ uint32_t b=L.inBuf[c]; for(int i=0;i<n;i++){ uint32_t u; memcpy(&u,&in[i],4); wr32(b+i*4,u); } }
    for(int c=0;c<L.nout;c++){ uint32_t b=L.outBuf[c]; for(int i=0;i<n;i++) wr32(b+i*4,0); }
    uint32_t ra[2]={ L.handle, (uint32_t)n };
    call_guest(L.run, ra, 2);
    uint32_t src = L.nout? L.outBuf[0] : (L.nin?L.inBuf[0]:0);
    for(int i=0;i<n;i++){ uint32_t u = src?rd32(src+i*4):0; memcpy(&out[i],&u,4); }
}
int ladspa_numinputs(void){ return L.nin; }
int ladspa_numoutputs(void){ return L.nout; }
uint32_t ladspa_handle(void){ return L.handle; }
