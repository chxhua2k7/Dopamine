// 設定頁圖示:Sileo 內建的「Tweaks」分類圖示(黃底白扳手,取自 Sileo.app/Assets.car 的
// Category_tweak),加上 iOS 圓角後輸出 bundle/ 的 29/58/87 和 logo/ 的 1024。
// 用法:swift logo/make_icon.swift .
import AppKit

let root = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
let source = NSImage(contentsOf: root.appendingPathComponent("logo/sileo-tweak-source.png"))!

func render(_ size: Int, to url: URL) {
    let s = CGFloat(size)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    NSGraphicsContext.current?.imageInterpolation = .high
    let rect = NSRect(x: 0, y: 0, width: s, height: s)
    NSBezierPath(roundedRect: rect, xRadius: s * 0.2237, yRadius: s * 0.2237).addClip()
    source.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1)
    NSGraphicsContext.restoreGraphicsState()
    try! rep.representation(using: .png, properties: [:])!.write(to: url)
}

render(29, to: root.appendingPathComponent("bundle/BootLogStop.png"))
render(58, to: root.appendingPathComponent("bundle/BootLogStop@2x.png"))
render(87, to: root.appendingPathComponent("bundle/BootLogStop@3x.png"))
render(1024, to: root.appendingPathComponent("logo/BootLogStop-1024.png"))
print("done")
