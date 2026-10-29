import CoreGraphics
import Foundation
import PagerKit
import UIKit

/// A pager screen capture (S13) as the app keeps it: the PNG (for Share and
/// the diagnostics export) and the UIImage it was drawn from. The RGB565
/// pixels are expanded to RGBA8 by PagerKit (`v * 255 / max`, like the
/// emulator's screenshot sweep), so a pager and an emulator capture compare
/// one to one.
struct CapturedScreen: Identifiable {
    let id = UUID()
    let width: Int
    let height: Int
    let capturedAt: Date
    let image: UIImage
    let png: Data

    init?(_ screen: ScreenImage) {
        let rgba = Data(screen.rgba)
        guard let provider = CGDataProvider(data: rgba as CFData),
              let cg = CGImage(width: screen.width, height: screen.height, bitsPerComponent: 8, bitsPerPixel: 32,
                               bytesPerRow: screen.width * 4, space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        let image = UIImage(cgImage: cg)
        guard let png = image.pngData() else { return nil }
        self.width = screen.width
        self.height = screen.height
        self.capturedAt = screen.capturedAt
        self.image = image
        self.png = png
    }

    /// The PNG written to a temporary file named after the capture time, for ShareLink.
    func fileURL() throws -> URL {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("pager-screen-\(formatter.string(from: capturedAt)).png")
        try png.write(to: url, options: .atomic)
        return url
    }
}
