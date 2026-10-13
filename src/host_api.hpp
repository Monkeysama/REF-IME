#pragma once
#include <Windows.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <string>
#include <functional>

namespace refime {
// 使用宿主导出的 cimgui 函数，避免在插件中创建另一套 ImGui 全局上下文或分配器。
// 按 ImGui 版本、布局和导出能力验证宿主，不依赖游戏名称或单个 REF DLL 哈希。
// 上下文函数只在 REF UI 回调使用；独立验证只调用无上下文的版本/布局函数。
struct HostApi {
    bool bind(HMODULE module, std::string& error);
    bool bind_exports(const std::function<FARPROC(const char*)>& resolve, std::string& error);
    const char* (*version)(){};
    bool (*check_layout)(const char*, size_t, size_t, size_t, size_t, size_t, size_t){};
    ImFontGlyphRangesBuilder* (*glyph_builder_new)(){};
    void (*glyph_builder_destroy)(ImFontGlyphRangesBuilder*){};
    ImGuiContext* (*current_context)(){};
    ImGuiIO* (*io)(ImGuiContext*){};
    ImGuiPlatformIO* (*platform)(ImGuiContext*){};
    ImGuiID (*active_id)(){};
    ImGuiInputTextState* (*input_state)(ImGuiID){};
    void (*key)(ImGuiIO*, ImGuiKey, bool){};
    void (*focus)(ImGuiIO*, bool){};
    ImDrawList* (*foreground)(){};
    void (*text)(ImDrawList*, ImVec2, ImU32, const char*, const char*){};
    void (*rect)(ImDrawList*, ImVec2, ImVec2, ImU32, float, ImDrawFlags){};
    void (*line)(ImDrawList*, ImVec2, ImVec2, ImU32, float){};
    void (*measure)(ImVec2*, const char*, const char*, bool, float){};
    bool (*tree)(const char*){};
    void (*tree_pop)(){};
    bool (*checkbox)(const char*, bool*){};
    void (*label)(const char*, const char*){};
    void* (*alloc)(size_t){};
    void (*free)(void*){};
};
extern HostApi host;
}

