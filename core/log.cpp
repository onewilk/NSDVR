#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sysdvr {

namespace {
std::mutex g_mutex;
std::function<void(LogLevel, const std::string&)> g_sink;
}  // namespace

void SetLogSink(std::function<void(LogLevel, const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void Logf(LogLevel level, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sink) {
        g_sink(level, buf);
    } else {
        static const char* kNames[] = {"D", "I", "W", "E"};
        std::fprintf(stderr, "[%s] %s\n", kNames[int(level)], buf);
    }
}

}  // namespace sysdvr
