# Are we building debug? y or n.
DEBUG=y
PREFIX ?= /usr
POSTGRESQL=n

# not finished
#bin_install_path := ${PREFIX}/bin
#etc_install_path ?= /etc/ft8goblin
#lib_install_path := ${PREFIX}/lib

# required libraries: -l${x} will be expanded later...
common_libs += yajl ev
callsign_lookup_libs += m curl

# If building DEBUG release
ifeq (${DEBUG},y)
WARN_FLAGS := -Wall -pedantic -Wno-unused-variable -Wno-unused-function #-Wno-missing-braces
ERROR_FLAGS += -Werror 
# Sanitizer options
SAN_FLAGS := -fsanitize=address
SAN_LDFLAGS := -fsanitize=address -static-libasan
OPT_FLAGS += -ggdb3 -fno-omit-frame-pointer
endif
OPT_FLAGS += -O2
CFLAGS += ${SAN_FLAGS} ${WARN_FLAGS} ${ERROR_FLAGS} ${OPT_FLAGS} -DDEBUG=1

C_STD := -std=gnu11
CXX_STD := -std=gnu++17
CFLAGS += ${C_STD} -I./ext/ -I./include/ -I./ext/ft8_lib/ -fPIC
CFLAGS += -DVERSION="\"${VERSION}\""
CXXFLAGS := ${CXX_STD} $(filter-out ${C_STD},${CFLAGS})
LDFLAGS += ${SAN_LDFLAGS} -L./lib/
LDFLAGS += $(foreach x,${common_libs},-l${x})

ifeq (${POSTGRESQL},y)
callsign_lookup_libs += pq
CFLAGS += -DUSE_POSTGRESQL
endif

callsign_lookup_ldflags := ${LDFLAGS} $(foreach x,${callsign_lookup_libs},-l${x})
callsign_lookup_ldflags += $(shell pkg-config --libs spatialite)
