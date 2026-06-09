#include "PsyX/PsyX_public.h"

#include "../platform.h"
#include "../gpu/PsyX_GPU.h"

#include "PsyX/PsyX_render.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/util/timer.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif //def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO					1
#define USE_OFFSCREEN_BLIT		1
#define USE_FRAMEBUFFER_BLIT	1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as framebuffer, so it's disabled
#define USE_PBO					(OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT		(OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT	(OGLES_VERSION == 3)

#endif

/* Internalformat for FBO color-attachment textures (present / blit /
 * offscreen render targets). Desktop GL accepts the unsized GL_RGBA as a
 * color-renderable attachment, but GLES3 / WebGL2 require a SIZED format —
 * an unsized GL_RGBA texture is not color-renderable, so glBlitFramebuffer
 * reports "Framebuffer is incomplete: Attachment is not renderable" and
 * nothing reaches the screen. GL_RGBA8 is valid on both. */
#if defined(RENDERER_OGLES)
#	define PSYX_RT_COLOR_INTERNALFORMAT GL_RGBA8
#else
#	define PSYX_RT_COLOR_INTERNALFORMAT GL_RGBA
#endif

extern SDL_Window* g_window;


#define MAX_NUM_VERTEX_BUFFERS		(2)
#define PSX_SCREEN_ASPECT	(240.0f / 320.0f)			// PSX screen is mapped always to this aspect

int g_PreviousBlendMode = BM_NONE;
int g_PreviousDepthMode = 0;
int g_PreviousStencilMode = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = { 0,0,0,0 };
RECT16 g_PreviousOffscreen = { 0,0,0,0 };

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

/* OpenPete: scene rendering is redirected to this FBO so the final present
 * blit can crop the PSX overscan bands (rows where DRAWENV.clip is shorter
 * than DISPENV.disp — typically 8px top + 8px bottom of a 240-tall display).
 * Default fb 0 only receives the cropped scene + post-render overlay. */
TextureID g_presentColorTexture = -1;
GLuint g_glPresentFramebuffer = 0;
GLuint g_glPresentDepthStencilRBO = 0;
GLuint g_glOffscreenDepthStencilRBO = 0;  /* Day 3 PGXP-Z-buffer mode */
unsigned int g_pgxp_split_seq = 0;        /* per-frame draw-call counter */
int g_presentFbWidth = 0;
int g_presentFbHeight = 0;

TextureID g_whiteTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;
int g_cfg_bilinearFiltering = 0;

int vram_need_update = 1;
int framebuffer_need_update = 0;

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif



#if USE_OPENGL
typedef struct
{
	GLenum fmt;
	GLuint* pbos;
	uint64_t num_pbos;
	uint64_t dx;
	uint64_t num_downloads;

	int width;
	int height;
	int nbytes; /* number of bytes in the pbo buffer. */
	unsigned char* pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO* pbo, GLenum format, int w, int h, int num)
{
	if (pbo->pbos)
	{
		eprinterr("Already initialized. Not necessary to initialize again; or shutdown first.");
		return -1;
	}

	if (0 >= num)
	{
		eprinterr("Invalid number of PBOs: %d", num);
		return -2;
	}

	pbo->fmt = format;
	pbo->width = w;
	pbo->height = h;
	pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
	if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height;
	}
	else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt)
	{
		pbo->nbytes = pbo->width * pbo->height * 3;
	}
	else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height * 4;
	}
	else
	{
		eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
		return -3;
	}

	if (pbo->nbytes == 0)
	{
		eprinterr("Invalid width or height given: %d x %d", pbo->width, pbo->height);
		return -4;
	}

	pbo->pbos = (GLuint*)malloc(sizeof(GLuint) * num);
	pbo->pixels = (u_char*)malloc(pbo->nbytes);

	glGenBuffers(num, pbo->pbos);
	for (int i = 0; i < num; ++i)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
	return 0;
}

void PBO_Destroy(GrPBO* pbo)
{
#if USE_PBO
	if(pbo->pbos)
	{
		glDeleteBuffers(pbo->num_pbos, pbo->pbos);
	
		free(pbo->pbos);
		pbo->num_pbos = 0;
		pbo->pbos = NULL;
	}

#endif
	if (pbo->pixels)
	{
		free(pbo->pixels);
		pbo->pixels = NULL;
	}

	pbo->num_downloads = 0;
	pbo->dx = 0;
	pbo->fmt = 0;
	pbo->nbytes = 0;
}

