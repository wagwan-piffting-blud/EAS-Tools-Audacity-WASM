// macrorun.c - execute an Audacity macro chain on a WAV, end-to-end.
//   macrorun.exe <macro.txt> <in.wav> <out.wav> [plugin_dir] [max_seconds]
//
// VST/LADSPA-as-VST steps run through the x86 emulator (params applied BY NAME from the
// macro). The common Audacity built-ins (Amplify/Normalize/High-pass/Low-pass/BassAndTreble)
// are applied as native DSP here; in a Wavacity port those are native already. Steps we
// don't model (Distortion, Clipper/Nyquist, LADSPA, Message) are logged and passed through.
#include "emu.h"
#include "vst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------- WAV I/O (16-bit PCM / 32-bit float, mono/stereo) ----------------
static uint32_t rl32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rl16(const uint8_t* p){ return p[0]|(p[1]<<8); }

static float* wav_read(const char* path,int* ch,int* n,int* sr){
    FILE* f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t* b=malloc(sz); if(fread(b,1,sz,f)!=(size_t)sz){fclose(f);free(b);return NULL;} fclose(f);
    if(memcmp(b,"RIFF",4)||memcmp(b+8,"WAVE",4)){free(b);return NULL;}
    uint32_t pos=12; int channels=2,rate=44100,bits=16,fmt=1; uint32_t doff=0,dlen=0;
    while(pos+8<=(uint32_t)sz){ uint32_t len=rl32(b+pos+4); uint32_t body=pos+8;
        if(!memcmp(b+pos,"fmt ",4)){ fmt=rl16(b+body); channels=rl16(b+body+2); rate=rl32(b+body+4); bits=rl16(b+body+14); }
        else if(!memcmp(b+pos,"data",4)){ doff=body; dlen=len; }
        pos=body+len+(len&1); }
    if(!doff){free(b);return NULL;}
    int frames=dlen/(channels*(bits/8));
    float* out=malloc(sizeof(float)*frames*channels);
    for(int i=0;i<frames*channels;i++){ const uint8_t* p=b+doff;
        if(bits==16) out[i]=(int16_t)rl16(p+i*2)/32768.0f;
        else if(bits==32&&fmt==3){ uint32_t u=rl32(p+i*4); memcpy(&out[i],&u,4); }
        else if(bits==8) out[i]=(p[i]-128)/128.0f; else out[i]=0; }
    free(b); *ch=channels; *n=frames; *sr=rate; return out;
}
static void wav_write(const char* path,const float* L,const float* R,int n,int sr){
    FILE* f=fopen(path,"wb"); if(!f) return; uint32_t db=n*4,riff=36+db,br=sr*4; uint16_t fmt=1,c=2,ba=4,bps=16; uint32_t fl=16;
    fwrite("RIFF",1,4,f);fwrite(&riff,4,1,f);fwrite("WAVE",1,4,f);fwrite("fmt ",1,4,f);fwrite(&fl,4,1,f);
    fwrite(&fmt,2,1,f);fwrite(&c,2,1,f);fwrite(&sr,4,1,f);fwrite(&br,4,1,f);fwrite(&ba,2,1,f);fwrite(&bps,2,1,f);
    fwrite("data",1,4,f);fwrite(&db,4,1,f);
    for(int i=0;i<n;i++){ int l=(int)(L[i]*32767.f),r=(int)(R[i]*32767.f);
        if(l>32767)l=32767;if(l<-32768)l=-32768;if(r>32767)r=32767;if(r<-32768)r=-32768;
        int16_t a=(int16_t)l,bb=(int16_t)r; fwrite(&a,2,1,f); fwrite(&bb,2,1,f); }
    fclose(f);
}

// ---------------- biquad (RBJ) for built-in filters/shelves ----------------
typedef struct { double b0,b1,b2,a1,a2, z1l,z2l,z1r,z2r; } biquad;
static void bq_run(biquad* q,float* L,float* R,int n){
    for(int i=0;i<n;i++){
        double xl=L[i], yl=q->b0*xl+q->z1l; q->z1l=q->b1*xl-q->a1*yl+q->z2l; q->z2l=q->b2*xl-q->a2*yl; L[i]=(float)yl;
        double xr=R[i], yr=q->b0*xr+q->z1r; q->z1r=q->b1*xr-q->a1*yr+q->z2r; q->z2r=q->b2*xr-q->a2*yr; R[i]=(float)yr;
    }
}
static biquad bq_lph(int hp,double f,double sr,double Q){
    biquad q; memset(&q,0,sizeof q); double w=2*M_PI*f/sr, cs=cos(w), sn=sin(w), al=sn/(2*Q);
    double a0=1+al, a1=-2*cs, a2=1-al, b0,b1,b2;
    if(hp){ b0=(1+cs)/2; b1=-(1+cs); b2=(1+cs)/2; } else { b0=(1-cs)/2; b1=1-cs; b2=(1-cs)/2; }
    q.b0=b0/a0; q.b1=b1/a0; q.b2=b2/a0; q.a1=a1/a0; q.a2=a2/a0; return q;
}
static biquad bq_shelf(int high,double f,double sr,double dB){
    biquad q; memset(&q,0,sizeof q); double A=pow(10,dB/40), w=2*M_PI*f/sr, cs=cos(w), sn=sin(w);
    double al=sn/2*sqrt((A+1/A)*(1/0.9-1)+2), tsa=2*sqrt(A)*al;
    double a0,a1,a2,b0,b1,b2;
    if(high){ b0=A*((A+1)+(A-1)*cs+tsa); b1=-2*A*((A-1)+(A+1)*cs); b2=A*((A+1)+(A-1)*cs-tsa);
        a0=(A+1)-(A-1)*cs+tsa; a1=2*((A-1)-(A+1)*cs); a2=(A+1)-(A-1)*cs-tsa; }
    else { b0=A*((A+1)-(A-1)*cs+tsa); b1=2*A*((A-1)-(A+1)*cs); b2=A*((A+1)-(A-1)*cs-tsa);
        a0=(A+1)+(A-1)*cs+tsa; a1=-2*((A-1)+(A+1)*cs); a2=(A+1)+(A-1)*cs-tsa; }
    q.b0=b0/a0;q.b1=b1/a0;q.b2=b2/a0;q.a1=a1/a0;q.a2=a2/a0; return q;
}

