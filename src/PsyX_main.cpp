#include "PsyX_main.h"

#include "PsyX/PsyX_version.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/util/timer.h"

#include "gpu/PsyX_GPU.h"
#include "pad/PsyX_pad.h"

#include "platform.h"
#include "util/crash_handler.h"

#include "psx/libetc.h"
#include "psx/libgte.h"
#include "psx/libgpu.h"
#include "psx/libspu.h"

#include "audio/PsyX_SPUAL.h"
#include "audio/PsyX_XA.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>  /* malloc/free/exit — SDL3 headers no longer leak these */

#include <stdio.h>
#include <SDL3/SDL.h>

#include "PsyX/PsyX_render.h"

#ifdef _WIN32
// pla.h is the Performance Logs and Alerts API; under MSVC's SDK it
// transitively #includes <windows.h>, so the original code reached
// WORD/BACKGROUND_INTENSITY/InitializeCriticalSection through it.
// MinGW's <pla.h> is a minimal stub with no Windows-API drag-in, so we
// pull <windows.h> in explicitly for the console-color spew helpers
// further down in this file.
#include <windows.h>
#include <pla.h>
#endif // _WIN32

#ifdef __EMSCRIPTEN__
int strcasecmp(const char* _l, const char* _r)
{
	const u_char* l = (u_char*)_l, * r = (u_char*)_r;
	for (; *l && *r && (*l == *r || tolower(*l) == tolower(*r)); l++, r++);
	return tolower(*l) - tolower(*r);
}
#elif !defined(_WIN32)
// PsyCross ships its own `include/psx/strings.h` (PSYQ surface) which shadows
// the system `<strings.h>` on the include path, so we forward-declare the
// POSIX symbol instead of relying on the header.
extern "C" int strcasecmp(const char* s1, const char* s2);
#endif

SDL_Window* g_window = NULL;
int g_swapInterval = 1;
int g_enableSwapInterval = 1;
int g_skipSwapInterval = 0;
timerCtx_t g_vblTimer;

int							g_cfg_swapInterval = 0;
PsyXKeyboardMapping			g_cfg_keyboardMapping;
PsyXControllerMapping		g_cfg_controllerMapping;
GameOnTextInputHandler		g_cfg_gameOnTextInput = NULL;

GameDebugKeysHandlerFunc	g_dbg_gameDebugKeys = NULL;
GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse = NULL;
PsyXPostRenderOverlayFunc	g_psyx_postRenderOverlay = NULL;
float						g_psyx_render_aspect_factor = 1.0f;
int							g_dbg_polygonSelected = 0;

enum EPsxCounters
{
	PsxCounter_VBLANK,

	PsxCounter_Num
};

volatile int g_psxSysCounters[PsxCounter_Num];

SDL_Thread* g_intrThread = NULL;

/* Null-GPU mode — see PsyX_public.h. Set by the host before PsyX_Initialise. */
int g_psyx_null_gpu = 0;
/* Core-up latch: PsyX_Shutdown used to key on g_window, but in null-GPU mode
 * no window exists while the intr thread / SPU / SDL still need teardown. */
