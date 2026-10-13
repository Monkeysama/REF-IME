#include "host_api.hpp"
#include <string_view>

namespace refime {
HostApi host;
// 只解析已由 REF 加载的模块，不另行 LoadLibrary，不依赖各游戏不同的二进制哈希。
bool HostApi::bind(HMODULE module, std::string& error) {
    if (!module) { *this = {}; error = "dinput8.dll not loaded"; return false; }
    return bind_exports([module](const char* name) { return GetProcAddress(module, name); }, error);
}

// 先检查无上下文的版本/布局，再绑定状态和绘制函数；全部成功才发布，失败不保留半套接口。
// 只适配官方 ImGui 1.92.0 ABI；游戏无白名单，其他 ImGui ABI 必须新增明确适配。
bool HostApi::bind_exports(const std::function<FARPROC(const char*)>& resolve, std::string& error) {
    *this = {};
    error.clear();
    if (!resolve) { error = "host export resolver missing"; return false; }
    HostApi candidate;
#define LOAD(member, name) candidate.member = reinterpret_cast<decltype(candidate.member)>(resolve(name)); if (!candidate.member) { error = "missing host export: " name; return false; }
    LOAD(version, "igGetVersion")
    const char* actual_version = candidate.version();
    if (!actual_version || std::string_view(actual_version) != IMGUI_VERSION) {
        error = "unsupported ImGui version: " + std::string(actual_version ? actual_version : "unknown") + "; requires " IMGUI_VERSION;
        return false;
    }
    LOAD(check_layout, "igDebugCheckVersionAndDataLayout")
    if (!candidate.check_layout(IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4), sizeof(ImDrawVert), sizeof(ImDrawIdx))) {
        error = "ImGui data layout mismatch (requires official REF ImGui 1.92.0 / WCHAR32 ABI)"; return false;
    }
    // IO 的 ImVector 大小不随 ImWchar 位宽变化，单靠公共布局检查无法识别 WCHAR16。
    // 使用宿主创建/销毁的临时字形范围对象观察 Unicode 位图长度，不创建上下文或注入测试字符。
    LOAD(glyph_builder_new, "ImFontGlyphRangesBuilder_ImFontGlyphRangesBuilder")
    LOAD(glyph_builder_destroy, "ImFontGlyphRangesBuilder_destroy")
    auto* probe = candidate.glyph_builder_new();
    if (!probe) { error = "host Unicode ABI probe allocation failed"; return false; }
    const bool wchar32 = probe->UsedChars.Size == (IM_UNICODE_CODEPOINT_MAX + 1) / 32;
    candidate.glyph_builder_destroy(probe);
    if (!wchar32) { error = "unsupported ImGui Unicode ABI: requires WCHAR32"; return false; }
    LOAD(current_context, "igGetCurrentContext")
    LOAD(io, "igGetIO_ContextPtr")
    LOAD(platform, "igGetPlatformIO_ContextPtr")
    LOAD(active_id, "igGetActiveID")
    LOAD(input_state, "igGetInputTextState")
    LOAD(key, "ImGuiIO_AddKeyEvent")
    LOAD(focus, "ImGuiIO_AddFocusEvent")
    LOAD(foreground, "igGetForegroundDrawList_Nil")
    LOAD(text, "ImDrawList_AddText_Vec2")
    LOAD(rect, "ImDrawList_AddRectFilled")
    LOAD(line, "ImDrawList_AddLine")
    LOAD(measure, "igCalcTextSize")
    LOAD(tree, "igTreeNode_Str")
    LOAD(tree_pop, "igTreePop")
    LOAD(checkbox, "igCheckbox")
    LOAD(label, "igTextUnformatted")
    LOAD(alloc, "igMemAlloc")
    LOAD(free, "igMemFree")
#undef LOAD
    *this = candidate;
    return true;
}
}

// ImVector 的内联扩容委托给宿主分配器；所有内存始终由同一个 ImGui 堆持有。
void* ImGui::MemAlloc(size_t size) { return refime::host.alloc(size); }
void ImGui::MemFree(void* data) { refime::host.free(data); }

