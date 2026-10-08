// native 多语言单测：SetLanguage 切换后 L() 返回对应语言，Format 正确格式化长字符串
#include <cstdio>
#include <cstring>
#include <string>

#include "../core/i18n.h"
#include "../core/sysdvr_protocol.h"

using namespace sysdvr;

static int g_failed = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                \
        }                                                              \
    } while (0)

int main() {
    CHECK(Language() == kLangHans);  // 默认简体，桌面测试里的中文断言不受影响
    CHECK(std::strcmp(L("简", "繁", "en"), "简") == 0);
    SetLanguage(kLangHant);
    CHECK(std::strcmp(L("简", "繁", "en"), "繁") == 0);
    CHECK(std::strstr(HandshakeResultName(1), "用戶端") != nullptr);
    SetLanguage(kLangEn);
    CHECK(std::strcmp(L("简", "繁", "en"), "en") == 0);
    CHECK(std::strstr(HandshakeResultName(1), "client and sysmodule") != nullptr);
    CHECK(Format(L("%d 秒", "%d 秒", "%d s"), 3) == "3 s");
    SetLanguage(99);  // 非法值回到简体
    CHECK(Language() == kLangHans);
    const std::string longText(1000, 'x');
    CHECK(Format("%s!", longText.c_str()).size() == 1001);  // 超过内部 512 字节缓冲也不截断
    if (g_failed) {
        std::printf("✗ native 多语言单测失败 %d 项\n", g_failed);
        return 1;
    }
    std::printf("✓ native 多语言单测通过\n");
    return 0;
}
