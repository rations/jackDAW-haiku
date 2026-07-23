/* vst2_abi.h — minimal, clean-room VST 2.4 plug-in ABI for the jackDAW host.
 *
 * This is an independent re-declaration of the well-known VST 2.4 binary
 * interface: the memory layout of `AEffect`, the dispatcher/audioMaster opcode
 * numbers, the event and time-info structs, and the flag/category constants.
 * Those numbers and offsets are the ABI a compiled plug-in was built against —
 * interface facts, not authored expression — and are reproduced here from the
 * public ABI (as documented across asseca.org's VST 2.4 notes, KVR, and the
 * layouts every open host relies on) rather than copied from any SDK or from
 * the GPL "vestige" header. It is deliberately limited to what this host calls,
 * but is ABI-complete for those parts: field order and sizes match, so a
 * plug-in's `AEffect*` can be driven correctly.
 *
 * x86_64 only. On the System V AMD64 ABI (Linux/Haiku) there is a single
 * calling convention, so the historical `__cdecl` marking is a no-op; a 32-bit
 * or Win32 target would need this revisited.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VST2_ABI_H_INCLUDED
#define VST2_ABI_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Calling convention: empty on x86_64 SysV (see file header). */
#define VST_CALL_CONV

/* Four-char code, MSB first: kEffectMagic = 'VstP'. */
#define VST2_CCONST(a, b, c, d)                                                                    \
    ((int32_t)(((int32_t)(a) << 24) | ((int32_t)(b) << 16) | ((int32_t)(c) << 8) | ((int32_t)(d))))

/* AEffect::magic — a valid VST2 effect returns this. */
#define kEffectMagic VST2_CCONST('V', 's', 't', 'P')

/* ---- AEffect::flags ---- */
#define effFlagsHasEditor (1 << 0)
#define effFlagsCanReplacing (1 << 4)
#define effFlagsProgramChunks (1 << 5)
#define effFlagsIsSynth (1 << 8)

/* ---- dispatcher() opcodes (effXxx) we issue ---- */
#define effOpen 0
#define effClose 1
#define effGetParamLabel 6
#define effGetParamDisplay 7
#define effGetParamName 8
#define effSetSampleRate 10
#define effSetBlockSize 11
#define effMainsChanged 12
#define effEditGetRect 13
#define effEditOpen 14
#define effEditClose 15
#define effEditIdle 19
#define effGetChunk 23
#define effSetChunk 24
#define effProcessEvents 25
#define effGetPlugCategory 35
#define effGetEffectName 45
#define effStartProcess 71
#define effStopProcess 72

/* ---- audioMasterCallback opcodes (audioMasterXxx) we answer ---- */
#define audioMasterVersion 1
#define audioMasterCurrentId 2
#define audioMasterWantMidi 6 /* deprecated, still queried */
#define audioMasterGetTime 7
#define audioMasterSizeWindow 15 /* plug-in requests its editor be resized: index=w, value=h */
#define audioMasterGetSampleRate 16
#define audioMasterGetBlockSize 17
#define audioMasterGetCurrentProcessLevel 23
#define audioMasterCanDo 37

/* ---- VstEvent::type ---- */
#define kVstMidiType 1
#define kVstSysExType 6

/* ---- VstTimeInfo::flags ---- */
#define kVstTransportChanged (1 << 0)
#define kVstTransportPlaying (1 << 1)
#define kVstTransportCycleActive (1 << 2)
#define kVstNanosValid (1 << 8)
#define kVstPpqPosValid (1 << 9)
#define kVstTempoValid (1 << 10)
#define kVstBarsValid (1 << 11)
#define kVstCyclePosValid (1 << 12)
#define kVstTimeSigValid (1 << 13)
#define kVstSmpteValid (1 << 14)
#define kVstClockValid (1 << 15)

/* ---- effGetPlugCategory return values (VstPlugCategory) ---- */
#define kPlugCategUnknown 0
#define kPlugCategEffect 1
#define kPlugCategSynth 2

struct AEffect;

typedef intptr_t(VST_CALL_CONV *audioMasterCallback)(struct AEffect *effect, int32_t opcode,
                                                     int32_t index, intptr_t value, void *ptr,
                                                     float opt);

typedef intptr_t(VST_CALL_CONV *AEffectDispatcherProc)(struct AEffect *effect, int32_t opcode,
                                                       int32_t index, intptr_t value, void *ptr,
                                                       float opt);
typedef void(VST_CALL_CONV *AEffectProcessProc)(struct AEffect *effect, float **inputs,
                                                float **outputs, int32_t sampleFrames);
typedef void(VST_CALL_CONV *AEffectProcessDoubleProc)(struct AEffect *effect, double **inputs,
                                                      double **outputs, int32_t sampleFrames);
typedef void(VST_CALL_CONV *AEffectSetParameterProc)(struct AEffect *effect, int32_t index,
                                                     float parameter);
typedef float(VST_CALL_CONV *AEffectGetParameterProc)(struct AEffect *effect, int32_t index);

/* The plug-in instance. Field order and sizes are the VST 2.4 ABI; do not
 * reorder. Reserved/deprecated slots are named to keep the layout explicit. */
typedef struct AEffect {
    int32_t magic; /* kEffectMagic */
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process; /* deprecated (accumulating) */
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;

    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;

    intptr_t resvd1; /* host-reserved */
    intptr_t resvd2; /* host-reserved */
    int32_t initialDelay;

    int32_t realQualities; /* deprecated */
    int32_t offQualities;  /* deprecated */
    float ioRatio;         /* deprecated */

    void *object; /* plug-in-owned */
    void *user;   /* host use (we stash the instance) */

    int32_t uniqueID;
    int32_t version;

    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;

    char future[56]; /* ABI tail padding */
} AEffect;

/* Editor rectangle returned by effEditGetRect (pointer-to-pointer out param). */
typedef struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
} ERect;

/* One MIDI event. First four int32 fields overlay VstEvent's header, so a
 * VstMidiEvent* may be handed to the plug-in through a VstEvent* slot. */
typedef struct VstMidiEvent {
    int32_t type;        /* kVstMidiType */
    int32_t byteSize;    /* sizeof(VstMidiEvent) */
    int32_t deltaFrames; /* sample offset within the block */
    int32_t flags;
    int32_t noteLength;
    int32_t noteOffset;
    char midiData[4]; /* status, data1, data2, 0 */
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
} VstMidiEvent;

/* Generic event header (its first four fields match VstMidiEvent). */
typedef struct VstEvent {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    char data[16];
} VstEvent;

/* Event list handed to effProcessEvents. `events` is a flexible tail; callers
 * over-allocate for the number of pointers they need. */
typedef struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;   /* zero */
    VstEvent *events[1]; /* [numEvents] */
} VstEvents;

/* Host transport/time, filled on audioMasterGetTime. */
typedef struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
} VstTimeInfo;

/* Plug-in entry point: VSTPluginMain (or legacy "main"). */
typedef AEffect *(VST_CALL_CONV *Vst2EntryProc)(audioMasterCallback host);

#ifdef __cplusplus
}
#endif

#endif /* VST2_ABI_H_INCLUDED */
