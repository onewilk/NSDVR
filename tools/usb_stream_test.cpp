// UsbStreamClient 单元测试：用内存里的 FakeSwitch 模拟 sysmodule 的 USB 行为（hello 循环、握手、每包一次 bulk 传输）。
// 覆盖：握手请求内容、残留数据跳过、粘包/拆包、NAL 重放、错位重同步、收流中途 Switch 重新发 hello、协议 02、设备断开后能停下。
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/log.h"
#include "../core/usb_stream.h"

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

static void PutU32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
static void PutU64(Bytes& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

static Bytes Packet(uint8_t meta, const Bytes& payload, uint64_t ts, uint8_t slot = kNoReplaySlot) {
    Bytes b;
    PutU32(b, kPacketMagic);
    PutU32(b, uint32_t(payload.size()));
    PutU64(b, ts);
    b.push_back(meta);
    b.push_back(slot);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

static Bytes VideoPayload(int i, size_t size) {
    Bytes b = {0, 0, 0, 1, uint8_t(i % 30 == 0 ? 0x65 : 0x41)};
    for (size_t k = 0; b.size() < size; ++k) b.push_back(uint8_t(i * 7 + k * 13));
    return b;
}

// 模拟 Switch：IN 端点是一个传输队列，每个元素就是一次 bulk 传输
class FakeSwitch : public UsbTransport {
public:
    struct Chunk {
        Bytes data;
        bool hello = false;
    };
    // 第 n 次握手成功后要发出的传输（n 从 1 开始）
    std::function<void(FakeSwitch&, int)> onConnected;
    std::string protocol = "03";
    Bytes lastRequest;
    int connections = 0;
    bool unplugged = false;

    void Push(Bytes b) { in_.push_back({std::move(b), false}); }
    void PushHello() { in_.push_back({Hello(), true}); }

    int Read(uint8_t* buf, size_t capacity, int) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (unplugged) return -1;
        if (in_.empty()) {
            if (state_ == State::Idle) {
                in_.push_back({Hello(), true});  // 没人连接时 sysmodule 一直在发 hello
            } else {
                // 先放锁再等：持锁睡眠、醒来立刻再抢锁，测试线程（拔线、推数据）可能被饿上好几秒
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return 0;
            }
        }
        Chunk& c = in_.front();
        const size_t n = std::min(capacity, c.data.size());
        std::memcpy(buf, c.data.data(), n);
        if (c.hello) state_ = State::WaitingRequest;
        if (n == c.data.size())
            in_.pop_front();
        else
            c.data.erase(c.data.begin(), c.data.begin() + long(n));
        return int(n);
    }

    int Write(const uint8_t* buf, size_t length, int) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (unplugged) return -1;
        if (state_ != State::WaitingRequest) return 0;  // sysmodule 没在读，写不进去
        state_ = State::Streaming;
        lastRequest.assign(buf, buf + length);
        Bytes resp;
        PutU32(resp, kHandshakeOk);
        if (protocol == "03" && length >= 10 && (buf[9] & 2)) {
            // 请求了内存报告（MemoryDiag）：QueryResult = 0，四个内存池的 size/used
            PutU32(resp, 0);
            const uint64_t mb = 1024 * 1024;
            for (uint64_t v : {3200 * mb, 1800 * mb, 512 * mb, 400 * mb, 64 * mb, 60 * mb, 32 * mb, 10 * mb})
                for (int i = 0; i < 8; ++i) resp.push_back(uint8_t(v >> (8 * i)));
        } else if (protocol == "03") {
            resp.resize(72, 0xFF);
        }
        in_.push_back({resp, false});
        ++connections;
        if (onConnected) onConnected(*this, connections);
        return int(length);
    }

    std::mutex mutex_;

private:
    Bytes Hello() const {
        std::string s = "SysDVR|" + protocol;
        Bytes b(s.begin(), s.end());
        b.push_back(0);
        return b;
    }
    std::deque<Chunk> in_;
    enum class State { Idle, WaitingRequest, Streaming } state_ = State::Idle;
};