static int g_psyx_core_up = 0;
SDL_Mutex* g_intrMutex = NULL;
volatile char g_stopIntrThread = 0;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern void(*vsync_callback)(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

extern int	PsyX_Pad_InitSystem();
extern void PsyX_Pad_Event_ControllerRemoved(Sint32 deviceId);
extern void PsyX_Pad_Event_ControllerAdded(Sint32 deviceId);

extern int	GR_InitialisePSX();
extern int	GR_InitialiseRender(char* windowName, int width, int height, int fullscreen, int startHidden);

extern void GR_ResetDevice();
extern void GR_Shutdown();
extern void GR_BeginScene();
extern void GR_EndScene();
extern void GR_UpdateSwapIntervalState(int swapInterval);

int g_vmode = -1;
int g_frameSkip = 0;

#ifdef __EMSCRIPTEN__

int g_emIntrInterval = -1;
int g_intrVMode = MODE_NTSC;
double g_emOldDate = 0;

void emIntrCallback(void* userData)
{
	double timestep = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;

	int newVBlank = (Util_GetHPCTime(&g_vblTimer, 0) / timestep) + g_frameSkip;

	int diff = newVBlank - g_psxSysCounters[PsxCounter_VBLANK];

	while (diff--)
	{
		if (vsync_callback)
			vsync_callback();

		g_psxSysCounters[PsxCounter_VBLANK]++;
	}
}

EM_BOOL emIntrCallback2(double time, void* userData)
{
	emIntrCallback(userData);
	return g_stopIntrThread ? EM_FALSE : EM_TRUE;
}

#endif

int PsyX_Sys_SetVMode(int mode)
{
	int old = g_vmode;
	g_vmode = mode;

#ifdef __EMSCRIPTEN__
	if (old != g_vmode)
	{
		//if(g_emIntrInterval != -1)
		//	emscripten_clear_interval(g_emIntrInterval);
		g_stopIntrThread = 1;

		emscripten_sleep(100);

		g_stopIntrThread = 0;
		emscripten_set_timeout_loop(emIntrCallback2, 1.0, NULL);
	}
#endif

	return old;
}

int PsyX_Sys_GetVBlankCount()
{
	if (g_skipSwapInterval)
	{
		// extra speedup.
		// does not affect `vsync_callback` count
		g_psxSysCounters[PsxCounter_VBLANK] += 1;
		g_frameSkip++;
	}
	
	return g_psxSysCounters[PsxCounter_VBLANK];
}

int intrThreadMain(void* data)
{
	Util_InitHPCTimer(&g_vblTimer);

	while (!g_stopIntrThread)
	{
		// step counters
		{
			const double timestep = g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL;
			const double vblDelta = Util_GetHPCTime(&g_vblTimer, 0);

			if (vblDelta > timestep)
			{
				SDL_LockMutex(g_intrMutex);
				
				if (vsync_callback)
					vsync_callback();
				
				SDL_UnlockMutex(g_intrMutex);
				
				// do vblank events
				g_psxSysCounters[PsxCounter_VBLANK]++;
			
				Util_GetHPCTime(&g_vblTimer, 1);
			}
			
		}
	}

	return 0;
}

static int PsyX_Sys_InitialiseCore()
{
#ifdef __EMSCRIPTEN__
	Util_InitHPCTimer(&g_vblTimer);
#else

	g_intrThread = SDL_CreateThread(intrThreadMain, "psyX_intr", NULL);

	if (NULL == g_intrThread)
	{
		eprinterr("SDL_CreateThread failed: %s\n", SDL_GetError());
		return 0;
	}
	
	g_intrMutex = SDL_CreateMutex();
	if (NULL == g_intrMutex)
	{
		eprinterr("SDL_CreateMutex failed: %s\n", SDL_GetError());
		return 0;
	}
#endif
	return 1;
}

static void PsyX_Sys_InitialiseInput()
{
	g_cfg_keyboardMapping.kc_square = SDL_SCANCODE_X;
	g_cfg_keyboardMapping.kc_circle = SDL_SCANCODE_V;
	g_cfg_keyboardMapping.kc_triangle = SDL_SCANCODE_Z;
	g_cfg_keyboardMapping.kc_cross = SDL_SCANCODE_C;

	g_cfg_keyboardMapping.kc_l1 = SDL_SCANCODE_LSHIFT;
	g_cfg_keyboardMapping.kc_l2 = SDL_SCANCODE_LCTRL;
	g_cfg_keyboardMapping.kc_l3 = SDL_SCANCODE_LEFTBRACKET;

	g_cfg_keyboardMapping.kc_r1 = SDL_SCANCODE_RSHIFT;
	g_cfg_keyboardMapping.kc_r2 = SDL_SCANCODE_RCTRL;
	g_cfg_keyboardMapping.kc_r3 = SDL_SCANCODE_RIGHTBRACKET;

	g_cfg_keyboardMapping.kc_dpad_up = SDL_SCANCODE_UP;
	g_cfg_keyboardMapping.kc_dpad_down = SDL_SCANCODE_DOWN;
	g_cfg_keyboardMapping.kc_dpad_left = SDL_SCANCODE_LEFT;
	g_cfg_keyboardMapping.kc_dpad_right = SDL_SCANCODE_RIGHT;

	g_cfg_keyboardMapping.kc_select = SDL_SCANCODE_SPACE;
	g_cfg_keyboardMapping.kc_start = SDL_SCANCODE_RETURN;

	g_cfg_keyboardMapping.kc_axis_left_up    = SDL_SCANCODE_W;
	g_cfg_keyboardMapping.kc_axis_left_down  = SDL_SCANCODE_S;
	g_cfg_keyboardMapping.kc_axis_left_left  = SDL_SCANCODE_A;
	g_cfg_keyboardMapping.kc_axis_left_right = SDL_SCANCODE_D;

	//----------------
	/* SDL3 renamed the face buttons positionally: A/B/X/Y → SOUTH/EAST/
	 * WEST/NORTH. PSX square=left→WEST, circle=right→EAST, triangle=top→
	 * NORTH, cross=bottom→SOUTH — the physical mapping is unchanged. */
	g_cfg_controllerMapping.gc_square = SDL_GAMEPAD_BUTTON_WEST;
	g_cfg_controllerMapping.gc_circle = SDL_GAMEPAD_BUTTON_EAST;
	g_cfg_controllerMapping.gc_triangle = SDL_GAMEPAD_BUTTON_NORTH;
	g_cfg_controllerMapping.gc_cross = SDL_GAMEPAD_BUTTON_SOUTH;

	g_cfg_controllerMapping.gc_l1 = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
	g_cfg_controllerMapping.gc_l2 = SDL_GAMEPAD_AXIS_LEFT_TRIGGER | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_l3 = SDL_GAMEPAD_BUTTON_LEFT_STICK;

	g_cfg_controllerMapping.gc_r1 = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
	g_cfg_controllerMapping.gc_r2 = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_r3 = SDL_GAMEPAD_BUTTON_RIGHT_STICK;

	g_cfg_controllerMapping.gc_dpad_up = SDL_GAMEPAD_BUTTON_DPAD_UP;
	g_cfg_controllerMapping.gc_dpad_down = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
	g_cfg_controllerMapping.gc_dpad_left = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
	g_cfg_controllerMapping.gc_dpad_right = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;

	g_cfg_controllerMapping.gc_select = SDL_GAMEPAD_BUTTON_BACK;
	g_cfg_controllerMapping.gc_start = SDL_GAMEPAD_BUTTON_START;

	g_cfg_controllerMapping.gc_axis_left_x = SDL_GAMEPAD_AXIS_LEFTX | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_left_y = SDL_GAMEPAD_AXIS_LEFTY | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_right_x = SDL_GAMEPAD_AXIS_RIGHTX | CONTROLLER_MAP_FLAG_AXIS;
	g_cfg_controllerMapping.gc_axis_right_y = SDL_GAMEPAD_AXIS_RIGHTY | CONTROLLER_MAP_FLAG_AXIS;

	PsyX_Pad_InitSystem();
}

#ifdef __GNUC__
#define _stricmp(s1, s2) strcasecmp(s1, s2)
#endif

// Keyboard mapping lookup
int PsyX_LookupKeyboardMapping(const char* str, int default_value)
{
	const char* scancodeName;
	int i;

	if (str)
	{
		if (!_stricmp("NONE", str))
			return SDL_SCANCODE_UNKNOWN;

		for (i = 0; i < SDL_SCANCODE_COUNT; i++)
		{
			scancodeName = SDL_GetScancodeName((SDL_Scancode)i);

			if (strlen(scancodeName) && !_stricmp(scancodeName, str))
			{
				return i;
			}
		}
	}

	return default_value;
}

// Game controller mapping lookup
// Available controller binds(refer to SDL2 game controller)
//
// Axes:
//	leftx lefty
//	rightx righty
//	lefttrigger righttrigger
//
// NOTE: adding `-` before axis names makes it inverse, so `-leftx` inverse left stick X axis
//
// Buttons:
// 	a, b, x, y
// 	back guide start
// 	leftstick rightstick
// 	leftshoulder rightshoulder
// 	dpup dpdown dpleft dpright

int PsyX_LookupGameControllerMapping(const char* str, int default_value)
{
	const char* axisStr;
	const char* buttonOrAxisName;
	int i, axisFlags;

	if (str)
	{
		axisFlags = CONTROLLER_MAP_FLAG_AXIS;
		axisStr = str;

		if (*axisStr == '-')
		{
			axisFlags |= CONTROLLER_MAP_FLAG_INVERSE;
			axisStr++;
		}

		if (!_stricmp("NONE", str))
			return SDL_GAMEPAD_BUTTON_INVALID;

		// check buttons
		for (i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
		{
			buttonOrAxisName = SDL_GetGamepadStringForButton((SDL_GamepadButton)i);

			if (buttonOrAxisName && strlen(buttonOrAxisName) && !_stricmp(buttonOrAxisName, str))
			{
				return i;
			}
		}

		// Check axes
		for (i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
		{
			buttonOrAxisName = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)i);

			if (buttonOrAxisName && strlen(buttonOrAxisName) && !_stricmp(buttonOrAxisName, axisStr))
			{
				return i | axisFlags;
			}
		}
	}

	return default_value;
}

char* g_appNameStr = NULL;

void PsyX_GetWindowName(char* buffer)
{
#ifdef _DEBUG
	sprintf(buffer, "%s | Debug", g_appNameStr);
#else
	sprintf(buffer, "%s", g_appNameStr);
#endif
}

FILE* g_logStream = NULL;

// intialise logging
void PsyX_Log_Initialise()
{
	/* openpete: the log file is opt-in. PsyCross otherwise drops a
	 * "<appname>.log" (e.g. "Spyro the Dragon.log") in the working
	 * directory on every launch; the host doesn't need it and it clutters
	 * the cwd. Set OPENPETE_PSYX_LOG=1 to get it back for debugging.
	 * Console (stdout) logging is unaffected — PrintMessageToOutput() only
	 * writes the file when g_logStream != NULL. */
	if (!getenv("OPENPETE_PSYX_LOG"))
		return;

	char appLogFilename[128];
	sprintf(appLogFilename, "%s.log", g_appNameStr);

	g_logStream = fopen(appLogFilename, "wb");

	if (!g_logStream)
		eprinterr("Error - cannot create log file '%s'\n", appLogFilename);
}

void PsyX_Log_Finalise()
{
	PsyX_Log_Warning("---- LOG CLOSED ----\n");

	if (g_logStream)
		fclose(g_logStream);

	g_logStream = NULL;
}

void PsyX_Log_Flush()
{
	if (g_logStream)
		fflush(g_logStream);
}

// spew types
typedef enum
{
	SPEW_NORM,
	SPEW_INFO,
	SPEW_WARNING,
	SPEW_ERROR,
	SPEW_SUCCESS,
}SpewType_t;

#ifdef _WIN32
static unsigned short g_InitialColor = 0xFFFF;
static unsigned short g_LastColor = 0xFFFF;
static unsigned short g_BadColor = 0xFFFF;
static WORD g_BackgroundFlags = 0xFFFF;
CRITICAL_SECTION g_SpewCS;
char g_bSpewCSInitted = 0;

static void Spew_GetInitialColors()
{
	// Get the old background attributes.
	CONSOLE_SCREEN_BUFFER_INFO oldInfo;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &oldInfo);
	g_InitialColor = g_LastColor = oldInfo.wAttributes & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
	g_BackgroundFlags = oldInfo.wAttributes & (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY);

	g_BadColor = 0;
	if (g_BackgroundFlags & BACKGROUND_RED)
		g_BadColor |= FOREGROUND_RED;
	if (g_BackgroundFlags & BACKGROUND_GREEN)
		g_BadColor |= FOREGROUND_GREEN;
	if (g_BackgroundFlags & BACKGROUND_BLUE)
		g_BadColor |= FOREGROUND_BLUE;
	if (g_BackgroundFlags & BACKGROUND_INTENSITY)
		g_BadColor |= FOREGROUND_INTENSITY;
}