// ---------------- macro param lookup ----------------
static int macro_get(const char* s,const char* key,float* out){
    size_t kl=strlen(key); const char* p=s;
    while((p=strstr(p,key))){
        if((p==s||p[-1]==' ') && p[kl]=='='){ p+=kl+1; if(*p=='"')p++; *out=(float)atof(p); return 1; }
        p+=kl;
    }
    return 0;
}
static void macro_get_str(const char* s,const char* key,char* out,int osz){
    size_t kl=strlen(key); const char* p=strstr(s,key); out[0]=0;
    if(p && p[kl]=='='){ p+=kl+1; if(*p=='"')p++; int i=0; while(*p&&*p!='"'&&i<osz-1) out[i++]=*p++; out[i]=0; }
}

// ---------------- name -> DLL resolution ----------------
static void norm(const char* s,char* o,int osz){ int j=0; for(;*s&&j<osz-1;s++){ if(isalnum((unsigned char)*s)) o[j++]=tolower((unsigned char)*s); } o[j]=0; }
static int find_dll(const char* cmd,const char* dir,char* out,int osz){
    char ncmd[128]; norm(cmd,ncmd,sizeof ncmd);
    char glob[512]; snprintf(glob,sizeof glob,"%s",dir);
    // scan dir for a .dll whose normalized basename == normalized command
#ifdef _WIN32
    char pat[600]; snprintf(pat,sizeof pat,"%s\\*.dll",dir);
    WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(pat,&fd);
    if(h==INVALID_HANDLE_VALUE) return 0;
    do{ char base[260]; snprintf(base,sizeof base,"%s",fd.cFileName); char* dot=strrchr(base,'.'); if(dot)*dot=0;
        char nb[260]; norm(base,nb,sizeof nb);
        if(!strcmp(nb,ncmd)){ snprintf(out,osz,"%s\\%s",dir,fd.cFileName); FindClose(h); return 1; }
    } while(FindNextFileA(h,&fd));
    FindClose(h);
#endif
    (void)glob; return 0;
}

// process the whole host buffer through the currently-loaded VST
static void vst_run_buffer(float* L,float* R,int N,int SR){
    int BS=512; vst_set_samplerate((float)SR); vst_set_blocksize(BS); vst_resume();
    const float* in[16]; float* out[16]; static float bi[16][512], bo[16][512];
    int ni=VST.numInputs<16?VST.numInputs:16, no=VST.numOutputs<16?VST.numOutputs:16;
    for(int pos=0;pos<N;pos+=BS){ int n=(N-pos)<BS?(N-pos):BS;
        for(int c=0;c<ni;c++){ const float* s=(c==0)?L:R; for(int i=0;i<n;i++) bi[c][i]=s[pos+i]; in[c]=bi[c]; }
        for(int c=0;c<no;c++){ out[c]=bo[c]; for(int i=0;i<n;i++) bo[c][i]=0; }
        vst_process(in,out,n);
        if(CPU.faulted){ fprintf(stderr,"      ** faulted at pos=%d; stopping this effect\n",pos); break; }
        for(int i=0;i<n;i++){ L[pos+i]=bo[0][i]; R[pos+i]=(no>1)?bo[1][i]:bo[0][i]; }
    }
}

static void stats(const char* tag,const float* L,int n){
    double s=0,pk=0; int nz=0,bad=0; for(int i=0;i<n;i++){ float v=L[i]; if(v!=v||fabs(v)>1e3){bad++;continue;} s+=v*v; if(fabs(v)>pk)pk=fabs(v); if(fabs(v)>1e-5)nz++; }
    fprintf(stderr,"      %-7s rms=%.4f peak=%.4f nz=%d/%d bad=%d\n",tag,sqrt(s/(n?n:1)),pk,nz,n,bad);
}

