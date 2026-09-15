#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "PsyX_pad.h"
#include "PsyX/PsyX_public.h"

#include <string.h>
#include <stdlib.h>  /* abs — SDL3 headers no longer leak <stdlib.h> */

extern "C"
{
extern int g_padCommEnable;
}

typedef struct
{
	Sint32				deviceId;	// linked device Id (SDL_JoystickID instance id)
	SDL_Gamepad*		gc;

	u_char*				padData;
} PsyXController;

int						g_cfg_controllerToSlotMapping[MAX_CONTROLLERS] = { -1, -1 };

PsyXController			g_controllers[MAX_CONTROLLERS];
const bool*				g_sdlKeyboardState = NULL;  /* SDL3: keyboard state is bool[] */

/* Default 0xFFFF = "no override, poll SDL normally". Host can stomp this
 * per-frame to drive scripted input (--bk2 / future record). C linkage so
 * openpete_core can extern it via PsyX/PsyX_public.h. */
extern "C" {
unsigned short g_PsyX_Pad_InputOverride = 0xFFFF;
int g_PsyX_Pad_OverrideActive = 0;
/* Analog-axis override for --bk2 replay; PSX pad->analog order (RX, RY, LX,
 * LY), 128 = centre. Applied whenever the override is active (the pad is
 * always presented analog). See PsyX_public.h. */
unsigned char g_PsyX_Pad_AnalogOverride[4] = { 128, 128, 128, 128 };
}

/* See PsyX_public.h. 1 once slot 0 has a registered pad buffer and pad comms
 * are enabled — i.e. once PsyX_Pad_InternalPadUpdates actually writes anything.
 * Before the game calls PadInitDirect there is no buffer to write, so the
 * "last P1" readbacks return their no-buffer defaults rather than real state. */
extern "C" int PsyX_Pad_P1Ready(void)
{
	return (g_padCommEnable != 0 && g_controllers[0].padData != NULL) ? 1 : 0;
}

/* See PsyX_public.h. Returns the last-merged P1 pad bits (0xFFFF when no
 * pad buffer is registered yet — happens before the game calls PadInitDirect
 * and during boot frames before the first internal-pad-update tick). */
extern "C" unsigned short PsyX_Pad_GetLastP1Bits(void)
{
	if (!g_controllers[0].padData) return 0xFFFF;
	LPPADRAW pad = (LPPADRAW)g_controllers[0].padData;
	return *(u_short*)pad->buttons;
}

/* See PsyX_public.h. Fills out_axes[4] with P1's analog axes in PSX
 * pad->analog order (RX, RY, LX, LY). */
extern "C" void PsyX_Pad_GetLastP1Analog(unsigned char out_axes[4])
{
	if (!g_controllers[0].padData)
	{
		out_axes[0] = out_axes[1] = out_axes[2] = out_axes[3] = 128;
		return;
	}
	LPPADRAW pad = (LPPADRAW)g_controllers[0].padData;
	out_axes[0] = pad->analog[0];
	out_axes[1] = pad->analog[1];
	out_axes[2] = pad->analog[2];
	out_axes[3] = pad->analog[3];
}

/* See PsyX_public.h. Raw P1 pad for host UI, read straight from SDL so it's
 * unaffected by g_PsyX_Pad_InputOverride (which the settings menu holds neutral
 * while it owns input). Uses the first connected controller slot. */