static WORD Spew_SetConsoleTextColor(int red, int green, int blue, int intensity)
{
	WORD ret = g_LastColor;

	g_LastColor = 0;
	if (red)	g_LastColor |= FOREGROUND_RED;
	if (green) g_LastColor |= FOREGROUND_GREEN;
	if (blue)  g_LastColor |= FOREGROUND_BLUE;
	if (intensity) g_LastColor |= FOREGROUND_INTENSITY;

	// Just use the initial color if there's a match...
	if (g_LastColor == g_BadColor)
		g_LastColor = g_InitialColor;

	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), g_LastColor | g_BackgroundFlags);
	return ret;
}

static void Spew_RestoreConsoleTextColor(WORD color)
{
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color | g_BackgroundFlags);
	g_LastColor = color;
}

void Spew_ConDebugSpew(SpewType_t type, char* text)
{
	// Hopefully two threads won't call this simultaneously right at the start!
	if (!g_bSpewCSInitted)
	{
		Spew_GetInitialColors();
		InitializeCriticalSection(&g_SpewCS);
		g_bSpewCSInitted = 1;
	}

	WORD old;
	EnterCriticalSection(&g_SpewCS);
	{
		if (type == SPEW_NORM)
		{
			old = Spew_SetConsoleTextColor(1, 1, 1, 0);
		}
		else if (type == SPEW_WARNING)
		{
			old = Spew_SetConsoleTextColor(1, 1, 0, 1);
		}
		else if (type == SPEW_SUCCESS)
		{
			old = Spew_SetConsoleTextColor(0, 1, 0, 1);
		}
		else if (type == SPEW_ERROR)
		{
			old = Spew_SetConsoleTextColor(1, 0, 0, 1);
		}
		else if (type == SPEW_INFO)
		{
			old = Spew_SetConsoleTextColor(0, 1, 1, 1);
		}
		else
		{
			old = Spew_SetConsoleTextColor(1, 1, 1, 1);
		}

		OutputDebugStringA(text);
		printf("%s", text);

		Spew_RestoreConsoleTextColor(old);
	}
	LeaveCriticalSection(&g_SpewCS);
}
#endif

