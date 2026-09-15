#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "../pad/PsyX_pad.h"

#include "PsyX/PsyX_public.h"

#include <string.h>

int g_padCommEnable = 0;

void PadInitDirect(unsigned char* pad1, unsigned char* pad2)
{
	PsyX_Pad_InitPad(0, pad1);
	PsyX_Pad_InitPad(1, pad2);
}

void PadInitMtap(unsigned char* slot1, unsigned char* slot2)
{
	PSYX_UNIMPLEMENTED();
	PsyX_Pad_InitPad(0, slot1);
	PsyX_Pad_InitPad(1, slot2);
}

void PadInitGun(unsigned char* unk00, int unk01)
{
	PSYX_UNIMPLEMENTED();
}

int PadChkVsync()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void PadStartCom()
{
	g_padCommEnable = 1;
}

void PadStopCom()
{
	g_padCommEnable = 0;
}

unsigned int PadEnableCom(unsigned int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void PadEnableGun(unsigned char unk00)
{
	PSYX_UNIMPLEMENTED();
}

void PadRemoveGun()
{
	PSYX_UNIMPLEMENTED();
}

int PadGetState(int port)
{
	int mtap, slot;

	mtap = port & 3;
	slot = (mtap * 2) + (port >> 4) & 1;

	return PsyX_Pad_GetStatus(mtap, slot) ? PadStateStable : PadStateDiscon;
}

/* The connected pad is a DualShock (SCPH-1200) in analog mode: its mode-id
 * table is {4 digital, 7 analog}, its extended id 7, and it carries two
 * actuators, described by the four bytes its configuration command 0x46
 * answers per actuator (function, sub-function, parameter size, current):
 * 01 02 00 0A for the small motor and 01 01 01 14 for the large one,
 * which libpad hands out through PadInfoAct's terms. An empty slot answers
 * 0. */
static int PadSlotOf(int port)
{
	return ((port >> 4) & 1);
}

int PadInfoMode(int port, int term, int offs)
{
	if (!PsyX_Pad_GetStatus(port & 3, PadSlotOf(port)))
		return 0;
	switch (term)
	{
	case InfoModeCurID:     return 7;
	case InfoModeCurExID:   return 7;
	case InfoModeCurExOffs: return 1;
	case InfoModeIdTable:   return offs == 0 ? 4 : (offs == 1 ? 7 : 0);
	default:                return 0;
	}
}

int PadInfoAct(int port, int acno, int term)
{
	if (!PsyX_Pad_GetStatus(port & 3, PadSlotOf(port)))
		return 0;
	if (acno < 0)
		return 2;
	if (acno > 1)
		return 0;
	switch (term)
	{
	case InfoActFunc: return 1;
	case InfoActSub:  return acno == 0 ? 2 : 1;
	case InfoActSize: return acno == 0 ? 0 : 1;
	case InfoActCurr: return acno == 0 ? 0x0A : 0x14;
	default:          return 0;
	}
}

int PadInfoComb(int unk00, int unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int PadSetActAlign(int port, unsigned char* table)
{
	return 1;
}

int PadSetMainMode(int socket, int offs, int lock)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

/* Real PSYQ libpad treats this as a REGISTRATION, not a command: the game
 * hands over the address of its actuator buffer once, and libpad reads that
 * buffer and ships its bytes to the pad every vsync from then on. Games rely
 * on it — Spyro calls PadSetAct exactly once, at boot, and afterwards only
 * writes new bytes into the buffer it registered.
 *
 * Reading the bytes here instead (the historical implementation, still the
 * fallback below) therefore captures whatever the buffer held at registration
 * time, which for a game that registers before it has anything to say is
 * {0, 0} — one zero-amplitude command, and silence forever after.
 *
 * With a host hook installed, ownership of the per-vsync half moves to the
 * host and this becomes the registration libpad always was. */
void PadSetAct(int port, unsigned char* table, int len)
{
	int mtap, slot;

	mtap = port & 3;
	slot = (mtap * 2) + (port >> 4) & 1;

	if (g_PsyX_Pad_ActuatorRegister)
	{
		g_PsyX_Pad_ActuatorRegister(slot, table, len);
		return;
	}

	PsyX_Pad_Vibrate(mtap, slot, table, len);
}