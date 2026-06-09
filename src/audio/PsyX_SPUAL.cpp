#include "../PsyX_main.h"

#include "psx/libspu.h"
#include "psx/libetc.h"
#include "psx/libmath.h"
#include "PsyX_SPUAL.h"
#include "PsyX_XA.h"

#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#ifndef __EMSCRIPTEN__
#include <AL/efx.h>
#endif

// TODO: implement XA, implement ADSR

static const char* getALCErrorString(int err)
{
	switch (err)
	{
	case ALC_NO_ERROR:
		return "AL_NO_ERROR";
	case ALC_INVALID_DEVICE:
		return "ALC_INVALID_DEVICE";
	case ALC_INVALID_CONTEXT:
		return "ALC_INVALID_CONTEXT";
	case ALC_INVALID_ENUM:
		return "ALC_INVALID_ENUM";
	case ALC_INVALID_VALUE:
		return "ALC_INVALID_VALUE";
	case ALC_OUT_OF_MEMORY:
		return "ALC_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN";
	}
}

static const char* getALErrorString(int err)
{
	switch (err)
	{
	case AL_NO_ERROR:
		return "AL_NO_ERROR";
	case AL_INVALID_NAME:
		return "AL_INVALID_NAME";
	case AL_INVALID_ENUM:
		return "AL_INVALID_ENUM";
	case AL_INVALID_VALUE:
		return "AL_INVALID_VALUE";
	case AL_INVALID_OPERATION:
		return "AL_INVALID_OPERATION";
	case AL_OUT_OF_MEMORY:
		return "AL_OUT_OF_MEMORY";
	default:
		return "AL_UNKNOWN";
	}
}

#define SPU_REALMEMSIZE			(512 * 1024)
#define SPU_MEMSIZE				(2048*1024)		// SPU_REALMEMSIZE

typedef struct
{
	u_char samplemem[SPU_MEMSIZE];
	u_char* writeptr;
} SPUMemory;

static SPUMemory s_SpuMemory;
static SDL_Mutex* g_SpuMutex = NULL;
static int g_spuInit = 0;
static int s_spuMallocVal = 0;


/* The SPU's two level generators, per psx-spx "SPU Volume and ADSR
 * Generator": a voice's ADSR envelope, whose level is the voice's ENVX
 * register, and the sweep of a volume register (a voice's left or right
 * volume, the main left or right volume), whose level is the register's
 * current volume.
 *
 * The SPU owns the clock and the register latches; these functions own the
 * level. The SPU calls, for each 44.1 kHz output sample:
 *   1. PsyX_SPUEnv_Tick for every voice whose phase is not
 *      PSYX_SPU_ENV_OFF, and PsyX_SPUSweep_Tick for every volume register;
 *   2. then PsyX_SPUEnv_KeyOff for a voice whose key-off store came since
 *      the previous sample, then PsyX_SPUEnv_KeyOn for a voice whose key-on
 *      store came since the previous sample.
 * Register writes of the ADSR words and of the volume registers call
 * PsyX_SPUEnv_SetADSR and PsyX_SPUSweep_Write at the store. A voice whose
 * playback head leaves an END block without REPEAT is silenced with
 * PsyX_SPUEnv_Silence. A zero-filled struct is the power-on state: phase
 * off, level 0, register words 0.
 *
 * The struct members after the public ones are the implementation's own;
 * the SPU copies the structs whole into save states and reads the public
 * members only through the functions. */

#include <stdint.h>


/** @brief The ADSR phase of a voice envelope. */
typedef enum
{
	PSYX_SPU_ENV_OFF     = 0,
	PSYX_SPU_ENV_ATTACK  = 1,
	PSYX_SPU_ENV_DECAY   = 2,
	PSYX_SPU_ENV_SUSTAIN = 3,
	PSYX_SPU_ENV_RELEASE = 4
} PsyX_SPUEnvPhase;

/** @brief One voice's ADSR envelope generator. */
typedef struct
{
	uint16_t adsr1;     /* ADSR register, low halfword, as last written */
	uint16_t adsr2;     /* ADSR register, high halfword, as last written */
	int16_t  level;     /* ENVX */
	uint8_t  phase;     /* PsyX_SPUEnvPhase */
	uint8_t  impl8;     /* implementation state from here on */
	uint32_t impl[5];
} PsyX_SPUEnvelope;

/** @brief The sweep generator of one volume register. */
typedef struct
{
	uint16_t reg;       /* the volume register as last written */
	int16_t  level;     /* the current volume */
	uint8_t  sweeping;  /* the register is in sweep mode */
	uint8_t  impl8[3];  /* implementation state from here on */
	uint32_t impl[4];
} PsyX_SPUSweep;

/** @brief Store the ADSR register pair; the envelope uses the words from this store on.
 * @param e The envelope.
 * @param adsr1 Low halfword (1F801C08h + voice * 10h).
 * @param adsr2 High halfword (1F801C0Ah + voice * 10h). */
static inline void PsyX_SPUEnv_SetADSR(PsyX_SPUEnvelope* e, uint16_t adsr1, uint16_t adsr2);

/** @brief Apply a key-on at the sample it takes effect.
 * @param e The envelope. */
static inline void PsyX_SPUEnv_KeyOn(PsyX_SPUEnvelope* e);

/** @brief Apply a key-off at the sample it takes effect.
 * @param e The envelope. */
static inline void PsyX_SPUEnv_KeyOff(PsyX_SPUEnvelope* e);

/** @brief Run the envelope for one 44.1 kHz sample.
 * @param e The envelope; its phase is not PSYX_SPU_ENV_OFF.
 * @return The level after the sample (ENVX). */
static inline int16_t PsyX_SPUEnv_Tick(PsyX_SPUEnvelope* e);

/** @brief Stop the envelope: phase off, level 0 (the voice's sample ended).
 * @param e The envelope. */
static inline void PsyX_SPUEnv_Silence(PsyX_SPUEnvelope* e);

/** @brief The envelope's level, the voice's ENVX.
 * @param e The envelope.
 * @return The level. */
static inline int16_t PsyX_SPUEnv_Level(const PsyX_SPUEnvelope* e);

/** @brief The envelope's ADSR phase.
 * @param e The envelope.
 * @return The phase. */
static inline PsyX_SPUEnvPhase PsyX_SPUEnv_Phase(const PsyX_SPUEnvelope* e);

/** @brief Store a volume register: direct mode sets the level, sweep mode starts the sweep from the current level.
 * @param s The sweep.
 * @param reg The register value. */
static inline void PsyX_SPUSweep_Write(PsyX_SPUSweep* s, uint16_t reg);

/** @brief Run the sweep for one 44.1 kHz sample; no change in direct mode.
 * @param s The sweep. */
static inline void PsyX_SPUSweep_Tick(PsyX_SPUSweep* s);

/** @brief The current volume, the value the register's current-volume read returns.
 * @param s The sweep.
 * @return The level. */
static inline int16_t PsyX_SPUSweep_Level(const PsyX_SPUSweep* s);

