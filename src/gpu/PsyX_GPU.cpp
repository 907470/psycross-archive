#include "PsyX_GPU.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"

#include "../PsyX_main.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <cstdlib>  /* std::getenv — SDL3 headers no longer leak <stdlib.h> */

#define GET_TPAGE_FORMAT(tpage) ((TexFormat)((tpage >> 7) & 0x3))
#define GET_TPAGE_BLEND(tpage)  ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage) ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)        ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)        (clut >> 6)

OT_TAG prim_terminator = { (uintptr_t)-1, 0 }; // P_TAG with zero primLength

DISPENV currentDispEnv;
DISPENV activeDispEnv;
DRAWENV activeDrawEnv;

static const char* currentSplitDebugText = nullptr;
TextureID overrideTexture = 0;
int overrideTextureWidth = 0;
int overrideTextureHeight = 0;

int g_GPUDisabledState = 0;
int g_DrawPrimMode = 0;

struct GPUDrawSplit
{
	DRAWENV			drawenv;
	DISPENV			dispenv;

	BlendMode		blendMode;

	TexFormat		texFormat;
	TextureID		textureId;

	int				drawPrimMode;

	u_short			startVertex;
	u_short			numVerts;

	const char*		debugText;
};

#define MAX_DRAW_SPLITS	 4096

GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
GPUDrawSplit g_splits[MAX_DRAW_SPLITS];

int g_vertexIndex = 0;
int g_splitIndex = 0;

void ClearSplits()
{
	currentSplitDebugText = nullptr;
	g_vertexIndex = 0;
	g_splitIndex = 0;
	g_splits[0].texFormat = (TexFormat)0xFFFF;
}

