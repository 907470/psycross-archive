#ifndef PSYX_SPUAL_H
#define PSYX_SPUAL_H

#include "psx/types.h"
#include "psx/libspu.h"
#include "PsyX/PsyX_config.h"
#include "PsyX/common/pgxp_defs.h"

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern int PsyX_SPUAL_InitSound();
extern void PsyX_SPUAL_ShutdownSound();

// Private
extern int PsyX_SPUAL_Alloc(int size);
extern int PsyX_SPUAL_InitAlloc(int num, char* top);
extern void PsyX_SPUAL_Free(u_int addr);
extern u_int PsyX_SPUAL_Write(u_char* addr, u_int size);
extern u_int PsyX_SPUAL_Read(u_char* addr, u_int size);
extern u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr);

extern void PsyX_SPUAL_GetVoiceVolume(int vNum, short* volL, short* volR);
extern void PsyX_SPUAL_GetVoicePitch(int vNum, u_short* pitch);
extern void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* psxAttrib);
extern void PsyX_SPUAL_SetKey(int on_off, u_int voice_bit);

extern int PsyX_SPUAL_GetKeyStatus(u_int voice_bit);
extern void PsyX_SPUAL_GetAllKeysStatus(char* status);

/* Advance the deterministic ADSR envelope simulation for all 24
 * voices by `samples` SPU samples (44100 Hz). Called once per game
 * VBL (~735 samples NTSC) by openpete so that game-logic-relevant
 * SPU state transitions track real PSX timing regardless of host
 * audio backend or whether an audio device is available.
 *
 * The envelope generator follows psx-spx ("SPU Volume and ADSR
 * Generator") and is independent of the audio device. */
extern void PsyX_SPUAL_TickEnvelope(int samples);

// Loopback audio capture (gated on g_psyx_audioCaptureMode at InitSound).
// CaptureDrain pulls mixed stereo s16 @ 44100 frames into a caller buffer
// (interleaved L,R), returning frames written; CaptureActive reports whether
// loopback capture is live. Both return 0 while capture is inactive.
extern int PsyX_SPUAL_CaptureDrain(short* out, int max_frames);
extern int PsyX_SPUAL_CaptureActive(void);

/* SPU state-export (used by openpete save states) — deterministic
 * per-voice mirror (+ transfer/alloc cursors) and the SPU sample RAM.
 * Snapshot/restore in the PsyX_VRAM_* style: fixed sizes (query first),
 * mismatched sizes return non-zero. Restore stops every OpenAL source and
 * marks samples dirty (next KeyOn re-uploads from the restored RAM); the
 * envelope-sim fields restore exactly so SpuGetKeyStatus stays coherent
 * with the loaded guest state. */
extern unsigned int PsyX_SPUAL_StateSize(void);
extern int PsyX_SPUAL_SnapshotState(void* dst, unsigned int size);
extern int PsyX_SPUAL_RestoreState(const void* src, unsigned int size);
/* quiet_al=1: restore ONLY the deterministic mirror (no source stops, no
 * sample-dirty) — for high-rate same-timeline restores (runahead/rewind
 * stress) where the audible half must not be perturbed. */
extern int PsyX_SPUAL_RestoreStateEx(const void* src, unsigned int size,
                                     int quiet_al);
extern unsigned int PsyX_SPUAL_MemSize(void);
extern int PsyX_SPUAL_SnapshotMem(void* dst, unsigned int size);
extern int PsyX_SPUAL_RestoreMem(const void* src, unsigned int size);

extern int PsyX_SPUAL_SetMute(int on_off);
extern int PsyX_SPUAL_SetReverb(int on_off);
extern int PsyX_SPUAL_GetReverbState();
extern u_int PsyX_SPUAL_SetReverbVoice(int on_off, u_int voice_bit);
extern u_int PsyX_SPUAL_GetReverbVoice();

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif // PSYX_SPUAL_H