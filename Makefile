.PHONY: all build flash monitor clean fullclean menuconfig size

# Default target: build, flash, and monitor
all: build flash monitor

# Build the project
build:
	idf.py build

# Flash the firmware
flash:
	idf.py flash

# Monitor serial output
monitor:
	idf.py monitor

# Build, flash, and monitor in one command
bfm: build flash monitor

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

# Git push with commit (default message or custom)
# Usage: make push
#        make push MSG="your custom commit message"
MSG ?= "Update code"
push:
	@echo "Staging all changes..."
	@git add -A
	@echo "Committing with message: $(MSG)"
	@git commit -m $(MSG) || true
	@echo "Pushing to remote..."
	@git push
	@echo "Done!"

# Help target
help:
	@echo "ESP-IDF Project Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  all, bfm          - Build, flash, and monitor (default)"
	@echo "  build             - Build the project"
	@echo "  flash             - Flash the firmware"
	@echo "  monitor           - Monitor serial output"
	@echo "  bf                - Build and flash (no monitor)"
	@echo "  clean, fullclean  - Clean build artifacts"
	@echo "  menuconfig        - Open configuration menu"
	@echo "  size              - Show binary size"
	@echo "  size-components   - Show size by component"
	@echo "  size-files        - Show size by file"
	@echo "  erase             - Erase flash"
	@echo "  partition-table   - Show partition table"
	@echo "  push              - Commit and push (use MSG='message' for custom commit)"
	@echo "  help              - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make              - Build, flash, and monitor"
	@echo "  make bfm          - Same as above"
	@echo "  make build        - Just build"
	@echo "  make flash        - Just flash"
	@echo "  make monitor      - Just monitor"
	@echo "  make push         - Commit and push with default message"
	@echo "  make push MSG=\"fix: update ADC config\" - Commit and push with custom message"
