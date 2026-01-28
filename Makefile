.PHONY: all build flash monitor flash-monitor reset clean fullclean menuconfig size push main test

# Default target: build, flash, and monitor
all: build flash-monitor

# Build the project
build:
	idf.py build

# Flash the firmware
flash:
	idf.py flash

# Monitor serial output (with auto-reset if needed)
monitor:
	idf.py monitor

# Flash and monitor in one command (ensures proper reset)
flash-monitor:
	idf.py flash monitor

# Reset the device (useful when stuck in download mode)
# This will attempt to reset via esptool, which works even when device is in download mode
reset:
	@echo "Resetting device..."
	@esptool.py --port /dev/ttyACM0 --baud 115200 run || \
	 esptool.py --port /dev/ttyUSB0 --baud 115200 run || \
	 (echo "Could not reset device. Try: make flash" && exit 1)

# Build, flash, and monitor in one command
bfm: build flash-monitor

# Just build and flash (no monitor)
bf: build flash

# Clean build artifacts
clean:
	idf.py fullclean

# Full clean (alias for clean)
fullclean: clean

# Open menuconfig
menuconfig:
	idf.py menuconfig

# Show binary size
size:
	idf.py size

# Show detailed size information
size-components:
	idf.py size-components

# Show size of files
size-files:
	idf.py size-files

# Erase flash
erase:
	idf.py erase-flash

# Show partition table
partition-table:
	idf.py partition-table

# Git commit + push (default branch: main)
# Usage:
#   make push
#   make push main
#   make push test
#   make push MSG="your custom commit message"
#
# Notes:
# - `make push test` works because Make treats `test` as an extra goal; the `test` target
#   is a no-op, and `push` reads the second goal as the branch name.
# - For safety, this target refuses to push if you are not currently on the requested branch.
MSG ?= Update code
BRANCH ?= main

# If the first goal is `push`, treat the second goal as the branch name.
ifeq ($(firstword $(MAKECMDGOALS)),push)
  override BRANCH := $(strip $(word 2,$(MAKECMDGOALS)))
  ifeq ($(BRANCH),)
    override BRANCH := main
  endif
endif

push:
	@current="$$(git branch --show-current)"; \
	if [ "$$current" != "$(BRANCH)" ]; then \
	  echo "ERROR: you are on '$$current' but asked to push '$(BRANCH)'."; \
	  echo "       Run: git switch $(BRANCH)   (or use: make push $$current)"; \
	  exit 1; \
	fi
	@echo "Staging all changes..."
	@git add -A
	@echo "Committing with message: $(MSG)"
	@git commit -m "$(MSG)" || true
	@echo "Pushing branch '$(BRANCH)' to origin..."
	@git push -u origin "$(BRANCH)"
	@echo "Done!"

# No-op targets so `make push test` / `make push main` doesn't error.
main test:
	@true

# Help target
help:
	@echo "ESP-IDF Project Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  all, bfm          - Build, flash, and monitor (default)"
	@echo "  build             - Build the project"
	@echo "  flash             - Flash the firmware"
	@echo "  monitor           - Monitor serial output"
	@echo "  flash-monitor     - Flash and monitor (auto-reset, recommended)"
	@echo "  reset             - Reset device if stuck in download mode"
	@echo "  bf                - Build and flash (no monitor)"
	@echo "  clean, fullclean  - Clean build artifacts"
	@echo "  menuconfig        - Open configuration menu"
	@echo "  size              - Show binary size"
	@echo "  size-components   - Show size by component"
	@echo "  size-files        - Show size by file"
	@echo "  erase             - Erase flash"
	@echo "  partition-table   - Show partition table"
	@echo "  push              - Commit and push to main (default) or to a branch"
	@echo "  help              - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make              - Build, flash, and monitor"
	@echo "  make bfm          - Same as above"
	@echo "  make build        - Just build"
	@echo "  make flash        - Just flash"
	@echo "  make monitor      - Just monitor"
	@echo "  make push         - Commit and push with default message"
	@echo "  make push test    - Commit and push the 'test' branch"
	@echo "  make push main    - Commit and push the 'main' branch"
	@echo "  make push MSG=\"fix: update ADC config\" - Commit and push with custom message"
