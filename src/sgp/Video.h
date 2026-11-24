#ifndef VIDEO_H
#define VIDEO_H

#include "Types.h"
#include "RustInterface.h"
#include "SDL3/SDL.h"


#define VIDEO_DEFAULT_TO_NO_CURSOR 0xFFFE // VIDEO_DEFAULT_TO_NO_CURSOR is equal to VIDEO_NO_CURSOR unless always_show_cursor_in_tactical is true
#define VIDEO_NO_CURSOR 0xFFFF
#define GAME_WINDOW g_game_window

extern INT16 gsMouseSizeYModifier;
extern SDL_Window* g_game_window;
extern SDL_Renderer* GameRenderer;

using VideoScaleQuality = ScalingQuality;

void         VideoSetFullScreen(BOOLEAN enable);
void         InitializeVideoManager(VideoScaleQuality quality, int32_t targetFPS);
void         ShutdownVideoManager(void);
void         InvalidateRegion(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom);
void         InvalidateScreen(void);

void VideoSetBrightness(float brightness);

/* Toggle between fullscreen and window mode after initialising the video
 * manager */
void VideoToggleFullScreen(void);

/* The resolution "auto" picks: the display's real pixels (in a window: the
 * part left by the taskbar and title bar) divided by the whole
 * number that gives a logical height closest to 720 (1280x720 on 2560x1440 and
 * 3840x2160, 960x540 on 1920x1080), never below 640x480. */
void VideoPickAutoResolution(UINT16& width, UINT16& height, bool fullscreen);

/* Size the window to scale x the game resolution in real pixels, or the
 * largest scale that fits the display when scale is 0 or does not fit.
 * Does nothing in fullscreen. */
void VideoSetWindowScale(int scale);

void SetMouseCursorProperties(INT16 sOffsetX, INT16 sOffsetY, UINT16 usCursorHeight, UINT16 usCursorWidth);

void InvalidateRegionEx(INT32 iLeft, INT32 iTop, INT32 iRight, INT32 iBottom);

void RefreshScreen(void);

// Creates a list to contain video Surfaces
void InitializeVideoSurfaceManager(void);

// Deletes any video Surface placed into list
void ShutdownVideoSurfaceManager(void);

#endif