void PrintMessageToOutput(SpewType_t spewtype, char const* pMsgFormat, va_list args)
{
	static char pTempBuffer[4096];
	int len = 0;
	vsprintf(&pTempBuffer[len], pMsgFormat, args);

#ifdef WIN32
	Spew_ConDebugSpew(spewtype, pTempBuffer);
#elif defined(__EMSCRIPTEN__)
	if (spewtype == SPEW_INFO)
	{
		EM_ASM({
			console.info(UTF8ToString($0));
		}, pTempBuffer);
	}
	else if (spewtype == SPEW_WARNING)
	{
		EM_ASM({
			console.warn(UTF8ToString($0));
		}, pTempBuffer);
	}
	else if (spewtype == SPEW_ERROR)
	{
		EM_ASM({
			console.error(UTF8ToString($0));
		}, pTempBuffer);
	}
	else
	{
		EM_ASM({
			console.log(UTF8ToString($0));
		}, pTempBuffer);
	}
#else
	printf("%s", pTempBuffer);
#endif

	if(g_logStream)
		fprintf(g_logStream, "%s", pTempBuffer);
}

void PsyX_Log(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_NORM, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Info(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_INFO, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Warning(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_WARNING, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Error(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_ERROR, fmt, argptr);
	va_end(argptr);
}

void PsyX_Log_Success(const char* fmt, ...)
{
	va_list		argptr;

	va_start(argptr, fmt);
	PrintMessageToOutput(SPEW_SUCCESS, fmt, argptr);
	va_end(argptr);
}


void PsyX_Initialise(char* appName, int width, int height, int fullscreen, int startHidden)
{
	char windowNameStr[128];

	g_appNameStr = appName;

	InstallExceptionHandler();

	PsyX_Log_Initialise();
	PsyX_GetWindowName(windowNameStr);

#if defined(_WIN32) && defined(_DEBUG)
	if (AllocConsole())
	{
		freopen("CONOUT$", "w", stdout);
		SetConsoleTitleA(windowNameStr);
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
	}
#endif

	eprintinfo("Initialising Psy-X %d.%d\n", PSYX_MAJOR_VERSION, PSYX_MINOR_VERSION);
	eprintinfo("Build date: %s:%s\n", PSYX_COMPILE_DATE, PSYX_COMPILE_TIME);

	/* SDL3 removed SDL_HINT_EMSCRIPTEN_ASYNCIFY. */

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		eprinterr("Failed to initialise SDL\n");
		PsyX_Shutdown();
		return;
	}
	g_psyx_core_up = 1;

	if (g_psyx_null_gpu)
	{
		/* Library-only mode: no window, no GL, no PSX GPU state. The CPU-side
		 * vram array + libgpu thunk semantics stay live; every GL-touching GR
		 * entry point early-returns on the flag. */
		eprintinfo("Psy-X null-GPU mode: window/GL/PSX-GPU init skipped\n");
	}
	else if (!GR_InitialiseRender(windowNameStr, width, height, fullscreen, startHidden))
	{
		eprinterr("Failed to Intialise Window\n");
		PsyX_Shutdown();
		return;
	}

	if (!PsyX_Sys_InitialiseCore())
	{
		eprinterr("Failed to Intialise Psy-X Core.\n");
		PsyX_Shutdown();
		return;
	}

	if (!g_psyx_null_gpu && !GR_InitialisePSX())
	{
		eprinterr("Failed to Intialise PSX.\n");
		PsyX_Shutdown();
		return;
	}

	PsyX_Sys_InitialiseInput();

	// set shutdown function (PSX apps usualy don't exit)
	atexit(PsyX_Shutdown);

	// disable cursor visibility
	if (!g_psyx_null_gpu)
		SDL_HideCursor();
}

void PsyX_GetScreenSize(int* screenWidth, int* screenHeight)
{
	SDL_GetWindowSize(g_window, screenWidth, screenHeight);
}

void PsyX_SetCursorPosition(int x, int y)
{
	SDL_WarpMouseInWindow(g_window, x, y);
}

void PsyX_Sys_DoDebugKeys(int nKey, char down); // forward decl
void PsyX_Sys_DoDebugMouseMotion(int x, int y);

void PsyX_Exit();

int g_activeKeyboardControllers = 0x1;
int g_altKeyState = 0;

void PsyX_Sys_DoPollEvent()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		/* SDL3: window events are top-level types (no SDL_WINDOWEVENT
		 * wrapper); the keysym struct is flattened onto event.key. */
		switch (event.type)
		{
			case SDL_EVENT_GAMEPAD_ADDED:
				PsyX_Pad_Event_ControllerAdded(event.gdevice.which);
				break;
			case SDL_EVENT_GAMEPAD_REMOVED:
				PsyX_Pad_Event_ControllerRemoved(event.gdevice.which);
				break;
			case SDL_EVENT_QUIT:
				PsyX_Exit();
				break;
			case SDL_EVENT_WINDOW_RESIZED:
				g_windowWidth = event.window.data1;
				g_windowHeight = event.window.data2;
				GR_ResetDevice();
				break;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				PsyX_Exit();
				break;
			case SDL_EVENT_MOUSE_MOTION:

				PsyX_Sys_DoDebugMouseMotion((int)event.motion.x, (int)event.motion.y);
				break;
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
			{
				int nKey = event.key.scancode;

				if (nKey == SDL_SCANCODE_LALT || nKey == SDL_SCANCODE_RALT)
				{
					g_altKeyState = (event.type == SDL_EVENT_KEY_DOWN);
				}
				else if (nKey == SDL_SCANCODE_RETURN)
				{
					/* Alt+Enter fullscreen now lives in the native renderer
					 * (run.c -> openpete_native_toggle_fullscreen). g_altKeyState
					 * still gates pad input (see PsyX_UpdateInput) so Enter isn't
					 * read as Start while Alt is held. Swallow Enter here so it
					 * isn't processed as a debug key. */
					break;
				}

				// lshift/right shift
				if (nKey == SDL_SCANCODE_RSHIFT)
					nKey = SDL_SCANCODE_LSHIFT;
				else if (nKey == SDL_SCANCODE_RCTRL)
					nKey = SDL_SCANCODE_LCTRL;
				else if (nKey == SDL_SCANCODE_RALT)
					nKey = SDL_SCANCODE_LALT;

				if (g_cfg_gameOnTextInput && nKey == SDL_SCANCODE_BACKSPACE && event.type == SDL_EVENT_KEY_DOWN)
				{
					(g_cfg_gameOnTextInput)(NULL);
				}

				PsyX_Sys_DoDebugKeys(nKey, (event.type == SDL_EVENT_KEY_UP) ? 0 : 1);
				break;
			}
			case SDL_EVENT_TEXT_INPUT:
			{
				if(g_cfg_gameOnTextInput)
					(g_cfg_gameOnTextInput)(event.text.text);
				break;
			}			
		}
	}
}