template<class T>
void DrawEnvDimensions(T& width, T& height)
{
	if (activeDrawEnv.dfe)
	{
		width = activeDispEnv.disp.w;
		height = activeDispEnv.disp.h;
	}
	else
	{
		width = activeDrawEnv.clip.w;
		height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float& ofsX, float& ofsY)
{
	if (activeDrawEnv.dfe)
	{
		// also make offset in draw dimensions range to prevent flicker
		const int x = activeDispEnv.disp.x;
		const int y = activeDispEnv.disp.y;
		ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		ofsX = 0.0f;
		ofsY = 0.0f;
	}
}

// remaps screen coordinates to [0..1]
// without clamping
inline void ScreenCoordsToEmulator(GrVertex* vertex, int count)
{
#if USE_PGXP
	float w, h;
	DrawEnvDimensions(w, h);

	/* Day 4 Phase 2: normalize every vertex unconditionally. Both shader
	 * branches consume the SAME ortho `Projection` matrix (the perspective
	 * branch composes (Projection * pos).xyz * w into gl_Position), so
	 * both need the same input coord convention: raw/disp.w - 0.5.
	 *
	 * Pre-Phase-2 this function skipped scr_h > 0 verts as a vestige of
	 * upstream PsyCross's perspective path, which expected MAC/128
	 * pre-projection coords; under our shim the cache lookup writes raw
	 * PSX pixel space regardless of perspective-vs-ortho gating, so the
	 * skip caused canonical-context verts to feed un-normalized coords
	 * into Projection and fly off-screen (90% geometry missing). */
	while (count--)
	{
		vertex[count].x = vertex[count].x / w - 0.5f;
		vertex[count].y = vertex[count].y / h - 0.5f;
	}
#endif
}

void LineSwapSourceVerts(VERTTYPE*& p0, VERTTYPE*& p1, unsigned char*& c0, unsigned char*& c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if ((p0[0] > p1[0]) ||
		(p0[1] > p1[1] && p0[0] == p1[0]))
	{
		VERTTYPE* tmp = p0;
		p0 = p1;
		p1 = tmp;

		unsigned char* tmpCol = c0;
		c0 = c1;
		c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, ushort gteidx)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if (dx > abs((short)dy)) 
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else 
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment

#if USE_PGXP
	vertex[0].scr_h = vertex[1].scr_h = vertex[2].scr_h = vertex[3].scr_h = 0.0f;
#endif

	ScreenCoordsToEmulator(vertex, 4);
}

/* Renderer-foundation Phase 0/1 bridges into openpete_core.
 *
 *  Phase 0 (probe): per-vertex SXY lookup in the cache populated by
 *    psxrecomp's gte_rtps_internal. Counter-only — verifies hit rate.
 *
 *  Phase 1 (matcher): per-prim observe hooks group prims by
 *    matrix_span_id and match against the previous frame's batches.
 *    Phase 1 produces stats only; no render changes.
 *
 *  Forward-declared (extern "C") so we don't drag openpete headers
 *  through PsyX's include surface. */
extern "C" {
    extern int g_openpete_render_probe_active;
    int  openpete_render_cache_lookup_curr_raw(uint32_t sxy_key);
    /* PGXP shim — see openpete_render_cache.h and openpete-pgxp.md.
     * Forward-decl avoids dragging the full openpete_render_cache.h
     * (and its iterator API) through the PsyX include surface. */
    typedef struct openpete_pgxp_lookup {
        float px, py, pz;
        float ofx, ofy;
        float scr_h;
    } openpete_pgxp_lookup_t;
    int  openpete_render_cache_lookup_pgxp(uint32_t sxy_key,
                                           openpete_pgxp_lookup_t* out);
    void openpete_render_matcher_observe_tri(const int16_t* p0,
                                             const int16_t* p1,
                                             const int16_t* p2);
    void openpete_render_matcher_observe_quad(const int16_t* p0,
                                              const int16_t* p1,
                                              const int16_t* p2,
                                              const int16_t* p3);
    typedef struct { float dx; float dy; } openpete_substitute_delta_t;
    openpete_substitute_delta_t openpete_render_matcher_substitute_query(
        const int16_t* p, float alpha);
    int openpete_render_matcher_is_smoothed(const int16_t* p);
    extern int g_openpete_dbg_highlight_smoothed;
    int64_t op_now(void);
    float   openpete_render_compute_alpha(int64_t now_ns);
}

/* Phase 2.1 alpha source.
 *
 * OPENPETE_LERP_ALPHA env var (>= 0) takes precedence — useful for
 * testing fixed-alpha visuals. Otherwise we call
 * openpete_render_compute_alpha(op_now()) which returns (now -
 * last_sim_tick) / sim_period clamped to [0, 1]. At sim-due renders
 * alpha ≈ 0; at intermediate renders (run.c re-issues the OT walk
 * between sim ticks at higher render-fps) alpha advances through 0→1
 * across the sim period, producing smooth motion. */
static float g_openpete_lerp_alpha_env = -2.0f;  /* -2 = unread, -1 = unset, 0+ = forced */
extern "C" float g_openpete_substitute_alpha_snapshot;
static inline float openpete_get_lerp_alpha() {
    if (g_openpete_lerp_alpha_env < -1.5f) {
        const char* e = std::getenv("OPENPETE_LERP_ALPHA");
        g_openpete_lerp_alpha_env = e ? (float)atof(e) : -1.0f;
    }
    if (g_openpete_lerp_alpha_env >= 0.0f) return g_openpete_lerp_alpha_env;
    /* Phase 2.1 hotfix — read the snapshot pinned at OT-walk start by
     * either mark_now (sim-due → 0) or snapshot_intermediate (re-emit
     * → compute_alpha at that moment). Avoids per-vertex op_now() drift
     * across a multi-ms OT walk. */
    return g_openpete_substitute_alpha_snapshot;
}
/* Debug render mode: OPENPETE_DEBUG_HIGHLIGHT_SMOOTHED=1 paints every
 * batch the interpolation is actively smoothing as untextured solid
 * red, leaving everything else (frozen / animated / UI) normal. Lets
 * you SEE the rigid/animated classification the harness can't show.
 *
 * Per-prim flow (single-threaded, strict order Vertex→Texcoord→Colour):
 * MakeVertex* sets s_dbg_prim_red from the matcher predicate; the
 * Colour stage (always last → overrides stick) consumes it, forcing
 * red and zeroing page/uv so the GL path renders flat untextured. */
/* Two-state overlay:
 *   RED  = prim is in a paired/eligible batch (it COULD interpolate —
 *          has a per-vertex correspondence) but got no visible motion
 *          THIS render (alpha≈0, or it didn't actually move).
 *   BLUE = prim was ACTUALLY interpolated this render — at least one
 *          vertex received a >=1 px applied delta.
 * Blue takes precedence (the stronger "frame-gen is doing work" signal).
 * Watching at high --render-fps: lots of red + little blue ⇒
 * interpolation is eligible but contributing no visible sub-frame
 * motion (explains a "still looks 30 fps" feel); blue everywhere ⇒
 * frame-gen IS displacing geometry. */
static bool s_dbg_prim_red  = false;
static bool s_dbg_prim_blue = false;
static inline bool openpete_dbg_hl_on() {
	/* Live global (CLI/env-seeded in run.c, F9-toggled). Not a
	 * getenv() — bare env vars don't cross the WSL2→Win32 boundary
	 * into the .exe, but a real symbol does. */
	return g_openpete_dbg_highlight_smoothed != 0;
}
static inline void openpete_dbg_mark_prim(const VERTTYPE* a, const VERTTYPE* b,
                                          const VERTTYPE* c, const VERTTYPE* d) {
	/* Do NOT reset s_dbg_prim_blue here: mark_prim runs AFTER the
	 * substitute block (note_applied already set blue for this prim).
	 * apply_color resets both when it consumes them, so no stale carry. */
	s_dbg_prim_red = false;
	if (!openpete_dbg_hl_on()) return;
	if (openpete_render_matcher_is_smoothed(a) ||
	    openpete_render_matcher_is_smoothed(b) ||
	    openpete_render_matcher_is_smoothed(c) ||
	    (d && openpete_render_matcher_is_smoothed(d)))
		s_dbg_prim_red = true;
}
/* Called from the substitute apply block with the just-computed
 * per-vertex deltas; flags the prim blue if any vertex visibly moved. */
static inline void openpete_dbg_note_applied(float adx, float ady) {
	if (!openpete_dbg_hl_on()) return;
	if (adx < 0) adx = -adx;
	if (ady < 0) ady = -ady;
	if (adx + ady >= 1.0f) s_dbg_prim_blue = true;
}
/* Consumed by every MakeColour* path. Returns true if it recoloured.
 * Blue (actually moved) wins over red (eligible only). */
static inline bool openpete_dbg_apply_color(GrVertex* v, int n) {
	const bool blue = s_dbg_prim_blue, red = s_dbg_prim_red;
	s_dbg_prim_blue = false;
	s_dbg_prim_red  = false;
	if (!blue && !red) return false;
	const unsigned char R = blue ? 0   : 255;
	const unsigned char B = blue ? 255 : 0;
	for (int i = 0; i < n; ++i) {
		v[i].r = R; v[i].g = 0; v[i].b = B; v[i].a = 255;
		v[i].bright = 1;            /* flat, no 2x texture-modulate */
		v[i].page = 0;              /* page 0 = untextured path     */
		v[i].clut = 0;
		v[i].u = v[i].v = 0;
		v[i].tcx = v[i].tcy = 0;
	}
	return true;
}
static inline void RenderCacheProbeVertex(const VERTTYPE* p) {
    if (g_openpete_render_probe_active != 1) return;
    const uint32_t key =
        ((uint32_t)(uint16_t)(int16_t)p[0]) |
        (((uint32_t)(uint16_t)(int16_t)p[1]) << 16);
    (void)openpete_render_cache_lookup_curr_raw(key);
}

inline void ApplyVertexPGXP(GrVertex* v, VERTTYPE* p, float ofsX, float ofsY, ushort gteidx, int lookupOfs)
{
#if USE_PGXP
	/* openpete: PGXP precision sourced from the renderer-foundation
	 * world-space cache (populated by psxrecomp's gte_execute). The
	 * cache shim float-projects SXY directly in PSX pixel space; the
	 * int16-derived v->x/y get overwritten with sub-pixel-precision
	 * floats in the same coord space. We leave scr_h = 0 so the
	 * shader takes its ortho fallback branch — mathematically
	 * identical to the integer route, just with float precision
	 * preserved end-to-end.
	 *
	 * The upstream PsyCross PGXP perspective shader branch is left
	 * un-engaged for now (it expects a different coord-space + scale
	 * relationship between px/scr_h that was tuned for a non-Spyro
	 * game — see openpete-pgxp.md Day 1b). PGXP-Z-buffer mode and
	 * texture correction will require re-engaging that branch with
	 * calibration work; out of scope for this pass.
	 *
	 * gteidx / lookupOfs are vestigial under the SXY-keyed lookup. */
	(void)gteidx; (void)lookupOfs;

	if (g_cfg_pgxpTextureCorrection)
	{
		const uint32_t sxy_key =
			((uint32_t)(uint16_t)(int16_t)p[0]) |
			(((uint32_t)(uint16_t)(int16_t)p[1]) << 16);

		openpete_pgxp_lookup_t vd;
		if (openpete_render_cache_lookup_pgxp(sxy_key, &vd))
		{
			/* vd.px/py are already in PSX pixel space — same coord
			 * space the int16 path produced via `p[i] + ofsX`. The
			 * int16 v->x/y assignment in the caller is overwritten
			 * with the float-precision projection. */
			v->x = vd.px + ofsX;
			v->y = vd.py + ofsY;
			/* Day 4 Phase 2: shim sets scr_h>0 for the canonical 3D
			 * world-projection context, else 0 (HUD/text glyph emit
			 * → ortho fallback). Pass through; the shader's
			 * `a_zw.y > 0.5` branch consumes it for perspective-
			 * correct UV interpolation. */
			v->scr_h = vd.scr_h;
			/* Day 3: small Z perturbation from cached real Z; the
			 * per-split u_pgxp_z_bias uniform dominates so OT order
			 * is preserved between batches. */
			v->z = vd.pz;
			return;
		}
	}

	/* Cache miss: zero perturbation. v->z = 0 means this vertex's
	 * Z = u_pgxp_z_bias (its batch's slot center), which sorts
	 * correctly between batches by draw order. */
	v->scr_h = 0.0f;
	v->z = 0.0f;
#endif
}

void MakeVertexTriangle(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	ApplyVertexPGXP(&vertex[0], p0, ofsX, ofsY, gteidx, -2);
	ApplyVertexPGXP(&vertex[1], p1, ofsX, ofsY, gteidx, -1);
	ApplyVertexPGXP(&vertex[2], p2, ofsX, ofsY, gteidx, 0);

#if USE_PGXP
	/* Day 4 Phase 2: prim-level perspective coherence. The cache-lookup
	 * hit rate is < 100% — partial-miss prims would put one vertex on
	 * the perspective branch (gl_Position.w = scr_h, gigantic) and the
	 * rest on ortho (gl_Position.w = 1, tiny), and the GPU interpolates
	 * between them through infinity. Demote any partial-hit prim to
	 * pure ortho. Only all-hit prims keep perspective. */
	if (vertex[0].scr_h <= 0.0f || vertex[1].scr_h <= 0.0f || vertex[2].scr_h <= 0.0f) {
		vertex[0].scr_h = 0.0f;
		vertex[1].scr_h = 0.0f;
		vertex[2].scr_h = 0.0f;
	}
#endif

	RenderCacheProbeVertex(p0);
	RenderCacheProbeVertex(p1);
	RenderCacheProbeVertex(p2);
	openpete_render_matcher_observe_tri(p0, p1, p2);

	/* Phase 2.0 substitute: per-vertex screen-space delta. Applied to
	 * the GL vertex buffer entry (NOT the prim packet) so subsequent
	 * cache lookups still use original SXY keys. */
	{
		const float a = openpete_get_lerp_alpha();
		if (a > 0.0f) {
			openpete_substitute_delta_t d0 = openpete_render_matcher_substitute_query(p0, a);
			openpete_substitute_delta_t d1 = openpete_render_matcher_substitute_query(p1, a);
			openpete_substitute_delta_t d2 = openpete_render_matcher_substitute_query(p2, a);
			vertex[0].x += d0.dx; vertex[0].y += d0.dy;
			vertex[1].x += d1.dx; vertex[1].y += d1.dy;
			vertex[2].x += d2.dx; vertex[2].y += d2.dy;
			openpete_dbg_note_applied(d0.dx, d0.dy);
			openpete_dbg_note_applied(d1.dx, d1.dy);
			openpete_dbg_note_applied(d2.dx, d2.dy);
		}
	}
	openpete_dbg_mark_prim(p0, p1, p2, nullptr);

	ScreenCoordsToEmulator(vertex, 3);
}

void MakeVertexQuad(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, VERTTYPE* p3, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

	ApplyVertexPGXP(&vertex[0], p0, ofsX, ofsY, gteidx, -3);
	ApplyVertexPGXP(&vertex[1], p1, ofsX, ofsY, gteidx, -2);
	ApplyVertexPGXP(&vertex[2], p2, ofsX, ofsY, gteidx, -1);
	ApplyVertexPGXP(&vertex[3], p3, ofsX, ofsY, gteidx, 0);

#if USE_PGXP
	/* Day 4 Phase 2: prim-level perspective coherence (see MakeVertexTriangle). */
	if (vertex[0].scr_h <= 0.0f || vertex[1].scr_h <= 0.0f ||
	    vertex[2].scr_h <= 0.0f || vertex[3].scr_h <= 0.0f) {
		vertex[0].scr_h = 0.0f;
		vertex[1].scr_h = 0.0f;
		vertex[2].scr_h = 0.0f;
		vertex[3].scr_h = 0.0f;
	}
#endif

	RenderCacheProbeVertex(p0);
	RenderCacheProbeVertex(p1);
	RenderCacheProbeVertex(p2);
	RenderCacheProbeVertex(p3);
	openpete_render_matcher_observe_quad(p0, p1, p2, p3);

	/* Phase 2.0 substitute (see MakeVertexTriangle for rationale). */
	{
		const float a = openpete_get_lerp_alpha();
		if (a > 0.0f) {
			openpete_substitute_delta_t d0 = openpete_render_matcher_substitute_query(p0, a);
			openpete_substitute_delta_t d1 = openpete_render_matcher_substitute_query(p1, a);
			openpete_substitute_delta_t d2 = openpete_render_matcher_substitute_query(p2, a);
			openpete_substitute_delta_t d3 = openpete_render_matcher_substitute_query(p3, a);
			vertex[0].x += d0.dx; vertex[0].y += d0.dy;
			vertex[1].x += d1.dx; vertex[1].y += d1.dy;
			vertex[2].x += d2.dx; vertex[2].y += d2.dy;
			vertex[3].x += d3.dx; vertex[3].y += d3.dy;
			openpete_dbg_note_applied(d0.dx, d0.dy);
			openpete_dbg_note_applied(d1.dx, d1.dy);
			openpete_dbg_note_applied(d2.dx, d2.dy);
			openpete_dbg_note_applied(d3.dx, d3.dy);
		}
	}
	openpete_dbg_mark_prim(p0, p1, p2, p3);

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexRect(GrVertex* vertex, VERTTYPE* p0, short w, short h, ushort gteidx)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;

#if USE_PGXP
	vertex[0].scr_h = vertex[1].scr_h = vertex[2].scr_h = vertex[3].scr_h = 0.0f;
#endif

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeTexcoordQuad(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, unsigned char* uv3, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const unsigned char bright = 2;

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = page;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = page;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = page;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = page;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordTriangle(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const unsigned char bright = 2;

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = page;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = page;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = page;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordRect(GrVertex* vertex, unsigned char* uv, short page, short clut, short w, short h)
{
	assert(uv);

	// sim overflow
	if (int(uv[0]) + w > 255) w = 255 - uv[0];
	if (int(uv[1]) + h > 255) h = 255 - uv[1];

	const unsigned char bright = 2;
	const unsigned char dither = 0;

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = page;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = page;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = page;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = page;
	vertex[3].clut = clut;

	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}
}

void MakeTexcoordLineZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex* vertex, int n)
{
	if (openpete_dbg_apply_color(vertex, n)) return;
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourLine(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	openpete_dbg_apply_color(vertex, 3);
}

void MakeColourQuad(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2, unsigned char* col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;

	openpete_dbg_apply_color(vertex, 4);
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	g_vertexBuffer[g_vertexIndex + 4] = g_vertexBuffer[g_vertexIndex + 3];

	g_vertexBuffer[g_vertexIndex + 5] = g_vertexBuffer[g_vertexIndex + 2];
	g_vertexBuffer[g_vertexIndex + 2] = g_vertexBuffer[g_vertexIndex + 3];
	g_vertexBuffer[g_vertexIndex + 3] = g_vertexBuffer[g_vertexIndex + 1];
}

//------------------------------------------------------------------------------------------------------------------------

static void AddSplit(bool semiTrans, bool textured)
{
	int tpage = activeDrawEnv.tpage;
	GPUDrawSplit& curSplit = g_splits[g_splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GET_TPAGE_FORMAT(tpage);
	TextureID textureId = textured ? g_vramTexture : g_whiteTexture;

	if (textured && overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = overrideTexture;
	}

	// FIXME: compare drawing environment too?
	if (curSplit.blendMode == blendMode &&
		curSplit.texFormat == texFormat &&
		curSplit.textureId == textureId &&
		curSplit.drawPrimMode == g_DrawPrimMode &&
		curSplit.drawenv.clip.x == activeDrawEnv.clip.x &&
		curSplit.drawenv.clip.y == activeDrawEnv.clip.y &&
		curSplit.drawenv.clip.w == activeDrawEnv.clip.w &&
		curSplit.drawenv.clip.h == activeDrawEnv.clip.h &&
		curSplit.drawenv.dfe == activeDrawEnv.dfe &&
		curSplit.debugText == currentSplitDebugText)
	{
		return;
	}

	curSplit.numVerts = g_vertexIndex - curSplit.startVertex;

	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		eprinterr("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors\n");
		return;
	}

	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.blendMode = blendMode;
	split.texFormat = texFormat;
	split.textureId = textureId;
	split.drawPrimMode = g_DrawPrimMode;
	split.drawenv = activeDrawEnv;
	split.dispenv = activeDispEnv;
	split.debugText = currentSplitDebugText;

	split.drawenv.tw.w = overrideTextureWidth;
	split.drawenv.tw.h = overrideTextureHeight;

	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
}

extern unsigned int g_pgxp_split_seq;
extern GLint u_pgxpZBiasLoc;

void DrawSplit(const GPUDrawSplit& split)
{
	if(split.debugText)
		GR_PushDebugLabel(split.debugText);

	GR_SetStencilMode(split.drawPrimMode);	// draw with mask 0x16

	GR_SetTexture(split.textureId, split.texFormat);

	if (split.texFormat == TF_32_BIT_RGBA)
		GR_SetOverrideTextureSize(split.drawenv.tw.w, split.drawenv.tw.h);

	const bool drawOnScreen = split.drawenv.dfe;
	GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
	GR_SetOffscreenState(&split.drawenv.clip, !drawOnScreen);

	GR_SetBlendMode(split.blendMode);

	/* Day 3 PGXP-Z-buffer: on the first split of each frame, the
	 * offscreen FBO is now bound (via GR_SetOffscreenState above),
	 * so it's safe to clear depth here without bind-swap hazards. */
	if (g_pgxp_split_seq == 0) {
		glClearDepth(1.0f);
		glClear(GL_DEPTH_BUFFER_BIT);
	}

	/* Per-split Z bias for PGXP-Z mode. Spyro draws OT back-to-front,
	 * so later splits (higher seq) should win LEQUAL. GR_Ortho2D
	 * INVERTS Z in its projection matrix (ortho_matrix[10] = -1 for
	 * znear=-1/zfar=+1), so pre-projection z=+1 maps to NDC z=-1
	 * (near plane), and pre-projection z=-1 maps to NDC z=+1 (far).
	 * Therefore later splits need LARGER pre-projection z. Range
	 * [-0.95, +0.95]; per-split slot ≈ 2/MAX_DRAW_SPLITS ≈ 0.0005;
	 * intra-batch perturbation is 0 (preserves OT/triangle order
	 * within a draw call — see openpete-pgxp.md Day 3). */
	if (u_pgxpZBiasLoc >= 0) {
		const float kMax = (float)MAX_DRAW_SPLITS;
		const float bias = ((float)g_pgxp_split_seq / kMax) * 1.9f - 0.95f;
		glUniform1f(u_pgxpZBiasLoc, bias);
	}
	g_pgxp_split_seq++;

	GR_DrawTriangles(split.startVertex, split.numVerts / 3);

	if (split.debugText)
		GR_PopDebugLabel();
}

extern int g_dbg_polygonSelected;

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
#ifdef _DEBUG
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex* vert = &g_vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			eprintf("==========================================\n");
			eprintf("POLYGON: %d\n", g_dbg_polygonSelected);
#if USE_PGXP
			eprintf("X: %.2f Y: %.2f\n", (float)vert->x, (float)vert->y);
			eprintf("U: %.2f V: %.2f\n", (float)vert->u, (float)vert->v);
			eprintf("TP: %d CLT: %d\n", (int)vert->page, (int)vert->clut);
#else
			eprintf("X: %d Y: %d\n", vert->x, vert->y);
			eprintf("U: %d V: %d\n", vert->u, vert->v);
			eprintf("TP: %d CLT: %d\n", vert->page, vert->clut);
#endif
			
			eprintf("==========================================\n");
		}

		PsyX_UpdateInput();
	}
#endif // _DEBUG

	// next code ideally should be called before EndScene
	GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);

	for (int i = 1; i <= g_splitIndex; i++)
		DrawSplit(g_splits[i]);

	ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG* polyTag);

