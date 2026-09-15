/**********************************************************************************************
*
*   rcore_wayland_layer - raylib core platform backend for Wayland layer-shell overlays
*
*   PLATFORM: PLATFORM_WAYLAND_LAYER
*       Native Wayland + wlr-layer-shell + EGL. No GLFW, no X11, no input.
*
*   LIMITATIONS:
*       - Single output, buffer scale 1, no fractional scaling
*       - No window management: fullscreen/maximize/position are no-ops
*       - Input is intentionally absent (click-through overlay)
*
*   DEPENDENCIES:
*       - wayland-client, wayland-egl, EGL (via src/wayland/wayland_glue.c)
*
*   LICENSE: zlib/libpng
*
*   Copyright (c) 2026 raylyrics contributors
*
**********************************************************************************************/

#include "wayland/wayland_glue.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
typedef struct {
    rl_wl_state *wl;    // Wayland/EGL backend state

} PlatformData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
extern CoreData CORE;                   // Global CORE state context

static PlatformData platform = { 0 };   // Platform specific data

//----------------------------------------------------------------------------------
// Module Internal Functions Declaration
//----------------------------------------------------------------------------------
int InitPlatform(void);          // Initialize platform (graphics, inputs and more)

//----------------------------------------------------------------------------------
// Module Functions Definition: Window and Graphics Device
//----------------------------------------------------------------------------------

// Check if application should close
bool WindowShouldClose(void)
{
    if (CORE.Window.ready) return CORE.Window.shouldClose || (platform.wl && rl_wl_should_close(platform.wl));
    else return true;
}

// Toggle fullscreen mode
void ToggleFullscreen(void)
{
    TRACELOG(LOG_WARNING, "ToggleFullscreen() not available on layer-shell overlay");
}

// Toggle borderless windowed mode
void ToggleBorderlessWindowed(void)
{
    TRACELOG(LOG_WARNING, "ToggleBorderlessWindowed() not available on layer-shell overlay");
}

// Set window state: maximized, if resizable
void MaximizeWindow(void)
{
    TRACELOG(LOG_WARNING, "MaximizeWindow() not available on layer-shell overlay");
}

// Set window state: minimized
void MinimizeWindow(void)
{
    TRACELOG(LOG_WARNING, "MinimizeWindow() not available on layer-shell overlay");
}

// Restore window from being minimized/maximized
void RestoreWindow(void)
{
    TRACELOG(LOG_WARNING, "RestoreWindow() not available on layer-shell overlay");
}

// Set window configuration state using flags
void SetWindowState(unsigned int flags)
{
    (void)flags;
}

// Clear window configuration state flags
void ClearWindowState(unsigned int flags)
{
    (void)flags;
}

// Set icon for window
void SetWindowIcon(Image image)
{
    (void)image;
}

// Set icon for window
void SetWindowIcons(Image *images, int count)
{
    (void)images; (void)count;
}

// Set title for window
void SetWindowTitle(const char *title)
{
    CORE.Window.title = title;
}

// Set window position on screen (windowed mode)
void SetWindowPosition(int x, int y)
{
    (void)x; (void)y;
}

// Set monitor for the current window
void SetWindowMonitor(int monitor)
{
    (void)monitor;
}

// Set window minimum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMinSize(int width, int height)
{
    CORE.Window.screenMin.width = width;
    CORE.Window.screenMin.height = height;
}

// Set window maximum dimensions (FLAG_WINDOW_RESIZABLE)
void SetWindowMaxSize(int width, int height)
{
    CORE.Window.screenMax.width = width;
    CORE.Window.screenMax.height = height;
}

// Set window dimensions
void SetWindowSize(int width, int height)
{
    (void)width; (void)height;
}

// Set window opacity, value opacity is between 0.0 and 1.0
void SetWindowOpacity(float opacity)
{
    (void)opacity;
}

// Set window focused
void SetWindowFocused(void)
{
}

// Get native window handle
void *GetWindowHandle(void)
{
    return platform.wl ? rl_wl_native_surface(platform.wl) : NULL;
}

// Get number of monitors
int GetMonitorCount(void)
{
    return 1;
}

// Get current monitor where window is placed
int GetCurrentMonitor(void)
{
    return 0;
}

// Get selected monitor position
Vector2 GetMonitorPosition(int monitor)
{
    (void)monitor;
    return (Vector2){ 0.0f, 0.0f };
}

// Get selected monitor width (currently used by monitor)
int GetMonitorWidth(int monitor)
{
    (void)monitor;
    const int width = platform.wl ? rl_wl_monitor_width(platform.wl) : 0;
    if (width > 0) return width;
    return (CORE.Window.display.width > 0) ? CORE.Window.display.width : CORE.Window.screen.width;
}

// Get selected monitor height (currently used by monitor)
int GetMonitorHeight(int monitor)
{
    (void)monitor;
    const int height = platform.wl ? rl_wl_monitor_height(platform.wl) : 0;
    if (height > 0) return height;
    return (CORE.Window.display.height > 0) ? CORE.Window.display.height : CORE.Window.screen.height;
}

// Get selected monitor physical width in millimetres
int GetMonitorPhysicalWidth(int monitor)
{
    (void)monitor;
    return 0;
}

// Get selected monitor physical height in millimetres
int GetMonitorPhysicalHeight(int monitor)
{
    (void)monitor;
    return 0;
}

// Get selected monitor refresh rate
int GetMonitorRefreshRate(int monitor)
{
    (void)monitor;
    return platform.wl ? rl_wl_monitor_refresh(platform.wl) : 0;
}

