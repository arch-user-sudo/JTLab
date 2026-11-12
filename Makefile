CC      ?= gcc
.DEFAULT_GOAL := all
CFLAGS  ?= -O2 -march=native -std=c11 -D_GNU_SOURCE \
           -flto -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables \
           -fno-pie -fno-plt -fno-ident
LDFLAGS ?= -no-pie -Wl,--gc-sections -Wl,--as-needed -Wl,-O1 -s
# Unoptimized, symbol-rich build for the debug target (keeps backtraces,
# is PIE, and never gets stripped).
DEBUG_CFLAGS = -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -std=c11 -D_GNU_SOURCE
LIBS    = -lwayland-client -lm -lpangocairo-1.0 -lpango-1.0 -lgobject-2.0 -lcairo -lglib-2.0 -lrsvg-2 -lpipewire-0.3 -lxkbcommon

PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin

PKGS    = wayland-client wayland-cursor pangocairo glib-2.0 gio-2.0 cairo librsvg-2.0 libpipewire-0.3 gdk-pixbuf-2.0 libsoup-3.0 xkbcommon

CFLAGS  += $(shell pkg-config --cflags $(PKGS) 2>/dev/null | sed 's/-I/-isystem /g')
DEBUG_CFLAGS += $(shell pkg-config --cflags $(PKGS) 2>/dev/null | sed 's/-I/-isystem /g')
LDFLAGS += $(shell pkg-config --libs $(PKGS) 2>/dev/null)

# Aggressive warning set for the hand-written sources.  It is used by the
# normal build too (without -Werror), so new warnings are visible immediately;
# `make strict` turns them into errors.  Generated protocol headers are
# included with -isystem so their own casts do not trip -Wcast-qual.
#
# This is deliberately everything GCC can warn about that is actionable in C11
# without turning normal code into warning soup.  Flags that only produce noise
# for idiomatic C (e.g. -Wpadded) or that conflict with the Wayland/GObject
# headers are intentionally omitted.
WARNFLAGS = -Wall -Wextra -Wpedantic \
            -Wshadow -Wconversion -Wsign-conversion \
            -Wcast-align=strict -Wcast-qual -Wpointer-arith \
            -Wbad-function-cast -Wstrict-prototypes -Wold-style-definition \
            -Wmissing-prototypes -Wmissing-declarations \
            -Wformat=2 -Wformat-security -Wformat-overflow=2 \
            -Wformat-truncation=2 \
            -Wnull-dereference -Wdouble-promotion -Wfloat-equal \
            -Wundef -Wwrite-strings -Wredundant-decls -Wnested-externs \
            -Wswitch-default -Wimplicit-fallthrough=5 \
            -Wlogical-op -Wduplicated-cond -Wduplicated-branches \
            -Wvla -Wdisabled-optimization -Wstrict-overflow=5 \
            -Walloc-zero -Walloc-size-larger-than=9223372036854775807 \
            -Wtrampolines -Wvector-operation-performance \
            -Wbidi-chars=any \
            -Wno-unused-parameter -Wno-missing-field-initializers

STRICT_FLAGS ?= -Werror
STRICT_CFLAGS = -O2 -march=native -std=c11 -D_GNU_SOURCE \
                -fno-strict-aliasing -fno-strict-overflow -I. \
                -isystem $(BUILD_DIR) $(WARNFLAGS) \
                $(shell pkg-config --cflags $(PKGS) 2>/dev/null | sed 's/-I/-isystem /g')

# Static analysis (opt-in).  Reports only; nothing here modifies sources.
TIDY_CHECKS = -*,clang-analyzer-*,bugprone-*,performance-*,readability-*,misc-*,portability-*
tidy:
	clang-tidy -quiet -checks='$(TIDY_CHECKS)' jtlab.c -- $(STRICT_CFLAGS)

cppcheck:
	cppcheck --enable=warning,style,performance,portability --inconclusive \
	         --std=c11 --language=c -I. -I$(BUILD_DIR) \
	         --suppress=missingIncludeSystem jtlab.c

PROTOCOL_DIR = protocols
BUILD_DIR    = build

PROTO_XML = $(PROTOCOL_DIR)/wlr-layer-shell-unstable-v1.xml
TOPLEVEL_XML = $(PROTOCOL_DIR)/wlr-foreign-toplevel-management-unstable-v1.xml
WORKSPACE_XML = $(PROTOCOL_DIR)/ext-workspace-v1.xml
GAMMA_XML = $(PROTOCOL_DIR)/wlr-gamma-control-unstable-v1.xml
SCREENCOPY_XML = $(PROTOCOL_DIR)/wlr-screencopy-unstable-v1.xml
VKEYBOARD_XML = $(PROTOCOL_DIR)/virtual-keyboard-unstable-v1.xml