void PBO_Download(GrPBO* pbo)
{
	unsigned char* ptr;
	
#if USE_PBO
	if (pbo->num_downloads < pbo->num_pbos)
	{
		/*
		   First we need to make sure all our pbos are bound, so glMap/Unmap will
		   read from the oldest bound buffer first.
		*/
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);   /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used as offset into the buffer to read into. */
#endif
	}
	else
	{
		/* Read from the oldest bound pbo */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		ptr = (unsigned char*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if (NULL != ptr)
		{
			memcpy(pbo->pixels, ptr, pbo->nbytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		else
			eprintwarn("Failed to map the buffer\n");

		/* Trigger the next read. */
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
	}

	++pbo->dx;
	pbo->dx = pbo->dx % pbo->num_pbos;

	pbo->num_downloads++;

	if (pbo->num_downloads == UINT64_MAX)
		pbo->num_downloads = pbo->num_pbos;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
	// FIXME: THIS is very slow
	// Do not use at all

	// glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not accidentilly using a PBO. */
	// glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint		g_glVertexArray[2];
GLuint		g_glVertexBuffer[2];
int			g_curVertexBuffer = 0;

GLuint		g_glBlitFramebuffer;
GrPBO		g_glFramebufferPBO;

GLuint		g_glVRAMFramebuffer;

GLuint		g_glOffscreenFramebuffer;
GrPBO		g_glOffscreenPBO;

#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char* windowName, int fullscreen, int startHidden)
{
	SDL_WindowFlags windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(__ANDROID__)
	windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
	if (fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

	/* SDL3 dropped SDL_WINDOW_FULLSCREEN_DESKTOP; SDL_WINDOW_FULLSCREEN is
	 * borderless desktop fullscreen by default. */
	if(g_windowWidth <= 0 || g_windowHeight <= 0)
		windowFlags |= SDL_WINDOW_FULLSCREEN;

	/* Headless: create the window unmapped so it never flashes on screen
	 * or steals focus during GL/PSX init. The GL context and FBO rendering
	 * work fine on a hidden window; the caller just never presents it. */
	if (startHidden)
		windowFlags |= SDL_WINDOW_HIDDEN;

	/* SDL3 SDL_CreateWindow no longer takes window position arguments. */
	g_window = SDL_CreateWindow(windowName, g_windowWidth, g_windowHeight, windowFlags);

	if (g_window == NULL)
	{
		eprinterr("Failed to initialise SDL window!\n");
		return 0;
	}
	
#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
	//Override to full screen.
	const SDL_DisplayMode* displayMode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
	if (displayMode)
	{
		screenWidth = displayMode->w;
		windowWidth = displayMode->w;
		screenHeight = displayMode->h;
		windowHeight = displayMode->h;
	}
#endif

	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

	if(!SDL_GL_CreateContext(g_window))
	{
		eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n", OGLES_VERSION);
		return 0;
	}

#elif defined(RENDERER_OGL)

	int major_version = 3;
	int minor_version = 3;
	int profile = SDL_GL_CONTEXT_PROFILE_CORE;

	// find best OpenGL version
	do
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

		if (SDL_GL_CreateContext(g_window))
			break;
	
		minor_version--;
		
	} while (minor_version >= 0);

	if (minor_version == -1)
	{
		eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please update video drivers.\n");
		return 0;
	}
#endif

	return 1;
}
#endif

int GR_InitialiseGLExt()
{
#ifdef USE_GLAD
	GLenum err = gladLoadGL();

	if (err == 0)
		return 0;
#endif
	
	const char* rend = (const char*)glGetString(GL_RENDERER);
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	eprintf("*Video adapter: %s by %s\n", rend, vendor);

	const char* versionStr = (const char*)glGetString(GL_VERSION);
	eprintf("*OpenGL version: %s\n", versionStr);

	const char* glslVersionStr = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	eprintf("*GLSL version: %s\n", glslVersionStr);

	return 1;
}

int GR_InitialiseRender(char* windowName, int width, int height, int fullscreen, int startHidden)
{
	g_windowWidth = width;
	g_windowHeight = height;

	// Due to debugging in fullscreen
	SDL_SetHint(SDL_HINT_WINDOW_ALLOW_TOPMOST, "0");
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#if USE_OPENGL
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	if (!GR_InitialiseGLContext(windowName, fullscreen, startHidden))
	{
		eprinterr("Failed to Initialise GL Context!\n");
		return 0;
	}
#endif

	if (!GR_InitialiseGLExt())
	{
		eprinterr("Failed to Intialise GL extensions\n");
		return 0;
	}
#endif
	
	return 1;
}

#if USE_OPENGL
static void GR_DestroyPresentFB();
#endif

void GR_Shutdown()
{
#if USE_OPENGL
	glDeleteVertexArrays(2, g_glVertexArray);
	glDeleteBuffers(2, g_glVertexBuffer);

	PBO_Destroy(&g_glFramebufferPBO);
	PBO_Destroy(&g_glOffscreenPBO);

	glDeleteFramebuffers(1, &g_glBlitFramebuffer);
	glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
	glDeleteFramebuffers(1, &g_glVRAMFramebuffer);
	if (g_glOffscreenDepthStencilRBO) {
		glDeleteRenderbuffers(1, &g_glOffscreenDepthStencilRBO);
		g_glOffscreenDepthStencilRBO = 0;
	}

	GR_DestroyPresentFB();

	GR_DestroyTexture(g_vramTexturesDouble[0]);
	GR_DestroyTexture(g_vramTexturesDouble[1]);

	GR_DestroyTexture(g_whiteTexture);
	GR_DestroyTexture(g_fbTexture);
	GR_DestroyTexture(g_offscreenRTTexture);
#endif
}

void GR_UpdateSwapIntervalState(int swapInterval)
{
#if defined(RENDERER_OGL)
	SDL_GL_SetSwapInterval(swapInterval);
#endif
}

#if USE_OPENGL
static void GR_DestroyPresentFB()
{
	if (g_glPresentFramebuffer)
	{
		glDeleteFramebuffers(1, &g_glPresentFramebuffer);
		g_glPresentFramebuffer = 0;
	}
	if (g_glPresentDepthStencilRBO)
	{
		glDeleteRenderbuffers(1, &g_glPresentDepthStencilRBO);
		g_glPresentDepthStencilRBO = 0;
	}
	if (g_presentColorTexture != (TextureID)-1)
	{
		GR_DestroyTexture(g_presentColorTexture);
		g_presentColorTexture = -1;
	}
	g_presentFbWidth = 0;
	g_presentFbHeight = 0;
}

static void GR_EnsurePresentFB(int w, int h)
{
	if (w <= 0 || h <= 0)
		return;

	/* Clamp the FBO dimensions to the GL limits. The present FBO width is
	 * derived from the window height times the content aspect, so an
	 * extreme aspect (e.g. 512:9 -> factor ~42.7) asks for a >60k-px-wide
	 * texture. glTexImage2D / glRenderbufferStorage past GL_MAX_TEXTURE_SIZE
	 * fail (GL_INVALID_VALUE), leaving an incomplete FBO that the next
	 * blit/swap hangs on (observed under the WSL2 D3D12->GL layer). Clamping
	 * just reduces horizontal pixel density at absurd aspects — the
	 * present-blit already scales the FBO onto the window. Query once. */
	{
		static int s_max_dim = 0;
		if (s_max_dim == 0) {
			GLint maxTex = 0, maxRb = 0;
			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
			glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRb);
			s_max_dim = (maxTex < maxRb || maxRb == 0) ? maxTex : maxRb;
			if (s_max_dim <= 0) s_max_dim = 8192;  /* conservative fallback */
		}
		if (w > s_max_dim || h > s_max_dim) {
			static int s_warned = 0;
			if (!s_warned) {
				s_warned = 1;
				fprintf(stderr, "[PsyX] present FBO %dx%d exceeds GL max %d; "
						"clamping (extreme aspect — horizontal detail reduced)\n",
						w, h, s_max_dim);
			}
			if (w > s_max_dim) w = s_max_dim;
			if (h > s_max_dim) h = s_max_dim;
		}
	}

	if (g_glPresentFramebuffer && g_presentFbWidth == w && g_presentFbHeight == h)
		return;

	GR_DestroyPresentFB();

	GLuint colorTex = 0;
	glGenTextures(1, &colorTex);
	glBindTexture(GL_TEXTURE_2D, colorTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, PSYX_RT_COLOR_INTERNALFORMAT, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	g_presentColorTexture = (TextureID)colorTex;

	glGenRenderbuffers(1, &g_glPresentDepthStencilRBO);
	glBindRenderbuffer(GL_RENDERBUFFER, g_glPresentDepthStencilRBO);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &g_glPresentFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, g_glPresentFramebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_glPresentDepthStencilRBO);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	g_presentFbWidth = w;
	g_presentFbHeight = h;
}
#endif

/* Compute the FBO height needed so the visible scissor region (clip.h disp
 * rows) maps 1:1 onto the window height. For Spyro: 240 disp / 224 clip →
 * fb_h = window_h * 240/224 = 480 (window_h=448). */
static int GR_PresentFBHeightForWindow(int window_h)
{
	int disp_h = activeDispEnv.disp.h;
	int clip_h = activeDrawEnv.clip.h;
	if (disp_h <= 0 || clip_h <= 0 || clip_h > disp_h)
		return window_h;
	if (clip_h == disp_h)
		return window_h;
	long long fb_h = ((long long)window_h * disp_h + (clip_h / 2)) / clip_h;
	if (fb_h < window_h) fb_h = window_h;
	return (int)fb_h;
}

/* FBO width is pinned to FBO height by the canonical PSX-NTSC display
 * aspect (4:3), times the Hor+ render-aspect factor when widescreen is
 * active. Decoupling FBO width from g_windowWidth keeps the cropped src's
 * aspect fixed across host window resizes — the present-blit then
 * letter/pillarboxes instead of stretching. */
static int GR_PresentFBWidthForHeight(int fb_h)
{
	double aspect = 4.0 / 3.0;
	if (g_psyx_render_aspect_factor > 0.0f)
		aspect *= (double)g_psyx_render_aspect_factor;
	int fb_w = (int)((double)fb_h * aspect + 0.5);
	if (fb_w < 1) fb_w = 1;
	return fb_w;
}

/* Compute the present-FBO source crop rect — the canonical PSX visible
 * region per DRAWENV.clip — in bottom-origin FBO pixels. Returns 0 (and
 * leaves outputs untouched) when there is no present FBO. Shared by the
 * cropped window present and the native-resolution screenshot reader so
 * both agree exactly on which FBO pixels constitute "the rendered scene":
 *   top edge    = fb_h - dy * fb_h/disp_h
 *   bottom edge = fb_h - (dy + clip_h) * fb_h/disp_h
 * For Spyro+window=448: fb_h=480, src_y=[16, 464], width 448 → 1:1. */
static int GR_GetPresentSceneCrop(int* out_x, int* out_y, int* out_w, int* out_h)
{
#if USE_OPENGL
	if (!g_glPresentFramebuffer)
		return 0;

	int disp_h = activeDispEnv.disp.h;
	int clip_h = activeDrawEnv.clip.h;
	int dy     = activeDrawEnv.clip.y - activeDispEnv.disp.y;
	if (disp_h <= 0)  disp_h = g_presentFbHeight;
	if (clip_h <= 0 || clip_h > disp_h) clip_h = disp_h;
	if (dy < 0) dy = 0;

	int top_disp_y = dy;
	int bot_disp_y = dy + clip_h;
	if (bot_disp_y > disp_h) bot_disp_y = disp_h;
	int src_y_bottom = g_presentFbHeight - (top_disp_y * g_presentFbHeight + disp_h / 2) / disp_h;
	int src_y_top    = g_presentFbHeight - (bot_disp_y * g_presentFbHeight + disp_h / 2) / disp_h;
	if (src_y_top < 0) src_y_top = 0;
	if (src_y_bottom > g_presentFbHeight) src_y_bottom = g_presentFbHeight;

	if (out_x) *out_x = 0;
	if (out_y) *out_y = src_y_top;
	if (out_w) *out_w = g_presentFbWidth;
	if (out_h) *out_h = src_y_bottom - src_y_top;
	return 1;
#else
	(void)out_x; (void)out_y; (void)out_w; (void)out_h;
	return 0;
#endif
}

/* Native-resolution screenshot helpers. Read straight from the present FBO
 * (where the scene is rendered) rather than the window backbuffer, so the
 * saved image is the rendered scene at internal resolution with no
 * letter/pillarbox bars regardless of how the window is sized. */
extern "C" int GR_GetPresentSceneSize(int* out_w, int* out_h)
{
	int x, y, w, h;
	if (!GR_GetPresentSceneCrop(&x, &y, &w, &h) || w <= 0 || h <= 0)
		return 0;
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
	return 1;
}

extern "C" int GR_ReadPresentSceneRGBA(unsigned char* pixels, int w, int h, unsigned int gl_format)
{
#if USE_OPENGL
	int x, y, cw, ch;
	if (!pixels || !GR_GetPresentSceneCrop(&x, &y, &cw, &ch) || cw != w || ch != h || w <= 0 || h <= 0)
		return 0;
	/* The scene leaves GL_SCISSOR_TEST enabled with a rect sized for the
	 * FBO; glReadPixels ignores scissor, but disable it defensively to keep
	 * the read independent of prior draw state, then restore. Rows come back
	 * bottom-up (GL origin bottom-left) — the caller flips. */
	GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (prev_scissor) glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glPresentFramebuffer);
	glReadPixels(x, y, w, h, (GLenum)gl_format, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);
	return 1;
#else
	(void)pixels; (void)w; (void)h; (void)gl_format;
	return 0;
#endif
}

/* Asynchronous present-scene readback ring. GR_ReadPresentSceneRGBA stalls
 * the GL thread until the GPU finishes the transfer; for continuous capture
 * (--record at high frame rates) that serializes CPU and GPU. This ring
 * issues each glReadPixels into a rotating pixel-pack buffer (PBO) so the
 * DMA overlaps subsequent rendering, then hands back a frame submitted a few
 * frames earlier once its transfer has completed. Single GL thread only.
 *
 * Protocol (per frame, before EndScene): if Pending == RingDepth, Map the
 * oldest buffer, copy it out, Unmap; then Submit the current frame. At
 * shutdown, Map/Unmap until Pending reaches 0 to drain in-flight frames.
 * Rows are bottom-up (GL origin), same as GR_ReadPresentSceneRGBA. */
#define PRESENT_PBO_RING 4
static GLuint s_presentPBO[PRESENT_PBO_RING];
static int    s_presentPBO_w[PRESENT_PBO_RING];
static int    s_presentPBO_h[PRESENT_PBO_RING];
static int    s_presentPBO_inited = 0;
static int    s_presentPBO_tail   = 0;  /* oldest in-flight slot */
static int    s_presentPBO_count  = 0;  /* in-flight buffers */
static int    s_presentPBO_mapped = 0;  /* tail currently mapped */

extern "C" int GR_PresentPBO_RingDepth(void) { return PRESENT_PBO_RING; }
extern "C" int GR_PresentPBO_Pending(void)   { return s_presentPBO_count; }

/* Issue an async read of the current present scene into the next free PBO.
 * Returns 1 (+ submitted dims) on success, 0 if there is no present FBO or
 * the ring is full (caller must Map/Unmap an entry first). */
extern "C" int GR_PresentPBO_Submit(unsigned int gl_format, int* out_w, int* out_h)
{
#if USE_OPENGL
	if (!g_glPresentFramebuffer || s_presentPBO_count >= PRESENT_PBO_RING)
		return 0;
	int x, y, w, h;
	if (!GR_GetPresentSceneCrop(&x, &y, &w, &h) || w <= 0 || h <= 0)
		return 0;
	if (!s_presentPBO_inited) {
		glGenBuffers(PRESENT_PBO_RING, s_presentPBO);
		for (int i = 0; i < PRESENT_PBO_RING; ++i) { s_presentPBO_w[i] = s_presentPBO_h[i] = 0; }
		s_presentPBO_inited = 1;
	}
	const int slot = (s_presentPBO_tail + s_presentPBO_count) % PRESENT_PBO_RING;
	GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (prev_scissor) glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glPresentFramebuffer);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, s_presentPBO[slot]);
	/* Orphan + size the buffer; the slot is free (not in flight) so this
	 * never blocks on a pending transfer. RGBA8 => row pitch w*4 is always
	 * 4-aligned, so the default GL_PACK_ALIGNMENT needs no adjustment. */
	glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)((size_t)w * h * 4), NULL, GL_STREAM_READ);
	glReadPixels(x, y, w, h, (GLenum)gl_format, GL_UNSIGNED_BYTE, (void*)0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);
	s_presentPBO_w[slot] = w;
	s_presentPBO_h[slot] = h;
	s_presentPBO_count++;
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
	return 1;
