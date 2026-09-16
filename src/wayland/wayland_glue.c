#include "wayland/wayland_glue.h"

#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <linux/input-event-codes.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef EGL_PLATFORM_WAYLAND_EXT
#define EGL_PLATFORM_WAYLAND_EXT 0x31D8
#endif

#define RL_WL_MAX_OUTPUTS 8

struct rl_wl_output {
    struct wl_output *proxy;
    int x;
    int y;
    int phys_width;
    int phys_height;
    int mode_width;
    int mode_height;
    int refresh;
    int scale;
    char name[64];
};

struct rl_wl_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_region *input_region;
    struct wl_egl_window *egl_window;

    struct wl_seat *seat;
    struct wl_pointer *pointer;
    int pointer_x;
    int pointer_y;
    int dragging;
    int drag_dx;
    int drag_dy;

    struct rl_wl_output outputs[RL_WL_MAX_OUTPUTS];
    int output_count;
    struct wl_output *surface_output;  // set by wl_surface.enter
    struct wl_output *selected_output; // from rl_wl_set_output

    EGLDisplay egl_display;
    EGLConfig egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    int width;
    int height;
    int configured;
    int closed;
};

/* Geometry configured by rl_wl_set_geometry() before rl_wl_create(). */
static int g_anchor = RL_WL_ANCHOR_BOTTOM | RL_WL_ANCHOR_LEFT | RL_WL_ANCHOR_RIGHT;
static int g_margin_top = 0;
static int g_margin_right = 0;
static int g_margin_bottom = 48;
static int g_margin_left = 0;
static char g_output_name[64] = {0};

/* Layer-shell placement, also set before rl_wl_create(). */
static int g_layer = RL_WL_LAYER_OVERLAY;
static char g_namespace[64] = "raylyrics";
static int g_exclusive_zone = -1;
static int g_keyboard = 0;

/* Explicit size request; non-positive means "full output". */
static int g_size_set = 0;
static int g_requested_width = 0;
static int g_requested_height = 0;

/* Pointer dragging of the surface (preset-declared). */
static int g_draggable = 0;

void rl_wl_set_draggable(int draggable) { g_draggable = draggable ? 1 : 0; }

void rl_wl_set_geometry(int anchor, int margin_top, int margin_right, int margin_bottom,
                        int margin_left) {
    g_anchor = anchor;
    g_margin_top = margin_top;
    g_margin_right = margin_right;
    g_margin_bottom = margin_bottom;
    g_margin_left = margin_left;
}

void rl_wl_set_size(int width, int height) {
    g_size_set = 1;
    g_requested_width = width;
    g_requested_height = height;
}

void rl_wl_set_layer(int layer) {
    if (layer < RL_WL_LAYER_BACKGROUND || layer > RL_WL_LAYER_OVERLAY) return;
    g_layer = layer;
}

void rl_wl_set_namespace(const char *name) {
    if (name == NULL) {
        g_namespace[0] = '\0';
        return;
    }
    snprintf(g_namespace, sizeof(g_namespace), "%s", name);
}

void rl_wl_set_exclusive_zone(int zone) { g_exclusive_zone = zone; }

void rl_wl_set_keyboard(int interactive) { g_keyboard = interactive ? 1 : 0; }

void rl_wl_set_output(const char *name) {
    if (name == NULL) {
        g_output_name[0] = '\0';
        return;
    }
    snprintf(g_output_name, sizeof(g_output_name), "%s", name);
}

/* ------------------------------------------------------------------ */
/* listeners                                                          */
/* ------------------------------------------------------------------ */

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform) {
    struct rl_wl_output *output = data;
    output->x = x;
    output->y = y;
    output->phys_width = physical_width;
    output->phys_height = physical_height;
    (void)wl_output; (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width,
                        int32_t height, int32_t refresh) {
    struct rl_wl_output *output = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        output->mode_width = width;
        output->mode_height = height;
        output->refresh = refresh;
    }
    (void)wl_output;
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)data; (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    struct rl_wl_output *output = data;
    output->scale = factor > 0 ? factor : 1;
    (void)wl_output;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    struct rl_wl_output *output = data;
    snprintf(output->name, sizeof(output->name), "%s", name != NULL ? name : "");
    (void)wl_output;
}

