#ifndef JCE_TEST_IMGUI_CONTEXT_H
#define JCE_TEST_IMGUI_CONTEXT_H
#include <jce/tools/jce_imgui.hpp>

/* Actual ImGui CPU context with licensed built-in Latin font pixels. No
 * renderer, texture upload, native window, IO persistence or GPU is started. */
struct JceTestImGuiContext {
    ImGuiContext *previous = nullptr;
    ImGuiContext *context = nullptr;
    bool frame_open = false;
    JceTestImGuiContext()
    {
        previous = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = true;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void begin()
    {
        ImGui::NewFrame();
        frame_open = true;
    }
    void end()
    {
        ImGui::EndFrame();
        frame_open = false;
    }
    ~JceTestImGuiContext()
    {
        if (frame_open) ImGui::EndFrame();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    JceTestImGuiContext(const JceTestImGuiContext &) = delete;
    JceTestImGuiContext &operator=(const JceTestImGuiContext &) = delete;
};
#endif