extern "C" int PsyX_Pad_GetRawP1(PsyX_RawPad* out)
{
	if (!out)
		return 0;
	memset(out, 0, sizeof(*out));

	SDL_Gamepad* gc = NULL;
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		if (g_controllers[i].gc && SDL_GamepadConnected(g_controllers[i].gc))
		{
			gc = g_controllers[i].gc;
			break;
		}
	}
	if (!gc)
		return 0;

	out->dpad_up    = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_UP)    ? 1 : 0;
	out->dpad_down  = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_DOWN)  ? 1 : 0;
	out->dpad_left  = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_LEFT)  ? 1 : 0;
	out->dpad_right = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1 : 0;
	out->cross      = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_SOUTH)      ? 1 : 0;
	out->circle     = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_EAST)       ? 1 : 0;
	out->start      = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_START)      ? 1 : 0;
	out->lstick_x   = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFTX);
	out->lstick_y   = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFTY);
	out->rstick_x   = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHTX);
	out->rstick_y   = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHTY);
	out->l1         = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)  ? 1 : 0;
	out->r1         = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1 : 0;
	/* Triggers are analog [0, 32767]; report a digital "pressed" past a
	 * generous threshold (host UI only wants an on/off edge). */
	out->l2         = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 8000 ? 1 : 0;
	out->r2         = SDL_GetGamepadAxis(gc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000 ? 1 : 0;
	out->l3         = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_LEFT_STICK)  ? 1 : 0;
	out->r3         = SDL_GetGamepadButton(gc, SDL_GAMEPAD_BUTTON_RIGHT_STICK) ? 1 : 0;
	return 1;
}

u_short PsyX_Pad_UpdateKeyboardInput();
void	PsyX_Pad_UpdateGameControllerInput(SDL_Gamepad* cont, LPPADRAW pad);
void	PsyX_Pad_DecodeLiveSlot(int i, u_short kbInputs, LPPADRAW pad);
void	PsyX_Pad_Event_ControllerAdded(Sint32 deviceId);
static bool PsyX_Pad_UpdateKeyboardLeftStick(u_char* outX, u_char* outY);

// Initializes SDL controllers
int PsyX_Pad_InitSystem()
{
	// do not init second time!
	if (g_sdlKeyboardState != NULL)
		return 1;

	memset(g_controllers, 0, sizeof(g_controllers));

	// init keyboard state
	g_sdlKeyboardState = SDL_GetKeyboardState(NULL);

	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
	{
		eprinterr("Failed to initialise SDL gamepad subsystem!\n");
		return 0;
	}

	// Supplemental mapping databases are loaded by the host once this
	// subsystem is up, so that a per-user file can outrank the shipped one.
	// Loading any of them here would place them below the shipped copy and
	// silently overwrite the user's own entry for the same pad.

	/* Defensive cold-plug enumeration. SDL2 normally synthesizes
	 * CONTROLLERDEVICEADDED for already-present devices when the
	 * GameController subsystem inits, and PsyX_Sys_DoPollEvent later
	 * drains them into PsyX_Pad_Event_ControllerAdded. That's enough
	 * for plain XInput/HID pads.
	 *
	 * It is NOT enough for Steam Input on Windows (notably the 2026
	 * Steam Controller, whose only public path is the Steam-injected
	 * virtual gamepad). Steam hooks the process slightly after we
	 * init SDL, so SDL has nothing to synthesize for at that moment;
	 * the pad then becomes visible only after a true hot-plug event
	 * fires later. Symptom: controller works iff (re)attached AFTER
	 * launch.
	 *
	 * Walking SDL_GetGamepads() here closes the cold-plug case
	 * directly, independent of whichever backend's enumeration timing
	 * you ended up with. PsyX_Pad_OpenController is idempotent.
	 *
	 * SDL3: SDL_GetGamepads returns instance ids of gamepad-capable
	 * devices only, so the old per-index SDL_IsGameController filter is
	 * no longer needed. */
	/* openpete cold-plug fix: the memset above left every slot deviceId==0, but
	 * PsyX_Pad_Event_ControllerAdded only binds a slot whose deviceId is -1
	 * (auto) — so the enumeration below would open nothing for a pad present at
	 * launch (the -1 is otherwise written only later, by PsyX_Pad_InitPad on the
	 * game's PadInitDirect, by which point the cold-plug ADDED event has already
	 * fired and been wasted). Seed the auto-assign sentinel now — the same value
	 * InitPad uses — so the walk below can claim a launch-present pad. */
	for (int i = 0; i < MAX_CONTROLLERS; i++)
		g_controllers[i].deviceId = g_cfg_controllerToSlotMapping[i];

	int n = 0;
	SDL_JoystickID* gamepads = SDL_GetGamepads(&n);
	if (gamepads)
	{
		for (int j = 0; j < n; j++)
			PsyX_Pad_Event_ControllerAdded(gamepads[j]);
		SDL_free(gamepads);
	}

	return 1;
}

