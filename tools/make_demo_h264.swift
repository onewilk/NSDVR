// 生成应用市场截图用的“示意画面”：原创的横版小游戏风格场景（不含任何真实游戏或任天堂素材），
// 1280x720@30fps H.264（Annex-B），规格和 SysDVR 实际输出一致。配合 mock_sysdvr.py --h264 推流到真机截图。
//
//   swift tools/make_demo_h264.swift demo.h264 [秒数，默认 10] [角标文字，默认“示意画面”]
import AppKit
import CoreMedia
import CoreVideo
import Foundation
import VideoToolbox

let args = CommandLine.arguments
let outPath = args.count > 1 ? args[1] : "demo.h264"
let seconds = args.count > 2 ? (Int(args[2]) ?? 10) : 10
let label = args.count > 3 ? args[3] : "示意画面"
let width = 1280, height = 720, fps = 30

FileManager.default.createFile(atPath: outPath, contents: nil)
guard let out = FileHandle(forWritingAtPath: outPath) else { fatalError("无法写入 \(outPath)") }

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

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: a)
}

/// 把 x 按周期 period 折回 [-margin, period - margin)，用于无限滚动的背景元素
func wrap(_ x: CGFloat, _ period: CGFloat, _ margin: CGFloat = 200) -> CGFloat {
    var v = (x + margin).truncatingRemainder(dividingBy: period)
    if v < 0 { v += period }
    return v - margin
}

func cloud(_ c: CGContext, _ x: CGFloat, _ y: CGFloat, _ s: CGFloat) {
    c.setFillColor(rgb(0xFFFFFF, 0.95))
    for (dx, dy, r) in [(0.0, 0.0, 34.0), (38.0, -14.0, 42.0), (80.0, 0.0, 32.0), (40.0, 12.0, 30.0)] {
        c.fillEllipse(in: CGRect(x: x + CGFloat(dx) * s - CGFloat(r) * s, y: y + CGFloat(dy) * s - CGFloat(r) * s,
                                 width: CGFloat(r) * 2 * s, height: CGFloat(r) * 2 * s))
    }
}

/// 用一串正弦叠加画一层连绵的山/丘，offset 控制视差滚动
func hills(_ c: CGContext, base: CGFloat, amp: CGFloat, freq: CGFloat, offset: CGFloat, color: CGColor) {
    c.setFillColor(color)
    c.beginPath()
    c.move(to: CGPoint(x: 0, y: CGFloat(height)))
    var x: CGFloat = 0
    while x <= CGFloat(width) {
        let t = (x + offset) * freq
        let y = base - amp * (0.6 * sin(t) + 0.3 * sin(t * 2.3 + 1.1) + 0.1 * sin(t * 5.1))
        c.addLine(to: CGPoint(x: x, y: y))
        x += 4
    }
    c.addLine(to: CGPoint(x: CGFloat(width), y: CGFloat(height)))
    c.closePath()
    c.fillPath()
}

func tree(_ c: CGContext, _ x: CGFloat, _ groundY: CGFloat, _ s: CGFloat) {
    c.setFillColor(rgb(0x7A4E2D))
    c.fill(CGRect(x: x - 6 * s, y: groundY - 40 * s, width: 12 * s, height: 40 * s))
    c.setFillColor(rgb(0x2E8B57))
    c.fillEllipse(in: CGRect(x: x - 34 * s, y: groundY - 100 * s, width: 68 * s, height: 70 * s))
    c.setFillColor(rgb(0x3FA96A))
    c.fillEllipse(in: CGRect(x: x - 24 * s, y: groundY - 96 * s, width: 40 * s, height: 40 * s))
}

