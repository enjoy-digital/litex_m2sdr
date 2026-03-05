/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef M2SDR_IMGUI_SDL_GL3_BRIDGE_H
#define M2SDR_IMGUI_SDL_GL3_BRIDGE_H

#include <stdbool.h>
#include <SDL2/SDL.h>
typedef struct ImDrawData ImDrawData;

#ifdef __cplusplus
extern "C" {
#endif

bool m2sdr_imgui_sdl2_init_for_opengl(SDL_Window *window, void *gl_context);
void m2sdr_imgui_sdl2_shutdown(void);
void m2sdr_imgui_sdl2_new_frame(void);
bool m2sdr_imgui_sdl2_process_event(const SDL_Event *event);

bool m2sdr_imgui_opengl3_init(const char *glsl_version);
void m2sdr_imgui_opengl3_shutdown(void);
void m2sdr_imgui_opengl3_new_frame(void);
void m2sdr_imgui_opengl3_render_draw_data(ImDrawData *draw_data);

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_IMGUI_SDL_GL3_BRIDGE_H */
