#include "usb_host.h"

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include "i18n.h"
#include "log.h"

using sysdvr::Logf;
using sysdvr::LogLevel;
using sysdvr::L;
using sysdvr::Format;

namespace {

constexpr size_t kSmallChunk = 16384;

int Control(int fd, uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, uint8_t* data,
            uint16_t length) {
    usbdevfs_ctrltransfer ctrl{};
    ctrl.bRequestType = requestType;
    ctrl.bRequest = request;
    ctrl.wValue = value;
    ctrl.wIndex = index;
    ctrl.wLength = length;
    ctrl.timeout = 1000;
    ctrl.data = data;
    return ioctl(fd, USBDEVFS_CONTROL, &ctrl);
}

// GET_DESCRIPTOR(STRING)：返回 UTF-16LE，这里只关心 ASCII
bool ReadStringDescriptor(int fd, uint8_t index, uint16_t lang, std::string* out) {
    uint8_t buf[255] = {};
    const int n = Control(fd, 0x80, 0x06, uint16_t(0x0300 | index), lang, buf, sizeof(buf));
    if (n < 2) return false;
    const int len = std::min<int>(n, buf[0]);
    out->clear();
    for (int i = 2; i + 1 < len; i += 2) out->push_back(buf[i + 1] == 0 ? char(buf[i]) : '?');
    return true;
}

}  // namespace

UsbProbeResult ProbeUsbFs(int fd, int interfaceId, int epIn) {
    UsbProbeResult r;
    unsigned int iface = unsigned(interfaceId);
    const bool claimed = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) == 0;
    if (!claimed && errno != EBUSY) {
        // EBUSY：可能已被 ArkTS 的 claimInterface 在同一个描述符上声明过，后面的传输仍然可以试
        r.error = Format(L("claim 接口失败：%s", "claim 介面失敗：%s", "failed to claim the interface: %s"), std::strerror(errno));
        return r;
    }
    // 探测失败时要把接口还回去：之后改走兼容传输，得由系统 USB 服务来 claim，
    // 我们占着的话服务的 claimInterface 会失败（实测返回 -1）
    auto fail = [&](const std::string& why) {
        r.error = why;
        if (claimed) ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
        return r;
    };
    uint8_t dev[18] = {};
    if (Control(fd, 0x80, 0x06, 0x0100, 0, dev, sizeof(dev)) < int(sizeof(dev)))
        return fail(Format(L("读取设备描述符失败：%s", "讀取裝置描述元失敗：%s", "failed to read the device descriptor: %s"),
                           std::strerror(errno)));
    const uint16_t vid = uint16_t(dev[8] | (dev[9] << 8));
    const uint16_t pid = uint16_t(dev[10] | (dev[11] << 8));
    const uint8_t serialIndex = dev[16];
    if (vid != 0x18D1 || pid != 0x4EE0) return fail(L("不是 SysDVR 设备", "不是 SysDVR 裝置", "not a SysDVR device"));
    uint8_t langs[4] = {};
    uint16_t lang = 0x0409;
    if (Control(fd, 0x80, 0x06, 0x0300, 0, langs, sizeof(langs)) >= 4) lang = uint16_t(langs[2] | (langs[3] << 8));
    if (serialIndex == 0 || !ReadStringDescriptor(fd, serialIndex, lang, &r.serial))
        return fail(Format(L("读取序列号失败：%s", "讀取序號失敗：%s", "failed to read the serial number: %s"), std::strerror(errno)));
    if (epIn >= 0) {
        // 试读一次 IN 端点（Switch 没连上客户端时在循环发 hello，读走一个也无妨；超时同样说明 ioctl 可用）。
        // 实测 Mate 60 Pro：控制传输允许，bulk 返回 EACCES（系统只放行了部分 usbfs ioctl）
        uint8_t buf[512];
        usbdevfs_bulktransfer bulk{};
        bulk.ep = unsigned(epIn);
        bulk.len = sizeof(buf);
        bulk.timeout = 300;
        bulk.data = buf;
        if (ioctl(fd, USBDEVFS_BULK, &bulk) < 0 && errno != ETIMEDOUT) {
            // bulkRefused 是给 ArkTS 判断“改走兼容传输”用的机器可读标志，不要依赖 error 文字（会随语言变化）
            r.bulkRefused = true;
            const int err = errno;
            return fail(Format(L("bulk 读取不可用：%s（errno %d）", "bulk 讀取不可用：%s（errno %d）",
                                 "bulk read not allowed: %s (errno %d)"),
                               std::strerror(err), err));
        }
    }
    r.ok = true;
    Logf(LogLevel::Info, "USB 探测成功：%s", r.serial.c_str());
    return r;
}

// ---------------------------------------------------------------- UsbFsTransport

UsbFsTransport::UsbFsTransport(int fd, int interfaceId, uint8_t epIn, uint8_t epOut)
    : fd_(fd), interfaceId_(interfaceId), epIn_(epIn), epOut_(epOut) {}

