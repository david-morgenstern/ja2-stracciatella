#include "Cursor_Control.h"
#include "Debug.h"
#include "FPS.h"
#include "HImage.h"
#include "Local.h"
#include "Logger.h"
#include "RenderWorld.h"
#include "Render_Dirty.h"
#include "Types.h"
#include "VObject_Blitters.h"
#include "VSurface.h"
#include "Video.h"
#include "Visualizer.h"
#include "UILayout.h"
#include "Icon.h"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#define MAX_CURSOR_WIDTH  64
#define MAX_CURSOR_HEIGHT 64

#define MAX_DIRTY_REGIONS 128

#define OVERSAMPLING_SCALE 4


// Globals for mouse cursor
static UINT16 gusMouseCursorWidth;
static UINT16 gusMouseCursorHeight;
static INT16  gsMouseCursorXOffset;
static INT16  gsMouseCursorYOffset;
INT16 gsMouseSizeYModifier = 0; // This can increase the size of gusMouseCursorHeight so image data (ie, text) outside the normal height of the mouse cursor can be copied onto screen buffer

static SDL_Rect MouseBackground = { 0, 0, 0, 0 };

// Dirty rectangle management variables
static SDL_Rect DirtyRegions[MAX_DIRTY_REGIONS];
static UINT32   guiDirtyRegionCount;
static BOOLEAN  gfForceFullScreenRefresh;


static SDL_Rect DirtyRegionsEx[MAX_DIRTY_REGIONS];
static UINT32   guiDirtyRegionExCount;


static SDL_Surface* MouseCursor;
static SDL_Surface* FrameBuffer;
SDL_Renderer*  GameRenderer;
SDL_Window* g_game_window;

static SDL_Surface* ScreenBuffer;
static SDL_Texture* ScreenTexture;
static SDL_Texture* ScaledScreenTexture;
static Uint32       g_window_flags = 0;
static VideoScaleQuality ScaleQuality = VideoScaleQuality::LINEAR;
static std::chrono::steady_clock::duration TimeBetweenRefreshScreens;

static void DeletePrimaryVideoSurfaces(void);

// the desktop size of a display in real pixels, which on a high DPI display
// is its logical size times the pixel density
static bool DisplayPixelSize(SDL_DisplayID const display, int& w, int& h)
{
	const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(display);
	if (!dm) return false;
	float const density = dm->pixel_density > 0 ? dm->pixel_density : 1.0f;
	w = static_cast<int>(std::lround(dm->w * density));
	h = static_cast<int>(std::lround(dm->h * density));
	return true;
}


// returns if desktop resolution larger game resolution
BOOLEAN IsDesktopLargeEnough()
{
	SDL_DisplayID const display = g_game_window ? SDL_GetDisplayForWindow(g_game_window) : SDL_GetPrimaryDisplay();
	int w, h;
	if (DisplayPixelSize(display, w, h))
	{
		if (w < SCREEN_WIDTH || h < SCREEN_HEIGHT)
		{
			return false;
		}
	}
	return true;
}


void VideoPickAutoResolution(UINT16& width, UINT16& height, bool const fullscreen)
{
	SDL_DisplayID const display = SDL_GetPrimaryDisplay();
	int w, h;
	bool ok = DisplayPixelSize(display, w, h);
	if (ok && !fullscreen)
	{
		// a window has to fit beside the taskbar and under its own title bar
		SDL_Rect usable;
		const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(display);
		if (dm && SDL_GetDisplayUsableBounds(display, &usable))
		{
			float const density = dm->pixel_density > 0 ? dm->pixel_density : 1.0f;
			int const title_bar = 40;
			int const side_borders = 16;
			w = static_cast<int>((usable.w - side_borders) * density);
			h = static_cast<int>((usable.h - title_bar) * density);
		}
	}
	if (!ok || w < 640 || h < 480)
	{
		width  = 640;
		height = 480;
		return;
	}
	int scale = std::max(1, static_cast<int>(std::lround(h / 720.0)));
	while (scale > 1 && (w / scale < 640 || h / scale < 480)) --scale;
	width  = static_cast<UINT16>(w / scale);
	height = static_cast<UINT16>(h / scale);
	SLOGI("Auto resolution: {}x{} at {}x for a {}x{} pixel display", width, height, scale, w, h);
}


