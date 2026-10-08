// 平滑模式（FramePacer）模拟测试：c++ -std=c++17 core/frame_pacer.cpp tools/frame_pacer_test.cpp && ./a.out
// 生成 30fps 的帧，按不同网络模型模拟到达时间，比较“到了就显示”和“平滑排期”两种出帧方式。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

#include "../core/frame_pacer.h"

using sysdvr::FramePacer;

namespace {

constexpr int64_t kFrameUs = 33'333;

struct Net {
    const char* name;
    int64_t baseUs;       // 固定延迟
    int64_t jitterUs;     // 随机抖动上限（均匀分布）
    int64_t stallEveryUs; // 每隔多久卡一次（0 = 不卡）
    int64_t stallUs;      // 每次卡多久
};

struct Result {
    int frames = 0;
    int stutters = 0;          // 相邻两帧显示间隔 > 50ms 的次数（肉眼可见的卡顿）
    double intervalStdMs = 0;  // 显示间隔的标准差
    double meanLatencyMs = 0;  // 显示时间 - 帧时间戳
    double maxLatencyMs = 0;
    double finalTargetMs = 0;
    uint64_t late = 0;
};

std::vector<int64_t> Arrivals(const Net& net, int frames, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int64_t> jitter(0, net.jitterUs);
    std::vector<int64_t> arr(frames);
    int64_t last = 0;
    for (int i = 0; i < frames; ++i) {
        const int64_t pts = i * kFrameUs;
        int64_t t = pts + net.baseUs + (net.jitterUs ? jitter(rng) : 0);
        if (net.stallEveryUs > 0) {
            const int64_t phase = pts % net.stallEveryUs;
            const int64_t stallStart = net.stallEveryUs - net.stallUs;
            if (phase >= stallStart) t = std::max(t, pts - phase + net.stallEveryUs + net.baseUs);  // 卡住直到恢复
        }
        last = std::max(last, t);  // TCP 保序：不会比前一帧早到
        arr[i] = last;
    }
    return arr;
}

// level: 0 不平滑，1 游戏档（≤80ms），2 观看档（≤300ms）
Result Simulate(const Net& net, int level, int seconds) {
    const int frames = seconds * 30;
    const auto arrival = Arrivals(net, frames, 42);
    FramePacer pacer;
    pacer.SetEnabled(level > 0, 0);
    if (level == 1) pacer.SetLimits(20'000, 80'000, 15'000);

    std::deque<int> queue;
    std::vector<int64_t> shown(frames, -1);
    int next = 0;
    const int64_t endUs = arrival.back() + 2'000'000;
    for (int64_t t = 0; t <= endUs; t += 1000) {  // 1ms 步进
        while (next < frames && arrival[next] <= t) {
            pacer.OnArrival(t, next * kFrameUs);
            queue.push_back(next++);
        }
        while (!queue.empty()) {
            const int f = queue.front();
            const int64_t span = (queue.back() - f) * kFrameUs;
            const int64_t due = pacer.Schedule(t, f * kFrameUs, span);
            if (due > t) break;
            shown[f] = t;
            queue.pop_front();
        }
    }

    Result r;
    r.frames = frames;
    std::vector<double> intervals;
    double latSum = 0;
    for (int i = 0; i < frames; ++i) {
        const double lat = (shown[i] - i * kFrameUs) / 1000.0;
        latSum += lat;
        r.maxLatencyMs = std::max(r.maxLatencyMs, lat);
        if (i > 0) {
            const double iv = (shown[i] - shown[i - 1]) / 1000.0;
            intervals.push_back(iv);
            if (iv > 50) ++r.stutters;
        }
    }
    double mean = 0;
    for (double v : intervals) mean += v;
    mean /= intervals.size();
    double var = 0;
    for (double v : intervals) var += (v - mean) * (v - mean);
    r.intervalStdMs = std::sqrt(var / intervals.size());
    r.meanLatencyMs = latSum / frames;
    r.finalTargetMs = pacer.TargetDelayUs() / 1000.0;
    r.late = pacer.LateCount();
    return r;
}

int g_failed = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "✓" : "✗", what);
    if (!ok) ++g_failed;
}

void Print(const char* mode, const Result& r) {
    std::printf("  %-6s 卡顿 %4d 次 | 间隔标准差 %5.1f ms | 延迟 平均 %5.0f / 最大 %5.0f ms | 缓冲目标 %3.0f ms | 迟到 %llu\n",
                mode, r.stutters, r.intervalStdMs, r.meanLatencyMs, r.maxLatencyMs, r.finalTargetMs,
                (unsigned long long)r.late);
}

}  // namespace

int main() {
    const Net hotspot{"不稳定热点（抖动 0–120ms，每 10 秒卡 300ms）", 20'000, 120'000, 10'000'000, 300'000};
    const Net stable{"稳定 5GHz（固定 5ms，无抖动）", 5'000, 0, 0, 0};
    const Net mild{"一般 Wi-Fi（抖动 0–40ms）", 10'000, 40'000, 0, 0};

    for (const Net* net : {&hotspot, &mild, &stable}) {
        std::printf("\n[%s] 60 秒\n", net->name);
        const Result off = Simulate(*net, 0, 60);
        const Result game = Simulate(*net, 1, 60);
        const Result on = Simulate(*net, 2, 60);
        Print("不平滑", off);
        Print("游戏档", game);
        Print("观看档", on);
        Check(game.finalTargetMs <= 80, "游戏档缓冲不超过 80ms");
        Check(game.stutters <= off.stutters, "游戏档卡顿不多于不平滑");

        if (net == &hotspot) {
            Check(on.stutters * 5 < off.stutters, "卡顿次数降到不平滑时的 1/5 以下");
            Check(on.intervalStdMs < off.intervalStdMs / 2, "出帧间隔标准差减半以上");
            Check(on.finalTargetMs >= 100 && on.finalTargetMs <= 200, "缓冲自动适配到抖动水平（100–200ms）");
            Check(on.meanLatencyMs < 300, "观看档平均延迟不超过 300ms");
            Check(game.meanLatencyMs < 200, "游戏档平均延迟不超过 200ms");
        } else if (net == &stable) {
            Check(on.stutters == 0, "网络稳定时没有卡顿");
            Check(on.finalTargetMs <= 50, "网络稳定时缓冲自动降到 50ms 以内");
            Check(on.meanLatencyMs < 70, "网络稳定时平均延迟不超过 70ms");
        } else {
            Check(on.stutters * 10 < off.stutters, "一般网络下卡顿降到 1/10 以下");
            Check(on.meanLatencyMs < 120, "一般网络下平均延迟不超过 120ms");
        }
    }
    std::printf(g_failed ? "\n失败 %d 项\n" : "\n全部通过\n", g_failed);
    return g_failed ? 1 : 0;
}
