# PagerCompanion app + PagerKit package. Everything is driven from here so the
# root Makefile's ios-* targets can just delegate with `$(MAKE) -C ios`.
#
# Xcode and XcodeGen are macOS-only, so `gen` / `test` / `sim` run there.
# `test-linux` runs the PagerKit suite on any host with Docker — the same Linux
# box that builds the firmware.

REPO := $(abspath ..)

PROJECT := PagerCompanion.xcodeproj
SCHEME  := PagerCompanion
DD      := DerivedData/device
APP     := $(DD)/Build/Products/Debug-iphoneos/PagerCompanion.app

# The minimum iOS the app is built for (project.yml deploymentTarget), so the
# device targets can refuse a phone that is too old with a readable message
# instead of a signing or install error.
MIN_IOS = $(shell awk -F'"' '/iOS:/ {print $$2; exit}' project.yml)

# The connected device, discovered rather than hard-coded (same scheme as
# ios-tts). Override with `make device DEVICE=<identifier|name|udid>`.
#
# Recursively expanded (`=`) so targets that never touch a device don't pay for
# the devicectl round trip. Simulators are dropped by `reality: simulated` and
# `transportType: sameMachine` (a booted one otherwise reports a connected
# tunnel and wins). A cabled, paired phone counts even with its tunnel down:
# CoreDevice opens it on demand. `DEVICE_TYPE` (iPhone or iPad) is preferred
# when several are usable, falling back to whatever is there; the app is
# iPhone-only (TARGETED_DEVICE_FAMILY 1) but runs on an iPad in compatibility mode.
DEVICE_TYPE ?= iPhone
DEVICE = $(shell f=$$(mktemp) && \
	xcrun devicectl list devices --json-output $$f >/dev/null 2>&1 && \
	python3 -c "import json; d = json.load(open('$$f')); \
hp = lambda x: x.get('hardwareProperties', {}); \
cp = lambda x: x.get('connectionProperties', {}); \
ios = [x for x in d['result']['devices'] \
if hp(x).get('platform') == 'iOS' and hp(x).get('reality') != 'simulated' \
and cp(x).get('transportType') != 'sameMachine']; \
ios = [x for x in ios if cp(x).get('tunnelState') == 'connected' \
or (cp(x).get('pairingState') == 'paired' and cp(x).get('transportType') == 'wired')]; \
ios = [x for x in ios if hp(x).get('deviceType') == '$(DEVICE_TYPE)'] or ios; \
ios.sort(key=lambda x: cp(x).get('tunnelState') != 'connected'); \
print(ios[0]['identifier'] if ios else '')"; rm -f $$f)

# A phone on iOS 16 or older, looked for only when devicectl finds nothing:
# CoreDevice is iOS 17+ and lists such a phone as `pairingState: unsupported`.
# ios-deploy still reaches it over USB. Prints "<udid> <major>.<minor>".
LEGACY_DEVICE = $(shell command -v ios-deploy >/dev/null 2>&1 && \
	ios-deploy --detect --timeout 2 2>/dev/null | python3 -c "import re, sys; \
m = [re.search(r'Found (\S+) .*?, iphoneos, [^,]*, (\d+)\.(\d+)', l) for l in sys.stdin]; \
print(next((f'{x.group(1)} {x.group(2)}.{x.group(3)}' for x in m if x and int(x.group(2)) < 17), ''))")

.PHONY: gen test test-linux sim device device-run device-console clean clean-device help

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

# Build Debug, sign it with your team, and install on the connected phone.
#
# Signing comes from Config/Local.xcconfig (DEVELOPMENT_TEAM; untracked, see
# README "Signing"). `-allowProvisioningUpdates` lets Xcode create the App ID
# and profile; `-allowProvisioningDeviceRegistration` adds a phone the account
# has never seen. An iOS 16 phone is built for as a generic iOS device and
# installed with ios-deploy, but only if MIN_IOS allows it.
device: gen
	@grep -qE '^[[:space:]]*DEVELOPMENT_TEAM[[:space:]]*=[[:space:]]*[A-Z0-9]+' Config/Local.xcconfig 2>/dev/null || { \
		echo "No signing team. Run: cp Config/Local.xcconfig.example Config/Local.xcconfig"; \
		echo "and set DEVELOPMENT_TEAM (Xcode -> Settings -> Accounts)."; exit 1; }
	@dev='$(DEVICE)'; legacy=''; \
	if [ -z "$$dev" ]; then \
		set -- $(LEGACY_DEVICE); legacy="$$1"; dev="$$1"; \
		if [ -n "$$legacy" ] && python3 -c "import sys; sys.exit(tuple(map(int, '$$2'.split('.'))) >= tuple(map(int, '$(MIN_IOS)'.split('.'))))"; then \
			echo "The connected phone ($$legacy) runs iOS $$2; PagerCompanion needs iOS $(MIN_IOS)"; \
			echo "(project.yml deploymentTarget). Plug in a newer phone, or override with DEVICE=<udid>."; \
			exit 1; \
		fi; \
	fi; \
	test -n "$$dev" || { \
		echo "No connected iOS device. Plug one in, unlock it, and trust this Mac."; \
		echo "Devices devicectl can see:"; xcrun devicectl list devices; \
		command -v ios-deploy >/dev/null || \
			echo "(An iOS 16 phone needs ios-deploy: brew install ios-deploy)"; \
		exit 1; }; \
	dest="id=$$dev"; [ -z "$$legacy" ] || dest='generic/platform=iOS'; \
	echo "==> Building Debug for $$dev"; \
	xcodebuild -project $(PROJECT) -scheme $(SCHEME) -configuration Debug \
		-destination "$$dest" -derivedDataPath $(DD) \
		-allowProvisioningUpdates -allowProvisioningDeviceRegistration build || exit 1; \
	echo "==> Installing"; \
	if [ -n "$$legacy" ]; then \
		ios-deploy --id "$$dev" --bundle $(APP) --no-wifi || exit 1; \
		echo "==> Installed. Launch it from the Home screen."; \
	else \
		xcrun devicectl device install app --device "$$dev" $(APP) || exit 1; \
		echo "==> Installed. Launch it from the Home screen, or: make device-console"; \
	fi

device-run: device device-console

# Launch the installed app with its stdout/stderr streamed here (not os_log).
# The bundle id is read from the built app, since BUNDLE_ID_PREFIX is per
# developer.
device-console:
	@test -n "$(DEVICE)" || { echo "No connected iOS 17+ device."; exit 1; }
	@test -d $(APP) || { echo "No device build yet; run make device first."; exit 1; }
	@echo "==> Launching with console attached (ctrl-C to detach; the app keeps running)"
	@xcrun devicectl device process launch --console --terminate-existing \
		--device $(DEVICE) $$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' $(APP)/Info.plist)

clean-device:
	rm -rf $(DD)

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
	@echo "  make device      - build, sign and install on the connected iPhone"
	@echo "                     (DEVICE_TYPE=iPad, or DEVICE=<udid> to pick one)"
	@echo "  make device-run  - the same, then launch it with the console attached"
	@echo "  make device-console - launch the installed app with its console"
	@echo "  make clean       - remove PagerKit/.build and DerivedData"
	@echo "  make help        - show this help"
