#ifndef PSYX_PAD_H
#define PSYX_PAD_H

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern void		PsyX_Pad_InitPad(int slot, u_char* padData);
extern void		PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len);
extern int		PsyX_Pad_GetStatus(int mtap, int slot);
extern void		PsyX_Pad_InternalPadUpdates();

/* SDL_Gamepad* for a slot, as void* (keeps this header SDL-free). NULL when
 * the slot has no open controller. */
extern void*	PsyX_Pad_GetGamepad(int slot);

/* The actuator-registration hook (g_PsyX_Pad_ActuatorRegister) and
 * PsyX_Pad_GetGamepad are declared in PsyX/PsyX_public.h — hosts install and
 * call them, so the public header is their home. Declared once there rather
 * than in both, so a TU including both does not hit a duplicate typedef. */

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif // PSYX_PAD_H