#else
	(void)gl_format; (void)out_w; (void)out_h;
	return 0;
#endif
}

/* Map the oldest in-flight buffer for reading. Returns a pointer to its
 * pixels (w*h*4, bottom-up) + dims, or NULL if the ring is empty / already
 * mapped. The pointer is valid until GR_PresentPBO_Unmap. */
extern "C" unsigned char* GR_PresentPBO_Map(int* out_w, int* out_h)
{
#if USE_OPENGL
	if (s_presentPBO_count <= 0 || s_presentPBO_mapped)
		return NULL;
	const int slot = s_presentPBO_tail;
	const int w = s_presentPBO_w[slot], h = s_presentPBO_h[slot];
	glBindBuffer(GL_PIXEL_PACK_BUFFER, s_presentPBO[slot]);
	void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
	                           (GLsizeiptr)((size_t)w * h * 4), GL_MAP_READ_BIT);
	if (!p) { glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); return NULL; }
	s_presentPBO_mapped = 1;
	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
	return (unsigned char*)p;
#else
	(void)out_w; (void)out_h;
	return NULL;
#endif
}

/* Unmap + retire the oldest in-flight buffer (advances the ring). */
extern "C" void GR_PresentPBO_Unmap(void)
{
#if USE_OPENGL
	if (!s_presentPBO_mapped)
		return;
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	s_presentPBO_mapped = 0;
	s_presentPBO_tail = (s_presentPBO_tail + 1) % PRESENT_PBO_RING;
	s_presentPBO_count--;
#endif
}

