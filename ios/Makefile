# PagerCompanion app + PagerKit package. Everything is driven from here so the
# root Makefile's ios-* targets can just delegate with `$(MAKE) -C ios`.
#
# Xcode and XcodeGen are macOS-only, so `gen` / `test` / `sim` run there.
# `test-linux` runs the PagerKit suite on any host with Docker — the same Linux
# box that builds the firmware.

REPO := $(abspath ..)

.PHONY: gen test test-linux sim clean help

# Regenerate PagerCompanion.xcodeproj from project.yml. The .xcodeproj is not
# committed, so run this after any change to project.yml or to the file layout.
gen:
	xcodegen generate --spec project.yml

# PagerKit's XCTest suite on the local host (macOS, Xcode toolchain).
test:
	cd PagerKit && swift test

# Same suite on Linux via the official Swift image. Mounts the repo root so the
# package can read ../../protocol/vectors/*.json.
test-linux:
	docker run --rm --user $$(id -u):$$(id -g) -e HOME=/tmp -v $(REPO):/repo -w /repo/ios/PagerKit swift:6.0-jammy swift test

# Build the app for the iOS Simulator (no signing required).
sim:
	xcodebuild -project PagerCompanion.xcodeproj -scheme PagerCompanion -destination 'generic/platform=iOS Simulator' build

clean:
	rm -rf PagerKit/.build DerivedData

help:
	@echo "PagerCompanion — iOS companion app"
	@echo ""
	@echo "Targets:"
	@echo "  make gen         - generate PagerCompanion.xcodeproj from project.yml"
	@echo "  make test        - run PagerKit tests on this host (macOS: swift test)"
	@echo "  make test-linux  - run PagerKit tests on Linux in Docker (swift:6.0-jammy)"
	@echo "  make sim         - build for the iOS Simulator (xcodebuild)"
	@echo "  make clean       - remove PagerKit/.build and DerivedData"
	@echo "  make help        - show this help"
