#ifndef PGXP_DEFS_H
#define PGXP_DEFS_H

#include "PsyX/PsyX_config.h"
#include "psx/types.h"  /* ushort, uint, u_short — was relied on transitively
                         * before; the openpete bridge includes pgxp_defs.h
                         * from a TU (psxrecomp gte.cpp) that doesn't pull
                         * types.h via any other PsyCross header, so make
                         * pgxp_defs.h self-contained. */

#if USE_PGXP
#include "PsyX/common/half_float.h"  /* still needed by the (unused) DVECTORF type */

/* VERTTYPE is `short` unconditionally. Upstream PsyCross declared it as
 * `half` in C++ so GTE-emitted sub-pixel floats could be packed into the
 * primitive's vertex slot — but openpete's recompiled MIPS code writes
 * int16 PSX-screen-space coords directly into primitives via the
 * psyq_thunks.cpp DrawOTag bridge. Reinterpreting those bytes as IEEE
 * half collapsed all geometry to the origin (commit 42decab). The PGXP
 * cache hash (PGXP_LOOKUP_VALUE) is keyed on the 16-bit pattern of the
 * screen coord, which matches between the GTE emit side (int16 SX2/SY2
 * after Lm_G clamp) and the GPU consumer side (int16 read out of the
 * PSX-side primitive) — so short works on both ends. Sub-pixel
 * precision rides in the parallel float px/py/pz fields. */
#define PGXP_LOOKUP_VALUE(x, y) (*(u_short*)&(x) | (*(u_short*)&(y) << 16))

typedef short VERTTYPE;
#define _HF(x) (x)

typedef struct
{
	float px, py, pz;	// 32 bit values — sub-pixel precision carrier
	VERTTYPE x, y, z;	// 16 bit values for lookup hash (must match PSX-side int16 SXY)
} PGXPVector3D;

typedef struct
{
	uint lookup;
	float px, py, pz;
	float scr_h, ofx, ofy;
} PGXPVData;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern PGXPVector3D g_FP_SXYZ0; // direct access PGXP without table lookup
extern PGXPVector3D g_FP_SXYZ1;
extern PGXPVector3D g_FP_SXYZ2;

/* clears PGXP vertex buffer */
void	PGXP_ClearCache();

/* emits new PGXP vertex */
ushort	PGXP_EmitCacheData(PGXPVData* newData);

/* sets Z offset (works like Z bias) */
void	PGXP_SetZOffsetScale(float offset, float scale);

/* searches for vertex with given lookup value */
int		PGXP_GetCacheData(PGXPVData* out, uint lookup, ushort indexhint /* = 0xFFFF */);

/* used by primitive setup */
ushort	PGXP_GetIndex(int checkTransform);

/* Content-keyed lookup for the openpete bridge. Spyro's batched flow
 * (GTE-then-DrawOTag rather than interleaved emit-and-consume) means
 * the consumer's "current PGXP index" is far past the emit indices of
 * the primitive being processed. The bridge consults this map at host
 * P_TAG synthesis time: given a primitive's vertex packed (sx,sy)
 * key, returns the most-recent PGXP cache index emitted for that key,
 * or 0xFFFF if not found (consumer falls back to "no PGXP" path).
 * Map is populated automatically by PGXP_EmitCacheData (uses
 * newData->lookup as the key) and cleared by PGXP_ClearCache. */
ushort	PGXP_GetIndexForXY(uint packed_sxy);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

// special PGXP type
typedef struct {		/* 2D short vector */
	half vx, vy;
	ushort pgxp_index;
} DVECTORF;

#else
typedef short VERTTYPE;

#define _HF(x) x

#endif


#endif // PGXP_DEFS