extern "C" void GR_PresentCroppedToWindow()
{
#if USE_OPENGL
	int src_y_top, src_w, src_crop_h;
	if (!GR_GetPresentSceneCrop(NULL, &src_y_top, &src_w, &src_crop_h))
		return;
	int src_y_bottom = src_y_top + src_crop_h;

	/* glBlitFramebuffer respects GL_SCISSOR_TEST, and the scene leaves
	 * scissor enabled with rect sized for the present FBO. On the (shorter)
	 * default framebuffer that scissor would clip the bottom of the dst
	 * rect, leaving a band of stale backbuffer pixels. Disable scissor for
	 * the blit and restore the prior state. */
	GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (prev_scissor) glDisable(GL_SCISSOR_TEST);

	/* Preserve the cropped region's aspect ratio in the host window: scale
	 * the dst rect to fit while matching src aspect, centre, and clear the
	 * surrounding area to black so resizing the window letter/pillarboxes
	 * instead of stretching. */
	const int src_h = src_y_bottom - src_y_top;
	int dst_w = g_windowWidth;
	int dst_h = g_windowHeight;
	int dst_x = 0;
	int dst_y = 0;
	if (src_w > 0 && src_h > 0) {
		const long long win_aspect_num = (long long)g_windowWidth  * src_h;
		const long long src_aspect_num = (long long)src_w * g_windowHeight;
		if (win_aspect_num > src_aspect_num) {
			/* window wider than content → pillarbox */
			dst_h = g_windowHeight;
			dst_w = (int)(((long long)g_windowHeight * src_w + src_h / 2) / src_h);
			dst_x = (g_windowWidth  - dst_w) / 2;
			dst_y = 0;
		} else if (win_aspect_num < src_aspect_num) {
			/* window taller than content → letterbox */
			dst_w = g_windowWidth;
			dst_h = (int)(((long long)g_windowWidth * src_h + src_w / 2) / src_w);
			dst_x = 0;
			dst_y = (g_windowHeight - dst_h) / 2;
		}
	}
	if (dst_x > 0 || dst_y > 0 ||
	    dst_x + dst_w < g_windowWidth || dst_y + dst_h < g_windowHeight) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glPresentFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBlitFramebuffer(
		0, src_y_top, src_w, src_y_bottom,
		dst_x, dst_y, dst_x + dst_w, dst_y + dst_h,
		GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	if (prev_scissor) glEnable(GL_SCISSOR_TEST);
#endif
}

void GR_BeginScene()
{
	g_lastBoundTexture = 0;

#if USE_OPENGL
	{
		int fb_h = GR_PresentFBHeightForWindow(g_windowHeight);
		int fb_w = GR_PresentFBWidthForHeight(fb_h);
		GR_EnsurePresentFB(fb_w, fb_h);
	}
	if (g_glPresentFramebuffer)
		glBindFramebuffer(GL_FRAMEBUFFER, g_glPresentFramebuffer);

#ifdef RENDERER_OGLES
	glClearDepthf(1.0f);
#else
	glClearDepth(1.0f);
#endif
	glClear(GL_DEPTH_BUFFER_BIT);
	glClear(GL_STENCIL_BUFFER_BIT);

	/* Day 3 PGXP-Z-buffer: reset per-frame split counter. DrawSplit
	 * clears the offscreen FBO's depth on the first split (when seq
	 * is still 0), at which point the offscreen FBO is already bound
	 * — that avoids the per-split clear of GR_SetOffscreenState and
	 * the bind-swap hazard of clearing here. */
	g_pgxp_split_seq = 0;
#endif

	GR_UpdateVRAM();
	/* Viewport spans the present FBO, not the window — the FBO may be taller
	 * than the window when overscan cropping is in play (see GR_PresentCroppedToWindow). */
	if (g_glPresentFramebuffer)
		GR_SetViewPort(0, 0, g_presentFbWidth, g_presentFbHeight);
	else
		GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

	if (g_dbg_wireframeMode)
	{
		GR_SetWireframe(1);

#if USE_OPENGL
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
#endif
	}
}

void GR_EndScene()
{
	framebuffer_need_update = 1;
	
	if (g_dbg_wireframeMode)
		GR_SetWireframe(0);

#if USE_OPENGL
	glBindVertexArray(0);
#endif
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];

/* See PsyX_public.h. The CPU-side `vram` mirror is authoritative — every
 * write path (LoadImage, MoveImage, framebuffer copyback) goes through it
 * and sets vram_need_update so GR_UpdateVRAM uploads the new bytes to the
 * GL texture next frame. Restore replays the same handshake so the GPU
 * picks up the snapshotted contents on the very next present. */
extern "C" unsigned int PsyX_VRAM_GetSize(void)
{
	return (unsigned int)(VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
}

extern "C" int PsyX_VRAM_Snapshot(void* dst, unsigned int dst_size)
{
	if (!dst || dst_size != PsyX_VRAM_GetSize()) return -1;
	memcpy(dst, vram, dst_size);
	return 0;
}

extern "C" int PsyX_VRAM_Restore(const void* src, unsigned int src_size)
{
	if (!src || src_size != PsyX_VRAM_GetSize()) return -1;
	memcpy(vram, src, src_size);
	vram_need_update = 1;
	return 0;
}

void GR_ResetDevice()
{
	GR_UpdateSwapIntervalState(0);
}

typedef struct
{
	// shader itself
	ShaderID shader;

#if USE_OPENGL
	GLint projectionLoc;
	GLint projection3DLoc;
	GLint bilinearFilterLoc;
	GLint texelSizeLoc;
	GLint pgxpZBiasLoc;   /* Day 3 — per-split Z bias for PGXP-Z mode */
#endif
} GTEShader;

GTEShader g_gte_shader_4;
GTEShader g_gte_shader_8;
GTEShader g_gte_shader_16;
GTEShader g_gte_shader_32_rgba;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_bilinearFilterLoc;
GLint u_texelSizeLoc;
GLint u_pgxpZBiasLoc;

#define GPU_PACK_RG\
	"		float color_16 = (color_rg.y * 256.0 + color_rg.x) * 255.0;\n"

#define GPU_DISCARD\
	"		if (color_16 == 0.0) { discard; }\n"

#define GPU_DECODE_RG\
	"		fragColor = fract(floor(color_16 / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);\n"

#define GPU_PACK_RG_FUNC\
	"	const float c_PackRange = 255.001;\n"\
	"	float packRG(vec2 rg) { return (rg.y * 256.0 + rg.x) * c_PackRange;}\n"

#define GPU_DECODE_RG_FUNC\
	" vec4 decodeRG(float rg) {\n"\
	" 	vec4 value = fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);\n"\
	" 	return vec4(value.xyz, rg == 0.0 ? rg : (1.0 - value.w * 16.0));\n"\
	" }\n"
	//"	vec4 decodeRG(float rg) { return fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0); }\n"

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 4.0) * 4.0);\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * v_texcoord.w);\n"

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { return array[idx]; }"

#else

#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { if(idx == 0) return array.x; else return array.y; }"

#endif

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC\
    "   // returns 16 bit colour\n"\
    "   float samplePSX(vec2 tc){\n"\
    "       vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
    "       vec2 comp = VRAM(uv);\n"\
    "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"\
    "       float v = _idx2(comp, index / 2) * (c_PackRange / 16.0);\n"\
    "       float f = floor(v);\n"\
    "       vec2 c = vec2( (v - f) * 16.0, f );\n"\
    "       vec2 clut_pos = v_page_clut.zw;\n"\
    "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;\n"\
    "       return packRG(VRAM(clut_pos));\n"\
    "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC\
	"	// returns 16 bit colour\n"\
	"	float samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 comp = VRAM(uv);\n"\
	"		vec2 clut_pos = v_page_clut.zw;\n"\
	"		int index = int(mod(tc.x, 2.0));\n"\
	"		clut_pos.x += _idx2(comp, index) * c_PackRange * c_VRAMTexel.x;\n"\
	"		vec2 color_rg = VRAM(clut_pos);\n"\
	"		return packRG(VRAM(clut_pos));\n"\
	"	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC\
	"	float samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 color_rg = VRAM(uv);\n"\
	"		return packRG(color_rg);\n"\
	"	}\n"


#define GPU_BILINEAR_SAMPLE_FUNC \
	"	float c_textureSize = 1.0;\n"\
	"	float c_onePixel = 1.0;\n"\
	"	vec4 BilinearTextureSample(vec2 P) {\n"\
	"		vec2 frac = fract(P);\n"\
	"		vec2 pixel = floor(P);\n"\
	"		float C11 = samplePSX(pixel);\n"\
	"		float C21 = samplePSX(pixel + vec2(c_onePixel, 0.0));\n"\
	"		float C12 = samplePSX(pixel + vec2(0.0, c_onePixel));\n"\
	"		float C22 = samplePSX(pixel + vec2(c_onePixel, c_onePixel));\n"\
	"		float ax1 = mix(float(C11 > 0.0), float(C21 > 0.0), frac.x);\n"\
	"		float ax2 = mix(float(C12 > 0.0), float(C22 > 0.0), frac.x);\n"\
	"		if(mix(ax1, ax2, frac.y) < 0.5) { discard; }\n"\
	"		vec4 x1 = mix(decodeRG(C11), decodeRG(C21), frac.x);\n"\
	"		vec4 x2 = mix(decodeRG(C12), decodeRG(C22), frac.x);\n"\
	"		return mix(x1, x2, frac.y);\n"\
	"	}\n"

