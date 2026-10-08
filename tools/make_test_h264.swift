// 用 macOS 自带的 VideoToolbox 生成 1280x720@30fps 的 H.264（Annex-B）测试流，不需要 ffmpeg。
// 配合 mock_sysdvr.py --h264 使用，没有 Switch 也能在鸿蒙手机上看到画面。
//
//   swift tools/make_test_h264.swift test.h264 [秒数，默认 20]
//
// 画面：上下滚动的亮度渐变 + 彩条 + 左右移动的白块 + 顶部帧号二进制条（便于肉眼判断卡顿/丢帧）
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

let args = CommandLine.arguments
let outPath = args.count > 1 ? args[1] : "test.h264"
let seconds = args.count > 2 ? (Int(args[2]) ?? 20) : 20
let width = 1280, height = 720, fps = 30

FileManager.default.createFile(atPath: outPath, contents: nil)
guard let out = FileHandle(forWritingAtPath: outPath) else { fatalError("无法写入 \(outPath)") }

// C 回调不能捕获上下文，状态通过 refcon 传进去
final class Writer {
    let out: FileHandle
    var frames = 0, keyframes = 0, bytes = 0
    let startCode = Data([0, 0, 0, 1])
    init(_ out: FileHandle) { self.out = out }

    func writeNal(_ bytes: UnsafeRawPointer, _ count: Int) {
        out.write(startCode)
        out.write(Data(bytes: bytes, count: count))
        self.bytes += 4 + count
    }

    func handle(_ sb: CMSampleBuffer) {
        var isKeyframe = true
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]],
           let first = attachments.first {
            isKeyframe = !((first[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)
        }
        // 关键帧前写 SPS/PPS，和 Switch 注入参数集的方式一致
        if isKeyframe, let format = CMSampleBufferGetFormatDescription(sb) {
            var count = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                               parameterSetSizeOut: nil, parameterSetCountOut: &count,
                                                               nalUnitHeaderLengthOut: nil)
            for i in 0..<count {
                var ptr: UnsafePointer<UInt8>?
                var size = 0
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &ptr,
                                                                   parameterSetSizeOut: &size, parameterSetCountOut: nil,
                                                                   nalUnitHeaderLengthOut: nil)
                if let ptr = ptr { writeNal(ptr, size) }
            }
            keyframes += 1
        }

        // VideoToolbox 输出 AVCC（4 字节大端长度前缀），转成 Annex-B 起始码
        guard let block = CMSampleBufferGetDataBuffer(sb) else { return }
        let total = CMBlockBufferGetDataLength(block)
        var data = [UInt8](repeating: 0, count: total)
        CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: total, destination: &data)
        var offset = 0
        while offset + 4 <= total {
            let len = Int(data[offset]) << 24 | Int(data[offset + 1]) << 16 | Int(data[offset + 2]) << 8 | Int(data[offset + 3])
            guard offset + 4 + len <= total else { break }
            data.withUnsafeBytes { writeNal($0.baseAddress! + offset + 4, len) }
            offset += 4 + len
        }
        frames += 1
    }
}

let writer = Writer(out)
let outputCallback: VTCompressionOutputCallback = { refcon, _, status, _, sampleBuffer in
    guard status == noErr, let refcon = refcon, let sb = sampleBuffer, CMSampleBufferDataIsReady(sb) else { return }
    Unmanaged<Writer>.fromOpaque(refcon).takeUnretainedValue().handle(sb)
}

var sessionOut: VTCompressionSession?
var rc = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height),
                                    codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                                    imageBufferAttributes: nil, compressedDataAllocator: nil,
                                    outputCallback: outputCallback, refcon: Unmanaged.passUnretained(writer).toOpaque(), compressionSessionOut: &sessionOut)
guard rc == noErr, let session = sessionOut else { fatalError("VTCompressionSessionCreate 失败：\(rc)") }
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)  // 无 B 帧
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 60 as CFNumber)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: 5_000_000 as CFNumber)
VTCompressionSessionPrepareToEncodeFrames(session)

// NV12：Y 平面按行 memset，UV 平面预先做好一行彩条模板再逐行拷贝，速度足够快
let barColors: [(UInt8, UInt8)] = [(128, 128), (16, 146), (166, 16), (54, 34), (202, 222), (90, 240), (240, 110), (128, 128)]
var uvTemplate = [UInt8](repeating: 128, count: width)
for x in stride(from: 0, to: width, by: 2) {
    let bar = barColors[x * barColors.count / width]
    uvTemplate[x] = bar.0
    uvTemplate[x + 1] = bar.1
}

let totalFrames = seconds * fps
for i in 0..<totalFrames {
    var pbOut: CVPixelBuffer?
    CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nil, &pbOut)
    guard let pb = pbOut else { fatalError("CVPixelBufferCreate 失败") }
    CVPixelBufferLockBaseAddress(pb, [])

    let y = CVPixelBufferGetBaseAddressOfPlane(pb, 0)!
    let yStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
    let boxX = (i * 12) % (width - 160)
    for row in 0..<height {
        let line = y + row * yStride
        memset(line, Int32(16 + ((row + i * 4) % height) * 200 / height), width)
        if row >= 280 && row < 440 { memset(line + boxX, 235, 160) }
        // 顶部 16 位帧号，每位 80 像素宽，1 = 白 0 = 黑
        if row < 24 {
            for bit in 0..<16 { memset(line + bit * 80, (i >> (15 - bit)) & 1 == 1 ? 235 : 16, 76) }
        }
    }
    let uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1)!
    let uvStride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1)
    for row in 0..<(height / 2) {
        let line = uv + row * uvStride
        if row < 12 || (row >= 140 && row < 220) {
            memset(line, 128, width)  // 帧号条和白块区域保持无色
        } else {
            uvTemplate.withUnsafeBytes { _ = memcpy(line, $0.baseAddress!, width) }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, [])

    rc = VTCompressionSessionEncodeFrame(session, imageBuffer: pb,
                                         presentationTimeStamp: CMTime(value: CMTimeValue(i), timescale: CMTimeScale(fps)),
                                         duration: CMTime(value: 1, timescale: CMTimeScale(fps)),
                                         frameProperties: nil, sourceFrameRefcon: nil, infoFlagsOut: nil)
    if rc != noErr { fatalError("编码第 \(i) 帧失败：\(rc)") }
}
VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
VTCompressionSessionInvalidate(session)
out.closeFile()
print("已生成 \(outPath)：\(writer.frames) 帧，\(writer.keyframes) 个关键帧，\(writer.bytes / 1024) KB")
