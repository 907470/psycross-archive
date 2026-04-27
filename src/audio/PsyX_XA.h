#ifndef PSYX_XA_H
#define PSYX_XA_H

/* XA-ADPCM streaming for Mode 2 Form 2 sectors.
 * Owned by PsyCross's audio backend; driven by libcd hooks
 * that detect Spyro-style "CdlSetmode CdlModeRT|SF, CdlSetfilter,
 * CdlReadS" sequences and forward the start sector + filter here. */

#ifdef __cplusplus
extern "C" {
#endif

void PsyX_XA_Init(void);
void PsyX_XA_Shutdown(void);

/* Begin streaming starting at absolute LBA `sector`.
 * file/chan filter: only matching XA audio sectors are decoded. */
void PsyX_XA_Start(int sector, int file, int chan);
void PsyX_XA_Stop(void);
int  PsyX_XA_IsActive(void);

/* Drive the decoder: reads up to maxSectors from the disc image,
 * decodes XA audio sectors, queues PCM into the OpenAL source.
 * Safe to call every frame from CdSync. */
void PsyX_XA_Pump(int maxSectors);

/* Current playhead (absolute LBA, in sectors). 0 if inactive. */
int  PsyX_XA_GetCurrentSector(void);

/* Linear gain 0.0 .. 1.0 (or higher; OpenAL clamps). */
void PsyX_XA_SetVolume(float linear_gain);

#ifdef __cplusplus
}
#endif

#endif
