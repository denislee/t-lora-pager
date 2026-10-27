// swift-tools-version:6.0
import PackageDescription

// End-to-end checks of the notes viewer and the move-to-GitHub engine
// against the emulator over TCP 7600 (IOS_NOTES_VIEWER_PLAN.md W7, W12,
// W14), with GitHub replaced by the in-process FakeGitHubRepo. Linux or macOS
// (POSIX socket transport); run with `make -C ios notes-e2e` while
// `.pio/build/emulator_lora_pager/program` is up. Exit status = failures.
let package = Package(
    name: "NotesE2E",
    platforms: [.macOS(.v14)],
    dependencies: [.package(path: "../PagerKit")],
    targets: [
        .executableTarget(
            name: "NotesE2E",
            dependencies: [.product(name: "PagerKit", package: "PagerKit")]
        ),
    ]
)
