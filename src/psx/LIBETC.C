#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "PsyX/PsyX_public.h"

#include <SDL_timer.h>
#include <stdint.h>

#ifdef DEBUG
char scratchData[4096 + 8];
char* _scratchData = scratchData + 4;
#else
char scratchData[4096];
char* _scratchData = scratchData;
#endif

void(*vsync_callback)(void) = NULL;

int StopCallback(void)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int ResetCallback(void)
{
	/* PSYQ returns the previous callback as an int — sign-correct on PSX's
	 * 32-bit MIPS, lossy on 64-bit hosts. Callers only use it as an opaque
	 * "different from last time" sentinel, so the truncation is OK. Cast
	 * through uintptr_t to silence the -fpermissive precision warning. */
	int old = (int)(uintptr_t)vsync_callback;
	vsync_callback = NULL;
	return old;
}

int VSync(int mode)
{
	if (mode < 0)
	{
		// don't wait but still return vblank count
		return PsyX_Sys_GetVBlankCount();
	}

	if (mode == 0)
	{
		PsyX_WaitForTimestep(1);
	}
	else if (mode > 0)
	{
		// FIXME: wait many times?
	}

	return PsyX_Sys_GetVBlankCount();
}

int VSyncCallback(void(*f)(void))
{
	int old = (int)(uintptr_t)vsync_callback;  /* see ResetCallback */
	vsync_callback = f;
	return old;
}

int SetVideoMode(int mode)
{
#ifdef DEBUG
	// debug marks for overflow cheks
	*(uint*)&scratchData[0] = 0xdeadb33f;
	*(uint*)&scratchData[4096 + 4] = 0xdeadb33f;
#endif
	
	return PsyX_Sys_SetVMode(mode);
}

int GetVideoMode()
{
	return g_vmode;
}

void PadInit(int mode)
{
	PSYX_UNIMPLEMENTED();

	// TODO: call PadInitDirect
}

u_int PadRead(int id)
{
	PSYX_UNIMPLEMENTED();
	
	// TODO: return pad data as u_int
	return 0;
}

void PadStop(void)
{
	PSYX_UNIMPLEMENTED();

	// TODO: stop pad reads
}