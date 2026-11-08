VERSION = 20230524
#CC := clang
all: world
bins := callsign-lookup
# The optional .pc file can supply flags, but a compiler/linker probe is
# authoritative. A normal installed -lrustyaxe works without pkg-config.
LIBRUSTYAXE_PROVIDER ?= auto
AXE_PKG_CONFIG ?= pkg-config
AXE_PROBE := $(shell AXE_CC='$(CC)' AXE_PKG_CONFIG='$(AXE_PKG_CONFIG)' \
    AXE_CPPFLAGS='$(CPPFLAGS)' AXE_CFLAGS='$(CFLAGS) $(shell pkg-config --cflags glib-2.0 2>/dev/null)' \
    AXE_LDFLAGS='$(LDFLAGS)' sh tools/probe-librustyaxe.sh)
ifeq ($(LIBRUSTYAXE_PROVIDER),auto)
LIBRUSTYAXE_PROVIDER := $(if $(filter none,$(AXE_PROBE)),local,system)
endif
ifeq ($(LIBRUSTYAXE_PROVIDER),system)
ifeq ($(AXE_PROBE),none)
$(error Installed librustyaxe cannot compile/link; use LIBRUSTYAXE_PROVIDER=local)
endif
librustyaxe :=
else ifeq ($(LIBRUSTYAXE_PROVIDER),local)
librustyaxe := ../librustyaxe.so
else
$(error Invalid LIBRUSTYAXE_PROVIDER=$(LIBRUSTYAXE_PROVIDER))
endif

# When this subproject is checked out below rustyrig-fw, put the executable
# alongside the other project binaries.  A standalone checkout retains its
# historical callsign-lookup/bin/ destination.  Set RUSTYRIG_FW_BUILD=true
# explicitly when the parent tree is not adjacent to this directory.
RUSTYRIG_FW_BUILD ?= auto
ifeq (${RUSTYRIG_FW_BUILD},auto)
ifneq ($(wildcard ../config/radio.config.json),)
RUSTYRIG_FW_BUILD := true
else
RUSTYRIG_FW_BUILD := false
endif
endif
ifeq (${RUSTYRIG_FW_BUILD},true)
CALLSIGN_BIN_DIR ?= ../bin
else
CALLSIGN_BIN_DIR ?= bin
endif

include mk/config.mk
extra_distclean += etc/calldata-cache.db etc/fcc-uls.db
callsign_lookup_objs += callsign-lookup.o
callsign_lookup_objs += fcc-db.o
callsign_lookup_objs += gnis-lookup.o	# place names database
callsign_lookup_objs += qrz-xml.o	# QRZ XML API callsign lookups (paid)

callsign_lookup_real_objs := $(foreach x,${callsign_lookup_objs} ${common_objs},obj/${x})

extra_build_targets += etc/calldata-cache.db
real_bins := $(foreach x,${bins},${CALLSIGN_BIN_DIR}/${x})
extra_clean += ${callsign_lookup_real_objs} 
extra_clean += ${real_bins}

#################
# Build Targets #
#################
${CALLSIGN_BIN_DIR}/callsign-lookup: ${librustyaxe} ${callsign_lookup_real_objs}
	@echo "[Linking] $@"
	@mkdir -p $(dir $@)
	@${CC} -o $@ ${SAN_LDFLAGS} ${callsign_lookup_real_objs} ${callsign_lookup_ldflags} ${LDFLAGS}

etc/calldata-cache.db:
	sqlite3 etc/calldata-cache.db < sql/cache.sql 

include mk/compile.mk
include mk/help.mk
include mk/clean.mk
include mk/install.mk

# Build all subdirectories first, then our binary
prebuild:
	@mkdir -p obj

world: prebuild ${extra_build_targets} ${real_bins}

todo:
	# We would use find here, but there's probably XXX: in subdirs we don't care about...
	grep -Hn "XXX:" include/* src/* * etc/* sql/* scripts/* mk/* 2>/dev/null | less