// Prints controller list into console
void PsyX_Pad_Debug_ListControllers()
{
	int numJoysticks = 0;
	SDL_JoystickID* gamepads = SDL_GetGamepads(&numJoysticks);
	int numHaptics = 0;
	SDL_HapticID* haptics = SDL_GetHaptics(&numHaptics);

	if (numJoysticks && gamepads)
	{
		eprintf("SDL gamepad list:\n");

		for (int i = 0; i < numJoysticks; i++)
		{
			eprintinfo("  %u '%s'\n", gamepads[i], SDL_GetGamepadNameForID(gamepads[i]));
		}
	}
	else
		eprintwarn("No SDL gamepads found!\n");

	if (numHaptics && haptics)
	{
		eprintf("SDL haptic list:\n");

		for (int i = 0; i < numHaptics; i++)
		{
			eprintinfo("  %u '%s'\n", haptics[i], SDL_GetHapticNameForID(haptics[i]));
		}
	}
	else
		eprintwarn("No SDL haptics found!\n");

	SDL_free(gamepads);
	SDL_free(haptics);
}

// Opens specific system controller and assigns to specified slot
void PsyX_Pad_OpenController(Sint32 deviceId, int slot)
{
	PsyXController* controller = &g_controllers[slot];

	if (controller->gc)
	{
		return;
	}

	controller->gc = SDL_OpenGamepad(deviceId);

	if (controller->gc)
	{
		// assign device id automatically
		if (controller->deviceId == -1)
			controller->deviceId = deviceId;
	}
}

// Closes controller in specific slot
void PsyX_Pad_CloseController(int slot)
{
	PsyXController* controller = &g_controllers[slot];
	SDL_CloseGamepad(controller->gc);

	controller->gc = NULL;
}

/* libpad's answer for a port with no controller: status 0xFF, every other
 * byte 0. Slot 0 always has the keyboard; another slot is empty when no
 * gamepad is bound to it and the keyboard is not assigned to it. */
static bool PsyX_Pad_SlotEmpty(int i)
{
	return i != 0 && g_controllers[i].gc == NULL && !(g_activeKeyboardControllers & (1 << i));
}

// Called from LIBPAD
void PsyX_Pad_InitPad(int slot, u_char* padData)
{
	PsyXController* controller = &g_controllers[slot];

	controller->padData = padData;

	/* openpete cold-plug fix: don't stomp a slot already bound to an open
	 * controller (e.g. one the init enumeration opened before the game called
	 * PadInitDirect); keep its real instance id so unplug still matches it. */
	if (!controller->gc)
		controller->deviceId = g_cfg_controllerToSlotMapping[slot];

	if (padData)
	{
		LPPADRAW pad = (LPPADRAW)padData;
		
		bool wasConnected = (pad->id == 0x41 || pad->id == 0x73);

		if (PsyX_Pad_SlotEmpty(slot))
		{
			memset(pad, 0, sizeof(*pad));
			pad->status = 0xFF;
			return;
		}

		if(!wasConnected)
			// slot 0 is always present (keyboard is the main controller);
			// 0x73 = analog pad — analog mode is on by default.
			pad->id = 0x73;

		// only reset buttons
		pad->buttons[0] = 0xFF;
		pad->buttons[1] = 0xFF;
		pad->analog[0] = 128;
		pad->analog[1] = 128;
		pad->analog[2] = 128;
		pad->analog[3] = 128;
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId)
{
	int i;
	PsyXController* controller;

	// reinitialize haptics (why we still here?)
	SDL_QuitSubSystem(SDL_INIT_HAPTIC);			// FIXME: this will crash if you already have haptics
	SDL_InitSubSystem(SDL_INIT_HAPTIC);

	PsyX_Pad_Debug_ListControllers();

	// find mapping and open
	for (i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->deviceId == -1 || controller->deviceId == deviceId)
		{
			PsyX_Pad_OpenController(deviceId, i);
			break;
		}
	}
}

