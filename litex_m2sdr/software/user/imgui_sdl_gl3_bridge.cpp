/* SPDX-License-Identifier: BSD-2-Clause */

#include "imgui_sdl_gl3_bridge.h"

#include "cimgui/imgui/backends/imgui_impl_sdl2.h"
#include "cimgui/imgui/backends/imgui_impl_opengl3.h"

extern "C" bool m2sdr_imgui_sdl2_init_for_opengl(SDL_Window *window, void *gl_context)
{
    return ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
}

extern "C" void m2sdr_imgui_sdl2_shutdown(void)
{
    ImGui_ImplSDL2_Shutdown();
}

extern "C" void m2sdr_imgui_sdl2_new_frame(void)
{
    ImGui_ImplSDL2_NewFrame();
}

extern "C" bool m2sdr_imgui_sdl2_process_event(const SDL_Event *event)
{
    return ImGui_ImplSDL2_ProcessEvent(event);
}

extern "C" bool m2sdr_imgui_opengl3_init(const char *glsl_version)
{
    return ImGui_ImplOpenGL3_Init(glsl_version);
}

extern "C" void m2sdr_imgui_opengl3_shutdown(void)
{
    ImGui_ImplOpenGL3_Shutdown();
}

extern "C" void m2sdr_imgui_opengl3_new_frame(void)
{
    ImGui_ImplOpenGL3_NewFrame();
}

extern "C" void m2sdr_imgui_opengl3_render_draw_data(ImDrawData *draw_data)
{
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}
