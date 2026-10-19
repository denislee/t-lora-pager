PIO ?= $(HOME)/.platformio/penv/bin/pio
ENV ?= tlora_pager
PORT ?=

UPLOAD_FLAGS :=
MONITOR_FLAGS :=
ifneq ($(PORT),)
UPLOAD_FLAGS += --upload-port $(PORT)
MONITOR_FLAGS += --port $(PORT)
endif

.PHONY: all build upload monitor flash clean emulator help envs ios-gen ios-test ios-sim ios-device protocol-test

all: build

build:
	$(PIO) run -e $(ENV)

upload:
	$(PIO) run -e $(ENV) --target upload $(UPLOAD_FLAGS)

monitor:
	$(PIO) device monitor -b 115200 $(MONITOR_FLAGS)

flash: upload monitor

clean:
	$(PIO) run -e $(ENV) --target clean

emulator:
	$(PIO) run -e emulator_lora_pager

envs:
	$(PIO) project config | grep '^\[env:'

ios-gen:
	$(MAKE) -C ios gen

ios-test:
	$(MAKE) -C ios test

ios-sim:
	$(MAKE) -C ios sim

ios-device:
	$(MAKE) -C ios device

protocol-test:
	python3 protocol/tools/mkvectors.py --check
	$(PIO) test -e native_test -f test_link_frame -f test_link_rpc -f test_link_json -f test_link_xfer
	cd tools/pagerlink && go test ./internal/pagerlink/
	$(MAKE) -C ios test-linux

help:
	@echo "Targets:"
	@echo "  make build          - compile (ENV=$(ENV))"
	@echo "  make upload         - compile and upload"
	@echo "  make monitor        - open serial monitor @ 115200"
	@echo "  make flash          - upload then monitor"
	@echo "  make clean          - clean build artifacts"
	@echo "  make emulator       - build SDL2 emulator"
	@echo "  make envs           - list available PlatformIO envs"
	@echo "  make ios-gen        - generate ios/PagerCompanion.xcodeproj (XcodeGen)"
	@echo "  make ios-test       - run ios/PagerKit tests on this host (macOS)"
	@echo "  make ios-sim        - build the iOS app for the Simulator"
	@echo "  make ios-device     - build and install the iOS app on the connected iPhone"
	@echo "  make protocol-test  - check vectors, Unity + Go + PagerKit tests"
	@echo ""
	@echo "Variables:"
	@echo "  ENV=<env>           - PlatformIO env (default: tlora_pager)"
	@echo "  PORT=/dev/ttyACM0   - override serial port"
	@echo "  PIO=<path>          - override pio binary (default: ~/.platformio/penv/bin/pio)"
