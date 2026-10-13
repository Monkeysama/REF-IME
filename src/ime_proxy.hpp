#pragma once
#include "bridge_state.hpp"
#include <Windows.h>
#include <atomic>
#include <bitset>
#include <thread>
#include <functional>

namespace refime {
// 专用消息线程拥有透明 EDIT、字体和 IME 上下文；不接触 ImGui。
// UI 线程只更新请求并取事件，窗口线程只投递消息，不跨线程 SendMessage 或等待退出。
class ImeProxy {
public:
    using Diagnostic = std::function<void(const char*, std::uint64_t, UINT, WPARAM, LPARAM)>;
    struct Counters { std::uint64_t starts, stops, recoveries, focus_messages, layout_syncs, mode_changes; };
    explicit ImeProxy(EventQueue& queue) : queue_(queue) {}
    ~ImeProxy();
    bool start(HWND owner, Diagnostic diagnostic = {});
    void update(InputRequest request);
    void redirect(UINT message, WPARAM key, LPARAM native);
    void request_stop();
    bool owns(HWND window) const { return window != nullptr && window == edit_.load(); }
    // WM_ACTIVATEAPP 的 lParam 是线程 ID，不能用 HWND/前台窗口快照判断跨线程内部激活。
    bool owns_thread(DWORD thread) const { return thread != 0 && thread == thread_id_.load(); }
    DWORD thread_id() const { return thread_id_.load(); }
    bool transitioning() const { return transitioning_.load(); }
    bool active() const { return active_.load(); }
    HWND window() const { return edit_.load(); }
    bool ready() const { return ready_.load(); }
    bool failed() const { return failed_.load(); }
    Counters counters() const { return {starts_.load(), stops_.load(), recoveries_.load(), focus_messages_.load(), layout_syncs_.load(), mode_changes_.load()}; }
    int ime_open() const { return ime_open_.load(); }
    DWORD ime_conversion() const { return ime_conversion_.load(); }
private:
#ifdef REF_IME_TESTING
    friend struct ImeProxyTestAccess;
#endif
    static constexpr UINT apply_message = WM_APP + 0x517;
    static constexpr UINT redirect_message = WM_APP + 0x518;
    static constexpr UINT stop_message = WM_APP + 0x519;
    struct Redirect { UINT message; WPARAM key; LPARAM native; };
    static LRESULT CALLBACK edit_proc(HWND, UINT, WPARAM, LPARAM);
    void run();
    void apply();
    void apply_bounds();
    bool ensure_input_focus();
    void process_redirects();
    void send_key(UINT message, WPARAM key, LPARAM native);
    void emit(InputEvent event);
    void deactivate(bool return_focus);
    void report(const char* action, UINT message = 0, WPARAM wparam = 0, LPARAM lparam = 0) noexcept;
    EventQueue& queue_;
    std::thread thread_;
    std::atomic<HWND> edit_{};
    std::atomic<bool> active_{}, ready_{}, failed_{}, stopping_{}, transitioning_{};
    std::atomic<DWORD> thread_id_{};
    std::atomic<std::uint64_t> starts_{}, stops_{}, recoveries_{}, focus_messages_{}, layout_syncs_{}, mode_changes_{};
    std::atomic<int> ime_open_{-1};
    std::atomic<DWORD> ime_conversion_{};
    Diagnostic diagnostic_;
    HWND owner_{};
    WNDPROC original_{};
    DWORD owner_thread_{};
    bool attached_{}, composing_{};
    HFONT font_{};
    int font_height_{};
    InputRequest current_, pending_;
    std::mutex mutex_;
    std::deque<Redirect> redirected_;
    CommitDeduper deduper_;
    std::bitset<256> forwarded_keys_;
    wchar_t high_surrogate_{};
};
}

