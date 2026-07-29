
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

# Toolchain
CC = /home/xahmol/oscar64/bin/oscar64

# Application name
MAIN = heartbeat-demo

# Build versioning
VERSION_MAJOR     = 0
VERSION_MINOR     = 1
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
          include/turbo.c \
          include/audio.c \
          include/ultimate_common_lib.c \
          include/ultimate_dos_lib.c

# Output
TARGET = build/$(MAIN).prg

########################################

# Demo install path on SD/USB (must match demo_path[] in src/main.c once used)
INSTALL_PATH = idi8b/heartbeat-demo
# NOTE: The zip target hardcodes the first path component "idi8b" in the cleanup
#       RMDIR step. If you change INSTALL_PATH to a different top-level folder,
#       update the RMDIR line in the zip target accordingly.

# Deployment target (FTP to Ultimate device)
# Set your U64 IP in .env (see .env.example) — .env is gitignored
-include .env
ULTHOST  ?= <YOUR_U64_IP>
ULTPATH  = /usb0/$(INSTALL_PATH)/
ULTFTP   = ftp://$(ULTHOST)

# Versioned release ZIP
ZIPFILE  = build/$(MAIN)-$(VERSION).zip

.SUFFIXES:
.PHONY: all clean deploy zip check-deploy

all: $(TARGET) zip

$(TARGET): $(ALLSRCS)
	@$(MKDIR) build 2>$(NULLDEV) ; true
	$(CC) $(CFLAGS) -n -o=$(TARGET) $<

clean:
	$(DEL) build/*.prg 2>$(NULLDEV) ; true
	$(DEL) build/*.map 2>$(NULLDEV) ; true
	$(DEL) build/*.asm 2>$(NULLDEV) ; true
	$(DEL) build/*.lbl 2>$(NULLDEV) ; true
	$(DEL) build/*.zip 2>$(NULLDEV) ; true

zip: $(TARGET)
	$(MKDIR) build/$(INSTALL_PATH) 2>$(NULLDEV) ; true
	cp $(TARGET)   build/$(INSTALL_PATH)/$(MAIN).prg
	cp README.md   build/$(INSTALL_PATH)/README.md
	cd build && zip -r $(MAIN)-$(VERSION).zip idi8b/
	$(RMDIR) build/idi8b 2>$(NULLDEV) ; true

check-deploy:
	@curl -s --connect-timeout 3 $(ULTFTP)/ >/dev/null 2>&1 || \
		(echo "ERROR: Cannot reach U64 at $(ULTHOST) -- check ULTHOST in .env" && false)

deploy: check-deploy $(TARGET)
	wput -u $(TARGET) $(ULTFTP)$(ULTPATH)$(MAIN).prg