char begin_scene_flag = 0;

/* Renderer-foundation Phase 1 (draw-call matcher) — finalize hook. */
/* Renderer-foundation Phase 2 (substitute) — eager dual-scan hook. */

char PsyX_BeginScene()
{
	if (g_psyx_null_gpu)
		return 0;   /* library-only: no GL scene ever begins */

	PsyX_Sys_DoPollEvent();

	if (begin_scene_flag)
		return 0;

	assert(!begin_scene_flag);

	{
		int swapInterval = (g_cfg_swapInterval && g_enableSwapInterval && !g_skipSwapInterval) ? g_swapInterval : 0;

		// Maximum is (ScreenRefreshRate / 2). 
		// If our screen refresh rate is lower than our PSX vmode refresh rate, 
		// we reducing swap interval to maintain the framerate.
		// Example:
		//		target 60fps, 50hz screen = no interval (tearing)
		//		target 30fps, 50hz screen = 60hz interval (less tearing)
		//		target 30fps, 60hz screen = 30hz interval (no tearing)
		/* SDL3: query the current mode of the display the window is on.
		 * SDL_GetWindowDisplayMode is gone; SDL_GetWindowFullscreenMode
		 * would return NULL for a windowed app, so go via the display. */
		const SDL_DisplayMode* curMode =
			SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
		if (curMode)
		{
			const int mode_frequency = g_vmode == MODE_NTSC ? VBLANK_FREQUENCY_NTSC : VBLANK_FREQUENCY_PAL;
			if (curMode->refresh_rate < mode_frequency)
				swapInterval--;
		}

		if (swapInterval < 0)
			swapInterval = 0;
		
		GR_UpdateSwapIntervalState(swapInterval);
	}

	GR_BeginScene();

	if (activeDrawEnv.isbg)
	{
		const RECT16 clipenv = activeDrawEnv.clip;
		const u_char r = activeDrawEnv.r0;
		const u_char g = activeDrawEnv.g0;
		const u_char b = activeDrawEnv.b0;

		// TODO: clear all affected backbuffers
		//GR_ClearVRAM(clipenv.x, clipenv.y, clipenv.w, clipenv.h, r, g, b);
		GR_Clear(clipenv.x, clipenv.y, clipenv.w, clipenv.h, r, g, b);
	}

	begin_scene_flag = 1;

	PsyX_Log_Flush();

	return 1;
}