#define GPU_NEAREST_SAMPLE_FUNC \
	"vec4 NearestTextureSample(vec2 P) {\n"\
	"	float color_16 = samplePSX(P);\n"\
	"	if(color_16 == 0.0) {discard;}\n"\
	"	return decodeRG(color_16);\n"\
	"}\n"

#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).ra; }\n"
#else
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).rg; }\n"
#endif

#if USE_PGXP
/* Day 4 Phase 2: perspective branch uses the NDC-times-w composition.
 * Ortho `clip = Projection * (px, py, z, 1)` produces (ndc.x, ndc.y, ndc.z, 1).
 * For perspective-correct UV/attribute interpolation we want gl_Position.w = w
 * with screen position preserved. Premultiply ortho clip-space xy/z by w and set
 * gl_Position.w = w; perspective divide on the GPU collapses back to the same NDC
 * coords (sub-pixel-exact) while attribute interpolation becomes hyperbolic.
 * The Day 3 per-split bias rides through unchanged because it's applied inside
 * pgxp_z before Projection. Non-canonical (ofx, ofy) emit sets scr_h = 0 in the
 * shim and stays on the ortho path, where Day 1c's vertex stability is already
 * verified. */
#define GTE_PERSPECTIVE_CORRECTION \
		"	float pgxp_z = u_pgxp_z_bias + a_zw.x;\n" \
		"	vec4 ortho_clip = Projection * vec4(a_position.xy, pgxp_z, 1.0);\n" \
		"	float w = a_zw.y;\n" \
		"	gl_Position = (w > 0.5) ? vec4(ortho_clip.xyz * w, w) : ortho_clip;\n"
#else
#define GTE_PERSPECTIVE_CORRECTION \
		"	gl_Position = Projection * vec4(a_position.xy, 0.0, 1.0);\n"
#endif

#define GTE_VERTEX_SHADER \
	"	attribute vec4 a_position;\n"\
	"	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"\
	"	attribute vec4 a_color;\n"\
	"	attribute vec4 a_extra; // texcoord.xy ofs, unused.xy\n"\
	"	attribute vec4 a_zw;\n"\
	"	uniform mat4 Projection;\n"\
	"	uniform mat4 Projection3D;\n"\
	"	uniform float u_pgxp_z_bias;\n"\
	"	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"\
	"	void main() {\n"\
	"		v_texcoord = a_texcoord;\n"\
	"		v_texcoord.xy += a_extra.xy * 0.5;\n"\
	"		v_color = a_color;\n"\
	"		v_color.xyz *= a_texcoord.z;\n"\
	"		v_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"\
	"		v_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"\
	"		v_page_clut.z = fract(a_position.w / 64.0);\n"\
	"		v_page_clut.w = floor(a_position.w / 64.0) / 512.0;\n"\
	"		v_page_clut.xy += c_UVFudge;\n"\
	"		v_page_clut.zw += c_UVFudge;\n"\
	GTE_PERSPECTIVE_CORRECTION\
	"		v_z = (gl_Position.z - 40.0) * 0.005;\n"\
	"	}\n"

#define GPU_FRAGMENT_SAMPLE_SHADER(bit) \
	GPU_PACK_RG_FUNC\
	GPU_DECODE_RG_FUNC\
	GPU_FETCH_VRAM_FUNC\
	"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"\
	GPU_ARRAY_FUNC\
	GPU_SAMPLE_TEXTURE_## bit ##BIT_FUNC\
	GPU_BILINEAR_SAMPLE_FUNC\
	GPU_NEAREST_SAMPLE_FUNC\
	"	uniform int bilinearFilter;\n"\
	"	void main() {\n"\
	"		if(bilinearFilter > 0)\n"\
	"			fragColor = BilinearTextureSample(v_texcoord.xy);\n"\
	"		else\n"\
	"			fragColor = NearestTextureSample(v_texcoord.xy);\n"\
	"		\n"\
	GPU_DITHERING\
	"	}\n"

const char* gte_shader_4 =
	"varying vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"varying vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(4)
	"#endif\n";

const char* gte_shader_8 =
	"varying vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"varying vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(8)
	"#endif\n";

const char* gte_shader_16 =
	"varying vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"varying vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(16)
	"#endif\n";

const char* gte_shader_32_rgba = 
	"varying vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"varying vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	"	uniform sampler2D s_texture;\n"\
	"	uniform int bilinearFilter;\n"\
	"	uniform vec2 texelSize;\n"\
	"	void main() {\n"\
	"		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"\
	"		fragColor = texture2D(s_texture, tc);\n"\
	GPU_DITHERING\
	"	}\n"
	"#endif\n";