/* PsyX_SPUEnvelope.cpp - the SPU's level generators: a voice's ADSR
 * envelope (ENVX) and the sweep of a volume register (VOLX, MVOLX).
 *
 * Both generators share one stepping rule. A phase or a sweep register
 * gives a 5-bit shift, a 2-bit step value, a linear or exponential mode and
 * a direction. Every 44.1 kHz sample a 16-bit counter gains an increment;
 * when its bit 15 is set the counter clears and the level moves by one
 * step. The shift sizes both: shifts below 11 scale the step up and step
 * every sample, shifts above 11 keep the base step and halve the increment
 * per unit. The exponential increase slows once the level is at 6000h or
 * above; the exponential decrease scales the step by level / 8000h.
 *
 * Measured behaviour the stepping rule alone does not state: a store to
 * either ADSR word clears the step counter, and the new words apply from
 * the next sample in whatever phase the envelope is; a sweep whose
 * direction bit is decrease takes one step and then holds its level. */

namespace {

/** @brief The stepping parameters of one envelope phase or one sweep register. */
struct StepRule
{
	unsigned shift;    /* 0..1Fh */
	unsigned step;     /* 0..3 */
	bool exponential;
	bool decrease;     /* the direction bit */
	bool negative;     /* the sweep phase bit; false for ADSR phases */
};

/** @brief Advance a level by one sample under a stepping rule.
 *
 * The counter gains the rule's increment; when its bit 15 sets, the counter
 * clears and the level moves by the step and saturates. A rule whose shift
 * and step bits are all ones never steps.
 * @param r The rule.
 * @param level The level, updated in place.
 * @param counter The step counter, updated in place.
 * @return true when the level stepped this sample. */
inline bool advance(const StepRule& r, int32_t& level, uint32_t& counter)
{
	const bool frozen = ((r.shift << 2) | r.step) == 0x7Fu;
	int32_t step = 7 - (int32_t)r.step;
	if (r.decrease != r.negative)
		step = ~step;                                    /* +7..+4 becomes -8..-5 */
	step *= (int32_t)1 << (r.shift < 11 ? 11 - r.shift : 0);
	uint32_t inc = 0x8000u >> (r.shift > 11 ? r.shift - 11 : 0);

	if (r.exponential && !r.decrease && level >= 0x6000)
	{
		if (r.shift < 10)
			step /= 4;
		else if (r.shift >= 11)
			inc >>= 2;
		else
		{
			step /= 2;
			inc >>= 1;
		}
	}
	else if (r.exponential && r.decrease)
		step = (int32_t)(((int64_t)step * level) >> 15);   /* floor */

	if (!frozen && inc == 0)
		inc = 1;
	counter += inc;
	if ((counter & 0x8000u) == 0)
		return false;
	counter = 0;

	int32_t next = level + step;
	if (!r.decrease)
		next = next < -0x8000 ? -0x8000 : (next > 0x7FFF ? 0x7FFF : next);
	else if (r.negative)
		next = next < -0x8000 ? -0x8000 : (next > 0 ? 0 : next);
	else if (next < 0)
		next = 0;
	level = next;
	return true;
}

/** @brief The stepping rule of an envelope's current phase, read from its ADSR words.
 * @param e The envelope; its phase is not off.
 * @return The rule. */
inline StepRule phase_rule(const PsyX_SPUEnvelope* e)
{
	const unsigned a1 = e->adsr1, a2 = e->adsr2;
	switch (e->phase)
	{
	case PSYX_SPU_ENV_ATTACK:
		return { (a1 >> 10) & 0x1Fu, (a1 >> 8) & 3u, (a1 & 0x8000u) != 0, false, false };
	case PSYX_SPU_ENV_DECAY:
		return { (a1 >> 4) & 0xFu, 0u, true, true, false };
	case PSYX_SPU_ENV_SUSTAIN:
		return { (a2 >> 8) & 0x1Fu, (a2 >> 6) & 3u, (a2 & 0x8000u) != 0, (a2 & 0x4000u) != 0, false };
	default:
		return { a2 & 0x1Fu, 0u, (a2 & 0x20u) != 0, true, false };
	}
}

/** @brief The step counter of an envelope.
 * @param e The envelope.
 * @return Its counter word. */
inline uint32_t& env_counter(PsyX_SPUEnvelope* e)
{
	return e->impl[0];
}

/** @brief The step counter of a sweep.
 * @param s The sweep.
 * @return Its counter word. */
inline uint32_t& sweep_counter(PsyX_SPUSweep* s)
{
	return s->impl[0];
}

} // namespace

static inline void PsyX_SPUEnv_SetADSR(PsyX_SPUEnvelope* e, uint16_t adsr1, uint16_t adsr2)
{
	e->adsr1 = adsr1;
	e->adsr2 = adsr2;
	env_counter(e) = 0;
}

static inline void PsyX_SPUEnv_KeyOn(PsyX_SPUEnvelope* e)
{
	e->phase = PSYX_SPU_ENV_ATTACK;
	e->level = 0;
	env_counter(e) = 0;
}

static inline void PsyX_SPUEnv_KeyOff(PsyX_SPUEnvelope* e)
{
	if (e->phase == PSYX_SPU_ENV_OFF)
		return;
	e->phase = PSYX_SPU_ENV_RELEASE;
	env_counter(e) = 0;
}

static inline int16_t PsyX_SPUEnv_Tick(PsyX_SPUEnvelope* e)
{
	if (e->phase == PSYX_SPU_ENV_OFF)
		return e->level;
	int32_t level = e->level;
	advance(phase_rule(e), level, env_counter(e));
	e->level = (int16_t)level;

	if (e->phase == PSYX_SPU_ENV_ATTACK && level >= 0x7FFF)
	{
		e->phase = PSYX_SPU_ENV_DECAY;
		env_counter(e) = 0;
	}
	else if (e->phase == PSYX_SPU_ENV_DECAY && level <= (int32_t)(((e->adsr1 & 0xFu) + 1) << 11))
	{
		e->phase = PSYX_SPU_ENV_SUSTAIN;
		env_counter(e) = 0;
	}
	return e->level;
}

static inline void PsyX_SPUEnv_Silence(PsyX_SPUEnvelope* e)
{
	e->phase = PSYX_SPU_ENV_OFF;
	e->level = 0;
	env_counter(e) = 0;
}

static inline int16_t PsyX_SPUEnv_Level(const PsyX_SPUEnvelope* e)
{
	return e->level;
}

static inline PsyX_SPUEnvPhase PsyX_SPUEnv_Phase(const PsyX_SPUEnvelope* e)
{
	return (PsyX_SPUEnvPhase)e->phase;
}

static inline void PsyX_SPUSweep_Write(PsyX_SPUSweep* s, uint16_t reg)
{
	s->reg = reg;
	sweep_counter(s) = 0;
	s->impl8[0] = 0;                             /* not holding */
	s->sweeping = (reg & 0x8000u) != 0;
	if (!s->sweeping)
		s->level = (int16_t)(uint16_t)(reg << 1);   /* bits 0-14 hold the volume / 2 */
}