uint PsyX_CalcFPS();

void PsyX_EndScene()
{
	if (!begin_scene_flag)
		return;

	assert(begin_scene_flag);
	begin_scene_flag = 0;


#if USE_PGXP
	PGXP_ClearCache();
#endif

	GR_EndScene();
	
	GR_StoreFrameBuffer(activeDispEnv.disp.x, activeDispEnv.disp.y, activeDispEnv.disp.w, activeDispEnv.disp.h);

	/* Crop the PSX overscan bands (DRAWENV.clip shorter than DISPENV.disp)
	 * onto the window's default framebuffer before the overlay draws on top. */
	GR_PresentCroppedToWindow();

	if (g_psyx_postRenderOverlay)
		g_psyx_postRenderOverlay();

	GR_SwapWindow();

	/* fps-unlock Phase 2 probe: per-frame summary of GTE + AddPrim
	 * counters maintained by gte.cpp and LIBGPU.C. Off in normal
	 * builds (gated by OPENPETE_RENDER_PROBE=1). */
	{
		extern int      g_openpete_render_probe_active;
		extern unsigned g_openpete_render_probe_count_rtps;
		extern unsigned g_openpete_render_probe_count_rtpt;
		extern unsigned g_openpete_render_probe_count_mvmva;
		extern unsigned g_openpete_render_probe_count_other;
		extern unsigned g_openpete_render_probe_count_matrix_changes;
		extern unsigned g_openpete_render_probe_count_addprim;
		extern unsigned g_openpete_render_probe_count_addprim_grp;
		extern unsigned g_openpete_render_probe_count_ot_walk;
		/* Renderer-foundation Phase 0 counters (world-space cache). */
		/* Renderer-foundation Phase 1 (matcher) counters. */
		extern unsigned openpete_frame;
		if (g_openpete_render_probe_active == 1) {
			printf("[render-probe] f%u SUMMARY rtps=%u rtpt=%u mvmva=%u other=%u "
			       "matrix-changes=%u addPrim_fn=%u addPrims_fn=%u "
			       "ot_walked=%u "
			       "(verts=%u, prims_per_matrix_batch=%.1f)\n",
			       openpete_frame,
			       g_openpete_render_probe_count_rtps,
			       g_openpete_render_probe_count_rtpt,
			       g_openpete_render_probe_count_mvmva,
			       g_openpete_render_probe_count_other,
			       g_openpete_render_probe_count_matrix_changes,
			       g_openpete_render_probe_count_addprim,
			       g_openpete_render_probe_count_addprim_grp,
			       g_openpete_render_probe_count_ot_walk,
			       g_openpete_render_probe_count_rtps + 3 * g_openpete_render_probe_count_rtpt,
			       g_openpete_render_probe_count_matrix_changes
			           ? (double)g_openpete_render_probe_count_addprim /
			             (double)g_openpete_render_probe_count_matrix_changes
			           : 0.0);
			fflush(stdout);
			g_openpete_render_probe_count_rtps  = 0;
			g_openpete_render_probe_count_rtpt  = 0;
			g_openpete_render_probe_count_mvmva = 0;
			g_openpete_render_probe_count_other = 0;
			g_openpete_render_probe_count_matrix_changes = 0;
			g_openpete_render_probe_count_addprim     = 0;
			g_openpete_render_probe_count_addprim_grp = 0;
			g_openpete_render_probe_count_ot_walk     = 0;
		}
	}
}

