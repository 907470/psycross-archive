#ifndef EMULATOR_PUBLIC_H
#define EMULATOR_PUBLIC_H

#define CONTROLLER_MAP_FLAG_AXIS		0x4000
#define CONTROLLER_MAP_FLAG_INVERSE		0x8000

typedef struct
{
	int id;

	int kc_square, kc_circle, kc_triangle, kc_cross;

	int kc_l1, kc_l2, kc_l3;
	int kc_r1, kc_r2, kc_r3;

	int kc_start, kc_select;

	int kc_dpad_left, kc_dpad_right, kc_dpad_up, kc_dpad_down;

	/* Left analog stick — keyboard-driven. When any of these is held the
	 * keyboard pad is forced into analog mode (id 0x73) so the game reads
	 * pad->analog[2..3]. Right stick has no keyboard binding. */
	int kc_axis_left_up, kc_axis_left_down, kc_axis_left_left, kc_axis_left_right;
} PsyXKeyboardMapping;

typedef struct
{
	int id;

	// you can bind axis by adding CONTROLLER_MAP_AXIS_FLAG
	int gc_square, gc_circle, gc_triangle, gc_cross;

	int gc_l1, gc_l2, gc_l3;
	int gc_r1, gc_r2, gc_r3;

	int gc_start, gc_select;

	int gc_dpad_left, gc_dpad_right, gc_dpad_up, gc_dpad_down;

	int gc_axis_left_x, gc_axis_left_y;
	int gc_axis_right_x, gc_axis_right_y;
} PsyXControllerMapping;

typedef void(*GameDebugKeysHandlerFunc)(int nKey, char down);
typedef void(*GameDebugMouseHandlerFunc)(int x, int y);
typedef void(*GameOnTextInputHandler)(const char* buf);
typedef void(*PsyXPostRenderOverlayFunc)(void);

//------------------------------------------------------------------------

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

/* Forward decl so PsyX_GetPSXWidescreenMappedViewport's prototype below
 * doesn't introduce a new (parameter-scoped) struct in C TUs that haven't
 * already pulled in psx/libgte.h. The real definition lives there. */
struct _RECT16;

/* Mapped inputs */
extern PsyXControllerMapping		g_cfg_controllerMapping;
extern PsyXKeyboardMapping			g_cfg_keyboardMapping;
extern int							g_cfg_controllerToSlotMapping[2];

/* Game inputs */
extern GameOnTextInputHandler		g_cfg_gameOnTextInput;

/* Graphics configuration */
extern int							g_cfg_swapInterval;
extern int							g_cfg_pgxpZBuffer;
extern int							g_cfg_bilinearFiltering;
extern int							g_cfg_pgxpTextureCorrection;

/* Debug inputs */
extern GameDebugKeysHandlerFunc		g_dbg_gameDebugKeys;
extern GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse;

/* Post-render overlay hook. If non-NULL, called inside PsyX_EndScene after
 * GR_StoreFrameBuffer (so the VRAM mirror is already snapshotted and the
 * overlay won't pollute it) and before GR_SwapWindow (so the overlay lands on
 * the backbuffer that's about to be presented). The callback is responsible
 * for saving/restoring any GL state it touches. */
extern PsyXPostRenderOverlayFunc	g_psyx_postRenderOverlay;

/* Hor+ widescreen aspect factor for the non-PGXP GL render path.
 * Multiplies the effective horizontal extent of GR_Ortho2D in
 * GR_SetOffscreenState (called per scene begin). 1.0f (default) =
 * native 4:3-from-DISPENV behavior; values >1 widen the GL ortho range
 * so geometry the GTE projects beyond the native 320/512 PSX plane is
 * mapped across the SDL window instead of being clipped at the
 * viewport edge.
 *
 * Crucially this does NOT touch DISPENV/DRAWENV VRAM widths, so
 * GR_StoreFrameBuffer's blit-back stays at the native 512 and texture
 * pages at x>=512 are not overwritten. Pair with a host-side widening
 * of SetGeomOffset's x-component so the wider geometry is centred. */
extern float g_psyx_render_aspect_factor;

/* Render the scene at the console's own display raster instead of a
 * square-pixel 4:3 grid derived from the host window height.
 *
 * Normally the present FBO is sized window_h * DISPENV.disp.h/DRAWENV.clip.h
 * tall by 4:3 wide, so the visible crop is a supersampled, square-pixel image
 * that a host window can display without further correction. With this set the
 * FBO is exactly DISPENV.disp.w x DISPENV.disp.h (512x240 for Spyro), so every
 * FBO texel is one PSX pixel: the crop carries the authentic raster, with the
 * console's non-square pixels and its aliasing, and the window blit scales it
 * up. Off by default; the display path is unchanged. */
