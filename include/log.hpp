// Debug logging via console + file + OutputDebugStringW. No deps.
//
// Log levels (controlled by log_level in myosotis.ini):
//   0 = quiet  — only MYO_LOG_ERROR
//   1 = info   — MYO_LOG + MYO_LOG_ERROR (default)
//   2 = debug  — MYO_LOG_DEBUG + MYO_LOG + MYO_LOG_ERROR
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

// Current log level (set by log_init from config). 0=quiet, 1=info, 2=debug.
extern int g_log_level;

// Type-safe formatted log. Tag is a short literal ("scan", "guard", ...).
// Usage:  MYO_LOG("scan", "resolved {} names", count);
template <typename... Args>
inline void log_info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (g_log_level < 1) return;
    std::string msg = std::format("[Myosotis:{}] ", tag);
    msg += std::format(fmt, std::forward<Args>(args)...);
    log_raw(msg.c_str());
}

// Debug-level log. Only emitted at log_level >= 2.
template <typename... Args>
inline void log_debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (g_log_level < 2) return;
    std::string msg = std::format("[Myosotis:{}] ", tag);
    msg += std::format(fmt, std::forward<Args>(args)...);
    log_raw(msg.c_str());
}

// Error-level log. Always emitted regardless of level.
template <typename... Args>
inline void log_error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    std::string msg = std::format("[Myosotis:{}] ", tag);
    msg += std::format(fmt, std::forward<Args>(args)...);
    log_raw(msg.c_str());
}

}  // namespace myosotis

// Macros keep call sites terse. `tag` is a string literal so std::format_string's
// compile-time format check fires at every MYO_LOG site.
#define MYO_LOG(tag, ...)       ::myosotis::log_info(tag, __VA_ARGS__)
#define MYO_LOG_DEBUG(tag, ...) ::myosotis::log_debug(tag, __VA_ARGS__)
#define MYO_LOG_ERROR(tag, ...) ::myosotis::log_error(tag, __VA_ARGS__)