static inline void PsyX_SPUSweep_Tick(PsyX_SPUSweep* s)
{
	if (!s->sweeping || s->impl8[0])
		return;
	const unsigned r = s->reg;
	const StepRule rule = { (r >> 2) & 0x1Fu, r & 3u, (r & 0x4000u) != 0, (r & 0x2000u) != 0, (r & 0x1000u) != 0 };
	int32_t level = s->level;
	const bool stepped = advance(rule, level, sweep_counter(s));
	s->level = (int16_t)level;
	if (stepped && rule.decrease)
		s->impl8[0] = 1;
}

static inline int16_t PsyX_SPUSweep_Level(const PsyX_SPUSweep* s)
{
	return s->level;
}


typedef struct
{
	SpuVoiceAttr attr;	// .voice is Id of this channel

	ALuint alBuffer;
	ALuint alSource;
	ushort sampledirty;
	ushort reverb;
	int keyed_on;       // 1 once SetKey(SPU_ON,...) fired; cleared by SetKey(SPU_OFF,...)

	// ADSR envelope state (deterministic, OpenAL-independent).
	uint16_t  adsr1;        // ADSR register words as PSYQ packs them
	uint16_t  adsr2;
	uint8_t   adsr_phase;   // PsyX_SPUEnvPhase
	int16_t  adsr_volume;  // current envelope output [-32768..32767]
	PsyX_SPUEnvelope env;

	// One-shot ADPCM auto-key-off (mirrors hardware: loop_end+!loop_repeat
	// in any block forces voice to Off). At KeyOn we walk SPU memory from
	// attr.addr until the first loop_end block and compute duration in
	// output samples scaled by pitch. Looping samples (loop_end+
	// loop_repeat) leave one_shot_samples_remaining == -1.
	int32_t one_shot_samples_remaining; // -1 = looping / no auto-off
} SPUALVoice;

const int s_spuVoiceCount = 24;

SPUALVoice	g_SpuVoices[s_spuVoiceCount];

/* The voice's envelope is the PsyX_SPUEnv generator above. adsr_phase and
 * adsr_volume mirror its phase and level after every call; the voice code
 * stores phase off when a one-shot sample ends, which silences the
 * generator, and writes adsr1/adsr2 from the attribute calls, which are
 * stored before the next sample. A release that reaches level 0 ends in
 * phase off, the state the voice allocation reads as free. */
static void SPUAL_Voice_EnvPull(SPUALVoice* v)
{
	if (v->adsr_phase == PSYX_SPU_ENV_OFF && PsyX_SPUEnv_Phase(&v->env) != PSYX_SPU_ENV_OFF)
		PsyX_SPUEnv_Silence(&v->env);
	if (v->env.adsr1 != v->adsr1 || v->env.adsr2 != v->adsr2)
		PsyX_SPUEnv_SetADSR(&v->env, v->adsr1, v->adsr2);
}

static void SPUAL_Voice_EnvPush(SPUALVoice* v)
{
	if (PsyX_SPUEnv_Phase(&v->env) == PSYX_SPU_ENV_RELEASE && PsyX_SPUEnv_Level(&v->env) == 0)
		PsyX_SPUEnv_Silence(&v->env);
	v->adsr_volume = PsyX_SPUEnv_Level(&v->env);
	v->adsr_phase = (uint8_t)PsyX_SPUEnv_Phase(&v->env);
}

static void SPUAL_Voice_EnvTick(SPUALVoice* v)
{
	SPUAL_Voice_EnvPull(v);
	PsyX_SPUEnv_Tick(&v->env);
	SPUAL_Voice_EnvPush(v);
}

static void SPUAL_Voice_EnvKeyOn(SPUALVoice* v)
{
	PsyX_SPUEnv_SetADSR(&v->env, v->adsr1, v->adsr2);
	PsyX_SPUEnv_KeyOn(&v->env);
	SPUAL_Voice_EnvPush(v);
}

static void SPUAL_Voice_EnvKeyOff(SPUALVoice* v)
{
	SPUAL_Voice_EnvPull(v);
	PsyX_SPUEnv_KeyOff(&v->env);
	SPUAL_Voice_EnvPush(v);
}

// Walk ADPCM blocks from `start_addr` (in SPU RAM, byte offset) until
// the first block with loop_end set. Compute duration in *output*
// samples (post-pitch). Returns -1 if the loop_end block also has
// loop_repeat (sound is looping; no auto-off) or if pitch is zero
// (paused; treat as looping).
static int32_t SPUAL_ScanOneShotSamples(uint32_t start_addr, uint16_t pitch)
{
	const int BLOCK_SIZE = 16;
	const int SAMPLES_PER_BLOCK = 28;
	const int MAX_BLOCKS = SPU_MEMSIZE / BLOCK_SIZE;
	if (pitch == 0)
		return -1;
	uint32_t addr = start_addr;
	int blocks = 0;
	for (int i = 0; i < MAX_BLOCKS; i++)
	{
		if (addr + BLOCK_SIZE > SPU_MEMSIZE) break;
		uint8_t flags = s_SpuMemory.samplemem[addr + 1];
		blocks++;
		if (flags & 0x01) // loop_end
		{
			if (flags & 0x02) // loop_repeat → infinite
				return -1;
			// One-shot end. Total decoder samples = blocks * 28; output
			// samples = decoder * 4096 / pitch.
			uint64_t decoder_samples = (uint64_t)blocks * SAMPLES_PER_BLOCK;
			uint64_t out_samples = (decoder_samples * 4096ULL) / (uint64_t)pitch;
			if (out_samples > 0x7FFFFFFFULL) out_samples = 0x7FFFFFFFULL;
			return (int32_t)out_samples;
		}
		addr += BLOCK_SIZE;
	}
	// Walked off SPU RAM without finding a loop_end. Treat as looping.
	return -1;
}

static void SPUAL_Voice_KeyOn(SPUALVoice* v)
{
	SPUAL_Voice_EnvKeyOn(v);
	v->one_shot_samples_remaining =
		SPUAL_ScanOneShotSamples(v->attr.addr, v->attr.pitch);
}

ALCdevice*	g_ALCdevice = NULL;
ALCcontext* g_ALCcontext = NULL;
int			g_SPUMuted = 0;

/* --- Audio capture (OpenAL loopback). Enabled by g_psyx_audioCaptureMode,
 *     set by the host (openpete) before PsyX_SPUAL_InitSound, so ONLY
 *     recording sessions switch to a loopback device — normal playback keeps
 *     the real `alcOpenDevice` path untouched. In loopback mode OpenAL does
 *     not auto-play; PsyX_SPUAL_TickEnvelope pulls the mixed PCM with
 *     alcRenderSamplesSOFT, optionally pushes it to an SDL audio stream for
 *     live monitoring, and tees a copy into a ring the host drains into the
 *     recorder (PsyX_SPUAL_CaptureDrain). See openpete-record-audio.md. */
