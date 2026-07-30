
# heartbeat-demo
# A demo for the Ultimate 64, built around a Heartbeat Soundtracker song
# Written in 2026 by Xander Mol

# Target platform
SYS = c64

# Cross-platform shell detection
ifneq ($(shell echo),)
  CMD_EXE = 1
endif

ifdef CMD_EXE
  NULLDEV = nul:
  DEL     = -del /f
  RMDIR   = rmdir /s /q
  MKDIR   = mkdir
else
  NULLDEV = /dev/null
  DEL     = $(RM)
  RMDIR   = $(RM) -r
  MKDIR   = mkdir -p
endif

# Toolchain -- override if oscar64 isn't on PATH:
#   make CC=/path/to/oscar64/bin/oscar64
# (plain '=', not '?=' -- CC is a Make built-in with a default of 'cc',
# which is never "unset", so '?=' would silently never take effect)
CC = oscar64

# Application name
MAIN = heartbeat-demo

# Build versioning
VERSION_MAJOR     = 1
VERSION_MINOR     = 0
VERSION_PATCH     = 0
VERSION_TIMESTAMP = $(shell date "+%Y%m%d-%H%M")
VERSION           = v$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)-$(VERSION_TIMESTAMP)

# Compile flags
#   -i=include   : add include/ to header search path
#   -tm=c64      : target Commodore 64
#   -tf=prg      : output standard .prg file
#   -O2          : optimise
#   -dNOFLOAT    : disable float support (saves space)
#   -n           : suppress default BASIC stub (Oscar64 adds one for prg)
#   -dVERSION    : pass version string to source
CFLAGS = -i=include \
         -tm=$(SYS) \
         -tf=prg \
         -O2 \
         -dNOFLOAT \
         -dHEAPCHECK \
         -dVERSION="\"$(VERSION)\""

# Main source (Oscar64 follows #pragma compile chains from here)
MAINSRC = src/main.c

# All sources that Oscar64 compiles via #pragma compile chains.
# Listed here so make rebuilds when any of them change.
ALLSRCS = $(MAINSRC) \
          src/screen.c \
          src/detect.c \
          src/visualizer.c \
          include/turbo.c \
          include/audio.c \
          include/ultimate_common_lib.c \
          include/ultimate_dos_lib.c \
          include/hbplayer.c

# Output
TARGET = build/$(MAIN).prg

########################################

# The two songs the demo can switch between at runtime (visualizer.c's 'S' key)
# -- both deployed alongside the .prg. Must match vis_song_files[] in
# src/visualizer.c. Quoted as one shell word each so the embedded space in
# "Knight Rider Theme.reu" survives the for-loops in zip/deploy below --
# this is a plain string, not a Make list, precisely so that quoting works.
SONGFILES = "assets/maniac.reu" "assets/Knight Rider Theme.reu"

# Ultimate 64 firmware config presets for this demo's required settings
# (SID addressing, filter curves, mixer levels, turbo) -- see README's
# Requirements section. Two variants because "Turbo Control"'s valid value
# differs by hardware/firmware: Heartbeat Soundtracker's own unmodified
# Heartbeat.cfg uses "C64U Turbo Registers", which a U64 Elite II (and any
# other firmware reporting that string as invalid) rejects -- confirmed
# against a live U64E2's own reported config choices, not just
# documentation. Heartbeat-U64E2.cfg uses "U64 Turbo Registers" instead.
CONFIGFILES = "config/Heartbeat-C64U.cfg" "config/Heartbeat-U64E2.cfg"

# Demo install path on SD/USB (must match demo_path[] in src/main.c once used)
INSTALL_PATH = idi8b/heartbeat-demo
# NOTE: The zip target hardcodes the first path component "idi8b" in the cleanup
#       RMDIR step. If you change INSTALL_PATH to a different top-level folder,
#       update the RMDIR line in the zip target accordingly.

# Deployment target (FTP to Ultimate device)
# Set your U64 IP in .env (see .env.example) — .env is gitignored
# NOTE: /usb0/ stopped working on this device (mount failure, confirmed via
#       ultimate_get_file_info 404); /sd/ is the working path instead.
-include .env
ULTHOST  ?= <YOUR_U64_IP>
ULTPATH  = /sd/$(INSTALL_PATH)/
ULTFTP   = ftp://$(ULTHOST)

# Versioned release ZIP
ZIPFILE  = build/$(MAIN)-$(VERSION).zip

.SUFFIXES:
.PHONY: all clean deploy zip check-deploy docs

all: $(TARGET) zip README.pdf

$(TARGET): $(ALLSRCS)
	@$(MKDIR) build 2>$(NULLDEV) ; true
	$(CC) $(CFLAGS) -n -o=$(TARGET) $<

clean:
	$(DEL) build/*.prg 2>$(NULLDEV) ; true
	$(DEL) build/*.map 2>$(NULLDEV) ; true
	$(DEL) build/*.asm 2>$(NULLDEV) ; true
	$(DEL) build/*.lbl 2>$(NULLDEV) ; true
	$(DEL) build/*.zip 2>$(NULLDEV) ; true

zip: $(TARGET) README.pdf
	$(MKDIR) build/$(INSTALL_PATH)/config 2>$(NULLDEV) ; true
	cp $(TARGET)   build/$(INSTALL_PATH)/$(MAIN).prg
	cp README.md   build/$(INSTALL_PATH)/README.md
	@if [ -f README.pdf ]; then cp README.pdf build/$(INSTALL_PATH)/README.pdf; fi
	@for f in $(SONGFILES); do \
		if [ -f "$$f" ]; then cp "$$f" build/$(INSTALL_PATH)/; else \
			echo "WARNING: $$f not found -- zip built without it"; fi; \
	done
	@for f in $(CONFIGFILES); do \
		if [ -f "$$f" ]; then cp "$$f" build/$(INSTALL_PATH)/config/; else \
			echo "WARNING: $$f not found -- zip built without it"; fi; \
	done
	cd build && zip -r $(MAIN)-$(VERSION).zip idi8b/
	$(RMDIR) build/idi8b 2>$(NULLDEV) ; true

# Regenerate README.pdf from README.md (requires pandoc + texlive-xetex).
# Install: sudo apt install pandoc texlive-xetex
# Warns and skips (does not fail the build) if pandoc is unavailable, since
# README.pdf is committed to git and only needs regenerating when docs change.
docs: README.pdf

README.pdf: README.md pandoc-defaults.yaml pandoc-header.tex
	@if which pandoc >/dev/null 2>&1; then \
		pandoc --defaults=pandoc-defaults.yaml README.md -o README.pdf; \
	else \
		echo "WARNING: pandoc not found -- README.pdf not updated (install: sudo apt install pandoc texlive-xetex)"; \
	fi

check-deploy:
	@curl -s --connect-timeout 3 $(ULTFTP)/ >/dev/null 2>&1 || \
		(echo "ERROR: Cannot reach U64 at $(ULTHOST) -- check ULTHOST in .env" && false)

deploy: check-deploy $(TARGET)
	wput -u $(TARGET) $(ULTFTP)$(ULTPATH)$(MAIN).prg
	@for f in $(SONGFILES); do \
		if [ -f "$$f" ]; then wput -u "$$f" $(ULTFTP)$(ULTPATH)"$$(basename "$$f")"; fi; \
	done
	@for f in $(CONFIGFILES); do \
		if [ -f "$$f" ]; then wput -u "$$f" $(ULTFTP)$(ULTPATH)"config/$$(basename "$$f")"; fi; \
	done