int GR_Shader_CheckShaderStatus(GLuint shader)
{
	char info[1024];
	GLint result;

	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	if (result == GL_TRUE)
		return 1;
	
	glGetShaderInfoLog(shader, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program)
{
	char info[1024];
	GLint result;

	glGetProgramiv(program, GL_LINK_STATUS, &result);

	if (result == GL_TRUE)
		return 1;

	glGetProgramInfoLog(program, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

ShaderID GR_Shader_Compile(const char* source)
{
#if defined(ES2_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 100\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n";

	const char* GLSL_HEADER_FRAG =
		"#version 100\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define fragColor gl_FragColor\n";
#elif defined(ES3_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 300 es\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 300 es\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#else
	const char* GLSL_HEADER_VERT =
		"#version 140\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 140\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#endif

	char extra_vs_defines[1024];
	char extra_fs_defines[1024];
	extra_vs_defines[0] = 0;
	extra_fs_defines[0] = 0;

	if (g_cfg_bilinearFiltering)
	{
		strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
	}

	const char* vs_list[] = { GLSL_HEADER_VERT, extra_vs_defines, source };
	const char* fs_list[] = { GLSL_HEADER_FRAG, extra_fs_defines, source };

	GLuint program = glCreateProgram();

	{
		GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vertexShader, 3, vs_list, NULL);
		glCompileShader(vertexShader);

		if( GR_Shader_CheckShaderStatus(vertexShader) == 0 )
			eprinterr("Failed to compile Vertex Shader!\n");
	
		glAttachShader(program, vertexShader);
		glDeleteShader(vertexShader);
	}

	{
		GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fragmentShader, 3, fs_list, NULL);
		glCompileShader(fragmentShader);

		if(GR_Shader_CheckShaderStatus(fragmentShader) == 0)
			eprinterr("Failed to compile Fragment Shader!\n");
	
		glAttachShader(program, fragmentShader);
		glDeleteShader(fragmentShader);
	}

	glBindAttribLocation(program, a_position, "a_position");
	glBindAttribLocation(program, a_texcoord, "a_texcoord");
	glBindAttribLocation(program, a_color, "a_color");

#if USE_PGXP
	glBindAttribLocation(program, a_zw, "a_zw");
#endif

	glLinkProgram(program);
	if(GR_Shader_CheckProgramStatus(program) == 0)
		eprinterr("Failed to link Shader!\n");

	GLint sampler = 0;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
	glUseProgram(0);

	return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

void GR_GenerateCommonTextures()
{
	unsigned int pixelData = 0xFFFFFFFF;

#if USE_OPENGL
	glGenTextures(1, &g_whiteTexture);
	{
		glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixelData);

		glBindTexture(GL_TEXTURE_2D, 0);
	}
#endif
}

TextureID GR_CreateRGBATexture(int width, int height, u_char* data /*= nullptr*/)
{
	TextureID newTexture;
	glGenTextures(1, &newTexture);

	glBindTexture(GL_TEXTURE_2D, newTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	
	// another WebGL stuff. Texture will be black without clamp to edge
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_2D, 0);

	return newTexture;
}

void GR_CompilePSXShader(GTEShader* sh, const char* source)
{
	sh->shader = GR_Shader_Compile(source);

#if USE_OPENGL
	
	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
#if USE_PGXP
	sh->projection3DLoc = glGetUniformLocation(sh->shader, "Projection3D");
	sh->pgxpZBiasLoc = glGetUniformLocation(sh->shader, "u_pgxp_z_bias");
#endif
#endif
}

void GR_InitialisePSXShaders()
{
	GR_CompilePSXShader(&g_gte_shader_4, gte_shader_4);
	GR_CompilePSXShader(&g_gte_shader_8, gte_shader_8);
	GR_CompilePSXShader(&g_gte_shader_16, gte_shader_16);
	GR_CompilePSXShader(&g_gte_shader_32_rgba, gte_shader_32_rgba);
}

int GR_InitialisePSX()
{
	SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
	GR_GenerateCommonTextures();
	GR_InitialisePSXShaders();

#if USE_OPENGL
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
	glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

	// gen framebuffer
	{
		memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
		PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// make a special texture
		// it will be resized later
		glGenTextures(1, &g_fbTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_fbTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, PSYX_RT_COLOR_INTERNALFORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glBlitFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen offscreen RT
	{
		memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
		PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// offscreen texture render target
		glGenTextures(1, &g_offscreenRTTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, PSYX_RT_COLOR_INTERNALFORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glOffscreenFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_offscreenRTTexture, 0);
			/* Day 3 PGXP-Z-buffer: depth-stencil RB so depth_test (with
			 * per-split bias dominating + intra-batch real-Z perturbation
			 * tiebreaking) works on the offscreen render target. */
			glGenRenderbuffers(1, &g_glOffscreenDepthStencilRBO);
			glBindRenderbuffer(GL_RENDERBUFFER, g_glOffscreenDepthStencilRBO);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, VRAM_WIDTH, VRAM_HEIGHT);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_glOffscreenDepthStencilRBO);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen VRAM textures.
	// double-buffered
	{
		int i;

		glGenTextures(2, g_vramTexturesDouble);

		for(i = 0; i < 2; i++)
		{
			glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// set storage size
			glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
		}

		g_vramTexture = g_vramTexturesDouble[0];

		glBindTexture(GL_TEXTURE_2D, 0);

		// VRAM framebuffer for offscreen blitting to VRAM
		glGenFramebuffers(1, &g_glVRAMFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen vertex buffer and index buffer
	{
		int i;

		glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
		glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

		for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
		{
			glBindVertexArray(g_glVertexArray[i]);

			glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
		}

		glBindVertexArray(0);
	}
#else
#error
#endif

	GR_ResetDevice();

	return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);

	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);

#if USE_OPENGL 
	// -1..1
	float z = (znear + zfar) / (znear - zfar);
#endif

	float ortho[16] = {
		a, 0, 0, 0,
		0, b, 0, 0,
		0, 0, c, 0,
		x, y, z, 1
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar)
{
	float sinF, cosF;
	sinF = sinf(0.5f * fov);
	cosF = cosf(0.5f * fov);

	float h = cosF / sinF;
	float w = (h * height) / width;

	float persp[16] = {
		w, 0, 0, 0,
		0, h, 0, 0,
		0, 0, (zFar + zNear) / (zFar - zNear), 1,
		0, 0, -(2 * zFar * zNear) / (zFar - zNear), 0
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
#endif
}

void GR_SetupClipMode(const RECT16* rect, int enable)
{
	// [A] isinterlaced dirty hack for widescreen
	const bool scissorOn = enable && (activeDispEnv.isinter ||
		(	rect->x - activeDispEnv.disp.x > 0 ||
			rect->y - activeDispEnv.disp.y > 0 ||
			rect->w < activeDispEnv.disp.w - 1 ||
			rect->h < activeDispEnv.disp.h - 1));

	GR_SetScissorState(scissorOn);

	if (!scissorOn)
		return;

	// emuScreenAspect compensated the scissor for window-aspect under upstream
	// PsyCross's PGXP design (which embedded window aspect in the projection
	// matrix). openpete's GR_SetOffscreenState now uses content-based ortho
	// for both USE_PGXP states (with Hor+ widening via
	// g_psyx_render_aspect_factor), so the scissor must match: no
	// window-aspect compensation in either path.
	const float emuScreenAspect = 1.0f;

	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

	// first map to 0..1
	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;

	// then map to screen
	{
		clipRectX -= 0.5f;

		clipRectX *= emuScreenAspect;
		clipRectW *= emuScreenAspect;

		clipRectX += 0.5f;
	}

#if USE_OPENGL
	// Scissor is applied to the currently-bound draw target. When the present
	// FBO is in play it's typically taller than the window (overscan crop), so
	// scissor must be normalized against the FBO's pixel dimensions, not the
	// window's — otherwise it clips into the rendered scene at the wrong row.
	const float targetW = (float)(g_glPresentFramebuffer ? g_presentFbWidth  : g_windowWidth);
	const float targetH = (float)(g_glPresentFramebuffer ? g_presentFbHeight : g_windowHeight);
	const float flipOffset = targetH - clipRectH * targetH;
	const float crx = clipRectX * targetW;
	const float cry = clipRectY * targetH;
	const float crw = clipRectW * targetW;
	const float crh = clipRectH * targetH;

	/* Round float→GLint to nearest. Truncation here produced an off-by-one
	 * scissor for PSX rects whose normalized fraction × targetH lands just
	 * shy of an integer (e.g. 224/240·480 = 447.999…), shaving 1 px off
	 * the bottom of the rendered scene and creating a 1-px clear line in
	 * the present-blit's output. */
	glScissor((GLint)(crx + 0.5f),
	          (GLint)(flipOffset - cry + 0.5f),
	          (GLint)(crw + 0.5f),
	          (GLint)(crh + 0.5f));
#endif
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect)
{
#if USE_PGXP
	float psxScreenW, psxScreenH;
	float emuScreenAspect;

	emuScreenAspect = (float)(g_windowWidth) / (float)(g_windowHeight);

	psxScreenW = activeDispEnv.disp.w;
	psxScreenH = activeDispEnv.disp.h;

	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;

	rect->w = psxScreenW * emuScreenAspect * PSX_SCREEN_ASPECT; // windowWidth;
	rect->h = psxScreenH; // windowHeight;

	rect->x -= (rect->w - activeDispEnv.disp.w) / 2;

	rect->w += rect->x;
#else
	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;
	rect->w = activeDispEnv.disp.w;
	rect->h = activeDispEnv.disp.h;
#endif
}

void GR_SetShader(const ShaderID shader)
{
	if (g_PreviousShader != shader)
	{
#if USE_OPENGL
		glUseProgram(shader);
#else
#error
#endif

		g_PreviousShader = shader;
	}
}


void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	switch (texFormat)
	{
	case TF_4_BIT:
		GR_SetShader(g_gte_shader_4.shader);
		u_bilinearFilterLoc = g_gte_shader_4.bilinearFilterLoc;
		u_projectionLoc = g_gte_shader_4.projectionLoc;
		u_projection3DLoc = g_gte_shader_4.projection3DLoc;
		u_pgxpZBiasLoc = g_gte_shader_4.pgxpZBiasLoc;
		u_texelSizeLoc = -1;
		break;
	case TF_8_BIT:
		GR_SetShader(g_gte_shader_8.shader);
		u_bilinearFilterLoc = g_gte_shader_8.bilinearFilterLoc;
		u_projectionLoc = g_gte_shader_8.projectionLoc;
		u_projection3DLoc = g_gte_shader_8.projection3DLoc;
		u_pgxpZBiasLoc = g_gte_shader_8.pgxpZBiasLoc;
		u_texelSizeLoc = -1;
		break;
	case TF_16_BIT:
		GR_SetShader(g_gte_shader_16.shader);
		u_bilinearFilterLoc = g_gte_shader_16.bilinearFilterLoc;
		u_projectionLoc = g_gte_shader_16.projectionLoc;
		u_projection3DLoc = g_gte_shader_16.projection3DLoc;
		u_pgxpZBiasLoc = g_gte_shader_16.pgxpZBiasLoc;
		u_texelSizeLoc = -1;
		break;
	case TF_32_BIT_RGBA:
		GR_SetShader(g_gte_shader_32_rgba.shader);
		u_bilinearFilterLoc = -1;
		u_projectionLoc = g_gte_shader_32_rgba.projectionLoc;
		u_projection3DLoc = g_gte_shader_32_rgba.projection3DLoc;
		u_pgxpZBiasLoc = g_gte_shader_32_rgba.pgxpZBiasLoc;
		u_texelSizeLoc = g_gte_shader_32_rgba.texelSizeLoc;
		break;
	}

	if (g_dbg_texturelessMode) {
		texture = g_whiteTexture;
	}

	if (g_lastBoundTexture == texture) {
		return;
	}

#if USE_OPENGL
	glBindTexture(GL_TEXTURE_2D, texture);
	if(u_bilinearFilterLoc != -1)
		glUniform1i(u_bilinearFilterLoc, g_cfg_bilinearFiltering);

#endif

	g_lastBoundTexture = texture;
}

void GR_SetOverrideTextureSize(int width, int height)
{
	if(u_texelSizeLoc == -1)
		return;

	// WebGL is fucking around with glUniform2f, so use vector version
	float vec[] = { 1.0f / (float)width, 1.0f / (float)height };
	glUniform2fv(u_texelSizeLoc, 1, vec);
}

void GR_DestroyTexture(TextureID texture)
{
	if (texture == -1)
		return;

#if USE_OPENGL
	glDeleteTextures(1, &texture);
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	vram_need_update = 1;

	u_short* dst = vram + x + y * VRAM_WIDTH;

	if (x + w > VRAM_WIDTH)
		w = VRAM_WIDTH - x;

	if (y + h > VRAM_HEIGHT)
		h = VRAM_HEIGHT - y;

	// clear VRAM region with given color
	for (int i = 0; i < h; i++)
	{
		u_short* tmp = dst;

		for (int j = 0; j < w; j++)
			*tmp++ = r | (g << 5) | (b << 11);

		dst += VRAM_WIDTH;
	}
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	framebuffer_need_update = 1;

#if USE_OPENGL
	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
}

void GR_SaveVRAM(const char* outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer)
{
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

	FILE* fp = fopen(outputFileName, "wb");
	if (fp == NULL)
		return;

	unsigned char TGAheader[12] = { 0,0,2,0,0,0,0,0,0,0,0,0 };
	unsigned char header[6];
	header[0] = (width % 256);
	header[1] = (width / 256);
	header[2] = (height % 256);
	header[3] = (height / 256);
	header[4] = 16;
	header[5] = 0;

	fwrite(TGAheader, sizeof(unsigned char), 12, fp);
	fwrite(header, sizeof(unsigned char), 6, fp);

	for (int i = 0; i < VRAM_HEIGHT; i++)
	{
		fwrite(vram + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
	}

	fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int* src, int x, int y, int w, int h, int update_vram, int flip_y)
{
	/* OpenPete: clamp + warn instead of assert. We've seen activeDispEnv.disp.x
	 * go negative after stub-misses inside untranslated overlays — keeping the
	 * renderer alive lets us continue past those frames and capture the value. */
	if (x < 0 || y < 0 || x + w > VRAM_WIDTH || y + h > VRAM_WIDTH || w <= 0 || h <= 0) {
		static int warned_count = 0;
		if (warned_count < 6) {
			++warned_count;
			fprintf(stderr, "[PsyX] GR_CopyRGBAFramebufferToVRAM bad rect (x=%d y=%d w=%d h=%d) — skipping\n", x, y, w, h);
		}
		return;
	}

	ushort* fb = (ushort*)malloc(w * h * sizeof(ushort));
	uint* data_src = (uint*)src;
	ushort* data_dst = (ushort*)fb;

	for (int i = 0; i < h; i++)
	{
		for (int j = 0; j < w; j++)
		{
			uint c = *data_src++;

			u_char b = ((c >> 3) & 0x1F);
			u_char g = ((c >> 11) & 0x1F);
			u_char r = ((c >> 19) & 0x1F);
			//u_char a = ((c >> 24) & 0x1F);

			int a = r == g == b == 0 ? 0 : 1;

			*data_dst++ = r | (g << 5) | (b << 10) | (a << 15);
		}
	}

	ushort* ptr = (ushort*)vram + VRAM_WIDTH * y + x;

	for (int fy = 0; fy < h; fy++)
	{
		int py = flip_y ? (h - fy - 1) : fy;
		ushort* fb_ptr = fb + (h * py / h) * w;

		for (int fx = 0; fx < w; fx++)
			ptr[fx] = fb_ptr[w * fx / w];

		ptr += VRAM_WIDTH;
	}

	free(fb);

	if (update_vram)
		vram_need_update = 1;
}

void GR_ReadFramebufferDataToVRAM()
{
	int x, y, w, h;
	if (!framebuffer_need_update)
		return;

	framebuffer_need_update = 0;

	x = g_PreviousFramebuffer.x;
	y = g_PreviousFramebuffer.y;
	w = g_PreviousFramebuffer.w;
	h = g_PreviousFramebuffer.h;

	// now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
	// read the texture
	if(g_glFramebufferPBO.pixels)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		PBO_Download(&g_glFramebufferPBO);
		glBindTexture(GL_TEXTURE_2D, 0);
		GR_CopyRGBAFramebufferToVRAM((u_int*)g_glFramebufferPBO.pixels, x, y, w, h, 0, 0);
	}
#endif
}

void GR_SetScissorState(int enable)
{
	if (g_PreviousScissorState == enable)
		return;

#if USE_OPENGL
	if (g_PreviousScissorState)
		glDisable(GL_SCISSOR_TEST);
	else
		glEnable(GL_SCISSOR_TEST);
#endif
	g_PreviousScissorState = enable;
}

void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	if (enable)
	{
		// setup render target viewport
#if USE_PGXP
		GR_Ortho2D(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
#else
		GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
#endif
	}
	else
	{
		// setup default viewport
#if USE_PGXP

		// these constants below are guessed
		const float perspectiveFOV = 0.9265f;
		const float perspectiveZNear = 0.25f;
		const float perspectiveZFar = 1000.0f;

		// PGXP-on ortho mirrors the off-path's content-based mapping.
		// Vertex coords have already been normalized by disp.w via
		// ScreenCoordsToEmulator (vertex.x = raw/disp.w - 0.5), so the
		// horizontal extent runs [-0.5, g_psyx_render_aspect_factor - 0.5]
		// when Hor+ widening is active. Map that to NDC [-1, +1] so the
		// cache-miss fallback renders identically to USE_PGXP=0 — gameplay
		// state must be presentation-independent. Window-aspect handling
		// (pillarbox / letterbox) is owned by the GL viewport, not the
		// projection matrix; mixing the two is what made widescreen look
		// like 4:3 with pillarboxing under the upstream PsyCross formula.
		const float emuScreenAspect = (float)(g_windowWidth) / (float)(g_windowHeight);
		const float horExtent = g_psyx_render_aspect_factor;

		GR_Ortho2D(-0.5f, horExtent - 0.5f, 0.5f, -0.5f, -1.0f, 1.0f);
		GR_Perspective3D(perspectiveFOV, 1.0f, 1.0f / (emuScreenAspect * PSX_SCREEN_ASPECT), perspectiveZNear, perspectiveZFar);
#else
		/* Hor+ widescreen: scale the GL ortho width by the host's
		 * runtime aspect factor so geometry the GTE projects past
		 * disp.w (because the host widened SetGeomOffset.x) is mapped
		 * across the full SDL window instead of being clipped.
		 * VRAM-facing operations (GR_StoreFrameBuffer, ClearImage,
		 * etc.) keep using activeDispEnv.disp.w / activeDrawEnv.clip.w
		 * untouched, so texture pages at x>=disp.w are unaffected. */
		GR_Ortho2D(0,
			(float)activeDispEnv.disp.w * g_psyx_render_aspect_factor,
			activeDispEnv.disp.h, 0, -1.0f, 1.0f);
#endif
	}

	if (g_PreviousOffscreenState == enable)
		return;

	g_PreviousOffscreenState = enable;

#if USE_OPENGL
	if (enable)
	{
		// set storage size first
		if (g_PreviousOffscreen.w != offscreenRect->w &&
			g_PreviousOffscreen.h != offscreenRect->h)
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, PSYX_RT_COLOR_INTERNALFORMAT, offscreenRect->w, offscreenRect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		g_PreviousOffscreen = *offscreenRect;

		GR_SetViewPort(0, 0, offscreenRect->w, offscreenRect->h);
		glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

		// clear it out
		glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		/* Restoring the scene viewport: the FBO (when overscan crop is active)
		 * is taller than the window. */
		if (g_glPresentFramebuffer)
			GR_SetViewPort(0, 0, g_presentFbWidth, g_presentFbHeight);
		else
			GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

#if USE_OFFSCREEN_BLIT
		// before drawing set source and target
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			// rebind texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

			// setup draw and read framebuffers
			glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);					// source is backbuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

			glBlitFramebuffer(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h, 
								g_PreviousOffscreen.x, g_PreviousOffscreen.y + g_PreviousOffscreen.h, g_PreviousOffscreen.x + g_PreviousOffscreen.w, g_PreviousOffscreen.y,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);

			// done, unbind
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		}
#endif
		
		/* OpenPete: scene rendering targets the present FBO, not default fb 0. */
		glBindFramebuffer(GL_FRAMEBUFFER, g_glPresentFramebuffer ? g_glPresentFramebuffer : 0);
		// copy rendering results to VRAM texture
		{
			// reat the texture
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			//glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			PBO_Download(&g_glOffscreenPBO);
			glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

			// Don't forcely update VRAM
			GR_CopyRGBAFramebufferToVRAM((u_int*)g_glOffscreenPBO.pixels, 
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, g_PreviousOffscreen.w, g_PreviousOffscreen.h, 
				USE_OFFSCREEN_BLIT == 0, 1);
		}

	}
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h)
{
#if USE_OPENGL
	// set storage size first
	if (g_PreviousFramebuffer.w != w ||
		g_PreviousFramebuffer.h != h)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, PSYX_RT_COLOR_INTERNALFORMAT, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	g_PreviousFramebuffer.x = x;
	g_PreviousFramebuffer.y = y;
	g_PreviousFramebuffer.w = w;
	g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
	glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

	// before drawing set source and target
	{
		/* OpenPete: read from the present FBO (where the scene was rendered)
		 * rather than default fb 0 (which only carries the cropped scene + overlay). */
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glPresentFramebuffer ? g_glPresentFramebuffer : 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

		int src_w = g_glPresentFramebuffer ? g_presentFbWidth  : g_windowWidth;
		int src_h = g_glPresentFramebuffer ? g_presentFbHeight : g_windowHeight;
		glBlitFramebuffer(0, 0, src_w, src_h, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Blit framebuffer to VRAM screen area

		// before drawing set source and target
		glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

		// rebind vram texture
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

		glBlitFramebuffer(0, 0, w, h,
			x, y + h, x + w, y,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		
		// done, unbind
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	}

	// after drawing
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();
#endif

	GR_ReadFramebufferDataToVRAM();
#endif
}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	vram_need_update = 1;

	int stride = w;

	if (!src)
	{
		framebuffer_need_update = 1;

		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;

	unsigned short* dst = vram + dst_x + dst_y * VRAM_WIDTH;

	for (int i = 0; i < h; i++) {
		SDL_memcpy(dst, src, w * sizeof(short));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	unsigned short* src = vram + x + VRAM_WIDTH * y;

	for (int i = 0; i < dst_h; i++) {
		SDL_memcpy(dst, src, dst_w * sizeof(short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_UpdateVRAM()
{
	if (!vram_need_update)
		return;

	vram_need_update = 0;

#if USE_OPENGL
	g_vramTexture = g_vramTexturesDouble[g_vramTextureIdx];
	g_vramTextureIdx++;
	g_vramTextureIdx &= 1;

	glBindTexture(GL_TEXTURE_2D, g_vramTexture);

#if defined(RENDERER_OGL)
	glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#else
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#endif

#endif
}

void GR_SwapWindow()
{
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	SDL_GL_SwapWindow(g_window);
#endif

	//glFinish();
}

void GR_EnableDepth(int enable)
{
	if (g_PreviousDepthMode == enable)
		return;

	g_PreviousDepthMode = enable;

#if USE_OPENGL
	if (enable && g_cfg_pgxpZBuffer)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
#endif
}

void GR_SetStencilMode(int drawPrim)
{
	if (g_PreviousStencilMode == drawPrim)
		return;

	g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
	if (drawPrim)
	{
		glStencilFunc(GL_ALWAYS, 1, 0x10);
		glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
	}
	else
	{
		glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
	}
#endif
}

void GR_SetBlendMode(BlendMode blendMode)
{
	if (g_PreviousBlendMode == blendMode)
		return;

#if USE_OPENGL
	if (blendMode == BM_NONE)
	{
		if (g_PreviousBlendMode != BM_NONE)
		{
			glBlendColor(1.f, 1.f, 1.f, 1.f);
			glDisable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(1);
		return;
	}
	else
	{
		if(g_PreviousBlendMode == BM_NONE)
		{
			glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
			glEnable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(0);
	}

	glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
	switch (blendMode) {
	case BM_AVERAGE:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case BM_ADD:
	case BM_SUBTRACT:
		glBlendFunc(GL_ONE, GL_ONE);
		break;
	case BM_ADD_QUATER_SOURCE:
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE); 
		break;
	}
#endif

	g_PreviousBlendMode = blendMode;
}

void GR_SetPolygonOffset(float ofs)
{
#if USE_OPENGL
	if (ofs == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	else
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.0f, ofs);
	}
#endif
}

void GR_SetViewPort(int x, int y, int width, int height)
{
#if USE_OPENGL
	glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable)
{
#if defined(RENDERER_OGL)
	glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer()
{
#if USE_OPENGL
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);

	glEnableVertexAttribArray(a_position);
	glEnableVertexAttribArray(a_texcoord);
	glEnableVertexAttribArray(a_color);
	glEnableVertexAttribArray(a_extra);

#if USE_PGXP
	glVertexAttribPointer(a_position, 4, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
	glVertexAttribPointer(a_zw, 4, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->z);

	glEnableVertexAttribArray(a_zw);
#else
	glVertexAttribPointer(a_position, 4, GL_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
#endif
	glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->u);
	glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex), &((GrVertex*)NULL)->r);
	glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->tcx);

	g_curVertexBuffer++;
	g_curVertexBuffer &= 1;
#else
#error
#endif
}

void GR_UpdateVertexBuffer(const GrVertex* vertices, int num_vertices)
{
	if (num_vertices >= MAX_VERTEX_BUFFER_SIZE)
	{
		eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
		num_vertices = MAX_VERTEX_BUFFER_SIZE;
	}

	//assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
	GR_BindVertexBuffer();

#if USE_OPENGL
	glBufferSubData(GL_ARRAY_BUFFER, 0, num_vertices * sizeof(GrVertex), vertices);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
#if USE_OPENGL
	glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char* label)
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPushDebugGroup)
		return;
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel()
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPopDebugGroup)
		return;
	glPopDebugGroup();
#endif
}