extern "C" int g_psyx_audioCaptureMode = 0;
static int  s_alLoopback = 0;           /* loopback device live */
static LPALCLOOPBACKOPENDEVICESOFT s_alcLoopbackOpenDeviceSOFT = NULL;
static LPALCRENDERSAMPLESSOFT      s_alcRenderSamplesSOFT      = NULL;
static SDL_AudioStream* s_capPlayback = NULL;   /* live monitor (best-effort) */

/* Mixed stereo s16 @ 44100 capture ring. Single-threaded under cooperative
 * fibers, but guarded for safety. Indices/counts are in stereo frames. */
#define SPUAL_CAP_FRAMES (44100)        /* ~1 s of slack */
static int16_t   s_capRing[SPUAL_CAP_FRAMES * 2];
static int       s_capHead = 0, s_capTail = 0, s_capCount = 0;
static SDL_Mutex* s_capMutex = NULL;
static unsigned long long s_capProduced = 0, s_capDropped = 0;
ALuint		g_ALEffectSlots[2];
int			g_currEffectSlotIdx = 0;
ALuint		g_nAlReverbEffect = 0;
int			g_enableSPUReverb = 0;
int			g_ALEffectsSupported = 0;

#ifndef __EMSCRIPTEN__

LPALGENEFFECTS alGenEffects = NULL;
LPALDELETEEFFECTS alDeleteEffects = NULL;
LPALEFFECTI alEffecti = NULL;
LPALEFFECTF alEffectf = NULL;
LPALGENAUXILIARYEFFECTSLOTS alGenAuxiliaryEffectSlots = NULL;
LPALDELETEAUXILIARYEFFECTSLOTS alDeleteAuxiliaryEffectSlots = NULL;
LPALAUXILIARYEFFECTSLOTI alAuxiliaryEffectSloti = NULL;

#endif // __EMSCRIPTEN__

static void InitOpenAlEffects()
{
	g_ALEffectsSupported = 0;
#ifndef __EMSCRIPTEN__
	if (!alcIsExtensionPresent(g_ALCdevice, ALC_EXT_EFX_NAME))
	{
		eprintf("PSX SPU effects are NOT supported!\n");
		return;
	}

	alGenEffects = (LPALGENEFFECTS)alGetProcAddress("alGenEffects");
	alDeleteEffects = (LPALDELETEEFFECTS)alGetProcAddress("alDeleteEffects");
	alEffecti = (LPALEFFECTI)alGetProcAddress("alEffecti");
	alEffectf = (LPALEFFECTF)alGetProcAddress("alEffectf");
	alGenAuxiliaryEffectSlots = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
	alDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
	alAuxiliaryEffectSloti = (LPALAUXILIARYEFFECTSLOTI)alGetProcAddress("alAuxiliaryEffectSloti");

	int max_sends = 0;
	alcGetIntegerv(g_ALCdevice, ALC_MAX_AUXILIARY_SENDS, 1, &max_sends);

	// make reverb effect slot
	g_currEffectSlotIdx = 0;
	alGenAuxiliaryEffectSlots(1, g_ALEffectSlots);

	// make reverb effect
	alGenEffects(1, &g_nAlReverbEffect);
	alEffecti(g_nAlReverbEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);

	// setup defaults of effect
	alEffectf(g_nAlReverbEffect, AL_REVERB_GAIN, 0.45f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_GAINHF, 0.25f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DECAY_TIME, 2.0f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DECAY_HFRATIO, 0.9f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_REFLECTIONS_DELAY, 0.08f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_REFLECTIONS_GAIN, 0.2f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DIFFUSION, 0.9f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_DENSITY, 0.1f);
	alEffectf(g_nAlReverbEffect, AL_REVERB_AIR_ABSORPTION_GAINHF, 0.1f);

	g_ALEffectsSupported = 1;

	eprintf("PSX SPU effects are supported and initialized\n");

	alAuxiliaryEffectSloti(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_EFFECT, g_nAlReverbEffect);
#endif // __EMSCRIPTEN__
}