static void output_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data; (void)wl_output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output) {
    struct rl_wl_state *state = data;
    state->surface_output = output;
    (void)surface;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output) {
    struct rl_wl_state *state = data;
    if (state->surface_output == output) state->surface_output = NULL;
    (void)surface;
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

/* Pointer input, only used when rl_wl_set_draggable(1) made the surface
 * interactive. Motion while the left button is held accumulates a delta that
 * rl_wl_dispatch() turns into a margin change, which is how a layer-shell
 * surface moves. */
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;
    (void)surface;
    struct rl_wl_state *state = data;
    state->pointer_x = wl_fixed_to_int(sx);
    state->pointer_y = wl_fixed_to_int(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)pointer;
    (void)serial;
    (void)surface;
    struct rl_wl_state *state = data;
    state->dragging = 0;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx,
                           wl_fixed_t sy) {
    (void)pointer;
    (void)time;
    struct rl_wl_state *state = data;
    const int x = wl_fixed_to_int(sx);
    const int y = wl_fixed_to_int(sy);
    if (state->dragging) {
        state->drag_dx += x - state->pointer_x;
        state->drag_dy += y - state->pointer_y;
    }
    state->pointer_x = x;
    state->pointer_y = y;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t button_state) {
    (void)pointer;
    (void)serial;
    (void)time;
    struct rl_wl_state *state = data;
    if (button != BTN_LEFT) return;
    state->dragging = button_state == WL_POINTER_BUTTON_STATE_PRESSED ? 1 : 0;
    if (!state->dragging) {
        state->drag_dx = 0;
        state->drag_dy = 0;
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    struct rl_wl_state *state = data;
    const int has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_pointer && state->pointer == NULL) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
};

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface,
                         uint32_t serial, uint32_t width, uint32_t height) {
    struct rl_wl_state *state = data;
    if (width > 0) state->width = (int)width;
    if (height > 0) state->height = (int)height;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    state->configured = 1;
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
    (void)layer_surface;
    struct rl_wl_state *state = data;
    state->closed = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = ls_configure,
    .closed = ls_closed,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct rl_wl_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, bind_version);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t bind_version = version < 4 ? version : 4;
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, bind_version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (g_draggable && state->seat == NULL) {
            // Version 1 keeps the pointer listener to the events every
            // compositor sends.
            state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
            wl_seat_add_listener(state->seat, &seat_listener, state);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (state->output_count < RL_WL_MAX_OUTPUTS) {
            struct rl_wl_output *output = &state->outputs[state->output_count];
            memset(output, 0, sizeof(*output));
            output->scale = 1;
            uint32_t bind_version = version < 4 ? version : 4;
            output->proxy = wl_registry_bind(registry, name, &wl_output_interface, bind_version);
            wl_output_add_listener(output->proxy, &output_listener, output);
            state->output_count++;
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* EGL                                                                */
/* ------------------------------------------------------------------ */

static int init_egl(struct rl_wl_state *state) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (get_platform_display) {
        state->egl_display = get_platform_display(EGL_PLATFORM_WAYLAND_EXT, state->display, NULL);
    } else {
        state->egl_display = eglGetDisplay((EGLNativeDisplayType)state->display);
    }

    if (state->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "raylyrics: eglGetDisplay failed\n");
        return -1;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(state->egl_display, &major, &minor)) {
        fprintf(stderr, "raylyrics: eglInitialize failed (0x%x)\n", eglGetError());
        return -1;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };

    EGLint num_configs = 0;
    if (!eglChooseConfig(state->egl_display, config_attribs, &state->egl_config, 1, &num_configs) ||
        num_configs < 1) {
        fprintf(stderr, "raylyrics: eglChooseConfig failed (0x%x)\n", eglGetError());
        return -1;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "raylyrics: eglBindAPI(EGL_OPENGL_API) failed\n");
        return -1;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };

    state->egl_context = eglCreateContext(state->egl_display, state->egl_config, EGL_NO_CONTEXT, context_attribs);
    if (state->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "raylyrics: eglCreateContext failed (0x%x)\n", eglGetError());
        return -1;
    }

    state->egl_window = wl_egl_window_create(state->surface, state->width, state->height);
    if (!state->egl_window) {
        fprintf(stderr, "raylyrics: wl_egl_window_create failed\n");
        return -1;
    }

    state->egl_surface = eglCreateWindowSurface(state->egl_display, state->egl_config,
                                                (EGLNativeWindowType)state->egl_window, NULL);
    if (state->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "raylyrics: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return -1;
    }

    if (!eglMakeCurrent(state->egl_display, state->egl_surface, state->egl_surface, state->egl_context)) {
        fprintf(stderr, "raylyrics: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return -1;
    }

    eglSwapInterval(state->egl_display, 0);

    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

static void resolve_output_size(const struct rl_wl_state *state, int *width, int *height);

rl_wl_state *rl_wl_create(int width, int height) {
    struct rl_wl_state *state = calloc(1, sizeof(*state));
    if (!state) return NULL;

    const int requested_width = g_size_set ? g_requested_width : width;
    const int requested_height = g_size_set ? g_requested_height : height;
    state->width = requested_width > 0 ? requested_width : 1;
    state->height = requested_height > 0 ? requested_height : 1;

    state->display = wl_display_connect(NULL);
    if (!state->display) {
        fprintf(stderr, "raylyrics: failed to connect to Wayland display\n");
        goto fail;
    }

    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    wl_display_roundtrip(state->display);
    // Second roundtrip so wl_output geometry/mode/scale/name events arrive.
    wl_display_roundtrip(state->display);

    if (!state->compositor) {
        fprintf(stderr, "raylyrics: wl_compositor not available\n");
        goto fail;
    }
    if (!state->layer_shell) {
        fprintf(stderr, "raylyrics: zwlr_layer_shell_v1 not available\n");
        goto fail;
    }

    if (g_output_name[0] != '\0') {
        for (int i = 0; i < state->output_count; i++) {
            if (strcmp(state->outputs[i].name, g_output_name) == 0) {
                state->selected_output = state->outputs[i].proxy;
                break;
            }
        }
        if (state->selected_output == NULL) {
            fprintf(stderr, "raylyrics: output '%s' not found, using compositor default\n",
                    g_output_name);
        }
    }

    // A non-positive request means "full output"; resolve it now that the
    // output geometry is known.
    if (requested_width <= 0 || requested_height <= 0) {
        int output_width = 0;
        int output_height = 0;
        resolve_output_size(state, &output_width, &output_height);
        if (requested_width <= 0 && output_width > 0) state->width = output_width;
        if (requested_height <= 0 && output_height > 0) state->height = output_height;
    }

    state->surface = wl_compositor_create_surface(state->compositor);
    wl_surface_add_listener(state->surface, &surface_listener, state);
    wl_surface_set_buffer_scale(state->surface, 1);

    if (!g_draggable) {
        // An empty input region makes the overlay click-through. A draggable
        // surface leaves the region unset, so the whole surface takes input.
        state->input_region = wl_compositor_create_region(state->compositor);
        wl_surface_set_input_region(state->surface, state->input_region);
    }

    state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state->layer_shell, state->surface, state->selected_output,
        (enum zwlr_layer_shell_v1_layer)g_layer, g_namespace);
    zwlr_layer_surface_v1_add_listener(state->layer_surface, &layer_surface_listener, state);

    zwlr_layer_surface_v1_set_size(state->layer_surface, (uint32_t)state->width, (uint32_t)state->height);
    zwlr_layer_surface_v1_set_anchor(state->layer_surface, (uint32_t)g_anchor);
    // NOTE: wlr-layer-shell set_margin() order is (top, right, bottom, left)
    zwlr_layer_surface_v1_set_margin(state->layer_surface, g_margin_top, g_margin_right,
                                     g_margin_bottom, g_margin_left);
    zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, g_exclusive_zone);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state->layer_surface,
        g_keyboard ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                   : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    wl_surface_commit(state->surface);

    for (int i = 0; i < 8 && !state->configured && !state->closed; i++) {
        if (wl_display_roundtrip(state->display) < 0) {
            fprintf(stderr, "raylyrics: wayland roundtrip failed\n");
            goto fail;
        }
    }

    if (!state->configured) {
        fprintf(stderr, "raylyrics: layer surface was never configured\n");
        goto fail;
    }

    if (init_egl(state) != 0) goto fail;

    return state;

