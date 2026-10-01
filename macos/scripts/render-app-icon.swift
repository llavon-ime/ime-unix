import AppKit
import Foundation

// Prepare the existing artwork for macOS's icon silhouette. An opaque square
// is inset onto a second system plate; a masked RGBA canvas avoids that seam.
guard CommandLine.arguments.count == 3,
      let artwork = NSImage(contentsOfFile: CommandLine.arguments[1]),
      let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 1024, pixelsHigh: 1024,
          bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
          colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
      let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
    fatalError("Usage: swift render-app-icon.swift SOURCE.png OUTPUT.png")
}

bitmap.size = NSSize(width: 1024, height: 1024)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
context.imageInterpolation = .high
let canvas = NSRect(x: 0, y: 0, width: 1024, height: 1024)
NSColor.clear.setFill()
canvas.fill(using: .copy)
let bounds = NSRect(x: 100, y: 100, width: 824, height: 824)
NSBezierPath(roundedRect: bounds, xRadius: 185, yRadius: 185).addClip()
NSColor.white.setFill()
bounds.fill()
artwork.draw(in: bounds, from: .zero, operation: .sourceOver, fraction: 1)
NSGraphicsContext.restoreGraphicsState()

guard let png = bitmap.representation(using: .png, properties: [:]) else {
    fatalError("Cannot encode the app icon")
}
try png.write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