// Get the human-readable, UTF-8 encoded name of the selected monitor
const char *GetMonitorName(int monitor)
{
    (void)monitor;
    return platform.wl ? rl_wl_output_name(platform.wl) : "";
}

// Get window position XY on monitor
Vector2 GetWindowPosition(void)
{
    return (Vector2){ 0.0f, 0.0f };
}

// Get window scale DPI factor for current monitor
Vector2 GetWindowScaleDPI(void)
{
    const float scale = (platform.wl ? rl_wl_output_scale(platform.wl) : 1);
    return (Vector2){ scale, scale };
}

// Set clipboard text content
void SetClipboardText(const char *text)
{
    (void)text;
}

// Get clipboard text content
const char *GetClipboardText(void)
{
    return NULL;
}

// Get clipboard image
Image GetClipboardImage(void)
{
    Image image = { 0 };
    return image;
}

// Show mouse cursor
void ShowCursor(void)
{
    CORE.Input.Mouse.cursorHidden = false;
}

// Hide mouse cursor
void HideCursor(void)
{
    CORE.Input.Mouse.cursorHidden = true;
}

// Enable cursor (unlock cursor)
void EnableCursor(void)
{
    CORE.Input.Mouse.cursorHidden = false;
}

// Disable cursor (lock cursor)
void DisableCursor(void)
{
    CORE.Input.Mouse.cursorHidden = true;
}

// Swap back buffer with front buffer (screen drawing)
void SwapScreenBuffer(void)
{
    if (platform.wl) rl_wl_swap(platform.wl);
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Misc
//----------------------------------------------------------------------------------

// Get elapsed time measure in seconds since InitTimer()
double GetTime(void)
{
    double time = 0.0;

    struct timespec ts = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long nanoSeconds = (unsigned long long)ts.tv_sec*1000000000LLU + (unsigned long long)ts.tv_nsec;

    time = (double)(nanoSeconds - CORE.Time.base)*1e-9; // Elapsed time since InitTimer()

    return time;
}

// Open URL with default system browser (if available)
void OpenURL(const char *url)
{
    (void)url;
    TRACELOG(LOG_WARNING, "OpenURL() not implemented on layer-shell overlay");
}

//----------------------------------------------------------------------------------
// Module Functions Definition: Inputs
//----------------------------------------------------------------------------------

// Set internal gamepad mappings
int SetGamepadMappings(const char *mappings)
{
    (void)mappings;
    return 0;
}

// Set gamepad vibration
void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    (void)gamepad; (void)leftMotor; (void)rightMotor; (void)duration;
}

// Set mouse position XY
void SetMousePosition(int x, int y)
{
    CORE.Input.Mouse.currentPosition = (Vector2){ (float)x, (float)y };
}

// Set mouse cursor
void SetMouseCursor(int cursor)
{
    (void)cursor;
}

// Get physical key name.
const char *GetKeyName(int key)
{
    (void)key;
    return "";
}

// Register all input events
void PollInputEvents(void)
{
    // Layer-shell overlay never receives input; only pump Wayland events.
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;

    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;

    CORE.Input.Gamepad.lastButtonPressed = 0;

    for (int i = 0; i < MAX_TOUCH_POINTS; i++) CORE.Input.Touch.previousTouchState[i] = CORE.Input.Touch.currentTouchState[i];

    for (int i = 0; i < 260; i++)
    {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }

    if (platform.wl) rl_wl_dispatch(platform.wl);
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition
//----------------------------------------------------------------------------------

// Initialize platform: graphics, inputs and more
int InitPlatform(void)
{
    int width = (CORE.Window.screen.width > 0) ? CORE.Window.screen.width : 800;
    int height = (CORE.Window.screen.height > 0) ? CORE.Window.screen.height : 120;

    platform.wl = rl_wl_create(width, height);
    if (!platform.wl)
    {
        TRACELOG(LOG_FATAL, "PLATFORM: Failed to initialize Wayland layer-shell backend");
        return -1;
    }

    const int monitor_width = rl_wl_monitor_width(platform.wl);
    const int monitor_height = rl_wl_monitor_height(platform.wl);

    CORE.Window.display.width = (monitor_width > 0) ? monitor_width : rl_wl_width(platform.wl);
    CORE.Window.display.height = (monitor_height > 0) ? monitor_height : rl_wl_height(platform.wl);
    CORE.Window.screen.width = rl_wl_width(platform.wl);
    CORE.Window.screen.height = rl_wl_height(platform.wl);
    CORE.Window.render.width = rl_wl_width(platform.wl);
    CORE.Window.render.height = rl_wl_height(platform.wl);
    CORE.Window.currentFbo.width = rl_wl_width(platform.wl);
    CORE.Window.currentFbo.height = rl_wl_height(platform.wl);
    CORE.Window.position = (Point){ 0, 0 };
    CORE.Window.ready = true;

    TRACELOG(LOG_INFO, "DISPLAY: Wayland layer-shell surface initialized successfully");
    TRACELOG(LOG_INFO, "    > Display size: %i x %i", CORE.Window.display.width, CORE.Window.display.height);
    TRACELOG(LOG_INFO, "    > Screen size:  %i x %i", CORE.Window.screen.width, CORE.Window.screen.height);

    // Load OpenGL extensions through EGL
    rlLoadExtensions((void *)rl_wl_get_proc_address);

    // Initialize timing system
    InitTimer();

    // Initialize storage system
    CORE.Storage.basePath = GetWorkingDirectory();

    TRACELOG(LOG_INFO, "PLATFORM: WAYLAND_LAYER: Initialized successfully");

    return 0;
}

// Close platform
void ClosePlatform(void)
{
    rl_wl_destroy(platform.wl);
    platform.wl = NULL;
}

// EOF
