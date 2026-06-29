// main_native.c - native driver: load an x86 VST under the emulator, push a test
// signal through processReplacing, write a WAV, and report stats proving DSP ran.
//   vstemu.exe "<plugin.dll>" [out.wav] [in.wav]
#include "emu.h"
#include "vst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// ---------------- tiny WAV I/O (16-bit PCM, mono/stereo) ----------------
static int16_t rdle16(const uint8_t* p){ return (int16_t)(p[0]|(p[1]<<8)); }
static uint32_t rdle32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

// returns interleaved float [0..1 channels], sets *ch,*n,*sr. caller frees.
static float* wav_read(const char* path, int* ch, int* n, int* sr){
    FILE* f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t* b=(uint8_t*)malloc(sz); if(fread(b,1,sz,f)!=(size_t)sz){fclose(f);free(b);return NULL;} fclose(f);
    if(memcmp(b,"RIFF",4)||memcmp(b+8,"WAVE",4)){ free(b); return NULL; }
    uint32_t pos=12; int channels=2,rate=44100,bits=16,fmt=1; uint32_t dataoff=0,datalen=0;
    while(pos+8<=(uint32_t)sz){
        uint32_t id=rdle32(b+pos), len=rdle32(b+pos+4); uint32_t body=pos+8;
        if(!memcmp(b+pos,"fmt ",4)){ fmt=rdle16(b+body); channels=rdle16(b+body+2); rate=rdle32(b+body+4); bits=rdle16(b+body+14); }
        else if(!memcmp(b+pos,"data",4)){ dataoff=body; datalen=len; }
        pos = body + len + (len&1);
        (void)id;
    }
    if(!dataoff){ free(b); return NULL; }
    int frames = datalen/(channels*(bits/8));
    float* out=(float*)malloc(sizeof(float)*frames*channels);
    for(int i=0;i<frames*channels;i++){
        const uint8_t* p=b+dataoff;
        if(bits==16) out[i]=rdle16(p+i*2)/32768.0f;
        else if(bits==32 && fmt==3){ uint32_t u=rdle32(p+i*4); memcpy(&out[i],&u,4); }
        else if(bits==8) out[i]=(p[i]-128)/128.0f;
        else out[i]=0;
    }
    free(b); *ch=channels; *n=frames; *sr=rate; return out;
}

static void wav_write_stereo(const char* path, const float* L, const float* R, int n, int sr){
    FILE* f=fopen(path,"wb"); if(!f) return;
    uint32_t db=n*2*2, riff=36+db, br=sr*2*2; uint16_t fmt=1,ch=2,ba=4,bps=16; uint32_t fl=16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f); fwrite(&fl,4,1,f);
    fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&sr,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&db,4,1,f);
    for(int i=0;i<n;i++){
        float l=L[i], r=R?R[i]:L[i];
        int li=(int)(l*32767.0f), ri=(int)(r*32767.0f);
        if(li>32767)li=32767; if(li<-32768)li=-32768; if(ri>32767)ri=32767; if(ri<-32768)ri=-32768;
        int16_t a=(int16_t)li,b=(int16_t)ri; fwrite(&a,2,1,f); fwrite(&b,2,1,f);
    }
    fclose(f);
}

static void stats(const char* tag, const float* x, int n){
    double sum=0, peak=0; int nz=0, nan=0;
    for(int i=0;i<n;i++){ float v=x[i]; if(v!=v||v>1e30||v<-1e30){nan++;continue;} double a=fabs(v); sum+=v*v; if(a>peak)peak=a; if(a>1e-6)nz++; }
    fprintf(stderr,"   %-8s rms=%.5f peak=%.5f nonzero=%d/%d nan/inf=%d\n", tag, sqrt(sum/(n?n:1)), peak, nz, n, nan);
}