/// 漂浮的草地小岛：上面一层草皮，下面圆底的土块
func island(_ c: CGContext, _ x: CGFloat, _ y: CGFloat, _ n: Int) {
    let w = CGFloat(n) * 56, h: CGFloat = 44
    c.addPath(CGPath(roundedRect: CGRect(x: x, y: y + 6, width: w, height: h), cornerWidth: 22, cornerHeight: 22,
                     transform: nil))
    c.setFillColor(rgb(0xA8744A))
    c.fillPath()
    c.setFillColor(rgb(0x8A5C37))
    c.fillEllipse(in: CGRect(x: x + w * 0.2, y: y + 30, width: w * 0.6, height: 34))
    c.addPath(CGPath(roundedRect: CGRect(x: x - 6, y: y, width: w + 12, height: 20), cornerWidth: 10, cornerHeight: 10,
                     transform: nil))
    c.setFillColor(rgb(0x6BCB3F))
    c.fillPath()
    c.setFillColor(rgb(0x8BE05A))
    c.fill(CGRect(x: x + 4, y: y + 2, width: w - 8, height: 5))
}

/// 宝石：菱形，phase 控制闪光
func gem(_ c: CGContext, _ x: CGFloat, _ y: CGFloat, _ phase: CGFloat) {
    func diamond(_ r: CGFloat, _ color: CGColor) {
        c.setFillColor(color)
        c.beginPath()
        c.move(to: CGPoint(x: x, y: y - r * 1.2))
        c.addLine(to: CGPoint(x: x + r, y: y))
        c.addLine(to: CGPoint(x: x, y: y + r * 1.2))
        c.addLine(to: CGPoint(x: x - r, y: y))
        c.closePath()
        c.fillPath()
    }
    diamond(15, rgb(0x7B5CFF))
    diamond(9, rgb(0xB9A6FF))
    c.setFillColor(rgb(0xFFFFFF, 0.5 + 0.5 * abs(sin(phase))))
    c.fillEllipse(in: CGRect(x: x - 7, y: y - 10, width: 5, height: 5))
}

/// 原创角色：圆角方块小机器人（青色机身、深色面罩、天线）
func robot(_ c: CGContext, _ x: CGFloat, _ footY: CGFloat, _ t: CGFloat) {
    let bodyW: CGFloat = 76, bodyH: CGFloat = 70
    let top = footY - bodyH - 14
    // 影子
    c.setFillColor(rgb(0x000000, 0.18))
    c.fillEllipse(in: CGRect(x: x - 34, y: 604 - 6, width: 68, height: 12))
    // 脚
    let step = sin(t * 0.6) * 6
    c.setFillColor(rgb(0x1F6F78))
    c.fill(CGRect(x: x - 26 + step, y: footY - 16, width: 18, height: 16))
    c.fill(CGRect(x: x + 8 - step, y: footY - 16, width: 18, height: 16))
    // 机身
    let body = CGPath(roundedRect: CGRect(x: x - bodyW / 2, y: top, width: bodyW, height: bodyH), cornerWidth: 18,
                      cornerHeight: 18, transform: nil)
    c.addPath(body)
    c.setFillColor(rgb(0x2EC4B6))
    c.fillPath()
    c.addPath(body)
    c.setStrokeColor(rgb(0x17857B))
    c.setLineWidth(4)
    c.strokePath()
    // 面罩和眼睛
    let visor = CGPath(roundedRect: CGRect(x: x - 26, y: top + 14, width: 52, height: 28), cornerWidth: 12,
                       cornerHeight: 12, transform: nil)
    c.addPath(visor)
    c.setFillColor(rgb(0x14323A))
    c.fillPath()
    c.setFillColor(rgb(0x7CF7FF))
    c.fillEllipse(in: CGRect(x: x - 16, y: top + 21, width: 12, height: 14))
    c.fillEllipse(in: CGRect(x: x + 4, y: top + 21, width: 12, height: 14))
    // 天线
    c.setStrokeColor(rgb(0x17857B))
    c.setLineWidth(4)
    c.move(to: CGPoint(x: x, y: top))
    c.addLine(to: CGPoint(x: x, y: top - 18))
    c.strokePath()
    c.setFillColor(rgb(0xFFD84A))
    c.fillEllipse(in: CGRect(x: x - 8, y: top - 30, width: 16, height: 16))
}

