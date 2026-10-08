// FrameGapDetector 单元测试：c++ -std=c++17 core/frame_loss.cpp tools/frame_loss_test.cpp && ./a.out
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "../core/frame_loss.h"

using namespace sysdvr;

static int g_failed = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                \
        }                                                              \
    } while (0)

// 按给定间隔喂 n 帧，返回累计检测到的丢帧数
static int Feed(FrameGapDetector& d, uint64_t* ts, int n, int64_t intervalUs, int64_t jitterUs = 0) {
    int lost = 0;
    for (int i = 0; i < n; ++i) {
        const int64_t j = jitterUs == 0 ? 0 : ((i * 7919) % (2 * jitterUs + 1)) - jitterUs;
        *ts += uint64_t(intervalUs + j);
        lost += d.OnFrame(*ts);
    }
    return lost;
}

static void TestSteady() {
    std::printf("30fps 稳定 / 带 ±8ms 抖动：不误报\n");
    FrameGapDetector d;
    uint64_t ts = 1'000'000;
    CHECK(Feed(d, &ts, 300, 33333) == 0);
    CHECK(Feed(d, &ts, 300, 33333, 8000) == 0);
    CHECK(d.IntervalUs() > 30000 && d.IntervalUs() < 37000);
}

static void TestSingleAndMultiGap() {
    std::printf("丢 1 帧 / 丢 3 帧：数量准确\n");
    FrameGapDetector d;
    uint64_t ts = 5'000'000;
    Feed(d, &ts, 60, 33333);
    ts += 2 * 33333;  // 中间少了 1 帧
    CHECK(d.OnFrame(ts) == 1);
    CHECK(Feed(d, &ts, 30, 33333) == 0);
    ts += 4 * 33333;  // 中间少了 3 帧
    CHECK(d.OnFrame(ts) == 3);
    CHECK(Feed(d, &ts, 30, 33333) == 0);
}

static void TestAdapts60fps() {
    std::printf("60fps：自适应帧间隔，丢 1 帧也能发现\n");
    FrameGapDetector d;
    uint64_t ts = 0;
    CHECK(Feed(d, &ts, 120, 16667) == 0);
    CHECK(d.IntervalUs() > 15000 && d.IntervalUs() < 18500);
    ts += 3 * 16667;  // 少 2 帧
    CHECK(d.OnFrame(ts) == 2);
}

static void TestBackwardsAndLongGap() {
    std::printf("时间戳回退不算丢帧；长时间断档按丢帧处理\n");
    FrameGapDetector d;
    uint64_t ts = 10'000'000;
    Feed(d, &ts, 60, 33333);
    ts = 200'000;  // 换游戏，时间戳重新开始
    CHECK(d.OnFrame(ts) == 0);
    CHECK(Feed(d, &ts, 60, 33333) == 0);
    ts += 3'000'000;  // 3 秒没有帧（网络长时间阻塞或回了主页）
    CHECK(d.OnFrame(ts) >= 1);
    CHECK(Feed(d, &ts, 30, 33333) == 0);
}

static void TestResetKeepsWorking() {
    std::printf("Reset 后第一帧只做基准\n");
    FrameGapDetector d;
    uint64_t ts = 0;
    Feed(d, &ts, 30, 33333);
    d.Reset();
    ts += 10 * 33333;
    CHECK(d.OnFrame(ts) == 0);
    CHECK(Feed(d, &ts, 30, 33333) == 0);
}

int main() {
    TestSteady();
    TestSingleAndMultiGap();
    TestAdapts60fps();
    TestBackwardsAndLongGap();
    TestResetKeepsWorking();
    if (g_failed) {
        std::printf("✗ %d 项失败\n", g_failed);
        return 1;
    }
    std::printf("✓ 丢帧检测单测全部通过\n");
    return 0;
}