// called from Psy-X SDL events
void PsyX_Pad_Event_ControllerRemoved(Sint32 deviceId)
{
	int i;
	PsyXController* controller;

	PsyX_Pad_Debug_ListControllers();

	// find mapping and close
	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->deviceId == deviceId)
		{
			PsyX_Pad_CloseController(i);
			/* openpete cold-plug fix: release the slot back to its configured
			 * (auto-assign, -1) binding so a re-plugged controller — which SDL
			 * hands a NEW instance id — can re-claim this slot. Without this the
			 * slot stays pinned to the now-dead id and the re-plug spills into
			 * the next slot (player 2) or nowhere. */
			controller->deviceId = g_cfg_controllerToSlotMapping[i];
		}
	}
}

/* Live SDL -> pad decode for one slot: gamepad first, then keyboard-as-pad
 * merged on top. Extracted so the host can decode a pad value WITHOUT owning
 * a PSX pad buffer (PsyX_Pad_DecodeP1) and get a bit-identical result by
 * construction — there is exactly one implementation of this merge.
 *
 * `pad` is fully written (buttons and all four axes) regardless of prior
 * contents: PsyX_Pad_UpdateGameControllerInput assigns rather than
 * accumulates, including its no-controller case (0xFFFF buttons, axes 128). */
void PsyX_Pad_DecodeLiveSlot(int i, u_short kbInputs, LPPADRAW pad)
{
	PsyX_Pad_UpdateGameControllerInput(g_controllers[i].gc, pad);

	/* Merge keyboard-as-pad into this slot IN ADDITION to any gamepad
	 * polled above, so keyboard and controller both drive the game at
	 * once (buttons are active-low ANDed — either source presses; WASD
	 * overrides the left stick only while held). We used to gate this on
	 * !controller->gc ("a controller owns its slot") to stop a Steam-
	 * Input config that types phantom keys from bleeding into gameplay,
	 * but that killed WASD/ZXCV/arrows entirely whenever any pad was
	 * connected. Same Steam-side-curable trade-off accepted for host
	 * hotkeys (see openpete run.c). */
	if (g_activeKeyboardControllers & (1 << i))
	{
		u_char kb_lx, kb_ly;
		bool wasd_held = PsyX_Pad_UpdateKeyboardLeftStick(&kb_lx, &kb_ly);

		if (wasd_held)
		{
			/* WASD -> left analog stick. The pad is always in
			 * analog mode (see PsyX_Pad_InitPad), so the game
			 * samples pad->analog[2..3]. */
			pad->analog[2] = kb_lx;
			pad->analog[3] = kb_ly;
			pad->status = 0;
		}

		if (kbInputs != 0xffff)
		{
			pad->status = 0;	// PadStateStable?
			*(u_short*)pad->buttons &= kbInputs;
		}
	}
}

/* See PsyX_public.h. Decodes slot 0's live SDL input into a caller-owned
 * snapshot without touching any registered pad buffer, so the host can own
 * decode (and choose when to latch it) while injection still happens through
 * the single override seam. Runs the same keyboard read and the same
 * per-slot merge the non-override path uses. */
extern "C" void PsyX_Pad_DecodeP1(unsigned short* out_bits, unsigned char out_axes[4])
{
	PADRAW scratch;
	memset(&scratch, 0, sizeof(scratch));

	PsyX_Pad_DecodeLiveSlot(0, PsyX_Pad_UpdateKeyboardInput(), &scratch);

	if (out_bits) *out_bits = *(u_short*)scratch.buttons;
	if (out_axes)
	{
		out_axes[0] = scratch.analog[0];
		out_axes[1] = scratch.analog[1];
		out_axes[2] = scratch.analog[2];
		out_axes[3] = scratch.analog[3];
	}
}