extern int g_psyx_present_native_raster;

/* Audio capture mode. When non-zero at PsyX_SPUAL_InitSound, OpenAL opens a
 * loopback device instead of the normal output device so the mixed SPU PCM
 * can be pulled (PsyX_SPUAL_CaptureDrain) for --record. The host sets this
 * before init; default 0 leaves normal playback completely unchanged. When
 * ALC_EXT_loopback is unavailable the normal output device opens instead and
 * PsyX_SPUAL_CaptureDrain returns 0. */
extern int g_psyx_audioCaptureMode;

/* Null-GPU mode. When non-zero BEFORE PsyX_Initialise, PsyCross runs as a
 * LIBRARY only: SDL + core (vblank/intr thread) + input + SPU + CDFS come up,
 * but no window, no GL context, and no PSX GPU state are created
 * (GR_InitialiseRender / GR_InitialisePSX skipped), and every GL-touching GR
 * entry point reachable from the libgpu thunks (GR_Clear / GR_UpdateVRAM /
 * GR_ReadFramebufferDataToVRAM / GR_SwapWindow / PsyX_BeginScene) early-
 * returns. The CPU-side `vram` array and its LoadImage/MoveImage/StoreImage
 * semantics stay fully live, so game readbacks behave exactly as before.
 * Set by openpete when the native Vulkan renderer is the sole display
 * (--renderer vk); PsyCross's own renderer remains fully functional for
 * psycross/ab/diff modes when the flag is 0. */
extern int g_psyx_null_gpu;

/* Pad input override (--bk2 replay). When g_PsyX_Pad_OverrideActive != 0,
 * PsyX_Pad_InternalPadUpdates uses these button bits verbatim (same convention
 * as PsyX_Pad_UpdateKeyboardInput: released set, pressed cleared) instead of
 * polling SDL keyboard / gamepad. The separate active flag — not a 0xFFFF
 * sentinel — gates it, so an analog frame with no buttons pressed (bits =
 * 0xFFFF) still overrides and applies its g_PsyX_Pad_AnalogOverride axes. Set
 * per-frame from the host loop; clear the flag when no movie frame is driving
 * input (before the offset, or past the movie end). */
extern unsigned short				g_PsyX_Pad_InputOverride;
extern int							g_PsyX_Pad_OverrideActive;

/* Analog axis override, paired with g_PsyX_Pad_InputOverride for --bk2 replay.
 * Bytes are in PSX pad->analog order (RStick X, RStick Y, LStick X, LStick Y;
 * 128 = centre). While the override is active (g_PsyX_Pad_InputOverride !=
 * 0xFFFF) the pad is always presented analog (id 0x73) with these axes — Spyro
 * behaves identically in digital vs analog mode except that analog drives the
 * stick, so a digital-only movie just replays with these left at 128 (centre). */
extern unsigned char				g_PsyX_Pad_AnalogOverride[4];

/* 1 once P1 has a registered pad buffer and pad comms are on. The readbacks
 * below cannot distinguish "no buffer yet" from "genuinely neutral", so anything
 * comparing against them must gate on this first. */
extern int PsyX_Pad_P1Ready(void);

/* Read P1's last computed pad bitfield (after PsyX_Pad_InternalPadUpdates). Same
 * "released = 1, pressed = 0" convention as g_PsyX_Pad_InputOverride. Returns
 * 0xFFFF when no controller buffer is registered yet. Used by the host's
 * --bk2-record path to capture the merged keyboard/gamepad/override state into
 * a BizHawk-format input log. */
extern unsigned short PsyX_Pad_GetLastP1Bits(void);

/* Read P1's last computed analog axes (after PsyX_Pad_InternalPadUpdates) into
 * out_axes[4] in PSX pad->analog order (RX, RY, LX, LY; 128 = centre); set to
 * 128 when no controller buffer is registered yet. Used by the host's
 * --bk2-record path to capture analog state. */
extern void PsyX_Pad_GetLastP1Analog(unsigned char out_axes[4]);

/* Decode P1's LIVE SDL input (keyboard + gamepad, same merge the non-override
 * path performs) into a caller-owned snapshot, without touching any registered
 * PSX pad buffer and regardless of whether the override is active. Bits use the
 * "released = 1, pressed = 0" convention of g_PsyX_Pad_InputOverride; axes are
 * PSX pad->analog order (RX, RY, LX, LY).
 *
 * This is the seam that lets the host own *decode* while injection stays on the
 * single override channel: the host decodes, arbitrates against its other
 * override writers, then stamps. Sharing this one implementation with the
 * non-override path is what makes the two bit-identical by construction, so no
 * separate decoder has to be proven equal to it.
 *
 * Reads SDL state as of the last event pump; it does not pump. */
