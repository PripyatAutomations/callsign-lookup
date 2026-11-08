# Are we building debug? y or n.
DEBUG ?= n
PREFIX ?= /usr
POSTGRESQL=n

# not finished
#bin_install_path := ${PREFIX}/bin
#etc_install_path ?= /etc/ft8goblin
#lib_install_path := ${PREFIX}/lib

# required libraries: -l${x} will be expanded later...
common_libs += ev sqlite3
callsign_lookup_libs += m curl

# If building DEBUG release
ifeq (${DEBUG},y)
WARN_FLAGS := -Wall -pedantic -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable #-Wno-missing-braces
#ERROR_FLAGS += -Werror 
# Sanitizer options
SAN_FLAGS := -fsanitize=address
SAN_LDFLAGS := -fsanitize=address -static-libasan
OPT_FLAGS += -ggdb3 -fno-omit-frame-pointer
endif
OPT_FLAGS += -O2
CFLAGS += ${SAN_FLAGS} ${WARN_FLAGS} ${ERROR_FLAGS} ${OPT_FLAGS} -DDEBUG=1

C_STD := -std=gnu11
CXX_STD := -std=gnu++17
# librustyaxe's public headers require GLib, including in standalone builds.
ifeq ($(shell pkg-config --exists glib-2.0 && echo yes),)
$(error Missing glib-2.0 development files; install libglib2.0-dev (or your platform equivalent) and pkg-config)
endif
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LIBS := $(shell pkg-config --libs glib-2.0)
CFLAGS += ${GLIB_CFLAGS}
ifeq ($(LIBRUSTYAXE_PROVIDER),system)
# Put installed headers ahead of the parent's checkout-root headers.
CFLAGS += $(shell pkg-config --cflags librustyaxe 2>/dev/null)
LDFLAGS += $(shell pkg-config --libs-only-L librustyaxe 2>/dev/null) -lrustyaxe
else
LDFLAGS += -L.. -lrustyaxe -Wl,-rpath,'$ORIGIN/..'
endif
CFLAGS += ${C_STD} -I./ -I./ext/ -I./include/ -I./ext/ft8_lib/ -fPIC
ifneq ($(LIBRUSTYAXE_PROVIDER),system)
CFLAGS += -I../
endif
CXXFLAGS := ${CXX_STD} $(filter-out ${C_STD},${CFLAGS})
LDFLAGS += ${SAN_LDFLAGS}
LDFLAGS += $(foreach x,${common_libs},-l${x})
LDFLAGS += ${GLIB_LIBS}

ifeq (${POSTGRESQL},y)
callsign_lookup_libs += pq
CFLAGS += -DUSE_POSTGRESQL
endif

callsign_lookup_ldflags := ${LDFLAGS} $(foreach x,${callsign_lookup_libs},-l${x})
callsign_lookup_ldflags += $(shell pkg-config --libs spatialite)