/* See PsyX_public.h. Same merge as the slot decode, against a caller-supplied
 * gamepad rather than one of the two tracked controller slots. */
extern "C" void PsyX_Pad_DecodeGamepad(void* gamepad, unsigned short* out_bits,
                                       unsigned char out_axes[4])
{
	PADRAW scratch;
	memset(&scratch, 0, sizeof(scratch));

	/* Assigns rather than accumulates, including its no-controller case
	 * (0xFFFF buttons, axes 128), so a NULL gamepad needs no special path. */
	PsyX_Pad_UpdateGameControllerInput((SDL_Gamepad*)gamepad, &scratch);

	if (out_bits) *out_bits = *(u_short*)scratch.buttons;
	if (out_axes)
	{
		out_axes[0] = scratch.analog[0];
		out_axes[1] = scratch.analog[1];
		out_axes[2] = scratch.analog[2];
		out_axes[3] = scratch.analog[3];
	}
}

void PsyX_Pad_InternalPadUpdates()
{
	PsyXController* controller;
	LPPADRAW pad;
	u_short kbInputs;

	if (g_padCommEnable == 0)
		return;

	/* --bk2 / future scripted-input path: when the host has set an override,
	 * skip the SDL keyboard read entirely. Game-controller input is also
	 * skipped below (we replace, don't AND), so the override wins cleanly
	 * even if a real pad is plugged in. Gated on the active flag (not a 0xFFFF
	 * sentinel) so an analog frame with no buttons still overrides. */
	if (g_PsyX_Pad_OverrideActive)
	{
		kbInputs = g_PsyX_Pad_InputOverride;
	}
	else
	{
		kbInputs = PsyX_Pad_UpdateKeyboardInput();
	}

	for (int i = 0; i < MAX_CONTROLLERS; i++)
	{
		controller = &g_controllers[i];

		if (controller->padData)
		{
			pad = (LPPADRAW)controller->padData;

			if (PsyX_Pad_SlotEmpty(i))
			{
				memset(pad, 0, sizeof(*pad));
				pad->status = 0xFF;
				continue;
			}

			/* Override path (--bk2 replay): write kbInputs + the recorded analog
			 * axes straight in and skip gamepad/keyboard polling (the override is
			 * the single source of truth). The pad is always presented analog (id
			 * 0x73) — Spyro plays identically in digital vs analog mode except that
			 * analog drives the stick, so a digital-only movie just replays with
			 * the axes centred (128). */
			if (g_PsyX_Pad_OverrideActive)
			{
				pad->status = 0;
				pad->id = 0x73;
				pad->analog[0] = g_PsyX_Pad_AnalogOverride[0];
				pad->analog[1] = g_PsyX_Pad_AnalogOverride[1];
				pad->analog[2] = g_PsyX_Pad_AnalogOverride[2];
				pad->analog[3] = g_PsyX_Pad_AnalogOverride[3];
				*(u_short*)pad->buttons = kbInputs;
				continue;
			}

			PsyX_Pad_DecodeLiveSlot(i, kbInputs, pad);
		}
	}

#if defined(__ANDROID__)
	///@TODO SDL_NumJoysticks always reports > 0 for some reason on Android.
#endif
}


int GetControllerButtonState(SDL_Gamepad* cont, int buttonOrAxis)
{
	if (buttonOrAxis & CONTROLLER_MAP_FLAG_AXIS)
	{
		int value = SDL_GetGamepadAxis(cont, (SDL_GamepadAxis)(buttonOrAxis & ~(CONTROLLER_MAP_FLAG_AXIS | CONTROLLER_MAP_FLAG_INVERSE)));

		if (abs(value) > 500 && (buttonOrAxis & CONTROLLER_MAP_FLAG_INVERSE))
			value *= -1;

		return value;
	}

	return SDL_GetGamepadButton(cont, (SDL_GamepadButton)buttonOrAxis) * 32767;
}