extern void PsyX_Pad_DecodeP1(unsigned short* out_bits, unsigned char out_axes[4]);

/* Decode ONE arbitrary gamepad into the same PSX pad bytes, for host-owned
 * pad slots beyond the two this module tracks. `gamepad` is an SDL_Gamepad*
 * (void* so the public header stays SDL-free, as PsyX_Pad_GetGamepad does);
 * NULL yields the no-controller reading rather than leaving `out_*` untouched.
 *
 * Runs the same controller merge as the slot decode above, so a host-owned
 * slot and a PsyCross slot cannot drift apart in mapping, deadzone or centre
 * convention. Keyboard is deliberately NOT merged: the keyboard-as-pad source
 * belongs to player 1, and folding it into every slot would press every
 * player's buttons at once.
 *
 * Reads SDL state as of the last event pump; it does not pump. */
extern void PsyX_Pad_DecodeGamepad(void* gamepad, unsigned short* out_bits,
                                   unsigned char out_axes[4]);

/* Raw P1 controller snapshot for host-side UI (the in-game settings menu), read
 * straight from SDL and INDEPENDENT of g_PsyX_Pad_InputOverride — the menu sets
 * that override to neutral to take input away from the game, which also makes
 * PsyX_Pad_InternalPadUpdates skip the gamepad poll, so the merged bits above go
 * blank while the menu owns input. This reads the live pad directly instead.
 * Returns 1 and fills *out when a game controller is connected (first connected
 * slot), else 0 with *out zeroed. Digital buttons are 0/1; the left-stick axes
 * are signed SDL values [-32768, 32767] (+x right, +y down). Keep this SDL-free
 * at the header boundary so openpete_core needn't pull in SDL. */
typedef struct PsyX_RawPad {
	int dpad_up, dpad_down, dpad_left, dpad_right;
	int cross;                 /* SOUTH face button (PSX X) */
	int circle;                /* EAST face button (PSX O) */
	int start;                 /* START button */
	int lstick_x, lstick_y;    /* left analog stick, signed */
	int rstick_x, rstick_y;    /* right analog stick, signed */
	int l1, r1;                /* shoulder buttons */
	int l2, r2;                /* triggers, 1 when pressed past a threshold */
	int l3, r3;                /* analog stick clicks */
} PsyX_RawPad;
extern int PsyX_Pad_GetRawP1(PsyX_RawPad* out);

/* Usually called at the beginning of main function. `startHidden` creates
 * the window unmapped (no flash / focus-steal) — for headless / offscreen
 * runs that render but never present to a visible window. */
extern void PsyX_Initialise(char* windowName, int screenWidth, int screenHeight, int fullscreen, int startHidden);

/* Cleans all resources and closes open instances */
extern void PsyX_Shutdown(void);

/* Returns the screen size dimensions */
extern void PsyX_GetScreenSize(int* screenWidth, int* screenHeight);

/* Sets mouse cursor position */
extern void PsyX_SetCursorPosition(int x, int y);

/* Usually called after ClearOTag/ClearOTagR */
extern char PsyX_BeginScene(void);

/* Usually called after DrawOTag/DrawOTagEnv */
extern void PsyX_EndScene(void);

/* Re-presents the last stored framebuffer to the window without re-running
 * GR_EndScene / GR_StoreFrameBuffer. Used by render-fps>30 paths that need
 * to saturate monitor refresh between sim ticks: no new display list has
 * been built, so we just re-blit the previously-stored scene FB + redraw
 * the post-render overlay + swap. Cheap (one textured quad + SDL_GL_SwapWindow).
 *
 * Safe to call any number of times between PsyX_EndScene() calls. Does NOT
 * touch the BeginScene/EndScene flag — interleaves freely with scene work. */
extern void PsyX_SwapWindowOnly(void);

/* Explicitly updates emulator input loop */
extern void PsyX_UpdateInput(void);

/* The two halves of PsyX_UpdateInput, exposed so the host can run the event
 * pump and the pad injection as separate steps with its own decode/arbitration
 * in between (and, once the host loop is threaded, on different threads).
 * PsyX_UpdateInput remains the combined convenience form. Note it also gates
 * the injection half on the Alt key so Alt+Enter can't register as Start —
 * callers driving the halves directly own that gate. */
extern void PsyX_Sys_DoPollEvent(void);

/* Route one already-dequeued SDL event to PsyCross's handlers (controller
 * hotplug, window resize, its debug keys/mouse, text input, Alt tracking).
 * For a host that owns the process's single SDL_PollEvent loop: it pumps and
 * calls this per event, so PsyCross keeps its handlers without a second drain
 * on the shared queue. Argument is a const SDL_Event*, typed void* so this
 * header stays SDL-free. */
