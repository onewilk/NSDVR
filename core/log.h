#pragma once

#include <functional>
#include <string>

namespace sysdvr {

enum class LogLevel { Debug, Info, Warn, Error };

// 默认输出到 stderr；鸿蒙侧替换为 hilog
void SetLogSink(std::function<void(LogLevel, const std::string&)> sink);
void Logf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace sysdvr