void PsyX_Pad_UpdateGameControllerInput(SDL_Gamepad* cont, LPPADRAW pad)
{
	short leftX, leftY, rightX, rightY;
	u_short ret = 0xFFFF;

	if (!cont)
	{
		// No gamepad: the sticks read as a DualShock's at rest, 0x80 per axis.
		pad->analog[0] = 128;
		pad->analog[1] = 128;
		pad->analog[2] = 128;
		pad->analog[3] = 128;

		*(u_short*)pad->buttons = ret;
		return;
	}

	// TODOL separate mapping for Pad 2
	const PsyXControllerMapping& mapping = g_cfg_controllerMapping;

	if (GetControllerButtonState(cont, mapping.gc_square) > 16384)//Square
		ret &= ~0x8000;

	if (GetControllerButtonState(cont, mapping.gc_circle) > 16384)//Circle
		ret &= ~0x2000;

	if (GetControllerButtonState(cont, mapping.gc_triangle) > 16384)//Triangle
		ret &= ~0x1000;

	if (GetControllerButtonState(cont, mapping.gc_cross) > 16384)//Cross
		ret &= ~0x4000;

	if (GetControllerButtonState(cont, mapping.gc_l1) > 16384)//L1
		ret &= ~0x400;

	if (GetControllerButtonState(cont, mapping.gc_r1) > 16384)//R1
		ret &= ~0x800;

	if (GetControllerButtonState(cont, mapping.gc_l2) > 16384)//L2
		ret &= ~0x100;

	if (GetControllerButtonState(cont, mapping.gc_r2) > 16384)//R2
		ret &= ~0x200;

	if (GetControllerButtonState(cont, mapping.gc_dpad_up) > 16384)//UP
		ret &= ~0x10;

	if (GetControllerButtonState(cont, mapping.gc_dpad_down) > 16384)//DOWN
		ret &= ~0x40;

	if (GetControllerButtonState(cont, mapping.gc_dpad_left) > 16384)//LEFT
		ret &= ~0x80;

	if (GetControllerButtonState(cont, mapping.gc_dpad_right) > 16384)//RIGHT
		ret &= ~0x20;

	if (GetControllerButtonState(cont, mapping.gc_l3) > 16384)//L3
		ret &= ~0x2;

	if (GetControllerButtonState(cont, mapping.gc_r3) > 16384)//R3
		ret &= ~0x4;

	if (GetControllerButtonState(cont, mapping.gc_select) > 16384)//SELECT
		ret &= ~0x1;

	if (GetControllerButtonState(cont, mapping.gc_start) > 16384)//START
		ret &= ~0x8;

	leftX = GetControllerButtonState(cont, mapping.gc_axis_left_x);
	leftY = GetControllerButtonState(cont, mapping.gc_axis_left_y);

	rightX = GetControllerButtonState(cont, mapping.gc_axis_right_x);
	rightY = GetControllerButtonState(cont, mapping.gc_axis_right_y);

	*(u_short*)pad->buttons = ret;

	// map to range
	pad->analog[0] = (rightX / 256) + 128;
	pad->analog[1] = (rightY / 256) + 128;
	pad->analog[2] = (leftX / 256) + 128;
	pad->analog[3] = (leftY / 256) + 128;
}

/* Sample WASD into PSX-style left-stick byte values (centered at 128).
 * Returns true if any axis key is held — caller uses this to flip the
 * keyboard pad into analog mode. */
static bool PsyX_Pad_UpdateKeyboardLeftStick(u_char* outX, u_char* outY)
{
	*outX = 128;
	*outY = 128;

	if (g_sdlKeyboardState == NULL)
		return false;

	const PsyXKeyboardMapping& mapping = g_cfg_keyboardMapping;
	bool held = false;

	if (g_sdlKeyboardState[mapping.kc_axis_left_left])  { *outX = 0;   held = true; }
	if (g_sdlKeyboardState[mapping.kc_axis_left_right]) { *outX = 255; held = true; }
	if (g_sdlKeyboardState[mapping.kc_axis_left_up])    { *outY = 0;   held = true; }
	if (g_sdlKeyboardState[mapping.kc_axis_left_down])  { *outY = 255; held = true; }

	return held;
}