fail:
    rl_wl_destroy(state);
    return NULL;
}

int rl_wl_width(const rl_wl_state *state) {
    return state ? state->width : 0;
}

int rl_wl_height(const rl_wl_state *state) {
    return state ? state->height : 0;
}

static const struct rl_wl_output *active_output(const struct rl_wl_state *state) {
    if (state == NULL) return NULL;
    if (state->surface_output != NULL) {
        for (int i = 0; i < state->output_count; i++) {
            if (state->outputs[i].proxy == state->surface_output) return &state->outputs[i];
        }
    }
    if (state->output_count > 0) return &state->outputs[0];
    return NULL;
}

/* Logical size of the output the surface will land on: the explicitly selected
 * one, else the first. Returns 0 when unknown. */
static void resolve_output_size(const struct rl_wl_state *state, int *width, int *height) {
    const struct rl_wl_output *output = NULL;
    if (state->selected_output != NULL) {
        for (int i = 0; i < state->output_count; i++) {
            if (state->outputs[i].proxy == state->selected_output) {
                output = &state->outputs[i];
                break;
            }
        }
    }
    if (output == NULL && state->output_count > 0) output = &state->outputs[0];

    *width = 0;
    *height = 0;
    if (output == NULL) return;
    const int scale = output->scale > 0 ? output->scale : 1;
    if (output->mode_width > 0) *width = output->mode_width / scale;
    if (output->mode_height > 0) *height = output->mode_height / scale;
}

