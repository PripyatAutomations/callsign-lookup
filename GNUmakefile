VERSION = 20230519
#CC := clang
all: world
bins := callsign-lookup

include mk/config.mk

common_objs += config.o
common_objs += daemon.o
common_objs += debuglog.o
common_objs += dict.o
common_objs += util.o
common_objs += maidenhead.o	# maidenhead coordinate tools

extra_distclean += etc/calldata-cache.db etc/fcc-uls.db
callsign_lookup_objs += callsign-lookup.o
callsign_lookup_objs += fcc-db.o
callsign_lookup_objs += gnis-lookup.o	# place names database
callsign_lookup_objs += qrz-xml.o	# QRZ XML API callsign lookups (paid)
callsign_lookup_objs += sql.o

callsign_lookup_real_objs := $(foreach x,${callsign_lookup_objs} ${common_objs},obj/${x})

extra_build_targets += etc/calldata-cache.db
real_bins := $(foreach x,${bins},bin/${x})
extra_clean += ${callsign_lookup_real_objs} 
extra_clean += ${real_bins} ${ft8lib} ${ft8lib_objs}

#################
# Build Targets #
#################
bin/callsign-lookup: ${callsign_lookup_real_objs}
	@echo "[Linking] $@"
	${CC} -o $@ ${SAN_LDFLAGS} ${callsign_lookup_real_objs} ${callsign_lookup_ldflags} ${LDFLAGS}

etc/calldata-cache.db:
	sqlite3 etc/calldata-cache.db < sql/cache.sql 

include mk/compile.mk
include mk/help.mk
include mk/clean.mk
include mk/install.mk

# Build all subdirectories first, then our binary
prebuild:
	mkdir -p obj

world: prebuild ${extra_build_targets} ${real_bins}

todo:
	# We would use find here, but there's probably XXX: in subdirs we don't care about...
	grep -Hn "XXX:" include/* src/* * etc/* sql/* scripts/* mk/* 2>/dev/null | less
