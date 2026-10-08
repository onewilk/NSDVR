// 鸿蒙上的 USB 主机传输，供 sysdvr::UsbStreamClient 使用。两条路径：
// 1. UsbFsTransport：直接对 usbManager.getFileDescriptor() 返回的 usbfs 描述符发 ioctl
//    （与官方 Android 客户端 libusb_wrap_sys_device 同理），没有跨进程开销；
// 2. JsPumpTransport：如果系统不让应用对这个描述符发 ioctl，就退回到 ArkTS 的 usbManager.bulkTransfer：
//    ArkTS 循环读 IN 端点，把数据喂给这里；要写的数据通过线程安全函数通知 ArkTS 来取、发送后回报结果。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "usb_stream.h"

struct UsbProbeResult {
    bool ok = false;
    std::string serial;  // 形如 "SysDVR|6.3|03|XAW10000000000"
    std::string error;
    bool bulkRefused = false;  // 系统拒绝对 bulk 端点发 ioctl（EACCES 等），应改走兼容传输
};

// 在 fd 上 claim 接口，并用控制传输读出序列号字符串：既拿到设备信息，也验证了 ioctl 这条路能不能走
// epIn 有效时再试读一次 bulk 端点：有的系统只放行控制传输的 ioctl，bulk 不行就得走兼容传输
UsbProbeResult ProbeUsbFs(int fd, int interfaceId, int epIn = -1);

class UsbFsTransport : public sysdvr::UsbTransport {
public:
    // fd 归 ArkTS 侧的 USBDevicePipe 所有（closePipe 时关闭），这里不关闭
    UsbFsTransport(int fd, int interfaceId, uint8_t epIn, uint8_t epOut);
    ~UsbFsTransport() override;
    int Read(uint8_t* buf, size_t capacity, int timeoutMs) override;
    int Write(const uint8_t* buf, size_t length, int timeoutMs) override;
    std::string LastError() const override;

private:
    int Bulk(uint8_t ep, void* data, size_t length, int timeoutMs);
    void SetError(const char* what, int err);

    mutable std::mutex errorMutex_;
    std::string lastError_;

    const int fd_;
    const int interfaceId_;
    const uint8_t epIn_;
    const uint8_t epOut_;
    // 单次读取上限：内核按长度 kmalloc 缓冲区，太大容易分配失败；一个包比它大时分几次读，由流解析拼回去。
    // 遇到 EINVAL/ENOMEM 再降到 16KB
    size_t maxChunk_ = 128 * 1024;
};

class JsPumpTransport : public sysdvr::UsbTransport {
public:
    // 有数据要写时调用（在串流线程上），由 NAPI 层转成线程安全函数通知 ArkTS
    explicit JsPumpTransport(std::function<void()> wakeWriter) : wakeWriter_(std::move(wakeWriter)) {}

    // ---- 以下由 ArkTS 主线程经 NAPI 调用
    void Feed(const uint8_t* data, size_t length);
    void FeedError();
    bool TakeWrite(std::vector<uint8_t>* out);
    void WriteDone(int result);
    // 让阻塞中的读写立刻返回（停止串流前调用）
    void Close();

    // ---- 串流线程
    int Read(uint8_t* buf, size_t capacity, int timeoutMs) override;
    int Write(const uint8_t* buf, size_t length, int timeoutMs) override;

private:
    static constexpr size_t kMaxQueuedBytes = 8 << 20;

    std::function<void()> wakeWriter_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> in_;
    size_t queuedBytes_ = 0;
    size_t headOffset_ = 0;  // in_.front() 已读走的字节数
    std::vector<uint8_t> out_;
    bool outPending_ = false;  // 有数据等 ArkTS 来取
    bool writeDone_ = false;
    int writeResult_ = 0;
    bool error_ = false;
    bool closed_ = false;
};