all: jtlab jtlabctl

debug: jtlab-debug

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Protocol generation
$(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h: $(PROTO_XML) | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.c: $(PROTO_XML) | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/xdg-shell-client-protocol.h: $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/xdg-shell-protocol.c: $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h: $(TOPLEVEL_XML) | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/wlr-foreign-toplevel-management-unstable-v1-protocol.c: $(TOPLEVEL_XML) | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/ext-workspace-v1-client-protocol.h: $(WORKSPACE_XML) | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/ext-workspace-v1-client-protocol.c: $(WORKSPACE_XML) | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/wlr-gamma-control-unstable-v1-client-protocol.h: $(GAMMA_XML) | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/wlr-gamma-control-unstable-v1-protocol.c: $(GAMMA_XML) | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.h: $(SCREENCOPY_XML) | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/wlr-screencopy-unstable-v1-protocol.c: $(SCREENCOPY_XML) | $(BUILD_DIR)
	wayland-scanner private-code $< $@

$(BUILD_DIR)/virtual-keyboard-unstable-v1-client-protocol.h: $(VKEYBOARD_XML) | $(BUILD_DIR)
	wayland-scanner client-header $< $@

$(BUILD_DIR)/virtual-keyboard-unstable-v1-protocol.c: $(VKEYBOARD_XML) | $(BUILD_DIR)
	wayland-scanner private-code $< $@

PROTO_HDRS = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
             $(BUILD_DIR)/xdg-shell-client-protocol.h \
             $(BUILD_DIR)/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h \
             $(BUILD_DIR)/ext-workspace-v1-client-protocol.h \
             $(BUILD_DIR)/wlr-gamma-control-unstable-v1-client-protocol.h \
             $(BUILD_DIR)/wlr-screencopy-unstable-v1-client-protocol.h \
             $(BUILD_DIR)/virtual-keyboard-unstable-v1-client-protocol.h

PROTO_SRCS = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.c \
             $(BUILD_DIR)/xdg-shell-protocol.c \
             $(BUILD_DIR)/wlr-foreign-toplevel-management-unstable-v1-protocol.c \
             $(BUILD_DIR)/ext-workspace-v1-client-protocol.c \
             $(BUILD_DIR)/wlr-gamma-control-unstable-v1-protocol.c \
             $(BUILD_DIR)/wlr-screencopy-unstable-v1-protocol.c \
             $(BUILD_DIR)/virtual-keyboard-unstable-v1-protocol.c

# Defined after PROTO_HDRS so the prerequisite list is not expanded while the
# variable is still empty.
strict: $(PROTO_HDRS)
	$(CC) $(STRICT_CFLAGS) $(STRICT_FLAGS) -fsyntax-only jtlab.c jtlabctl.c

PROTO_OBJS = $(PROTO_SRCS:.c=.o)

jtlab.o: jtlab.c config.h $(PROTO_HDRS)
	$(CC) $(CFLAGS) $(WARNFLAGS) -isystem $(BUILD_DIR) -c $< -o $@

$(BUILD_DIR)/%.o: $(BUILD_DIR)/%.c
	$(CC) $(CFLAGS) -I$(BUILD_DIR) -c $< -o $@

jtlab: jtlab.o $(PROTO_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LIBS)

# Debug build (ASan/UBSan, -O1). Built WITHOUT the small-binary flags so
# backtraces, symbol names and PIE addresses stay useful, and never stripped.
$(BUILD_DIR)/jtlab-debug.o: jtlab.c config.h $(PROTO_HDRS)
	$(CC) $(DEBUG_CFLAGS) $(WARNFLAGS) -isystem $(BUILD_DIR) -c $< -o $@

$(BUILD_DIR)/%-debug.o: $(BUILD_DIR)/%.c
	$(CC) $(DEBUG_CFLAGS) -I$(BUILD_DIR) -c $< -o $@

DEBUG_OBJS = $(PROTO_SRCS:$(BUILD_DIR)/%.c=$(BUILD_DIR)/%-debug.o)

jtlab-debug: $(BUILD_DIR)/jtlab-debug.o $(DEBUG_OBJS)
	$(CC) $^ -o $@ $(filter-out -s -no-pie,$(LDFLAGS)) $(LIBS) -fsanitize=address,undefined

jtlabctl: jtlabctl.c
	$(CC) $(CFLAGS) $(WARNFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR) jtlab.o jtlab jtlabctl jtlab-debug

install: jtlab jtlabctl
	install -Dm755 jtlab $(DESTDIR)$(BINDIR)/jtlab
	install -Dm755 jtlabctl $(DESTDIR)$(BINDIR)/jtlabctl

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jtlab $(DESTDIR)$(BINDIR)/jtlabctl

.PHONY: all clean install uninstall debug strict tidy cppcheck
