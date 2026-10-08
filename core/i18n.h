// 面向用户的状态文字多语言：0 简体中文（默认）/ 1 繁體中文 / 2 English。
// 用法：Status(L("正在连接 %s", "正在連線 %s", "Connecting to %s"), host)——三种写法的占位符必须一致。
// 只给界面上显示的文字用；只进 hilog 的开发日志（Logf）保持中文。
#pragma once

#include <string>

namespace sysdvr {

enum : int { kLangHans = 0, kLangHant = 1, kLangEn = 2 };

void SetLanguage(int lang);
int Language();

// 按当前语言选一个；返回的就是传进来的指针之一（字符串字面量），不用释放
const char* L(const char* hans, const char* hant, const char* en);

// printf 风格格式化成 std::string，配合 L() 使用：Format(L("…%d…", "…%d…", "…%d…"), n)
std::string Format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace sysdvr
