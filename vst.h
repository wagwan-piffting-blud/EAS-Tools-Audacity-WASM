// vst.h - VST 2.4 ABI constants. The plugin's AEffect struct lives in *guest* memory;
// these are byte offsets into it (32-bit layout, all pointers/ints are 4 bytes).
#ifndef VST_H
#define VST_H
#include <stdint.h>
#include "emu.h"

// AEffect field offsets (VstInt32 / pointers, 32-bit guest).
#define AE_magic              0x00   // 'VstP' = 0x56737450
#define AE_dispatcher         0x04   // intptr (*)(AEffect*,op,index,value,ptr,opt)
#define AE_process            0x08   // deprecated
#define AE_setParameter       0x0C   // void (*)(AEffect*,index,float)
#define AE_getParameter       0x10   // float (*)(AEffect*,index)
#define AE_numPrograms        0x14
#define AE_numParams          0x18
#define AE_numInputs          0x1C
#define AE_numOutputs         0x20
#define AE_flags              0x24
#define AE_initialDelay       0x30
#define AE_object             0x40
#define AE_user               0x44
#define AE_uniqueID           0x48
#define AE_version            0x4C
#define AE_processReplacing   0x50   // void (*)(AEffect*,float**,float**,int)
#define AE_processDoubleRepl  0x54

#define AE_MAGIC  0x56737450u  // 'VstP'

// effFlags bits
#define effFlagsCanReplacing      (1<<4)
#define effFlagsCanDoubleReplacing (1<<12)

// dispatcher opcodes (host -> plugin)
#define effOpen                0
#define effClose               1
#define effSetProgram          2
#define effGetProgram          3
#define effGetParamLabel       6
#define effGetParamDisplay     7
#define effGetParamName        8
#define effSetSampleRate      10   // opt = sample rate (float)
#define effSetBlockSize       11   // value = block size
#define effMainsChanged       12   // value = 0 off / 1 on (resume)
#define effEditGetRect        13
#define effEditOpen           14
#define effEditClose          15
#define effGetChunk           23
#define effSetChunk           24
#define effGetEffectName      45
#define effGetVendorString    47
#define effGetProductString   48
#define effGetVendorVersion   49
#define effCanDo              51
#define effGetVstVersion      58

// audioMaster opcodes (plugin -> host)
#define audioMasterAutomate            0
#define audioMasterVersion             1
#define audioMasterCurrentId           2
#define audioMasterIdle                3
#define audioMasterGetTime             7
#define audioMasterProcessEvents       8
#define audioMasterIOChanged          13
#define audioMasterSizeWindow         15
#define audioMasterGetSampleRate      16
#define audioMasterGetBlockSize       17
#define audioMasterGetInputLatency    18
#define audioMasterGetOutputLatency   19
#define audioMasterGetCurrentProcessLevel 23
#define audioMasterGetAutomationState 24
#define audioMasterGetVendorString    32
#define audioMasterGetProductString   33
#define audioMasterGetVendorVersion   34
#define audioMasterCanDo              37
#define audioMasterGetLanguage        38
#define audioMasterUpdateDisplay      42
#define audioMasterBeginEdit          43
#define audioMasterEndEdit            44
#define audioMasterOpenFileSelector   45

// ---- host API ----
typedef struct {
    uint32_t aeffect;        // guest VA of AEffect
    int numInputs, numOutputs, numParams, numPrograms;
    uint32_t flags, uniqueID, version, initialDelay;
    int canReplacing;
    uint32_t dispatcher, setParameter, getParameter, processReplacing;
} vst_plugin_t;

extern vst_plugin_t VST;
extern float VST_SAMPLE_RATE;
extern int   VST_BLOCK_SIZE;

int   vst_load(const char* dll_path);          // native
int   vst_load_mem(const uint8_t* b, uint32_t n); // WASM
int64_t vst_dispatch(int opcode, int index, int32_t value, uint32_t ptr, float opt);
void  vst_set_param(int index, float value);
float vst_get_param(int index);
int   vst_param_index(const char* name);                  // param index by name (case-insensitive), -1 if none
int   vst_set_param_by_name(const char* name, float v);   // returns index set, or -1
int   vst_get_param_name(int index, char* out, int outsz);// query plugin's name for a param
int   vst_apply_macro(const char* s);                     // 'Name="v" Name2="v2"' (Audacity macro) -> count applied
void  vst_set_samplerate(float sr);
void  vst_set_blocksize(int bs);
void  vst_resume(void);     // effMainsChanged(1)
void  vst_suspend(void);    // effMainsChanged(0)
// process one block: in[ch][nframes] -> out[ch][nframes] (host float buffers).
void  vst_process(const float* const* in, float* const* out, int nframes);
int   vst_get_string(int opcode, char* out, int outsz); // effGetEffectName etc.

// ---- LADSPA host (ladspa_host.c) — pure-C DSP plugins (sc4, chebstortion) ----
int   ladspa_load(const char* dll, float sr);  // 0 on success
void  ladspa_prepare(int cap);                  // connect ports + alloc buffers for `cap` frames
int   ladspa_set_param(const char* name, float v);  // control port by name; -1 if none
int   ladspa_apply_macro(const char* s);        // 'Name="v" ...' -> count applied
void  ladspa_run(const float* in, float* out, int n); // mono block
int   ladspa_numinputs(void);
int   ladspa_numoutputs(void);

#endif
