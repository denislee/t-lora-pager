// swift-tools-version:6.0
import PackageDescription

// End-to-end check of the settings registry (IOS_SETTINGS_PLAN.md W12):
// every writable setting is set over PagerLink, read back, the emulator is
// killed (-9) and restarted, and every value is read back again; driven by
// run.sh (make -C ios settings-e2e). Linux or macOS (POSIX socket transport);
// the emulator must be up on TCP 7600. Exit status = failures.
let package = Package(
    name: "SettingsE2E",
    platforms: [.macOS(.v14)],
    dependencies: [.package(path: "../PagerKit")],
    targets: [
        .executableTarget(
            name: "SettingsE2E",
            dependencies: [.product(name: "PagerKit", package: "PagerKit")]
        ),
    ]
)
