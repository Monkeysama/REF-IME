#include "API.h"
#include "host_api.hpp"
#include "ime_proxy.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <cmath>
#include <algorithm>

namespace refime {
namespace {
using ImeCallback = void (*)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*);
const REFrameworkPluginFunctions* functions{};

// 唯一游戏窗口和 UI 会话归 Runtime 所有。DLL 固定到进程退出，避免 REF 注册回调引用卸载后的代码。
// Runtime 保持进程生命周期；WM_NCDESTROY 异步停止工作线程，不能在窗口过程/Loader Lock 内等待线程。
struct Runtime {
    EventQueue events;
    ImeProxy proxy{events};
    std::atomic<HWND> owner{};
    std::atomic<WNDPROC> original{};
    ImGuiContext* context{};
    ImeCallback previous{};
    std::atomic<bool> capture{}, enabled{true}, suspended{};
    std::uint64_t session{}, sequence{};
    ImGuiID input{};
    ImGuiPlatformImeData caret{};
    ImGuiViewport* viewport{};
    std::wstring composition;
    std::array<bool, ImGuiKey_NamedKey_COUNT> held{};
    OrderedEvents ordered;
    ImU32 key_barrier{};
    bool proxy_failure_reported{};
    std::atomic<unsigned> diagnostic_events{}, game_focus_messages{};
    std::uint64_t ends{}, diagnostic_tick{};
    std::array<std::uint64_t, 9> diagnostic_snapshot{};
};
Runtime* runtime{};

// 有界记录非文本诊断：两条线程都可调用；不读取 UI 线程独占的输入状态。
void trace(const char* action, std::uint64_t session, UINT message = 0, WPARAM wparam = 0, LPARAM lparam = 0) {
    auto* r = runtime;
    if (!r || !functions) return;
    const auto count = r->diagnostic_events.fetch_add(1);
    if (count < 256)
        functions->log_info("[REF-IME] trace action=%s session=%llu thread=%lu msg=0x%x wp=0x%llx lp=0x%llx foreground=%p focus=%p layout=%p capture=%d suspended=%d proxy_active=%d ime_open=%d conversion=0x%lx",
            action, static_cast<unsigned long long>(session), GetCurrentThreadId(), message,
            static_cast<unsigned long long>(wparam), static_cast<unsigned long long>(lparam),
            GetForegroundWindow(), GetFocus(), GetKeyboardLayout(0),
            static_cast<int>(r->capture.load()), static_cast<int>(r->suspended.load()), static_cast<int>(r->proxy.active()), r->proxy.ime_open(), r->proxy.ime_conversion());
    else if (count == 256) functions->log_info("[REF-IME] detailed trace capped at 256 events; lifecycle counters continue");
}

// 每秒最多记录一次发生变化的累计计数，详细日志达到上限后仍能判断是否反复启动/恢复焦点。
void report_counters() {
    auto& r = *runtime;
    const auto now = GetTickCount64();
    if (now - r.diagnostic_tick < 1000) return;
    r.diagnostic_tick = now;
    const auto p = r.proxy.counters();
    const std::array<std::uint64_t, 9> snapshot{r.sequence, r.ends, p.starts, p.stops, p.recoveries, p.focus_messages, p.layout_syncs, r.game_focus_messages.load(), p.mode_changes};
    if (snapshot == r.diagnostic_snapshot) return;
    r.diagnostic_snapshot = snapshot;
    functions->log_info("[REF-IME] counters ui_start=%llu ui_stop=%llu proxy_start=%llu proxy_stop=%llu focus_recover=%llu proxy_focus=%llu layout_sync=%llu game_focus=%llu mode_changes=%llu active_id=0x%x want_text=%d suspended=%d ime_open=%d conversion=0x%lx",
        static_cast<unsigned long long>(snapshot[0]), static_cast<unsigned long long>(snapshot[1]),
        static_cast<unsigned long long>(snapshot[2]), static_cast<unsigned long long>(snapshot[3]),
        static_cast<unsigned long long>(snapshot[4]), static_cast<unsigned long long>(snapshot[5]),
        static_cast<unsigned long long>(snapshot[6]), static_cast<unsigned long long>(snapshot[7]),
        static_cast<unsigned long long>(snapshot[8]), host.active_id(), static_cast<int>(r.caret.WantTextInput), static_cast<int>(r.suspended.load()), r.proxy.ime_open(), r.proxy.ime_conversion());
}

// 虚拟键转换为 ImGui 命名键；字符由 WM_CHAR/IME 结果提供，不从键码猜测布局。
ImGuiKey imgui_key(std::uint32_t key) {
    if (key >= '0' && key <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + key - '0');
    if (key >= 'A' && key <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + key - 'A');
    if (key >= VK_F1 && key <= VK_F24) return static_cast<ImGuiKey>(ImGuiKey_F1 + key - VK_F1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + key - VK_NUMPAD0);
    switch (key) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp; case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home; case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert; case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace; case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter; case 0x100: return ImGuiKey_KeypadEnter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift; case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl; case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_MENU: case VK_LMENU: return ImGuiKey_LeftAlt; case VK_RMENU: return ImGuiKey_RightAlt;
    case VK_LWIN: return ImGuiKey_LeftSuper; case VK_RWIN: return ImGuiKey_RightSuper;
    case VK_APPS: return ImGuiKey_Menu;
    case VK_OEM_1: return ImGuiKey_Semicolon; case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_COMMA: return ImGuiKey_Comma; case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period; case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_3: return ImGuiKey_GraveAccent; case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash; case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_CAPITAL: return ImGuiKey_CapsLock; case VK_SCROLL: return ImGuiKey_ScrollLock;
    case VK_NUMLOCK: return ImGuiKey_NumLock; case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
    case VK_PAUSE: return ImGuiKey_Pause;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal; case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply; case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    default: return ImGuiKey_None;
    }
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return {};
    std::string text(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), text.data(), length, nullptr, nullptr);
    return text;
}