struct Received {
    std::mutex m;
    Bytes video, audio;
    int videoPackets = 0, audioPackets = 0;
    std::vector<std::string> status;
};

static std::unique_ptr<UsbStreamClient> MakeClient(FakeSwitch* sw, Received* r, bool audio = true) {
    StreamOptions opt;
    opt.audio = audio;
    opt.audioBatching = 1;
    StreamCallbacks cb;
    cb.onVideo = [r](const uint8_t* p, size_t n, uint64_t) {
        std::lock_guard<std::mutex> lock(r->m);
        r->video.insert(r->video.end(), p, p + n);
        ++r->videoPackets;
    };
    cb.onAudio = [r](const uint8_t* p, size_t n, uint64_t) {
        std::lock_guard<std::mutex> lock(r->m);
        r->audio.insert(r->audio.end(), p, p + n);
        ++r->audioPackets;
    };
    cb.onStatus = [r](const std::string& s) {
        std::lock_guard<std::mutex> lock(r->m);
        r->status.push_back(s);
    };
    return std::make_unique<UsbStreamClient>(std::unique_ptr<UsbTransport>(sw), opt, cb);
}

static bool WaitFor(Received* r, int videoPackets, int audioPackets, int timeoutMs = 5000) {
    for (int t = 0; t < timeoutMs; t += 10) {
        {
            std::lock_guard<std::mutex> lock(r->m);
            if (r->videoPackets >= videoPackets && r->audioPackets >= audioPackets) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// 生成一段码流：video 个视频包（每 10 个里有 1 个重放包）+ 每 3 个视频包 1 个音频包。
// 按不同方式切成 bulk 传输：大多数一包一次，每 7 包把两包粘成一次，每 11 包拆成两次。
struct Stream {
    std::vector<Bytes> transfers;
    std::vector<bool> endsAtPacket;  // 这次传输结束处正好是包边界（拆包的前半截不是）
    Bytes expectVideo, expectAudio;
    int videoPackets = 0, audioPackets = 0;
};
static Stream MakeStream(int video, int seed) {
    Stream s;
    std::vector<Bytes> packets;
    Bytes slotContent[kReplaySlots];
    for (int i = 0; i < video; ++i) {
        const uint64_t ts = uint64_t(seed * 1000000 + i * 33333);
        const uint8_t slot = uint8_t((i / 2) % kReplaySlots);
        if (i % 10 == 9 && !slotContent[slot].empty()) {
            packets.push_back(Packet(kMetaVideo | kMetaReplay, {}, ts, slot));
            s.expectVideo.insert(s.expectVideo.end(), slotContent[slot].begin(), slotContent[slot].end());
        } else {
            // 大小覆盖小包、接近 512 整数倍、以及上百 KB 的关键帧
            const size_t size = i % 30 == 0 ? 150000 + size_t(seed) * 100 : 512 * size_t(1 + i % 5) - kHeaderSize;
            Bytes payload = VideoPayload(i + seed * 1000, size);
            packets.push_back(Packet(kMetaVideo, payload, ts, slot));
            slotContent[slot] = payload;
            s.expectVideo.insert(s.expectVideo.end(), payload.begin(), payload.end());
        }
        ++s.videoPackets;
        if (i % 3 == 0) {
            Bytes pcm(0x1000 * 2);
            for (size_t k = 0; k < pcm.size(); ++k) pcm[k] = uint8_t(i + k + seed);
            packets.push_back(Packet(kMetaAudio, pcm, ts));
            s.expectAudio.insert(s.expectAudio.end(), pcm.begin(), pcm.end());
            ++s.audioPackets;
        }
    }
    for (size_t i = 0; i < packets.size(); ++i) {
        if (i % 7 == 6 && i + 1 < packets.size()) {
            Bytes glued = packets[i];
            glued.insert(glued.end(), packets[i + 1].begin(), packets[i + 1].end());
            s.transfers.push_back(glued);
            s.endsAtPacket.push_back(true);
            ++i;
        } else if (i % 11 == 10) {
            const auto& p = packets[i];
            const size_t half = p.size() / 2 < 10 ? 5 : p.size() / 2;  // 前半截可能比包头还短
            s.transfers.emplace_back(p.begin(), p.begin() + long(half));
            s.transfers.emplace_back(p.begin() + long(half), p.end());
            s.endsAtPacket.push_back(false);
            s.endsAtPacket.push_back(true);
        } else {
            s.transfers.push_back(packets[i]);
            s.endsAtPacket.push_back(true);
        }
    }
    return s;
}

static void TestNormal() {
    std::printf("• 握手 + 粘包/拆包 + 重放 + 残留数据跳过\n");
    auto* sw = new FakeSwitch;
    const Stream st = MakeStream(200, 1);
    // 上一次连接残留在端点里的数据包：客户端要跳过它，等到 hello 再握手
    sw->Push(Packet(kMetaVideo, VideoPayload(0, 100), 0));
    sw->onConnected = [&st](FakeSwitch& s, int) {
        for (const auto& t : st.transfers) s.Push(t);
    };
    Received r;
    auto client = MakeClient(sw, &r);
    client->Start();
    CHECK(WaitFor(&r, st.videoPackets, st.audioPackets));
    const BridgeStats stats = client->GetStats();
    client->Stop();

    CHECK(sw->connections == 1);
    // 请求：magic、协议号、音视频一起订阅、视频开重放+注入 SPS/PPS+只重放关键帧、音频合并 1 块
    const Bytes& q = sw->lastRequest;
    CHECK(q.size() == kRequestSize);
    CHECK(q[0] == 0xAA && q[1] == 0xAA && q[2] == 0xAA && q[3] == 0xAA);
    CHECK(q[4] == '0' && q[5] == '3');
    CHECK(q[6] == 0x3);
    CHECK(q[7] == 0x7);
    CHECK(q[8] == 1);
    CHECK((q[9] & 0x2) != 0);  // ExtraFeatureFlags_MemoryDiag：请求内存报告
    // 72 字节应答里的内存报告
    const uint64_t mb = 1024 * 1024;
    CHECK(stats.switchMemory.valid);
    CHECK(stats.switchMemory.queryResult == 0);
    CHECK(stats.switchMemory.applicationSize == 3200 * mb && stats.switchMemory.applicationUsed == 1800 * mb);
    CHECK(stats.switchMemory.appletSize == 512 * mb && stats.switchMemory.appletUsed == 400 * mb);
    CHECK(stats.switchMemory.systemSize == 64 * mb && stats.switchMemory.systemUsed == 60 * mb);
    CHECK(stats.switchMemory.systemUnsafeSize == 32 * mb && stats.switchMemory.systemUnsafeUsed == 10 * mb);
    CHECK(r.video == st.expectVideo);
    CHECK(r.audio == st.expectAudio);
    CHECK(stats.videoConnected);
    CHECK(stats.replayHits > 10);
    CHECK(stats.replayMisses == 0);
    CHECK(stats.resyncs == 0);
    std::printf("  视频 %d 包 %zu 字节、音频 %d 包，重放命中 %llu\n", r.videoPackets, r.video.size(), r.audioPackets,
                (unsigned long long)stats.replayHits);
}

static void TestReconnectAndResync() {
    std::printf("• 收流中途 Switch 超时断开重发 hello + 包间垃圾数据\n");
    auto* sw = new FakeSwitch;
    const Stream a = MakeStream(90, 2), b = MakeStream(60, 3);
    sw->onConnected = [&](FakeSwitch& s, int n) {
        const Stream& st = n == 1 ? a : b;
        size_t nextGarbage = 20;
        for (size_t i = 0; i < st.transfers.size(); ++i) {
            s.Push(st.transfers[i]);
            // 第一段里每隔约 40 次传输、在包边界处插一段垃圾（例如传输出错留下的半截数据）
            if (n == 1 && i >= nextGarbage && st.endsAtPacket[i]) {
                s.Push(Bytes{0x12, 0x34, 0xCC, 0x56, 0x00, 0x00, 0x01});
                nextGarbage += 40;
            }
        }
        if (n == 1) s.PushHello();  // Switch 发送超时 → DisconnectClient → 回到 hello 循环
    };
    Received r;
    auto client = MakeClient(sw, &r);
    client->Start();
    CHECK(WaitFor(&r, a.videoPackets + b.videoPackets, a.audioPackets + b.audioPackets));
    const BridgeStats stats = client->GetStats();
    client->Stop();

    Bytes expectVideo = a.expectVideo, expectAudio = a.expectAudio;
    expectVideo.insert(expectVideo.end(), b.expectVideo.begin(), b.expectVideo.end());
    expectAudio.insert(expectAudio.end(), b.expectAudio.begin(), b.expectAudio.end());
    CHECK(sw->connections == 2);
    CHECK(stats.reconnects == 1);
    CHECK(stats.resyncs >= 3);
    // 垃圾数据只会让紧挨着的包头错位，重同步后内容仍完整
    CHECK(r.video == expectVideo);
    CHECK(r.audio == expectAudio);
    std::printf("  握手 %d 次、重连 %llu 次、重同步 %llu 次，数据完整\n", sw->connections,
                (unsigned long long)stats.reconnects, (unsigned long long)stats.resyncs);
}

static void TestProtocol02AndMute() {
    std::printf("• 协议 02（4 字节应答）+ 本地静音\n");
    auto* sw = new FakeSwitch;
    sw->protocol = "02";
    const Stream st = MakeStream(60, 4);
    sw->onConnected = [&st](FakeSwitch& s, int) {
        for (const auto& t : st.transfers) s.Push(t);
    };
    Received r;
    auto client = MakeClient(sw, &r, /*audio=*/false);
    client->Start();
    CHECK(WaitFor(&r, st.videoPackets, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const BridgeStats stats = client->GetStats();
    client->Stop();
    CHECK(!stats.switchMemory.valid);  // 协议 02 的应答只有结果码
    CHECK((sw->lastRequest[9] & 0x2) == 0);  // 协议 02 不请求
    CHECK(sw->lastRequest.size() == kRequestSize && sw->lastRequest[4] == '0' && sw->lastRequest[5] == '2');
    CHECK(r.video == st.expectVideo);
    CHECK(r.audioPackets == 0);                            // 静音：不回调
    CHECK(stats.audioPackets == uint64_t(st.audioPackets));  // 但照常收下、计数
}

static void TestUnplug() {
    std::printf("• 拔线后读写报错，Stop 能及时返回\n");
    auto* sw = new FakeSwitch;
    sw->onConnected = [](FakeSwitch& s, int) {
        for (int i = 0; i < 5; ++i) s.Push(Packet(kMetaVideo, VideoPayload(i, 1000), uint64_t(i)));
    };
    Received r;
    auto client = MakeClient(sw, &r);
    client->Start();
    CHECK(WaitFor(&r, 5, 0));
    {
        std::lock_guard<std::mutex> lock(sw->mutex_);
        sw->unplugged = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!client->GetStats().videoConnected);
    const auto t0 = std::chrono::steady_clock::now();
    client->Stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 300);
    bool reported = false;
    for (const auto& s : r.status) reported |= s.find("断开") != std::string::npos;
    CHECK(reported);
}

int main() {
    // 只显示警告及以上，避免握手日志刷屏
    SetLogSink([](LogLevel level, const std::string& msg) {
        if (level >= LogLevel::Warn) std::fprintf(stderr, "%s\n", msg.c_str());
    });
    TestNormal();
    TestReconnectAndResync();
    TestProtocol02AndMute();
    TestUnplug();
    if (g_failed) {
        std::printf("✗ USB 串流测试失败 %d 项\n", g_failed);
        return 1;
    }
    std::printf("✓ USB 串流测试全部通过\n");
    return 0;
}