int main(int argc,char**argv){
    if(getenv("EMU_VERBOSE")) EMU_VERBOSE=1;
    if(argc<4){ fprintf(stderr,"usage: %s <macro.txt> <in.wav> <out.wav> [plugin_dir] [max_sec]\n",argv[0]); return 2; }
    const char* macp=argv[1]; const char* inw=argv[2]; const char* outw=argv[3];
    const char* dir = argc>4?argv[4]:"..";
    double maxsec = argc>5?atof(argv[5]):8.0;

    int ch,N,SR; float* inter=wav_read(inw,&ch,&N,&SR);
    if(!inter){ fprintf(stderr,"cannot read %s\n",inw); return 1; }
    int maxN=(int)(maxsec*SR); if(N>maxN) N=maxN;
    float* L=malloc(sizeof(float)*N); float* R=malloc(sizeof(float)*N);
    for(int i=0;i<N;i++){ L[i]=inter[i*ch]; R[i]=inter[i*ch+(ch>1?1:0)]; }
    free(inter);
    fprintf(stderr,"=== macro: %s   audio: %s (%.1fs @ %dHz, using %.1fs) ===\n",macp,inw,(double)N/SR* (maxsec<99?1:1),SR,(double)N/SR);
    stats("input",L,N);

    FILE* mf=fopen(macp,"rb"); if(!mf){ fprintf(stderr,"cannot read macro\n"); return 1; }
    char line[2048]; int step=0;
    while(fgets(line,sizeof line,mf)){
        char* nl=strpbrk(line,"\r\n"); if(nl)*nl=0;
        if(!line[0]) continue;
        char* colon=strchr(line,':'); if(!colon) continue;
        *colon=0; char* cmd=line; char* params=colon+1;
        while(*cmd==' ')cmd++;
        if(!strncmp(cmd,"Macro_",6) || !params[0]) continue;   // chain header
        if(!strcmp(cmd,"Message")) { continue; }
        step++;
        fprintf(stderr,"  [%d] %s\n",step,cmd);

        // built-ins (native DSP here; native in a Wavacity port)
        if(!strcmp(cmd,"Amplify")){ float r=1; macro_get(params,"Ratio",&r); for(int i=0;i<N;i++){L[i]*=r;R[i]*=r;} fprintf(stderr,"      [builtin] gain x%.3f\n",r); }
        else if(!strcmp(cmd,"Normalize")){ double pk=0; for(int i=0;i<N;i++){ if(fabs(L[i])>pk)pk=fabs(L[i]); if(fabs(R[i])>pk)pk=fabs(R[i]); } double g=pk>1e-9?(0.891/pk):1; for(int i=0;i<N;i++){L[i]*=g;R[i]*=g;} fprintf(stderr,"      [builtin] normalize x%.3f\n",g); }
        else if(!strcmp(cmd,"High-passFilter")||!strcmp(cmd,"Low-passFilter")){
            int hp=(cmd[0]=='H'); float f=1000; macro_get(params,"frequency",&f);
            char ro[16]; macro_get_str(params,"rolloff",ro,sizeof ro); int order=(strstr(ro,"48"))?8:(strstr(ro,"24"))?4:(strstr(ro,"6"))?1:2;
            int sec=order/2; if(sec<1)sec=1;
            for(int s=0;s<sec;s++){ biquad q=bq_lph(hp,f,SR,0.7071); bq_run(&q,L,R,N); }
            fprintf(stderr,"      [builtin] %s %gHz %s\n",hp?"HPF":"LPF",f,ro);
        }
        else if(!strcmp(cmd,"BassAndTreble")){ float b=0,t=0,g=0; macro_get(params,"Bass",&b); macro_get(params,"Treble",&t); macro_get(params,"Gain",&g);
            if(b!=0){ biquad q=bq_shelf(0,250,SR,b); bq_run(&q,L,R,N); }
            if(t!=0){ biquad q=bq_shelf(1,4000,SR,t); bq_run(&q,L,R,N); }
            if(g!=0){ float lg=(float)pow(10,g/20); for(int i=0;i<N;i++){L[i]*=lg;R[i]*=lg;} }
            fprintf(stderr,"      [builtin] bass%+gdB treble%+gdB gain%+gdB\n",b,t,g);
        }
        else {
            // try to resolve as an emulatable VST plugin
            char dll[600];
            if(find_dll(cmd,dir,dll,sizeof dll)){
                int rc=vst_load(dll);
                if(rc==0){ int ap=vst_apply_macro(params); fprintf(stderr,"      [emulated VST] %s  (%d params by name, in=%d out=%d)\n",strrchr(dll,'\\')+1,ap,VST.numInputs,VST.numOutputs);
                    vst_run_buffer(L,R,N,SR); }
                else fprintf(stderr,"      [skip] %s load rc=%d\n",strrchr(dll,'\\')+1,rc);
            } else fprintf(stderr,"      [skip] no emulatable plugin for '%s' (built-in/Nyquist/LADSPA -> native in Wavacity)\n",cmd);
        }
        stats("after",L,N);
    }
    fclose(mf);
    wav_write(outw,L,R,N,SR);
    fprintf(stderr,"=== wrote %s (%d steps, %.1fs) ===\n",outw,step,(double)N/SR);
    return 0;
}