int PsyX_SPUAL_InitSound()
{
	if (!g_SpuMutex)
		g_SpuMutex = SDL_CreateMutex();

	if (!g_spuInit)
		memset(&s_SpuMemory, 0, sizeof(s_SpuMemory));

	g_spuInit = 1;

	int numDevices, alErr, i;
	const char* devices;
	const char* devStrptr;

	// out_channel_formats snd_outputchannels
	static int al_context_params[] =
	{
		ALC_FREQUENCY, 44100,
#ifndef __EMSCRIPTEN__
		ALC_MAX_AUXILIARY_SENDS, 2,
#endif
		0
	};

	if (g_ALCdevice)
		return 1;

	/* Capture mode: open a loopback device first. It works without a real
	 * output device (e.g. headless), so it is tried before the normal
	 * enumeration/open path below. */
	if (g_psyx_audioCaptureMode)
	{
		s_alcLoopbackOpenDeviceSOFT = (LPALCLOOPBACKOPENDEVICESOFT)alcGetProcAddress(NULL, "alcLoopbackOpenDeviceSOFT");
		s_alcRenderSamplesSOFT      = (LPALCRENDERSAMPLESSOFT)alcGetProcAddress(NULL, "alcRenderSamplesSOFT");
		if (s_alcLoopbackOpenDeviceSOFT && s_alcRenderSamplesSOFT)
		{
			g_ALCdevice = s_alcLoopbackOpenDeviceSOFT(NULL);
			if (g_ALCdevice)
			{
				s_alLoopback = 1;
				if (!s_capMutex) s_capMutex = SDL_CreateMutex();
				s_capHead = s_capTail = s_capCount = 0;
				eprintinfo("audio capture: OpenAL loopback active (44100/stereo/s16)\n");
			}
		}
		if (!s_alLoopback)
			eprinterr("audio capture: ALC_EXT_loopback unavailable; recording will have no audio\n");
	}

	numDevices = 0;

	// Init openAL
	// check devices list

	devStrptr = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
	devices = devStrptr;

	// go through device list (each device terminated with a single NULL, list terminated with double NULL)
	while ((*devStrptr) != '\0')
	{
		eprintinfo("found sound device: %s\n", devStrptr);
		devStrptr += strlen(devStrptr) + 1;
		numDevices++;
	}

	/* Loopback mode already holds a device; don't require enumerated output
	 * devices (headless has none). */
	if (!g_ALCdevice && numDevices == 0)
		return 0;

	if (!g_ALCdevice)
		g_ALCdevice = alcOpenDevice(NULL);

	alErr = AL_NO_ERROR;

	if (!g_ALCdevice)
	{
		alErr = alcGetError(NULL);
		eprinterr("alcOpenDevice: NULL DEVICE error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	/* Loopback contexts must pin the render format we'll pull samples in. */
	static int al_loopback_params[] =
	{
		ALC_FREQUENCY, 44100,
		ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
		ALC_FORMAT_TYPE_SOFT, ALC_SHORT_SOFT,
#ifndef __EMSCRIPTEN__
		ALC_MAX_AUXILIARY_SENDS, 2,
#endif
		0
	};
#ifndef __EMSCRIPTEN__
	g_ALCcontext = alcCreateContext(g_ALCdevice, s_alLoopback ? al_loopback_params : al_context_params);
#else
	g_ALCcontext = alcCreateContext(g_ALCdevice, NULL);
#endif

	alErr = alcGetError(g_ALCdevice);
	if (alErr != AL_NO_ERROR)
	{
		eprinterr("alcCreateContext error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	alcMakeContextCurrent(g_ALCcontext);

	alErr = alcGetError(g_ALCdevice);
	if (alErr != AL_NO_ERROR)
	{
		eprinterr("alcMakeContextCurrent error: %s\n", getALCErrorString(alErr));
		return 0;
	}

	// Setup defaults
	alListenerf(AL_GAIN, 1.0f);
	alDistanceModel(AL_NONE);

	/* Loopback mode best-effort live monitor: loopback OpenAL does not
	 * auto-play, so feed the rendered mix to an SDL output stream for live
	 * sound while recording. Headless / no device -> capture-only (silent
	 * to speakers, still recorded). */
	if (s_alLoopback)
	{
		if (SDL_InitSubSystem(SDL_INIT_AUDIO))
		{
			SDL_AudioSpec spec;
			spec.format = SDL_AUDIO_S16;
			spec.channels = 2;
			spec.freq = 44100;
			s_capPlayback = SDL_OpenAudioDeviceStream(
				SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
			if (s_capPlayback)
				SDL_ResumeAudioStreamDevice(s_capPlayback);
			else
				eprintinfo("audio capture: no output device for live monitor (capture-only)\n");
		}
	}

	// create channels
	for (i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		memset(voice, 0, sizeof(SPUALVoice));

		alGenSources(1, &voice->alSource);
		alGenBuffers(1, &voice->alBuffer);
#ifdef AL_SOFT_source_resampler
		alSourcei(voice->alSource, AL_SOURCE_RESAMPLER_SOFT, 2);	// Use cubic resampler
#endif
		alSourcei(voice->alSource, AL_SOURCE_RELATIVE, AL_TRUE);
	}


	InitOpenAlEffects();

	PsyX_XA_Init();

	return 1;
}

void PsyX_SPUAL_ShutdownSound()
{
	g_spuInit = 0;

#ifndef __EMSCRIPTEN__
	if (!g_ALCcontext)
		return;

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		alDeleteSources(1, &voice->alSource);
		alDeleteBuffers(1, &voice->alBuffer);
	}

	if (g_ALEffectsSupported)
	{
		alDeleteEffects(1, &g_nAlReverbEffect);
		g_ALEffectsSupported = AL_NONE;
		alDeleteAuxiliaryEffectSlots(1, g_ALEffectSlots);
	}

	alcDestroyContext(g_ALCcontext);
	alcCloseDevice(g_ALCdevice);

	g_ALCcontext = NULL;
	g_ALCdevice = NULL;

	/* Tear down loopback capture (the recorder is flushed separately by the
	 * host before this runs). */
	if (s_capPlayback)
	{
		SDL_DestroyAudioStream(s_capPlayback);
		s_capPlayback = NULL;
	}
	s_alLoopback = 0;
	s_capHead = s_capTail = s_capCount = 0;
#endif // __EMSCRIPTEN__
}

//--------------------------------------------------------------------------------

int PsyX_SPUAL_Alloc(int size)
{
	int addr = s_spuMallocVal;
	s_spuMallocVal += size;

	if (s_spuMallocVal > SPU_MEMSIZE)
		return -1;

	return addr;
}

int PsyX_SPUAL_InitAlloc(int num, char* top)
{
	s_spuMallocVal = 0;
	return 0;
}

void PsyX_SPUAL_Free(u_int addr)
{
	s_spuMallocVal = 0;
}

u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr)
{
	s_SpuMemory.writeptr = s_SpuMemory.samplemem + addr;

	if (addr > SPU_MEMSIZE)
		return 0;

	if (addr < 0x1010)
		return 0;

	return 1;
}

u_int PsyX_SPUAL_Write(u_char* addr, u_int size)
{
	//if (0x7EFF0 < size)
	//	size = 0x7EFF0;

	volatile int wptr_ofs = s_SpuMemory.writeptr - s_SpuMemory.samplemem;

	if (wptr_ofs + size > SPU_REALMEMSIZE)
	{
		eprintf("SPU WARNING: SpuWrite exceeded SPU_REALMEMSIZE by %d bytes!\n", wptr_ofs + size - SPU_REALMEMSIZE);
	}
	assert(size > 0 && wptr_ofs + size < SPU_MEMSIZE);

	// simply copy to the writeptr
	memcpy(s_SpuMemory.writeptr, addr, size);

#if 0 // BANK TEST
	{
		static short waveBuffer[SPU_MEMSIZE];

		ALuint alSource;
		ALuint alBuffer;

		alGenSources(1, &alSource);
		alGenBuffers(1, &alBuffer);

		int loopStart = 0, loopLen = 0;
		int count = decodeSound(addr, size, waveBuffer, &loopStart, &loopLen);

		// update AL buffer
		alBufferData(alBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 11000);

		// set the buffer
		alSourcei(alSource, AL_BUFFER, alBuffer);
		alSourcef(alSource, AL_GAIN, 1.0f);// TODO: panning
		alSourcef(alSource, AL_PITCH, 1);

		alSourcePlay(alSource);
		int status;
		do
		{
			alGetSourcei(alSource, AL_SOURCE_STATE, &status);
		} while (status == AL_PLAYING);

		alSourceStop(alSource);

		alDeleteSources(1, &alSource);
		alDeleteBuffers(1, &alBuffer);
	}
#endif

	return size;
}

u_int PsyX_SPUAL_Read(u_char* addr, u_int size)
{
	volatile int rptr_ofs = s_SpuMemory.writeptr - s_SpuMemory.samplemem;

	if (rptr_ofs + size > SPU_REALMEMSIZE)
	{
		eprintf("SPU WARNING: SpuRead exceeded SPU_REALMEMSIZE by %d bytes!\n", rptr_ofs + size - SPU_REALMEMSIZE);
	}
	assert(size > 0 && rptr_ofs + size < SPU_MEMSIZE);

	// simply copy to the writeptr
	memcpy(addr, s_SpuMemory.writeptr, size);

	return size;
}

// PSX ADPCM coefficients
static const float K0[5] = { 0, 0.9375, 1.796875, 1.53125, 1.90625 };
static const float K1[5] = { 0, 0, -0.8125, -0.859375, -0.9375 };

// PSX ADPCM decoding routine - decodes a single sample
static short vagToPcm(u_char soundParameter, int soundData, float* vagPrev1, float* vagPrev2)
{
	int resultInt = 0;

	float dTmp1 = 0.0;
	float dTmp2 = 0.0;
	float dTmp3 = 0.0;

	if (soundData > 7)
		soundData -= 16;

	dTmp1 = (float)soundData * pow(2, (float)(12 - (soundParameter & 0x0F)));

	dTmp2 = (*vagPrev1) * K0[(soundParameter >> 4) & 0x0F];
	dTmp3 = (*vagPrev2) * K1[(soundParameter >> 4) & 0x0F];

	(*vagPrev2) = (*vagPrev1);
	(*vagPrev1) = dTmp1 + dTmp2 + dTmp3;

	resultInt = (int)round((*vagPrev1));

	if (resultInt > 32767)
		resultInt = 32767;

	if (resultInt < -32768)
		resultInt = -32768;

	return (short)resultInt;
}

typedef enum 
{
	LoopEnd = 1 << 0,		// Jump to repeat address after this block
							// 1 - Copy repeatAddress to currentAddress AFTER this block
							//     set ENDX (TODO: Immediately or after this block?)
							// 0 - Nothing

	Repeat = 1 << 1,		// Takes an effect only with LoopEnd bit set.
							// 1 - Loop normally
							// 0 - Loop and force Release

	LoopStart = 1 << 2,		// Mark current address as the beginning of repeat
							// 1 - Load currentAddress to repeatAddress
							// 0 - Nothing
} ADPCM_FLAGS;


// Main decoding routine - Takes PSX ADPCM formatted audio data and converts it to PCM. It also extracts the looping information if used.
static int decodeSound(u_char* iData, int soundSize, short* oData, int* loopStart, int* loopLength, int breakOnEnd /*= 0*/)
{
	u_char sp;
	u_char flag;
	int sd = 0;
	float vagPrev1 = 0.0;
	float vagPrev2 = 0.0;
	int k = 0;

	int loopStrt = 0, loopEnd = 0;
	int breakOn = -1;

	for (int i = 0; i < soundSize; i++)
	{
		if (i % 16 == 0)
		{
			sp = iData[i];
			flag = iData[i+1];
			i += 2;
		}

		sd = (int)iData[i] & 0xF;
		oData[k++] = vagToPcm(sp, sd, &vagPrev1, &vagPrev2);

		sd = ((int)iData[i] >> 4) & 0xF;
		oData[k++] = vagToPcm(sp, sd, &vagPrev1, &vagPrev2);

		if (breakOnEnd && k == breakOn)
			return k;

		if (breakOn == -1)
		{
			// flags parsed
			if (flag & LoopStart)
			{
				loopStrt = k + 26; // FIXME: is that correct?
			}

			if (flag & LoopEnd)
			{
				loopEnd = k + 26;

				if (flag & Repeat)
				{
					*loopStart = loopStrt;
					*loopLength = loopEnd - loopStrt;
				}

				if (breakOnEnd)
					breakOn = k + 26;
			}
		}
	}

	return soundSize;
}

static void UpdateVoiceSample(SPUALVoice* voice)
{
	static short waveBuffer[SPU_REALMEMSIZE];
	int loopStart, loopLen, count;
	ALuint alSource, alBuffer;

	//if (!voice->sampledirty)
	//	return;

	voice->sampledirty = 0;

	alSource = voice->alSource;
	alBuffer = voice->alBuffer;

	if (alSource == AL_NONE)
		return;

	loopStart = 0;
	loopLen = 0;

	/* decodeSound emits up to 2 shorts per input byte; cap the input window
	 * so the output never overruns waveBuffer[SPU_REALMEMSIZE]. Samples that
	 * lack a LoopEnd flag (e.g. uninitialized SPU RAM) would otherwise loop
	 * to the SPU_MEMSIZE end (~2 MB) and write ~8 MB into a 1 MB buffer,
	 * silently corrupting adjacent statics and deferred-crashing the mixer. */
	{
		int avail = SPU_MEMSIZE - voice->attr.addr;
		int max_in = SPU_REALMEMSIZE / 2;  /* yields <= SPU_REALMEMSIZE shorts */
		if (avail > max_in) avail = max_in;
		count = decodeSound(s_SpuMemory.samplemem + voice->attr.addr, avail, waveBuffer, &loopStart, &loopLen, 1);
	}

	if (count == 0)
		return;

#if 0	// sample test
	{
		ALuint aalSource;
		ALuint aalBuffer;

		alGenSources(1, &aalSource);
		alGenBuffers(1, &aalBuffer);

		// update AL buffer
		alBufferData(aalBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 11000);

		// set the buffer
		alSourcei(aalSource, AL_BUFFER, aalBuffer);
		alSourcef(aalSource, AL_GAIN, 1.0f);// TODO: panning
		alSourcef(aalSource, AL_PITCH, 1);

		alSourcePlay(aalSource);
		int status;
		do
		{
			alGetSourcei(aalSource, AL_SOURCE_STATE, &status);
		} while (status == AL_PLAYING);

		alSourceStop(aalSource);

		alDeleteSources(1, &aalSource);
		alDeleteBuffers(1, &aalBuffer);
	}
#endif

	alSourcei(alSource, AL_BUFFER, 0);
	alBufferData(alBuffer, AL_FORMAT_MONO16, waveBuffer, count * sizeof(short), 44100);

	if (loopLen > 0)
	{
		loopStart += voice->attr.loop_addr - voice->attr.addr;

		if (loopStart - 54 > 0 && loopStart + loopLen <= count)
		{
			int sampleOffs[] = { loopStart, loopStart + loopLen };
			alBufferiv(alBuffer, AL_LOOP_POINTS_SOFT, sampleOffs);
		}

		alSourcei(alSource, AL_LOOPING, AL_TRUE);
	}
	else
	{
		//int sampleOffs[] = { 0, 0 };
		//alBufferiv(alBuffer, AL_LOOP_POINTS_SOFT, sampleOffs);
		alSourcei(alSource, AL_LOOPING, AL_FALSE);
	}

	// set the buffer
	alSourcei(alSource, AL_BUFFER, alBuffer);
}

int PsyX_SPUAL_SetMute(int on_off)
{
	int old_state = g_SPUMuted;
	g_SPUMuted = on_off;
	return old_state;
}

void PsyX_SPUAL_GetVoiceVolume(int vNum, short* volL, short* volR)
{
	if (volL)
		*volL = g_SpuVoices[vNum].attr.volume.left;

	if (volR)
		*volR = g_SpuVoices[vNum].attr.volume.right;
}

void PsyX_SPUAL_GetVoicePitch(int vNum, u_short* pitch)
{
	*pitch = g_SpuVoices[vNum].attr.pitch;
}

// Pack PSYQ-style ADSR fields (ar/dr/sr/rr/sl plus a_mode/s_mode/r_mode)
// into PSX hardware ADSR1/ADSR2 register words. Conversion is best-
// effort: PSYQ exposes a more abstract API than the raw register
// bits, so we reconstruct attack_exponential/sustain_direction/etc.
// from the SPU_VOICE_LINEAR*/EXP* mode constants.
static void SPUAL_PackADSR(SPUALVoice* voice, const SpuVoiceAttr* a)
{
	if (a->mask & SPU_VOICE_ADSR_ADSR1) voice->adsr1 = a->adsr1;
	if (a->mask & SPU_VOICE_ADSR_ADSR2) voice->adsr2 = a->adsr2;

	// Attack: only the rate is configurable per-field; SPU_VOICE_ADSR_AMODE
	// chooses linear vs exponential.
	if (a->mask & SPU_VOICE_ADSR_AR)
	{
		voice->adsr1 = (uint16_t)((voice->adsr1 & ~(0x7F << 8))
		                    | ((a->ar & 0x7F) << 8));
	}
	if (a->mask & SPU_VOICE_ADSR_AMODE)
	{
		bool exp = (a->a_mode == SPU_VOICE_EXPIncN || a->a_mode == SPU_VOICE_EXPIncR);
		voice->adsr1 = (uint16_t)((voice->adsr1 & ~(1u << 15))
		                    | ((exp ? 1u : 0u) << 15));
	}
	if (a->mask & SPU_VOICE_ADSR_DR)
	{
		voice->adsr1 = (uint16_t)((voice->adsr1 & ~(0xF << 4))
		                    | ((a->dr & 0xF) << 4));
	}
	if (a->mask & SPU_VOICE_ADSR_SL)
	{
		voice->adsr1 = (uint16_t)((voice->adsr1 & ~0xF)
		                    | (a->sl & 0xF));
	}
	if (a->mask & SPU_VOICE_ADSR_SR)
	{
		voice->adsr2 = (uint16_t)((voice->adsr2 & ~(0x7F << 6))
		                    | ((a->sr & 0x7F) << 6));
	}
	if (a->mask & SPU_VOICE_ADSR_SMODE)
	{
		bool dec = (a->s_mode == SPU_VOICE_LINEARDecN || a->s_mode == SPU_VOICE_LINEARDecR ||
		            a->s_mode == SPU_VOICE_EXPDecN    || a->s_mode == SPU_VOICE_EXPDecR);
		bool exp = (a->s_mode == SPU_VOICE_EXPIncN || a->s_mode == SPU_VOICE_EXPIncR ||
		            a->s_mode == SPU_VOICE_EXPDecN || a->s_mode == SPU_VOICE_EXPDecR);
		voice->adsr2 = (uint16_t)((voice->adsr2 & ~((1u << 14) | (1u << 15)))
		                    | ((dec ? 1u : 0u) << 14)
		                    | ((exp ? 1u : 0u) << 15));
	}
	if (a->mask & SPU_VOICE_ADSR_RR)
	{
		voice->adsr2 = (uint16_t)((voice->adsr2 & ~0x1F)
		                    | (a->rr & 0x1F));
	}
	if (a->mask & SPU_VOICE_ADSR_RMODE)
	{
		bool exp = (a->r_mode == SPU_VOICE_EXPDec || a->r_mode == SPU_VOICE_EXPDecR ||
		            a->r_mode == SPU_VOICE_EXPDecN);
		voice->adsr2 = (uint16_t)((voice->adsr2 & ~(1u << 5))
		                    | ((exp ? 1u : 0u) << 5));
	}
}

void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* psxAttrib)
{
	if (!g_spuInit)
		return;

	const float STEREO_FACTOR = 3.0f;

	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((psxAttrib->voice & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		// ADSR mirror + addr/pitch/volume mirror are independent of
		// OpenAL — always update so the envelope sim runs even when
		// the audio device is missing.
		SPUAL_PackADSR(voice, psxAttrib);

		if (psxAttrib->mask & SPU_VOICE_WDSA)
		{
			if (voice->attr.addr != psxAttrib->addr)
				voice->sampledirty++;
			voice->attr.addr = psxAttrib->addr;
		}
		if (psxAttrib->mask & SPU_VOICE_LSAX)
		{
			if (voice->attr.loop_addr != psxAttrib->loop_addr)
				voice->sampledirty++;
			voice->attr.loop_addr = psxAttrib->loop_addr;
		}
		if (psxAttrib->mask & SPU_VOICE_VOLL)
			voice->attr.volume.left = psxAttrib->volume.left;
		if (psxAttrib->mask & SPU_VOICE_VOLR)
			voice->attr.volume.right = psxAttrib->volume.right;
		if (psxAttrib->mask & SPU_VOICE_PITCH)
			voice->attr.pitch = psxAttrib->pitch;

		if (alSource == AL_NONE)
			continue;

		// OpenAL playback path: gain/panning/pause are derived from
		// the mirror written above. Skipped entirely when no audio
		// device is attached.
		if ((psxAttrib->mask & SPU_VOICE_VOLL) || (psxAttrib->mask & SPU_VOICE_VOLR))
		{
			float left_gain = (float)(voice->attr.volume.left) / (float)(16384);
			float right_gain = (float)(voice->attr.volume.right) / (float)(16384);
			if (left_gain > 1.0f) left_gain = 1.0f;
			if (right_gain > 1.0f) right_gain = 1.0f;
			float pan = (acosf(left_gain) + asinf(right_gain)) / ((float)(M_PI));
			pan = 2.0f * pan - 1.0f;
			pan = pan * 0.5f;
			alSource3f(alSource, AL_POSITION, pan * STEREO_FACTOR, 0, -sqrtf(1.0f - pan * pan));
			alSourcef(alSource, AL_GAIN, (left_gain + right_gain) * 0.5f);
		}
		if (psxAttrib->mask & SPU_VOICE_PITCH)
		{
			ALint state;
			alGetSourcei(alSource, AL_SOURCE_STATE, &state);
			if (psxAttrib->pitch == 0 && state == AL_PLAYING)
				alSourcePause(alSource);
			else if (voice->attr.pitch == 0 && state == AL_PAUSED)
				alSourcePlay(alSource);
			const float pitch = (float)(voice->attr.pitch) / 4096.0f;
			alSourcef(alSource, AL_PITCH, pitch);
		}
	}
	SDL_UnlockMutex(g_SpuMutex);
}

void PsyX_SPUAL_SetKey(int on_off, u_int voice_bit)
{
	if (!g_spuInit)
		return;

	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;

		// Update game-logic state (keyed_on + ADSR sim) regardless of
		// whether OpenAL has a source to play through. Without this,
		// SpuGetKeyStatus would never report SPU_RESET in headless or
		// no-audio-device runs and the game would leak voice slots.
		if (on_off)
		{
			voice->keyed_on = 1;
			SPUAL_Voice_KeyOn(voice);
		}
		else
		{
			voice->keyed_on = 0;
			SPUAL_Voice_EnvKeyOff(voice);
		}

		if (alSource == AL_NONE)
			continue;

		if (on_off && !g_SPUMuted)
		{
			alSourceStop(alSource);
			UpdateVoiceSample(voice);

			alSourcePlay(alSource);
		}
		else
		{
			alSourceStop(alSource);
		}
	}
	SDL_UnlockMutex(g_SpuMutex);
}

// Map ADSR phase + keyed_on to PSX libspu key-status return code.
// Spyro's spu.c (and many other games) polls this to detect natural
// sample completion and free voice slots; without correct mapping
// the 24-voice pool leaks and subsequent sounds drop.
//   SPU_OFF        = 0  key-off, env-off  (idle / never keyed)
//   SPU_ON         = 1  key-on,  env-on   (playing)
//   SPU_OFF_ENV_ON = 2  key-off, env-on   (release fade in progress)
//   SPU_ON_ENV_OFF = 3  key-on,  env-off  (= SPU_RESET; finished while keyed)
static int SPUAL_VoiceKeyStatus(const SPUALVoice* v)
{
	const uint8_t phase = v->adsr_phase;
	if (phase == PSYX_SPU_ENV_OFF)
		return v->keyed_on ? SPU_ON_ENV_OFF : SPU_OFF;
	if (phase == PSYX_SPU_ENV_RELEASE)
		return SPU_OFF_ENV_ON;
	return SPU_ON;
}

int PsyX_SPUAL_GetKeyStatus(u_int voice_bit)
{
	int result = SPU_OFF;
	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if (voice_bit != SPU_VOICECH(i))
			continue;
		result = SPUAL_VoiceKeyStatus(&g_SpuVoices[i]);
		break;
	}
	SDL_UnlockMutex(g_SpuMutex);
	return result;
}

void PsyX_SPUAL_GetAllKeysStatus(char* status)
{
	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
		status[i] = (char)SPUAL_VoiceKeyStatus(&g_SpuVoices[i]);
	SDL_UnlockMutex(g_SpuMutex);
}

void PsyX_SPUAL_TickEnvelope(int samples)
{
	if (samples <= 0) return;
	SDL_LockMutex(g_SpuMutex);
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* v = &g_SpuVoices[i];
		if (v->adsr_phase == PSYX_SPU_ENV_OFF)
			continue;
		for (int s = 0; s < samples; s++)
		{
			// One-shot ADPCM auto-key-off: when output-sample countdown
			// hits zero, force voice to Off (mirrors PSX SPU behavior on
			// loop_end+!loop_repeat block).
			if (v->one_shot_samples_remaining > 0)
			{
				v->one_shot_samples_remaining--;
				if (v->one_shot_samples_remaining == 0)
				{
					v->adsr_phase = PSYX_SPU_ENV_OFF;
					v->adsr_volume = 0;
					break;
				}
			}
			SPUAL_Voice_EnvTick(v);
			if (v->adsr_phase == PSYX_SPU_ENV_OFF)
				break;
		}
	}
	SDL_UnlockMutex(g_SpuMutex);

	/* Loopback capture: pull the mixed PCM for these `samples` frames (the
	 * SPU envelope cadence is the audio clock — 735 frames/VBL), feed the
	 * live monitor, and tee into the capture ring for the recorder. Rendered
	 * outside g_SpuMutex; audio runs on the single game/host fiber so the
	 * OpenAL state is stable here. */
	if (s_alLoopback && s_alcRenderSamplesSOFT)
	{
		int16_t tmp[2048 * 2];
		int remaining = samples;
		while (remaining > 0)
		{
			const int n = remaining > 2048 ? 2048 : remaining;
			s_alcRenderSamplesSOFT(g_ALCdevice, tmp, n);  /* n stereo frames */
			if (s_capPlayback)
				SDL_PutAudioStreamData(s_capPlayback, tmp, n * 2 * (int)sizeof(int16_t));
			SDL_LockMutex(s_capMutex);
			for (int f = 0; f < n; f++)
			{
				if (s_capCount >= SPUAL_CAP_FRAMES)
				{
					/* Ring full (consumer behind) — drop oldest. */
					s_capTail = (s_capTail + 1) % SPUAL_CAP_FRAMES;
					s_capCount--;
					s_capDropped++;
				}
				s_capRing[s_capHead * 2 + 0] = tmp[f * 2 + 0];
				s_capRing[s_capHead * 2 + 1] = tmp[f * 2 + 1];
				s_capHead = (s_capHead + 1) % SPUAL_CAP_FRAMES;
				s_capCount++;
				s_capProduced++;
			}
			SDL_UnlockMutex(s_capMutex);
			remaining -= n;
		}
	}
}