void PsyX_SwapWindowOnly()
{
	/* Re-present the last stored framebuffer without re-running GR_EndScene
	 * or GR_StoreFrameBuffer. The previous PsyX_EndScene captured the scene
	 * to a texture; GR_PresentCroppedToWindow just reads it. The overlay
	 * hook runs again so FPS / debug overlays update at the swap cadence
	 * (otherwise they'd stutter at sim cadence inside an unlocked render
	 * loop). Does not touch begin_scene_flag — calling sites are render-only
	 * iterations that bracket a paused game fiber. */

	GR_PresentCroppedToWindow();

	if (g_psyx_postRenderOverlay)
		g_psyx_postRenderOverlay();

	GR_SwapWindow();
}

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
void PsyX_TakeScreenshot()
{
	u_char* pixels = (u_char*)malloc(g_windowWidth * g_windowHeight * 4);
	
#if defined(RENDERER_OGL)
	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
#elif defined(RENDERER_OGLES)
	glReadPixels(0, 0, g_windowWidth, g_windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels);	// FIXME: is that correct format?
#endif

#if defined(RENDERER_OGL)
	const SDL_PixelFormat shotFmt = SDL_PIXELFORMAT_ARGB8888;  /* GL_BGRA bytes */
#else
	const SDL_PixelFormat shotFmt = SDL_PIXELFORMAT_ABGR8888;  /* GL_RGBA bytes */
#endif
	SDL_Surface* surface = SDL_CreateSurfaceFrom(g_windowWidth, g_windowHeight, shotFmt, pixels, g_windowWidth * 4);

	SDL_SaveBMP(surface, "SCREENSHOT.BMP");
	SDL_DestroySurface(surface);

	free(pixels);
}
#endif

void PsyX_Sys_DoDebugMouseMotion(int x, int y)
{
	if (g_dbg_gameDebugMouse)
		g_dbg_gameDebugMouse(x, y);
}