void VideoSetWindowScale(int scale)
{
	if (!g_game_window) return;
	if (SDL_GetWindowFlags(g_game_window) & SDL_WINDOW_FULLSCREEN) return;

	float density = SDL_GetWindowPixelDensity(g_game_window);
	if (density <= 0) density = 1.0f;

	SDL_DisplayID const display = SDL_GetDisplayForWindow(g_game_window);
	SDL_Rect usable{ 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
	SDL_GetDisplayUsableBounds(display, &usable);
	int top = 0, left = 0, bottom = 0, right = 0;
	SDL_GetWindowBordersSize(g_game_window, &top, &left, &bottom, &right);
	if (top == 0) top = 40; // borders are not known before the window is shown

	int const avail_w = static_cast<int>((usable.w - left - right) * density);
	int const avail_h = static_cast<int>((usable.h - top - bottom) * density);
	int const max_scale = std::max(1, std::min(avail_w / SCREEN_WIDTH, avail_h / SCREEN_HEIGHT));
	if (scale <= 0 || scale > max_scale) scale = max_scale;

	int const w = static_cast<int>(std::ceil(SCREEN_WIDTH  * scale / density));
	int const h = static_cast<int>(std::ceil(SCREEN_HEIGHT * scale / density));
	SDL_SetWindowSize(g_game_window, w, h);
	SDL_SetWindowPosition(g_game_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
	SLOGI("Window scaled to {}x ({}x{} pixels, density {})", scale, SCREEN_WIDTH * scale, SCREEN_HEIGHT * scale, density);
}

void VideoSetFullScreen(const BOOLEAN enable)
{
	SDL_SetWindowFullscreen(g_game_window, enable);
	SDL_SetWindowMouseGrab(g_game_window, enable);
}

void VideoToggleFullScreen(void)
{
	VideoSetFullScreen(!(SDL_GetWindowFlags(g_game_window) & SDL_WINDOW_FULLSCREEN));
}

void VideoSetBrightness(float brightness)
{
	if (brightness < 0) return;

	if (ScreenTexture)        SDL_SetTextureColorModFloat(ScreenTexture, brightness, brightness, brightness);
	if (ScaledScreenTexture)  SDL_SetTextureColorModFloat(ScaledScreenTexture, brightness, brightness, brightness);
}


static void GetRGBDistribution();


void InitializeVideoManager(const VideoScaleQuality quality, const int32_t targetFPS)
{
	ScaleQuality = quality;
	g_window_flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

	g_game_window = SDL_CreateWindow(APPLICATION_NAME,
					SCREEN_WIDTH, SCREEN_HEIGHT,
					g_window_flags);

	GameRenderer = SDL_CreateRenderer(g_game_window, nullptr);
	SDL_SetRenderLogicalPresentation(GameRenderer, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);

	SurfaceUniquePtr windowIcon(SDL_CreateSurfaceFrom(
			gWindowIconData.width,
			gWindowIconData.height,
			SDL_PIXELFORMAT_ABGR8888,
			(void*)gWindowIconData.pixel_data,
			gWindowIconData.bytes_per_pixel*gWindowIconData.width
	));
	SDL_SetWindowIcon(g_game_window, windowIcon.get());

	// open at the largest whole-number scale the display has room for
	VideoSetWindowScale(0);


	ClippingRect.set(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

	ScreenBuffer = SDL_CreateSurface(
					SCREEN_WIDTH,
					SCREEN_HEIGHT,
					SDL_PIXELFORMAT_RGB565
	);

	if (ScreenBuffer == NULL) {
		SLOGE("SDL_CreateRGBSurface for ScreenBuffer failed: {}\n", SDL_GetError());
	}

	ScreenTexture = SDL_CreateTexture(GameRenderer,
					SDL_PIXELFORMAT_RGB565,
					SDL_TEXTUREACCESS_STREAMING,
					SCREEN_WIDTH, SCREEN_HEIGHT);

	if (ScreenTexture == NULL) {
		SLOGE("SDL_CreateTexture for ScreenTexture failed: {}\n", SDL_GetError());
	}


	if (ScaleQuality == VideoScaleQuality::PERFECT)
	{
		if (!IsDesktopLargeEnough())
		{
			// Pixel-perfect mode cannot handle scaling down, and will
			// result in a empty black screen if the window size is
			// smaller than logical render resolution.
			throw std::runtime_error("Game resolution must not be larger than desktop size. "
				"Please reduce game resolution or choose another scaling mode.");
		}
		SDL_SetWindowMinimumSize(g_game_window, SCREEN_WIDTH, SCREEN_HEIGHT);
		SDL_SetTextureScaleMode(ScreenTexture, SDL_SCALEMODE_NEAREST);
		SDL_SetRenderLogicalPresentation(GameRenderer, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
	}
	else if (ScaleQuality == VideoScaleQuality::NEAR_PERFECT)
	{
		SDL_SetTextureScaleMode(ScreenTexture, SDL_SCALEMODE_NEAREST);
		ScaledScreenTexture = SDL_CreateTexture(GameRenderer,
			SDL_PIXELFORMAT_RGB565,
			SDL_TEXTUREACCESS_TARGET,
			SCREEN_WIDTH * OVERSAMPLING_SCALE, SCREEN_HEIGHT * OVERSAMPLING_SCALE);

		if (ScaledScreenTexture == NULL)
		{
			SLOGE("SDL_CreateTexture for ScaledScreenTexture failed: {}\n", SDL_GetError());
		}
	}
	else
	{
		SDL_SetTextureScaleMode(ScreenTexture, SDL_SCALEMODE_LINEAR);
	}

	FrameBuffer = SDL_CreateSurface(
		SCREEN_WIDTH,
		SCREEN_HEIGHT,
		SDL_PIXELFORMAT_RGB565
	);

	if (FrameBuffer == NULL)
	{
		SLOGE("SDL_CreateRGBSurface for FrameBuffer failed: {}\n", SDL_GetError());
	}

	MouseCursor = SDL_CreateSurface(
		MAX_CURSOR_WIDTH,
		MAX_CURSOR_HEIGHT,
		SDL_PIXELFORMAT_RGB565
	);
	SDL_SetSurfaceColorKey(MouseCursor, true, 0);

	if (MouseCursor == NULL)
	{
		SLOGE("SDL_CreateRGBSurface for MouseCursor failed: {}\n", SDL_GetError());
	}

	SDL_HideCursor();

	// Initialize state variables
	gfForceFullScreenRefresh = TRUE;

	// This function must be called to setup RGB information
	GetRGBDistribution();

	TimeBetweenRefreshScreens = std::chrono::microseconds{1'000'000} / targetFPS;
}


void ShutdownVideoManager(void)
{
	// ScreenBuffer SDL surface freed by its SGPVSurface wrapper.
	ScreenBuffer = nullptr;

	if (ScreenTexture != NULL) {
		SDL_DestroyTexture(ScreenTexture);
		ScreenTexture = NULL;
	}

	if (ScaledScreenTexture != NULL) {
		SDL_DestroyTexture(ScaledScreenTexture);
		ScaledScreenTexture = NULL;
	}

	if (GameRenderer != NULL) {
		SDL_DestroyRenderer(GameRenderer);
		GameRenderer = NULL;
	}

	if (g_game_window != NULL) {
		SDL_DestroyWindow(g_game_window);
		g_game_window = NULL;
	}

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}


namespace {
void AddToGivenRegionsList(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom,
	decltype(DirtyRegions) & regions,
	decltype(guiDirtyRegionCount) & regionCount)
{
	if (gfForceFullScreenRefresh)
	{
		// There's no point in going on since we are forcing a full screen refresh
		return;
	}

	if (regionCount < MAX_DIRTY_REGIONS)
	{
		// Well we haven't broken the MAX_DIRTY_REGIONS limit yet, so we register the new region

		// DO SOME PREMIMARY CHECKS FOR VALID RECTS
		if (iLeft < 0) iLeft = 0;
		if (iTop  < 0) iTop  = 0;

		if (iRight  > SCREEN_WIDTH)  iRight  = SCREEN_WIDTH;
		if (iBottom > SCREEN_HEIGHT) iBottom = SCREEN_HEIGHT;

		if (iRight - iLeft <= 0) return;
		if (iBottom - iTop <= 0) return;

		auto & newRegion{ regions[regionCount] };

		newRegion.x = iLeft;
		newRegion.y = iTop;
		newRegion.w = iRight  - iLeft;
		newRegion.h = iBottom - iTop;
		++regionCount;
	}
	else
	{
		// The MAX_DIRTY_REGIONS limit has been exceeded. Therefore we arbitrarely invalidate the entire
		// screen and force a full screen refresh
		InvalidateScreen();
	}
}

void AddRegionEx(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom)
{
	AddToGivenRegionsList(iLeft, iTop, iRight, iBottom, DirtyRegionsEx, guiDirtyRegionExCount);
}
}

void InvalidateRegion(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom)
{
	AddToGivenRegionsList(iLeft, iTop, iRight, iBottom, DirtyRegions, guiDirtyRegionCount);
}



void InvalidateRegionEx(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom)
{
	if (gfForceFullScreenRefresh)
	{
		// There's no point in going on since we are forcing a full screen refresh
		return;
	}

	// Check if we are spanning the rectangle - if so slit it up!
	if (iTop <= gsVIEWPORT_WINDOW_END_Y && iBottom > gsVIEWPORT_WINDOW_END_Y)
	{
		// Add new top region
		AddRegionEx(iLeft, iTop, iRight, gsVIEWPORT_WINDOW_END_Y);

		// Add new bottom region
		AddRegionEx(iLeft, gsVIEWPORT_WINDOW_END_Y, iRight, iBottom);
	}
	else
	{
		AddRegionEx(iLeft, iTop, iRight, iBottom);
	}
}


void InvalidateScreen(void)
{
	guiDirtyRegionCount = 0;
	guiDirtyRegionExCount = 0;
	gfForceFullScreenRefresh = TRUE;
}


/* Clip an SDL Rect to the SDL_Surface. This was previously done automatically by SDL_BlitSurface */
static SDL_Rect ClipToSurface(SDL_Rect const& rect, SDL_Surface const* const surface)
{
	SDL_Rect const bounds{ 0, 0, surface->w, surface->h };
	SDL_Rect clipped;
	// On an empty intersection SDL leaves a rect with a negative extent behind.
	if (!SDL_GetRectIntersection(&rect, &bounds, &clipped)) return SDL_Rect{ 0, 0, 0, 0 };
	return clipped;
}


//#define SCROLL_TEST

static void ScrollJA2Background(INT16 sScrollXIncrement, INT16 sScrollYIncrement)
{
	SDL_Surface* Dest   = ScreenBuffer; // Back
	SDL_Rect     SrcRect;
	SDL_Rect     DstRect;
	SDL_Rect     StripRegions[2];
	UINT16       NumStrips = 0;

	int const width  = SCREEN_WIDTH;
	int const height = gsVIEWPORT_WINDOW_END_Y - gsVIEWPORT_WINDOW_START_Y;

	if (sScrollXIncrement < 0)
	{
		SrcRect.x = 0;
		SrcRect.w = width + sScrollXIncrement;
		DstRect.x = -sScrollXIncrement;
		StripRegions[0].x = gsVIEWPORT_START_X;
		StripRegions[0].y = gsVIEWPORT_WINDOW_START_Y;
		StripRegions[0].w = -sScrollXIncrement;
		StripRegions[0].h = height;
		++NumStrips;
	}
	else if (sScrollXIncrement > 0)
	{
		SrcRect.x = sScrollXIncrement;
		SrcRect.w = width - sScrollXIncrement;
		DstRect.x = 0;
		StripRegions[0].x = gsVIEWPORT_END_X - sScrollXIncrement;
		StripRegions[0].y = gsVIEWPORT_WINDOW_START_Y;
		StripRegions[0].w = sScrollXIncrement;
		StripRegions[0].h = height;
		++NumStrips;
	}
	else
	{
		SrcRect.x = 0;
		SrcRect.w = width;
		DstRect.x = 0;
	}

	if (sScrollYIncrement < 0)
	{
		SrcRect.y = gsVIEWPORT_WINDOW_START_Y;
		SrcRect.h = height + sScrollYIncrement;
		DstRect.y = gsVIEWPORT_WINDOW_START_Y - sScrollYIncrement;
		StripRegions[NumStrips].x = DstRect.x;
		StripRegions[NumStrips].y = gsVIEWPORT_WINDOW_START_Y;
		StripRegions[NumStrips].w = SrcRect.w;
		StripRegions[NumStrips].h = -sScrollYIncrement;
		++NumStrips;
	}
	else if (sScrollYIncrement > 0)
	{
		SrcRect.y = gsVIEWPORT_WINDOW_START_Y + sScrollYIncrement;
		SrcRect.h = height - sScrollYIncrement;
		DstRect.y = gsVIEWPORT_WINDOW_START_Y;
		StripRegions[NumStrips].x = DstRect.x;
		StripRegions[NumStrips].y = gsVIEWPORT_WINDOW_END_Y - sScrollYIncrement;
		StripRegions[NumStrips].w = SrcRect.w;
		StripRegions[NumStrips].h = sScrollYIncrement;
		++NumStrips;
	}
	else
	{
		SrcRect.y = gsVIEWPORT_WINDOW_START_Y;
		SrcRect.h = height;
		DstRect.y = gsVIEWPORT_WINDOW_START_Y;
	}

#ifdef SCROLL_TEST
	SDL_FillRect(Dest, NULL, 0);
#endif

	SDL_BlitSurface(Dest, &SrcRect, Dest, &DstRect);

	for (UINT i = 0; i < NumStrips; i++)
	{
		INT16 const x = static_cast<INT16>(StripRegions[i].x);
		INT16 const y = static_cast<INT16>(StripRegions[i].y);
		INT16 const w = static_cast<INT16>(StripRegions[i].w);
		INT16 const h = static_cast<INT16>(StripRegions[i].h);
		for (int j = y; j < y + h; ++j)
		{
			std::fill_n(gpZBuffer + j * SCREEN_WIDTH + x, w, 0);
		}

		RenderStaticWorldRect(x, y, x + w, y + h, TRUE);
		SDL_BlitSurface(FrameBuffer, &StripRegions[i], Dest, &StripRegions[i]);
	}

	// RESTORE SHIFTED
	RestoreShiftedVideoOverlays(sScrollXIncrement, sScrollYIncrement);

	// SAVE NEW
	SaveVideoOverlaysArea(BACKBUFFER);

	// BLIT NEW
	ExecuteVideoOverlaysToAlternateBuffer(BACKBUFFER);
}

void RefreshScreen(void)
{
	// Not initialised yet or already shut down?
	if (!ScreenTexture) return;

	const BOOLEAN scrolling = (gsScrollXIncrement != 0 || gsScrollYIncrement != 0);

	SDL_BlitSurface(FrameBuffer, &MouseBackground, ScreenBuffer, &MouseBackground);

	// This variable will hold the union of all modified regions.
	struct rect : SDL_Rect {
		void operator+=(SDL_Rect const& r) { SDL_GetRectUnion(this, &r, this); }
	} ScreenTextureUpdateRect{ MouseBackground };

	if (gfForceFullScreenRefresh || guiDirtyRegionCount > 0 || guiDirtyRegionExCount > 0)
	{
		if (gfForceFullScreenRefresh)
		{
			SDL_BlitSurface(FrameBuffer, NULL, ScreenBuffer, NULL);
			ScreenTextureUpdateRect = { 0, 0, ScreenBuffer->w, ScreenBuffer->h };
		}
		else
		{
			for (UINT32 i = 0; i < guiDirtyRegionCount; i++)
			{
				ScreenTextureUpdateRect += DirtyRegions[i];
				SDL_BlitSurface(FrameBuffer, &DirtyRegions[i], ScreenBuffer, &DirtyRegions[i]);
			}

			for (UINT32 i = 0; i < guiDirtyRegionExCount; i++)
			{
				SDL_Rect* r = &DirtyRegionsEx[i];
				if (scrolling)
				{
					// Check if we are completely out of bounds
					if (r->y <= gsVIEWPORT_WINDOW_END_Y && r->y + r->h <= gsVIEWPORT_WINDOW_END_Y)
					{
						continue;
					}
				}
				ScreenTextureUpdateRect += *r;
				SDL_BlitSurface(FrameBuffer, r, ScreenBuffer, r);
			}
		}
		if (scrolling)
		{
			ScrollJA2Background(gsScrollXIncrement, gsScrollYIncrement);
			gsScrollXIncrement = 0;
			gsScrollYIncrement = 0;
			ScreenTextureUpdateRect += SDL_Rect{
				gsVIEWPORT_START_X, gsVIEWPORT_WINDOW_START_Y,
				gsVIEWPORT_END_X - gsVIEWPORT_START_X,
				gsVIEWPORT_WINDOW_END_Y - gsVIEWPORT_WINDOW_START_Y };
		}
		gfIgnoreScrollDueToCenterAdjust = FALSE;
	}

	auto const cursorPos{ GetCursorPos() };
	SDL_Rect src;
	src.x = 0;
	src.y = 0;
	src.w = gusMouseCursorWidth;
	// gsMouseSizeYModifier can push the height past the cursor surface
	src.h = std::min<int>(gusMouseCursorHeight + gsMouseSizeYModifier, MouseCursor->h);
	// The cursor hot spot offset can move this off screen on any side.
	SDL_Rect const dst{
		cursorPos.iX - gsMouseCursorXOffset,
		cursorPos.iY - gsMouseCursorYOffset,
		src.w, src.h };
	SDL_BlitSurface(MouseCursor, &src, ScreenBuffer, &dst);

	// The part that actually ended up on screen
	SDL_Rect const blitted{ ClipToSurface(dst, ScreenBuffer) };
	ScreenTextureUpdateRect += blitted;
	MouseBackground = blitted;

	SDL_Rect const UpdateRect{ ClipToSurface(ScreenTextureUpdateRect, ScreenBuffer) };
	if (UpdateRect.w > 0 && UpdateRect.h > 0)
	{
		uint8_t const * SrcPixels = static_cast<uint8_t *>(ScreenBuffer->pixels)
			+ UpdateRect.y * ScreenBuffer->pitch
			+ UpdateRect.x * SDL_GetPixelFormatDetails(ScreenBuffer->format)->bytes_per_pixel;
		SDL_UpdateTexture(ScreenTexture, &UpdateRect,
		                  SrcPixels, ScreenBuffer->pitch);
	}

	SDL_RenderClear(GameRenderer);

	if (ScaleQuality == VideoScaleQuality::NEAR_PERFECT) {
		SDL_SetRenderTarget(GameRenderer, ScaledScreenTexture);
		SDL_RenderTexture(GameRenderer, ScreenTexture, nullptr, nullptr);

		SDL_SetRenderTarget(GameRenderer, nullptr);
		SDL_RenderTexture(GameRenderer, ScaledScreenTexture, nullptr, nullptr);
	}
	else {
		SDL_RenderTexture(GameRenderer, ScreenTexture, NULL, NULL);
	}

	Visualizer::Render(GameRenderer);

	FPS::RenderPresentPtr(GameRenderer);

	gfForceFullScreenRefresh = FALSE;
	guiDirtyRegionCount = 0;
	guiDirtyRegionExCount = 0;
}


// This is a semi-private function that is supposed to be called only
// by GameLoop(). This is why is has external linkage but is not
// declared in Video.h.
void RefreshScreenCapped()
{
	static std::chrono::steady_clock::time_point LastRefresh;

	auto const now{ std::chrono::steady_clock::now() };
	if (gsScrollXIncrement != 0 || gsScrollYIncrement != 0 ||
	    now - LastRefresh >= TimeBetweenRefreshScreens)
	{
		LastRefresh = now;
		RefreshScreen();
	}
}


static void GetRGBDistribution()
{
	auto f = *SDL_GetPixelFormatDetails(ScreenBuffer->format);

	UINT32          const  r = f.Rmask;
	UINT32          const  g = f.Gmask;
	UINT32          const  b = f.Bmask;

	/* Mask the highest bit of each component. This is used for alpha blending. */
	guiTranslucentMask = (r & r >> 1) | (g & g >> 1) | (b & b >> 1);

	gusRedMask   = static_cast<UINT16>(r);
	gusGreenMask = static_cast<UINT16>(g);
	gusBlueMask  = static_cast<UINT16>(b);

	gusRedShift   = f.Rshift - (8 - f.Rbits);
	gusGreenShift = f.Gshift - (8 - f.Gbits);
	gusBlueShift  = f.Bshift - (8 - f.Bbits);
}


void SetMouseCursorProperties(INT16 sOffsetX, INT16 sOffsetY, UINT16 usCursorHeight, UINT16 usCursorWidth)
{
	gsMouseCursorXOffset = sOffsetX;
	gsMouseCursorYOffset = sOffsetY;
	gusMouseCursorWidth  = usCursorWidth;
	gusMouseCursorHeight = usCursorHeight;
}


static void SetPrimaryVideoSurfaces(void)
{
	// Delete surfaces if they exist
	DeletePrimaryVideoSurfaces();

	g_back_buffer  = new SGPVSurface(ScreenBuffer);
	g_mouse_buffer = new SGPVSurface(MouseCursor);
	g_frame_buffer = new SGPVSurface(FrameBuffer);
}

static void DeletePrimaryVideoSurfaces(void)
{
	delete g_back_buffer;
	g_back_buffer = NULL;

	delete g_frame_buffer;
	g_frame_buffer = NULL;

	delete g_mouse_buffer;
	g_mouse_buffer = NULL;
}

SGPVSurface* gpVSurfaceHead = 0;

void InitializeVideoSurfaceManager(void)
{
	//Shouldn't be calling this if the video surface manager already exists.
	//Call shutdown first...
	Assert(gpVSurfaceHead == NULL);
	gpVSurfaceHead = NULL;

	// Create primary and backbuffer from globals
	SetPrimaryVideoSurfaces();
}


void ShutdownVideoSurfaceManager(void)
{
	// Delete primary viedeo surfaces
	DeletePrimaryVideoSurfaces();

	while (gpVSurfaceHead)
	{
		delete gpVSurfaceHead;
	}
}
