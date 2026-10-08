// 生成 NSDVR 应用图标（macOS 自带 swift 即可运行，不依赖第三方库）：
//   swift tools/make_icon.swift <输出目录>
// 输出：
//   foreground.png / background.png  1024×1024，鸿蒙分层图标的前景层和背景层
//   icon_1024.png / icon_512.png / icon_216.png / start_256.png  合成后的平面图标（应用市场上传、启动页等）
// 造型：原来的“录制”圆环 + 红点，圆环改用手柄的蓝色（不使用任何手柄外形，避免商品外观风险）。
import AppKit
import CoreGraphics
import Foundation

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
let size = 1024

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
}

let bgColor = color(0x222428)
let dotColor = color(0xE53030)
let joyBlue = color(0x00B2E3)

func makeContext(_ px: Int) -> CGContext {
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    // 以左上角为原点、1024 为单位坐标绘制
    ctx.translateBy(x: 0, y: CGFloat(px))
    ctx.scaleBy(x: CGFloat(px) / CGFloat(size), y: -CGFloat(px) / CGFloat(size))
    ctx.setShouldAntialias(true)
    return ctx
}

func save(_ ctx: CGContext, _ name: String) {
    let image = ctx.makeImage()!
    let rep = NSBitmapImageRep(cgImage: image)
    let data = rep.representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: "\(outDir)/\(name)"))
    print("wrote \(outDir)/\(name)")
}

func drawBackground(_ ctx: CGContext) {
    ctx.setFillColor(bgColor)
    ctx.fill(CGRect(x: 0, y: 0, width: size, height: size))
}

func drawForeground(_ ctx: CGContext) {
    // 原来的“录制”造型：外圈圆环 + 中心红点，居中、保持原尺寸；圆环用手柄的蓝色
    let c = CGPoint(x: 512, y: 512)
    let ringOuter: CGFloat = 368, ringWidth: CGFloat = 60, dotR: CGFloat = 226
    ctx.setStrokeColor(joyBlue)
    ctx.setLineWidth(ringWidth)
    ctx.strokeEllipse(in: CGRect(x: c.x - ringOuter + ringWidth / 2, y: c.y - ringOuter + ringWidth / 2,
                                 width: 2 * ringOuter - ringWidth, height: 2 * ringOuter - ringWidth))
    ctx.setFillColor(dotColor)
    ctx.fillEllipse(in: CGRect(x: c.x - dotR, y: c.y - dotR, width: 2 * dotR, height: 2 * dotR))
}

// 分层图标的两层
let fg = makeContext(size)
drawForeground(fg)
save(fg, "foreground.png")
let bg = makeContext(size)
drawBackground(bg)
save(bg, "background.png")

// 平面图标（应用市场上传用方形，系统会自己裁圆角）
for (px, name) in [(1024, "icon_1024.png"), (512, "icon_512.png"), (216, "icon_216.png")] {
    let ctx = makeContext(px)
    drawBackground(ctx)
    drawForeground(ctx)
    save(ctx, name)
}
// 启动页图标：自己裁成圆角，放在浅色/深色启动背景上都不会显出方块
let start = makeContext(256)
start.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: size, height: size), cornerWidth: 230, cornerHeight: 230,
                     transform: nil))
start.clip()
drawBackground(start)
drawForeground(start)
save(start, "start_256.png")