int main(int argc, char** argv){
    if(getenv("EMU_VERBOSE")) EMU_VERBOSE=1;
    if(argc<2){ fprintf(stderr,"usage: %s <plugin.dll> [out.wav] [in.wav]\n", argv[0]); return 2; }
    const char* dll = argv[1];
    const char* outw= argc>2?argv[2]:"vst_out.wav";
    const char* inw = argc>3?argv[3]:NULL;
    int SR=44100, BS=512;

    fprintf(stderr,"=== load %s ===\n", dll);
    int rc = vst_load(dll);
    if(rc!=0){ fprintf(stderr,"LOAD FAILED rc=%d\n", rc); return 1; }

    char name[128]={0}, vendor[128]={0};
    vst_get_string(effGetEffectName, name, sizeof name);
    vst_get_string(effGetVendorString, vendor, sizeof vendor);
    fprintf(stderr,"name='%s' vendor='%s' uid=0x%08x ver=%u in=%d out=%d params=%d programs=%d\n",
        name, vendor, VST.uniqueID, VST.version, VST.numInputs, VST.numOutputs, VST.numParams, VST.numPrograms);

    // dump parameters
    for(int p=0; p<VST.numParams && p<24; p++){
        char pn[64]={0}, pd[64]={0}, pl[64]={0};
        vst_dispatch(effGetParamName, p, 0, VST_SCRATCH+VST_SCRATCH_SZ-0x200, 0.0f);
        { uint32_t b=VST_SCRATCH+VST_SCRATCH_SZ-0x200; for(int i=0;i<31;i++){ pn[i]=(char)rd8(b+i); if(!pn[i])break; } }
        float v=vst_get_param(p);
        vst_dispatch(effGetParamDisplay, p, 0, VST_SCRATCH+VST_SCRATCH_SZ-0x180, 0.0f);
        { uint32_t b=VST_SCRATCH+VST_SCRATCH_SZ-0x180; for(int i=0;i<31;i++){ pd[i]=(char)rd8(b+i); if(!pd[i])break; } }
        vst_dispatch(effGetParamLabel, p, 0, VST_SCRATCH+VST_SCRATCH_SZ-0x140, 0.0f);
        { uint32_t b=VST_SCRATCH+VST_SCRATCH_SZ-0x140; for(int i=0;i<31;i++){ pl[i]=(char)rd8(b+i); if(!pl[i])break; } }
        fprintf(stderr,"   param[%2d] %-16s = %.3f  (%s %s)\n", p, pn, v, pd, pl);
    }

    // direct guest-function test: EMU_TESTFN="<hexaddr>,<d1>[,<d2>...]" pushes doubles, returns ST0
    if(getenv("EMU_TESTFN")){
        char tb[256]; strncpy(tb,getenv("EMU_TESTFN"),255); tb[255]=0;
        char* t=strtok(tb,","); unsigned addr=0; if(t){ sscanf(t,"%x",&addr); t=strtok(NULL,","); }
        uint32_t args[16]; int n=0; double dv[8]; int nd=0;
        while(t && nd<8){ dv[nd]=atof(t); uint32_t lo,hi; memcpy(&lo,&dv[nd],4); memcpy(&hi,((char*)&dv[nd])+4,4); args[n++]=lo; args[n++]=hi; nd++; t=strtok(NULL,","); }
        call_guest(addr,args,n);
        double r=CPU.st[CPU.fpu_top&7];
        fprintf(stderr,"[testfn] %x(",addr); for(int i=0;i<nd;i++) fprintf(stderr,"%g%s",dv[i],i+1<nd?",":"");
        fprintf(stderr,") -> %.8g  fpu_top=%d faulted=%d cw=%04x\n",r,CPU.fpu_top,CPU.faulted,CPU.fpu_cw);
        return 0;
    }
    vst_set_samplerate((float)SR);
    vst_set_blocksize(BS);
    // Re-apply each parameter's current value so the plugin (re)computes derived DSP
    // coefficients. Many plugins only compute these inside setParameter().
    for(int p=0;p<VST.numParams;p++){ float v=vst_get_param(p); vst_set_param(p, v); }
    // optional override: EMU_PARAMS="0=0.7,2=1.0" (by index or by name) OR
    // EMU_MACRO='Drive="0.052" Muffle="0.5" Output="0.3"'  (raw Audacity macro param string)
    { const char* ps=getenv("EMU_PARAMS"); const char* mac=getenv("EMU_MACRO");
      if(ps){ int n=vst_apply_macro(ps); fprintf(stderr,"   applied %d params from EMU_PARAMS\n",n); }
      if(mac){ int n=vst_apply_macro(mac); fprintf(stderr,"   applied %d params from EMU_MACRO\n",n); } }
    // show resolved param values
    for(int p=0; p<VST.numParams && p<24; p++){ char pn[64]={0}; vst_get_param_name(p,pn,sizeof pn);
        fprintf(stderr,"   -> param[%2d] %-16s = %.4f\n", p, pn, vst_get_param(p)); }
    vst_resume();

    // build input signal
    int inFrames, inCh, inSR;
    float* inter=NULL;
    if(inw){ inter=wav_read(inw,&inCh,&inFrames,&inSR); if(!inter){ fprintf(stderr,"cannot read %s\n",inw); } }
    int N = inter ? inFrames : SR*2;   // 2 seconds if synthesizing
    float* inL=(float*)malloc(sizeof(float)*N);
    float* inR=(float*)malloc(sizeof(float)*N);
    if(inter){
        for(int i=0;i<N;i++){ inL[i]=inter[i*inCh]; inR[i]=inter[i*inCh+(inCh>1?1:0)]; }
        free(inter);
    } else {
        // 220 Hz sine, amplitude 0.5, plus a short impulse at t=0 for transient/delay tests
        for(int i=0;i<N;i++){ float s=0.5f*sinf(2.0f*3.14159265f*220.0f*i/SR); inL[i]=inR[i]=s; }
        inL[0]=inR[0]=0.9f;
    }

    float* outL=(float*)calloc(N,sizeof(float));
    float* outR=(float*)calloc(N,sizeof(float));

    // process block by block (EMU_BENCH=K repeats the whole buffer K times for stable timing)
    const float* inbuf[16]; float* outbuf[16];
    float blkIn[16][512]; float blkOut[16][512];
    int ni = VST.numInputs<16?VST.numInputs:16, no=VST.numOutputs<16?VST.numOutputs:16;
    int reps = getenv("EMU_BENCH") ? atoi(getenv("EMU_BENCH")) : 1; if(reps<1) reps=1;
    clock_t t0 = clock();
    for(int r=0;r<reps;r++)
    for(int pos=0; pos<N; pos+=BS){
        int n = (N-pos)<BS ? (N-pos) : BS;
        for(int c=0;c<ni;c++){ const float* s=(c==0)?inL:inR; for(int i=0;i<n;i++) blkIn[c][i]=s[i+pos]; inbuf[c]=blkIn[c]; }
        for(int c=0;c<no;c++){ outbuf[c]=blkOut[c]; for(int i=0;i<n;i++) blkOut[c][i]=0; }
        vst_process(inbuf, outbuf, n);
        if(CPU.faulted){ fprintf(stderr,"** process FAULTED @block pos=%d eip=%08x (%s)\n",pos,CPU.eip,CPU.fault_msg?CPU.fault_msg:"?"); break; }
        for(int i=0;i<n;i++){ outL[pos+i]=blkOut[0][i]; outR[pos+i]=(no>1)?blkOut[1][i]:blkOut[0][i]; }
    }
    { extern uint64_t g_insns; double ms=(double)(clock()-t0)*1000.0/CLOCKS_PER_SEC; double audioSec=(double)N*reps/SR;
      fprintf(stderr,"=== BENCH: %d frames x%d in %.1f ms  = %.2fx realtime (%.1f kframes/s) | %.0f insns/frame, %.1f M insns/s ===\n",
        N, reps, ms, audioSec/(ms/1000.0), (double)N*reps/ms,
        (double)g_insns/((double)N*reps), (double)g_insns/ms/1000.0); }

    fprintf(stderr,"=== signal stats (%d frames @ %d Hz) ===\n", N, SR);
    stats("in.L", inL, N);
    stats("out.L", outL, N);
    stats("out.R", outR, N);

    wav_write_stereo(outw, outL, outR, N, SR);
    fprintf(stderr,"wrote %s\n", outw);
    return 0;
}