int rl_wl_monitor_width(const rl_wl_state *state) {
    const struct rl_wl_output *output = active_output(state);
    if (output == NULL || output->mode_width <= 0) return 0;
    const int scale = output->scale > 0 ? output->scale : 1;
    return output->mode_width / scale;
}

int rl_wl_monitor_height(const rl_wl_state *state) {
    const struct rl_wl_output *output = active_output(state);
    if (output == NULL || output->mode_height <= 0) return 0;
    const int scale = output->scale > 0 ? output->scale : 1;
    return output->mode_height / scale;
}

int rl_wl_monitor_refresh(const rl_wl_state *state) {
    const struct rl_wl_output *output = active_output(state);
    return output != NULL ? output->refresh : 0;
}

int rl_wl_output_scale(const rl_wl_state *state) {
    const struct rl_wl_output *output = active_output(state);
    return (output != NULL && output->scale > 0) ? output->scale : 1;
}

const char *rl_wl_output_name(const rl_wl_state *state) {
    const struct rl_wl_output *output = active_output(state);
    return output != NULL ? output->name : "";
}

int rl_wl_swap(rl_wl_state *state) {
    if (!state || state->egl_surface == EGL_NO_SURFACE) return -1;
    if (!eglSwapBuffers(state->egl_display, state->egl_surface)) return -1;
    return 0;
}

/* Move the surface by adjusting the margins of the anchored edges. The surface
 * keeps its size, so only the position changes. */