func text(_ s: String, _ x: CGFloat, _ y: CGFloat, _ size: CGFloat, _ color: NSColor, bold: Bool = true) -> CGSize {
    let font = NSFont(name: bold ? "PingFangSC-Semibold" : "PingFangSC-Regular", size: size)
        ?? NSFont.boldSystemFont(ofSize: size)
    let str = NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: color])
    str.draw(at: NSPoint(x: x, y: y))
    return str.size()
}

func hudPill(_ c: CGContext, _ r: CGRect) {
    c.addPath(CGPath(roundedRect: r, cornerWidth: r.height / 2, cornerHeight: r.height / 2, transform: nil))
    c.setFillColor(rgb(0x000000, 0.35))
    c.fillPath()
}

func drawFrame(_ c: CGContext, _ i: Int) {
    let t = CGFloat(i)
    let W = CGFloat(width), H = CGFloat(height)
    // 天空
    let sky = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [rgb(0x3D7BE0), rgb(0xA8DCFF)] as CFArray,
                         locations: [0, 1])!
    c.drawLinearGradient(sky, start: CGPoint(x: 0, y: 0), end: CGPoint(x: 0, y: 470), options: [.drawsAfterEndLocation])
    // 太阳
    let glow = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                          colors: [rgb(0xFFF3B0, 0.9), rgb(0xFFF3B0, 0)] as CFArray, locations: [0, 1])!
    c.drawRadialGradient(glow, startCenter: CGPoint(x: 1030, y: 140), startRadius: 40, endCenter: CGPoint(x: 1030, y: 140),
                         endRadius: 150, options: [])
    c.setFillColor(rgb(0xFFE27A))
    c.fillEllipse(in: CGRect(x: 1030 - 56, y: 140 - 56, width: 112, height: 112))
    // 云
    for (k, (y, s)) in [(110.0, 1.0), (190.0, 0.7), (80.0, 0.8), (230.0, 1.1)].enumerated() {
        cloud(c, wrap(CGFloat(k) * 380 - t * 0.6, W + 400), CGFloat(y), CGFloat(s))
    }
    // 远山、中丘、近丘（视差）
    hills(c, base: 400, amp: 110, freq: 0.006, offset: t * 0.4, color: rgb(0x8EA4E6))
    hills(c, base: 470, amp: 60, freq: 0.01, offset: t * 1.0 + 300, color: rgb(0x6CC46F))
    for k in 0..<6 {
        let x = wrap(CGFloat(k) * 260 + 90 - t * 1.0, W + 400)
        let gy = 470 - 60 * (0.6 * sin((x + t * 1.0 + 300) * 0.01))
        tree(c, x, gy + 20, 0.8)
    }
    hills(c, base: 560, amp: 34, freq: 0.014, offset: t * 1.8 + 900, color: rgb(0x4DAA55))
    // 地面：草皮 + 土层，按 3 px/帧 滚动
    let groundY: CGFloat = 604
    c.setFillColor(rgb(0xA8744A))
    c.fill(CGRect(x: 0, y: groundY, width: W, height: H - groundY))
    // 土里零星的小石子，跟着地面滚动
    c.setFillColor(rgb(0x8A5C37))
    for k in 0..<14 {
        let x = wrap(CGFloat(k) * 97 - t * 3, W + 100, 50)
        c.fillEllipse(in: CGRect(x: x, y: groundY + 50 + CGFloat((k * 37) % 60), width: 22, height: 12))
    }
    c.setFillColor(rgb(0x6BCB3F))
    c.fill(CGRect(x: 0, y: groundY, width: W, height: 26))
    c.setFillColor(rgb(0x8BE05A))
    c.fill(CGRect(x: 0, y: groundY, width: W, height: 8))
    // 漂浮小岛和宝石
    let period: CGFloat = 1500
    for (k, (y, n)) in [(420.0, 3), (330.0, 2), (450.0, 4)].enumerated() {
        let x = wrap(CGFloat(k) * 520 + 520 - t * 3, period, 300)
        island(c, x, CGFloat(y), n)
        for m in 0..<n {
            gem(c, x + 28 + CGFloat(m) * 56, CGFloat(y) - 34 + 4 * sin(t * 0.15 + CGFloat(m)), t * 0.12 + CGFloat(m))
        }
    }
    // 角色：原地小跳
    let jump = abs(sin(t * 0.09)) * 90
    robot(c, 360, groundY - jump, t)

    // HUD
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: c, flipped: true)
    hudPill(c, CGRect(x: 32, y: 28, width: 170, height: 52))
    gem(c, 62, 54, 1.2)
    _ = text("× \(128 + i / 20)", 86, 36, 26, .white)
    // 分数放顶部正中：右上角在播放页会被“设置 / 断开”按钮盖住
    hudPill(c, CGRect(x: W / 2 - 115, y: 28, width: 230, height: 52))
    _ = text(String(format: "SCORE %06d", 2450 + i * 10), W / 2 - 95, 36, 26, .white)
    // “示意画面”角标
    let font = NSFont(name: "PingFangSC-Regular", size: 20) ?? NSFont.systemFont(ofSize: 20)
    let tag = NSAttributedString(string: label, attributes: [.font: font, .foregroundColor: NSColor(white: 1, alpha: 0.92)])
    let ts = tag.size()
    hudPill(c, CGRect(x: W - ts.width - 56, y: H - ts.height - 40, width: ts.width + 28, height: ts.height + 12))
    tag.draw(at: NSPoint(x: W - ts.width - 42, y: H - ts.height - 34))
    NSGraphicsContext.restoreGraphicsState()
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
                                    outputCallback: outputCallback, refcon: Unmanaged.passUnretained(writer).toOpaque(),
                                    compressionSessionOut: &sessionOut)
