// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "PagerKit",
    platforms: [
        .iOS(.v16),
        .macOS(.v14),
    ],
    products: [
        .library(name: "PagerKit", targets: ["PagerKit"]),
    ],
    targets: [
        .target(
            name: "PagerKit",
            path: "Sources/PagerKit"
        ),
        .testTarget(
            name: "PagerKitTests",
            dependencies: ["PagerKit"],
            path: "Tests/PagerKitTests"
        ),
    ]
)
