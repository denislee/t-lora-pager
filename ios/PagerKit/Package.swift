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
    // swift-crypto (decision D1, IOS_NOTES_VIEWER_PLAN.md) so notes decryption
    // and the git blob SHA-1 that gates deletion are tested on Linux too; 3.x
    // because 4.x needs Swift tools 6.1.
    dependencies: [
        .package(url: "https://github.com/apple/swift-crypto.git", from: "3.15.1"),
    ],
    targets: [
        .target(
            name: "PagerKit",
            dependencies: [
                .product(name: "Crypto", package: "swift-crypto"),
                .product(name: "_CryptoExtras", package: "swift-crypto"),
            ],
            path: "Sources/PagerKit"
        ),
        .testTarget(
            name: "PagerKitTests",
            dependencies: ["PagerKit"],
            path: "Tests/PagerKitTests"
        ),
    ]
)
