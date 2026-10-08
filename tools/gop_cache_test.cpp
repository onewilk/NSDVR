// GopCache / ForEachNal 单元测试：c++ -std=c++17 core/*.cpp tools/gop_cache_test.cpp && ./a.out
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "../core/gop_cache.h"
#include "../core/sysdvr_protocol.h"

using namespace sysdvr;
using Bytes = std::vector<uint8_t>;

static int g_failed = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                \
        }                                                              \
    } while (0)

static Bytes Nal(uint8_t header, size_t payload, uint8_t fill, bool fourByteStart = true) {
    Bytes b = fourByteStart ? Bytes{0, 0, 0, 1} : Bytes{0, 0, 1};
    b.push_back(header);
    b.insert(b.end(), payload, fill);
    return b;
}
static Bytes Cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
static void Push(GopCache& c, const Bytes& b, uint64_t ts) { c.Push(b.data(), b.size(), ts); }

int main() {
    const Bytes sps = Nal(0x67, 10, 0x11), pps = Nal(0x68, 3, 0x22);
    const Bytes sps2 = Nal(0x27, 12, 0x33), pps2 = Nal(0x28, 4, 0x44);  // nal_ref_idc 不同也要识别
    const Bytes idr = Nal(0x65, 500, 0x55), idr2 = Nal(0x65, 400, 0x66, false);
    const Bytes p1 = Nal(0x41, 100, 0x77), p2 = Nal(0x41, 120, 0x78, false);

    std::printf("ForEachNal\n");
    {
        const Bytes stream = Cat({sps, pps, idr, p2});
        std::vector<int> types;
        std::vector<size_t> lens;
        ForEachNal(stream.data(), stream.size(), [&](const uint8_t*, size_t len, int t) {
            types.push_back(t);
            lens.push_back(len);
        });
        CHECK((types == std::vector<int>{7, 8, 5, 1}));
        CHECK((lens == std::vector<size_t>{sps.size(), pps.size(), idr.size(), p2.size()}));
    }

    std::printf("P 帧在首个 IDR 之前不缓存\n");
    {
        GopCache c;
        Push(c, p1, 1);
        CHECK(c.Packets() == 0);
        CHECK(c.Snapshot().empty());
    }

    std::printf("IDR 带参数集：原样缓存，后续 P 帧追加\n");
    {
        GopCache c;
        const Bytes first = Cat({sps, pps, idr});
        Push(c, first, 100);
        Push(c, p1, 133);
        Push(c, p2, 166);
        const auto snap = c.Snapshot();
        CHECK(snap.size() == 3);
        CHECK(snap[0].data == first);
        CHECK(snap[2].timestampUs == 166);
        CHECK(c.ParamSets() == Cat({sps, pps}));
    }

    std::printf("新 IDR 不带参数集：重置缓存，快照补上最近的 SPS/PPS\n");
    {
        GopCache c;
        Push(c, Cat({sps, pps, idr}), 1);
        Push(c, p1, 2);
        Push(c, Cat({sps2, pps2}), 3);  // 单独到达的参数集也要记住
        Push(c, idr2, 4);
        Push(c, p2, 5);
        const auto snap = c.Snapshot();
        CHECK(snap.size() == 2);
        CHECK(snap[0].data == Cat({sps2, pps2, idr2}));
        CHECK(snap[0].timestampUs == 4);
    }

    std::printf("从没见过参数集：用 Switch 固定 SPS/PPS\n");
    {
        GopCache c;
        Push(c, idr, 1);
        const auto snap = c.Snapshot();
        Bytes expect(std::begin(kSps), std::end(kSps));
        expect.insert(expect.end(), std::begin(kPps), std::end(kPps));
        expect.insert(expect.end(), idr.begin(), idr.end());
        CHECK(snap.size() == 1 && snap[0].data == expect);
    }

    std::printf("超过上限：放弃当前 GOP，直到下一个 IDR\n");
    {
        GopCache c(2000, 5);
        Push(c, idr, 1);
        for (int i = 0; i < 5; ++i) Push(c, p1, 2 + i);  // 第 6 包超出 5 包上限
        CHECK(c.Packets() == 0);
        Push(c, p1, 10);
        CHECK(c.Packets() == 0);
        Push(c, idr2, 11);
        CHECK(c.Packets() == 1);

        GopCache bySize(2000, 100);
        Push(bySize, idr, 1);  // 505 字节
        for (int i = 0; i < 14; ++i) Push(bySize, p1, 2 + i);  // 每包 105 字节，累计 1975
        CHECK(bySize.Packets() == 15);
        Push(bySize, p1, 20);  // 2080 > 2000
        CHECK(bySize.Packets() == 0);
    }

    std::printf(g_failed ? "失败 %d 项\n" : "全部通过\n", g_failed);
    return g_failed ? 1 : 0;
}