u_short PsyX_Pad_UpdateKeyboardInput()
{
	u_short ret = 0xFFFF;

	//Not initialised yet
	if (g_sdlKeyboardState == NULL)
		return ret;

	SDL_PumpEvents();

	const PsyXKeyboardMapping& mapping = g_cfg_keyboardMapping;

	if (g_sdlKeyboardState[mapping.kc_square])//Square
		ret &= ~0x8000;

	if (g_sdlKeyboardState[mapping.kc_circle])//Circle
		ret &= ~0x2000;

	if (g_sdlKeyboardState[mapping.kc_triangle])//Triangle
		ret &= ~0x1000;

	if (g_sdlKeyboardState[mapping.kc_cross])//Cross
		ret &= ~0x4000;

	if (g_sdlKeyboardState[mapping.kc_l1])//L1
		ret &= ~0x400;

	if (g_sdlKeyboardState[mapping.kc_l2])//L2
		ret &= ~0x100;

	if (g_sdlKeyboardState[mapping.kc_l3])//L3
		ret &= ~0x2;

	if (g_sdlKeyboardState[mapping.kc_r1])//R1
		ret &= ~0x800;

	if (g_sdlKeyboardState[mapping.kc_r2])//R2
		ret &= ~0x200;

	if (g_sdlKeyboardState[mapping.kc_r3])//R3
		ret &= ~0x4;

	if (g_sdlKeyboardState[mapping.kc_dpad_up])//UP
		ret &= ~0x10;

	if (g_sdlKeyboardState[mapping.kc_dpad_down])//DOWN
		ret &= ~0x40;

	if (g_sdlKeyboardState[mapping.kc_dpad_left])//LEFT
		ret &= ~0x80;

	if (g_sdlKeyboardState[mapping.kc_dpad_right])//RIGHT
		ret &= ~0x20;

	if (g_sdlKeyboardState[mapping.kc_select])//SELECT
		ret &= ~0x1;

	if (g_sdlKeyboardState[mapping.kc_start])//START
		ret &= ~0x8;

	return ret;
}

int PsyX_Pad_GetStatus(int mtap, int slot)
{
	PsyXController* controller;

	if (slot == 0)
		return 1;	// keyboard always here

	controller = &g_controllers[slot];

	if (controller->gc && SDL_GamepadConnected(controller->gc))
		return 1;

	return 0;
}

void PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len)
{
	PsyXController* controller = &g_controllers[slot];

	if (len == 0)
		return;

	Uint16 freq_high	= table[0] * 255;
	Uint16 freq_low		= len > 1 ? table[1] * 255 : 0;

	// apply minimal shake
	if(freq_low != 0 && freq_low < 4096)
		freq_low = 4096;

	if (freq_high != 0 && freq_high < 4096)
		freq_high = 4096;

	SDL_RumbleGamepad(controller->gc, freq_low, freq_high, 200);
}

/* openpete: set by the host to take ownership of the actuator path. NULL
 * leaves PadSetAct with its historical one-shot behaviour. */
extern "C" PsyX_Pad_ActuatorRegisterFn g_PsyX_Pad_ActuatorRegister = NULL;

/* openpete: the SDL handle bound to a pad slot.
 *
 * The host drives the actuators itself (libpad registers a buffer and the host
 * ships it every tick — see core/input/host_rumble.c), but the handle stays
 * owned here, where the controller-added / -removed events already maintain
 * the slot mapping, the cold-plug enumeration and the hotplug re-claim. Handing
 * out the pointer keeps one notion of "which pad is player one" instead of
 * growing a second, subtly different one on the host side.
 *
 * void* so PsyX_pad.h stays free of an SDL dependency for its C callers. */
extern "C" void* PsyX_Pad_GetGamepad(int slot)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return NULL;

	return (void*)g_controllers[slot].gc;
}