/* Drain up to max_frames mixed stereo s16 frames into `out` (interleaved
 * L,R). Returns the number of stereo frames written. 0 if capture inactive.
 * Called by the host (openpete) to feed the recorder's audio stream. */
extern "C" int PsyX_SPUAL_CaptureDrain(int16_t* out, int max_frames)
{
	if (!s_alLoopback || !s_capMutex || !out || max_frames <= 0)
		return 0;
	int drained = 0;
	SDL_LockMutex(s_capMutex);
	while (drained < max_frames && s_capCount > 0)
	{
		out[drained * 2 + 0] = s_capRing[s_capTail * 2 + 0];
		out[drained * 2 + 1] = s_capRing[s_capTail * 2 + 1];
		s_capTail = (s_capTail + 1) % SPUAL_CAP_FRAMES;
		s_capCount--;
		drained++;
	}
	SDL_UnlockMutex(s_capMutex);
	return drained;
}

/* 1 if loopback capture is live (host should pull audio), else 0. */
extern "C" int PsyX_SPUAL_CaptureActive(void)
{
	return s_alLoopback;
}

int PsyX_SPUAL_SetReverb(int on_off)
{
	int old_state = g_enableSPUReverb;
	g_enableSPUReverb = on_off;

	if (!g_spuInit)
		return old_state;
#ifndef __EMSCRIPTEN__
	// switch if needed
	if (g_ALEffectsSupported && old_state != g_enableSPUReverb)
	{
		if (g_enableSPUReverb)
		{
			alAuxiliaryEffectSloti(g_ALEffectSlots[g_currEffectSlotIdx], AL_EFFECTSLOT_EFFECT, g_nAlReverbEffect);
		}
		else
		{
			g_currEffectSlotIdx = 0;
			alAuxiliaryEffectSloti(g_ALEffectSlots[0], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
			alAuxiliaryEffectSloti(g_ALEffectSlots[1], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
		}
	}
#endif // __EMSCRIPTEN__
	return old_state;
}

int PsyX_SPUAL_GetReverbState()
{
	return g_enableSPUReverb;
}

u_int PsyX_SPUAL_SetReverbVoice(int on_off, u_int voice_bit)
{
	if (!g_spuInit)
		return 0;

	if (!g_ALEffectsSupported)
		return 0;

	SDL_LockMutex(g_SpuMutex);

	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		if ((voice_bit & SPU_VOICECH(i)) == 0)
			continue;

		SPUALVoice* voice = &g_SpuVoices[i];
		ALuint alSource = voice->alSource;
		if (alSource == AL_NONE)
			continue;

		voice->reverb = on_off > 0;
#ifndef __EMSCRIPTEN__
		if (on_off)
			alSource3i(alSource, AL_AUXILIARY_SEND_FILTER, g_ALEffectSlots[g_currEffectSlotIdx], 0, AL_FILTER_NULL);
		else
			alSource3i(alSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
#endif // __EMSCRIPTEN__
	}

	SDL_UnlockMutex(g_SpuMutex);

	return 0;
}

u_int PsyX_SPUAL_GetReverbVoice()
{
	u_int bits = 0;
	for (int i = 0; i < s_spuVoiceCount; i++)
	{
		SPUALVoice* voice = &g_SpuVoices[i];
		if (voice->reverb)
			bits |= SPU_KEYCH(i);
	}
	return bits;
}