// 释放本插件发出的按键，不清空 REF 的其他输入。工作线程取消请求是异步的。
void deactivate(ImGuiIO* io, const char* reason = "ui_stop_requested") {
    auto& r = *runtime;
    if (r.session) { ++r.ends; trace(reason, r.session); }
    r.capture = false;
    r.proxy.update({});
    r.session = 0; r.input = 0; r.composition.clear();
    r.ordered.reset(0); r.key_barrier = 0;
    if (io) {
        for (std::size_t i = 0; i < r.held.size(); ++i) {
            if (r.held[i]) host.key(io, static_cast<ImGuiKey>(ImGuiKey_NamedKey_BEGIN + i), false);
            r.held[i] = false;
        }
        for (auto key : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super}) host.key(io, key, false);
    }
}

// 游戏窗口先于 REF 的 Win32 后端处理本代理消息；on_message 回调顺序太晚，不能用它去重。
// 仅文本会话期间转交键盘，鼠标和外部应用焦点消息仍走原链；不改游戏 exe、导入表或 REF DLL。
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* r = runtime;
    const WNDPROC original = r ? r->original.load() : nullptr;
    if (!original) return DefWindowProcW(window, message, wparam, lparam);
    try {
        const bool capture = r->capture && r->proxy.active();
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATEAPP || message == WM_ACTIVATE) {
            ++r->game_focus_messages; trace("game_focus_message", 0, message, wparam, lparam);
        }
        // 显式识别代理线程的激活通知：此时前台窗口可能仍为旧值或 nullptr。
        // 若把它送入失焦处理或游戏原过程，会触发取消→重新聚焦→布局重置的循环。
        if (message == WM_ACTIVATEAPP && !wparam && r->proxy.owns_thread(static_cast<DWORD>(lparam))) return 0;
        if (message == WM_APP + 0x51a && (wparam == WM_KEYDOWN || wparam == WM_KEYUP))
            return CallWindowProcW(original, window, static_cast<UINT>(wparam), VK_INSERT, lparam);
        if (capture) {
            if (message == WM_KILLFOCUS && r->proxy.owns(reinterpret_cast<HWND>(wparam))) return 0;
            if (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE && r->proxy.owns(reinterpret_cast<HWND>(lparam))) return 0;
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP ||
                message == WM_INPUTLANGCHANGEREQUEST || message == WM_INPUTLANGCHANGE) {
                r->proxy.redirect(message, wparam, lparam); return 0;
            }
            if (message == WM_CHAR || message == WM_SYSCHAR || message == WM_IME_CHAR ||
                message == WM_IME_STARTCOMPOSITION || message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION) return 0;
        }
        if ((message == WM_ACTIVATEAPP && !wparam) ||
            (message == WM_KILLFOCUS && !r->proxy.owns(reinterpret_cast<HWND>(wparam)))) {
            // 鼠标重新聚焦游戏不暂停；外部焦点交由下一次 UI 回调撤销会话，禁止抢回焦点。
            if (GetForegroundWindow() != window && !r->proxy.owns(GetForegroundWindow())) {
                r->suspended = true; r->capture = false; r->proxy.update({});
            }
        }
        if (message == WM_SETFOCUS || (message == WM_ACTIVATEAPP && wparam)) r->suspended = false;
        if (message == WM_NCDESTROY) {
            r->capture = false; r->enabled = false; r->proxy.request_stop(); r->owner = nullptr;
            // 只在仍处于链顶时恢复，不能覆盖后来安装的其他插件窗口过程。
            if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == window_proc)
                SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        }
        return CallWindowProcW(original, window, message, wparam, lparam);
    } catch (...) {
        r->capture = false; r->enabled = false; r->proxy.request_stop();
        return CallWindowProcW(original, window, message, wparam, lparam);
    }
}

// 绑定同进程游戏 HWND 一次；原窗口过程始终保留并放行非文本消息。
bool attach_window(HWND window) {
    auto& r = *runtime;
    if (r.owner) return r.owner == window;
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    if (!window || process != GetCurrentProcessId()) return false;
    r.original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    SetLastError(0);
    auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(window_proc)));
    if (!original) return false;
    r.original = original; r.owner = window;
    if (!r.proxy.start(window, trace)) { r.enabled = false; return false; }
    return true;
}

// 同一会话由 ImGui 的活动输入 ID 标识；切换输入框时使用新序号拒绝旧线程迟到事件。
void reconcile(ImGuiContext* context, ImGuiViewport* viewport, const ImGuiPlatformImeData& caret) {
    auto& r = *runtime;
    ImGuiIO* io = host.io(context);
    ImGuiInputTextState* state = host.input_state(host.active_id());
    const HWND foreground = GetForegroundWindow();
    const HWND window = static_cast<HWND>(viewport ? viewport->PlatformHandleRaw : nullptr);
    const char* rejection{};
    if (!r.enabled) rejection = "ui_disabled";
    else if (r.suspended) rejection = "ui_suspended";
    else if (!caret.WantTextInput) rejection = "ui_no_text_request";
    else if (!state) rejection = "ui_active_input_lost";
    else if (state->Flags & ImGuiInputTextFlags_ReadOnly) rejection = "ui_read_only";
    else if (!window || !attach_window(window)) rejection = "ui_window_invalid";
    else if (foreground != window && !r.proxy.owns(foreground) && !(foreground == nullptr && r.proxy.transitioning())) rejection = "ui_external_foreground";
    if (rejection) {
        if (r.session) deactivate(io, rejection);
        return;
    }
    if (r.proxy.failed()) {
        if (!r.proxy_failure_reported) { functions->log_error("[REF-IME] Windows input proxy failed; bridge disabled"); r.proxy_failure_reported = true; }
        r.enabled = false; deactivate(io); return;
    }
    if (!r.session || r.input != state->ID) {
        if (r.session) deactivate(io);
        r.session = ++r.sequence; r.input = state->ID;
        trace("ui_start", r.session);
        r.ordered.reset(r.session);
    }
    r.caret = caret; r.viewport = viewport;
    // ImGui 插入点是 viewport 全局坐标，转为游戏客户区像素后交给代理线程。
    const auto x = caret.InputPos.x - viewport->Pos.x;
    const auto y = caret.InputPos.y - viewport->Pos.y;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(caret.InputLineHeight)) { deactivate(io); return; }
    r.capture = true;
    r.proxy.update({r.session, true, static_cast<int>(std::clamp(x, -32768.0f, 32768.0f)),
        static_cast<int>(std::clamp(y, -32768.0f, 32768.0f)), static_cast<int>(std::clamp(caret.InputLineHeight, 8.0f, 200.0f))});
}

// ImGui EndFrame 提供当前插入点；不在这里同步等待代理线程或读取 Windows IME 状态。
void ime_callback(ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    try {
        if (!runtime || context != runtime->context || host.current_context() != context) return;
        reconcile(context, viewport, *data);
        if (!runtime->enabled && runtime->previous) runtime->previous(context, viewport, data);
    } catch (...) {
        runtime->enabled = false; deactivate(host.io(context));
        functions->log_error("[REF-IME] IME callback failed; bridge disabled");
    }
}

// 未确认的组合显示在插入点下方，确认前不修改用户输入值；密码输入框不展示组合内容。
void draw_composition(ImGuiInputTextState* state) {
    auto& r = *runtime;
    if (r.composition.empty() || !state || (state->Flags & ImGuiInputTextFlags_Password)) return;
    const std::string text = utf8(r.composition);
    if (text.empty()) return;
    ImVec2 size{}; host.measure(&size, text.c_str(), nullptr, false, -1);
    const ImVec2 pos(r.caret.InputPos.x, r.caret.InputPos.y + r.caret.InputLineHeight + 3);
    auto* draw = host.foreground();
    host.rect(draw, ImVec2(pos.x - 3, pos.y - 2), ImVec2(pos.x + size.x + 4, pos.y + size.y + 3), IM_COL32(24, 24, 28, 235), 3, 0);
    host.text(draw, pos, IM_COL32(245, 245, 245, 255), text.c_str(), nullptr);
    host.line(draw, ImVec2(pos.x, pos.y + size.y + 1), ImVec2(pos.x + size.x, pos.y + size.y + 1), IM_COL32(130, 185, 255, 255), 1);
}

// APIProxy 的回调在 NewFrame 后、Lua 和 REF 原生控件前：本帧的字符交给标准 InputText 过滤/撤销。
// 按键走宿主 AddKeyEvent 队列。只读取当前会话，工作线程从不调用 ImGui。
void on_imgui_frame(REFImGuiFrameCbData* data) {
    try {
        auto& r = *runtime;
        auto* context = static_cast<ImGuiContext*>(data->context);
        if (!context || host.current_context() != context) return;
        auto* platform = host.platform(context);
        if (!r.context) {
            r.context = context; r.previous = platform->Platform_SetImeDataFn;
            platform->Platform_SetImeDataFn = ime_callback;
            functions->log_info("[REF-IME] " REF_IME_VERSION " installed on compatible ImGui 1.92.0 context (all game targets)");
        } else if (r.context != context) {
            r.enabled = false; r.capture = false; r.proxy.update({}); return;
        }
        if (platform->Platform_SetImeDataFn != ime_callback) {
            r.enabled = false; deactivate(host.io(context));
            return; // 不与其他接管 IME 回调的插件争抢。
        }
        ImGuiIO* io = host.io(context);
        if (r.viewport) reconcile(context, r.viewport, r.caret);
        report_counters();
        bool overflow{};
        auto events = r.events.drain(r.session, overflow);
        if (overflow) {
            functions->log_error("[REF-IME] input event queue overflow; session cancelled");
            deactivate(io); return;
        }
        if (std::any_of(events.begin(), events.end(), [](const InputEvent& event) { return event.kind == EventKind::focus_lost; })) {
            r.suspended = true; deactivate(io, "ui_proxy_focus_failure"); return;
        }
        if (!r.ordered.append(std::move(events))) { deactivate(io); return; }
        // 鼠标点击可能在稍后绘制控件时切换输入框；先等一帧确定活动 ID，避免提交到新输入框。
        if (io->MouseClicked[0]) return;
        if (r.key_barrier) {
            bool pending = false;
            for (const auto& event : context->InputEventsQueue) if (event.EventId <= r.key_barrier) { pending = true; break; }
            if (pending) { draw_composition(host.input_state(r.input)); return; }
            r.key_barrier = 0;
        }
        while (!r.ordered.empty()) {
            // 先提交一批相邻按键，再让宿主 NewFrame 应用；后面的字符不能越过按键屏障。
            if (r.key_barrier && r.ordered.front().kind != EventKind::key) break;
            const auto event = r.ordered.front(); r.ordered.pop();
            if (event.kind == EventKind::composition) r.composition = event.text;
            else if (event.kind == EventKind::cancel) r.composition.clear();
            else if (event.kind == EventKind::commit || event.kind == EventKind::character) {
                if (event.kind == EventKind::commit) r.composition.clear();
                for (auto value : codepoints(event.text)) io->InputQueueCharacters.push_back(static_cast<ImWchar>(value));
            } else if (event.kind == EventKind::key) {
                host.key(io, ImGuiMod_Ctrl, event.ctrl); host.key(io, ImGuiMod_Shift, event.shift);
                host.key(io, ImGuiMod_Alt, event.alt); host.key(io, ImGuiMod_Super, event.super);
                const auto key = imgui_key(event.key);
                if (key != ImGuiKey_None) {
                    host.key(io, key, event.down);
                    r.held[key - ImGuiKey_NamedKey_BEGIN] = event.down;
                }
                if (!context->InputEventsQueue.empty()) r.key_barrier = context->InputEventsQueue.back().EventId;
            }
        }
        draw_composition(host.input_state(r.input));
    } catch (...) {
        runtime->enabled = false; runtime->capture = false; runtime->proxy.update({});
        functions->log_error("[REF-IME] UI callback failed; bridge disabled");
    }
}

// REF 菜单中的诊断与关闭开关，不显示或记录用户输入；关闭后原始 IME 回调继续可用。
void on_imgui_ui(REFImGuiFrameCbData* data) {
    if (!runtime || data->context != runtime->context || host.current_context() != runtime->context) return;
    if (!host.tree("REF-IME " REF_IME_VERSION)) return;
    bool enabled = runtime->enabled;
    if (host.checkbox("Enable native IME bridge", &enabled)) {
        runtime->enabled = enabled;
        if (!enabled) deactivate(host.io(runtime->context));
    }
    host.label(runtime->proxy.failed() ? "Windows proxy failed (restart required)" :
        runtime->proxy.ready() ? "Windows proxy ready" : "Windows proxy starts when an input is focused", nullptr);
    host.label("All REFramework game targets / ImGui 1.92.0 / WCHAR32", nullptr);
    host.tree_pop();
}
}
}

// 通用入口不按游戏名称或 DX11/DX12 限制；桥接使用 REF 的 UI 渲染和窗口，无游戏专属 SDK。
extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = 1; version->minor = 15; version->patch = 0; version->game_name = nullptr;
}
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    using namespace refime;
    if (!param || !param->functions || !param->version || !param->functions->log_error || !param->functions->log_info) return false;
    functions = param->functions;
    if (param->version->major != 1 || param->version->minor < 15 || !functions->on_imgui_frame || !functions->on_imgui_draw_ui) {
        functions->log_error("[REF-IME] not installed: REF API 1.15+ and ImGui callbacks required"); return false;
    }
    std::string error;
    if (!host.bind(GetModuleHandleW(L"dinput8.dll"), error)) {
        functions->log_error("[REF-IME] not installed: %s", error.c_str()); return false;
    }
    functions->log_info("[REF-IME] compatible host game=%s api=%d.%d.%d ImGui=%s; no game/hash whitelist",
        param->version->game_name ? param->version->game_name : "unspecified",
        param->version->major, param->version->minor, param->version->patch, IMGUI_VERSION);
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&reframework_plugin_initialize), &pinned)) return false;
    runtime = new Runtime;
    if (!functions->on_imgui_frame(on_imgui_frame)) return false;
    if (!functions->on_imgui_draw_ui(on_imgui_ui)) return false;
    return true;
}