void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive)
{
	if (!p)
		return;

	// setup single primitive flag (needed for AddSplits)
	g_DrawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG* polyTag = reinterpret_cast<P_TAG*>(p);
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
		// force PGXP off
		polyTag->pgxp_index = 0xFFFF;
#endif
		ParsePrimitive(polyTag);

		GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
		lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
	}
	else
	{
		// walk OT_TAG linked list
		for (uintptr_t basePacket = reinterpret_cast<uintptr_t>(p);; basePacket = reinterpret_cast<uintptr_t>(nextPrim(basePacket)))
		{
			const int tagLength = getlen(basePacket);
			if (tagLength > 0)
			{
				if (tagLength > 32)
				{
					eprinterr("got invalid tag length %d, code %d\n", tagLength, reinterpret_cast<P_TAG*>(basePacket)->code);
				}

				uintptr_t currentPacket = basePacket;
				const uintptr_t endPacket = basePacket + (tagLength + P_LEN) * sizeof(u_int);
				int primLength = 0;
				while (currentPacket < endPacket)
				{
					primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket));
					currentPacket += (primLength + P_LEN) * sizeof(u_int);
				}

				if (currentPacket != endPacket)
				{
					eprinterr("did not output valid primitive or ptag length is not valid (diff=%d)\n", endPacket-currentPacket);
				}
			}

			GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
			lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;

			if (isendprim(basePacket))
				break;
		}
	}
}