void PsyX_Sys_DoDebugKeys(int nKey, char down)
{
	if (g_dbg_gameDebugKeys)
		g_dbg_gameDebugKeys(nKey, down);

#if 1 //def _DEBUG
	if (nKey == SDL_SCANCODE_BACKSPACE)
	{
		if (down)
			g_skipSwapInterval = 1;
		else
			g_skipSwapInterval = 0;
	}
#endif

	if (!down)
	{
		switch (nKey)
		{
#ifdef _DEBUG
		case SDL_SCANCODE_F1:
			g_dbg_wireframeMode ^= 1;
			eprintwarn("wireframe mode: %d\n", g_dbg_wireframeMode);
			break;

		case SDL_SCANCODE_F2:
			g_dbg_texturelessMode ^= 1;
			eprintwarn("textureless mode: %d\n", g_dbg_texturelessMode);
			break;
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_DOWN:
			if (g_dbg_emulatorPaused)
			{
				g_dbg_polygonSelected += (nKey == SDL_SCANCODE_UP) ? 3 : -3;
			}
			break;
		case SDL_SCANCODE_F10:
			eprintwarn("saving VRAM.TGA\n");
			GR_SaveVRAM("VRAM.TGA", 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 1);
			break;
#endif
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
		case SDL_SCANCODE_F12:
			eprintwarn("Saving screenshot...\n");
			PsyX_TakeScreenshot();
			break;
#endif
		case SDL_SCANCODE_F3:
			g_cfg_bilinearFiltering ^= 1;
			eprintwarn("filtering mode: %d\n", g_cfg_bilinearFiltering);
			break;
		case SDL_SCANCODE_F4:

			g_activeKeyboardControllers++;
			g_activeKeyboardControllers = g_activeKeyboardControllers % 4;

			if (g_activeKeyboardControllers == 0)
				g_activeKeyboardControllers++;

			eprintwarn("Active keyboard controller: %d\n", g_activeKeyboardControllers);
			break;
		}
	}
}

void PsyX_UpdateInput()
{
	// also poll events here
	PsyX_Sys_DoPollEvent();

	if(!g_altKeyState)
		PsyX_Pad_InternalPadUpdates();
}

uint PsyX_CalcFPS()
{
#define FPS_INTERVAL 1.0

	static Uint64 lastTime = 0;  /* SDL3: SDL_GetTicks is 64-bit */
	static unsigned int currentFps = 0;
	static unsigned int passedFrames = 0;

	lastTime = SDL_GetTicks();

	passedFrames++;
	if (lastTime < SDL_GetTicks() - FPS_INTERVAL * 1000)
	{
		lastTime = SDL_GetTicks();
		currentFps = passedFrames;
		passedFrames = 0;
	}

	return currentFps;
}

void PsyX_SetSwapInterval(int interval)
{
	g_swapInterval = interval;
}

void PsyX_EnableSwapInterval(int enable)
{
	g_enableSwapInterval = enable;
}

void PsyX_WaitForTimestep(int count)
{
#if 0 // defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	glFinish(); // best time to complete GPU drawing
#endif

	// wait for vblank
	if (!g_skipSwapInterval)
	{	
		static int swapLastVbl = 0;

		int vbl;
		do
		{
#ifdef __EMSCRIPTEN__
			emscripten_sleep(0);
#endif
			vbl = PsyX_Sys_GetVBlankCount();
		}
		while (vbl - swapLastVbl < count);

		swapLastVbl = PsyX_Sys_GetVBlankCount();
	}
}

void PsyX_Exit()
{
	exit(0);
}

void PsyX_Shutdown()
{
	if (!g_psyx_core_up)
		return;
	g_psyx_core_up = 0;

	// quit vblank thread
	if (g_intrThread)
	{
		g_stopIntrThread = 1;

		int returnValue;
		SDL_WaitThread(g_intrThread, &returnValue);

		SDL_DestroyMutex(g_intrMutex);
	}

	if (g_window)
	{
		SDL_DestroyWindow(g_window);
		g_window = NULL;
	}

	if (!g_psyx_null_gpu)
		GR_Shutdown();
	SDL_QuitSubSystem(SDL_INIT_GAMEPAD);

	// Tear down audio before SDL_Quit so OpenAL's PulseAudio backend gets a
	// clean pa_stream_disconnect / pa_context_disconnect. Without this the
	// process exits with the device still open; libopenal's atexit handler
	// only logs ("AL lib: (EE) alc_cleanup: 1 device not closed") and the
	// WSLg PulseAudio server leaks shm-pool registrations across launches,
	// eventually returning AL_OUT_OF_MEMORY to fresh alcOpenDevice calls.
	PsyX_XA_Shutdown();
	PsyX_SPUAL_ShutdownSound();

	SDL_Quit();

	UnInstallExceptionHandler();

	PsyX_Log_Finalise();
}
