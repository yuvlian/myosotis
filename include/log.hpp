// Debug logging via OutputDebugStringW. No deps.
//
// C++23: the public API is a type-safe std::format-based variadic template.
// This removes the old snprintf variadic forwarder and its -Wformat-security
// warning entirely — arguments are checked at compile time by std::format.
#pragma once
#include <string>
#include <string_view>
#include <format>

namespace myosotis {

void log_init();
void log_raw(const char* msg);
void log_raw(const wchar_t* msg);

// Type-safe formatted log. Tag is a short literal ("scan", "guard", ...).
// Usage:  MYO_LOG("scan", "resolved {} names", count);
template <typename... Args>
inline void log_info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format("[Myosotis:{}] ", tag);
    msg += std::format(fmt, std::forward<Args>(args)...);
    log_raw(msg.c_str());
}

}  // namespace myosotis

// Macro keeps call sites terse. `tag` is a string literal so std::format_string's
// compile-time format check fires at every MYO_LOG site.
#define MYO_LOG(tag, ...) ::myosotis::log_info(tag, __VA_ARGS__)