static void apply_drag(struct rl_wl_state *state, int dx, int dy) {
    int output_width = 0;
    int output_height = 0;
    resolve_output_size(state, &output_width, &output_height);
    const int max_x = output_width > 0 ? output_width : 100000;
    const int max_y = output_height > 0 ? output_height : 100000;

    // A centered axis (neither edge anchored) ignores its margins, so it cannot
    // be dragged. Anchor both edges of that axis at the position the surface
    // already occupies, which makes the margins meaningful.
    if (dx != 0 && !(g_anchor & (RL_WL_ANCHOR_LEFT | RL_WL_ANCHOR_RIGHT))) {
        const int centred = (max_x - state->width) / 2;
        g_anchor |= RL_WL_ANCHOR_LEFT | RL_WL_ANCHOR_RIGHT;
        g_margin_left = centred;
        g_margin_right = max_x - state->width - centred;
        zwlr_layer_surface_v1_set_anchor(state->layer_surface, (uint32_t)g_anchor);
    }
    if (dy != 0 && !(g_anchor & (RL_WL_ANCHOR_TOP | RL_WL_ANCHOR_BOTTOM))) {
        const int centred = (max_y - state->height) / 2;
        g_anchor |= RL_WL_ANCHOR_TOP | RL_WL_ANCHOR_BOTTOM;
        g_margin_top = centred;
        g_margin_bottom = max_y - state->height - centred;
        zwlr_layer_surface_v1_set_anchor(state->layer_surface, (uint32_t)g_anchor);
    }

    if (dx != 0) {
        if (g_anchor & RL_WL_ANCHOR_LEFT) g_margin_left += dx;
        if (g_anchor & RL_WL_ANCHOR_RIGHT) g_margin_right -= dx;
    }
    if (dy != 0) {
        if (g_anchor & RL_WL_ANCHOR_TOP) g_margin_top += dy;
        if (g_anchor & RL_WL_ANCHOR_BOTTOM) g_margin_bottom -= dy;
    }

    // Keep at least part of the surface on the output.
    if (g_margin_left < 0) g_margin_left = 0;
    if (g_margin_right < 0) g_margin_right = 0;
    if (g_margin_top < 0) g_margin_top = 0;
    if (g_margin_bottom < 0) g_margin_bottom = 0;
    if (g_margin_left > max_x) g_margin_left = max_x;
    if (g_margin_right > max_x) g_margin_right = max_x;
    if (g_margin_top > max_y) g_margin_top = max_y;
    if (g_margin_bottom > max_y) g_margin_bottom = max_y;

    zwlr_layer_surface_v1_set_margin(state->layer_surface, g_margin_top, g_margin_right,
                                     g_margin_bottom, g_margin_left);
    wl_surface_commit(state->surface);
}

int rl_wl_dispatch(rl_wl_state *state) {
    if (!state || !state->display) return -1;

    while (wl_display_prepare_read(state->display) != 0) {
        if (wl_display_dispatch_pending(state->display) < 0) return -1;
    }

    if (wl_display_flush(state->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(state->display);
        return -1;
    }

    struct pollfd pfd = {
        .fd = wl_display_get_fd(state->display),
        .events = POLLIN,
    };

    int ret = poll(&pfd, 1, 0);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_read_events(state->display) < 0) return -1;
    } else {
        wl_display_cancel_read(state->display);
    }

    if (wl_display_dispatch_pending(state->display) < 0) return -1;

    if (state->drag_dx != 0 || state->drag_dy != 0) {
        apply_drag(state, state->drag_dx, state->drag_dy);
        state->drag_dx = 0;
        state->drag_dy = 0;
    }

    return 0;
}

int rl_wl_should_close(const rl_wl_state *state) {
    return state ? state->closed : 1;
}

void rl_wl_destroy(rl_wl_state *state) {
    if (!state) return;

    if (state->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(state->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (state->egl_surface != EGL_NO_SURFACE) eglDestroySurface(state->egl_display, state->egl_surface);
        if (state->egl_context != EGL_NO_CONTEXT) eglDestroyContext(state->egl_display, state->egl_context);
        eglTerminate(state->egl_display);
    }

    if (state->egl_window) wl_egl_window_destroy(state->egl_window);
    if (state->pointer) wl_pointer_destroy(state->pointer);
    if (state->seat) wl_seat_destroy(state->seat);
    if (state->input_region) wl_region_destroy(state->input_region);
    if (state->layer_surface) zwlr_layer_surface_v1_destroy(state->layer_surface);
    if (state->surface) wl_surface_destroy(state->surface);
    if (state->layer_shell) zwlr_layer_shell_v1_destroy(state->layer_shell);
    if (state->compositor) wl_compositor_destroy(state->compositor);
    if (state->registry) wl_registry_destroy(state->registry);
    if (state->display) wl_display_disconnect(state->display);

    free(state);
}

void *rl_wl_native_surface(rl_wl_state *state) {
    return state ? (void *)state->surface : NULL;
}

void *rl_wl_get_proc_address(const char *name) {
    return (void *)eglGetProcAddress(name);
}
