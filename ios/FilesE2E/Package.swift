// swift-tools-version:6.0
import PackageDescription

// End-to-end checks of the Files screen's engine (PagerKit FileBrowser)
// against the emulator over TCP 7600 (suggestion S1, IOS_COMPANION_SUGGESTIONS
// §8 `files-e2e`): byte-for-byte round trips with CRC-32 in both directions on
// the SD volume, folder changes, and `evt.fs.changed` refreshing an open
// listing. Linux or macOS (POSIX socket transport); run with
// `make -C ios files-e2e` while `.pio/build/emulator_lora_pager/program` is
// up. Exit status = failures.
let package = Package(
    name: "FilesE2E",
    platforms: [.macOS(.v14)],
    dependencies: [.package(path: "../PagerKit")],
    targets: [
        .executableTarget(
            name: "FilesE2E",
            dependencies: [.product(name: "PagerKit", package: "PagerKit")]
        ),
    ]
)