extern void PsyX_Sys_HandleEvent(void* sdl_event);
extern void PsyX_Pad_InternalPadUpdates(void);

/* SDL_Gamepad* bound to a pad slot, as void* (keeps this header SDL-free).
 * NULL when the slot has no open controller. The handles are owned by the
 * controller-added / -removed handlers, so a host driving the actuators asks
 * for the pointer here rather than keeping its own notion of which pad is
 * player one. */
extern void* PsyX_Pad_GetGamepad(int slot);

/* Actuator-buffer registration hook.
 *
 * Real PSYQ libpad treats PadSetAct as a REGISTRATION: the game hands over the
 * address of its actuator buffer once, and libpad ships that buffer's bytes to
 * the pad every vsync. Installing this hook moves the per-vsync half onto the
 * host and makes PadSetAct the registration it is supposed to be. Left NULL,
 * PadSetAct keeps its historical one-shot behaviour (reading whatever the
 * buffer happens to hold at call time). */
typedef void (*PsyX_Pad_ActuatorRegisterFn)(int slot, unsigned char* table, int len);
extern PsyX_Pad_ActuatorRegisterFn g_PsyX_Pad_ActuatorRegister;

/* Returns keyboard mapping index */
extern int PsyX_LookupKeyboardMapping(const char* str, int default_value);

/* Returns controller mapping index */
extern int PsyX_LookupGameControllerMapping(const char* str, int default_value);

/* Screen size of emulated PSX viewport with widescreen offsets */
extern void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect);

/* Waits for timer */
extern void PsyX_WaitForTimestep(int count);

/* Changes swap interval state */
extern void PsyX_EnableSwapInterval(int enable);

/* Changes swap interval interval interval */
extern void PsyX_SetSwapInterval(int interval);

/* --- VRAM state-export (used by openpete save states) ---------------------- *
 * The 1 MiB CPU-side mirror at `vram[VRAM_WIDTH * VRAM_HEIGHT]` is the
 * source of truth for textures + framebuffer; the GPU texture is uploaded
 * from it lazily via GR_UpdateVRAM(). PsyX_VRAM_Snapshot copies the mirror
 * out, PsyX_VRAM_Restore copies bytes back in and marks the GPU upload
 * dirty so the next frame picks up the restored contents. Pass exactly
 * PsyX_VRAM_GetSize() bytes; smaller/larger calls return non-zero. */
extern unsigned int PsyX_VRAM_GetSize(void);
extern int  PsyX_VRAM_Snapshot(void* dst, unsigned int dst_size);
extern int  PsyX_VRAM_Restore (const void* src, unsigned int src_size);
/* Row-delta variant for high-frequency rollback (per-tick runahead):
 * copies back and dirty-notifies ONLY the rows that differ. Returns 1 if
 * anything was restored, 0 if VRAM already matched, -1 on size mismatch. */
extern int  PsyX_VRAM_RestoreDelta(const void* src, unsigned int src_size);

/* --- VRAM write notification (openpete W2.4 native VRAM model) ------------- *
 * Fired after EVERY write to the CPU-side `vram` mirror with the effective
 * (post-mask/clamp) rect: GR_CopyVRAM (LoadImage/MoveImage + DR_LOAD/DR_MOVE),
 * GR_ClearVRAM (ClearImage), GR_CopyRGBAFramebufferToVRAM (framebuffer
 * copyback), PsyX_VRAM_Restore (savestate, full frame). The callback runs on
 * the caller's thread, synchronously, after the bytes have landed — a consumer
 * may read the written region from the mirror immediately. NULL disables. */
extern void PsyX_SetVRAMDirtyCallback(void (*cb)(int x, int y, int w, int h));

/* --- VRAM op forwarding (openpete W2.4 native op implementations) ---------- *
 * Typed pre-write forwarding of the PSYQ image ops with their RAW arguments
 * (no masking/clamping applied), fired from the LIBGPU adapters — which also
 * catches the OT-embedded DR_LOAD/DR_MOVE transfer packets and the
 * LoadTPage/LoadClut wrappers (both funnel into LoadImage/MoveImage). The
 * consumer implements the PSX masking semantics itself. StoreImage is a read
 * and is not forwarded. NULL entries / NULL ops disable. */
typedef struct PsyX_VRAMOps {
	void (*load) (int x, int y, int w, int h, const unsigned short* src);
	void (*move) (int src_x, int src_y, int w, int h, int dst_x, int dst_y);
	void (*clear)(int x, int y, int w, int h,
	              unsigned char r, unsigned char g, unsigned char b);
} PsyX_VRAMOps;
extern void PsyX_SetVRAMOpCallbacks(const PsyX_VRAMOps* ops);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif