#include "ime_proxy.hpp"
#include <imm.h>
#include <utility>

namespace refime {
namespace {
// 焦点迁移期间仅允许 UI 忽略暂时为空的前台快照；嵌套恢复保留外层状态，异常也会复位。
struct FocusTransition {
    std::atomic<bool>& flag;
    bool previous;
    explicit FocusTransition(std::atomic<bool>& value) : flag(value), previous(value.exchange(true)) {}
    ~FocusTransition() { flag.store(previous); }
};
// IMM 文本长度以字节计；同一次读取限定长度，避免畸形消息造成无界分配。
std::wstring composition(HWND window, DWORD kind) {
    HIMC context = ImmGetContext(window);
    if (!context) return {};
    LONG size = ImmGetCompositionStringW(context, kind, nullptr, 0);
    std::wstring result;
    if (size > 0 && size <= 65536 && size % sizeof(wchar_t) == 0) {
        result.resize(size / sizeof(wchar_t));
        if (ImmGetCompositionStringW(context, kind, result.data(), size) != size) result.clear();
    }
    ImmReleaseContext(window, context);
    return result;
}
bool key_message(UINT message) {
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP;
}
}

ImeProxy::~ImeProxy() { request_stop(); if (thread_.joinable()) thread_.join(); }

// 首次创建消息线程，启动阶段也可排队请求；失败由 ready/failed 状态传回 UI 线程。
bool ImeProxy::start(HWND owner, Diagnostic diagnostic) {
    if (thread_.joinable()) return owner == owner_;
    if (!IsWindow(owner)) return false;
    owner_ = owner;
    diagnostic_ = std::move(diagnostic);
    try { thread_ = std::thread([this] { run(); }); }
    catch (...) { failed_ = true; return false; }
    return true;
}
void ImeProxy::request_stop() {
    stopping_ = true;
    if (auto edit = edit_.load()) PostMessageW(edit, stop_message, 0, 0);
    else if (auto thread = thread_id_.load()) PostThreadMessageW(thread, WM_QUIT, 0, 0);
}
void ImeProxy::update(InputRequest request) {
    { std::lock_guard lock(mutex_); pending_ = request; }
    if (auto edit = edit_.load()) PostMessageW(edit, apply_message, 0, 0);
}
void ImeProxy::redirect(UINT message, WPARAM key, LPARAM native) {
    if (!active_) return;
    {
        std::lock_guard lock(mutex_);
        if (redirected_.size() >= 1024) { redirected_.clear(); pending_.active = false; }
        else redirected_.push_back({message, key, native});
    }
    if (auto edit = edit_.load()) PostMessageW(edit, redirect_message, 0, 0);
}

void ImeProxy::emit(InputEvent event) {
    event.session = current_.session;
    if (!queue_.push(std::move(event))) {
        active_ = false;
        { std::lock_guard lock(mutex_); pending_.active = false; }
        if (auto edit = edit_.load()) PostMessageW(edit, apply_message, 0, 0);
    }
}

// 只上报生命周期和 Win32 焦点参数，不记录字符；诊断失败不能影响正在输入的会话。
void ImeProxy::report(const char* action, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    // HIMC 仅在拥有 EDIT 的线程读取；UI 和日志回调只读取原子快照。
    if (HIMC context = ImmGetContext(edit_.load())) {
        ime_open_ = ImmGetOpenStatus(context) ? 1 : 0;
        DWORD conversion{}, sentence{};
        if (ImmGetConversionStatus(context, &conversion, &sentence)) ime_conversion_ = conversion;
        ImmReleaseContext(edit_.load(), context);
    } else { ime_open_ = -1; ime_conversion_ = 0; }
    if (diagnostic_) try { diagnostic_(action, current_.session, message, wparam, lparam); } catch (...) {}
}

// 取消组合、清理队列并放弃 Windows 焦点；只有焦点仍属于代理时才还给游戏，不能抢 Alt-Tab。
void ImeProxy::deactivate(bool return_focus) {
    const bool was_active = active_.exchange(false);
    if (was_active) { ++stops_; report("proxy_stop"); }
    HWND edit = edit_.load();
    if (!edit) return;
    if (HIMC context = ImmGetContext(edit)) {
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(edit, context);
    }
    composing_ = false; deduper_.new_key(); high_surrogate_ = 0;
    forwarded_keys_.reset();
    { std::lock_guard lock(mutex_); redirected_.clear(); }
    if (was_active) emit({EventKind::cancel});
    if (return_focus && GetFocus() == edit && IsWindow(owner_)) SetFocus(owner_);
    ShowWindow(edit, SW_HIDE);
    current_.active = false;
}

// 几何数据来自 ImGui 插入点的游戏客户区坐标；不创建可见 UI，也不把编辑内容写入日志。
void ImeProxy::apply_bounds() {
    HWND edit = edit_.load();
    const int height = std::clamp(current_.line_height, 8, 200);
    if (height != font_height_) {
        HFONT font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (font) {
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            if (font_) DeleteObject(font_);
            font_ = font; font_height_ = height;
            SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        }
    }
    POINT point{current_.x, current_.y};
    if (!ClientToScreen(owner_, &point)) return;
    SetWindowPos(edit, HWND_TOP, point.x, point.y, 2, height + 4, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (HIMC context = ImmGetContext(edit)) {
        COMPOSITIONFORM form{}; form.dwStyle = CFS_FORCE_POSITION; form.ptCurrentPos = {0, 0};
        ImmSetCompositionWindow(context, &form);
        CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_CANDIDATEPOS; candidate.ptCurrentPos = {0, height};
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(edit, context);
    }
}

// 同一会话仅更新坐标；新会话取消旧组合，再取得 EDIT 默认 IME 上下文和键盘焦点。
void ImeProxy::apply() {
    InputRequest request;
    { std::lock_guard lock(mutex_); request = pending_; }
    if (!request.active || stopping_) { deactivate(true); return; }
    const bool changed = !active_ || current_.session != request.session;
    if (changed && active_) deactivate(false);
    current_ = request;
    active_ = true;
    apply_bounds();
    if (changed) {
        ++starts_; report("proxy_start");
        const HWND foreground = GetForegroundWindow();
        if (foreground != owner_ && foreground != edit_.load()) {
            report("proxy_start_external_foreground");
            deactivate(false); emit({EventKind::focus_lost}); return;
        }
        FocusTransition transition{transitioning_};
        if (HKL layout = GetKeyboardLayout(owner_thread_)) { ++layout_syncs_; ActivateKeyboardLayout(layout, 0); }
        SetWindowTextW(edit_.load(), L"");
        ShowWindow(edit_.load(), SW_SHOWNOACTIVATE);
        if (!ensure_input_focus()) { report("proxy_focus_failed"); deactivate(false); emit({EventKind::focus_lost}); }
    }
}

// 复用已有 IME 上下文和系统焦点；只在缺失时恢复，不能重置中英模式或正在进行的组合。
bool ImeProxy::ensure_input_focus() {
    HWND edit = edit_.load();
    if (!edit) return false;
    if (HIMC context = ImmGetContext(edit)) ImmReleaseContext(edit, context);
    else { ImmAssociateContextEx(edit, nullptr, IACE_DEFAULT); report("proxy_context_restored"); }
    if (GetFocus() != edit) {
        ++recoveries_;
        FocusTransition transition{transitioning_};
        SetFocus(edit);
        if (GetFocus() != edit) { SetActiveWindow(edit); SetFocus(edit); }
        report(GetFocus() == edit ? "proxy_focus_recovered" : "proxy_focus_failed");
    }
    return GetFocus() == edit;
}

// 回退的游戏窗口键盘消息只在代理线程执行 TranslateMessage；确保输入法切换和组合仍由系统处理。
void ImeProxy::process_redirects() {
    std::deque<Redirect> input;
    { std::lock_guard lock(mutex_); input.swap(redirected_); }
    if (!active_) return;
    if (GetForegroundWindow() != owner_ && GetForegroundWindow() != edit_.load()) return;
    if (!ensure_input_focus()) return;
    for (const auto& item : input) {
        MSG message{}; message.hwnd = edit_.load(); message.message = item.message;
        message.wParam = item.key; message.lParam = item.native;
        if (item.message == WM_KEYDOWN || item.message == WM_SYSKEYDOWN) TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// IME 的候选操作不进入 ImGui（特别是确认用 Enter）；普通编辑快捷键由 ImGui 自身处理。
void ImeProxy::send_key(UINT message, WPARAM key, LPARAM native) {
    if (key >= 256) return;
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (down && key != VK_PROCESSKEY) deduper_.new_key();
    if (key == VK_PROCESSKEY || key == VK_PACKET) return;
    if (down && composing_) return;
    if (!down && !forwarded_keys_.test(key)) return;
    forwarded_keys_.set(key, down);
    InputEvent event{EventKind::key};
    event.key = static_cast<std::uint32_t>(key);
    if (key == VK_RETURN && (native & (1LL << 24))) event.key = 0x100; // 小键盘 Enter
    event.down = down;
    event.ctrl = GetKeyState(VK_CONTROL) < 0; event.shift = GetKeyState(VK_SHIFT) < 0;
    event.alt = GetKeyState(VK_MENU) < 0; event.super = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
    emit(std::move(event));
}

// 子类处理字符去重和组合生命周期，其余消息保留标准 EDIT 的系统行为。
LRESULT CALLBACK ImeProxy::edit_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ImeProxy*>(GetPropW(window, L"REF-IME.Proxy"));
    if (!self || !self->original_) return DefWindowProcW(window, message, wparam, lparam);
    try {
        if (message == apply_message) { self->apply(); return 0; }
        if (message == redirect_message) { self->process_redirects(); return 0; }
        if (message == stop_message) { PostQuitMessage(0); return 0; }
        // 与 REFF 相同：EDIT 的临时失焦不是逻辑输入框失焦，不能在系统窗口过程内取消/隐藏代理。
        // 候选窗口、TSF 和内部激活可能产生空目标；外部应用切换由游戏通知及 UI 前台检查结束会话。
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ACTIVATEAPP) {
            ++self->focus_messages_; self->report("proxy_focus_message", message, wparam, lparam);
        }
        if (message == WM_INPUTLANGCHANGEREQUEST || message == WM_INPUTLANGCHANGE)
            self->report("proxy_language_message", message, wparam, lparam);
        if (message == WM_IME_STARTCOMPOSITION) {
            self->composing_ = true; self->emit({EventKind::composition}); self->apply_bounds();
        } else if (message == WM_IME_COMPOSITION) {
            if (lparam & GCS_COMPSTR) {
                self->composing_ = true;
                InputEvent event{EventKind::composition}; event.text = composition(window, GCS_COMPSTR); self->emit(std::move(event));
            }
            if (lparam & GCS_RESULTSTR) {
                InputEvent event{EventKind::commit}; event.text = composition(window, GCS_RESULTSTR);
                self->deduper_.committed(event.text, GetTickCount64());
                self->emit(std::move(event)); self->composing_ = false;
            }
            self->apply_bounds();
        } else if (message == WM_IME_ENDCOMPOSITION) {
            self->composing_ = false; self->emit({EventKind::cancel});
        } else if (message == WM_IME_CHAR) {
            return 0; // 已通过 GCS_RESULTSTR 提交，禁止 EDIT 再派生 WM_CHAR。
        } else if (message == WM_CHAR || message == WM_SYSCHAR) {
            const auto character = static_cast<wchar_t>(wparam);
            if (!self->active_ || self->composing_ || self->deduper_.suppress(character, GetTickCount64())) return 0;
            InputEvent event{EventKind::character};
            if (character >= 0xd800 && character <= 0xdbff) { self->high_surrogate_ = character; return 0; }
            if (self->high_surrogate_) { event.text += self->high_surrogate_; self->high_surrogate_ = 0; }
            event.text += character; self->emit(std::move(event));
            return 0;
        } else if (key_message(message)) {
            self->send_key(message, wparam, lparam);
            // Insert 是 REF 默认菜单键；交给游戏窗口的原始消息链，不能只变成 ImGui 的编辑键。
            if (wparam == VK_INSERT && !self->composing_ && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_SHIFT) >= 0)
                PostMessageW(self->owner_, WM_APP + 0x51a, message, lparam);
            // 不让隐藏控件参与剪贴板、撤销或导航，避免覆盖真实 ImGui 输入框的选区/剪贴板。
            if (!self->composing_ && wparam != VK_PROCESSKEY) return 0;
        }
        const auto result = CallWindowProcW(self->original_, window, message, wparam, lparam);
        if (message == WM_IME_NOTIFY && (wparam == IMN_SETOPENSTATUS || wparam == IMN_SETCONVERSIONMODE)) {
            ++self->mode_changes_; self->report("proxy_ime_mode_notify", message, wparam, lparam);
        }
        return result;
    } catch (...) {
        self->failed_ = true; self->active_ = false; PostQuitMessage(1); return 0;
    }
}

// 线程入口集中清理资源；窗口销毁/进程退出只发停止请求，不在游戏窗口线程 join。
void ImeProxy::run() {
    MSG probe{}; PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    thread_id_ = GetCurrentThreadId();
    owner_thread_ = GetWindowThreadProcessId(owner_, nullptr);
    attached_ = owner_thread_ && AttachThreadInput(thread_id_.load(), owner_thread_, TRUE);
    HWND edit{};
    try {
        if (!attached_ || stopping_) throw 1;
        edit = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED, L"EDIT", L"",
            WS_POPUP | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 2, 24, owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!edit) throw 1;
        if (!SetPropW(edit, L"REF-IME.Proxy", this)) throw 1;
        SetLastError(0);
        original_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_proc)));
        if (!original_) throw 1;
        SetLayeredWindowAttributes(edit, 0, 0, LWA_ALPHA);
        edit_ = edit; ready_ = true;
        report("proxy_ready");
        apply();
        MSG message{};
        while (!stopping_) {
            const int result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) { if (result < 0) failed_ = true; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    } catch (...) { failed_ = true; }
    if (edit) {
        deactivate(true);
        ImmAssociateContext(edit, nullptr);
        if (original_) SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_));
        RemovePropW(edit, L"REF-IME.Proxy"); DestroyWindow(edit);
    }
    if (font_) DeleteObject(font_);
    font_ = nullptr; original_ = nullptr; edit_ = nullptr; active_ = false; ready_ = false;
    if (attached_) AttachThreadInput(thread_id_.load(), owner_thread_, FALSE);
    attached_ = false; thread_id_ = 0;
}
}