guard rc == noErr, let session = sessionOut else { fatalError("VTCompressionSessionCreate 失败：\(rc)") }
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanFalse)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)  // 无 B 帧
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 60 as CFNumber)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: 8_000_000 as CFNumber)
VTCompressionSessionPrepareToEncodeFrames(session)

let attrs: [CFString: Any] = [kCVPixelBufferCGImageCompatibilityKey: true, kCVPixelBufferCGBitmapContextCompatibilityKey: true]
for i in 0..<(seconds * fps) {
    var pbOut: CVPixelBuffer?
    CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pbOut)
    guard let pb = pbOut else { fatalError("CVPixelBufferCreate 失败") }
    CVPixelBufferLockBaseAddress(pb, [])
    let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb), width: width, height: height, bitsPerComponent: 8,
                        bytesPerRow: CVPixelBufferGetBytesPerRow(pb), space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    // 以左上角为原点绘制
    ctx.translateBy(x: 0, y: CGFloat(height))
    ctx.scaleBy(x: 1, y: -1)
    drawFrame(ctx, i)
    CVPixelBufferUnlockBaseAddress(pb, [])
    rc = VTCompressionSessionEncodeFrame(session, imageBuffer: pb,
                                         presentationTimeStamp: CMTime(value: CMTimeValue(i), timescale: CMTimeScale(fps)),
                                         duration: CMTime(value: 1, timescale: CMTimeScale(fps)),
                                         frameProperties: nil, sourceFrameRefcon: nil, infoFlagsOut: nil)
    if rc != noErr { fatalError("编码第 \(i) 帧失败：\(rc)") }
    // 预览：第 0 帧另存一张 PNG，方便不推流也能看效果
    if i == 0, args.count > 4 {
        let img = ctx.makeImage()!
        try! NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])!
            .write(to: URL(fileURLWithPath: args[4]))
    }
}
VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
VTCompressionSessionInvalidate(session)
out.closeFile()
print("已生成 \(outPath)：\(writer.frames) 帧，\(writer.keyframes) 个关键帧，\(writer.bytes / 1024) KB")
