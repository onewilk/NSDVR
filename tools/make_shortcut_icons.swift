// 生成桌面快捷方式图标（长按应用图标弹出的菜单里显示）：
//   swift tools/make_shortcut_icons.swift harmony/entry/src/main/resources/base/media
// 输出 192×192 PNG：蓝色圆底 + 白色线条图形，全部自己画，不用任何图标库。
//   shortcut_connect_last.png     “连接上次”：循环箭头 + 播放三角
//   shortcut_hotspot_connect.png  “热点+连接”：热点信号（中心点 + 两侧弧线）
import AppKit
import CoreGraphics
import Foundation

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
let px = 192

func color(_ hex: UInt32) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
}

let accent = color(0x0A59F7)  // 与 App 的强调色一致
let white = color(0xFFFFFF)

func draw(_ name: String, _ glyph: (CGContext) -> Void) {
    let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    // 以左上角为原点、100 为单位坐标绘制
    ctx.translateBy(x: 0, y: CGFloat(px))
    ctx.scaleBy(x: CGFloat(px) / 100, y: -CGFloat(px) / 100)
    ctx.setShouldAntialias(true)
    ctx.setFillColor(accent)
    ctx.fillEllipse(in: CGRect(x: 0, y: 0, width: 100, height: 100))
    ctx.setStrokeColor(white)
    ctx.setFillColor(white)
    ctx.setLineWidth(6)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    glyph(ctx)
    let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
    let data = rep.representation(using: .png, properties: [:])!
    try! data.write(to: URL(fileURLWithPath: outDir).appendingPathComponent(name))
    print("已生成 \(name)")
}

// 角度：0 = 正右，顺时针为正（坐标系 y 向下）
func point(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat, _ deg: CGFloat) -> CGPoint {
    let a = deg * .pi / 180
    return CGPoint(x: cx + r * cos(a), y: cy + r * sin(a))
}

draw("shortcut_connect_last.png") { ctx in
    // 循环箭头：从右上方顺时针绕一圈到左上方，顶部留缺口，箭头在终点沿切线方向
    let c = CGPoint(x: 50, y: 50)
    let r: CGFloat = 26
    let start: CGFloat = -50
    let end: CGFloat = 225
    ctx.addArc(center: c, radius: r, startAngle: start * .pi / 180, endAngle: end * .pi / 180, clockwise: false)
    ctx.strokePath()
    let tip = point(50, 50, r, end)
    let a = end * .pi / 180
    // 顺时针前进方向的反方向（箭头两翼从箭尖往回伸）
    let back = CGPoint(x: sin(a), y: -cos(a))
    for turn: CGFloat in [-40, 40] {
        let t = turn * .pi / 180
        let wing = CGPoint(x: back.x * cos(t) - back.y * sin(t), y: back.x * sin(t) + back.y * cos(t))
        ctx.move(to: tip)
        ctx.addLine(to: CGPoint(x: tip.x + wing.x * 12, y: tip.y + wing.y * 12))
        ctx.strokePath()
    }
    // 播放三角
    ctx.move(to: CGPoint(x: 44, y: 38))
    ctx.addLine(to: CGPoint(x: 62, y: 50))
    ctx.addLine(to: CGPoint(x: 44, y: 62))
    ctx.closePath()
    ctx.fillPath()
}

draw("shortcut_hotspot_connect.png") { ctx in
    // 中心点
    ctx.fillEllipse(in: CGRect(x: 43, y: 45, width: 14, height: 14))
    // 两侧各两道弧线
    for r: CGFloat in [16, 29] {
        ctx.addArc(center: CGPoint(x: 50, y: 52), radius: r, startAngle: 140 * .pi / 180, endAngle: 220 * .pi / 180,
                   clockwise: false)
        ctx.strokePath()
        ctx.addArc(center: CGPoint(x: 50, y: 52), radius: r, startAngle: -40 * .pi / 180, endAngle: 40 * .pi / 180,
                   clockwise: false)
        ctx.strokePath()
    }
}