inline int IsNull(POLY_FT3* poly)
{
	return  poly->x0 == -1 &&
		poly->y0 == -1 &&
		poly->x1 == -1 &&
		poly->y1 == -1 &&
		poly->x2 == -1 &&
		poly->y2 == -1;
}

static int ProcessFlatLines(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2* poly = (LINE_F2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = c0;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3* poly = (LINE_F3*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 5;
	}
	case 0xc:
	{
		int i;
		LINE_F4* poly = (LINE_F4*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x2;
			VERTTYPE* p1 = &poly->x3;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 6;
	}
	}
	return 0;
}

static int ProcessGouraudLines(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2* poly = (LINE_G2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = &poly->r1;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

static int ProcessFlatPoly(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3* poly = (POLY_F3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x4:
	{
		POLY_FT3* poly = (POLY_FT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
			AddSplit(semiTrans, true);

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4* poly = (POLY_F4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 5;
	}
	case 0xC:
	{
		POLY_FT4* poly = (POLY_FT4*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	}
	return 0;
}

static int ProcessGouraudPoly(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3* poly = (POLY_G3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 6;
	}
	case 0x4:
	{
		POLY_GT3* poly = (POLY_GT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	case 0x8:
	{
		POLY_G4* poly = (POLY_G4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 8;
	}
	case 0xC:
	{
		POLY_GT4* poly = (POLY_GT4*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 12;
	}
	}
	return 0;
}

static int ProcessTileAndSprt(P_TAG* polyTag)
{
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
	const u_short gteIndex = polyTag->pgxp_index;
#else
	const u_short gteIndex = 0xFFFF;
#endif

	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE* poly = (TILE*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x64:
	{
		SPRT* poly = (SPRT*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x68:
	{
		TILE_1* poly = (TILE_1*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x70:
	{
		TILE_8* poly = (TILE_8*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x74:
	{
		SPRT_8* poly = (SPRT_8*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x78:
	{
		TILE_16* poly = (TILE_16*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x7C:
	{
		SPRT_16* poly = (SPRT_16*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	}
	return 0;
}

static int ProcessDrawEnv(P_TAG* polyTag)
{
	const u_int* codePtr = (u_int*)&polyTag->pad0;
	int processedLongs = 0;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u_int code = codePtr[i];
		const int primSubType = code >> 24 & 0x0F;

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE — update tpage/dither only. Bit 10 of the GP0(E1)
			// hardware word is "drawing to display area allowed", a writes-
			// gating flag. PsyCross's `dfe` field also drives a coord-remap
			// mode switch (DrawEnvDimensions/DrawEnvOffset, disp vs clip
			// rect). Letting an inline DR_TPAGE rewrite that flag conflates
			// the two — Spyro emits DR_TPAGE with bit 10 = 0 from
			// setDrawMode-style encoders, which flipped the coord remap and
			// vanished far/world geometry. The drawenv rect/offset config is
			// owned by PutDrawEnv; a mid-chain texture-page change should
			// not touch it.
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w -= activeDrawEnv.clip.x;
			activeDrawEnv.clip.h -= activeDrawEnv.clip.y;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			// TODO
			activeDrawEnv.ofs[0] = code & 2047;
			activeDrawEnv.ofs[1] = (code >> 11) & 2047;
			break;
		}
		case 0x6:
		{
			eprintf("Mask setting: %08x\n", code);
			//MaskSetOR = (*cb & 1) ? 0x8000 : 0x0000;
			//MaskEvalAND = (*cb & 2) ? 0x8000 : 0x0000;
			break;
		}
		case 0:
			// proceed to next primitive tag
			return processedLongs;
		}
		++processedLongs;
	}

	return processedLongs;
}

static int ProcessPsyXPrims(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX* psytex = (DR_PSYX_TEX*)polyTag;
		overrideTexture = psytex->code[0] & 0xFFFFFF;
		overrideTextureWidth = psytex->code[1] & 0xFFF;
		overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom texture packet
		DR_PSYX_DBGMARKER* psydbg = (DR_PSYX_DBGMARKER*)polyTag;
		currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
extern "C" {
	extern int      g_openpete_render_probe_active;
}
extern "C" unsigned g_openpete_render_probe_count_ot_walk  = 0u;
extern "C" unsigned g_openpete_render_probe_count_ot_pgxp_hit = 0u;
extern "C" unsigned g_openpete_render_probe_count_ot_pgxp_miss = 0u;

// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;

	/* fps-unlock Phase 2 probe: count every primitive consumed by the
	 * OT walker. This is the ground truth for "how many prims actually
	 * exist in the OT each frame", regardless of whether they got there
	 * via AddPrim() function calls or via the inline addPrim() macro
	 * (which is just two memory writes — the common path). */
	if (g_openpete_render_probe_active == 1) {
		g_openpete_render_probe_count_ot_walk++;
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
		if (polyTag->pgxp_index != 0xFFFF) g_openpete_render_probe_count_ot_pgxp_hit++;
		else                               g_openpete_render_probe_count_ot_pgxp_miss++;
#endif
	}

	int primLength = 0;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE* drmove = (DR_MOVE*)polyTag;

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drmove->code[2];
			*(uint*)&rect.w = *(uint*)&drmove->code[4];

			MoveImage(&rect, x, y);
			primLength = 5;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD* drload = (DR_LOAD*)polyTag;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drload->code[1];
			*(uint*)&rect.w = *(uint*)&drload->code[2];

			LoadImage(&rect, (u_long*)drload->p);
			//Emulator_UpdateVRAM();			// FIXME: should it be updated immediately?

			// FIXME: is there othercommands?
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
	//default:
	//	eprinterr("got %0x primitive\n", primType);
	}

	if(primLength == 0)
	{
		eprinterr("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}
