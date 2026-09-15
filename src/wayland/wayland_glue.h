#ifndef RAYLYRICS_WAYLAND_GLUE_H
#define RAYLYRICS_WAYLAND_GLUE_H

/*
 * Opaque Wayland + EGL backend for the raylib PLATFORM_WAYLAND_LAYER rcore
 * platform. Everything Wayland/EGL-specific stays behind this header; the
 * raylib platform file only talks in ints and void pointers.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rl_wl_state rl_wl_state;

/* Anchor bits for rl_wl_set_geometry(). */
enum {
    RL_WL_ANCHOR_TOP = 1,
    RL_WL_ANCHOR_BOTTOM = 2,
    RL_WL_ANCHOR_LEFT = 4,
    RL_WL_ANCHOR_RIGHT = 8,
};

/* Configure the layer-shell anchor/margins. Must be called before InitWindow()
 * (i.e. before rl_wl_create). Unset values default to bottom|left|right and a
 * 48px bottom margin. */
void rl_wl_set_geometry(int anchor, int margin_top, int margin_right, int margin_bottom,
                        int margin_left);

/* Select the output to place the overlay on, by wl_output name (e.g. "DP-1").
 * NULL/empty lets the compositor choose. Must be called before rl_wl_create. */
void rl_wl_set_output(const char *name);

/* Connect, create a layer-shell surface, wait for configure and make an EGL
 * context current. Returns NULL on failure. width/height are the requested
 * surface size in logical pixels; pass 0 to let the compositor decide. */
rl_wl_state *rl_wl_create(int width, int height);

/* Surface size as configured by the compositor. */
int rl_wl_width(const rl_wl_state *state);
int rl_wl_height(const rl_wl_state *state);

/* Real geometry of the output the surface is on. Returns 0 when unknown. */
int rl_wl_monitor_width(const rl_wl_state *state);
int rl_wl_monitor_height(const rl_wl_state *state);
int rl_wl_monitor_refresh(const rl_wl_state *state);
int rl_wl_output_scale(const rl_wl_state *state);
const char *rl_wl_output_name(const rl_wl_state *state);

/* Present the current EGL back buffer. Returns 0 on success. */
int rl_wl_swap(rl_wl_state *state);

/* Non-blocking Wayland event pump. Returns 0 on success. */
int rl_wl_dispatch(rl_wl_state *state);

/* Non-zero once the compositor closed the layer surface. */
int rl_wl_should_close(const rl_wl_state *state);

/* Tear everything down. */
void rl_wl_destroy(rl_wl_state *state);

/* Native wl_surface pointer (for GetWindowHandle). */
void *rl_wl_native_surface(rl_wl_state *state);

/* GL/EGL procedure loader for rlLoadExtensions(). */
void *rl_wl_get_proc_address(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* RAYLYRICS_WAYLAND_GLUE_H */