UsbFsTransport::~UsbFsTransport() {
    unsigned int iface = unsigned(interfaceId_);
    ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &iface);
}

void UsbFsTransport::SetError(const char* what, int err) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = Format(L("%s：%s（errno %d）", "%s：%s（errno %d）", "%s: %s (errno %d)"), what, std::strerror(err), err);
}

std::string UsbFsTransport::LastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

int UsbFsTransport::Bulk(uint8_t ep, void* data, size_t length, int timeoutMs) {
    usbdevfs_bulktransfer bulk{};
    bulk.ep = ep;
    bulk.len = unsigned(length);
    bulk.timeout = unsigned(timeoutMs);
    bulk.data = data;
    const int n = ioctl(fd_, USBDEVFS_BULK, &bulk);
    if (n >= 0) return n;
    return -errno;
}

int UsbFsTransport::Read(uint8_t* buf, size_t capacity, int timeoutMs) {
    // usbfs 的读取按 512 字节整包进行，缓冲区不是整数倍时会报溢出，向下取整
    const size_t len = std::min(capacity & ~size_t(511), maxChunk_);
    int n = Bulk(epIn_, buf, len, timeoutMs);
    if ((n == -EINVAL || n == -ENOMEM) && maxChunk_ > kSmallChunk) {
        maxChunk_ = kSmallChunk;
        Logf(LogLevel::Warn, "USB：内核不接受 %zu 字节的传输，改为每次 %zu 字节", len, maxChunk_);
        n = Bulk(epIn_, buf, std::min(len, maxChunk_), timeoutMs);
    }
    if (n >= 0) return n;
    if (n == -ETIMEDOUT) return 0;
    if (n == -EPIPE) {
        // 端点被 STALL：清掉再继续
        unsigned int ep = epIn_;
        ioctl(fd_, USBDEVFS_CLEAR_HALT, &ep);
        return 0;
    }
    Logf(LogLevel::Warn, "USB 读取失败：%s", std::strerror(-n));
    SetError(L("bulk 读取", "bulk 讀取", "bulk read"), -n);
    return -1;
}

int UsbFsTransport::Write(const uint8_t* buf, size_t length, int timeoutMs) {
    const int n = Bulk(epOut_, const_cast<uint8_t*>(buf), length, timeoutMs);
    if (n >= 0) return n;
    if (n == -ETIMEDOUT) return 0;
    Logf(LogLevel::Warn, "USB 写入失败：%s", std::strerror(-n));
    SetError(L("bulk 写入", "bulk 寫入", "bulk write"), -n);
    return -1;
}

// ---------------------------------------------------------------- JsPumpTransport

void JsPumpTransport::Feed(const uint8_t* data, size_t length) {
    if (length == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    // 串流线程卡住时别无限堆积：丢掉最旧的数据（之后靠包头重同步恢复）
    while (queuedBytes_ + length > kMaxQueuedBytes && !in_.empty()) {
        queuedBytes_ -= in_.front().size() - headOffset_;
        in_.pop_front();
        headOffset_ = 0;
    }
    in_.emplace_back(data, data + length);
    queuedBytes_ += length;
    cv_.notify_all();
}

void JsPumpTransport::FeedError() {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = true;
    cv_.notify_all();
}

bool JsPumpTransport::TakeWrite(std::vector<uint8_t>* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outPending_) return false;
    outPending_ = false;
    *out = out_;
    return true;
}

void JsPumpTransport::WriteDone(int result) {
    std::lock_guard<std::mutex> lock(mutex_);
    writeResult_ = result;
    writeDone_ = true;
    cv_.notify_all();
}

void JsPumpTransport::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

int JsPumpTransport::Read(uint8_t* buf, size_t capacity, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return closed_ || error_ || !in_.empty(); });
    if (closed_) return -1;
    if (in_.empty()) {
        if (!error_) return 0;
        error_ = false;  // 报一次错；设备重新插上后还能继续
        return -1;
    }
    // 一次只交出一个 ArkTS 读取块（相当于一次 bulk 传输），保持和 usbfs 路径一致的语义
    std::vector<uint8_t>& head = in_.front();
    const size_t n = std::min(capacity, head.size() - headOffset_);
    std::memcpy(buf, head.data() + headOffset_, n);
    headOffset_ += n;
    queuedBytes_ -= n;
    if (headOffset_ == head.size()) {
        in_.pop_front();
        headOffset_ = 0;
    }
    return int(n);
}

int JsPumpTransport::Write(const uint8_t* buf, size_t length, int timeoutMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return -1;
        out_.assign(buf, buf + length);
        outPending_ = true;
        writeDone_ = false;
    }
    if (wakeWriter_) wakeWriter_();
    std::unique_lock<std::mutex> lock(mutex_);
    // 多等一会：ArkTS 可能要等正在进行的 IN 读取超时后才能发出
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs + 1000), [this] { return closed_ || writeDone_; });
    if (closed_) return -1;
    if (!writeDone_) {
        outPending_ = false;
        return 0;
    }
    return writeResult_;
}
