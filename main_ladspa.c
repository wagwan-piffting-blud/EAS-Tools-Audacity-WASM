// main_ladspa.c - native test driver for the LADSPA host.
//   ladspa.exe <plugin.dll> ["Param=val,Param2=val2"]
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int   ladspa_load(const char* dll, float sr);
void  ladspa_prepare(int cap);
int   ladspa_set_param(const char* name, float v);
void  ladspa_run(const float* in, float* out, int n);
int   ladspa_numinputs(void);
int   ladspa_numoutputs(void);

int main(int argc, char** argv){
    if(getenv("EMU_VERBOSE")) EMU_VERBOSE=1;
    if(argc<2){ fprintf(stderr,"usage: %s <plugin.dll> [\"Name=val,...\"]\n", argv[0]); return 2; }
    int SR=44100, N=SR; // 1s
    fprintf(stderr,"=== LADSPA load %s ===\n", argv[1]);
    int rc = ladspa_load(argv[1], (float)SR);
    if(rc!=0){ fprintf(stderr,"LOAD FAILED rc=%d\n", rc); return 1; }
    fprintf(stderr,"in=%d out=%d\n", ladspa_numinputs(), ladspa_numoutputs());
    ladspa_prepare(512);
    if(argc>2){ char buf[512]; strncpy(buf,argv[2],511); buf[511]=0; char* t=strtok(buf,",");
        while(t){ char* eq=strchr(t,'='); if(eq){ *eq=0; float v=(float)atof(eq+1); int p=ladspa_set_param(t,v);
            fprintf(stderr,"   set '%s'=%.3f -> port %d\n", t, v, p);} t=strtok(NULL,","); } }

    float* in=(float*)malloc(N*sizeof(float)); float* out=(float*)malloc(N*sizeof(float));
    for(int i=0;i<N;i++) in[i]=0.5f*sinf(2.0f*3.14159265f*220.0f*i/SR);
    for(int pos=0;pos<N;pos+=512){ int n=(N-pos)<512?(N-pos):512;
        ladspa_run(in+pos, out+pos, n);
        if(CPU.faulted){ fprintf(stderr,"** run FAULTED @%d\n",pos); break; } }

    double si=0,so=0,pk=0; for(int i=0;i<N;i++){ si+=in[i]*in[i]; so+=out[i]*out[i]; if(fabs(out[i])>pk)pk=fabs(out[i]); }
    fprintf(stderr,"in.rms=%.4f  out.rms=%.4f peak=%.4f  %s\n",
        sqrt(si/N), sqrt(so/N), pk, (so>1e-9 && !CPU.faulted)?"LADSPA OK":"(no output)");
    return 0;
}
