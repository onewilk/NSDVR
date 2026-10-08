#include "i18n.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace sysdvr {

namespace {
std::atomic<int> g_lang{kLangHans};
}

void SetLanguage(int lang) {
    g_lang = (lang == kLangHant || lang == kLangEn) ? lang : kLangHans;
}

int Language() { return g_lang; }

const char* L(const char* hans, const char* hant, const char* en) {
    switch (g_lang.load()) {
        case kLangHant: return hant;
        case kLangEn: return en;
        default: return hans;
    }
}

std::string Format(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return std::string();
    if (size_t(n) < sizeof(buf)) return std::string(buf, size_t(n));
    std::string out(size_t(n) + 1, '\0');
    va_start(args, fmt);
    std::vsnprintf(&out[0], out.size(), fmt, args);
    va_end(args);
    out.resize(size_t(n));
    return out;
}

}  // namespace sysdvr
