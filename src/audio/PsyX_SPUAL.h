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
// Frames pushed into / discarded from the capture ring. A non-zero dropped
// count means the recorded audio is time-compressed and must not be trusted
// for timing or level comparisons.
// SpuWrite re-sync accounting: voices rebuffered because a write landed in
// their decode window (total since start; the voice mask of the last write).
extern unsigned int PsyX_SPUAL_ResyncCount(void);
extern unsigned int PsyX_SPUAL_LastWriteResyncMask(void);
// Wall time of the last stop/rebuffer/play re-sync, microseconds (0 when the
// last re-sync rewrote the buffer in place).
extern unsigned int PsyX_SPUAL_LastResyncMicros(void);
// Head replays that hit the walk bound or ran off SPU RAM (the countdown is
// then left as it was).
extern unsigned int PsyX_SPUAL_HeadBoundBreaks(void);

extern void PsyX_SPUAL_CaptureStats(unsigned long long* produced,
                                    unsigned long long* dropped);
// Zero the counters; call when discarding the pre-roll backlog at capture start.
extern void PsyX_SPUAL_CaptureResetStats(void);

// SPU main volume (SPU_COMMON_MVOLL/R), linear 0..1. Hardware applies it to
// the summed mix, so it multiplies every voice gain and the XA stream gain.
extern void PsyX_SPUAL_SetMasterVolume(float linear_gain);
extern float PsyX_SPUAL_GetMasterVolume(void);

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

/* Host-side sample tools over caller-owned SPU-ADPCM bytes: the voice
 * path's own END-flag walk and decoder. Length = bytes up to and
 * including the first END-flagged 16-byte block (max_bytes rounded down
 * to a block when none is found). Decode writes 28 samples per block
 * up to the END block into `out` (size it as (bytes/16)*28) and reports
 * the loop window in samples (loop_len 0 = one-shot). */
/* Read-only view of SPU sample RAM (512 KB): what a voice keyed at
 * `addr` decodes. Host consumers hash/identify samples through it. */
extern const u_char* PsyX_SPUAL_MemPtr(unsigned int* size_out);

/* Sample override (see PsyX_SPUAL_SetSampleOverride). The callback is
 * invoked under the SPU mutex on the thread that keys the voice, with
 * the stock decode already sized: `stock_samples` at 44.1 kHz, and
 * whether the stock def loops. It must not block. Return 1 and fill
 * `out` to substitute; `pcm` must stay valid until `done` (if set) is
 * called, which happens right after the buffer upload. */
typedef struct PsyX_SPUAL_SampleOverride {
	const short* pcm;
	int          count;       /* samples */
	unsigned int rate;        /* Hz the PCM is played at (AL buffer rate) */
	int          loop_start;  /* samples; loop_len 0 = no window of its own */
	int          loop_len;
	int          flat;        /* in: pin playback to `rate` — ignore the game's
	                           * pitch writes for this key-on (audible only) */
	int          rings_out;   /* out: the file outlasts the stock sample; when
	                           * the game keys the voice off at the stock end,
	                           * the tail keeps playing on an orphan source */
	void       (*done)(struct PsyX_SPUAL_SampleOverride*);
	void*        user;
} PsyX_SPUAL_SampleOverride;
/* Silence every ring-out tail now (level load, mute). */
extern void PsyX_SPUAL_StopOrphans(void);
typedef int (*PsyX_SPUAL_SampleOverrideFn)(int voice, unsigned int addr,
                                           int stock_samples, int stock_loops,
                                           PsyX_SPUAL_SampleOverride* out);
extern void PsyX_SPUAL_SetSampleOverride(PsyX_SPUAL_SampleOverrideFn fn);

extern int PsyX_SPUAL_SampleByteLength(const u_char* adpcm, int max_bytes);
extern int PsyX_SPUAL_DecodeSample(const u_char* adpcm, int bytes, short* out,
                                   int* loop_start, int* loop_len);
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