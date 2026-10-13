#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace refime {

enum class EventKind { character, key, composition, commit, cancel, focus_lost };
struct InputEvent {
    EventKind kind{};
    std::uint64_t session{};
    std::uint32_t key{};
    bool down{}, ctrl{}, shift{}, alt{}, super{};
    std::wstring text;
};
struct InputRequest {
    std::uint64_t session{};
    bool active{};
    int x{}, y{}, line_height{20};
};

// 跨线程事件队列：只有当前输入会话能取得事件，焦点变化后的迟到提交一律丢弃。
// 溢出时清空整批并报告失败，避免半截文本或没有配对的按键继续进入输入框。
class EventQueue {
public:
    bool push(InputEvent event) {
        std::lock_guard lock(mutex_);
        if (events_.size() >= 4096) { events_.clear(); overflow_ = true; return false; }
        if (overflow_) return false;
        events_.push_back(std::move(event));
        return true;
    }
    std::vector<InputEvent> drain(std::uint64_t session, bool& overflow) {
        std::lock_guard lock(mutex_);
        std::vector<InputEvent> result;
        overflow = overflow_; overflow_ = false;
        while (!events_.empty()) {
            auto event = std::move(events_.front()); events_.pop_front();
            if (session != 0 && event.session == session) result.push_back(std::move(event));
        }
        return result;
    }
private:
    std::mutex mutex_;
    std::deque<InputEvent> events_;
    bool overflow_{};
};

// IMM 确认结果只提交一次；匹配的尾随 WM_CHAR 被抵消，WM_IME_CHAR 始终不转发。
// 下一次普通按键或超时清除抵消状态，不能吞掉后续英文字符。
class CommitDeduper {
public:
    void committed(std::wstring text, std::uint64_t now) { remaining_ = std::move(text); at_ = 0; expires_ = now + 250; }
    void new_key() { remaining_.clear(); at_ = 0; }
    bool suppress(wchar_t character, std::uint64_t now) {
        if (now > expires_ || at_ >= remaining_.size() || remaining_[at_] != character) { new_key(); return false; }
        ++at_; return true;
    }
private:
    std::wstring remaining_;
    std::size_t at_{};
    std::uint64_t expires_{};
};

// UTF-16 解码为 Unicode 码点；代理先合并跨消息代理对，孤立代理项不会进入输入框。
inline std::vector<std::uint32_t> codepoints(std::wstring_view text) {
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t value = static_cast<std::uint16_t>(text[i]);
        if (value >= 0xd800 && value <= 0xdbff) {
            if (i + 1 == text.size()) continue;
            const auto low = static_cast<std::uint16_t>(text[i + 1]);
            if (low < 0xdc00 || low > 0xdfff) continue;
            value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00; ++i;
        } else if (value >= 0xdc00 && value <= 0xdfff) continue;
        if (value >= 0x20 && value != 0x7f) result.push_back(value);
    }
    return result;
}

// 保留事件先后顺序；键盘屏障未被宿主消费时不能让后面的文字越过 Ctrl+A、方向键等编辑操作。
class OrderedEvents {
public:
    void reset(std::uint64_t session) { session_ = session; events_.clear(); }
    bool append(std::vector<InputEvent> events) {
        if (events_.size() + events.size() > 4096) { events_.clear(); return false; }
        for (auto& event : events) if (event.session == session_ && session_ != 0) events_.push_back(std::move(event));
        return true;
    }
    bool empty() const { return events_.empty(); }
    const InputEvent& front() const { return events_.front(); }
    void pop() { events_.pop_front(); }
private:
    std::uint64_t session_{};
    std::deque<InputEvent> events_;
};
}

