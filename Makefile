CC      ?= gcc
CFLAGS  ?= -Os -march=native -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -std=c11 -D_GNU_SOURCE
LDFLAGS ?=
LIBS    = -lwayland-client -lm -lpangocairo-1.0 -lpango-1.0 -lgobject-2.0 -lcairo -lglib-2.0 -lrsvg-2 -lpipewire-0.3 -lxkbcommon

CFLAGS  += $(shell pkg-config --cflags wayland-client wayland-cursor pangocairo glib-2.0 gio-2.0 cairo librsvg-2.0 libpipewire-0.3 gdk-pixbuf-2.0 libsoup-3.0 xkbcommon 2>/dev/null)
LDFLAGS += $(shell pkg-config --libs wayland-client wayland-cursor pangocairo glib-2.0 gio-2.0 cairo librsvg-2.0 libpipewire-0.3 gdk-pixbuf-2.0 libsoup-3.0 xkbcommon 2>/dev/null)

PROTOCOL_DIR = protocols
BUILD_DIR    = build

PROTO_XML = $(PROTOCOL_DIR)/wlr-layer-shell-unstable-v1.xml
TOPLEVEL_XML = $(PROTOCOL_DIR)/wlr-foreign-toplevel-management-unstable-v1.xml
WORKSPACE_XML = $(PROTOCOL_DIR)/ext-workspace-v1.xml

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

PROTO_HDRS = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h \
             $(BUILD_DIR)/xdg-shell-client-protocol.h \
             $(BUILD_DIR)/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h \
             $(BUILD_DIR)/ext-workspace-v1-client-protocol.h

PROTO_SRCS = $(BUILD_DIR)/wlr-layer-shell-unstable-v1-client-protocol.c \
             $(BUILD_DIR)/xdg-shell-protocol.c \
             $(BUILD_DIR)/wlr-foreign-toplevel-management-unstable-v1-protocol.c \
             $(BUILD_DIR)/ext-workspace-v1-client-protocol.c

PROTO_OBJS = $(PROTO_SRCS:.c=.o)

jtlab.o: jtlab.c config.h $(PROTO_HDRS)
	$(CC) $(CFLAGS) -I$(BUILD_DIR) -c $< -o $@

$(BUILD_DIR)/%.o: $(BUILD_DIR)/%.c
	$(CC) $(CFLAGS) -I$(BUILD_DIR) -c $< -o $@

jtlab: jtlab.o $(PROTO_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS) $(LIBS)

jtlab-debug: $(PROTO_OBJS)
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -I$(BUILD_DIR) -c jtlab.c -o $(BUILD_DIR)/jtlab-debug.o
	$(CC) $(BUILD_DIR)/jtlab-debug.o $(PROTO_OBJS) -o $@ $(LDFLAGS) $(LIBS) -fsanitize=address,undefined

jtlabctl: jtlabctl.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD_DIR) jtlab.o jtlab jtlabctl jtlab-debug

install: jtlab jtlabctl
	install -Dm755 jtlab /usr/local/bin/jtlab
	install -Dm755 jtlabctl /usr/local/bin/jtlabctl

uninstall:
	rm -f /usr/local/bin/jtlab /usr/local/bin/jtlabctl

.PHONY: all clean install uninstall debug
