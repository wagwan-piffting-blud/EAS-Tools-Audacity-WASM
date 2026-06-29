// probe_sc4.c - call sc4's pure helper funcs (db2lin/lin2db/sqrt) UNDER THE EMULATOR
// across a sweep, dumping CSV to compare vs a native real run (probe_sc4_native.c).
// Build with: cl ... mem.c cpu.c loader.c win32_vst.c ladspa_host.c probe_sc4.c /Fe:probe_sc4.exe
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ladspa_load(const char* dll, float sr);
void ladspa_prepare(int cap);
int ladspa_apply_macro(const char* s);
void ladspa_run(const float* in, float* out, int n);
uint32_t ladspa_handle(void);

#define DB2LIN 0x10001000u
#define LIN2DB 0x10001090u
#define SQRTF  0x100020c0u

static float call_f(uint32_t fn, float x){
    uint32_t a; memcpy(&a,&x,4);
    uint32_t args[1]={a};
    CPU.fpu_cw = 0x027F;   // match native probe (PC=53), call_guest empties the stack
    call_guest(fn, args, 1);
    return (float)CPU.st[CPU.fpu_top & 7];
}

int main(int argc, char** argv){
    const char* dll = argc>1?argv[1]:"D:/Programs/Audacity242/Plug-Ins/sc4_1882.dll";
    const char* out = argc>2?argv[2]:"emu_probe.csv";
    if(ladspa_load(dll, 44100.0f)!=0){ fprintf(stderr,"load failed\n"); return 1; }
    ladspa_prepare(512);
    FILE* f=fopen(out,"wb");
    fprintf(f,"func,x,y\n");
    // db2lin: dB input in [-90, 20]
    for(int i=0;i<=11000;i++){ double x=-90.0 + i*0.01; fprintf(f,"db2lin,%.6f,%.9g\n",x,call_f(DB2LIN,(float)x)); }
    // lin2db: linear input in [1e-6, 3]
    for(int i=0;i<=12000;i++){ double x=1e-6 + i*0.00025; fprintf(f,"lin2db,%.6f,%.9g\n",x,call_f(LIN2DB,(float)x)); }
    // sqrt: input in [0, 4]
    for(int i=0;i<=8000;i++){ double x=i*0.0005; fprintf(f,"sqrt,%.6f,%.9g\n",x,call_f(SQRTF,(float)x)); }
    fclose(f);
    fprintf(stderr,"wrote %s\n",out);

    // set sc4 macro params, run a DC block, dump handle coefficient region
    ladspa_apply_macro("Attack_time_(ms)=\"10\" Knee_radius_(dB)=\"1\" Makeup_gain_(dB)=\"12\" Ratio_(1_n)=\"20\" Release_time_(ms)=\"800\" RMS_peak=\"0\" Threshold_level_(dB)=\"-30\"");
    float din[512], dout[512];
    for(int i=0;i<512;i++) din[i]=0.3f;
    for(int b=0;b<8;b++) ladspa_run(din,dout,512);  // run a few blocks so coefs/state settle
    uint32_t h = ladspa_handle();
    fprintf(stderr,"[emu] handle=%08x  out[0..3]=%.5f %.5f %.5f %.5f\n", h, dout[0],dout[1],dout[2],dout[3]);
    fprintf(stderr,"[emu] handle floats [0x00..0x110]:\n");
    for(int off=0; off<0x110; off+=4){ uint32_t u=rd32(h+off); float fv; memcpy(&fv,&u,4);
        fprintf(stderr,"  +0x%02x = %08x = %.9g\n", off, u, fv); }
    uint32_t asbase = rd32(h+0x38);   // as[] table pointer (guest VA)
    fprintf(stderr,"[emu] as[] @%08x:\n", asbase);
    for(int i=0;i<12;i++){ uint32_t u=rd32(asbase+i*4); float fv; memcpy(&fv,&u,4); fprintf(stderr,"  as[%d]=%.9g\n", i, fv); }
    { uint32_t u=rd32(asbase+204*4); float fv; memcpy(&fv,&u,4); fprintf(stderr,"  as[204]=%.9g\n", fv); }
    return 0;
}
