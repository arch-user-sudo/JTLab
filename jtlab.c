#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <execinfo.h>
#include <linux/input-event-codes.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <librsvg/rsvg.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libsoup/soup.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>

#include "config.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_cursor_theme *cursor_theme;
    struct wl_cursor *cursor_default;
    struct wl_surface *cursor_surface;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr;
    struct ext_workspace_manager_v1 *ext_ws_mgr;
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t compositor_name, shm_name, seat_name, layer_shell_name;
    uint32_t toplevel_mgr_name, ext_ws_mgr_name;
} g_wl = { 0 };

static void
cursor_init(void)
{
    g_wl.cursor_theme = wl_cursor_theme_load(NULL, 24, g_wl.shm);
    if (!g_wl.cursor_theme) return;
    g_wl.cursor_default = wl_cursor_theme_get_cursor(g_wl.cursor_theme, "default");
    if (!g_wl.cursor_default)
        g_wl.cursor_default = wl_cursor_theme_get_cursor(g_wl.cursor_theme, "left_ptr");
    if (!g_wl.cursor_default) return;
    g_wl.cursor_surface = wl_compositor_create_surface(g_wl.compositor);
}

static void
cursor_set(struct wl_pointer *pointer, uint32_t serial)
{
    if (!g_wl.cursor_default || !g_wl.cursor_surface) return;
    struct wl_cursor_image *img = g_wl.cursor_default->images[0];
    wl_pointer_set_cursor(pointer, serial, g_wl.cursor_surface,
                          img->hotspot_x, img->hotspot_y);
    wl_surface_attach(g_wl.cursor_surface,
                      wl_cursor_image_get_buffer(img), 0, 0);
    wl_surface_damage(g_wl.cursor_surface, 0, 0, img->width, img->height);
    wl_surface_commit(g_wl.cursor_surface);
}

struct app_entry {
    char *name;
    char *exec;
    char *icon_name;
    char *desktop_id;
    char *wm_class;
    cairo_surface_t *icon;
    int terminal;
};

static struct {
    int running;
    char search[64];
    int *menu_idx;
    int menu_idx_cap;
    int menu_n;
    int menu_sel;
    struct app_entry *apps;
    int n_apps;
    int apps_cap;
    int *pinned;
    int n_pinned;
    int pinned_cap;
    int apps_fd;
    char art_url[512];
} g_app = { .running = 1 };

struct pointer_state {
    struct wl_pointer *pointer;
    double x, y;
    struct wl_surface *enter_surface;
    int hover_menu_idx;
    int hover_ctx_idx;
    int hover_pinned_idx;
    int hover_taskbar_idx;
    int hover_pill;
    int hover_bell;
    int hover_clock;
    int hover_net;
    int hover_audio;
    int hover_tray;
    int hover_ws;
};

static struct pointer_state g_pointer = { .hover_tray = -1 };

struct ctx_menu {
    int open;
    int target_app;
    int n_items;
    double x, y;
};

static struct ctx_menu g_ctx = { 0 };

struct toplevel_entry {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *app_id;
    char *title;
    cairo_surface_t *icon;
    int activated;
};

#define MAX_TOPLEVELS 128
static struct toplevel_entry g_toplevels[MAX_TOPLEVELS];
static int g_n_toplevels = 0;

struct shm_pool {
    int fd;
    size_t size;
    void *data;
    struct wl_shm_pool *wl_pool;
    struct wl_buffer *buffer;
};

static void
shm_pool_cleanup(struct shm_pool *p)
{
    if (p->buffer) { wl_buffer_destroy(p->buffer); p->buffer = NULL; }
    if (p->wl_pool) { wl_shm_pool_destroy(p->wl_pool); p->wl_pool = NULL; }
    if (p->data && p->data != MAP_FAILED) { munmap(p->data, p->size); p->data = NULL; }
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
}

static int
shm_pool_init(struct shm_pool *p, size_t size)
{
    shm_pool_cleanup(p);
    p->fd = -1;
    p->data = MAP_FAILED;
    p->wl_pool = NULL;
    p->buffer = NULL;
    p->size = size;

    for (int i = 0; i < 100; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/jtlab-%d-%d", getpid(), i);
        p->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (p->fd >= 0) {
            shm_unlink(name);
            if (ftruncate(p->fd, size) == 0) break;
            close(p->fd);
            p->fd = -1;
        }
    }
    if (p->fd < 0) return -1;

    p->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, 0);
    if (p->data == MAP_FAILED) { close(p->fd); p->fd = -1; return -1; }
    p->wl_pool = wl_shm_create_pool(g_wl.shm, p->fd, size);
    if (!p->wl_pool) { shm_pool_cleanup(p); return -1; }
    return 0;
}

static struct wl_buffer *
shm_pool_create_buffer(struct shm_pool *p, int w, int h, int stride, uint32_t fmt)
{
    return wl_shm_pool_create_buffer(p->wl_pool, 0, w, h, stride, fmt);
}

static void
pool_cairo_init(struct shm_pool *pool, cairo_surface_t **sf, cairo_t **cr,
                int phys_w, int phys_h, int stride, int scale)
{
    if (*sf) { cairo_surface_destroy(*sf); *sf = NULL; }
    if (*cr) { cairo_destroy(*cr); *cr = NULL; }
    *sf = cairo_image_surface_create_for_data(pool->data, CAIRO_FORMAT_ARGB32,
                                              phys_w, phys_h, stride);
    *cr = cairo_create(*sf);
    cairo_scale(*cr, scale, scale);
}

struct bar {
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    int width, height, scale, configured;
    struct shm_pool pool;
    cairo_surface_t *cairo;
    cairo_t *cr;
    _Atomic int needs_render;
    struct wl_callback *frame_cb;
    int frame_pending;
    int resize_pending;
    int size_dirty;

    int menu_open;
    int menu_height;
    int menu_scroll;
    double menu_anim_px;
    double menu_scroll_target_px;
    GSource *menu_anim_src;
    int prev_height;

    struct wl_surface *menu_surface;
    struct zwlr_layer_surface_v1 *menu_layer;
    int menu_surf_width, menu_surf_height, menu_surf_configured;
    struct shm_pool menu_pool;
    cairo_surface_t *menu_cairo;
    cairo_t *menu_cr;
    _Atomic int menu_needs_render;
    struct wl_callback *menu_frame_cb;
    int menu_frame_pending;
};

static struct bar g_bar = { 0 };

static void
render_request(void)
{
    g_bar.needs_render = 1;
    if (g_bar.menu_open)
        g_bar.menu_needs_render = 1;
}

struct power_menu {
    int hover;
};

static struct power_menu g_pmenu = { 0 };

enum { SYS_STAT_RAM, SYS_STAT_CPU, SYS_STAT_GPU, SYS_STAT_COUNT };

struct sys_stats {
    double pct[SYS_STAT_COUNT];
    int pct_shown[SYS_STAT_COUNT];
    long long cpu_prev_total, cpu_prev_idle;
    int cpu_prev_valid;

    char player_bus[128];
    char track[160];
    char artist[160];
    char album[160];
    char track_id[256];
    char art_path[512];
    cairo_surface_t *art;
    char status[16];
    guint props_sig;
    guint owner_sig;
    int64_t last_refresh_ms;
    int ctrl_hover;
};

static struct sys_stats g_sys = { .ctrl_hover = -1 };

struct bar_item {
    double x, w;
};

#define MAX_TASKBAR_GROUPS 256
#define MAX_WINDOWS_PER_GROUP 32

struct taskbar_group {
    double x, w;
    int n_windows;
    int windows[MAX_WINDOWS_PER_GROUP];
};

#define PW_MAX_DEVICES 32

struct audio_snap_item {
    char desc[160];
    int is_sink;
    int is_default;
    int dev_idx;
};

struct audio_snapshot {
    int n_sinks, n_sources;
    float sink_volume, source_volume;
    int sink_muted, source_muted;
    int has_value;
    struct audio_snap_item items[PW_MAX_DEVICES];
};

static struct {
    int sys_fd;
    struct bar_item pl[256];
    int n_pl;
    struct taskbar_group taskbar_groups[MAX_TASKBAR_GROUPS];
    int n_taskbar_groups;
    int64_t resize_at_ms;
    int64_t last_motion_at_ms;
    char time_str[24];
    int net_status;
    int audio_present;
    struct audio_snapshot vol_snap;
} g_box = { 0 };

struct group_popup {
    int open;
    int windows[MAX_WINDOWS_PER_GROUP];
    int n_windows;
    double x, y;
    int height;
    int hover_idx;
    int hover_close;
};

static struct group_popup g_gpopup = { 0 };

struct close_popup {
    int open;
    int window_idx;
    int app_idx;
    int n_items;
    int hover;
    double x, y;
    double w, h;
};

static struct close_popup g_cpop = { 0 };

static int64_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int
popup_coords_safe(uint32_t event_time)
{
    (void)event_time;
    if (g_bar.resize_pending) return 0;
    if (g_box.last_motion_at_ms <= g_box.resize_at_ms) return 0;
    return 1;
}

static void
time_now(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%I:%M %p", &tm);
}

#define clampd(v, lo, hi) \
    ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static PangoContext *g_pango_ctx;

static PangoLayout *
pango_ctx_layout(cairo_t *cr)
{
    if (!g_pango_ctx)
        g_pango_ctx = pango_cairo_create_context(cr);
    PangoLayout *pl = pango_layout_new(g_pango_ctx);
    pango_cairo_update_layout(cr, pl);
    return pl;
}

static PangoLayout *
clock_layout(cairo_t *cr)
{
    static PangoLayout *pl = NULL;
    static cairo_t *bound_cr = NULL;
    static char prev[24] = "";
    if (!g_pango_ctx)
        g_pango_ctx = pango_cairo_create_context(cr);
    if (!pl) {
        pl = pango_layout_new(g_pango_ctx);
        PangoFontDescription *pfd = pango_font_description_from_string(FONT_FAMILY);
        pango_font_description_set_absolute_size(pfd, FONT_SIZE * PANGO_SCALE);
        pango_layout_set_font_description(pl, pfd);
        pango_font_description_free(pfd);
    }
    if (bound_cr != cr) {
        pango_cairo_update_layout(cr, pl);
        bound_cr = cr;
    }
    if (strcmp(prev, g_box.time_str) != 0) {
        pango_layout_set_text(pl, g_box.time_str, -1);
        snprintf(prev, sizeof(prev), "%s", g_box.time_str);
    }
    return pl;
}

static double
text_width_px(const char *text)
{
    if (!g_bar.cr) return 0;
    PangoFontDescription *pfd = pango_font_description_from_string(FONT_FAMILY);
    pango_font_description_set_absolute_size(pfd, FONT_SIZE * PANGO_SCALE);
    PangoLayout *pl = pango_ctx_layout(g_bar.cr);
    pango_layout_set_font_description(pl, pfd);
    pango_layout_set_text(pl, text, -1);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    (void)th;
    g_object_unref(pl);
    pango_font_description_free(pfd);
    return tw;
}

static double
time_text_width(void)
{
    static char prev[24] = "";
    static double prev_w;
    static cairo_t *bound_cr;
    char buf[24];
    if (!g_bar.cr) return 0;
    time_now(buf, sizeof(buf));
    if (bound_cr != g_bar.cr || strcmp(prev, buf) != 0) {
        prev_w = text_width_px(buf);
        snprintf(prev, sizeof(prev), "%s", buf);
        bound_cr = g_bar.cr;
    }
    return prev_w;
}

enum { NET_NONE, NET_WIFI, NET_ETHERNET };

static double
net_icon_x(void)
{
    double bell_sz = NOTIF_ICON_SIZE;
    double bell_left = g_bar.width - TRAY_EDGE_PAD - bell_sz;
    double clock_left = bell_left - TRAY_GAP - time_text_width();
    return clock_left - TRAY_GAP - NOTIF_ICON_SIZE;
}

static int
net_iface_carrier(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char c = 0;
    ssize_t r = read(fd, &c, 1);
    close(fd);
    return r > 0 && c == '1';
}

static void
net_update(void)
{
    int wifi = 0, ether = 0;
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *n = e->d_name;
        if (n[0] == '.') continue;
        if (strcmp(n, "lo") == 0) continue;
        if (strncmp(n, "veth", 4) == 0 || strncmp(n, "virbr", 5) == 0 ||
            strncmp(n, "docker", 6) == 0 || strncmp(n, "br-", 3) == 0 ||
            strncmp(n, "wg", 2) == 0 || strncmp(n, "tun", 3) == 0 ||
            strncmp(n, "tap", 3) == 0)
            continue;
        if (!net_iface_carrier(n)) continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", n);
        if (access(path, F_OK) == 0)
            wifi = 1;
        else
            ether = 1;
    }
    closedir(d);
    int s = ether ? NET_ETHERNET : (wifi ? NET_WIFI : NET_NONE);
    if (s != g_box.net_status) {
        g_box.net_status = s;
        render_request();
    }
}

static double
audio_icon_x(void)
{
    return net_icon_x() - TRAY_GAP - NOTIF_ICON_SIZE;
}

static void
audio_update(void)
{
    int present = 0;
    int fd = open("/proc/asound/cards", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r > 0) {
            buf[r] = '\0';
            present = (strchr(buf, '[') != NULL);
        }
    }
    if (present != g_box.audio_present) {
        g_box.audio_present = present;
        render_request();
    }
}

struct audio_dev {
    uint32_t id;
    struct pw_proxy *proxy;
    struct spa_hook node_listener;
    struct spa_hook proxy_listener;
    float volume;
    int muted;
    int channels;
    int is_sink;
    char name[128];
    char desc[160];
};

static struct {
    struct pw_thread_loop *thread_loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_metadata *metadata;
    struct spa_hook metadata_listener;
    struct audio_dev devices[PW_MAX_DEVICES];
    _Atomic int n_devices;
    _Atomic int n_sinks, n_sources;
    int sink_idx;
    int source_idx;
    char default_sink_json[512];
    char default_source_json[512];
    _Atomic int default_sink_valid;
    _Atomic int default_source_valid;
    struct spa_hook core_listener;
    _Atomic int connected;
    _Atomic int core_dead;
    _Atomic int pw_inited;
    int loop_started;
    int64_t retry_at_ms;
    int retry_delay_ms;
} g_pw = { 0 };

static void node_event_param(void *data, int seq, uint32_t id, uint32_t index,
                             uint32_t next, const struct spa_pod *param);
static const struct pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .param = node_event_param,
};

static const struct pw_proxy_events node_proxy_events = {
    .version = PW_VERSION_PROXY_EVENTS,
};

static int
metadata_event_property(void *data, uint32_t subject, const char *key,
                        const char *type, const char *value)
{
    (void)data; (void)subject; (void)type;
    if (!key || !value) return 0;
    if (strcmp(key, "default.audio.sink") == 0) {
        snprintf(g_pw.default_sink_json, sizeof(g_pw.default_sink_json), "%s", value);
        g_pw.default_sink_valid = 1;
        render_request();
    } else if (strcmp(key, "default.audio.source") == 0) {
        snprintf(g_pw.default_source_json, sizeof(g_pw.default_source_json), "%s", value);
        g_pw.default_source_valid = 1;
        render_request();
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = metadata_event_property,
};

static void
core_event_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    (void)data; (void)id; (void)seq; (void)res; (void)message;
    g_pw.core_dead = 1;
    render_request();
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = core_event_error,
};

static void
registry_event_global(void *data, uint32_t id, uint32_t permissions,
                      const char *type, uint32_t version,
                      const struct spa_dict *props)
{
    (void)data; (void)permissions;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        if (!props) return;
        const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!mc) return;
        int is_sink;
        if (strcmp(mc, "Audio/Sink") == 0) is_sink = 1;
        else if (strcmp(mc, "Audio/Source") == 0) is_sink = 0;
        else return;

        if (g_pw.n_devices >= PW_MAX_DEVICES) return;
        struct audio_dev *d = &g_pw.devices[g_pw.n_devices];
        memset(d, 0, sizeof(*d));
        d->id = id;
        d->is_sink = is_sink;

        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        if (!desc) desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
        if (node_name)
            snprintf(d->name, sizeof(d->name), "%s", node_name);
        else if (desc)
            snprintf(d->name, sizeof(d->name), "%s", desc);
        else
            snprintf(d->name, sizeof(d->name), "node-%u", id);
        char tdesc[160];
        if (desc)
            snprintf(tdesc, sizeof(tdesc), "%s", desc);
        else
            snprintf(tdesc, sizeof(tdesc), "%s", d->name);
        snprintf(d->desc, sizeof(d->desc), "%s", tdesc);

        d->proxy = pw_registry_bind(g_pw.registry, id, type,
                                    version < PW_VERSION_NODE ? version : PW_VERSION_NODE, 0);
        if (d->proxy) {
            spa_zero(d->node_listener);
            spa_zero(d->proxy_listener);
            pw_proxy_add_object_listener(d->proxy, &d->node_listener,
                                         &node_events, d);
            pw_proxy_add_listener(d->proxy, &d->proxy_listener,
                                  &node_proxy_events, d);
            uint32_t ids[] = { SPA_PARAM_Props };
            pw_node_subscribe_params((struct pw_node *)d->proxy, ids, 1);
        }
        g_pw.n_devices++;
        if (is_sink) g_pw.n_sinks++;
        else g_pw.n_sources++;
        render_request();
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        if (!props) return;
        const char *mn = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!mn || strcmp(mn, "default") != 0) return;
        if (g_pw.metadata) return;
        g_pw.metadata = (struct pw_metadata *)pw_registry_bind(
            g_pw.registry, id, type,
            version < PW_VERSION_METADATA ? version : PW_VERSION_METADATA, 0);
        if (g_pw.metadata) {
            spa_zero(g_pw.metadata_listener);
            pw_metadata_add_listener(g_pw.metadata, &g_pw.metadata_listener,
                                     &metadata_events, NULL);
        }
        return;
    }
}

static void
registry_event_global_remove(void *data, uint32_t id)
{
    (void)data;
    for (int i = 0; i < g_pw.n_devices; i++) {
        if (g_pw.devices[i].id != id) continue;
        struct audio_dev *d = &g_pw.devices[i];
        int n_after = g_pw.n_devices - i - 1;

        if (d->proxy) {
            spa_hook_remove(&d->node_listener);
            spa_hook_remove(&d->proxy_listener);
            pw_proxy_destroy(d->proxy);
            d->proxy = NULL;
        }
        if (d->is_sink) g_pw.n_sinks--;
        else g_pw.n_sources--;
        if (g_pw.sink_idx == i) g_pw.sink_idx = -1;
        else if (g_pw.sink_idx > i) g_pw.sink_idx--;
        if (g_pw.source_idx == i) g_pw.source_idx = -1;
        else if (g_pw.source_idx > i) g_pw.source_idx--;

        if (n_after > 0) {
            for (int k = i + 1; k < g_pw.n_devices; k++) {
                struct audio_dev *s = &g_pw.devices[k];
                if (s->proxy) {
                    spa_hook_remove(&s->node_listener);
                    spa_hook_remove(&s->proxy_listener);
                }
            }
        }

        memmove(&g_pw.devices[i], &g_pw.devices[i + 1],
                n_after * sizeof(struct audio_dev));
        g_pw.n_devices--;

        for (int k = i; k < g_pw.n_devices; k++) {
            struct audio_dev *s = &g_pw.devices[k];
            if (s->proxy) {
                pw_proxy_add_object_listener(s->proxy, &s->node_listener,
                                             &node_events, s);
                pw_proxy_add_listener(s->proxy, &s->proxy_listener,
                                      &node_proxy_events, s);
            }
        }
        render_request();
        return;
    }
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static void
node_event_param(void *data, int seq, uint32_t id, uint32_t index,
                 uint32_t next, const struct spa_pod *param)
{
    (void)seq; (void)index; (void)next;

    struct audio_dev *d = data;
    if (!d) return;
    if (id != SPA_PARAM_Props || !param) return;
    if (param->type != SPA_TYPE_Object) return;

    const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
    if (obj->body.type != SPA_TYPE_OBJECT_Props) return;
    if (param->size <= sizeof(struct spa_pod_object_body)) return;

    const uint8_t *body = (const uint8_t *)&obj->body + sizeof(struct spa_pod_object_body);
    uint32_t body_size = param->size - sizeof(struct spa_pod_object_body);

    float volumes[64];
    int n_ch = 0;
    int muted = -1;

    uint32_t off = 0;
    while (off + 8 <= body_size) {
        const struct spa_pod_prop *prop = (const struct spa_pod_prop *)(body + off);
        uint32_t padded = SPA_ROUND_UP_N(SPA_POD_PROP_SIZE(prop), 8);
        if (padded == 0 || off + padded > body_size) break;

        if (prop->key == SPA_PROP_channelVolumes && prop->value.type == SPA_TYPE_Array) {
            const struct spa_pod_array_body *arr =
                (const struct spa_pod_array_body *)SPA_POD_BODY(&prop->value);
            uint32_t child_size = arr->child.size;
            if (child_size == sizeof(float)) {
                const void *data_ptr = (const uint8_t *)arr + sizeof(*arr);
                uint32_t data_size = SPA_POD_BODY_SIZE(&prop->value) - sizeof(*arr);
                n_ch = data_size / child_size;
                if (n_ch > 64) n_ch = 64;
                const float *vol_data = data_ptr;
                for (int i = 0; i < n_ch; i++)
                    volumes[i] = vol_data[i];
            }
        } else if (prop->key == SPA_PROP_mute && prop->value.type == SPA_TYPE_Bool) {
            muted = *(const uint32_t *)SPA_POD_BODY(&prop->value) ? 1 : 0;
        }
        off += padded;
    }

    if (n_ch > 0) {
        float sum = 0;
        for (int i = 0; i < n_ch; i++) sum += volumes[i];
        d->volume = cbrtf(sum / n_ch) * 100.0f;
        d->channels = n_ch;
    }
    if (muted >= 0) d->muted = muted;
    render_request();
}

static void
extract_json_name(const char *json, char *out, size_t outsz)
{
    out[0] = '\0';
    if (!json) return;
    const char *p = strstr(json, "\"name\"");
    if (!p) { snprintf(out, outsz, "%s", json); return; }
    const char *q = strchr(p + 6, ':');
    if (!q) return;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '"') { snprintf(out, outsz, "%s", json); return; }
    q++;
    size_t n = 0;
    while (*q && *q != '"' && n + 1 < outsz) out[n++] = *q++;
    out[n] = '\0';
}

static void
audio_snapshot(struct audio_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    if (!g_pw.thread_loop) return;

    pw_thread_loop_lock(g_pw.thread_loop);

    snap->n_sinks = g_pw.n_sinks;
    snap->n_sources = g_pw.n_sources;

    char default_sink[256], default_source[256];
    if (g_pw.default_sink_valid)
        extract_json_name(g_pw.default_sink_json, default_sink, sizeof(default_sink));
    else
        default_sink[0] = '\0';
    if (g_pw.default_source_valid)
        extract_json_name(g_pw.default_source_json, default_source, sizeof(default_source));
    else
        default_source[0] = '\0';

    int best_sink = -1, best_source = -1;
    for (int i = 0; i < g_pw.n_devices; i++) {
        struct audio_dev *d = &g_pw.devices[i];
        struct audio_snap_item *it = &snap->items[i];
        snprintf(it->desc, sizeof(it->desc), "%s", d->desc);
        it->is_sink = d->is_sink;
        it->dev_idx = i;
        if (d->is_sink && default_sink[0] && strcmp(default_sink, d->name) == 0) {
            it->is_default = 1;
            if (best_sink < 0) best_sink = i;
        } else if (!d->is_sink && default_source[0] && strcmp(default_source, d->name) == 0) {
            it->is_default = 1;
            if (best_source < 0) best_source = i;
        }
    }
    if (best_sink < 0)
        for (int i = 0; i < g_pw.n_devices; i++)
            if (g_pw.devices[i].is_sink) { best_sink = i; break; }
    if (best_source < 0)
        for (int i = 0; i < g_pw.n_devices; i++)
            if (!g_pw.devices[i].is_sink) { best_source = i; break; }

    g_pw.sink_idx = best_sink;
    g_pw.source_idx = best_source;

    if (best_sink >= 0) {
        snap->sink_volume = g_pw.devices[best_sink].volume;
        snap->sink_muted = g_pw.devices[best_sink].muted;
    }
    if (best_source >= 0) {
        snap->source_volume = g_pw.devices[best_source].volume;
        snap->source_muted = g_pw.devices[best_source].muted;
    }
    snap->has_value = (best_sink >= 0 || best_source >= 0);

    pw_thread_loop_unlock(g_pw.thread_loop);
}

static void pw_disconnect(void);

static void
pw_drop_core(void)
{
    if (g_pw.registry) {
        spa_hook_remove(&g_pw.registry_listener);
        pw_proxy_destroy((struct pw_proxy *)g_pw.registry);
        g_pw.registry = NULL;
    }
    if (g_pw.core) {
        spa_hook_remove(&g_pw.core_listener);
        pw_core_disconnect(g_pw.core);
        g_pw.core = NULL;
    }
}

static void
pw_connect(void)
{
    if (g_pw.connected && !g_pw.core_dead) return;

    if (!g_pw.thread_loop) {
        if (!g_pw.pw_inited) {
            pw_init(NULL, NULL);
            g_pw.pw_inited = 1;
        }
        g_pw.thread_loop = pw_thread_loop_new("jtlab", NULL);
        if (!g_pw.thread_loop) return;
        g_pw.context = pw_context_new(pw_thread_loop_get_loop(g_pw.thread_loop),
                                      NULL, 0);
        if (!g_pw.context) {
            pw_thread_loop_destroy(g_pw.thread_loop);
            g_pw.thread_loop = NULL;
            return;
        }
        if (pw_thread_loop_start(g_pw.thread_loop) != 0) {
            pw_context_destroy(g_pw.context);
            pw_thread_loop_destroy(g_pw.thread_loop);
            g_pw.thread_loop = NULL;
            g_pw.context = NULL;
            return;
        }
        g_pw.loop_started = 1;
    }

    pw_thread_loop_lock(g_pw.thread_loop);

    g_pw.connected = 0;

    g_pw.core = pw_context_connect(g_pw.context, NULL, 0);
    if (g_pw.core) {
        spa_zero(g_pw.core_listener);
        pw_core_add_listener(g_pw.core, &g_pw.core_listener, &core_events, NULL);
        g_pw.core_dead = 0;

        g_pw.registry = pw_core_get_registry(g_pw.core, PW_VERSION_REGISTRY, 0);
        if (g_pw.registry) {
            spa_zero(g_pw.registry_listener);
            pw_registry_add_listener(g_pw.registry, &g_pw.registry_listener,
                                     &registry_events, NULL);
            g_pw.connected = 1;
            g_pw.retry_delay_ms = 250;
        }
    }

    if (!g_pw.connected)
        pw_drop_core();

    pw_thread_loop_unlock(g_pw.thread_loop);
}

static void
pw_disconnect(void)
{
    if (!g_pw.thread_loop) return;

    pw_thread_loop_lock(g_pw.thread_loop);

    for (int i = 0; i < g_pw.n_devices; i++) {
        struct audio_dev *d = &g_pw.devices[i];
        if (d->proxy) {
            spa_hook_remove(&d->node_listener);
            spa_hook_remove(&d->proxy_listener);
            pw_proxy_destroy(d->proxy);
            d->proxy = NULL;
        }
    }
    g_pw.n_devices = 0;
    g_pw.n_sinks = 0;
    g_pw.n_sources = 0;
    g_pw.sink_idx = -1;
    g_pw.source_idx = -1;

    if (g_pw.metadata) {
        spa_hook_remove(&g_pw.metadata_listener);
        pw_proxy_destroy((struct pw_proxy *)g_pw.metadata);
        g_pw.metadata = NULL;
    }
    pw_drop_core();

    g_pw.default_sink_valid = 0;
    g_pw.default_source_valid = 0;
    g_pw.connected = 0;
    g_pw.core_dead = 0;

    pw_thread_loop_unlock(g_pw.thread_loop);
}

static void
pw_reconnect_check(void)
{
    if (g_pw.connected && !g_pw.core_dead) return;

    int64_t now = now_ms();
    if (now < g_pw.retry_at_ms) return;

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        char path[512];
        snprintf(path, sizeof(path), "%s/pipewire-0", rt);
        if (access(path, F_OK) != 0) {
            g_pw.retry_at_ms = now + 1000;
            return;
        }
    }

    if (g_pw.thread_loop)
        pw_disconnect();

    g_pw.retry_at_ms = now + g_pw.retry_delay_ms;
    g_pw.retry_delay_ms *= 2;
    if (g_pw.retry_delay_ms > 5000) g_pw.retry_delay_ms = 5000;
    pw_connect();
}

static void
pw_shutdown(void)
{
    if (g_pw.thread_loop) {
        if (g_pw.loop_started)
            pw_thread_loop_stop(g_pw.thread_loop);

        pw_disconnect();

        pw_context_destroy(g_pw.context);
        g_pw.context = NULL;
        pw_thread_loop_destroy(g_pw.thread_loop);
        g_pw.thread_loop = NULL;
        g_pw.loop_started = 0;
    }

    if (g_pw.pw_inited) {
        pw_deinit();
        g_pw.pw_inited = 0;
    }
}
static int
trace_on(void)
{
    static int v = -1;
    if (v < 0) v = getenv("JTLAB_TRACE") ? 1 : 0;
    return v;
}

static void
pinned_recalc_layout(void)
{
    g_box.n_pl = 0;
    if (!g_bar.cr || !g_bar.configured) return;

    double px_x = 10 + MENU_PILL_W + PINNED_GAP;
    double px_max_x = g_bar.width - 10;

    for (int i = 0; i < g_app.n_pinned && g_box.n_pl < 256; i++) {
        double item_w = PINNED_ICON + PINNED_SPACING * 2;

        if (px_x + item_w > px_max_x) break;

        g_box.pl[g_box.n_pl].x = px_x;
        g_box.pl[g_box.n_pl].w = item_w;
        g_box.n_pl++;
        px_x += item_w;
    }
}

static void gpopup_close(void);
static void cpop_clear_hover(void);
static void cpop_open(int app_idx, int window_idx, double x, double y);
static void cpop_close(void);
static int cpop_hit_test(double px, double py);
static int app_match(const char *desktop_id, const char *app_id);
static int app_matches_id(const struct app_entry *a, const char *app_id);
static int app_index_by_app_id(const char *app_id);
static int windows_of_app(const struct app_entry *a, int *out, int max);
static void notif_dbus_init(void);
static void notif_shutdown(void);
static void center_taskbar_items(void);

static void
toplevel_recalc_layout(void)
{
    g_box.n_taskbar_groups = 0;
    if (!g_bar.cr || !g_bar.configured) return;

    pinned_recalc_layout();

    double x_start = 10 + MENU_PILL_W + PINNED_GAP;
    for (int i = 0; i < g_box.n_pl; i++) {
        double end = g_box.pl[i].x + g_box.pl[i].w;
        if (end > x_start) x_start = end;
    }

    double px_max_x = g_bar.width - 10;

    for (int i = 0; i < g_n_toplevels; i++) {
        const char *app_id = g_toplevels[i].app_id;
        if (!app_id) continue;

        int is_pinned = 0;
        for (int p = 0; p < g_app.n_pinned; p++) {
            if (g_app.pinned[p] < g_app.n_apps &&
                app_matches_id(&g_app.apps[g_app.pinned[p]], app_id)) {
                is_pinned = 1; break;
            }
        }
        if (is_pinned) continue;

        int gidx = -1;
        for (int j = 0; j < g_box.n_taskbar_groups; j++) {
            int first = g_box.taskbar_groups[j].windows[0];
            if (g_toplevels[first].app_id &&
                app_match(g_toplevels[first].app_id, app_id)) {
                gidx = j;
                break;
            }
        }

        if (gidx >= 0) {
            struct taskbar_group *g = &g_box.taskbar_groups[gidx];
            if (g->n_windows < MAX_WINDOWS_PER_GROUP) {
                g->windows[g->n_windows] = i;
                g->n_windows++;
            }
        } else {
            if (g_box.n_taskbar_groups >= MAX_TASKBAR_GROUPS) break;
            double item_w = TASKBAR_ICON + TASKBAR_GAP;
            if (x_start + item_w > px_max_x) break;

            struct taskbar_group *g = &g_box.taskbar_groups[g_box.n_taskbar_groups];
            g->x = x_start;
            g->w = item_w;
            g->n_windows = 1;
            g->windows[0] = i;
            g_box.n_taskbar_groups++;
            x_start += item_w;
        }
    }

    center_taskbar_items();

    if (trace_on()) {
        fprintf(stderr, "[trace] recalc w=%d n_pl=%d n_grp=%d |", g_bar.width,
                g_box.n_pl, g_box.n_taskbar_groups);
        for (int i = 0; i < g_box.n_pl && i < 8; i++)
            fprintf(stderr, " P%d=%.0f/%.0f", i, g_box.pl[i].x, g_box.pl[i].w);
        for (int i = 0; i < g_box.n_taskbar_groups && i < 8; i++)
            fprintf(stderr, " G%d=%.0f/%.0f", i, g_box.taskbar_groups[i].x,
                    g_box.taskbar_groups[i].w);
        fputc('\n', stderr);
    }

    if (g_gpopup.open) {
        for (int i = 0; i < g_gpopup.n_windows; i++) {
            if (g_gpopup.windows[i] >= g_n_toplevels) { gpopup_close(); break; }
        }
    }
    if (g_cpop.open && g_cpop.window_idx >= g_n_toplevels)
        cpop_close();
}

static double right_cluster_limit(void);

static void
center_taskbar_items(void)
{
    if (g_box.n_pl == 0 && g_box.n_taskbar_groups == 0) return;

    double avail_center = g_bar.width / 2.0;

    double lmost = 1e9, rmost = -1e9;
    for (int i = 0; i < g_box.n_pl; i++) {
        double x = g_box.pl[i].x, r = x + g_box.pl[i].w;
        if (x < lmost) lmost = x;
        if (r > rmost) rmost = r;
    }
    for (int i = 0; i < g_box.n_taskbar_groups; i++) {
        double x = g_box.taskbar_groups[i].x, r = x + g_box.taskbar_groups[i].w;
        if (x < lmost) lmost = x;
        if (r > rmost) rmost = r;
    }

    double block_center = (lmost + rmost) / 2.0;
    double shift = avail_center - block_center;

    {
        double min_x = 10 + MENU_PILL_W + 4;
        if (lmost + shift < min_x) shift = min_x - lmost;
    }
    {
        double max_r = right_cluster_limit();
        if (rmost + shift > max_r) shift = max_r - rmost;
    }

    for (int i = 0; i < g_box.n_pl; i++)
        g_box.pl[i].x += shift;
    for (int i = 0; i < g_box.n_taskbar_groups; i++)
        g_box.taskbar_groups[i].x += shift;

    if (trace_on())
        fprintf(stderr,
                "[trace] center shift=%.1f lmost=%.1f rmost=%.1f max_r=%.1f\n",
                shift, lmost, rmost, right_cluster_limit());
}

static void bar_draw(struct bar *b);
static void bar_render(struct bar *b);
static const struct wl_callback_listener frame_listener;
static void bar_update_size(struct bar *b);
static void
popup_size_changed(void)
{
    if (g_bar.configured)
        bar_update_size(&g_bar);
}
static void menu_close(struct bar *b);
static void menu_toggle_ctl(void);
static void ctx_close(void);
static int menu_hit_test(double px, double py);
static int pinned_running(int pinned_idx);
static void sys_tick(void);
static int sys_column_height(void);
static double menu_vert_pad(double mh_body);

static void
desktop_file_trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' '))
        s[--len] = '\0';
}

static void
app_add(const char *name, const char *exec, const char *icon, const char *desktop_id,
        const char *wm_class, int terminal)
{
    if (!name || !name[0] || !exec || !exec[0]) return;
    if (desktop_id && desktop_id[0]) {
        for (int i = 0; i < g_app.n_apps; i++) {
            if (g_app.apps[i].desktop_id &&
                strcmp(g_app.apps[i].desktop_id, desktop_id) == 0)
                return;
        }
    }
    if (g_app.n_apps >= g_app.apps_cap) {
        int new_cap = g_app.apps_cap ? g_app.apps_cap * 2 : 64;
        struct app_entry *new_apps = realloc(g_app.apps, new_cap * sizeof(*g_app.apps));
        if (!new_apps) return;
        g_app.apps = new_apps;
        g_app.apps_cap = new_cap;
    }
    g_app.apps[g_app.n_apps].name = strdup(name);
    g_app.apps[g_app.n_apps].exec = strdup(exec);
    g_app.apps[g_app.n_apps].icon_name = (icon && icon[0]) ? strdup(icon) : NULL;
    g_app.apps[g_app.n_apps].desktop_id = (desktop_id && desktop_id[0]) ? strdup(desktop_id) : NULL;
    g_app.apps[g_app.n_apps].wm_class = (wm_class && wm_class[0]) ? strdup(wm_class) : NULL;
    g_app.apps[g_app.n_apps].icon = NULL;
    g_app.apps[g_app.n_apps].terminal = terminal;
    g_app.n_apps++;
}

static int
app_compare(const void *a, const void *b)
{
    return strcasecmp(((const struct app_entry *)a)->name,
                      ((const struct app_entry *)b)->name);
}

static cairo_surface_t *
icon_load_svg(const char *path, int target_size)
{
    GError *err = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_file(path, &err);
    if (!handle) { g_error_free(err); return NULL; }

    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                     target_size, target_size);
    cairo_t *cr = cairo_create(sf);
    rsvg_handle_render_document(handle, cr,
        &(RsvgRectangle){0, 0, target_size, target_size},
        &err);
    cairo_destroy(cr);
    g_object_unref(handle);
    if (err) { g_error_free(err); cairo_surface_destroy(sf); return NULL; }
    return sf;
}

static cairo_surface_t *
png_load(const char *path)
{
    cairo_surface_t *sf = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS)
        return sf;
    cairo_surface_destroy(sf);
    return NULL;
}

static char *
icon_theme_name(void)
{
    static char name[128] = {0};
    if (name[0]) return name;

    const char *config_home = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char paths[3][512];
    int n = 0;
    if (config_home && config_home[0]) {
        snprintf(paths[n++], sizeof(paths[0]), "%s/gtk-3.0/settings.ini",
                 config_home);
        snprintf(paths[n++], sizeof(paths[0]), "%s/gtk-4.0/settings.ini",
                 config_home);
    } else if (home) {
        snprintf(paths[n++], sizeof(paths[0]),
                 "%s/.config/gtk-3.0/settings.ini", home);
        snprintf(paths[n++], sizeof(paths[0]),
                 "%s/.config/gtk-4.0/settings.ini", home);
    }
    if (home)
        snprintf(paths[n++], sizeof(paths[0]), "%s/.gtkrc-2.0", home);

    for (int i = 0; i < n && !name[0]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            size_t kl = (size_t)(eq - line);
            while (kl > 0 && (line[kl-1] == ' ' || line[kl-1] == '\t')) kl--;
            if (kl == sizeof("gtk-icon-theme-name") - 1 &&
                strncmp(line, "gtk-icon-theme-name", kl) == 0) {
                char *val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                size_t vl = strlen(val);
                while (vl > 0 && (val[vl-1] == '\n' || val[vl-1] == '\r' ||
                                  val[vl-1] == ' ' || val[vl-1] == '\t' ||
                                  val[vl-1] == '"'))
                    val[--vl] = '\0';
                while (*val == '"') { memmove(val, val + 1, vl--); }
                if (vl > 0 && vl < sizeof(name))
                    memcpy(name, val, vl + 1);
                break;
            }
        }
        fclose(f);
    }
    return name[0] ? name : NULL;
}

static cairo_surface_t *
icon_fallback(void)
{
    static cairo_surface_t *s;
    if (s) return s;
    s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_arc(cr, 32, 32, 22, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_destroy(cr);
    return s;
}

struct icon_theme_dir {
    char name[64];
    int size;
    int scale;
    int type;
    int min_size, max_size;
};

static struct icon_theme_dir *
icon_theme_parse(const char *theme_dir, char (*inherits)[64], int *n_inherits,
                 int *n_out)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/index.theme", theme_dir);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char *line = NULL;
    size_t cap = 0;
    int n = 0;
    int in_main = 1;
    int cur = -1;
    *n_inherits = 0;
    struct icon_theme_dir *dirs = NULL;
    while (getline(&line, &cap, f) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p || *p == '\n' || *p == '\r') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) continue;
            *end = '\0';
            if (strcmp(p + 1, "Icon Theme") == 0) {
                in_main = 1;
                cur = -1;
            } else {
                in_main = 0;
                cur = -1;
                for (int i = 0; i < n; i++)
                    if (strcmp(dirs[i].name, p + 1) == 0) { cur = i; break; }
            }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p, *val = eq + 1;
        char *ke = key + strlen(key);
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        while (*val == ' ' || *val == '\t') val++;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t' ||
                            ve[-1] == '\n' || ve[-1] == '\r')) *--ve = '\0';
        char *qf = val;
        while (*qf == '"' || *qf == ' ') qf++;
        char *qe = val + strlen(val);
        while (qe > qf && (qe[-1] == '"' || qe[-1] == ' ' || qe[-1] == '\t')) qe--;
        *qe = '\0';
        if (qf != val) memmove(val, qf, (size_t)(qe - qf + 1));

        if (in_main) {
            if (strcmp(key, "Directories") == 0) {
                char *tk = val;
                while (tk && *tk) {
                    char *comma = strchr(tk, ',');
                    if (comma) *comma = '\0';
                    char *t = tk;
                    while (*t == ' ' || *t == '\t') t++;
                    char *te = t + strlen(t);
                    while (te > t && (te[-1] == ' ' || te[-1] == '\t')) *--te = '\0';
                    if (*t) {
                        struct icon_theme_dir *nd =
                            realloc(dirs, (size_t)(n + 1) * sizeof(*nd));
                        if (!nd) break;
                        dirs = nd;
                        struct icon_theme_dir *d = &dirs[n++];
                        memset(d, 0, sizeof(*d));
                        d->scale = 1;
                        snprintf(d->name, sizeof(d->name), "%s", t);
                    }
                    tk = comma ? comma + 1 : NULL;
                }
            } else if (strcmp(key, "Inherits") == 0) {
                char *tk = val;
                while (tk && *tk && *n_inherits < 4) {
                    char *comma = strchr(tk, ',');
                    if (comma) *comma = '\0';
                    char *t = tk;
                    while (*t == ' ' || *t == '\t') t++;
                    char *te = t + strlen(t);
                    while (te > t && (te[-1] == ' ' || te[-1] == '\t')) *--te = '\0';
                    if (*t)
                        snprintf(inherits[(*n_inherits)++], 64, "%s", t);
                    tk = comma ? comma + 1 : NULL;
                }
            }
            continue;
        }

        if (cur < 0) continue;
        struct icon_theme_dir *d = &dirs[cur];
        if (strcmp(key, "Size") == 0) d->size = atoi(val);
        else if (strcmp(key, "Scale") == 0) d->scale = atoi(val);
        else if (strcmp(key, "Type") == 0) {
            if (strcmp(val, "Scalable") == 0) d->type = 2;
            else if (strcmp(val, "Threshold") == 0) d->type = 1;
            else d->type = 0;
        } else if (strcmp(key, "MinSize") == 0) d->min_size = atoi(val);
        else if (strcmp(key, "MaxSize") == 0) d->max_size = atoi(val);
    }
    free(line);
    fclose(f);
    *n_out = n;
    return dirs;
}

static cairo_surface_t *
icon_theme_search(const char *theme_dir, const struct icon_theme_dir *dirs,
                  int n, const char *base, int target)
{
    for (int prio = 4; prio >= 1; prio--) {
        for (int i = 0; i < n; i++) {
            const struct icon_theme_dir *d = &dirs[i];
            int eff = d->size * (d->scale ? d->scale : 1);
            int p = 0;
            if (d->type == 2) {
                p = 4;
            } else if (d->type == 1) {
                int lo = (d->min_size ? d->min_size : d->size) * (d->scale ? d->scale : 1);
                int hi = (d->max_size ? d->max_size : d->size) * (d->scale ? d->scale : 1);
                if (target >= lo && target <= hi) p = 3;
                else if (eff > target) p = 2;
                else p = 1;
            } else {
                if (eff == target) p = 3;
                else if (eff > target) p = 2;
                else p = 1;
            }
            if (p != prio) continue;

            char path[1024];
            const char *ctx_alt[2][2] = { { d->name, "" }, { NULL, NULL } };
            const char *slash = strchr(d->name, '/');
            char part1[64], part2[64];
            if (slash && slash[1]) {
                size_t l1 = (size_t)(slash - d->name);
                snprintf(part1, sizeof(part1), "%.*s", (int)(l1 > 63 ? 63 : l1), d->name);
                snprintf(part2, sizeof(part2), "%s", slash + 1);
                ctx_alt[1][0] = part2;
                ctx_alt[1][1] = part1;
            }

            for (int v = 0; v < 2; v++) {
                if (!ctx_alt[v][0]) break;
                for (int e = 0; e < 2; e++) {
                    snprintf(path, sizeof(path), "%.400s/%.40s/%.40s/%.200s.%s",
                             theme_dir, ctx_alt[v][0], ctx_alt[v][1], base,
                             e ? "svg" : "png");
                    if (e) {
                        cairo_surface_t *sf = icon_load_svg(path, target);
                        if (sf) return sf;
                    } else {
                        cairo_surface_t *sf = png_load(path);
                        if (sf) return sf;
                    }
                }
            }
        }
    }
    return NULL;
}

static cairo_surface_t *
icon_theme_lookup(const char *base, int target, char (*roots)[512], int n_roots)
{
    char chain[8][64];
    int n_chain = 0;
    const char *cfg = icon_theme_name();
    const char *ordered[] = { ICON_THEME, cfg, "hicolor", "default", "plasma",
                              NULL };
    for (int i = 0; ordered[i] && n_chain < 8; i++) {
        if (!ordered[i] || !ordered[i][0]) continue;
        int dup = 0;
        for (int j = 0; j < n_chain; j++)
            if (strcmp(chain[j], ordered[i]) == 0) dup = 1;
        if (!dup) snprintf(chain[n_chain++], sizeof(chain[0]), "%s", ordered[i]);
    }

    for (int ci = 0; ci < n_chain && n_chain < 8; ci++) {
        for (int r = 0; r < n_roots; r++) {
            char dir[600];
            snprintf(dir, sizeof(dir), "%.480s/icons/%.80s", roots[r], chain[ci]);
            char inh[4][64];
            int n_inh = 0, nd = 0;
            struct icon_theme_dir *dirs =
                icon_theme_parse(dir, inh, &n_inh, &nd);
            if (!dirs) continue;
            cairo_surface_t *sf = icon_theme_search(dir, dirs, nd, base, target);
            free(dirs);
            if (sf) return sf;
            for (int k = 0; k < n_inh && n_chain < 8; k++) {
                int dup = 0;
                for (int j = 0; j < n_chain; j++)
                    if (strcmp(chain[j], inh[k]) == 0) dup = 1;
                if (!dup) snprintf(chain[n_chain++], sizeof(chain[0]), "%s", inh[k]);
            }
        }
    }
    return NULL;
}

static cairo_surface_t *
icon_load(const char *icon_name)
{
    if (!icon_name || !icon_name[0]) return NULL;

    char pathbuf[512];
    if (icon_name[0] == '$') {
        const char *home = getenv("HOME");
        if (home && strncmp(icon_name, "$HOME", 5) == 0) {
            snprintf(pathbuf, sizeof(pathbuf), "%s%s", home, icon_name + 5);
            icon_name = pathbuf;
        }
    }

    if (icon_name[0] == '/') {
        cairo_surface_t *s = png_load(icon_name);
        if (s) return s;
        s = icon_load_svg(icon_name, 64);
        if (s) return s;
        return cairo_surface_reference(icon_fallback());
    }

    char base[256];
    strncpy(base, icon_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".png") == 0)
        base[blen - 4] = '\0';

    const char *sizes[] = { "256x256", "128x128", "96x96", "64x64", "48x48",
                            "32x32", "24x24", "22x22", "16x16", "512x512",
                            NULL };
    const char *contexts[] = { "apps", "status", "devices", "actions",
                               "places", "preferences", "categories",
                               "emblems", "mimetypes", NULL };

    char themes[4][128] = {{0}};
    int n_themes = 0;
    const char *cfg = icon_theme_name();
    const char *fallbacks[] = { "hicolor", "default", "plasma", NULL };
    if (ICON_THEME[0])
        snprintf(themes[n_themes++], sizeof(themes[0]), "%s", ICON_THEME);
    if (cfg && cfg[0]) {
        int dup = 0;
        for (int i = 0; i < n_themes; i++)
            if (strcmp(themes[i], cfg) == 0) dup = 1;
        if (!dup && n_themes < 4)
            snprintf(themes[n_themes++], sizeof(themes[0]), "%s", cfg);
    }
    for (int i = 0; fallbacks[i] && n_themes < 4; i++) {
        int dup = 0;
        for (int j = 0; j < n_themes; j++)
            if (strcmp(themes[j], fallbacks[i]) == 0) dup = 1;
        if (!dup)
            snprintf(themes[n_themes++], sizeof(themes[0]), "%s", fallbacks[i]);
    }

    char roots[16][512];
    int n_roots = 0;
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *data_dirs = getenv("XDG_DATA_DIRS");
    const char *home = getenv("HOME");

    if (data_home && data_home[0])
        snprintf(roots[n_roots++], sizeof(roots[0]), "%s", data_home);
    else if (home)
        snprintf(roots[n_roots++], sizeof(roots[0]), "%s/.local/share", home);

    if (!data_dirs || !data_dirs[0]) data_dirs = "/usr/local/share:/usr/share";
    char buf[4096];
    strncpy(buf, data_dirs, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *tok = buf; tok && tok[0] && n_roots < 12; ) {
        char *next = strchr(tok, ':');
        if (next) *next++ = '\0';
        snprintf(roots[n_roots], sizeof(roots[0]), "%.450s", tok);
        n_roots++;
        tok = next;
    }

    if (n_roots < 16)
        snprintf(roots[n_roots++], sizeof(roots[0]),
                 "/var/lib/flatpak/exports/share");
    if (n_roots < 16)
        snprintf(roots[n_roots++], sizeof(roots[0]),
                 "/usr/local/share/flatpak/exports/share");

    {
        cairo_surface_t *sf = icon_theme_lookup(base, 20, roots, n_roots);
        if (sf) return sf;
    }

    for (int r = 0; r < n_roots; r++) {
        for (int t = 0; t < n_themes; t++) {
            char dir[600];
            snprintf(dir, sizeof(dir), "%.480s/icons/%.80s", roots[r],
                     themes[t]);
            for (int c = 0; contexts[c]; c++) {
                for (int s = 0; sizes[s]; s++) {
                    char path[1024];
                    snprintf(path, sizeof(path), "%.480s/%.12s/%.20s/%.200s.png",
                             dir, sizes[s], contexts[c], base);
                    cairo_surface_t *sf = png_load(path);
                    if (sf) return sf;
                    snprintf(path, sizeof(path), "%.480s/%.20s/%.12s/%.200s.png",
                             dir, contexts[c], sizes[s], base);
                    sf = png_load(path);
                    if (sf) return sf;
                }
                {
                    char path[1024];
                    snprintf(path, sizeof(path), "%.480s/scalable/%.20s/%.200s.svg",
                             dir, contexts[c], base);
                    cairo_surface_t *sf = icon_load_svg(path, 64);
                    if (sf) return sf;
                    snprintf(path, sizeof(path), "%.480s/%.20s/scalable/%.200s.svg",
                             dir, contexts[c], base);
                    sf = icon_load_svg(path, 64);
                    if (sf) return sf;
                }
            }
        }
        for (int ext = 0; ext < 2; ext++) {
            char path[1024];
            snprintf(path, sizeof(path), "%.500s/pixmaps/%s%s", roots[r],
                     base, ext == 0 ? ".png" : ".svg");
            if (ext == 0) {
                cairo_surface_t *sf = png_load(path);
                if (sf) return sf;
            } else {
                cairo_surface_t *sf = icon_load_svg(path, 64);
                if (sf) return sf;
            }
        }
    }

    for (int ext = 0; ext < 2; ext++) {
        char path[512];
        snprintf(path, sizeof(path), "/usr/share/pixmaps/%s%s", base,
                 ext == 0 ? ".png" : "");
        cairo_surface_t *sf = png_load(path);
        if (sf) return sf;
    }

    return cairo_surface_reference(icon_fallback());
}

static void
apps_load_icons(void)
{
    for (int i = 0; i < g_app.n_apps; i++) {
        if (g_app.apps[i].icon) continue;
        g_app.apps[i].icon = icon_load(g_app.apps[i].icon_name);
#ifdef JTLAB_DEBUG
        fprintf(stderr, "[dbg] app %-30s icon_name=%-24s desktop_id=%s wm=%s icon_ok=%d\n",
                g_app.apps[i].name, g_app.apps[i].icon_name ? g_app.apps[i].icon_name : "(none)",
                g_app.apps[i].desktop_id ? g_app.apps[i].desktop_id : "(none)",
                g_app.apps[i].wm_class ? g_app.apps[i].wm_class : "(none)",
                g_app.apps[i].icon != icon_fallback());
#endif
    }
}

static int
apps_data_dirs(const char **dirs, int max)
{
    static char home_apps[512], home_flatpak[512];
    const char *home = getenv("HOME");
    int n = 0;
    if (home) {
        snprintf(home_apps, sizeof(home_apps),
                 "%s/.local/share/applications", home);
        dirs[n++] = home_apps;
    }
    if (n < max) dirs[n++] = "/usr/share/applications";
    if (n < max) dirs[n++] = "/usr/local/share/applications";
    if (home) {
        snprintf(home_flatpak, sizeof(home_flatpak),
                 "%s/.local/share/flatpak/exports/share/applications", home);
        if (n < max) dirs[n++] = home_flatpak;
    }
    if (n < max) dirs[n++] = "/var/lib/flatpak/exports/share/applications";
    if (n < max) dirs[n++] = "/usr/local/share/flatpak/exports/share/applications";
    return n;
}

static void menu_filter_update(void);
static void pinned_save(void);
static void cpop_close(void);

static void
pinned_drop(int p)
{
    memmove(&g_app.pinned[p], &g_app.pinned[p + 1], (g_app.n_pinned - p - 1) * sizeof(int));
    g_app.n_pinned--;
    pinned_save();
    pinned_recalc_layout();
    toplevel_recalc_layout();
    render_request();
}

static void
app_drop(int k)
{
    free(g_app.apps[k].name);
    free(g_app.apps[k].exec);
    free(g_app.apps[k].icon_name);
    free(g_app.apps[k].desktop_id);
    free(g_app.apps[k].wm_class);
    if (g_app.apps[k].icon) cairo_surface_destroy(g_app.apps[k].icon);
    memmove(&g_app.apps[k], &g_app.apps[k + 1], (g_app.n_apps - k - 1) * sizeof(*g_app.apps));
    g_app.n_apps--;

    int w = 0;
    int dropped = 0;
    for (int p = 0; p < g_app.n_pinned; p++) {
        if (g_app.pinned[p] == k) { dropped++; continue; }
        if (g_app.pinned[p] > k) g_app.pinned[p]--;
        g_app.pinned[w++] = g_app.pinned[p];
    }
    g_app.n_pinned = w;

    if (g_cpop.open) {
        if (g_cpop.app_idx == k)
            cpop_close();
        else if (g_cpop.app_idx > k)
            g_cpop.app_idx--;
    }
    if (g_ctx.open) {
        if (g_ctx.target_app == k)
            g_ctx.open = 0;
        else if (g_ctx.target_app > k)
            g_ctx.target_app--;
    }

    if (dropped) {
        pinned_save();
        pinned_recalc_layout();
    }
    toplevel_recalc_layout();
    render_request();
}

static void
apps_scan_load(void)
{
    const char *dirs[8];
    int ndirs = apps_data_dirs(dirs, 8);
    char (*seen)[256] = NULL;
    int n_seen = 0, seen_cap = 0;
    int seen_ok = 1;
    for (int d = 0; d < ndirs; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop") != 0)
                continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], ent->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;

            if (n_seen == seen_cap) {
                int nc = seen_cap ? seen_cap * 2 : 128;
                char (*ns)[256] = realloc(seen, (size_t)nc * sizeof(*ns));
                if (ns) {
                    seen = ns;
                    seen_cap = nc;
                } else {
                    seen_ok = 0;
                }
            }
            char desktop_id[256];
            snprintf(desktop_id, sizeof(desktop_id), "%.*s",
                     (int)(len - 8), ent->d_name);
            if (seen_ok && n_seen < seen_cap)
                snprintf(seen[n_seen++], sizeof(seen[0]), "%s", desktop_id);

            char line[256];
            char name[256] = {0};
            char exec_cmd[256] = {0};
            char icon_name[256] = {0};
            char wm_class[256] = {0};
            int terminal = 0;
            int in_desktop_entry = 0;

            while (fgets(line, sizeof(line), f)) {
                desktop_file_trim(line);
                if (line[0] == '[') {
                    in_desktop_entry = (strcmp(line, "[Desktop Entry]") == 0);
                    if (!in_desktop_entry) break;
                    continue;
                }
                if (!in_desktop_entry) continue;

                if (strncmp(line, "Name=", 5) == 0 && !name[0])
                    strncpy(name, line + 5, sizeof(name) - 1);
                else if (strncmp(line, "Exec=", 5) == 0 && !exec_cmd[0])
                    strncpy(exec_cmd, line + 5, sizeof(exec_cmd) - 1);
                else if (strncmp(line, "Icon=", 5) == 0 && !icon_name[0])
                    strncpy(icon_name, line + 5, sizeof(icon_name) - 1);
                else if (strncmp(line, "StartupWMClass=", 15) == 0 && !wm_class[0])
                    strncpy(wm_class, line + 15, sizeof(wm_class) - 1);
                else if (strncmp(line, "NoDisplay=", 10) == 0) {
                    if (strcmp(line + 10, "true") == 0) goto next;
                }
                else if (strncmp(line, "Hidden=", 7) == 0) {
                    if (strcmp(line + 7, "true") == 0) goto next;
                }
                else if (strncmp(line, "Terminal=", 9) == 0) {
                    if (strcmp(line + 9, "true") == 0) terminal = 1;
                }
            }
            if (name[0] && exec_cmd[0])
                app_add(name, exec_cmd, icon_name, desktop_id, wm_class, terminal);
    next:
            fclose(f);
        }
        closedir(dir);
    }

    if (seen_ok) {
        for (int i = 0; i < g_app.n_apps; i++) {
            if (!g_app.apps[i].desktop_id) continue;
            int exists = 0;
            for (int k = 0; k < n_seen && !exists; k++) {
                if (strcmp(seen[k], g_app.apps[i].desktop_id) == 0)
                    exists = 1;
            }
            if (exists) continue;
            app_drop(i);
            i--;
        }
    }
    free(seen);
}

static void
apps_scan(void)
{
    apps_scan_load();
    qsort(g_app.apps, g_app.n_apps, sizeof(*g_app.apps), app_compare);
}

static void
apps_watch_init(void)
{
    const char *dirs[8];
    int ndirs = apps_data_dirs(dirs, 8);
    g_app.apps_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_app.apps_fd < 0) return;
    for (int d = 0; d < ndirs; d++)
        inotify_add_watch(g_app.apps_fd, dirs[d],
                          IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO |
                          IN_DELETE | IN_MOVED_FROM);
}

static void
apps_refresh(void)
{
    apps_load_icons();
    menu_filter_update();
    render_request();
}

static void
apps_watch_handle(void)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(g_app.apps_fd, buf, sizeof(buf))) > 0)
        ;
    apps_scan_load();
    apps_refresh();
}


static void
apps_free(void)
{
    for (int i = 0; i < g_app.n_apps; i++) {
        free(g_app.apps[i].name);
        free(g_app.apps[i].exec);
        free(g_app.apps[i].icon_name);
        free(g_app.apps[i].desktop_id);
        free(g_app.apps[i].wm_class);
        if (g_app.apps[i].icon) cairo_surface_destroy(g_app.apps[i].icon);
    }
    free(g_app.apps);
    free(g_app.menu_idx);
    g_app.menu_idx = NULL;
    g_app.menu_idx_cap = 0;
    g_app.menu_n = 0;
    if (g_wl.xkb_state) { xkb_state_unref(g_wl.xkb_state); g_wl.xkb_state = NULL; }
    if (g_wl.xkb_keymap) { xkb_keymap_unref(g_wl.xkb_keymap); g_wl.xkb_keymap = NULL; }
    if (g_wl.xkb_ctx) { xkb_context_unref(g_wl.xkb_ctx); g_wl.xkb_ctx = NULL; }
}

static int bar_y_offset(void);
static cairo_surface_t *surface_from_pixbuf(GdkPixbuf *pb);
static void panel_begin(cairo_t *cr, double x, double y, double w, double h,
                        double r, double sw, double sa, int recurse);

static int
in_rect(double px, double py, double x, double y, double w, double h)
{
    return px >= x && px <= x + w && py >= y && py <= y + h;
}

static int
bar_icon_hit_test(double px, double py, double x, double w, double h)
{
    int bar_y = bar_y_offset();
    double cy = bar_y + (BAR_HEIGHT - h) / 2.0;
    return in_rect(px, py, x, cy, w, h);
}

#define TRAY_MAX_ITEMS 16

static struct {
    GDBusConnection *dbus_conn;
    GDBusNodeInfo *node;
    guint obj_reg;
    guint owner;
    int n_tray;
    uint32_t tray_gen;
    GMainContext *glib_ctx;
    int main_epoll_fd;
    int wl_fd;
    int clock_fd;
    SoupSession *art_session;
    GSocketService *ctl_service;
    GDBusNodeInfo *notif_node;
    guint notif_obj_reg;
    guint notif_owner;
} g_sni = {
    .main_epoll_fd = -1,
    .wl_fd = -1,
    .clock_fd = -1,
};

/* Generic exponential-approach scroll animation, shared by the menu, wallpaper
 * and notification popups. Holds pointers into the owner's state so the tick
 * drives that popup's own scroll/current value. */
struct scroll_anim {
    double *current;        /* value currently displayed */
    double *target;         /* value animating toward */
    GSource **src;          /* active timeout source (NULL when idle) */
    void (*on_frame)(void); /* called after each value update (if set) */
    int (*wants_render)(void); /* if set, render only while it returns 1 */
};

static gboolean
scroll_anim_tick(void *data)
{
    struct scroll_anim *a = data;
    double diff = *a->target - *a->current;
    if (fabs(diff) < 0.5) {
        *a->current = *a->target;
        *a->src = NULL;
        if (a->on_frame) a->on_frame();
        if (!a->wants_render || a->wants_render())
            render_request();
        return G_SOURCE_REMOVE;
    }
    *a->current += diff * 0.2;
    if (a->on_frame) a->on_frame();
    if (!a->wants_render || a->wants_render())
        render_request();
    return G_SOURCE_CONTINUE;
}

static void
scroll_anim_start(struct scroll_anim *a)
{
    if (*a->src) return;
    GSource *src = g_timeout_source_new(16);
    g_source_set_callback(src, scroll_anim_tick, a, NULL);
    g_source_attach(src, g_sni.glib_ctx);
    *a->src = src;
    g_source_unref(src);
}

static void
scroll_anim_cancel(struct scroll_anim *a)
{
    if (*a->src) {
        g_source_destroy(*a->src);
        *a->src = NULL;
    }
}

struct tray_item {
    char id[512];
    char bus[256];
    char path[256];
    GDBusProxy *proxy;
    guint watch_id;
    guint signal_id;
    char *icon_name;
    char *icon_theme_path;
    char *menu_path;
    GVariant *pixmap;
    cairo_surface_t *icon;
    int is_menu;
    gboolean valid;
    uint32_t gen;
};

static struct tray_item g_tray[TRAY_MAX_ITEMS];

struct tray_ref {
    uint32_t gen;
};

static struct tray_item *
tray_find_by_gen(uint32_t gen)
{
    for (int i = 0; i < g_sni.n_tray; i++)
        if (g_tray[i].valid && g_tray[i].gen == gen)
            return &g_tray[i];
    return NULL;
}

static double
tray_width(void)
{
    if (g_sni.n_tray == 0) return 0;
    return g_sni.n_tray * TRAY_ICON_SIZE + (g_sni.n_tray - 1) * TRAY_SPACING;
}

static double
tray_icon_x(int i)
{
    double right = audio_icon_x() - TRAY_CLUSTER_GAP;
    return right - tray_width() + i * (TRAY_ICON_SIZE + TRAY_SPACING);
}

static double
right_cluster_limit(void)
{
    double left;
    int gap = TRAY_GAP;
    if (g_sni.n_tray > 0) { left = tray_icon_x(0); gap = TRAY_CLUSTER_GAP; }
    else if (g_box.audio_present) left = audio_icon_x();
    else if (g_box.net_status != NET_NONE) left = net_icon_x();
    else left = g_bar.width - TRAY_EDGE_PAD - NOTIF_ICON_SIZE
                    - TRAY_GAP - time_text_width();
    return left - gap - WS_ICON_SIZE - 4;
}

static double
workspace_icon_x(void)
{
    return right_cluster_limit() + 4;
}

static int
workspace_icon_hit_test(double px, double py)
{
    return bar_icon_hit_test(px, py, workspace_icon_x(), WS_ICON_SIZE, WS_ICON_SIZE);
}

static int
tray_hit_test(double px, double py)
{
    if (g_sni.n_tray == 0) return -1;
    for (int i = 0; i < g_sni.n_tray; i++) {
        if (bar_icon_hit_test(px, py, tray_icon_x(i), TRAY_ICON_SIZE, TRAY_ICON_SIZE))
            return i;
    }
    return -1;
}

static cairo_surface_t *
tray_surface_from_pixmap(GVariant *pixmap)
{
    gint32 best_w = 0, best_h = 0;
    const guint8 *best_data = NULL;
    int best_score = 1 << 30;

    GVariantIter iter;
    g_variant_iter_init(&iter, pixmap);
    GVariant *tuple;
    while ((tuple = g_variant_iter_next_value(&iter)) != NULL) {
        GVariant *wv = g_variant_get_child_value(tuple, 0);
        GVariant *hv = g_variant_get_child_value(tuple, 1);
        GVariant *dv = g_variant_get_child_value(tuple, 2);
        gint32 w = g_variant_get_int32(wv);
        gint32 h = g_variant_get_int32(hv);
        gsize len = g_variant_get_size(dv);
        if (w > 0 && h > 0 && len >= (gsize)w * (gsize)h * 4) {
            int score = (w >= TRAY_ICON_SIZE && h >= TRAY_ICON_SIZE)
                ? (w - TRAY_ICON_SIZE) + (h - TRAY_ICON_SIZE)
                : (TRAY_ICON_SIZE - w) + (TRAY_ICON_SIZE - h) + 10000;
            if (score < best_score) {
                best_score = score;
                best_w = w;
                best_h = h;
                best_data = g_variant_get_data(dv);
            }
        }
        g_variant_unref(wv);
        g_variant_unref(hv);
        g_variant_unref(dv);
        g_variant_unref(tuple);
    }
    if (!best_data || best_w <= 0 || best_h <= 0) return NULL;

    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                     best_w, best_h);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    unsigned char *out = cairo_image_surface_get_data(sf);
    int stride = cairo_image_surface_get_stride(sf);
    const unsigned char *src = best_data;
    for (int y = 0; y < best_h; y++) {
        memcpy(out + (size_t)y * stride, src, (size_t)best_w * 4);
        src += (size_t)best_w * 4;
    }
    cairo_surface_mark_dirty(sf);
    return sf;
}

static void tray_item_fetch_props(struct tray_item *it);
static void tray_item_destroy(struct tray_item *it);

static void
tray_item_reload_icon(struct tray_item *it)
{
    if (it->icon) { cairo_surface_destroy(it->icon); it->icon = NULL; }
    if (it->pixmap) {
        it->icon = tray_surface_from_pixmap(it->pixmap);
        if (it->icon) { render_request(); return; }
    }
    if (it->icon_name && it->icon_name[0]) {
        if (it->icon_theme_path && it->icon_theme_path[0]) {
            char p[1024];
            snprintf(p, sizeof(p), "%.480s/%s.png", it->icon_theme_path,
                     it->icon_name);
            cairo_surface_t *s = png_load(p);
            if (s) {
                it->icon = s;
                render_request();
                return;
            }
            snprintf(p, sizeof(p), "%.480s/%s.svg", it->icon_theme_path,
                     it->icon_name);
            s = icon_load_svg(p, 64);
            if (s) {
                it->icon = s;
                render_request();
                return;
            }
        }
        it->icon = icon_load(it->icon_name);
        if (it->icon) { render_request(); return; }
    }
    render_request();
}

static void
tray_item_signal(GDBusConnection *connection, const char *sender_name,
                 const char *object_path, const char *interface_name,
                 const char *signal_name, GVariant *parameters,
                 gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path;
    (void)interface_name; (void)parameters;
    struct tray_ref *ref = user_data;
    if (!ref) return;
    struct tray_item *it = tray_find_by_gen(ref->gen);
    if (!it) return;
    if (strcmp(signal_name, "NewIcon") == 0)
        tray_item_fetch_props(it);
    render_request();
}

static void
tray_call_finish(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    GError *err = NULL;
    GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &err);
    if (err) g_clear_error(&err);
    if (v) g_variant_unref(v);
}

static void
tray_item_call(struct tray_item *it, const char *method, int x, int y)
{
    if (!it || !it->proxy) return;
    g_dbus_proxy_call(it->proxy, method, g_variant_new("(ii)", x, y),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      tray_call_finish, NULL);
    g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
}


#define TRAY_MENU_MAX       256
#define TRAY_MENU_ROW_ICON  20
#define TRAY_MENU_SEP_H     7
#define TRAY_MENU_MAX_W     360

struct tray_menu_entry {
    gint32 id;
    int depth;
    char *label;
    int enabled;
    int is_separator;
    int is_check;
    int is_radio;
    int toggle_state;
};

struct tray_menu_row {
    int e_idx;
    double y0, y1;
};

struct tray_menu {
    int open;
    int tray_idx;
    struct tray_menu_entry e[TRAY_MENU_MAX];
    int n;
    struct tray_menu_row row[TRAY_MENU_MAX];
    int n_row;
    double x, y;
    double w;
    int height;
    int hover_row;
    guint signal_id;
    char bus[256];
    char path[256];
};

static struct tray_menu g_tm = { 0 };

static void tray_menu_fetch(void);
static void tray_menu_close(void);

static void
tray_menu_free_entries(void)
{
    for (int i = 0; i < g_tm.n; i++) {
        g_free(g_tm.e[i].label);
    }
    g_tm.n = 0;
    g_tm.n_row = 0;
}

static void
tray_menu_layout(void)
{
    g_tm.n_row = 0;
    double y = MENU_PADDING;
    double w = 2 * MENU_PADDING + TRAY_MENU_ROW_ICON + 8 + 24;
    int any_toggle = 0;

    for (int i = 0; i < g_tm.n && g_tm.n_row < TRAY_MENU_MAX; i++) {
        struct tray_menu_entry *e = &g_tm.e[i];
        g_tm.row[g_tm.n_row].e_idx = i;
        g_tm.row[g_tm.n_row].y0 = y;
        if (e->is_separator) {
            g_tm.row[g_tm.n_row].y1 = y + TRAY_MENU_SEP_H;
            y += TRAY_MENU_SEP_H;
        } else {
            g_tm.row[g_tm.n_row].y1 = y + MENU_ROW_HEIGHT;
            y += MENU_ROW_HEIGHT;
            if (e->label && e->label[0]) {
                double need = 2 * MENU_PADDING + TRAY_MENU_ROW_ICON + 8 +
                              e->depth * 10 + text_width_px(e->label);
                if (need > w) w = need;
            }
            if (e->is_check || e->is_radio) any_toggle = 1;
        }
        g_tm.n_row++;
    }
    y += MENU_PADDING;

    if (!any_toggle) w -= 26;
    if (w > TRAY_MENU_MAX_W) w = TRAY_MENU_MAX_W;
    if (w < 120) w = 120;
    g_tm.w = w;
    g_tm.height = (int)y + MENU_FLOAT_GAP;

    double ix = 0;
    if (g_tm.tray_idx >= 0 && g_tm.tray_idx < g_sni.n_tray)
        ix = tray_icon_x(g_tm.tray_idx) + TRAY_ICON_SIZE / 2.0;
    double x = ix - w / 2.0;
    if (x < 8) x = 8;
    if (x + w > g_bar.width - 8) x = g_bar.width - 8 - w;
    g_tm.x = x;
    g_tm.y = 0;
}

static void tray_menu_parse_one(GVariant *tuple, gint32 parent_id, int depth);

static void
tray_menu_parse_av(GVariant *av, gint32 parent_id, int depth)
{
    GVariantIter it;
    g_variant_iter_init(&it, av);
    GVariant *sv;
    while ((sv = g_variant_iter_next_value(&it)) != NULL) {
        GVariant *child = g_variant_get_variant(sv);
        g_variant_unref(sv);
        tray_menu_parse_one(child, parent_id, depth);
        g_variant_unref(child);
        if (g_tm.n >= TRAY_MENU_MAX) break;
    }
}

static void
tray_menu_parse_one(GVariant *tuple, gint32 parent_id, int depth)
{
    if (g_tm.n >= TRAY_MENU_MAX) return;
    GVariant *props = NULL, *sub = NULL;
    gint32 id;
    if (!g_variant_is_of_type(tuple, G_VARIANT_TYPE("(ia{sv}av)"))) return;
    g_variant_get(tuple, "(i@a{sv}@av)", &id, &props, &sub);

    struct tray_menu_entry *e = &g_tm.e[g_tm.n++];
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->depth = depth;
    e->enabled = 1;
    e->toggle_state = -1;

    GVariant *v;
    v = g_variant_lookup_value(props, "label", G_VARIANT_TYPE_STRING);
    if (v) {
        e->label = g_strdup(g_variant_get_string(v, NULL));
        g_variant_unref(v);
    }
    v = g_variant_lookup_value(props, "enabled", G_VARIANT_TYPE_BOOLEAN);
    if (v) {
        e->enabled = g_variant_get_boolean(v);
        g_variant_unref(v);
    }
    v = g_variant_lookup_value(props, "type", G_VARIANT_TYPE_STRING);
    if (v) {
        const char *t = g_variant_get_string(v, NULL);
        e->is_separator = (strcmp(t, "separator") == 0);
        g_variant_unref(v);
    }
    v = g_variant_lookup_value(props, "toggle-type", G_VARIANT_TYPE_STRING);
    if (v) {
        const char *t = g_variant_get_string(v, NULL);
        e->is_check = (strcmp(t, "checkmark") == 0);
        e->is_radio = (strcmp(t, "radio") == 0);
        g_variant_unref(v);
    }
    v = g_variant_lookup_value(props, "toggle-state", G_VARIANT_TYPE_INT32);
    if (v) {
        e->toggle_state = g_variant_get_int32(v);
        g_variant_unref(v);
    }
    g_variant_unref(props);

    if (sub) {
        tray_menu_parse_av(sub, id, depth + 1);
        g_variant_unref(sub);
    }
}

static void
tray_menu_parse_items(GVariant *items, gint32 parent_id, int depth)
{
    GVariantIter iter;
    g_variant_iter_init(&iter, items);
    GVariant *tuple;
    while ((tuple = g_variant_iter_next_value(&iter)) != NULL) {
        tray_menu_parse_one(tuple, parent_id, depth);
        g_variant_unref(tuple);
        if (g_tm.n >= TRAY_MENU_MAX) break;
    }
}

static void
tray_menu_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source; (void)user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(g_sni.dbus_conn, res, &err);
    if (err) {
        g_clear_error(&err);
        return;
    }

    tray_menu_free_entries();
    GVariant *root_props, *children;
    guint32 rev;
    const char *rtype = g_variant_get_type_string(reply);

    if (strcmp(rtype, "(ua{sv}a(ia{sv}av))") == 0) {
        g_variant_get(reply, "(u@a{sv}@a(ia{sv}av))", &rev, &root_props,
                      &children);
        (void)rev;
        tray_menu_parse_items(children, 0, 0);
        g_variant_unref(children);
        g_variant_unref(root_props);
    } else if (strcmp(rtype, "(u(ia{sv}av))") == 0) {
        GVariant *root_tuple;
        g_variant_get(reply, "(u@(ia{sv}av))", &rev, &root_tuple);
        (void)rev;
        gint32 rid;
        GVariant *rprops, *rsub;
        g_variant_get(root_tuple, "(i@a{sv}@av)", &rid, &rprops, &rsub);
        if (rsub) {
            tray_menu_parse_av(rsub, rid, 0);
            g_variant_unref(rsub);
        }
        g_variant_unref(rprops);
        g_variant_unref(root_tuple);
    } else {
        g_variant_unref(reply);
        return;
    }
    g_variant_unref(reply);

    tray_menu_layout();
    if (g_tm.n_row == 0) {
        tray_menu_close();
        return;
    }
    render_request();
    popup_size_changed();
}

static void
tray_menu_fetch(void)
{
    if (!g_sni.dbus_conn || !g_tm.bus[0] || !g_tm.path[0]) return;
    const char *names[] = {
        "label", "enabled", "type", "toggle-type", "toggle-state",
        "icon-name", "icon-data", NULL
    };
    GVariant *props = g_variant_new_strv(names, -1);
    GVariant *params = g_variant_new("(ii@as)", 0, -1, props);
    g_dbus_connection_call(g_sni.dbus_conn, g_tm.bus, g_tm.path,
        "com.canonical.dbusmenu", "GetLayout", params,
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, tray_menu_fetch_done, NULL);
    g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
}

static void
tray_menu_signal(GDBusConnection *connection, const char *sender_name,
                 const char *object_path, const char *interface_name,
                 const char *signal_name, GVariant *parameters,
                 gpointer user_data)
{
    (void)connection; (void)sender_name; (void)object_path;
    (void)interface_name; (void)parameters; (void)user_data;
    if (strcmp(signal_name, "LayoutUpdated") != 0) return;
    if (!g_tm.open) return;
    tray_menu_fetch();
}

static void
tray_menu_open(struct tray_item *it, int idx)
{
    if (!it || !it->menu_path || !it->menu_path[0]) {
        return;
    }
    tray_menu_close();
    g_tm.open = 1;
    g_tm.tray_idx = idx;
    g_tm.hover_row = -1;
    g_tm.height = 0;
    snprintf(g_tm.bus, sizeof(g_tm.bus), "%s", it->bus);
    snprintf(g_tm.path, sizeof(g_tm.path), "%s", it->menu_path);
    g_tm.signal_id = g_dbus_connection_signal_subscribe(g_sni.dbus_conn,
        it->bus, "com.canonical.dbusmenu", "LayoutUpdated", it->menu_path,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, tray_menu_signal, NULL, NULL);
    tray_menu_fetch();
    popup_size_changed();
}

static void
tray_menu_close(void)
{
    if (!g_tm.open) return;
    g_tm.open = 0;
    g_tm.hover_row = -1;
    if (g_tm.signal_id) {
        g_dbus_connection_signal_unsubscribe(g_sni.dbus_conn, g_tm.signal_id);
        g_tm.signal_id = 0;
    }
    tray_menu_free_entries();
    g_tm.bus[0] = 0;
    g_tm.path[0] = 0;
    g_tm.height = 0;
    popup_size_changed();
}

static int
tray_menu_hit_test(double px, double py)
{
    if (!g_tm.open) return -1;
    if (!in_rect(px, py, g_tm.x, g_tm.y, g_tm.w, g_tm.height)) return -1;
    for (int i = 0; i < g_tm.n_row; i++) {
        if (py >= g_tm.row[i].y0 && py < g_tm.row[i].y1) return i;
    }
    return -1;
}

static void
tray_menu_event_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source; (void)user_data;
    GError *err = NULL;
    GVariant *v = g_dbus_connection_call_finish(g_sni.dbus_conn, res, &err);
    if (err) {
        g_clear_error(&err);
    }
    if (v) g_variant_unref(v);
}

static void
tray_menu_click(int row_idx)
{
    if (row_idx < 0 || row_idx >= g_tm.n_row) return;
    struct tray_menu_entry *e = &g_tm.e[g_tm.row[row_idx].e_idx];
    if (e->is_separator || !e->enabled) return;
    if (!g_tm.bus[0] || !g_tm.path[0]) return;
    GVariant *data;
    if (e->is_check || e->is_radio)
        data = g_variant_new_boolean(e->toggle_state == 1 ? FALSE : TRUE);
    else
        data = g_variant_new_boolean(TRUE);
    GVariant *args = g_variant_new("(isvu)", e->id, "clicked", data, (guint)0);
    g_dbus_connection_call(g_sni.dbus_conn, g_tm.bus, g_tm.path,
        "com.canonical.dbusmenu", "Event", args,
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, tray_menu_event_done, NULL);
    g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
}

struct tray_async {
    uint32_t gen;
};

static struct tray_async *
tray_async_new(struct tray_item *it)
{
    struct tray_async *a = g_new0(struct tray_async, 1);
    a->gen = it->gen;
    return a;
}

static void
tray_item_fetch_props_done(GObject *source, GAsyncResult *res,
                           gpointer user_data)
{
    struct tray_async *a = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res,
                                               &err);
    struct tray_item *it = tray_find_by_gen(a->gen);
    if (!reply || !it) {
        if (reply) g_variant_unref(reply);
        else g_clear_error(&err);
        g_free(a);
        return;
    }
    g_free(a);

    GVariant *dict = g_variant_get_child_value(reply, 0);
    if (g_variant_is_of_type(dict, G_VARIANT_TYPE("a{sv}"))) {
        GVariant *v;
        v = g_variant_lookup_value(dict, "IconName", G_VARIANT_TYPE_STRING);
        if (v) {
            g_free(it->icon_name);
            it->icon_name = g_strdup(g_variant_get_string(v, NULL));
            g_variant_unref(v);
        }
        v = g_variant_lookup_value(dict, "IconThemePath", G_VARIANT_TYPE_STRING);
        if (v) {
            g_free(it->icon_theme_path);
            it->icon_theme_path = g_strdup(g_variant_get_string(v, NULL));
            g_variant_unref(v);
        }
        if (it->pixmap) g_variant_unref(it->pixmap);
        it->pixmap = NULL;
        v = g_variant_lookup_value(dict, "IconPixmap", G_VARIANT_TYPE("a(iiay)"));
        if (v) it->pixmap = v;
        v = g_variant_lookup_value(dict, "ItemIsMenu", G_VARIANT_TYPE_BOOLEAN);
        if (v) {
            it->is_menu = g_variant_get_boolean(v);
            g_variant_unref(v);
        }
        v = g_variant_lookup_value(dict, "Menu", G_VARIANT_TYPE_OBJECT_PATH);
        if (v) {
            g_free(it->menu_path);
            it->menu_path = g_strdup(g_variant_get_string(v, NULL));
            g_variant_unref(v);
        }
    }
    g_variant_unref(dict);
    g_variant_unref(reply);
    tray_item_reload_icon(it);
    render_request();
}

static void
tray_item_fetch_props(struct tray_item *it)
{
    if (!it || !it->proxy) return;
    g_dbus_proxy_call(it->proxy, "org.freedesktop.DBus.Properties.GetAll",
                      g_variant_new("(s)", "org.kde.StatusNotifierItem"),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                      tray_item_fetch_props_done, tray_async_new(it));
}

static void
tray_item_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    struct tray_async *a = user_data;
    GError *err = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(res, &err);
    if (!proxy) { g_clear_error(&err); g_free(a); return; }
    struct tray_item *it = tray_find_by_gen(a->gen);
    if (!it) {
        g_object_unref(proxy);
        g_free(a);
        return;
    }
    it->proxy = proxy;
    tray_item_fetch_props(it);
    g_free(a);
}

static void
tray_item_destroy(struct tray_item *it)
{
    if (it->watch_id) g_bus_unwatch_name(it->watch_id);
    if (it->signal_id) g_dbus_connection_signal_unsubscribe(g_sni.dbus_conn, it->signal_id);
    if (it->proxy) g_object_unref(it->proxy);
    if (it->icon) cairo_surface_destroy(it->icon);
    if (it->pixmap) g_variant_unref(it->pixmap);
    g_free(it->icon_name);
    g_free(it->icon_theme_path);
    g_free(it->menu_path);
    memset(it, 0, sizeof(*it));
}

static void
tray_item_vanished(GDBusConnection *connection, const char *name,
                   gpointer user_data)
{
    (void)connection; (void)name;
    struct tray_ref *ref = user_data;
    if (!ref) return;
    struct tray_item *it = tray_find_by_gen(ref->gen);
    if (!it) return;
    char id[512];
    snprintf(id, sizeof(id), "%s", it->id);
    int idx = (int)(it - g_tray);
    tray_item_destroy(it);
    if (idx < g_sni.n_tray - 1)
        g_tray[idx] = g_tray[g_sni.n_tray - 1];
    g_sni.n_tray--;
    g_pointer.hover_tray = -1;
    if (g_sni.dbus_conn) {
        g_dbus_connection_emit_signal(g_sni.dbus_conn, NULL, "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher", "StatusNotifierItemUnregistered",
            g_variant_new("(s)", id), NULL);
    }
    render_request();
}

static GVariant *
tray_ids_variant(void)
{
    GVariantBuilder *b = g_variant_builder_new(G_VARIANT_TYPE("as"));
    for (int i = 0; i < g_sni.n_tray; i++)
        g_variant_builder_add(b, "s", g_tray[i].id);
    GVariant *v = g_variant_builder_end(b);
    g_variant_builder_unref(b);
    return v;
}

static void
tray_emit_props_changed(GDBusConnection *conn)
{
    GVariantBuilder props, invalid;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "RegisteredStatusNotifierItems",
                          tray_ids_variant());
    g_variant_builder_init(&invalid, G_VARIANT_TYPE("as"));
    g_dbus_connection_emit_signal(conn, NULL, "/StatusNotifierWatcher",
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", "org.kde.StatusNotifierWatcher",
                      &props, &invalid),
        NULL);
}

static const char *
tray_register(const char *sender, const char *arg)
{
    if (!g_sni.dbus_conn || !arg) return NULL;
    char bus[256], path[256];
    if (arg[0] == '/') {
        snprintf(bus, sizeof(bus), "%s", sender);
        snprintf(path, sizeof(path), "%s", arg);
    } else if (arg[0] == ':') {
        snprintf(bus, sizeof(bus), "%s", arg);
        snprintf(path, sizeof(path), "/StatusNotifierItem");
    } else {
        const char *colon = strchr(arg, ':');
        if (colon) {
            size_t n = (size_t)(colon - arg);
            if (n >= sizeof(bus)) n = sizeof(bus) - 1;
            memcpy(bus, arg, n);
            bus[n] = '\0';
            snprintf(path, sizeof(path), "%s", colon + 1);
            if (!path[0]) snprintf(path, sizeof(path), "/StatusNotifierItem");
        } else {
            snprintf(bus, sizeof(bus), "%s", arg);
            snprintf(path, sizeof(path), "/StatusNotifierItem");
        }
    }

    for (int i = 0; i < g_sni.n_tray; i++) {
        if (strcmp(g_tray[i].bus, bus) == 0 && strcmp(g_tray[i].path, path) == 0)
            return g_tray[i].id;
    }

    if (g_sni.n_tray >= TRAY_MAX_ITEMS) return NULL;

    struct tray_item *it = &g_tray[g_sni.n_tray];
    memset(it, 0, sizeof(*it));
    it->valid = TRUE;
    it->gen = ++g_sni.tray_gen;
    snprintf(it->bus, sizeof(it->bus), "%s", bus);
    snprintf(it->path, sizeof(it->path), "%s", path);
    snprintf(it->id, sizeof(it->id), "%s:%s", bus, path);

    struct tray_ref *sig_ref = g_new0(struct tray_ref, 1);
    sig_ref->gen = it->gen;
    it->signal_id = g_dbus_connection_signal_subscribe(g_sni.dbus_conn,
        bus, "org.kde.StatusNotifierItem", NULL, path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, tray_item_signal, sig_ref, g_free);

    struct tray_ref *watch_ref = g_new0(struct tray_ref, 1);
    watch_ref->gen = it->gen;
    it->watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, bus,
        G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, tray_item_vanished, watch_ref,
        g_free);

    g_sni.n_tray++;
    g_pointer.hover_tray = tray_hit_test(g_pointer.x, g_pointer.y);

    g_dbus_proxy_new(g_sni.dbus_conn, G_DBUS_PROXY_FLAGS_NONE, NULL, bus, path,
                     "org.kde.StatusNotifierItem", NULL,
                     tray_item_proxy_ready, tray_async_new(it));
    render_request();
    return it->id;
}

static void
tray_watcher_method_call(GDBusConnection *connection, const char *sender,
                         const char *object_path, const char *interface_name,
                         const char *method_name, GVariant *parameters,
                         GDBusMethodInvocation *invocation,
                         gpointer user_data)
{
    (void)object_path; (void)interface_name; (void)user_data;

    if (strcmp(method_name, "RegisterStatusNotifierItem") == 0) {
        const char *arg = NULL;
        if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)")))
            g_variant_get(parameters, "(&s)", &arg);
        const char *id = tray_register(sender, arg);
        if (id) {
            g_dbus_connection_emit_signal(connection, NULL, "/StatusNotifierWatcher",
                "org.kde.StatusNotifierWatcher", "StatusNotifierItemRegistered",
                g_variant_new("(s)", id), NULL);
            tray_emit_props_changed(connection);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (strcmp(method_name, "RegisterStatusNotifierHost") == 0) {
        g_dbus_connection_emit_signal(connection, NULL, "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher", "StatusNotifierHostRegistered",
            NULL, NULL);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method_name);
    }
}

static GVariant *
tray_watcher_get_property(GDBusConnection *connection, const char *sender,
                          const char *object_path, const char *interface_name,
                          const char *property_name, GError **error,
                          gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path;
    (void)interface_name; (void)error; (void)user_data;
    if (strcmp(property_name, "RegisteredStatusNotifierItems") == 0)
        return tray_ids_variant();
    if (strcmp(property_name, "IsStatusNotifierHostRegistered") == 0)
        return g_variant_new_boolean(TRUE);
    if (strcmp(property_name, "ProtocolVersion") == 0)
        return g_variant_new_int32(0);
    return NULL;
}

static const GDBusInterfaceVTable tray_watcher_vtable = {
    .method_call = tray_watcher_method_call,
    .get_property = tray_watcher_get_property,
};

static const char tray_watcher_xml[] =
    "<node>"
    "<interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'>"
    "<arg type='s' name='service' direction='in'/>"
    "</method>"
    "<method name='RegisterStatusNotifierHost'>"
    "<arg type='s' name='service' direction='in'/>"
    "</method>"
    "<property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "<property name='ProtocolVersion' type='i' access='read'/>"
    "<signal name='StatusNotifierItemRegistered'>"
    "<arg type='s' name='service'/>"
    "</signal>"
    "<signal name='StatusNotifierItemUnregistered'>"
    "<arg type='s' name='service'/>"
    "</signal>"
    "<signal name='StatusNotifierHostRegistered'/>"
    "</interface>"
    "</node>";

static void
tray_init(void)
{
    if (!g_sni.glib_ctx) {
        g_sni.glib_ctx = g_main_context_new();
        g_main_context_push_thread_default(g_sni.glib_ctx);
    }

    if (g_sni.dbus_conn)
        return;

    GError *err = NULL;
    g_sni.dbus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!g_sni.dbus_conn) {
        g_clear_error(&err);
        return;
    }

    g_sni.node = g_dbus_node_info_new_for_xml(tray_watcher_xml, &err);
    if (!g_sni.node) {
        g_clear_error(&err);
        g_object_unref(g_sni.dbus_conn);
        g_sni.dbus_conn = NULL;
        return;
    }

    g_sni.obj_reg = g_dbus_connection_register_object(g_sni.dbus_conn,
        "/StatusNotifierWatcher",
        g_sni.node->interfaces[0], &tray_watcher_vtable, NULL, NULL, &err);
    g_clear_error(&err);

    g_sni.owner = g_bus_own_name_on_connection(g_sni.dbus_conn,
        "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        NULL, NULL, NULL, NULL);

    notif_dbus_init();
}

static void
tray_shutdown(void)
{
    for (int i = 0; i < g_sni.n_tray; i++)
        tray_item_destroy(&g_tray[i]);
    g_sni.n_tray = 0;
    if (g_sni.owner) g_bus_unown_name(g_sni.owner);
    g_sni.owner = 0;
    if (g_sni.dbus_conn) {
        g_dbus_connection_unregister_object(g_sni.dbus_conn, g_sni.obj_reg);
        g_object_unref(g_sni.dbus_conn);
        g_sni.dbus_conn = NULL;
    }
    if (g_sni.node) { g_dbus_node_info_unref(g_sni.node); g_sni.node = NULL; }
    notif_shutdown();
    if (g_sni.glib_ctx) {
        g_main_context_pop_thread_default(g_sni.glib_ctx);
        g_main_context_unref(g_sni.glib_ctx);
        g_sni.glib_ctx = NULL;
    }
}

static void
clock_rearm(void)
{
    if (g_sni.clock_fd < 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    int64_t ns_per_min = 60LL * 1000000000;
    int64_t next = ((now / ns_per_min) + 1) * ns_per_min;
    int64_t delta = next - now;
    struct itimerspec its = {
        .it_value = { .tv_sec = delta / 1000000000, .tv_nsec = delta % 1000000000 },
    };
    timerfd_settime(g_sni.clock_fd, 0, &its, NULL);
}

static gint
glib_epoll_poll(GPollFD *ufds, guint nfds, gint timeout_ms)
{
    if (g_sni.main_epoll_fd < 0)
        return g_poll(ufds, nfds, timeout_ms);

    for (guint i = 0; i < nfds; i++) {
        uint32_t ev = 0;
        if (ufds[i].events & G_IO_IN) ev |= EPOLLIN;
        if (ufds[i].events & G_IO_OUT) ev |= EPOLLOUT;
        if (ufds[i].events & G_IO_HUP) ev |= EPOLLHUP;
        if (ufds[i].events & G_IO_ERR) ev |= EPOLLERR;
        struct epoll_event ge = { .events = ev, .data.fd = ufds[i].fd };
        if (epoll_ctl(g_sni.main_epoll_fd, EPOLL_CTL_ADD, ufds[i].fd, &ge) < 0 &&
            errno == EEXIST)
            epoll_ctl(g_sni.main_epoll_fd, EPOLL_CTL_MOD, ufds[i].fd, &ge);
    }

    while (wl_display_prepare_read(g_wl.display) != 0)
        wl_display_dispatch_pending(g_wl.display);
    wl_display_flush(g_wl.display);

    struct epoll_event evs[8];
    int n = epoll_wait(g_sni.main_epoll_fd, evs, 8, timeout_ms);

    int wl_readable = 0;
    int wl_dead = 0;
    int clock_ticked = 0;
    int sys_ticked = 0;
    int apps_ticked = 0;
    for (int i = 0; i < n; i++) {
        if (evs[i].data.fd == g_sni.wl_fd) {
            if (evs[i].events & EPOLLIN) wl_readable = 1;
            if (evs[i].events & (EPOLLHUP | EPOLLERR)) wl_dead = 1;
            continue;
        }
        if (evs[i].data.fd == g_sni.clock_fd) clock_ticked = 1;
        if (evs[i].data.fd == g_box.sys_fd) sys_ticked = 1;
        if (evs[i].data.fd == g_app.apps_fd) apps_ticked = 1;
    }

    if (clock_ticked) {
        uint64_t expirations;
        if (read(g_sni.clock_fd, &expirations, sizeof(expirations)) < 0)
            (void)0;
        net_update();
        audio_update();
        time_now(g_box.time_str, sizeof(g_box.time_str));
        render_request();
        clock_rearm();
    }

    if (sys_ticked) {
        uint64_t expirations;
        if (read(g_box.sys_fd, &expirations, sizeof(expirations)) < 0)
            (void)0;
        sys_tick();
    }

    if (apps_ticked)
        apps_watch_handle();

    if (wl_readable) {
        if (wl_display_read_events(g_wl.display) < 0)
            wl_dead = 1;
    } else {
        wl_display_cancel_read(g_wl.display);
    }

    if (wl_dead)
        g_app.running = 0;

    int ret = 0;
    for (guint i = 0; i < nfds; i++) {
        ufds[i].revents = 0;
        for (int j = 0; j < n; j++) {
            if (evs[j].data.fd == ufds[i].fd) {
                if (evs[j].events & EPOLLIN) ufds[i].revents |= G_IO_IN;
                if (evs[j].events & EPOLLOUT) ufds[i].revents |= G_IO_OUT;
                if (evs[j].events & EPOLLHUP) ufds[i].revents |= G_IO_HUP;
                if (evs[j].events & EPOLLERR) ufds[i].revents |= G_IO_ERR;
                if (ufds[i].revents) ret++;
                break;
            }
        }
    }
    return ret;
}

static char *
pinned_config_path(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    static char path[512];
    snprintf(path, sizeof(path), "%s/.config/jtlab/pinned", home);
    return path;
}

static void
pinned_save(void)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/jtlab", getenv("HOME") ?: "/tmp");
    mkdir(dir, 0755);

    FILE *f = fopen(pinned_config_path(), "w");
    if (!f) return;
    for (int i = 0; i < g_app.n_pinned; i++) {
        const char *id = g_app.apps[g_app.pinned[i]].desktop_id;
        fprintf(f, "%s\n", id ? id : g_app.apps[g_app.pinned[i]].exec);
    }
    fclose(f);
}

static void
pinned_load(void)
{
    FILE *f = fopen(pinned_config_path(), "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        desktop_file_trim(line);
        if (!line[0]) continue;
        int found = -1;
        for (int i = 0; i < g_app.n_apps; i++) {
            const char *id = g_app.apps[i].desktop_id;
            if ((id && strcmp(id, line) == 0) ||
                strcmp(g_app.apps[i].exec, line) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) continue;
        if (g_app.n_pinned >= g_app.pinned_cap) {
            int new_cap = g_app.pinned_cap ? g_app.pinned_cap * 2 : 16;
            int *new_pinned = realloc(g_app.pinned, new_cap * sizeof(int));
            if (!new_pinned) break;
            g_app.pinned = new_pinned;
            g_app.pinned_cap = new_cap;
        }
        g_app.pinned[g_app.n_pinned++] = found;
    }
    fclose(f);
}

static int
pinned_is(int app_idx)
{
    for (int i = 0; i < g_app.n_pinned; i++)
        if (g_app.pinned[i] == app_idx) return 1;
    return 0;
}

static void
pinned_toggle(int app_idx)
{
    for (int i = 0; i < g_app.n_pinned; i++) {
        if (g_app.pinned[i] == app_idx) {
            pinned_drop(i);
            return;
        }
    }
    if (g_app.n_pinned >= g_app.pinned_cap) {
        int new_cap = g_app.pinned_cap ? g_app.pinned_cap * 2 : 16;
        int *new_pinned = realloc(g_app.pinned, new_cap * sizeof(int));
        if (!new_pinned) return;
        g_app.pinned = new_pinned;
        g_app.pinned_cap = new_cap;
    }
    g_app.pinned[g_app.n_pinned++] = app_idx;
    pinned_save();
    pinned_recalc_layout();
    toplevel_recalc_layout();
    render_request();
}

static void
pinned_free(void)
{
    free(g_app.pinned);
    g_app.pinned = NULL;
    g_app.n_pinned = 0;
    g_app.pinned_cap = 0;
}

static void
exec_clean(const char *src, char *dst, size_t dst_sz)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di < dst_sz - 1; ) {
        if (src[i] == '@' && src[i + 1] == '@') {
            i += 2;
            while (src[i] && src[i] != '@' && src[i] != ' ') i++;
            if (src[i] == '@') i++;
            while (src[i] == ' ') i++;
            continue;
        }
        if (src[i] == '%' && src[i + 1] && strchr("ufUFdDnNicvm", src[i + 1])) {
            i += 2;
            if (di > 0 && dst[di - 1] == ' ') di--;
            continue;
        }
        dst[di++] = src[i++];
    }
    dst[di] = '\0';
}

static void
run_cmd_child_cleanup(void)
{
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) _exit(127);
    if (devnull != 0) dup2(devnull, 0);
    if (devnull != 1) dup2(devnull, 1);
    if (devnull != 2) dup2(devnull, 2);
    if (devnull > 2) close(devnull);
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        int dfd = dirfd(d);
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end = NULL;
            long fd = strtol(e->d_name, &end, 10);
            if (end == e->d_name || *end) continue;
            if (fd > 2 && (int)fd != dfd) close((int)fd);
        }
        closedir(d);
    }
}

static void
run_cmd(const char *cmd, int terminal)
{
    if (!cmd || !cmd[0]) return;
    char clean[512];
    exec_clean(cmd, clean, sizeof(clean));
    if (!clean[0]) return;
    char buf[1024];
    if (terminal)
        snprintf(buf, sizeof(buf), "%s %s", TERMINAL_CMD, clean);
    else
        snprintf(buf, sizeof(buf), "%s", clean);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        run_cmd_child_cleanup();
        execl("/bin/sh", "sh", "-c", buf, NULL);
        _exit(127);
    }
}

/* The scrollable app list region for a given menu body height. Single source of
 * truth for where rows are drawn and hit-tested: double vertical padding on
 * top/bottom plus the search bar below the list. */
static void
menu_list_area(double mh_body, double *top, double *bottom)
{
    double pad = menu_vert_pad(mh_body);
    *top = 2 * pad;
    *bottom = mh_body - 2 * pad - MENU_SEARCH_H - 2 * pad;
}

/* Number of full app rows that fit in the visible list region, derived from
 * the actual geometry so the list shows as many entries as fit without a
 * hand-tuned row count. */
static int
menu_vis_rows(double mh_body)
{
    double top, bottom;
    menu_list_area(mh_body, &top, &bottom);
    double list_h = bottom - top;
    if (list_h < MENU_ROW_HEIGHT) list_h = MENU_ROW_HEIGHT;
    int rows = (int)(list_h / MENU_ROW_HEIGHT);
    if (rows > g_app.menu_n) rows = g_app.menu_n;
    return rows;
}

static int
menu_max_scroll(void)
{
    double mh_body = g_bar.menu_height - MENU_FLOAT_GAP;
    int vis = menu_vis_rows(mh_body);
    return g_app.menu_n - vis;
}

static void
menu_clamp_scroll(void)
{
    int maxs = menu_max_scroll();
    g_bar.menu_scroll = clampd(g_bar.menu_scroll, 0, maxs);
}

static int
menu_anim_wants_render(void)
{
    return g_bar.menu_open;
}

static struct scroll_anim menu_anim = {
    .current = &g_bar.menu_anim_px,
    .target  = &g_bar.menu_scroll_target_px,
    .src     = &g_bar.menu_anim_src,
    .wants_render = menu_anim_wants_render,
};

static void
menu_scroll_anim_start(void)
{
    scroll_anim_start(&menu_anim);
}

static void
menu_scroll_anim_cancel(void)
{
    scroll_anim_cancel(&menu_anim);
}

static void
menu_scroll_sync_target(void)
{
    g_bar.menu_scroll_target_px = g_bar.menu_scroll * (double)MENU_ROW_HEIGHT;
}

static void
menu_scroll_set_row(int row)
{
    int old = g_bar.menu_scroll;
    g_bar.menu_scroll = clampd(row, 0, menu_max_scroll());
    if (g_bar.menu_scroll == old) return;
    menu_scroll_sync_target();
    menu_scroll_anim_start();
    render_request();
}

static int
menu_idx_compare(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return strcasecmp(g_app.apps[ia].name, g_app.apps[ib].name);
}

static void
menu_filter_update(void)
{
    if (g_app.apps_cap > g_app.menu_idx_cap) {
        int cap = g_app.apps_cap > 0 ? g_app.apps_cap : 64;
        int *ni = realloc(g_app.menu_idx, cap * sizeof(*g_app.menu_idx));
        if (!ni) return;
        g_app.menu_idx = ni;
        g_app.menu_idx_cap = cap;
    }
    g_app.menu_n = 0;
    for (int i = 0; i < g_app.n_apps; i++) {
        if (g_app.search[0] &&
            !strcasestr(g_app.apps[i].name, g_app.search) &&
            !strcasestr(g_app.apps[i].exec, g_app.search))
            continue;
        g_app.menu_idx[g_app.menu_n++] = i;
    }
    qsort(g_app.menu_idx, g_app.menu_n, sizeof(*g_app.menu_idx), menu_idx_compare);
    if (g_app.menu_sel >= g_app.menu_n) g_app.menu_sel = g_app.menu_n ? g_app.menu_n - 1 : 0;
    if (g_app.menu_sel < 0) g_app.menu_sel = 0;
    menu_clamp_scroll();
    menu_scroll_anim_cancel();
    g_bar.menu_anim_px = g_bar.menu_scroll * (double)MENU_ROW_HEIGHT;
    g_bar.menu_scroll_target_px = g_bar.menu_anim_px;
}

static void
menu_search_add(const char *utf8, int len)
{
    int n = (int)strlen(g_app.search);
    if (n + len >= (int)sizeof(g_app.search)) return;
    memcpy(g_app.search + n, utf8, len);
    g_app.search[n + len] = '\0';
    menu_filter_update();
    render_request();
}

static void
menu_search_backspace(void)
{
    int n = (int)strlen(g_app.search);
    if (n == 0) return;
    n--;
    while (n > 0 && (g_app.search[n] & 0xC0) == 0x80) n--;
    g_app.search[n] = '\0';
    menu_filter_update();
    render_request();
}

static void
menu_search_clear(void)
{
    if (!g_app.search[0]) return;
    g_app.search[0] = '\0';
    menu_filter_update();
}

static void
menu_sel_move(int dir)
{
    if (g_app.menu_n == 0) return;
    int old = g_app.menu_sel;
    g_app.menu_sel += dir;
    if (g_app.menu_sel < 0) g_app.menu_sel = 0;
    if (g_app.menu_sel >= g_app.menu_n) g_app.menu_sel = g_app.menu_n - 1;
    if (g_app.menu_sel != old) {
        double mh_body = g_bar.menu_height - MENU_FLOAT_GAP;
        int vis = menu_vis_rows(mh_body);
        if (g_app.menu_sel < g_bar.menu_scroll)
            menu_scroll_set_row(g_app.menu_sel);
        else if (g_app.menu_sel >= g_bar.menu_scroll + vis)
            menu_scroll_set_row(g_app.menu_sel - vis + 1);
        render_request();
    }
}

static int
menu_pos_of_app(int app_idx)
{
    for (int i = 0; i < g_app.menu_n; i++)
        if (g_app.menu_idx[i] == app_idx) return i;
    return -1;
}

static double
menu_vert_pad(double mh_body)
{
    (void)mh_body;
    return MENU_PADDING;
}

static double
menu_search_area(double mh_body)
{
    return MENU_SEARCH_H + 2 * menu_vert_pad(mh_body);
}

static int
calc_menu_height(void)
{
    int rows = g_app.n_apps;
    double body = 0;
    for (int i = 0; i < 10; i++) {
        double apps_h = 2 * menu_vert_pad(body) + rows * MENU_ROW_HEIGHT
                        + 2 * menu_search_area(body);
        double power_h = MENU_PADDING + 4 * POWER_ROW_H + 2 * menu_vert_pad(body);
        double sys_h = sys_column_height();
        double b = apps_h > power_h ? apps_h : power_h;
        if (sys_h > b) b = sys_h;
        if ((int)b & 1) b++;
        b += 2;
        if (fabs(b - body) < 0.5) { body = b; break; }
        body = b;
    }
    int h = MENU_FLOAT_GAP + (int)(body + 0.5);
    int max_h = MENU_FLOAT_GAP + POPUP_BODY_H;
    return h > max_h ? max_h : h;
}

static void
ctx_open(int app_idx, double x, double y)
{
    g_ctx.open = 1;
    g_ctx.target_app = app_idx;
    g_ctx.n_items = 2;

    double cw = CTX_WIDTH;
    double ch = g_ctx.n_items * CTX_ROW_HEIGHT;
    double mx = MENU_MARGIN_L;
    double mw = MENU_WIDTH + POWER_COL_W + SYS_COL_W;
    double mh_body = g_bar.menu_height - MENU_FLOAT_GAP;

    double cx = x + 4;
    cx = clampd(cx, mx, mx + mw - cw);
    g_ctx.x = cx;

    double cy = y;
    cy = clampd(cy, 0, mh_body - ch);
    g_ctx.y = cy;
}

static void
ctx_close(void)
{
    g_ctx.open = 0;
}

static int
ctx_hit_test(double px, double py)
{
    if (!g_ctx.open) return -1;
    if (!in_rect(px, py, g_ctx.x, g_ctx.y, CTX_WIDTH,
                 g_ctx.n_items * CTX_ROW_HEIGHT)) return -1;
    int idx = (int)((py - g_ctx.y) / CTX_ROW_HEIGHT);
    if (idx >= 0 && idx < g_ctx.n_items) return idx;
    return -1;
}

static void
gpopup_clear_hover(void)
{
    g_gpopup.hover_idx = -1;
    g_gpopup.hover_close = -1;
}

static double
popup_x_clamp(double x, double w)
{
    double lo = 8, hi = g_bar.width - 8 - w;
    if (hi < lo) hi = lo;
    return clampd(x, lo, hi);
}

static void
gpopup_fill(int count, const int *windows, double cx, double y)
{
    g_gpopup.open = 1;
    g_gpopup.n_windows = count;
    for (int i = 0; i < count; i++)
        g_gpopup.windows[i] = windows[i];
    g_gpopup.x = popup_x_clamp(cx - GROUP_POPUP_W / 2.0, GROUP_POPUP_W);
    g_gpopup.y = y;
    g_gpopup.height = MENU_FLOAT_GAP + MENU_PADDING * 2 + count * MENU_ROW_HEIGHT;
    gpopup_clear_hover();
    popup_size_changed();
}

static void close_sibling_popups(void);

static void
gpopup_open(int group_idx, double x, double y)
{
    close_sibling_popups();
    struct taskbar_group *g = &g_box.taskbar_groups[group_idx];
    gpopup_fill(g->n_windows, g->windows, x + g->w / 2.0, y);
}

static void
gpopup_open_pinned(int pinned_idx, double x, double y)
{
    const struct app_entry *a = &g_app.apps[g_app.pinned[pinned_idx]];
    int count = windows_of_app(a, g_gpopup.windows, MAX_WINDOWS_PER_GROUP);
    if (count < 2) return;
    close_sibling_popups();
    gpopup_fill(count, g_gpopup.windows, x + g_box.pl[pinned_idx].w / 2.0, y);
}

static void
gpopup_close(void)
{
    if (!g_gpopup.open) return;
    g_gpopup.open = 0;
    gpopup_clear_hover();
    popup_size_changed();
}

static const char *
cpop_label(int i)
{
    return (i == 0 && g_cpop.app_idx >= 0) ? "Open new window" : "Close window";
}

static void
cpop_open(int app_idx, int window_idx, double x, double y)
{
    g_cpop.app_idx = app_idx;
    g_cpop.window_idx = window_idx;
    g_cpop.n_items = 0;
    if (app_idx >= 0) g_cpop.n_items++;
    if (window_idx >= 0) g_cpop.n_items++;
    if (g_cpop.n_items == 0) return;

    g_cpop.open = 1;
    g_cpop.y = y;
    cpop_clear_hover();

    double tw = 90;
    if (g_bar.cr) {
        for (int i = 0; i < g_cpop.n_items; i++) {
            double t = text_width_px(cpop_label(i)) + 20;
            if (t > tw) tw = t;
        }
    }
    g_cpop.w = tw * 1.2;
    g_cpop.x = popup_x_clamp(x - g_cpop.w / 2.0, g_cpop.w);
    g_cpop.h = MENU_FLOAT_GAP + MENU_PADDING * 2 + CTX_ROW_HEIGHT * g_cpop.n_items;
    popup_size_changed();
}

static void
cpop_close(void)
{
    if (!g_cpop.open) return;
    g_cpop.open = 0;
    cpop_clear_hover();
    popup_size_changed();
}

static void
cpop_clear_hover(void)
{
    g_cpop.hover = -1;
}

static int
cpop_hit_test(double px, double py)
{
    if (!g_cpop.open) return -1;
    double ch = g_cpop.h - MENU_FLOAT_GAP;
    if (!in_rect(px, py, g_cpop.x, g_cpop.y, g_cpop.w, ch)) return -1;
    int row = (int)((py - g_cpop.y - MENU_PADDING) / CTX_ROW_HEIGHT);
    if (row < 0 || row >= g_cpop.n_items) return -1;
    return row;
}

struct vol_popup {
    int open;
    double x, y;
    double w, h;
    double mute_x, mute_y, mute_w, mute_h;
    double slider_x_min, slider_x_max, slider_y;
    int hover_mute;
    int dragging;
    int tab;
    int hover_tab;
    int hover_dev;
    double tab_y;
    double list_y;
};

static struct vol_popup g_volpop = { 0 };

static void pw_set_volume(float vol_pct);
static void pw_set_mute(int muted);

static int
volpop_list_rows(void)
{
    int n = g_volpop.tab == 0 ? g_box.vol_snap.n_sinks : g_box.vol_snap.n_sources;
    if (n < 0) n = 0;
    return n;
}

static int
volpop_max_rows(void)
{
    int n = g_box.vol_snap.n_sinks > g_box.vol_snap.n_sources
            ? g_box.vol_snap.n_sinks : g_box.vol_snap.n_sources;
    if (n < 0) n = 0;
    return n;
}

static int
volpop_height(void)
{
    return (int)g_volpop.h;
}

static void
volpop_recalc_layout(void)
{
    double pw = VOL_POPUP_WIDTH;
    double px = g_bar.width - TRAY_EDGE_PAD - pw;
    double tab_y = MENU_PADDING;
    double list_y = tab_y + VOL_TAB_H + 4;
    int n_rows = volpop_max_rows();
    double ctrl_y = list_y + n_rows * VOL_DEV_ROW_H + 4;
    double body_h = ctrl_y + VOL_ROW_H + MENU_PADDING;
    double yc = ctrl_y + VOL_ROW_H / 2.0;

    g_volpop.x = px;
    g_volpop.y = 0;
    g_volpop.w = pw;
    g_volpop.h = body_h + MENU_FLOAT_GAP;
    g_volpop.tab_y = tab_y;
    g_volpop.list_y = list_y;
    g_volpop.mute_x = px + MENU_PADDING;
    g_volpop.mute_y = yc - VOL_MUTE_ICON / 2.0;
    g_volpop.mute_w = VOL_MUTE_ICON;
    g_volpop.mute_h = VOL_MUTE_ICON;
    g_volpop.slider_y = yc;

    char vlabel[8];
    snprintf(vlabel, sizeof(vlabel), "%d%%", 100);
    double label_w = text_width_px(vlabel);
    double label_right = px + pw - MENU_PADDING;
    double slider_right = label_right - 10 - label_w;
    g_volpop.slider_x_min = g_volpop.mute_x + VOL_MUTE_ICON + 10;
    g_volpop.slider_x_max = slider_right;
    if (g_volpop.slider_x_max < g_volpop.slider_x_min)
        g_volpop.slider_x_max = g_volpop.slider_x_min;
}

static void
volpop_clear_hover(void)
{
    g_volpop.hover_mute = 0;
    g_volpop.dragging = 0;
    g_volpop.hover_tab = -1;
    g_volpop.hover_dev = -1;
}

static void
volpop_open(void)
{
    g_volpop.open = 1;
    pw_reconnect_check();
    if (g_box.audio_present)
        audio_snapshot(&g_box.vol_snap);
    volpop_clear_hover();
    volpop_recalc_layout();
    popup_size_changed();
}

static void
volpop_close(void)
{
    if (!g_volpop.open) return;
    g_volpop.open = 0;
    volpop_clear_hover();
    popup_size_changed();
}

static int
volpop_hit_test(double px, double py)
{
    if (!g_volpop.open) return 0;
    return px >= g_volpop.x && px < g_volpop.x + g_volpop.w &&
           py >= g_volpop.y && py < g_volpop.y + g_volpop.h;
}

static int
volpop_tab_hit_test(double px, double py)
{
    if (!g_volpop.open) return -1;
    if (px < g_volpop.x || px >= g_volpop.x + g_volpop.w) return -1;
    if (py < g_volpop.tab_y || py >= g_volpop.tab_y + VOL_TAB_H) return -1;
    return (px - g_volpop.x) < g_volpop.w / 2.0 ? 0 : 1;
}

static int
volpop_dev_hit_test(double px, double py)
{
    if (!g_volpop.open) return -1;
    int n_rows = volpop_list_rows();
    if (n_rows <= 0) return -1;
    if (px < g_volpop.x || px >= g_volpop.x + g_volpop.w) return -1;
    if (py < g_volpop.list_y || py >= g_volpop.list_y + n_rows * VOL_DEV_ROW_H)
        return -1;
    int row = (int)((py - g_volpop.list_y) / VOL_DEV_ROW_H);
    if (row < 0 || row >= n_rows) return -1;
    return row;
}

static int
volpop_mute_hit_test(double px, double py)
{
    if (!g_volpop.open) return 0;
    return in_rect(px, py, g_volpop.mute_x - 4, g_volpop.mute_y - 4,
                   g_volpop.mute_w + 8, g_volpop.mute_h + 8);
}

static int
volpop_slider_hit_test(double px, double py)
{
    if (!g_volpop.open) return 0;
    if (px < g_volpop.slider_x_min || px > g_volpop.slider_x_max) return 0;
    double reach = VOL_SLIDER_H / 2.0 + 6;
    return py >= g_volpop.slider_y - reach && py <= g_volpop.slider_y + reach;
}

static void
volpop_set_from_x(double px)
{
    double range = g_volpop.slider_x_max - g_volpop.slider_x_min;
    if (range <= 0) return;
    float pct = (float)((px - g_volpop.slider_x_min) / range * VOLUME_MAX);
    if (pct < VOLUME_MIN) pct = (float)VOLUME_MIN;
    if (pct > VOLUME_MAX) pct = (float)VOLUME_MAX;
    pw_set_volume(pct);
    render_request();
}

static struct audio_dev *
volpop_active_dev(void)
{
    if (!g_pw.thread_loop) return NULL;
    int idx = (g_volpop.tab == 0) ? g_pw.sink_idx : g_pw.source_idx;
    struct audio_dev *d = NULL;
    if (idx >= 0 && idx < g_pw.n_devices)
        d = &g_pw.devices[idx];
    else
        for (int i = 0; i < g_pw.n_devices; i++)
            if (g_pw.devices[i].is_sink == (g_volpop.tab == 0)) {
                d = &g_pw.devices[i];
                break;
            }
    return d;
}

static void
volpop_scroll(double delta)
{
    if (!g_pw.thread_loop) return;
    pw_thread_loop_lock(g_pw.thread_loop);
    struct audio_dev *d = volpop_active_dev();
    int has_dev = (d != NULL);
    float vol = has_dev ? d->volume : 0.0f;
    pw_thread_loop_unlock(g_pw.thread_loop);
    if (!has_dev) return;
    vol += (float)delta;
    if (vol < VOLUME_MIN) vol = (float)VOLUME_MIN;
    if (vol > VOLUME_MAX) vol = (float)VOLUME_MAX;
    pw_set_volume(vol);
    render_request();
}

static void
pw_set_volume(float vol_pct)
{
    struct audio_dev *d;
    if (!g_pw.thread_loop) return;

    float linear = powf(vol_pct / 100.0f, 3.0f);
    linear = clampd(linear, 0.0f, 1.0f);

    pw_thread_loop_lock(g_pw.thread_loop);
    d = volpop_active_dev();
    if (!d || !d->proxy || d->channels == 0) {
        pw_thread_loop_unlock(g_pw.thread_loop);
        return;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));
    struct spa_pod_frame frame;

    spa_pod_builder_push_object(&builder, &frame,
                                SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
    struct spa_pod_frame arr_frame;
    spa_pod_builder_push_array(&builder, &arr_frame);
    for (int i = 0; i < d->channels; i++)
        spa_pod_builder_float(&builder, linear);
    spa_pod_builder_pop(&builder, &arr_frame);
    spa_pod_builder_pop(&builder, &frame);

    const struct spa_pod *pod = spa_pod_builder_deref(&builder, 0);
    if (pod)
        pw_node_set_param((struct pw_node *)d->proxy,
                          SPA_PARAM_Props, 0, pod);
    pw_thread_loop_unlock(g_pw.thread_loop);
}

static void
pw_set_mute(int muted)
{
    struct audio_dev *d;
    if (!g_pw.thread_loop) return;

    pw_thread_loop_lock(g_pw.thread_loop);
    d = volpop_active_dev();
    if (!d || !d->proxy) {
        pw_thread_loop_unlock(g_pw.thread_loop);
        return;
    }

    uint8_t buffer[256];
    struct spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));
    struct spa_pod_frame frame;

    spa_pod_builder_push_object(&builder, &frame,
                                SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&builder, muted);
    spa_pod_builder_pop(&builder, &frame);

    const struct spa_pod *pod = spa_pod_builder_deref(&builder, 0);
    if (pod)
        pw_node_set_param((struct pw_node *)d->proxy,
                          SPA_PARAM_Props, 0, pod);
    pw_thread_loop_unlock(g_pw.thread_loop);
}

static void
pw_set_default(int idx)
{
    struct audio_dev *d;
    if (!g_pw.thread_loop) return;

    pw_thread_loop_lock(g_pw.thread_loop);
    if (idx < 0 || idx >= g_pw.n_devices) {
        pw_thread_loop_unlock(g_pw.thread_loop);
        return;
    }
    d = &g_pw.devices[idx];
    char json[640];
    snprintf(json, sizeof(json), "{\"name\":\"%s\"}", d->name);

    if (g_pw.metadata)
        pw_metadata_set_property(g_pw.metadata, 0,
                                 d->is_sink ? "default.audio.sink" : "default.audio.source",
                                 "Spa:String:JSON", json);
    pw_thread_loop_unlock(g_pw.thread_loop);
}

static int
volpop_row_to_dev(int row)
{
    int want_sink = (g_volpop.tab == 0);
    int k = 0;
    for (int i = 0; i < g_pw.n_devices; i++) {
        if (g_box.vol_snap.items[i].is_sink != want_sink) continue;
        if (k == row) return g_box.vol_snap.items[i].dev_idx;
        k++;
    }
    return -1;
}

#define WS_MAX 9
struct workspace_state {
    char name[8];
    int active;
    struct ext_workspace_handle_v1 *handle;
};

static struct workspace_state g_workspaces[WS_MAX];

struct ws_proxy {
    struct ext_workspace_handle_v1 *handle;
    char name[64];
    uint32_t state;
    int removed;
};

static struct ws_proxy g_ws_proxies[WS_MAX];

static struct {
    int ws_count;
    int ws_proxies_n;
} g_ws = { 0 };

static void
workspaces_placeholder_init(void)
{
    for (int i = 0; i < WS_MAX; i++) {
        snprintf(g_workspaces[i].name, sizeof(g_workspaces[i].name), "%d", i + 1);
        g_workspaces[i].active = (i == 0);
        g_workspaces[i].handle = NULL;
    }
    g_ws.ws_count = WS_MAX;
}

static const struct ext_workspace_handle_v1_listener ws_handle_listener;

static void
workspaces_compact_proxies(void)
{
    int n = 0;
    for (int i = 0; i < g_ws.ws_proxies_n; i++) {
        struct ws_proxy *p = &g_ws_proxies[i];
        if (p->removed) continue;
        if (n != i) {
            g_ws_proxies[n] = *p;
            ext_workspace_handle_v1_add_listener(g_ws_proxies[n].handle,
                                                 &ws_handle_listener,
                                                 &g_ws_proxies[n]);
        }
        n++;
    }
    g_ws.ws_proxies_n = n;
}

static void
workspaces_sync_from_proxies(void)
{
    workspaces_compact_proxies();
    int n = 0;
    for (int i = 0; i < g_ws.ws_proxies_n; i++) {
        struct ws_proxy *p = &g_ws_proxies[i];
        if (p->removed) continue;
        if (n >= WS_MAX) break;
        g_workspaces[n].active = (p->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0;
        snprintf(g_workspaces[n].name, sizeof(g_workspaces[n].name), "%s", p->name);
        g_workspaces[n].handle = p->handle;
        n++;
    }
    g_ws.ws_count = n;
    render_request();
}

static void
workspace_switch_to(int row)
{
    if (row < 0 || row >= g_ws.ws_count) return;
    if (g_wl.ext_ws_mgr && g_workspaces[row].handle) {
        ext_workspace_handle_v1_activate(g_workspaces[row].handle);
        ext_workspace_manager_v1_commit(g_wl.ext_ws_mgr);
    }
    for (int i = 0; i < g_ws.ws_count; i++)
        g_workspaces[i].active = (i == row);
    render_request();
}

static void
ws_handle_id(void *data, struct ext_workspace_handle_v1 *h, const char *id)
{ (void)data; (void)h; (void)id; }

static void
ws_handle_name(void *data, struct ext_workspace_handle_v1 *h, const char *name)
{
    struct ws_proxy *p = data;
    (void)h;
    snprintf(p->name, sizeof(p->name), "%s", name);
}

static void
ws_handle_coordinates(void *data, struct ext_workspace_handle_v1 *h,
    struct wl_array *coordinates)
{ (void)data; (void)h; (void)coordinates; }

static void
ws_handle_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t state)
{
    struct ws_proxy *p = data;
    (void)h;
    p->state = state;
}

static void
ws_handle_capabilities(void *data, struct ext_workspace_handle_v1 *h,
    uint32_t caps)
{ (void)data; (void)h; (void)caps; }

static void
ws_handle_removed(void *data, struct ext_workspace_handle_v1 *h)
{
    struct ws_proxy *p = data;
    (void)h;
    p->removed = 1;
}

static const struct ext_workspace_handle_v1_listener ws_handle_listener = {
    .id = ws_handle_id,
    .name = ws_handle_name,
    .coordinates = ws_handle_coordinates,
    .state = ws_handle_state,
    .capabilities = ws_handle_capabilities,
    .removed = ws_handle_removed,
};

static void
ws_mgr_workspace_group(void *data, struct ext_workspace_manager_v1 *m,
    struct ext_workspace_group_handle_v1 *group)
{ (void)data; (void)m; (void)group; }

static void
ws_mgr_workspace(void *data, struct ext_workspace_manager_v1 *m,
    struct ext_workspace_handle_v1 *handle)
{
    (void)data; (void)m;
    if (g_ws.ws_proxies_n >= WS_MAX) return;
    struct ws_proxy *p = &g_ws_proxies[g_ws.ws_proxies_n++];
    p->handle = handle;
    p->state = 0;
    p->removed = 0;
    ext_workspace_handle_v1_add_listener(handle, &ws_handle_listener, p);
}

static void
ws_mgr_done(void *data, struct ext_workspace_manager_v1 *m)
{
    (void)data; (void)m;
    workspaces_sync_from_proxies();
}

static void
ws_mgr_finished(void *data, struct ext_workspace_manager_v1 *m)
{
    (void)data;
    if (g_wl.ext_ws_mgr == m) g_wl.ext_ws_mgr = NULL;
    ext_workspace_manager_v1_destroy(m);
    g_ws.ws_proxies_n = 0;
    workspaces_placeholder_init();
}

static const struct ext_workspace_manager_v1_listener ws_mgr_listener = {
    .workspace_group = ws_mgr_workspace_group,
    .workspace = ws_mgr_workspace,
    .done = ws_mgr_done,
    .finished = ws_mgr_finished,
};

struct ws_popup {
    int open;
    double x, y, w, h;
    int hover_row;
};

static struct ws_popup g_wspop = { 0 };

static int
wspop_rows(void)
{
    return g_ws.ws_count > 0 ? g_ws.ws_count : 1;
}

static double
wspop_width(void)
{
    int n = wspop_rows();
    double ws_w = MENU_PADDING * 2 + n * WS_BTN_SIZE + (n - 1) * WS_BTN_GAP;
    double wp_w = MENU_PADDING * 2 + 8 + WS_WP_ICON + 8 + 80;
    return ws_w > wp_w ? ws_w : wp_w;
}

static int
wspop_height(void)
{
    return MENU_FLOAT_GAP + MENU_PADDING + WS_WP_BTN_H + MENU_PADDING +
           WS_BTN_SIZE + MENU_PADDING;
}

static void
wspop_open(void)
{
    g_wspop.open = 1;
    g_wspop.hover_row = -1;

    double pw = wspop_width();
    double body_h = MENU_PADDING + WS_WP_BTN_H + MENU_PADDING +
                    WS_BTN_SIZE + MENU_PADDING;
    double icon_center = workspace_icon_x() + WS_ICON_SIZE / 2.0;
    double px = icon_center - pw / 2.0;
    if (px < 0) px = 0;
    if (px + pw > g_bar.width) px = g_bar.width - pw;
    if (px < 0) px = 0;

    g_wspop.x = px;
    g_wspop.y = 0;
    g_wspop.w = pw;
    g_wspop.h = body_h;

    popup_size_changed();
}

static void
wspop_close(void)
{
    if (!g_wspop.open) return;
    g_wspop.open = 0;
    g_wspop.hover_row = -1;
    popup_size_changed();
}

static int
wspop_hit_test(double px, double py)
{
    if (!g_wspop.open) return -1;

    /* Wallpaper button row (returned as -2) */
    double wp_y = g_wspop.y + MENU_PADDING;
    if (in_rect(px, py, g_wspop.x, wp_y, g_wspop.w, WS_WP_BTN_H))
        return -2;

    /* Workspace button row */
    double btn_y = wp_y + WS_WP_BTN_H + MENU_PADDING;
    if (!in_rect(px, py, g_wspop.x, btn_y, g_wspop.w, WS_BTN_SIZE)) return -1;
    double step = WS_BTN_SIZE + WS_BTN_GAP;
    double rel = px - g_wspop.x - MENU_PADDING;
    int col = (int)(rel / step);
    if (col < 0 || col >= g_ws.ws_count) return -1;
    if (rel - col * step >= WS_BTN_SIZE) return -1;
    return col;
}

/* ---------------------------------------------------------------------------
 * Calendar popup
 * ------------------------------------------------------------------------- */

struct cal_popup {
    int open;
    double x, y;
    double w, h;
    int view_year;
    int view_mon;
    int hover_prev;
    int hover_next;
};

static struct cal_popup g_calpop = { 0 };

static PangoFontDescription *
cal_day_font(void)
{
    static PangoFontDescription *fd = NULL;
    if (!fd) {
        fd = pango_font_description_from_string(FONT_FAMILY);
        pango_font_description_set_absolute_size(fd, SYS_FONT_SIZE * PANGO_SCALE);
    }
    return fd;
}

static PangoFontDescription *
cal_small_font(void)
{
    static PangoFontDescription *fd = NULL;
    if (!fd) {
        fd = pango_font_description_from_string(FONT_FAMILY);
        pango_font_description_set_absolute_size(fd, SYS_FONT_SMALL * PANGO_SCALE);
    }
    return fd;
}

static int
cal_days_in_month(int year, int mon)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = mon + 1, .tm_mday = 0 };
    mktime(&tm);
    return tm.tm_mday;
}

static int
cal_first_weekday(int year, int mon)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = mon, .tm_mday = 1 };
    mktime(&tm);
    return tm.tm_wday;
}

static int
calpop_height(void)
{
    return MENU_FLOAT_GAP + MENU_PADDING * 2 +
           CAL_HEADER_H + CAL_DOW_H + 6 * CAL_CELL_H;
}

static void
calpop_open(void)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    g_calpop.open = 1;
    g_calpop.hover_prev = 0;
    g_calpop.hover_next = 0;
    g_calpop.view_year = tm.tm_year + 1900;
    g_calpop.view_mon = tm.tm_mon;

    double pw = CAL_WIDTH;
    double ph = calpop_height();
    double bell_left = g_bar.width - TRAY_EDGE_PAD - NOTIF_ICON_SIZE;
    double clock_cx = bell_left - TRAY_GAP - time_text_width() / 2.0;
    double px = clock_cx - pw / 2.0;
    if (px + pw > g_bar.width - TRAY_EDGE_PAD)
        px = g_bar.width - TRAY_EDGE_PAD - pw;
    if (px < 0) px = 0;

    g_calpop.x = px;
    g_calpop.y = 0;
    g_calpop.w = pw;
    g_calpop.h = ph;
    popup_size_changed();
}

static void
calpop_close(void)
{
    if (!g_calpop.open) return;
    g_calpop.open = 0;
    g_calpop.hover_prev = 0;
    g_calpop.hover_next = 0;
    popup_size_changed();
}

static void
calpop_step(int dir)
{
    g_calpop.view_mon += dir;
    if (g_calpop.view_mon < 0) {
        g_calpop.view_mon = 11;
        g_calpop.view_year--;
    } else if (g_calpop.view_mon > 11) {
        g_calpop.view_mon = 0;
        g_calpop.view_year++;
    }
    render_request();
}

static int
cal_arrow_hit(double px, double py, int side)
{
    if (!g_calpop.open) return 0;
    double ax = side < 0
        ? g_calpop.x + MENU_PADDING
        : g_calpop.x + g_calpop.w - MENU_PADDING - CAL_ARROW_W;
    return in_rect(px, py, ax, g_calpop.y + MENU_PADDING,
                   CAL_ARROW_W, CAL_HEADER_H);
}

static int
calprev_hit_test(double px, double py)
{
    return cal_arrow_hit(px, py, -1);
}

static int
calnext_hit_test(double px, double py)
{
    return cal_arrow_hit(px, py, 1);
}

/* ---------------------------------------------------------------------------
 * Notifications
 * ------------------------------------------------------------------------- */

struct notif_entry {
    uint32_t id;
    int64_t ts_ms;
    char app_name[64];
    char app_id[128];
    char summary[256];
    char body[512];
    int unread;
};

static struct notif_entry g_notifs[NOTIF_MAX];
static int g_n_notifs;
static uint32_t g_next_notif_id;

struct notif_popup {
    int open;
    double x, y;
    double w, h;
    double list_y, list_h;
    double rows_x, rows_w;
    double text_x, text_w;
    double time_right;
    double time_left;
    double trash_cx;
    double clear_x, clear_y, clear_w, clear_h;
    double max_scroll, scroll;
    double target_scroll;
    GSource *anim_src;
    int hover_row;
    int hover_trash;
    int hover_clear;
};

static struct notif_popup g_npop = { 0 };
static PangoFontDescription *g_notif_fd;

/* ---------------------------------------------------------------------------
 * Wallpaper browser popup
 * ------------------------------------------------------------------------- */

#define WLP_MAX 512

struct wall_entry {
    char path[512];
    char name[64];
    cairo_surface_t *thumb;
};

struct wall_entry_staging {
    GdkPixbuf *pb;
};

struct wall_popup {
    int open;
    double x, y, w, h;
    double scroll;
    double max_scroll;
    double anim_px;
    double target_px;
    GSource *anim_src;
    int hover_idx;
    int enabled;
    int hover_power;
    char cur_wallpaper[512];
    struct wall_entry entries[WLP_MAX];
    struct wall_entry_staging staging[WLP_MAX];
    int n_entries;
    _Atomic int loaded_count;
    GThread *loader;
};

static struct wall_popup g_wlp = { 0 };

static void rounded_rect(cairo_t *cr, double x, double y, double w,
                         double h, double r);
static void draw_hover_circle(cairo_t *cr, double cx, double cy,
                              double icon_sz, double pad);
static PangoFontDescription *notif_font(void);

static int
notif_height(void)
{
    return (int)g_npop.h;
}

static int
notif_unread_count(void)
{
    int n = 0;
    for (int i = 0; i < g_n_notifs; i++)
        if (g_notifs[i].unread) n++;
    return n;
}

static void
notif_store_save(void)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(uxssssb)"));
    for (int i = 0; i < g_n_notifs; i++) {
        struct notif_entry *e = &g_notifs[i];
        g_variant_builder_add(&b, "(uxssssb)", e->id, e->ts_ms,
            e->app_name, e->app_id, e->summary, e->body, e->unread != 0);
    }
    GVariant *v = g_variant_builder_end(&b);
    char *text = g_variant_print(v, FALSE);
    char *path = g_build_filename(g_get_user_state_dir(),
                                  "jtlab-notifications.txt", NULL);
    GError *err = NULL;
    if (!g_file_set_contents(path, text, -1, &err))
        g_clear_error(&err);
    g_free(path);
    g_free(text);
    g_variant_unref(v);
}

static void
notif_store_load(void)
{
    char *path = g_build_filename(g_get_user_state_dir(),
                                  "jtlab-notifications.txt", NULL);
    char *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        GError *err = NULL;
        GVariant *v = g_variant_parse(G_VARIANT_TYPE("a(uxssssb)"), text,
                                      NULL, NULL, &err);
        if (v) {
            GVariantIter iter;
            g_variant_iter_init(&iter, v);
            uint32_t id;
            int64_t ts;
            const char *an, *ai, *sum, *bod;
            gboolean un;
            while (g_variant_iter_next(&iter, "(uxssssb)", &id, &ts,
                                       &an, &ai, &sum, &bod, &un) &&
                   g_n_notifs < NOTIF_MAX) {
                struct notif_entry *e = &g_notifs[g_n_notifs++];
                e->id = id;
                e->ts_ms = ts;
                g_strlcpy(e->app_name, an, sizeof(e->app_name));
                g_strlcpy(e->app_id, ai, sizeof(e->app_id));
                g_strlcpy(e->summary, sum, sizeof(e->summary));
                g_strlcpy(e->body, bod, sizeof(e->body));
                e->unread = un ? 1 : 0;
                if (id >= g_next_notif_id) g_next_notif_id = id + 1;
            }
            g_variant_unref(v);
        }
        g_clear_error(&err);
        g_free(text);
    }
    g_free(path);
}

static PangoFontDescription *
clock_font(void)
{
    static PangoFontDescription *fd = NULL;
    if (!fd) {
        fd = pango_font_description_from_string(FONT_FAMILY);
        pango_font_description_set_absolute_size(fd, FONT_SIZE * PANGO_SCALE);
    }
    return fd;
}

static double
notif_time_text_w(void)
{
    static double w = -1;
    if (w < 0) {
        cairo_surface_t *surf =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr = cairo_create(surf);
        PangoContext *ctx = pango_cairo_create_context(cr);
        PangoLayout *tl = pango_layout_new(ctx);
        pango_layout_set_font_description(tl, clock_font());
        pango_layout_set_text(tl, "00:00 AM", -1);
        int tw, th;
        pango_layout_get_pixel_size(tl, &tw, &th);
        w = tw;
        g_object_unref(tl);
        g_object_unref(ctx);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    }
    return w;
}

static double
notif_clear_text_w(void)
{
    static double w = -1;
    if (w < 0) {
        cairo_surface_t *surf =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *cr = cairo_create(surf);
        PangoContext *ctx = pango_cairo_create_context(cr);
        PangoLayout *tl = pango_layout_new(ctx);
        pango_layout_set_font_description(tl, clock_font());
        pango_layout_set_text(tl, "Clear all", -1);
        int tw, th;
        pango_layout_get_pixel_size(tl, &tw, &th);
        w = tw;
        g_object_unref(tl);
        g_object_unref(ctx);
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    }
    return w;
}

static void
notifpop_recalc_layout(void)
{
    double pw = NOTIF_POPUP_WIDTH;
    double px = g_bar.width - TRAY_EDGE_PAD - pw;
    g_npop.x = px;
    g_npop.y = 0;
    g_npop.w = pw;
    g_npop.list_y = NOTIF_HEADER_H + 1 + 4;
    g_npop.list_h = NOTIF_VISIBLE_ROWS * NOTIF_ROW_H;
    g_npop.h = g_npop.list_y + g_npop.list_h + NOTIF_LIST_BOTTOM_PAD +
               MENU_FLOAT_GAP;
    g_npop.max_scroll = fmax(0.0, g_n_notifs * NOTIF_ROW_H - g_npop.list_h);
    if (g_npop.scroll > g_npop.max_scroll) g_npop.scroll = g_npop.max_scroll;
    if (g_npop.scroll < 0) g_npop.scroll = 0;
    if (g_npop.target_scroll > g_npop.max_scroll)
        g_npop.target_scroll = g_npop.max_scroll;
    if (g_npop.target_scroll < 0) g_npop.target_scroll = 0;

    g_npop.rows_x = px + MENU_PADDING;
    g_npop.rows_w = (px + pw - MENU_PADDING) - g_npop.rows_x;
    g_npop.text_x = g_npop.rows_x + NOTIF_BOX_PAD;
    g_npop.trash_cx = g_npop.rows_x + g_npop.rows_w - NOTIF_TRASH_ICON / 2.0 - 6;
    g_npop.time_right = g_npop.trash_cx - NOTIF_TRASH_ICON / 2.0 - 8;
    g_npop.time_left = g_npop.time_right - notif_time_text_w();
    g_npop.text_w = (g_npop.time_left - 8) - g_npop.text_x;

    g_npop.clear_w = notif_clear_text_w() + 2 * NOTIF_CLEAR_PAD_X;
    g_npop.clear_h = NOTIF_CLEAR_H;
    g_npop.clear_x = px + pw - MENU_PADDING - g_npop.clear_w;
    g_npop.clear_y = (NOTIF_HEADER_H - g_npop.clear_h) / 2.0;
}

static void
notifpop_open(void)
{
    g_npop.open = 1;
    for (int i = 0; i < g_n_notifs; i++)
        g_notifs[i].unread = 0;
    notif_store_save();
    notifpop_recalc_layout();
    popup_size_changed();
}

static struct scroll_anim notif_anim;

static void
notifpop_close(void)
{
    if (!g_npop.open) return;
    scroll_anim_cancel(&notif_anim);
    g_npop.scroll = g_npop.target_scroll;
    g_npop.open = 0;
    g_npop.hover_row = -1;
    g_npop.hover_trash = 0;
    g_npop.hover_clear = 0;
    popup_size_changed();
}

static int
notifpop_hit_test(double px, double py)
{
    if (!g_npop.open) return 0;
    return in_rect(px, py, g_npop.x, g_npop.y, g_npop.w, g_npop.h);
}

static int
notifpop_row_hit_test(double px, double py)
{
    if (!g_npop.open) return -1;
    if (px < g_npop.rows_x || px >= g_npop.rows_x + g_npop.rows_w) return -1;
    if (py < g_npop.list_y || py >= g_npop.list_y + g_npop.list_h) return -1;
    int row = (int)((py - g_npop.list_y + g_npop.scroll) / NOTIF_ROW_H);
    if (row < 0 || row >= g_n_notifs) return -1;
    return row;
}

static int
notifpop_trash_hit_test(double px, double py)
{
    if (!g_npop.open) return 0;
    int row = notifpop_row_hit_test(px, py);
    if (row < 0) return 0;
    double tcy = g_npop.list_y + row * NOTIF_ROW_H - lround(g_npop.scroll) +
                 NOTIF_ROW_H / 2.0;
    return in_rect(px, py, g_npop.trash_cx - NOTIF_TRASH_ICON / 2.0 - 5,
                   tcy - NOTIF_TRASH_ICON / 2.0 - 5,
                   NOTIF_TRASH_ICON + 10, NOTIF_TRASH_ICON + 10);
}

static int
notifpop_clear_hit_test(double px, double py)
{
    if (!g_npop.open) return 0;
    return in_rect(px, py, g_npop.clear_x, g_npop.clear_y,
                   g_npop.clear_w, g_npop.clear_h);
}

static int
notif_anim_wants_render(void)
{
    return g_npop.open;
}

static struct scroll_anim notif_anim = {
    .current  = &g_npop.scroll,
    .target   = &g_npop.target_scroll,
    .src      = &g_npop.anim_src,
    .wants_render = notif_anim_wants_render,
};

static void
notifpop_anim_start(void)
{
    scroll_anim_start(&notif_anim);
}

static void
notifpop_scroll(double delta)
{
    double old = g_npop.target_scroll;
    g_npop.target_scroll += delta;
    if (g_npop.target_scroll < 0) g_npop.target_scroll = 0;
    if (g_npop.target_scroll > g_npop.max_scroll)
        g_npop.target_scroll = g_npop.max_scroll;
    if (g_npop.target_scroll == old) return;
    notifpop_anim_start();
    render_request();
}

static void
notif_remove_idx(int idx, uint32_t reason)
{
    if (idx < 0 || idx >= g_n_notifs) return;
    uint32_t id = g_notifs[idx].id;
    memmove(&g_notifs[idx], &g_notifs[idx + 1],
            (g_n_notifs - idx - 1) * sizeof(struct notif_entry));
    g_n_notifs--;
    if (g_sni.dbus_conn) {
        g_dbus_connection_emit_signal(g_sni.dbus_conn, NULL,
            "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
            "NotificationClosed", g_variant_new("(uu)", id, reason), NULL);
        g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
    }
    notif_store_save();
    if (g_npop.open)
        notifpop_recalc_layout();
    render_request();
}

static void
notif_remove_by_id(uint32_t id, uint32_t reason)
{
    for (int i = 0; i < g_n_notifs; i++) {
        if (g_notifs[i].id == id) {
            notif_remove_idx(i, reason);
            return;
        }
    }
}

static void
notif_clear_all(void)
{
    if (g_n_notifs == 0) return;
    if (g_sni.dbus_conn) {
        for (int i = 0; i < g_n_notifs; i++) {
            g_dbus_connection_emit_signal(g_sni.dbus_conn, NULL,
                "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                "NotificationClosed", g_variant_new("(uu)", g_notifs[i].id, 2), NULL);
        }
        g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
    }
    g_n_notifs = 0;
    notif_store_save();
    if (g_npop.open) {
        g_npop.scroll = 0;
        g_npop.target_scroll = 0;
        notifpop_recalc_layout();
    }
    render_request();
}

static uint32_t
notif_add(const char *app_name, const char *app_id, const char *summary,
          const char *body, uint32_t replaces_id)
{
    int idx = -1;
    for (int i = 0; i < g_n_notifs; i++) {
        if (g_notifs[i].id == replaces_id) { idx = i; break; }
    }

    struct notif_entry *e;
    if (idx >= 0) {
        e = &g_notifs[idx];
    } else {
        if (g_n_notifs >= NOTIF_MAX)
            notif_remove_idx(0, 3);
        idx = g_n_notifs++;
        e = &g_notifs[idx];
        do {
            g_next_notif_id = (g_next_notif_id % 0xFFFFFFFE) + 1;
        } while (g_next_notif_id == 0);
        e->id = g_next_notif_id;
    }
    e->ts_ms = (int64_t)time(NULL) * 1000;
    e->unread = g_npop.open ? 0 : 1;
    g_strlcpy(e->app_name, app_name ? app_name : "", sizeof(e->app_name));
    g_strlcpy(e->app_id, app_id ? app_id : "", sizeof(e->app_id));
    g_strlcpy(e->summary, summary ? summary : "", sizeof(e->summary));
    g_strlcpy(e->body, body ? body : "", sizeof(e->body));
    notif_store_save();
    if (g_npop.open)
        notifpop_recalc_layout();
    render_request();
    return e->id;
}

static int
notif_activate(int idx)
{
    struct notif_entry *e = &g_notifs[idx];
    int app_idx = -1;
    if (e->app_id[0]) {
        app_idx = app_index_by_app_id(e->app_id);
        if (app_idx < 0 && e->app_name[0]) {
            for (int i = 0; i < g_app.n_apps; i++) {
                if (g_strcmp0(g_app.apps[i].name, e->app_name) == 0) {
                    app_idx = i;
                    break;
                }
            }
        }
    }
    if (app_idx < 0) return 0;
    int wins[MAX_WINDOWS_PER_GROUP];
    int count = windows_of_app(&g_app.apps[app_idx], wins,
                               MAX_WINDOWS_PER_GROUP);
    if (count > 0) {
        if (g_bar.menu_open) menu_close(&g_bar);
        zwlr_foreign_toplevel_handle_v1_activate(g_toplevels[wins[0]].handle,
                                                 g_wl.seat);
    } else {
        run_cmd(g_app.apps[app_idx].exec, g_app.apps[app_idx].terminal);
    }
    return 1;
}

static int
notifpop_hover_refresh(int on_bar)
{
    int old_row = g_npop.hover_row;
    int old_trash = g_npop.hover_trash;
    int old_clear = g_npop.hover_clear;
    if (g_npop.open) {
        g_npop.hover_row = on_bar
            ? notifpop_row_hit_test(g_pointer.x, g_pointer.y) : -1;
        g_npop.hover_trash = on_bar && g_npop.hover_row >= 0
            ? notifpop_trash_hit_test(g_pointer.x, g_pointer.y) : 0;
        g_npop.hover_clear = on_bar
            ? notifpop_clear_hit_test(g_pointer.x, g_pointer.y) : 0;
    } else {
        g_npop.hover_row = -1;
        g_npop.hover_trash = 0;
        g_npop.hover_clear = 0;
    }
    return old_row != g_npop.hover_row || old_trash != g_npop.hover_trash ||
           old_clear != g_npop.hover_clear;
}

static PangoFontDescription *
notif_font(void)
{
    if (!g_notif_fd) {
        g_notif_fd = pango_font_description_from_string(FONT_FAMILY);
        pango_font_description_set_size(g_notif_fd,
                                        (int)(NOTIF_FONT_SIZE * PANGO_SCALE));
    }
    return g_notif_fd;
}

/* --- Wallpaper browser popup --------------------------------------------- */

static void
wlp_free_thumbs(void)
{
    for (int i = 0; i < g_wlp.n_entries; i++) {
        if (g_wlp.entries[i].thumb)
            cairo_surface_destroy(g_wlp.entries[i].thumb);
        if (g_wlp.staging[i].pb)
            g_object_unref(g_wlp.staging[i].pb);
        g_wlp.entries[i].thumb = NULL;
        g_wlp.staging[i].pb = NULL;
    }
    g_wlp.n_entries = 0;
    atomic_store(&g_wlp.loaded_count, 0);
}

static int
wlp_cmp_mtime(const void *a, const void *b)
{
    const struct wall_entry *ea = a, *eb = b;
    struct stat sa, sb;
    stat(ea->path, &sa);
    stat(eb->path, &sb);
    if (sb.st_mtime > sa.st_mtime) return 1;
    if (sb.st_mtime < sa.st_mtime) return -1;
    return 0;
}

static void
wlp_scan(void)
{
    if (g_wlp.n_entries > 0) return;

    DIR *dir = opendir(WALLPAPER_DIR);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_wlp.n_entries < WLP_MAX) {
        if (entry->d_type != DT_REG) continue;
        const char *n = entry->d_name;
        int l = strlen(n);
        if (l < 5) continue;
        const char *dot = strrchr(n, '.');
        if (!dot || (strcmp(dot, ".jpg") != 0 && strcmp(dot, ".png") != 0 &&
            strcmp(dot, ".jpeg") != 0 && strcmp(dot, ".webp") != 0))
            continue;

        struct wall_entry *we = &g_wlp.entries[g_wlp.n_entries];
        snprintf(we->path, sizeof(we->path), "%s%s", WALLPAPER_DIR, n);
        snprintf(we->name, sizeof(we->name), "%.*s", (int)(l > 20 ? 17 : l), n);
        if (l > 20) strcat(we->name, "...");
        we->thumb = NULL;
        g_wlp.staging[g_wlp.n_entries].pb = NULL;
        g_wlp.n_entries++;
    }
    closedir(dir);

    qsort(g_wlp.entries, g_wlp.n_entries, sizeof(struct wall_entry), wlp_cmp_mtime);
}

static void
wlp_cache_path(const char *name, char *out, size_t sz)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, sz, "%s/.cache/wallpaper-browser/%s.png", home, name);
}

static gpointer
wlp_loader_thread(gpointer data)
{
    (void)data;
    char cache_dir[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/wallpaper-browser", home);
    g_mkdir_with_parents(cache_dir, 0755);

    int n = g_wlp.n_entries;
    for (int i = 0; i < n; i++) {
        struct wall_entry *we = &g_wlp.entries[i];
        char cpath[512];
        wlp_cache_path(we->name, cpath, sizeof(cpath));

        struct stat st_src, st_cache;
        int src_ok = stat(we->path, &st_src) == 0;
        int cache_ok = stat(cpath, &st_cache) == 0 &&
                       st_cache.st_mtime >= st_src.st_mtime;

        GdkPixbuf *pb = NULL;
        if (cache_ok)
            pb = gdk_pixbuf_new_from_file(cpath, NULL);
        if (!pb)
            pb = gdk_pixbuf_new_from_file_at_size(we->path,
                     WALL_THUMB_W, WALL_THUMB_H, NULL);
        if (pb) {
            if (!cache_ok && src_ok)
                gdk_pixbuf_save(pb, cpath, "png", NULL, NULL);
            g_wlp.staging[i].pb = pb;
        }
        atomic_fetch_add(&g_wlp.loaded_count, 1);
    }
    return NULL;
}

static int
wlp_rows(void)
{
    if (g_wlp.n_entries == 0) return 0;
    return (g_wlp.n_entries + WALL_THUMB_COLS - 1) / WALL_THUMB_COLS;
}

static int
wlp_width(void)
{
    return 2 * MENU_PADDING +
           WALL_THUMB_COLS * WALL_THUMB_W +
           (WALL_THUMB_COLS - 1) * WALL_THUMB_GAP;
}

static int
wlp_visible_h(void)
{
    return WALL_HEADER_H + MENU_PADDING +
           WALL_VISIBLE_ROWS * (WALL_THUMB_H + WALL_LABEL_H + WALL_THUMB_GAP);
}

static int
wlp_height(void)
{
    int rows = wlp_rows();
    if (rows >= WALL_VISIBLE_ROWS)
        return MENU_FLOAT_GAP + POPUP_BODY_H;
    int content = WALL_HEADER_H + MENU_PADDING +
                  rows * (WALL_THUMB_H + WALL_LABEL_H + WALL_THUMB_GAP) +
                  MENU_PADDING + MENU_FLOAT_GAP;
    return content;
}

static void
wlp_open(void)
{
    g_wlp.open = 1;
    g_wlp.hover_idx = -1;
    g_wlp.hover_power = 0;
    g_wlp.scroll = 0;
    g_wlp.anim_px = 0;
    g_wlp.target_px = 0;
    g_wlp.loader = NULL;
    atomic_store(&g_wlp.loaded_count, 0);
    wlp_scan();

    double pw = wlp_width();
    double ph = wlp_height();
    double px = (g_bar.width - pw) / 2.0;
    if (px < 0) px = 0;

    g_wlp.x = px;
    g_wlp.y = 0;
    g_wlp.w = pw;
    g_wlp.h = ph;

    int rows = wlp_rows();
    int content = WALL_HEADER_H + MENU_PADDING +
                  rows * (WALL_THUMB_H + WALL_LABEL_H + WALL_THUMB_GAP);
    int vis = wlp_visible_h();
    g_wlp.max_scroll = content > vis ? content - vis : 0;

    if (g_wlp.n_entries > 0)
        g_wlp.loader = g_thread_new("wlp-load", wlp_loader_thread, NULL);

    popup_size_changed();
}

static void
wlp_anim_on_frame(void)
{
    g_wlp.scroll = g_wlp.anim_px;
}

static struct scroll_anim wlp_anim = {
    .current  = &g_wlp.anim_px,
    .target   = &g_wlp.target_px,
    .src      = &g_wlp.anim_src,
    .on_frame = wlp_anim_on_frame,
};

static void
wlp_scroll_anim_start(void)
{
    scroll_anim_start(&wlp_anim);
}

static void
wlp_scroll_anim_cancel(void)
{
    scroll_anim_cancel(&wlp_anim);
}

static void
wlp_scroll(double delta)
{
    double target = g_wlp.target_px + delta;
    if (target < 0) target = 0;
    if (target > g_wlp.max_scroll) target = g_wlp.max_scroll;
    g_wlp.target_px = target;
    wlp_scroll_anim_start();
}

static void
wlp_close(void)
{
    if (!g_wlp.open) return;
    g_wlp.open = 0;
    g_wlp.hover_idx = -1;
    wlp_scroll_anim_cancel();
    if (g_wlp.loader) {
        g_thread_join(g_wlp.loader);
        g_wlp.loader = NULL;
    }
    wlp_free_thumbs();
    popup_size_changed();
}

static int
wlp_hit_test(double px, double py)
{
    if (!g_wlp.open) return -1;
    double body_h = g_wlp.h - MENU_FLOAT_GAP;
    if (!in_rect(px, py, g_wlp.x, g_wlp.y, g_wlp.w, body_h)) return -1;

    double gy = g_wlp.y + WALL_HEADER_H + MENU_PADDING - g_wlp.scroll;
    double gx = g_wlp.x + MENU_PADDING;
    int step = WALL_THUMB_H + WALL_LABEL_H + WALL_THUMB_GAP;

    for (int i = 0; i < g_wlp.n_entries; i++) {
        int col = i % WALL_THUMB_COLS;
        int row = i / WALL_THUMB_COLS;
        double cx = gx + col * (WALL_THUMB_W + WALL_THUMB_GAP);
        double cy = gy + row * step;
        if (in_rect(px, py, cx, cy, WALL_THUMB_W, WALL_THUMB_H + WALL_LABEL_H))
            return i;
    }
    return -1;
}

static char *
wlp_state_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.cache/wallpaper-browser/current.txt", home);
    return path;
}

static void
wlp_save_wallpaper(const char *path)
{
    const char *sp = wlp_state_path();
    FILE *f = fopen(sp, "w");
    if (f) { fprintf(f, "%s\n", path); fclose(f); }
}

static void
wlp_set_wallpaper(const char *path)
{
    /* Kill old wbg */
    DIR *proc = opendir("/proc");
    if (proc) {
        struct dirent *de;
        char ppath[512], comm[64];
        while ((de = readdir(proc)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
            snprintf(ppath, sizeof(ppath), "/proc/%s/comm", de->d_name);
            int fd = open(ppath, O_RDONLY);
            if (fd < 0) continue;
            ssize_t n = read(fd, comm, 63);
            close(fd);
            if (n > 0) {
                comm[n] = '\0';
                char *nl = strchr(comm, '\n');
                if (nl) *nl = '\0';
                if (strcmp(comm, "wbg") == 0)
                    kill((pid_t)atoi(de->d_name), SIGTERM);
            }
        }
        closedir(proc);
    }

    /* Launch new wbg */
    char wbg_cmd[600];
    snprintf(wbg_cmd, sizeof(wbg_cmd), "wbg -s '%s'", path);
    run_cmd(wbg_cmd, 0);

    /* Track active wallpaper */
    strncpy(g_wlp.cur_wallpaper, path, sizeof(g_wlp.cur_wallpaper) - 1);
    g_wlp.cur_wallpaper[sizeof(g_wlp.cur_wallpaper) - 1] = '\0';
}

static void
wlp_set_wallpaper_user(const char *path)
{
    g_wlp.enabled = 1;
    wlp_set_wallpaper(path);
    wlp_save_wallpaper(path);

    char notify_cmd[600];
    snprintf(notify_cmd, sizeof(notify_cmd),
             "notify-send 'Wallpaper Changed' '%s'", path);
    run_cmd(notify_cmd, 0);
}

static void
wlp_load_wallpaper(void)
{
    FILE *f = fopen(wlp_state_path(), "r");
    if (!f) return;
    char line[512];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] && access(line, R_OK) == 0) {
            g_wlp.enabled = 1;
            wlp_set_wallpaper(line);
        }
    }
    fclose(f);
}

#define WLP_TOGGLE_W 34
#define WLP_TOGGLE_H 18

static void
wlp_toggle_rect(double *rx, double *ry)
{
    *rx = g_wlp.x + g_wlp.w - MENU_PADDING - WLP_TOGGLE_W;
    *ry = g_wlp.y + (WALL_HEADER_H - WLP_TOGGLE_H) / 2.0;
}

static int
wlp_toggle_hit_test(double px, double py)
{
    if (!g_wlp.open) return 0;
    double rx, ry;
    wlp_toggle_rect(&rx, &ry);
    return in_rect(px, py, rx, ry, WLP_TOGGLE_W, WLP_TOGGLE_H);
}

static void
wlp_kill_wbg(void)
{
    DIR *proc = opendir("/proc");
    if (!proc) return;
    struct dirent *de;
    char ppath[512], comm[64];
    while ((de = readdir(proc)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(ppath, sizeof(ppath), "/proc/%s/comm", de->d_name);
        int fd = open(ppath, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, comm, 63);
        close(fd);
        if (n > 0) {
            comm[n] = '\0';
            char *nl = strchr(comm, '\n');
            if (nl) *nl = '\0';
            if (strcmp(comm, "wbg") == 0)
                kill((pid_t)atoi(de->d_name), SIGTERM);
        }
    }
    closedir(proc);
}

static void
wlp_toggle_power(void)
{
    g_wlp.enabled = !g_wlp.enabled;
    if (g_wlp.enabled) {
        if (g_wlp.cur_wallpaper[0])
            wlp_set_wallpaper(g_wlp.cur_wallpaper);
        run_cmd("notify-send 'Wallpaper' 'Enabled'", 0);
    } else {
        wlp_kill_wbg();
        run_cmd("notify-send 'Wallpaper' 'Disabled'", 0);
    }
    render_request();
}

static void
wlp_draw_power_toggle(cairo_t *cr)
{
    double rx, ry;
    wlp_toggle_rect(&rx, &ry);

    int on = g_wlp.enabled;
    int hover = g_wlp.hover_power;

    cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 1.0);
    cairo_set_line_width(cr, 1);
    cairo_new_path(cr);
    rounded_rect(cr, rx, ry, WLP_TOGGLE_W, WLP_TOGGLE_H, WLP_TOGGLE_H / 2.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, on ? 0.25 : 0.12, on ? 0.5 : 0.2, on ? 0.4 : 0.2,
                          hover ? 1.0 : 0.9);
    cairo_new_path(cr);
    rounded_rect(cr, rx + 1, ry + 1, WLP_TOGGLE_W - 2, WLP_TOGGLE_H - 2,
                 (WLP_TOGGLE_H - 2) / 2.0);
    cairo_fill(cr);

    double knob_d = WLP_TOGGLE_H - 6;
    double kx = on ? rx + WLP_TOGGLE_W - knob_d - 3 : rx + 3;
    double ky = ry + 3;
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hover ? 1.0 : 0.85);
    cairo_new_path(cr);
    cairo_arc(cr, kx + knob_d / 2.0, ky + knob_d / 2.0, knob_d / 2.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

static void
wlp_draw(cairo_t *cr, PangoLayout *pl)
{
    if (!g_wlp.open) return;

    int loaded = atomic_load(&g_wlp.loaded_count);
    int need_render = 0;
    for (int i = 0; i < g_wlp.n_entries; i++) {
        if (!g_wlp.entries[i].thumb && g_wlp.staging[i].pb) {
            g_wlp.entries[i].thumb = surface_from_pixbuf(g_wlp.staging[i].pb);
            g_object_unref(g_wlp.staging[i].pb);
            g_wlp.staging[i].pb = NULL;
            need_render = 1;
        }
    }
    if (need_render || loaded < g_wlp.n_entries) render_request();

    double px = g_wlp.x;
    double py = g_wlp.y;
    double pw = g_wlp.w;
    double body_h = g_wlp.h - MENU_FLOAT_GAP;
    double r = MENU_CORNER_R;

    panel_begin(cr, px, py, pw, body_h, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

    /* Header */
    {
        PangoFontDescription *hfd = pango_font_description_from_string(FONT_FAMILY);
        pango_font_description_set_absolute_size(hfd, FONT_SIZE * PANGO_SCALE);
        pango_layout_set_font_description(pl, hfd);
        pango_layout_set_text(pl, "Wallpapers", -1);
        pango_font_description_free(hfd);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        cairo_set_source_rgba(cr, COLOR(FG_HEX), 1.0);
        cairo_move_to(cr, px + MENU_PADDING,
                      py + (WALL_HEADER_H - th) / 2.0);
        pango_cairo_show_layout(cr, pl);

        wlp_draw_power_toggle(cr);
    }

    /* Clip to content area */
    cairo_save(cr);
    cairo_rectangle(cr, px, py + WALL_HEADER_H, pw,
                    body_h - WALL_HEADER_H - MENU_PADDING);
    cairo_clip(cr);

    int cols = WALL_THUMB_COLS;
    int step = WALL_THUMB_H + WALL_LABEL_H + WALL_THUMB_GAP;
    double gx = px + MENU_PADDING;
    double gy = py + WALL_HEADER_H + MENU_PADDING - g_wlp.scroll;

    for (int i = 0; i < g_wlp.n_entries; i++) {
        int col = i % cols;
        int row = i / cols;
        double cx = gx + col * (WALL_THUMB_W + WALL_THUMB_GAP);
        double cy = gy + row * step;

        /* Skip if off-screen */
        if (cy + step < py + WALL_HEADER_H || cy > py + body_h)
            continue;

        int hover = (g_wlp.hover_idx == i);

        /* Thumbnail background */
        if (hover) {
            cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
            cairo_new_path(cr);
            rounded_rect(cr, cx, cy, WALL_THUMB_W, WALL_THUMB_H, 4);
            cairo_fill(cr);
        }

        /* Thumbnail image */
        if (g_wlp.entries[i].thumb) {
            int iw = cairo_image_surface_get_width(g_wlp.entries[i].thumb);
            int ih = cairo_image_surface_get_height(g_wlp.entries[i].thumb);
            double sc = fmax((double)WALL_THUMB_W / iw, (double)WALL_THUMB_H / ih);
            double dw = iw * sc, dh = ih * sc;
            double dx = cx + (WALL_THUMB_W - dw) / 2.0;
            double dy = cy + (WALL_THUMB_H - dh) / 2.0;
            cairo_save(cr);
            cairo_new_path(cr);
            rounded_rect(cr, cx, cy, WALL_THUMB_W, WALL_THUMB_H, 4);
            cairo_clip(cr);
            cairo_translate(cr, dx, dy);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, g_wlp.entries[i].thumb, 0, 0);
            cairo_paint_with_alpha(cr, hover ? 1.0 : 0.9);
            cairo_restore(cr);
        } else {
            /* Placeholder */
            cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.15);
            cairo_new_path(cr);
            rounded_rect(cr, cx + 2, cy + 2, WALL_THUMB_W - 4, WALL_THUMB_H - 4, 4);
            cairo_fill(cr);
        }

        /* Thumbnail border */
        int active = (strcmp(g_wlp.cur_wallpaper, g_wlp.entries[i].path) == 0);
        if (active) {
            cairo_set_source_rgba(cr, 0.4, 0.6, 0.9, 0.9);
            cairo_set_line_width(cr, 2);
        } else {
            cairo_set_source_rgba(cr, COLOR(FG_HEX), hover ? 0.6 : 0.2);
            cairo_set_line_width(cr, 1);
        }
        cairo_new_path(cr);
        rounded_rect(cr, cx, cy, WALL_THUMB_W, WALL_THUMB_H, 4);
        cairo_stroke(cr);

        /* Filename label */
        {
            PangoFontDescription *lfd = pango_font_description_from_string(FONT_FAMILY);
            pango_font_description_set_absolute_size(lfd, 11.0 * PANGO_SCALE);
            pango_layout_set_font_description(pl, lfd);
            pango_layout_set_text(pl, g_wlp.entries[i].name, -1);
            pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
            pango_layout_set_width(pl, WALL_THUMB_W * PANGO_SCALE);
            int tw, th;
            pango_layout_get_pixel_size(pl, &tw, &th);
            cairo_set_source_rgba(cr, COLOR(FG_HEX), hover ? 1.0 : 0.7);
            cairo_move_to(cr, cx, cy + WALL_THUMB_H + 2);
            pango_cairo_show_layout(cr, pl);
            pango_font_description_free(lfd);
        }
    }

    cairo_restore(cr);
    cairo_restore(cr);
}

static void
xml_escape_append(GString *s, const char *in)
{
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        switch (*p) {
        case '&': g_string_append(s, "&amp;"); break;
        case '<': g_string_append(s, "&lt;"); break;
        case '>': g_string_append(s, "&gt;"); break;
        default: g_string_append_c(s, *p); break;
        }
    }
}

static void
notif_draw_row(cairo_t *cr, PangoLayout *pl, int row)
{
    struct notif_entry *e = &g_notifs[g_n_notifs - 1 - row];
    double ry = g_npop.list_y + row * NOTIF_ROW_H - lround(g_npop.scroll);
    double row_w = g_npop.rows_w;
    double box_y = ry + NOTIF_BOX_GAP / 2.0;
    double box_h = NOTIF_ROW_H - NOTIF_BOX_GAP;

    cairo_set_source_rgba(cr, COLOR(NOTIF_BOX_HEX), NOTIF_BOX_A);
    rounded_rect(cr, g_npop.rows_x, box_y, row_w, box_h, 6);
    cairo_fill(cr);

    if (g_npop.hover_row == row) {
        cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A * 0.5);
        rounded_rect(cr, g_npop.rows_x, box_y, row_w, box_h, 6);
        cairo_fill(cr);
    }

    GString *t = g_string_new("<b>");
    xml_escape_append(t, e->summary[0] ? e->summary : e->app_name);
    g_string_append(t, "</b>");
    pango_layout_set_markup(pl, t->str, -1);
    g_string_free(t, TRUE);
    pango_layout_set_width(pl, (int)(g_npop.text_w * PANGO_SCALE));
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    int tht, thh;
    pango_layout_get_pixel_size(pl, &tht, &thh);
    cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.95);
    cairo_move_to(cr, g_npop.text_x, box_y + NOTIF_TEXT_PAD_Y);
    pango_cairo_show_layout(cr, pl);

    if (e->body[0]) {
        pango_layout_set_attributes(pl, NULL);
        pango_layout_set_text(pl, e->body, -1);
        pango_layout_set_width(pl, (int)(g_npop.text_w * PANGO_SCALE));
        pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
        cairo_move_to(cr, g_npop.text_x,
                      box_y + NOTIF_TEXT_PAD_Y + thh + NOTIF_LINE_SPACING);
        pango_cairo_show_layout(cr, pl);
    }
    pango_layout_set_width(pl, -1);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_NONE);

    char ts[16];
    struct tm tmv;
    time_t tt = (time_t)(e->ts_ms / 1000);
    localtime_r(&tt, &tmv);
    strftime(ts, sizeof(ts), "%I:%M %p", &tmv);
    pango_layout_set_font_description(pl, clock_font());
    pango_layout_set_attributes(pl, NULL);
    pango_layout_set_text(pl, ts, -1);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.85);
    cairo_move_to(cr, g_npop.time_right - tw,
                  ry + NOTIF_ROW_H / 2.0 - th / 2.0);
    pango_cairo_show_layout(cr, pl);
    pango_layout_set_font_description(pl, clock_font());

    double tcx = g_npop.trash_cx;
    double tcy = ry + NOTIF_ROW_H / 2.0;
    int over = g_npop.hover_trash && g_npop.hover_row == row;
    if (over)
        draw_hover_circle(cr, tcx, tcy, NOTIF_TRASH_ICON, 4);
    cairo_set_source_rgba(cr, COLOR(FG_HEX), over ? 1.0 : 0.6);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double s = NOTIF_TRASH_ICON;
    double cx = tcx;
    double cy = tcy;

    cairo_move_to(cr, cx - s * 0.13, cy - s * 0.30);
    cairo_line_to(cr, cx - s * 0.13, cy - s * 0.42);
    cairo_line_to(cr, cx + s * 0.13, cy - s * 0.42);
    cairo_line_to(cr, cx + s * 0.13, cy - s * 0.30);

    cairo_move_to(cr, cx - s * 0.50, cy - s * 0.30);
    cairo_line_to(cr, cx + s * 0.50, cy - s * 0.30);

    cairo_move_to(cr, cx - s * 0.46, cy - s * 0.22);
    cairo_line_to(cr, cx + s * 0.46, cy - s * 0.22);
    cairo_line_to(cr, cx + s * 0.32, cy + s * 0.46);
    cairo_line_to(cr, cx - s * 0.32, cy + s * 0.46);
    cairo_close_path(cr);

    cairo_move_to(cr, cx - s * 0.16, cy - s * 0.18);
    cairo_line_to(cr, cx - s * 0.16, cy + s * 0.40);
    cairo_move_to(cr, cx + s * 0.16, cy - s * 0.18);
    cairo_line_to(cr, cx + s * 0.16, cy + s * 0.40);
    cairo_stroke(cr);
}

static void
notif_method_call(GDBusConnection *connection, const char *sender,
                  const char *object_path, const char *interface_name,
                  const char *method_name, GVariant *parameters,
                  GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path;
    (void)interface_name; (void)user_data;

    if (strcmp(method_name, "Notify") == 0) {
        const char *app_name = "", *app_icon = "", *summary = "", *body = "";
        const char **actions = NULL;
        uint32_t replaces_id = 0;
        GVariant *hints = NULL;
        int timeout = -1;
        if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(susssasa{sv}i)"))) {
            g_variant_get(parameters, "(&su&s&s&s^a&s@a{sv}i)",
                          &app_name, &replaces_id, &app_icon, &summary, &body,
                          &actions, &hints, &timeout);
        }

        char app_id[128] = "";
        if (hints) {
            GVariant *de = g_variant_lookup_value(hints, "desktop-entry",
                                                  G_VARIANT_TYPE_STRING);
            if (de) {
                const char *s = g_variant_get_string(de, NULL);
                if (s) g_strlcpy(app_id, s, sizeof(app_id));
                g_variant_unref(de);
            }
            g_variant_unref(hints);
        }
        g_free(actions);
        if (!app_id[0] && app_name[0]) {
            for (int i = 0; i < g_app.n_apps; i++) {
                if (g_strcmp0(g_app.apps[i].name, app_name) == 0) {
                    g_strlcpy(app_id, g_app.apps[i].desktop_id
                                          ? g_app.apps[i].desktop_id : "",
                              sizeof(app_id));
                    break;
                }
            }
        }

        uint32_t id = notif_add(app_name, app_id, summary, body, replaces_id);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u)", id));
    } else if (strcmp(method_name, "CloseNotification") == 0) {
        uint32_t id = 0;
        g_variant_get(parameters, "(u)", &id);
        notif_remove_by_id(id, 3);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (strcmp(method_name, "GetCapabilities") == 0) {
        GVariantBuilder rb;
        g_variant_builder_init(&rb, G_VARIANT_TYPE("(as)"));
        g_variant_builder_open(&rb, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&rb, "s", "body");
        g_variant_builder_add(&rb, "s", "persistence");
        g_variant_builder_close(&rb);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_builder_end(&rb));
    } else if (strcmp(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(ssss)", "jtlab", "jtlab", "1.0", "1.2"));
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method %s", method_name);
    }
}

static const GDBusInterfaceVTable notif_vtable = {
    .method_call = notif_method_call,
};

static const char notif_xml[] =
    "<node>"
    "<interface name='org.freedesktop.Notifications'>"
    "<method name='GetCapabilities'>"
    "<arg type='as' name='capabilities' direction='out'/>"
    "</method>"
    "<method name='Notify'>"
    "<arg type='s' name='app_name' direction='in'/>"
    "<arg type='u' name='replaces_id' direction='in'/>"
    "<arg type='s' name='app_icon' direction='in'/>"
    "<arg type='s' name='summary' direction='in'/>"
    "<arg type='s' name='body' direction='in'/>"
    "<arg type='as' name='actions' direction='in'/>"
    "<arg type='a{sv}' name='hints' direction='in'/>"
    "<arg type='i' name='expire_timeout' direction='in'/>"
    "<arg type='u' name='id' direction='out'/>"
    "</method>"
    "<method name='CloseNotification'>"
    "<arg type='u' name='id' direction='in'/>"
    "</method>"
    "<method name='GetServerInformation'>"
    "<arg type='s' name='name' direction='out'/>"
    "<arg type='s' name='vendor' direction='out'/>"
    "<arg type='s' name='version' direction='out'/>"
    "<arg type='s' name='spec_version' direction='out'/>"
    "</method>"
    "<signal name='NotificationClosed'>"
    "<arg type='u' name='id'/>"
    "<arg type='u' name='reason'/>"
    "</signal>"
    "</interface>"
    "</node>";

static void
notif_dbus_init(void)
{
    if (g_sni.notif_owner || !g_sni.dbus_conn) return;
    notif_store_load();
    GError *err = NULL;
    g_sni.notif_node = g_dbus_node_info_new_for_xml(notif_xml, &err);
    if (!g_sni.notif_node) {
        g_clear_error(&err);
        return;
    }
    g_sni.notif_obj_reg = g_dbus_connection_register_object(g_sni.dbus_conn,
        "/org/freedesktop/Notifications",
        g_sni.notif_node->interfaces[0], &notif_vtable, NULL, NULL, &err);
    g_clear_error(&err);
    g_sni.notif_owner = g_bus_own_name_on_connection(g_sni.dbus_conn,
        "org.freedesktop.Notifications",
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        NULL, NULL, NULL, NULL);
}

static void
notif_shutdown(void)
{
    if (g_sni.notif_owner) {
        g_bus_unown_name(g_sni.notif_owner);
        g_sni.notif_owner = 0;
    }
    if (g_sni.dbus_conn && g_sni.notif_obj_reg) {
        g_dbus_connection_unregister_object(g_sni.dbus_conn,
                                            g_sni.notif_obj_reg);
        g_sni.notif_obj_reg = 0;
    }
    if (g_sni.notif_node) {
        g_dbus_node_info_unref(g_sni.notif_node);
        g_sni.notif_node = NULL;
    }
}

static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, -M_PI_2);
    cairo_close_path(cr);
}

static void
draw_shadow(cairo_t *cr, double x, double y, double w, double h, double r, double sw, double sa)
{
    for (int i = (int)sw; i >= 1; i--) {
        double a = sa * (1.0 - (double)i / (sw + 1.0));
        cairo_set_source_rgba(cr, 0, 0, 0, a);
        cairo_new_path(cr);
        rounded_rect(cr, x - i, y - i, w + 2*i, h + 2*i, r + i);
        cairo_fill(cr);
    }
}

static void
panel_begin(cairo_t *cr, double x, double y, double w, double h, double r,
            double sh_w, double sh_a, int inset_border)
{
    draw_shadow(cr, x, y, w, h, r, sh_w, sh_a);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, COLOR(MENU_BG_HEX), MENU_BG_A);
    cairo_new_path(cr);
    rounded_rect(cr, x, y, w, h, r);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), MENU_BORDER_A);
    cairo_set_line_width(cr, MENU_BORDER_W);
    cairo_new_path(cr);
    if (inset_border) {
        rounded_rect(cr, x + MENU_BORDER_W / 2.0, y + MENU_BORDER_W / 2.0,
                     w - MENU_BORDER_W, h - MENU_BORDER_W, r);
    } else {
        rounded_rect(cr, x, y, w, h, r);
    }
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_new_path(cr);
    rounded_rect(cr, x, y, w, h, r);
    cairo_clip(cr);
}

static double
draw_icon_fit(cairo_t *cr, cairo_surface_t *sf, double x, double y,
              double box_w, double box_h, double size, int center_h)
{
    if (sf && cairo_surface_status(sf) == CAIRO_STATUS_SUCCESS) {
        int iw = cairo_image_surface_get_width(sf);
        int ih = cairo_image_surface_get_height(sf);
        if (iw > 0 && ih > 0) {
            double scale_i = size / (iw > ih ? iw : ih);
            double dw = iw * scale_i;
            double dh = ih * scale_i;
            double ho = center_h ? (box_w - dw) / 2.0 : 0.0;
            cairo_save(cr);
            cairo_translate(cr, x + ho, y + (box_h - dh) / 2.0);
            cairo_scale(cr, scale_i, scale_i);
            cairo_set_source_surface(cr, sf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }
    return size + 8;
}

static void
draw_active_indicator(cairo_t *cr, double px_x, double item_w, int focused)
{
    double by = bar_y_offset();
    cairo_set_source_rgba(cr, COLOR(FG_HEX), 1.0);
    cairo_new_path(cr);
    if (focused) {
        double lw = 18, lh = 2;
        cairo_rectangle(cr, px_x + (item_w - lw) / 2.0, by + BAR_HEIGHT - 4, lw, lh);
    } else {
        double dw = 4, dh = 2;
        cairo_rectangle(cr, px_x + (item_w - dw) / 2.0, by + BAR_HEIGHT - 4, dw, dh);
    }
    cairo_fill(cr);
}

static void sys_draw_text(cairo_t *cr, double x, double y, double w,
                          double h, const char *text, double size,
                          double r, double g, double b, double a,
                          PangoEllipsizeMode el, PangoAlignment align);

static void
draw_item_badge(cairo_t *cr, double px_x, double item_w, double bar_y,
                double bar_h, int icon_size, int count)
{
    char txt[8];
    double cx = px_x + item_w / 2.0 + icon_size / 2.0 - 3;
    double cy = bar_y + bar_h / 2.0 + icon_size / 2.0 - 5;
    double rad = count > 99 ? 9 : 7;

    snprintf(txt, sizeof(txt), count > 99 ? "99+" : "%d", count);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, COLOR(BADGE_HEX), 1.0);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    sys_draw_text(cr, cx - rad, cy - rad - 1, rad * 2, rad * 2 + 2,
                  txt, 9.5, 0.08, 0.08, 0.08, 1.0,
                  PANGO_ELLIPSIZE_NONE, PANGO_ALIGN_CENTER);
}

static void
draw_launcher_item(cairo_t *cr, cairo_surface_t *icon, double px_x, double item_w,
                   double bar_y, double bar_h, int icon_size,
                   int hovered, int focused, int draw_active, int badge_count)
{
    double row_y = bar_y + (bar_h - icon_size) / 2.0;

    if (focused) {
        double hx = px_x + (item_w - icon_size) / 2.0 - 4;
        cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), FOCUS_HOVER_A);
        cairo_new_path(cr);
        rounded_rect(cr, hx, bar_y + 2, icon_size + 8, bar_h - 4,
                     (bar_h - 4) / 2.0);
        cairo_fill(cr);
    }

    if (hovered) {
        double hx = px_x + (item_w - icon_size) / 2.0 - 4;
        cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
        cairo_new_path(cr);
        rounded_rect(cr, hx, bar_y + 2, icon_size + 8, bar_h - 4,
                     (bar_h - 4) / 2.0);
        cairo_fill(cr);
    }

    draw_icon_fit(cr, icon, px_x, row_y, item_w, icon_size, icon_size, 1);
    if (draw_active)
        draw_active_indicator(cr, px_x, item_w, focused);
    if (badge_count > 1)
        draw_item_badge(cr, px_x, item_w, bar_y, bar_h, icon_size,
                        badge_count);
}

static void
draw_speaker(cairo_t *cr, double cx, double cy, double a, double b,
             double c, double d, double w, double r1, double r2, double alpha)
{
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 1.8);
    cairo_set_source_rgba(cr, COLOR(FG_HEX), alpha);

    cairo_move_to(cr, cx - a, cy - c);
    cairo_line_to(cr, cx - b, cy - c);
    cairo_line_to(cr, cx + 1, cy - d);
    cairo_line_to(cr, cx + 1, cy + d);
    cairo_line_to(cr, cx - b, cy + c);
    cairo_line_to(cr, cx - a, cy + c);
    cairo_close_path(cr);
    cairo_stroke(cr);

    cairo_arc(cr, cx + w, cy, r1, -50 * M_PI / 180.0, 50 * M_PI / 180.0);
    cairo_stroke(cr);
    cairo_arc(cr, cx + w, cy, r2, -45 * M_PI / 180.0, 45 * M_PI / 180.0);
    cairo_stroke(cr);
}

static void
draw_hover_circle(cairo_t *cr, double cx, double cy, double icon_sz, double pad)
{
    cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, icon_sz / 2.0 + pad, 0, 2 * M_PI);
    cairo_fill(cr);
}

static int
draw_vcenter_text(cairo_t *cr, PangoLayout *pl, const char *text,
                  double x, double y, double box_h,
                  double r, double g, double b, double a,
                  double max_w, int ellipsize)
{
    cairo_set_source_rgba(cr, r, g, b, a);
    pango_layout_set_text(pl, text, -1);
    if (max_w > 0) {
        pango_layout_set_width(pl, (int)(max_w * PANGO_SCALE));
        pango_layout_set_ellipsize(pl, ellipsize ? PANGO_ELLIPSIZE_END
                                                  : PANGO_ELLIPSIZE_NONE);
    }
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    cairo_move_to(cr, x, y + (box_h - th) / 2.0);
    pango_cairo_show_layout(cr, pl);
    if (max_w > 0) {
        pango_layout_set_width(pl, -1);
        pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_NONE);
    }
    return tw;
}

static void
draw_power_icon(int action, cairo_t *cr, double cx, double cy)
{
    cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), 1.0);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    double r = 5.0;
    switch (action) {
    case 0:
        cairo_arc(cr, cx, cy, r, 2 * M_PI / 3, M_PI / 3);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - r);
        cairo_line_to(cr, cx, cy - r - 3.2);
        cairo_stroke(cr);
        break;
    case 1:
        cairo_arc(cr, cx, cy, r, 2 * M_PI / 3, 3 * M_PI / 2);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy, r, 3 * M_PI / 2, M_PI / 3);
        cairo_stroke(cr);
        {
            double ax = cx;
            double ay = cy - r;
            cairo_move_to(cr, ax, ay);
            cairo_line_to(cr, ax + 4.5, ay - 2.2);
            cairo_move_to(cr, ax, ay);
            cairo_line_to(cr, ax + 4.5, ay + 2.2);
            cairo_stroke(cr);
        }
        break;
    case 2:
        cairo_rectangle(cr, cx - r - 1, cy - r, r + 1.5, 2 * r);
        cairo_stroke(cr);
        cairo_move_to(cr, cx - 1, cy);
        cairo_line_to(cr, cx + r + 2, cy);
        cairo_move_to(cr, cx + r - 1, cy - 2.5);
        cairo_line_to(cr, cx + r + 2, cy);
        cairo_line_to(cr, cx + r - 1, cy + 2.5);
        cairo_stroke(cr);
        break;
    case 3:
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, r, 0.5 * M_PI, 1.5 * M_PI);
        cairo_arc_negative(cr, cx + r * 0.8, cy, r * 0.9, 1.5 * M_PI, 0.5 * M_PI);
        cairo_close_path(cr);
        cairo_fill(cr);
        break;
    }
}


static cairo_surface_t *
surface_from_pixbuf(GdkPixbuf *pb)
{
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int has_a = gdk_pixbuf_get_has_alpha(pb);
    int nch = gdk_pixbuf_get_n_channels(pb);
    const guchar *src = gdk_pixbuf_get_pixels(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);

    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char *dst = cairo_image_surface_get_data(sf);
    int drs = cairo_image_surface_get_stride(sf);
    for (int y = 0; y < h; y++) {
        const guchar *s = src + (gint64)y * rs;
        unsigned char *d = dst + (gint64)y * drs;
        for (int x = 0; x < w; x++) {
            guchar r = s[x*nch], g = s[x*nch+1], b = s[x*nch+2];
            guchar a = has_a ? s[x*nch+3] : 255;
            d[x*4+0] = (unsigned char)(((unsigned)b * a + 127) / 255);
            d[x*4+1] = (unsigned char)(((unsigned)g * a + 127) / 255);
            d[x*4+2] = (unsigned char)(((unsigned)r * a + 127) / 255);
            d[x*4+3] = a;
        }
    }
    cairo_surface_mark_dirty(sf);
    return sf;
}

static void
art_clear(void)
{
    if (g_sys.art) { cairo_surface_destroy(g_sys.art); g_sys.art = NULL; }
}

static void
url_decode(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 1 < n; i++) {
        if (in[i] == '%' && in[i+1] && in[i+2]) {
            unsigned int v;
            if (sscanf(in + i + 1, "%2x", &v) == 1) {
                out[o++] = (char)v;
                i += 2;
                continue;
            }
        }
        if (in[i] == '+') { out[o++] = ' '; continue; }
        out[o++] = in[i];
    }
    out[o] = '\0';
}

static void
art_reload(void)
{
    art_clear();
    if (!g_sys.art_path[0]) return;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(g_sys.art_path, NULL);
    if (!pb) return;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    if (w <= 0 || h <= 0) { g_object_unref(pb); return; }
    double sc = (double)SYS_THUMB / (w > h ? w : h);
    int nw = (int)(w * sc + 0.5), nh = (int)(h * sc + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    GdkPixbuf *spb = gdk_pixbuf_scale_simple(pb, nw, nh, GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    if (!spb) return;
    g_sys.art = surface_from_pixbuf(spb);
    g_object_unref(spb);
}

static void
art_url_rewrite(const char *in, char *out, size_t n)
{
    snprintf(out, n, "%s", in);
    char *p = strstr(out, "/vi_webp/");
    if (!p) return;
    memmove(p + 4, p + 9, strlen(p + 9) + 1);
    p[3] = '/';
    char *q = strstr(p, ".webp");
    if (q) {
        q[1] = 'j'; q[2] = 'p'; q[3] = 'g';
        memmove(q + 4, q + 5, strlen(q + 5) + 1);
    }
}

static void
art_cache_path(const char *url, char *out, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    const char *cache = getenv("XDG_CACHE_HOME");
    char dir[480];
    if (cache && cache[0]) {
        snprintf(dir, sizeof(dir), "%s/jtlab", cache);
    } else {
        const char *home = getenv("HOME");
        snprintf(dir, sizeof(dir), "%s/.cache/jtlab", home ? home : "/tmp");
    }
    mkdir(dir, 0700);
    size_t max_dir = n > 32 ? n - 32 : 1;
    if (strlen(dir) >= max_dir) dir[max_dir - 1] = '\0';
    snprintf(out, n, "%s/art-%016llx.img", dir, (unsigned long long)h);
}

static void
art_download_finish(GObject *src, GAsyncResult *res, gpointer data)
{
    char *cache = data;
    GError *err = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (!body) {
        if (err) g_error_free(err);
        g_free(cache);
        return;
    }
    gsize len;
    const void *p = g_bytes_get_data(body, &len);
    if (strcmp(cache, g_sys.art_path) == 0) {
        GFile *f = g_file_new_for_path(cache);
        g_file_replace_contents(f, p, len, NULL, FALSE,
                                G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, &err);
        g_object_unref(f);
        if (!err) {
            art_reload();
            if (!g_sys.art) unlink(cache);
        }
    }
    g_bytes_unref(body);
    if (err) g_error_free(err);
    g_free(cache);
    if (g_bar.menu_open) render_request();
}

static void
art_download_start(const char *url, const char *cache)
{
    if (!g_sni.art_session) {
        g_sni.art_session = soup_session_new_with_options("timeout", 15, NULL);
        if (!g_sni.art_session) return;
    }
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) return;
    soup_session_send_and_read_async(g_sni.art_session, msg, G_PRIORITY_DEFAULT,
                                     NULL, art_download_finish, g_strdup(cache));
}

static void
sys_stats_refresh(void)
{
    long long total = -1, avail = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal:", 9) == 0)
                sscanf(line + 9, "%lld", &total);
            else if (strncmp(line, "MemAvailable:", 13) == 0)
                sscanf(line + 13, "%lld", &avail);
            else if (strncmp(line, "MemFree:", 8) == 0 && avail < 0)
                sscanf(line + 8, "%lld", &avail);
            if (total >= 0 && avail >= 0) break;
        }
        fclose(f);
    }
    if (total > 0 && avail >= 0)
        g_sys.pct[SYS_STAT_RAM] = 100.0 * (total - avail) / total;

    f = fopen("/proc/stat", "r");
    if (f) {
        long long user, nice, sys, idle, iowait, irq, softirq, steal;
        int got = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                         &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal);
        fclose(f);
        if (got == 8) {
            long long idle_all = idle + iowait;
            long long tsum = user + nice + sys + idle_all + irq + softirq + steal;
            if (g_sys.cpu_prev_valid) {
                long long dt = tsum - g_sys.cpu_prev_total;
                long long di = idle_all - g_sys.cpu_prev_idle;
                if (dt > 0) {
                    double pct = 100.0 * (double)(dt - di) / (double)dt;
                    pct = clampd(pct, 0, 100);
                    g_sys.pct[SYS_STAT_CPU] = pct;
                }
            }
            g_sys.cpu_prev_total = tsum;
            g_sys.cpu_prev_idle = idle_all;
            g_sys.cpu_prev_valid = 1;
        }
    }

    double t = (double)now_ms() / 1000.0;
    double g = 18 + 14 * sin(t / 13.0) + 7 * sin(t / 4.3);
    g = clampd(g, 0, 100);
    g_sys.pct[SYS_STAT_GPU] = g;
}

static void mpris_props_cb(GDBusConnection *conn, const gchar *sender,
    const gchar *path, const gchar *iface, const gchar *sig,
    GVariant *params, gpointer data);

static void
mpris_fetch_meta(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(g_sni.dbus_conn, res, &err);
    if (err) { g_error_free(err); return; }

    GVariant *props = g_variant_get_child_value(reply, 0);
#ifdef JTLAB_DEBUG
    {
        GVariant *dm = g_variant_lookup_value(props, "Metadata", G_VARIANT_TYPE("a{sv}"));
        if (dm) {
            GVariant *du = g_variant_lookup_value(dm, "mpris:artUrl", G_VARIANT_TYPE_STRING);
            fprintf(stderr, "[dbg] player=%s artUrl=%s\n", g_sys.player_bus,
                    du ? g_variant_get_string(du, NULL) : "(none)");
            if (du) g_variant_unref(du);
            g_variant_unref(dm);
        }
    }
#endif

    GVariant *v = g_variant_lookup_value(props, "PlaybackStatus",
                                         G_VARIANT_TYPE_STRING);
    if (v) {
        snprintf(g_sys.status, sizeof(g_sys.status), "%s",
                 g_variant_get_string(v, NULL));
        g_variant_unref(v);
    }

    GVariant *meta = g_variant_lookup_value(props, "Metadata",
                                            G_VARIANT_TYPE("a{sv}"));
    if (meta) {
        g_sys.track[0] = 0;
        g_sys.artist[0] = 0;
        g_sys.album[0] = 0;

        v = g_variant_lookup_value(meta, "xesam:title", G_VARIANT_TYPE_STRING);
        if (v) {
            snprintf(g_sys.track, sizeof(g_sys.track), "%s",
                     g_variant_get_string(v, NULL));
            g_variant_unref(v);
        }

        v = g_variant_lookup_value(meta, "xesam:album", G_VARIANT_TYPE_STRING);
        if (v) {
            snprintf(g_sys.album, sizeof(g_sys.album), "%s",
                     g_variant_get_string(v, NULL));
            g_variant_unref(v);
        }

        v = g_variant_lookup_value(meta, "xesam:artist", G_VARIANT_TYPE("as"));
        if (v) {
            char buf[160] = {0};
            gsize n = g_variant_n_children(v);
            for (gsize i = 0; i < n; i++) {
                GVariant *s = g_variant_get_child_value(v, i);
                const char *name = g_variant_get_string(s, NULL);
                size_t len = strlen(buf);
                if (i && len + 2 < sizeof(buf)) { strcat(buf, ", "); len += 2; }
                if (len + strlen(name) + 1 < sizeof(buf))
                    strcat(buf, name);
                g_variant_unref(s);
            }
            snprintf(g_sys.artist, sizeof(g_sys.artist), "%s", buf);
            g_variant_unref(v);
        }

        int new_track = 0;
        v = g_variant_lookup_value(meta, "mpris:trackid", G_VARIANT_TYPE_OBJECT_PATH);
        if (v) {
            const char *tid = g_variant_get_string(v, NULL);
            if (strcmp(tid, g_sys.track_id) != 0) {
                snprintf(g_sys.track_id, sizeof(g_sys.track_id), "%s", tid);
                new_track = 1;
            }
            g_variant_unref(v);
        }

        v = g_variant_lookup_value(meta, "mpris:artUrl", G_VARIANT_TYPE_STRING);
        if (v) {
            const char *url = g_variant_get_string(v, NULL);
            g_variant_unref(v);
            if (strcmp(url, g_app.art_url) != 0) {
                snprintf(g_app.art_url, sizeof(g_app.art_url), "%s", url);
                if (strncmp(url, "http://", 7) == 0 ||
                    strncmp(url, "https://", 8) == 0) {
                    char good[512];
                    art_url_rewrite(url, good, sizeof(good));
                    char cache[512];
                    art_cache_path(good, cache, sizeof(cache));
                    snprintf(g_sys.art_path, sizeof(g_sys.art_path), "%s", cache);
                    if (access(cache, R_OK) == 0) {
                        art_reload();
                    } else {
                        art_download_start(good, cache);
                    }
                } else if (strncmp(url, "file://", 7) == 0) {
                    char newpath[512] = {0};
                    url_decode(newpath, sizeof(newpath), url + 7);
                    snprintf(g_sys.art_path, sizeof(g_sys.art_path), "%s", newpath);
                    art_reload();
                }
            }
        } else if (new_track && g_sys.art_path[0]) {
            g_sys.art_path[0] = 0;
            g_app.art_url[0] = 0;
            art_clear();
        }

        g_variant_unref(meta);
    }
    g_variant_unref(props);
    g_variant_unref(reply);
    if (g_bar.menu_open) render_request();
}

static void
mpris_fetch(void)
{
    if (!g_sni.dbus_conn || !g_sys.player_bus[0]) return;
    g_dbus_connection_call(g_sni.dbus_conn, g_sys.player_bus,
        "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
        "GetAll", g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        mpris_fetch_meta, NULL);
}

static void
mpris_clear_player(void)
{
    g_sys.track[0] = g_sys.artist[0] = g_sys.album[0] = g_sys.status[0] = 0;
    g_sys.track_id[0] = 0;
    g_sys.art_path[0] = 0;
    g_app.art_url[0] = 0;
    art_clear();
}

static void
mpris_set_player(const char *bus)
{
    if (strcmp(bus, g_sys.player_bus) == 0) return;
    if (g_sys.props_sig) {
        g_dbus_connection_signal_unsubscribe(g_sni.dbus_conn, g_sys.props_sig);
        g_sys.props_sig = 0;
    }
    snprintf(g_sys.player_bus, sizeof(g_sys.player_bus), "%s", bus);
    mpris_clear_player();
    g_sys.props_sig = g_dbus_connection_signal_subscribe(g_sni.dbus_conn, bus,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        "/org/mpris/MediaPlayer2", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        mpris_props_cb, NULL, NULL);
    mpris_fetch();
}

static void
mpris_list_names_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(g_sni.dbus_conn, res, &err);
    if (err) { g_error_free(err); return; }

    char pick[sizeof(g_sys.player_bus)];
    pick[0] = 0;
    GVariant *names = g_variant_get_child_value(reply, 0);
    gsize n = g_variant_n_children(names);
    for (gsize i = 0; i < n; i++) {
        GVariant *s = g_variant_get_child_value(names, i);
        const char *name = g_variant_get_string(s, NULL);
        if (strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
            if (strstr(name, ".mpd") || strstr(name, ".playerctld")) {
                snprintf(pick, sizeof(pick), "%s", name);
                g_variant_unref(s);
                break;
            }
            if (!pick[0]) snprintf(pick, sizeof(pick), "%s", name);
        }
        g_variant_unref(s);
    }
    g_variant_unref(names);
    g_variant_unref(reply);
    if (pick[0]) mpris_set_player(pick);
}

static void
mpris_discover(void)
{
    if (!g_sni.dbus_conn) return;
    g_dbus_connection_call(g_sni.dbus_conn, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        mpris_list_names_cb, NULL);
}

static void
mpris_props_cb(GDBusConnection *conn, const gchar *sender, const gchar *path,
    const gchar *iface, const gchar *sig, GVariant *params, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;
    if (strcmp(sig, "PropertiesChanged") != 0) return;
    const gchar *changed_iface = NULL;
    GVariant *changed = NULL, *invalid = NULL;
    g_variant_get(params, "(&s@a{sv}@as)", &changed_iface, &changed, &invalid);
    if (changed) g_variant_unref(changed);
    if (invalid) g_variant_unref(invalid);
    if (changed_iface && strcmp(changed_iface, "org.mpris.MediaPlayer2.Player") == 0)
        mpris_fetch();
}

static void
mpris_owner_cb(GDBusConnection *conn, const gchar *sender, const gchar *path,
    const gchar *iface, const gchar *sig, GVariant *params, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)sig; (void)data;
    const gchar *name = NULL, *old_owner = NULL, *new_owner = NULL;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
    (void)old_owner;
    if (name && g_sys.player_bus[0] && strcmp(name, g_sys.player_bus) == 0 &&
        (!new_owner || !new_owner[0])) {
        g_sys.player_bus[0] = 0;
        mpris_clear_player();
        if (g_sys.props_sig) {
            g_dbus_connection_signal_unsubscribe(g_sni.dbus_conn, g_sys.props_sig);
            g_sys.props_sig = 0;
        }
        if (g_bar.menu_open) render_request();
    }
}

static void
mpris_refresh(void)
{
    if (!g_sni.dbus_conn) return;
    if (!g_sys.player_bus[0])
        mpris_discover();
    else
        mpris_fetch();
}

static void
mpris_ctrl_finish(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)res; (void)data;
}

static void
mpris_control(const char *method)
{
    if (!g_sni.dbus_conn || !g_sys.player_bus[0]) return;
    g_dbus_connection_call(g_sni.dbus_conn, g_sys.player_bus,
        "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
        method, g_variant_new_tuple(NULL, 0), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, mpris_ctrl_finish, NULL);
    g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
}

static void
sys_init(void)
{
    if (g_sni.dbus_conn) {
        g_sys.owner_sig = g_dbus_connection_signal_subscribe(g_sni.dbus_conn,
            "org.freedesktop.DBus", "org.freedesktop.DBus",
            "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, mpris_owner_cb, NULL, NULL);
    }
}

static int
sys_column_height(void)
{
    return 2 * MENU_PADDING + 8 + SYS_THUMB + 6 + SYS_CTRL_BTN + 8 + 1 + 8
           + SYS_STAT_COUNT * SYS_STAT_H + MENU_PADDING;
}

static void
sys_draw_text(cairo_t *cr, double x, double y, double w, double h,
              const char *text, double size, double r, double g, double b,
              double a, PangoEllipsizeMode el, PangoAlignment align)
{
    PangoFontDescription *fd = pango_font_description_from_string(FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    PangoLayout *pl = pango_ctx_layout(cr);
    pango_layout_set_font_description(pl, fd);
    pango_layout_set_text(pl, text, -1);
    pango_layout_set_width(pl, (int)(w * PANGO_SCALE));
    pango_layout_set_alignment(pl, align);
    pango_layout_set_ellipsize(pl, el);
    int th;
    pango_layout_get_pixel_size(pl, NULL, &th);
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, x, y + (h - th) / 2.0);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    pango_font_description_free(fd);
}

static void
sys_draw_bar(cairo_t *cr, double x, double y, double w, double pct,
             double r, double g, double b)
{
    double bh = 4;
    cairo_new_path(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    rounded_rect(cr, x, y, w, bh, 2);
    cairo_fill(cr);
    double fw = w * (pct > 100 ? 1.0 : pct / 100.0);
    if (fw > 0.5) {
        cairo_new_path(cr);
        cairo_set_source_rgba(cr, r, g, b, 1.0);
        rounded_rect(cr, x, y, fw, bh, 2);
        cairo_fill(cr);
    }
}

static void
sys_draw_ctrl_glyph(cairo_t *cr, int idx, double cx, double cy)
{
    double h = 10;
    cairo_new_path(cr);
    if (idx == 0) {
        cairo_rectangle(cr, cx - 5, cy - h / 2, 2.5, h);
        cairo_fill(cr);
        cairo_move_to(cr, cx - 2, cy);
        cairo_line_to(cr, cx + 5, cy - h / 2);
        cairo_line_to(cr, cx + 5, cy + h / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else if (idx == 2) {
        cairo_rectangle(cr, cx + 2.5, cy - h / 2, 2.5, h);
        cairo_fill(cr);
        cairo_move_to(cr, cx + 2, cy);
        cairo_line_to(cr, cx - 5, cy - h / 2);
        cairo_line_to(cr, cx - 5, cy + h / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else {
        if (strcmp(g_sys.status, "Playing") == 0) {
            cairo_rectangle(cr, cx - 5, cy - h / 2, 3, h);
            cairo_fill(cr);
            cairo_rectangle(cr, cx + 2, cy - h / 2, 3, h);
            cairo_fill(cr);
        } else {
            cairo_move_to(cr, cx - 4, cy - h / 2);
            cairo_line_to(cr, cx - 4, cy + h / 2);
            cairo_line_to(cr, cx + 5, cy);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
}

static void
sys_draw(cairo_t *cr, double mx, double my, double mh_body, double apad)
{
    (void)mh_body;
    double sx = mx + POWER_COL_W + MENU_WIDTH;
    double sxp = sx + SYS_PAD;
    double avail = SYS_COL_W - 2 * SYS_PAD;
    double th_y = my + apad + 2;

    cairo_new_path(cr);
    rounded_rect(cr, sxp, th_y, SYS_THUMB, SYS_THUMB, 4);
    cairo_set_source_rgba(cr, 0.13, 0.13, 0.13, 1.0);
    cairo_fill(cr);
    if (g_sys.art) {
        cairo_save(cr);
        rounded_rect(cr, sxp, th_y, SYS_THUMB, SYS_THUMB, 4);
        cairo_clip(cr);
        int w = cairo_image_surface_get_width(g_sys.art);
        int h = cairo_image_surface_get_height(g_sys.art);
        double sc = (w > h ? (double)SYS_THUMB / w : (double)SYS_THUMB / h);
        double dw = w * sc, dh = h * sc;
        double dx = sxp + (SYS_THUMB - dw) / 2.0;
        double dy = th_y + (SYS_THUMB - dh) / 2.0;
        cairo_set_source_surface(cr, g_sys.art, dx, dy);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_fill(cr);
        cairo_restore(cr);
    } else {
        cairo_new_path(cr);
        double cx = sxp + SYS_THUMB / 2.0, cy = th_y + SYS_THUMB / 2.0;
        cairo_set_source_rgba(cr, 1, 1, 1, 0.30);
        cairo_set_line_width(cr, 2);
        cairo_move_to(cr, cx - 1, cy + 10);
        cairo_line_to(cr, cx - 1, cy - 8);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, cx - 7, cy + 10, 6, 0, 2 * M_PI);
        cairo_arc(cr, cx + 5, cy + 10, 6, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    double tx = sxp + SYS_THUMB + 8;
    double tw = (sx + SYS_COL_W - SYS_PAD) - tx;
    const char *title = g_sys.track[0] ? g_sys.track : "No music";
    sys_draw_text(cr, tx, th_y, tw, 18, title, SYS_FONT_SIZE,
                  COLOR(MENU_FG_HEX), 1.0,
                  PANGO_ELLIPSIZE_END, PANGO_ALIGN_LEFT);
    const char *artist = g_sys.artist[0] ? g_sys.artist : g_sys.album;
    sys_draw_text(cr, tx, th_y + 17, tw, 15, artist ? artist : "",
                  SYS_FONT_SMALL, 0.75, 0.75, 0.75, 1.0,
                  PANGO_ELLIPSIZE_END, PANGO_ALIGN_LEFT);
    const char *album = (g_sys.artist[0] && g_sys.album[0]) ? g_sys.album : "";
    sys_draw_text(cr, tx, th_y + 31, tw, 15, album,
                  SYS_FONT_SMALL, 0.55, 0.55, 0.55, 1.0,
                  PANGO_ELLIPSIZE_END, PANGO_ALIGN_LEFT);

    double cy = th_y + SYS_THUMB + 6;
    double btn_w = SYS_CTRL_BTN;
    double gap = 8;
    double x0 = sxp + (avail - 3 * btn_w - 2 * gap) / 2.0;
    int has_player = g_sys.player_bus[0] != 0;
    for (int i = 0; i < 3; i++) {
        double bx = x0 + i * (btn_w + gap);
        if (has_player && g_sys.ctrl_hover == i) {
            cairo_new_path(cr);
            cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX),
                                  MENU_HOVER_A);
            rounded_rect(cr, bx, cy, btn_w, btn_w, 4);
            cairo_fill(cr);
        }
        cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX),
                              has_player ? 0.85 : 0.25);
        sys_draw_ctrl_glyph(cr, i, bx + btn_w / 2.0, cy + btn_w / 2.0);
    }

    double sy = cy + btn_w + 8;
    cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 0.2);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, sx + SYS_PAD, sy);
    cairo_line_to(cr, sx + SYS_COL_W - SYS_PAD, sy);
    cairo_stroke(cr);
    sy += 8;

    static const char *labels[SYS_STAT_COUNT] = { "RAM", "CPU", "GPU" };
    static const double colors[SYS_STAT_COUNT][3] = {
        { 0.42, 0.72, 0.45 }, { 0.40, 0.60, 0.90 }, { 0.72, 0.45, 0.85 },
    };
    static const int order[SYS_STAT_COUNT] = {
        SYS_STAT_CPU, SYS_STAT_GPU, SYS_STAT_RAM,
    };
    for (int i = 0; i < SYS_STAT_COUNT; i++) {
        int k = order[i];
        double ry = sy + i * SYS_STAT_H;
        sys_draw_text(cr, sxp, ry, 40, SYS_STAT_H, labels[k], SYS_FONT_SMALL,
                      0.75, 0.75, 0.75, 1.0, PANGO_ELLIPSIZE_NONE, PANGO_ALIGN_LEFT);
        char pct[16];
        snprintf(pct, sizeof(pct), "%.0f%%", g_sys.pct[k]);
        sys_draw_text(cr, sxp + avail - 40, ry, 40, SYS_STAT_H, pct,
                      SYS_FONT_SMALL, 0.85, 0.85, 0.85, 1.0,
                      PANGO_ELLIPSIZE_NONE, PANGO_ALIGN_RIGHT);
        sys_draw_bar(cr, sxp + 44, ry + (SYS_STAT_H - 4) / 2.0,
                     avail - 88, g_sys.pct[k],
                     colors[k][0], colors[k][1], colors[k][2]);
    }
}

static int
sys_ctrl_hit_test(double px, double py)
{
    if (!g_bar.menu_open || !g_sys.player_bus[0]) return -1;
    double mx = MENU_MARGIN_L;
    double sx = mx + POWER_COL_W + MENU_WIDTH;
    double avail = SYS_COL_W - 2 * SYS_PAD;
    double btn_w = SYS_CTRL_BTN;
    double gap = 8;
    double x0 = sx + SYS_PAD + (avail - 3 * btn_w - 2 * gap) / 2.0;
    double y0 = 2 * MENU_PADDING + 8 + SYS_THUMB + 6;
    for (int i = 0; i < 3; i++) {
        double bx = x0 + i * (btn_w + gap);
        if (in_rect(px, py, bx, y0, btn_w, btn_w))
            return i;
    }
    return -1;
}

static void
sys_tick(void)
{
    pw_reconnect_check();
    if (!g_sni.dbus_conn) {
        tray_init();
        if (g_sni.dbus_conn)
            sys_init();
    }
    if (!g_bar.menu_open) return;

    sys_stats_refresh();
    if (now_ms() - g_sys.last_refresh_ms > 4000) {
        g_sys.last_refresh_ms = now_ms();
        mpris_refresh();
    }
    int changed = 0;
    for (int i = 0; i < SYS_STAT_COUNT; i++) {
        int shown = (int)(g_sys.pct[i] + 0.5);
        if (shown != g_sys.pct_shown[i])
            changed = 1;
        g_sys.pct_shown[i] = shown;
    }
    if (changed)
        render_request();
}

static void
menu_draw(cairo_t *cr, PangoLayout *pl, double mh)
{
    if (!g_bar.menu_open || mh <= 0) return;

    int mw = MENU_WIDTH + POWER_COL_W + SYS_COL_W;
    double r = MENU_CORNER_R;
    double mx = MENU_MARGIN_L;
    double my = 0;
    double mh_body = mh - MENU_FLOAT_GAP;

        panel_begin(cr, mx, my, mw, mh_body, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

    static const char *power_labels[4] = {
        "Power off", "Reboot", "Log out", "Suspend",
    };
    int power_h = 4 * POWER_ROW_H;
    double apad = menu_vert_pad(mh_body) * 2;
    double row_base = mh_body - MENU_PADDING - power_h;
    for (int i = 0; i < 4; i++) {
        double row_y = my + row_base + i * POWER_ROW_H;

        if (g_pmenu.hover == i) {
            cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
            cairo_rectangle(cr, mx, row_y, POWER_COL_W, POWER_ROW_H);
            cairo_fill(cr);
        }

        draw_power_icon(i, cr, mx + POWER_COL_W / 2.0, row_y + POWER_ROW_H / 2.0);
    }

    menu_clamp_scroll();
    int vis_rows = menu_vis_rows(mh_body);
    int has_sb = g_app.menu_n > vis_rows;
    double text_max_x = mx + POWER_COL_W + MENU_WIDTH - MENU_PADDING - 4;
    double sby = my + mh_body - apad - MENU_SEARCH_H;
    double list_bottom, list_top;
    menu_list_area(mh_body, &list_top, &list_bottom);
    double list_h = list_bottom - list_top;
    if (has_sb) {
        cairo_save(cr);
        cairo_rectangle(cr, mx + POWER_COL_W, list_top, MENU_WIDTH, list_h);
        cairo_clip(cr);
    }
    double sofs = lround(g_bar.menu_anim_px);
    int first_pos = (int)(sofs / MENU_ROW_HEIGHT);
    for (int r = 0; r <= vis_rows; r++) {
        int pos = first_pos + r;
        if (pos >= g_app.menu_n) break;
        int i = g_app.menu_idx[pos];
        double row_y = list_top + pos * MENU_ROW_HEIGHT - sofs;
        if (row_y >= list_bottom) break;
        if (row_y + MENU_ROW_HEIGHT <= list_top) continue;

        if (pos == g_app.menu_sel && g_pointer.hover_menu_idx != i) {
            cairo_set_source_rgba(cr, COLOR(SEARCH_ACCENT_HEX), 0.28);
            cairo_rectangle(cr, mx + POWER_COL_W, row_y, MENU_WIDTH, MENU_ROW_HEIGHT);
            cairo_fill(cr);
        }

        if (g_pointer.hover_menu_idx == i) {
            cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
            cairo_rectangle(cr, mx + POWER_COL_W, row_y, MENU_WIDTH, MENU_ROW_HEIGHT);
            cairo_fill(cr);
        }

        double text_x = mx + POWER_COL_W + MENU_PADDING + 4;

        text_x += draw_icon_fit(cr, g_app.apps[i].icon, text_x, row_y,
                                0, MENU_ROW_HEIGHT, MENU_ROW_ICON, 0);

        draw_vcenter_text(cr, pl, g_app.apps[i].name, text_x, row_y, MENU_ROW_HEIGHT,
                          COLOR(MENU_FG_HEX), 1.0,
                          text_max_x - text_x, has_sb);
    }
    if (has_sb) cairo_restore(cr);

    double sbx = mx + POWER_COL_W + MENU_PADDING + 2;
    double sbw = MENU_WIDTH - 2 * (MENU_PADDING + 2);
    cairo_set_source_rgba(cr, COLOR(SEARCH_BG_HEX), SEARCH_BG_A);
    cairo_new_path(cr);
    rounded_rect(cr, sbx, sby, sbw, MENU_SEARCH_H, 4);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, COLOR(SEARCH_ACCENT_HEX), 0.6);
    cairo_set_line_width(cr, 1);
    rounded_rect(cr, sbx + 0.5, sby + 0.5, sbw - 1, MENU_SEARCH_H - 1, 4);
    cairo_stroke(cr);

    double stx = sbx + 8;
    if (g_app.search[0]) {
        stx += draw_vcenter_text(cr, pl, g_app.search, stx, sby, MENU_SEARCH_H,
                                 COLOR(SEARCH_FG_HEX), 1.0, 0, 0);
    } else {
        stx += draw_vcenter_text(cr, pl, "Search apps...", stx, sby, MENU_SEARCH_H,
                                 COLOR(SEARCH_HINT_HEX), 1.0, 0, 0);
    }
    cairo_set_source_rgba(cr, COLOR(SEARCH_FG_HEX), 0.9);
    cairo_rectangle(cr, stx + 2, sby + 6, 2, MENU_SEARCH_H - 12);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 0.2);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, mx + POWER_COL_W, my + apad);
    cairo_line_to(cr, mx + POWER_COL_W, my + mh_body - MENU_PADDING);
    cairo_stroke(cr);
    cairo_move_to(cr, mx + POWER_COL_W + MENU_WIDTH, my + apad);
    cairo_line_to(cr, mx + POWER_COL_W + MENU_WIDTH, my + mh_body - MENU_PADDING);
    cairo_stroke(cr);

    sys_draw(cr, mx, my, mh_body, apad);

    if (g_ctx.open) {
        double cr2_x = g_ctx.x;
        double cr2_y = g_ctx.y;
        double cw = CTX_WIDTH;
        double ch = g_ctx.n_items * CTX_ROW_HEIGHT;
        double r = CTX_CORNER_R;

        panel_begin(cr, cr2_x, cr2_y, cw, ch, r, CTX_SHADOW_W, CTX_SHADOW_A, 0);

        const char *pin_label = pinned_is(g_ctx.target_app) ? "Unpin from bar" : "Pin to bar";

        for (int i = 0; i < g_ctx.n_items; i++) {
            double row_y = cr2_y + i * CTX_ROW_HEIGHT;

            if (g_pointer.hover_ctx_idx == i) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                cairo_rectangle(cr, cr2_x, row_y, cw, CTX_ROW_HEIGHT);
                cairo_fill(cr);
            }

            draw_vcenter_text(cr, pl, i == 0 ? pin_label : "Launch", cr2_x + 10,
                              row_y, CTX_ROW_HEIGHT,
                              COLOR(MENU_FG_HEX), 1.0, 0, 0);
        }

        cairo_restore(cr);
    }

    if (g_pmenu.hover >= 0) {
        const char *label = power_labels[g_pmenu.hover];
        pango_layout_set_text(pl, label, -1);
        int tw, th;
        pango_layout_get_pixel_size(pl, &tw, &th);
        double hover_y = my + row_base + g_pmenu.hover * POWER_ROW_H;
        double tx = mx + POWER_COL_W + 8;
        double ty = hover_y + (POWER_ROW_H - th) / 2.0 - 3;
        double tw_w = tw + 12;
        double tw_h = th + 8;
        if (tx + tw_w > mx + mw - 20) tx = mx + mw - 20 - tw_w;
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, 0.13, 0.13, 0.13, 0.98);
        cairo_new_path(cr);
        rounded_rect(cr, tx, ty, tw_w, tw_h, 4);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 1.0);
        cairo_set_line_width(cr, 1);
        cairo_new_path(cr);
        rounded_rect(cr, tx, ty, tw_w, tw_h, 4);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), 1.0);
        cairo_move_to(cr, tx + 6, ty + 4);
        pango_cairo_show_layout(cr, pl);
    }

    cairo_restore(cr);
}

static int
icon_content_box(cairo_surface_t *sf, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = cairo_image_surface_get_width(sf);
    *y0 = cairo_image_surface_get_height(sf);
    *x1 = -1; *y1 = -1;
    int iw = *x0, ih = *y0;
    if (iw <= 0 || ih <= 0) return 0;
    int stride = cairo_image_surface_get_stride(sf);
    unsigned char *d = cairo_image_surface_get_data(sf);
    for (int y = 0; y < ih; y++) {
        const unsigned char *row = d + (size_t)y * stride;
        for (int x = 0; x < iw; x++) {
            if (row[(size_t)x * 4 + 3] > 0x20) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
        }
    }
    return *x1 >= *x0;
}

static void
ws_button_draw(cairo_t *cr, double bx, double by, const char *label,
               const double fill[4], const double stroke[4], const double text[4])
{
    if (fill) {
        cairo_set_source_rgba(cr, fill[0], fill[1], fill[2], fill[3]);
        cairo_new_path(cr);
        rounded_rect(cr, bx, by, WS_BTN_SIZE, WS_BTN_SIZE, WS_BTN_R);
        cairo_fill(cr);
    }
    if (stroke) {
        cairo_set_source_rgba(cr, stroke[0], stroke[1], stroke[2], stroke[3]);
        cairo_set_line_width(cr, 1.2);
        cairo_new_path(cr);
        rounded_rect(cr, bx, by, WS_BTN_SIZE, WS_BTN_SIZE, WS_BTN_R);
        cairo_stroke(cr);
    }
    sys_draw_text(cr, bx, by, WS_BTN_SIZE, WS_BTN_SIZE, label, FONT_SIZE,
                  text[0], text[1], text[2], text[3],
                  PANGO_ELLIPSIZE_NONE, PANGO_ALIGN_CENTER);
}

static void
bar_draw(struct bar *b)
{
    if (!b->configured || !b->cr) return;

    if (cairo_status(b->cr) != CAIRO_STATUS_SUCCESS) {
        int s = b->scale ? b->scale : 1;
        int pw = b->width * s;
        int ph = b->height * s;
        int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
        pool_cairo_init(&b->pool, &b->cairo, &b->cr, pw, ph, stride, s);
    }

    int w = b->width;
    int bar_h = BAR_HEIGHT;
    double bar_y = bar_y_offset();
    cairo_t *cr = b->cr;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_new_path(cr);

    PangoFontDescription *fd = pango_font_description_from_string(FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, FONT_SIZE * PANGO_SCALE);
    PangoLayout *pl = pango_ctx_layout(cr);
    pango_layout_set_font_description(pl, fd);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, COLOR(BG_HEX), BG_A);
    cairo_rectangle(cr, 0, bar_y, w, bar_h);
    cairo_fill(cr);

    {
        double bell_sz = NOTIF_ICON_SIZE;
        double bell_x = b->width - TRAY_EDGE_PAD - bell_sz;
        double left = workspace_icon_x();
        double pill_x = left - PILL_PAD_X;
        double pill_w = (bell_x + bell_sz - left) + 2 * PILL_PAD_X;
        double pill_y = bar_y + PILL_PAD_Y;
        double pill_h = bar_h - 2 * PILL_PAD_Y;
        cairo_set_source_rgba(cr, COLOR(PILL_HEX), PILL_A);
        cairo_new_path(cr);
        rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, pill_h / 2.0);
        cairo_fill(cr);
    }

    double pill_w = MENU_PILL_W;
    double pill_h = MENU_PILL_H;
    double icon_x = 10;
    double icon_y = bar_y + (bar_h - pill_h) / 2.0;

    cairo_set_line_width(cr, 1.5);
    if (g_pointer.hover_pill) {
        cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
        cairo_new_path(cr);
        rounded_rect(cr, icon_x, icon_y, pill_w, pill_h, pill_h / 2.0);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, COLOR(FG_HEX), FG_A);
        cairo_new_path(cr);
        rounded_rect(cr, icon_x, icon_y, pill_w, pill_h, pill_h / 2.0);
        cairo_stroke(cr);
    } else {
        cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.3);
        cairo_new_path(cr);
        rounded_rect(cr, icon_x, icon_y, pill_w, pill_h, pill_h / 2.0);
        cairo_stroke(cr);
    }

    for (int i = 0; i < g_sni.n_tray; i++) {
        double tsz = TRAY_ICON_SIZE;
        double tx = tray_icon_x(i);
        double ty = bar_y + (bar_h - tsz) / 2.0;
        double tcx = tx + tsz / 2.0;
        double tcy = ty + tsz / 2.0;

        if (g_pointer.hover_tray == i) draw_hover_circle(cr, tcx, tcy, tsz, 6);

        if (g_tray[i].icon) {
            int iw = cairo_image_surface_get_width(g_tray[i].icon);
            int ih = cairo_image_surface_get_height(g_tray[i].icon);
            int ox = 0, oy = 0, bx = 0, by = 0;
            double sc, dw, dh;
            if (icon_content_box(g_tray[i].icon, &ox, &oy, &bx, &by) &&
                bx > ox && by > oy) {
                int cw = bx - ox + 1, ch = by - oy + 1;
                sc = fmin((double)TRAY_ICON_MAX / cw, (double)TRAY_ICON_MAX / ch);
                dw = cw * sc; dh = ch * sc;
            } else {
                if (iw <= 0 || ih <= 0) continue;
                sc = fmin((double)tsz / iw, (double)tsz / ih);
                dw = iw * sc; dh = ih * sc;
            }
            cairo_save(cr);
            cairo_translate(cr, tcx - dw / 2.0, tcy - dh / 2.0);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, g_tray[i].icon, -ox, -oy);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }

    {
        double tsz = WS_ICON_SIZE;
        double tx = workspace_icon_x();
        double ty = bar_y + (bar_h - tsz) / 2.0;
        double tcx = tx + tsz / 2.0;
        double tcy = ty + tsz / 2.0;

        if (g_pointer.hover_ws) draw_hover_circle(cr, tcx, tcy, tsz, 6);

        double csz = (tsz - 7) / 2.0;
        double gx = tx + 2, gy = ty + 2;
        cairo_set_line_width(cr, 1.2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        for (int r = 0; r < 2; r++) {
            for (int c = 0; c < 2; c++) {
                int idx = r * 2 + c;
                int active = (idx < g_ws.ws_count) && g_workspaces[idx].active;
                double cx = gx + c * (csz + 3);
                double cy = gy + r * (csz + 3);
                if (active) {
                    cairo_set_source_rgba(cr, COLOR(FG_HEX), 1.0);
                    cairo_new_path(cr);
                    rounded_rect(cr, cx, cy, csz, csz, 1.5);
                    cairo_fill(cr);
                } else {
                    cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.4);
                    cairo_new_path(cr);
                    rounded_rect(cr, cx, cy, csz, csz, 1.5);
                    cairo_stroke(cr);
                }
            }
        }
    }

    if (g_box.net_status != NET_NONE) {
        double nsz = NOTIF_ICON_SIZE;
        double nx = net_icon_x();
        double ny = bar_y + (bar_h - nsz) / 2.0;
        double ncx = nx + nsz / 2.0;
        double ncy = ny + nsz / 2.0;

        if (g_pointer.hover_net) draw_hover_circle(cr, ncx, ncy, nsz, 6);

        cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.95);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_width(cr, 1.8);

        if (g_box.net_status == NET_WIFI) {
            cairo_arc(cr, ncx, ncy + 3.5, 1.6, 0, 2 * M_PI);
            cairo_fill(cr);
            for (int k = 0; k < 3; k++) {
                double r = 3.2 + k * 2.7;
                cairo_arc(cr, ncx, ncy + 3.5, r,
                          205 * M_PI / 180.0, 335 * M_PI / 180.0);
                cairo_stroke(cr);
            }
        } else {
            double jw = 10, jh = 7;
            cairo_rectangle(cr, ncx - jw / 2.0, ncy - 4.5, jw, jh);
            cairo_stroke(cr);
            cairo_move_to(cr, ncx, ncy + 2.5);
            cairo_line_to(cr, ncx, ncy + 5);
            cairo_stroke(cr);
            cairo_move_to(cr, ncx - 3, ncy - 4.5);
            cairo_line_to(cr, ncx - 3, ncy - 2);
            cairo_move_to(cr, ncx + 3, ncy - 4.5);
            cairo_line_to(cr, ncx + 3, ncy - 2);
            cairo_stroke(cr);
        }
    }

    if (g_box.audio_present) {
        double asz = NOTIF_ICON_SIZE;
        double ax = audio_icon_x();
        double ay = bar_y + (bar_h - asz) / 2.0;
        double acx = ax + asz / 2.0;
        double acy = ay + asz / 2.0;

        if (g_pointer.hover_audio) draw_hover_circle(cr, acx, acy, asz, 6);

        draw_speaker(cr, acx, acy, 5.5, 3, 3, 6, 4, 2.2, 4.2, 0.95);
    }

    {
        for (int i = 0; i < g_box.n_pl; i++) {
            if (g_app.pinned[i] >= g_app.n_apps) continue;
            struct app_entry *a = &g_app.apps[g_app.pinned[i]];
            int running = pinned_running(i);
            int focused = 0;
            int bcount = 0;
            if (running) {
                int wins[MAX_WINDOWS_PER_GROUP];
                bcount = windows_of_app(a, wins, MAX_WINDOWS_PER_GROUP);
                for (int w = 0; w < g_n_toplevels; w++) {
                    if (app_matches_id(a, g_toplevels[w].app_id) &&
                        g_toplevels[w].activated) {
                        focused = 1; break;
                    }
                }
            }
            draw_launcher_item(cr, a->icon, g_box.pl[i].x, g_box.pl[i].w, bar_y, bar_h,
                               PINNED_ICON, g_pointer.hover_pinned_idx == i,
                               focused, running, bcount);
        }
    }

    {
        for (int i = 0; i < g_box.n_taskbar_groups; i++) {
            struct taskbar_group *g = &g_box.taskbar_groups[i];
            int focused = 0;
            for (int w = 0; w < g->n_windows; w++) {
                if (g_toplevels[g->windows[w]].activated) { focused = 1; break; }
            }
            draw_launcher_item(cr, g_toplevels[g->windows[0]].icon,
                               g->x, g->w, bar_y, bar_h, TASKBAR_ICON,
                               g_pointer.hover_taskbar_idx == i, focused, 1,
                               g->n_windows);
        }
    }

    {
        double bell_sz = NOTIF_ICON_SIZE;
        double bell_x = b->width - TRAY_EDGE_PAD - bell_sz;
        double bell_y = bar_y + (bar_h - bell_sz) / 2.0;
        double cx = bell_x + bell_sz / 2.0;

        PangoLayout *clk = clock_layout(cr);
        int clk_tw, clk_th;
        pango_layout_get_pixel_size(clk, &clk_tw, &clk_th);
        double clk_x = bell_x - TRAY_GAP - clk_tw;

        if (g_pointer.hover_clock) {
            cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
            cairo_new_path(cr);
            rounded_rect(cr, clk_x - 7, bar_y + 4,
                         clk_tw + 14, BAR_HEIGHT - 8, MENU_CORNER_R);
            cairo_fill(cr);
        }

        cairo_set_source_rgba(cr, COLOR(FG_HEX), FG_A);
        cairo_move_to(cr, clk_x, bar_y + (bar_h - clk_th) / 2.0);
        pango_cairo_show_layout(cr, clk);

        if (g_pointer.hover_bell) draw_hover_circle(cr, cx, bell_y + bell_sz / 2.0, bell_sz, 6);

        cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.95);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_width(cr, 1.8);

        double top_cy = bell_y + 2.0;
        double bell_r = bell_sz * 0.38;
        double bot_y = bell_y + bell_sz - 4.5;
        double bot_w = bell_sz * 0.5;

        cairo_move_to(cr, cx - bot_w / 2.0, bot_y);
        cairo_line_to(cr, cx - bell_r, top_cy + bell_r);
        cairo_arc(cr, cx, top_cy + bell_r, bell_r, M_PI, 0);
        cairo_line_to(cr, cx + bot_w / 2.0, bot_y);
        cairo_stroke(cr);

        cairo_move_to(cr, cx - bot_w / 2.0 - 2, bot_y);
        cairo_line_to(cr, cx + bot_w / 2.0 + 2, bot_y);
        cairo_stroke(cr);

        cairo_arc(cr, cx, bot_y + 2.0, 1.6, 0, 2 * M_PI);
        cairo_fill(cr);

        if (notif_unread_count() > 0) {
            cairo_set_source_rgba(cr, 0.9, 0.25, 0.25, 1.0);
            cairo_arc(cr, cx + bell_sz / 2.0 - 2, bell_y + 3, 3, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }

    if (g_gpopup.open) {
        double pw = GROUP_POPUP_W;
        double ph = g_gpopup.height - MENU_FLOAT_GAP;
        double px = g_gpopup.x;
        double py = g_gpopup.y;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, ph, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        for (int i = 0; i < g_gpopup.n_windows; i++) {
            double row_y = py + MENU_PADDING + i * MENU_ROW_HEIGHT;
            if (g_gpopup.hover_idx == i) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                cairo_rectangle(cr, px, row_y, pw, MENU_ROW_HEIGHT);
                cairo_fill(cr);
            }
            int tl_idx = g_gpopup.windows[i];
            struct toplevel_entry *t = &g_toplevels[tl_idx];
            double text_x = px + MENU_PADDING;
            text_x += draw_icon_fit(cr, t->icon, text_x, row_y,
                                    0, MENU_ROW_HEIGHT, MENU_ROW_ICON, 0);
            double xc = px + pw - MENU_PADDING - 8;
            double yc = row_y + MENU_ROW_HEIGHT / 2.0;

            const char *title = t->title;
            double max_text_w = xc - text_x - 10;
            draw_vcenter_text(cr, pl, title && title[0] ? title : "(untitled)",
                              text_x, row_y, MENU_ROW_HEIGHT,
                              COLOR(MENU_FG_HEX), 1.0,
                              max_text_w, 1);
            if (g_gpopup.hover_close == i) {
                cairo_set_source_rgba(cr, 0.85, 0.2, 0.2, 0.9);
                cairo_new_path(cr);
                cairo_arc(cr, xc, yc, 8, 0, 2 * M_PI);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            } else {
                cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), 0.55);
            }
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_width(cr, 1.5);
            cairo_move_to(cr, xc - 3, yc - 3);
            cairo_line_to(cr, xc + 3, yc + 3);
            cairo_stroke(cr);
            cairo_move_to(cr, xc + 3, yc - 3);
            cairo_line_to(cr, xc - 3, yc + 3);
            cairo_stroke(cr);
        }

        cairo_restore(cr);
    }

    if (g_cpop.open) {
        double px = g_cpop.x;
        double py = g_cpop.y;
        double pw = g_cpop.w;
        double ph = g_cpop.h - MENU_FLOAT_GAP;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, ph, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        for (int i = 0; i < g_cpop.n_items; i++) {
            double ry = py + MENU_PADDING + i * CTX_ROW_HEIGHT;

            if (g_cpop.hover == i) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                cairo_rectangle(cr, px + 1, ry, pw - 2, CTX_ROW_HEIGHT);
                cairo_fill(cr);
            }

            if (i > 0) {
                cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 0.4);
                cairo_set_line_width(cr, 1);
                cairo_move_to(cr, px + MENU_PADDING + 4, ry);
                cairo_line_to(cr, px + pw - MENU_PADDING - 4, ry);
                cairo_stroke(cr);
            }

            draw_vcenter_text(cr, pl, cpop_label(i), px + MENU_PADDING + 2, ry,
                              CTX_ROW_HEIGHT,
                              COLOR(MENU_FG_HEX), 1.0, 0, 0);
        }

        cairo_restore(cr);
    }

    if (g_tm.open && g_tm.n_row > 0) {
        double px = g_tm.x;
        double py = g_tm.y;
        double pw = g_tm.w;
        double ph = g_tm.height - MENU_FLOAT_GAP;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, ph, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        for (int i = 0; i < g_tm.n_row; i++) {
            struct tray_menu_entry *e = &g_tm.e[g_tm.row[i].e_idx];
            double row_y = g_tm.row[i].y0;
            double row_h = g_tm.row[i].y1 - g_tm.row[i].y0;

            if (e->is_separator) {
                double sy = row_y + row_h / 2.0;
                cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 0.4);
                cairo_set_line_width(cr, 1);
                cairo_move_to(cr, px + MENU_PADDING + 4, sy);
                cairo_line_to(cr, px + pw - MENU_PADDING - 4, sy);
                cairo_stroke(cr);
                continue;
            }

            if (g_tm.hover_row == i && e->enabled) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                cairo_rectangle(cr, px + 1, row_y, pw - 2, row_h);
                cairo_fill(cr);
            }

            double text_x = px + MENU_PADDING;
            if (e->depth > 0)
                text_x += e->depth * 10;

            if (e->is_check || e->is_radio) {
                double cx = px + pw - MENU_PADDING - 8;
                double cy = row_y + row_h / 2.0;
                int state = (e->toggle_state == 1);
                if (e->is_radio) {
                    cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), state ? 1.0 : 0.35);
                    cairo_set_line_width(cr, 1.5);
                    cairo_new_path(cr);
                    cairo_arc(cr, cx, cy, 5, 0, 2 * M_PI);
                    cairo_stroke(cr);
                    if (state) {
                        cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), 1.0);
                        cairo_new_path(cr);
                        cairo_arc(cr, cx, cy, 2.6, 0, 2 * M_PI);
                        cairo_fill(cr);
                    }
                } else {
                    cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), state ? 1.0 : 0.35);
                    cairo_set_line_width(cr, 2);
                    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                    cairo_move_to(cr, cx - 4.5, cy);
                    cairo_line_to(cr, cx - 1.5, cy + 3.5);
                    cairo_line_to(cr, cx + 4.5, cy - 4);
                    cairo_stroke(cr);
                }
            }

            const char *label = e->label;
            draw_vcenter_text(cr, pl, label && label[0] ? label : "", text_x,
                              row_y, row_h,
                              COLOR(MENU_FG_HEX),
                              e->enabled ? 1.0 : 0.4, 0, 0);
        }

        cairo_restore(cr);
    }

    if (g_volpop.open) {
        if (g_box.audio_present)
            audio_snapshot(&g_box.vol_snap);

        double px = g_volpop.x;
        double py = g_volpop.y;
        double pw = g_volpop.w;
        double body_h = g_volpop.h - MENU_FLOAT_GAP;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, body_h, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        int cur_tab = g_volpop.tab;
        double tabw = (pw - 2 * MENU_PADDING - 4) / 2.0;
        for (int t = 0; t < 2; t++) {
            double tx = px + MENU_PADDING + t * (tabw + 4);
            double ty = g_volpop.tab_y;
            double tw = tabw;
            double th = VOL_TAB_H;
            if (t == cur_tab) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                cairo_new_path(cr);
                rounded_rect(cr, tx, ty, tw, th, 4);
                cairo_fill(cr);
            } else if (g_volpop.hover_tab == t) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), 0.6);
                cairo_new_path(cr);
                rounded_rect(cr, tx, ty, tw, th, 4);
                cairo_fill(cr);
            }
            const char *label = t == 0 ? "Output" : "Input";
            pango_layout_set_text(pl, label, -1);
            int ltw, lth;
            pango_layout_get_pixel_size(pl, &ltw, &lth);
            cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX),
                                  t == cur_tab ? 1.0 : 0.55);
            cairo_move_to(cr, tx + (tw - ltw) / 2.0, ty + (th - lth) / 2.0);
            pango_cairo_show_layout(cr, pl);
        }

        {
            int want_sink = (cur_tab == 0);
            int n_rows = volpop_list_rows();
            int row = 0;
            for (int i = 0; i < PW_MAX_DEVICES; i++) {
                if (g_box.vol_snap.n_sinks + g_box.vol_snap.n_sources == 0) break;
                if (row >= n_rows) break;
                struct audio_snap_item *it = &g_box.vol_snap.items[i];
                if (it->is_sink != want_sink) continue;
                double ry = g_volpop.list_y + row * VOL_DEV_ROW_H;
                if (g_volpop.hover_dev == row) {
                    cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                    cairo_rectangle(cr, px + 1, ry, pw - 2, VOL_DEV_ROW_H);
                    cairo_fill(cr);
                }
                double rcy = ry + VOL_DEV_ROW_H / 2.0;
                double dx = px + MENU_PADDING + 5;
                if (it->is_default) {
                    cairo_set_source_rgba(cr, 0.4, 0.8, 0.4, 1.0);
                    cairo_arc(cr, dx, rcy, 4, 0, 2 * M_PI);
                    cairo_fill(cr);
                } else {
                    cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX),
                                          g_volpop.hover_dev == row ? 0.75 : 0.38);
                    cairo_arc(cr, dx, rcy, 3.2, 0, 2 * M_PI);
                    cairo_fill(cr);
                }
                const char *dtxt = it->desc[0] ? it->desc : "device";
                pango_layout_set_text(pl, dtxt, -1);
                int dtw, dth;
                pango_layout_get_pixel_size(pl, &dtw, &dth);
                double dlx = dx + 12;
                double max_w = px + pw - MENU_PADDING - dlx;
                if (dtw > max_w && max_w > 0) {
                    char tmp[160];
                    snprintf(tmp, sizeof(tmp), "%s", dtxt);
                    while (dtw > max_w && tmp[0]) {
                        char *u = g_utf8_prev_char(tmp + strlen(tmp));
                        *u = '\0';
                        pango_layout_set_text(pl, tmp, -1);
                        pango_layout_get_pixel_size(pl, &dtw, &dth);
                    }
                    pango_layout_set_text(pl, tmp, -1);
                }
                cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), 0.92);
                cairo_move_to(cr, dlx, ry + (VOL_DEV_ROW_H - dth) / 2.0);
                pango_cairo_show_layout(cr, pl);
                row++;
            }
        }

        double mcx = g_volpop.mute_x + g_volpop.mute_w / 2.0;
        double mcy = g_volpop.mute_y + g_volpop.mute_h / 2.0;

        if (g_volpop.hover_mute) draw_hover_circle(cr, mcx, mcy, g_volpop.mute_w, 5);

        double vol = cur_tab == 0 ? g_box.vol_snap.sink_volume : g_box.vol_snap.source_volume;
        int muted = cur_tab == 0 ? g_box.vol_snap.sink_muted : g_box.vol_snap.source_muted;
        draw_speaker(cr, mcx, mcy, 5.5, 3, 3, 6, 4, 2.2, 4.2,
                     muted ? 0.4 : 0.95);

        if (muted) {
            cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.95);
            cairo_move_to(cr, mcx + 2.5, mcy - 7);
            cairo_line_to(cr, mcx + 9.5, mcy + 7);
            cairo_move_to(cr, mcx + 9.5, mcy - 7);
            cairo_line_to(cr, mcx + 2.5, mcy + 7);
            cairo_stroke(cr);
        }

        double sx = g_volpop.slider_x_min;
        double sw = g_volpop.slider_x_max - g_volpop.slider_x_min;
        double sy = g_volpop.slider_y;
        double fill_w = 0;
        if (g_box.vol_snap.has_value) {
            fill_w = sw * (vol / VOLUME_MAX);
            fill_w = clampd(fill_w, 0, sw);
        }
        double sr = VOL_SLIDER_H / 2.0;

        cairo_set_source_rgba(cr, COLOR(VOL_SLIDER_BG_HEX), VOL_SLIDER_BG_A);
        cairo_new_path(cr);
        rounded_rect(cr, sx, sy - sr, sw, VOL_SLIDER_H, sr);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, COLOR(VOL_SLIDER_FILL_HEX), VOL_SLIDER_FILL_A);
        if (fill_w >= sr * 2) {
            cairo_new_path(cr);
            rounded_rect(cr, sx, sy - sr, fill_w, VOL_SLIDER_H, sr);
        } else {
            cairo_rectangle(cr, sx, sy - sr, fill_w, VOL_SLIDER_H);
        }
        cairo_fill(cr);

        double kx = sx + fill_w;
        kx = clampd(kx, sx, sx + sw);
        cairo_set_source_rgba(cr, COLOR(FG_HEX), 1.0);
        cairo_arc(cr, kx, sy, VOL_KNOB_R, 0, 2 * M_PI);
        cairo_fill(cr);

        char vlabel[8];
        snprintf(vlabel, sizeof(vlabel), "%d%%",
                 (int)lroundf(g_box.vol_snap.has_value ? vol : 0.0f));
        pango_layout_set_text(pl, vlabel, -1);
        int vtw, vth;
        pango_layout_get_pixel_size(pl, &vtw, &vth);
        cairo_set_source_rgba(cr, COLOR(MENU_FG_HEX), 1.0);
        cairo_move_to(cr, px + pw - MENU_PADDING - vtw, sy - vth / 2.0);
        pango_cairo_show_layout(cr, pl);
        (void)vtw;

        cairo_restore(cr);
    }

    if (g_wspop.open) {
        double px = g_wspop.x;
        double py = g_wspop.y;
        double pw = g_wspop.w;
        double body_h = g_wspop.h;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, body_h, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        /* Wallpaper button (top row) */
        {
            double wbx = px + MENU_PADDING;
            double wby = py + MENU_PADDING;
            double wbw = pw - MENU_PADDING * 2;
            int wp_hover = (g_wspop.hover_row == -2);

            if (wp_hover) {
                cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
                cairo_new_path(cr);
                rounded_rect(cr, wbx, wby, wbw, WS_WP_BTN_H, WS_WP_BTN_R);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.8);
                cairo_set_line_width(cr, 1.2);
                cairo_new_path(cr);
                rounded_rect(cr, wbx, wby, wbw, WS_WP_BTN_H, WS_WP_BTN_R);
                cairo_stroke(cr);
            } else {
                cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.45);
                cairo_set_line_width(cr, 1.2);
                cairo_new_path(cr);
                rounded_rect(cr, wbx, wby, wbw, WS_WP_BTN_H, WS_WP_BTN_R);
                cairo_stroke(cr);
            }

            /* Picture icon */
            double isz = WS_WP_ICON;
            double ix = wbx + 10;
            double iy = wby + (WS_WP_BTN_H - isz) / 2.0;
            double ia = wp_hover ? 1.0 : 0.7;
            cairo_set_source_rgba(cr, COLOR(FG_HEX), ia);
            cairo_set_line_width(cr, 1.3);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            /* frame */
            cairo_new_path(cr);
            rounded_rect(cr, ix, iy, isz, isz, 2);
            cairo_stroke(cr);
            /* mountain peaks */
            cairo_move_to(cr, ix + 2, iy + isz - 3);
            cairo_line_to(cr, ix + isz / 2.0 - 1, iy + 4);
            cairo_line_to(cr, ix + isz - 2, iy + isz - 3);
            cairo_stroke(cr);
            /* sun dot */
            cairo_arc(cr, ix + isz - 4, iy + 4, 1.5, 0, 2 * M_PI);
            cairo_fill(cr);

            /* Label */
            sys_draw_text(cr, ix + isz + 8, wby, wbw - isz - 20, WS_WP_BTN_H,
                          "Wallpaper", FONT_SIZE,
                          COLOR(FG_HEX), wp_hover ? 1.0 : 0.8,
                          PANGO_ELLIPSIZE_NONE, PANGO_ALIGN_LEFT);
        }

        /* Workspace buttons (bottom row) */
        for (int i = 0; i < g_ws.ws_count; i++) {
            double step = WS_BTN_SIZE + WS_BTN_GAP;
            double bx = px + MENU_PADDING + i * step;
            double by = py + MENU_PADDING + WS_WP_BTN_H + MENU_PADDING;
            int active = g_workspaces[i].active;
            int hover = (g_wspop.hover_row == i);
            const char *name = g_workspaces[i].name;

            if (hover && active) {
                ws_button_draw(cr, bx, by, name,
                               (double[4]){ 0.55, 0.9, 0.55, 1.0 }, NULL,
                               (double[4]){ 0.1, 0.1, 0.1, 1.0 });
            } else if (hover) {
                ws_button_draw(cr, bx, by, name,
                               (double[4]){ COLOR(MENU_HOVER_HEX), MENU_HOVER_A },
                               (double[4]){ COLOR(FG_HEX), 0.8 },
                               (double[4]){ COLOR(FG_HEX), 1.0 });
            } else if (active) {
                ws_button_draw(cr, bx, by, name,
                               (double[4]){ 0.4, 0.8, 0.4, 1.0 }, NULL,
                               (double[4]){ 0.1, 0.1, 0.1, 1.0 });
            } else {
                ws_button_draw(cr, bx, by, name,
                               NULL, (double[4]){ COLOR(FG_HEX), 0.45 },
                               (double[4]){ COLOR(FG_HEX), 0.7 });
            }
        }
        cairo_restore(cr);
    }

    if (g_calpop.open) {
        static const char *mon_names[12] = {
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December",
        };
        static const char *dow_names[7] = {
            "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa",
        };

        double px = g_calpop.x;
        double py = g_calpop.y;
        double pw = g_calpop.w;
        double body_h = g_calpop.h - MENU_FLOAT_GAP;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, body_h, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        double inner_w = pw - MENU_PADDING * 2;
        double cell_w = inner_w / 7.0;
        double hdr_y = py + MENU_PADDING;
        double dow_y = hdr_y + CAL_HEADER_H;
        double grid_y = dow_y + CAL_DOW_H;

        char hdr[32];
        snprintf(hdr, sizeof(hdr), "%s %d",
                 mon_names[g_calpop.view_mon], g_calpop.view_year);
        pango_layout_set_font_description(pl, clock_font());
        int htw, hth;
        pango_layout_set_text(pl, hdr, -1);
        pango_layout_get_pixel_size(pl, &htw, &hth);
        cairo_set_source_rgba(cr, COLOR(FG_HEX), FG_A);
        cairo_move_to(cr, px + (pw - htw) / 2.0,
                      hdr_y + (CAL_HEADER_H - hth) / 2.0);
        pango_cairo_show_layout(cr, pl);

        double alx = px + MENU_PADDING + CAL_ARROW_W / 2.0;
        double arx = px + pw - MENU_PADDING - CAL_ARROW_W / 2.0;
        double acy = hdr_y + CAL_HEADER_H / 2.0;

        if (g_calpop.hover_prev || g_calpop.hover_next) {
            cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX), MENU_HOVER_A);
            cairo_new_path(cr);
            if (g_calpop.hover_prev)
                rounded_rect(cr, alx - CAL_ARROW_W / 2.0, hdr_y,
                             CAL_ARROW_W, CAL_HEADER_H, MENU_CORNER_R);
            else
                rounded_rect(cr, arx - CAL_ARROW_W / 2.0, hdr_y,
                             CAL_ARROW_W, CAL_HEADER_H, MENU_CORNER_R);
            cairo_fill(cr);
        }

        cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.95);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_width(cr, 1.8);

        cairo_move_to(cr, alx + 3, acy - 5);
        cairo_line_to(cr, alx - 3, acy);
        cairo_line_to(cr, alx + 3, acy + 5);
        cairo_stroke(cr);

        cairo_move_to(cr, arx - 3, acy - 5);
        cairo_line_to(cr, arx + 3, acy);
        cairo_line_to(cr, arx - 3, acy + 5);
        cairo_stroke(cr);

        pango_layout_set_font_description(pl, cal_small_font());
        for (int i = 0; i < 7; i++) {
            int dw, dh;
            pango_layout_set_text(pl, dow_names[i], -1);
            pango_layout_get_pixel_size(pl, &dw, &dh);
            cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.45);
            cairo_move_to(cr, px + MENU_PADDING + i * cell_w +
                                (cell_w - dw) / 2.0,
                          dow_y + (CAL_DOW_H - dh) / 2.0);
            pango_cairo_show_layout(cr, pl);
        }

        time_t now_t = time(NULL);
        struct tm now_tm;
        localtime_r(&now_t, &now_tm);
        int today_y = now_tm.tm_year + 1900;
        int today_m = now_tm.tm_mon;
        int today_d = now_tm.tm_mday;

        int first = cal_first_weekday(g_calpop.view_year, g_calpop.view_mon);
        int ndays = cal_days_in_month(g_calpop.view_year, g_calpop.view_mon);

        pango_layout_set_font_description(pl, cal_day_font());
        for (int d = 1; d <= ndays; d++) {
            int slot = first + d - 1;
            int col = slot % 7;
            int row = slot / 7;
            double cx0 = px + MENU_PADDING + col * cell_w;
            double cy0 = grid_y + row * CAL_CELL_H;
            int is_today = (d == today_d && g_calpop.view_mon == today_m &&
                            g_calpop.view_year == today_y);

            char num[8];
            snprintf(num, sizeof(num), "%d", d);
            int dw, dh;
            pango_layout_set_text(pl, num, -1);
            pango_layout_get_pixel_size(pl, &dw, &dh);

            if (is_today) {
                double ir = dh / 2.0 + 6.0;
                cairo_set_source_rgba(cr, COLOR(FG_HEX), FG_A);
                cairo_new_path(cr);
                cairo_arc(cr, cx0 + cell_w / 2.0, cy0 + CAL_CELL_H / 2.0,
                          ir, 0, 2 * M_PI);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
            } else {
                cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.85);
            }

            cairo_move_to(cr, cx0 + (cell_w - dw) / 2.0,
                          cy0 + (CAL_CELL_H - dh) / 2.0);
            pango_cairo_show_layout(cr, pl);
        }

        cairo_restore(cr);
    }

    if (g_npop.open) {
        double px = g_npop.x;
        double py = g_npop.y;
        double pw = g_npop.w;
        double body_h = g_npop.h - MENU_FLOAT_GAP;
        double r = MENU_CORNER_R;

        panel_begin(cr, px, py, pw, body_h, r, MENU_SHADOW_W, MENU_SHADOW_A, 1);

        pango_layout_set_font_description(pl, notif_font());

        char hdr[64];
        snprintf(hdr, sizeof(hdr), "Notifications  %d", g_n_notifs);
        pango_layout_set_text(pl, hdr, -1);
        int htw, hth;
        pango_layout_get_pixel_size(pl, &htw, &hth);
        cairo_set_source_rgba(cr, COLOR(FG_HEX), 0.55);
        cairo_move_to(cr, px + MENU_PADDING,
                      py + (NOTIF_HEADER_H - hth) / 2.0);
        pango_cairo_show_layout(cr, pl);

        pango_layout_set_font_description(pl, clock_font());

        cairo_set_source_rgba(cr, COLOR(MENU_BORDER_HEX), 0.4);
        cairo_rectangle(cr, px + MENU_PADDING, py + NOTIF_HEADER_H,
                        pw - 2 * MENU_PADDING, 1);
        cairo_fill(cr);

        int can_clear = g_n_notifs > 0;
        cairo_set_source_rgba(cr, COLOR(MENU_HOVER_HEX),
                              g_npop.hover_clear
                                  ? MENU_HOVER_A : (can_clear ? 0.35 : 0.18));
        cairo_new_path(cr);
        rounded_rect(cr, g_npop.clear_x, g_npop.clear_y, g_npop.clear_w,
                     g_npop.clear_h, g_npop.clear_h / 2.0);
        cairo_fill(cr);
        pango_layout_set_text(pl, "Clear all", -1);
        int ctw, cth;
        pango_layout_get_pixel_size(pl, &ctw, &cth);
        cairo_set_source_rgba(cr, COLOR(FG_HEX), can_clear ? 0.95 : 0.4);
        cairo_move_to(cr, g_npop.clear_x + (g_npop.clear_w - ctw) / 2.0,
                      g_npop.clear_y + (g_npop.clear_h - cth) / 2.0);
        pango_cairo_show_layout(cr, pl);

        cairo_save(cr);
        cairo_new_path(cr);
        cairo_rectangle(cr, g_npop.rows_x, g_npop.list_y,
                        g_npop.rows_w, g_npop.list_h);
        cairo_clip(cr);
        int first = (int)(lround(g_npop.scroll) / NOTIF_ROW_H);
        for (int i = first; i < g_n_notifs; i++) {
            double ry = g_npop.list_y + i * NOTIF_ROW_H - lround(g_npop.scroll);
            if (ry >= g_npop.list_y + g_npop.list_h) break;
            if (ry + NOTIF_ROW_H <= g_npop.list_y) continue;
            notif_draw_row(cr, pl, i);
        }
        cairo_restore(cr);

        cairo_restore(cr);
    }

    if (g_wlp.open)
        wlp_draw(cr, pl);

    g_object_unref(pl);
    pango_font_description_free(fd);

    cairo_surface_flush(b->cairo);
}

struct surf_commit {
    int ready;
    struct wl_surface *surface;
    struct shm_pool *pool;
    cairo_surface_t *cairo;
    int width, height, scale;
    struct wl_callback **frame_cb;
    int *frame_pending;
    const struct wl_callback_listener *listener;
    void *listener_data;
};

static void
surf_commit(struct surf_commit *c)
{
    if (!c->ready || !c->cairo) return;
    int scale = c->scale ? c->scale : 1;
    int phys_w = c->width * scale;
    int phys_h = c->height * scale;
    int stride = cairo_image_surface_get_stride(c->cairo);
    if (!c->pool->buffer) {
        c->pool->buffer = shm_pool_create_buffer(c->pool, phys_w, phys_h,
                                                 stride, WL_SHM_FORMAT_ARGB8888);
    }
    wl_surface_set_buffer_scale(c->surface, scale);
    wl_surface_attach(c->surface, c->pool->buffer, 0, 0);
    wl_surface_damage_buffer(c->surface, 0, 0, phys_w, phys_h);

    if (*c->frame_cb) wl_callback_destroy(*c->frame_cb);
    *c->frame_cb = wl_surface_frame(c->surface);
    wl_callback_add_listener(*c->frame_cb, c->listener, c->listener_data);
    *c->frame_pending = 1;

    wl_surface_commit(c->surface);
}

static void
bar_commit(struct bar *b)
{
    struct surf_commit c = {
        .ready = b->configured,
        .surface = b->surface,
        .pool = &b->pool,
        .cairo = b->cairo,
        .width = b->width,
        .height = b->height,
        .scale = b->scale,
        .frame_cb = &b->frame_cb,
        .frame_pending = &b->frame_pending,
        .listener = &frame_listener,
        .listener_data = b,
    };
    surf_commit(&c);
}

static void
bar_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    struct bar *b = data;
    (void)time;
    if (b->frame_cb == cb)
        b->frame_cb = NULL;
    wl_callback_destroy(cb);
    b->frame_pending = 0;
    b->resize_pending = 0;
    if (b->size_dirty) {
        b->size_dirty = 0;
        bar_update_size(b);
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = bar_frame_done,
};


static void
menu_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    struct bar *b = data;
    (void)time;
    if (b->menu_frame_cb == cb)
        b->menu_frame_cb = NULL;
    wl_callback_destroy(cb);
    b->menu_frame_pending = 0;
}

static const struct wl_callback_listener menu_frame_listener = {
    .done = menu_frame_done,
};

static void
menu_commit(struct bar *b)
{
    struct surf_commit c = {
        .ready = b->menu_surf_configured,
        .surface = b->menu_surface,
        .pool = &b->menu_pool,
        .cairo = b->menu_cairo,
        .width = b->menu_surf_width,
        .height = b->menu_surf_height,
        .scale = b->scale,
        .frame_cb = &b->menu_frame_cb,
        .frame_pending = &b->menu_frame_pending,
        .listener = &menu_frame_listener,
        .listener_data = b,
    };
    surf_commit(&c);
}

static void
menu_surface_size_alloc(struct bar *b, int width, int height)
{
    int scale = b->scale ? b->scale : 1;
    int phys_w = width * scale;
    int phys_h = height * scale;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, phys_w);

    if (b->menu_cairo && width == b->menu_surf_width &&
        height == b->menu_surf_height)
        return;
    b->menu_surf_width = width;
    b->menu_surf_height = height;
    size_t buf_size = stride * phys_h;
    shm_pool_init(&b->menu_pool, buf_size);
    if (b->menu_pool.fd < 0) return;
    pool_cairo_init(&b->menu_pool, &b->menu_cairo, &b->menu_cr,
                    phys_w, phys_h, stride, scale);
}

static void
menu_layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                     uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    struct bar *b = &g_bar;
    if (!b->menu_layer) return;
    menu_surface_size_alloc(b, width, height);
    b->menu_surf_configured = 1;
    b->menu_needs_render = 1;
}

static void
menu_hover_reset(void)
{
    g_pointer.hover_menu_idx = -1;
    g_pmenu.hover = -1;
    g_sys.ctrl_hover = -1;
}

static void
menu_surface_free(struct bar *b)
{
    if (b->menu_frame_cb) { wl_callback_destroy(b->menu_frame_cb); b->menu_frame_cb = NULL; }
    b->menu_frame_pending = 0;
    if (b->menu_cairo) { cairo_surface_destroy(b->menu_cairo); b->menu_cairo = NULL; }
    if (b->menu_cr) { cairo_destroy(b->menu_cr); b->menu_cr = NULL; }
    shm_pool_cleanup(&b->menu_pool);
    b->menu_surf_configured = 0;
    b->menu_needs_render = 0;
}

static void
menu_layer_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    (void)data; (void)surface;
    struct bar *b = &g_bar;
    b->menu_open = 0;
    b->menu_height = 0;
    b->menu_layer = NULL;
    b->menu_surface = NULL;
    menu_surface_free(b);
    menu_hover_reset();
}

static const struct zwlr_layer_surface_v1_listener menu_layer_listener = {
    .configure = menu_layer_configure,
    .closed = menu_layer_closed,
};

static void
menu_surface_render(struct bar *b)
{
    if (!b->menu_surf_configured || !b->menu_cr) return;

    cairo_t *cr = b->menu_cr;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    PangoFontDescription *fd = pango_font_description_from_string(FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, FONT_SIZE * PANGO_SCALE);
    PangoLayout *pl = pango_ctx_layout(cr);
    pango_layout_set_font_description(pl, fd);

    menu_draw(cr, pl, g_bar.menu_height);

    g_object_unref(pl);
    pango_font_description_free(fd);

    cairo_surface_flush(b->menu_cairo);
    menu_commit(b);
}

static void
menu_surface_create(struct bar *b)
{
    if (b->menu_layer) return;
    if (!g_wl.compositor || !g_wl.layer_shell) return;

    struct wl_surface *surf = wl_compositor_create_surface(g_wl.compositor);
    b->menu_surface = surf;

    struct zwlr_layer_surface_v1 *ls =
        zwlr_layer_shell_v1_get_layer_surface(g_wl.layer_shell, surf, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP, "jtlab-menu");
    b->menu_layer = ls;
    zwlr_layer_surface_v1_add_listener(ls, &menu_layer_listener, NULL);

    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    zwlr_layer_surface_v1_set_anchor(ls, anchor);
    zwlr_layer_surface_v1_set_size(ls, 0, b->menu_height);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(ls,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    struct wl_region *region = wl_compositor_create_region(g_wl.compositor);
    wl_region_add(region, 0, 0,
                  MENU_MARGIN_L + MENU_WIDTH + POWER_COL_W + SYS_COL_W,
                  b->menu_height);
    wl_surface_set_input_region(surf, region);
    wl_region_destroy(region);
    wl_surface_commit(surf);
}

static void
menu_surface_destroy(struct bar *b)
{
    if (!b->menu_layer) return;
    zwlr_layer_surface_v1_destroy(b->menu_layer);
    b->menu_layer = NULL;
    if (b->menu_surface) wl_surface_destroy(b->menu_surface);
    b->menu_surface = NULL;
    menu_surface_free(b);
}

static void
bar_render(struct bar *b)
{
    bar_draw(b);
#ifdef JTLAB_DEBUG
    const char *dump = getenv("JTLAB_DUMP");
    if (dump && !getenv("JTLAB_DUMPED") && b->configured) {
        setenv("JTLAB_DUMPED", "1", 1);
        if (!getenv("JTLAB_VOLDUMP")) {
            b->menu_open = 1;
            b->menu_height = calc_menu_height();
            menu_filter_update();
        }
        strcpy(g_sys.player_bus, "org.mpris.MediaPlayer2.mpd");
        strcpy(g_sys.track, "Debug Track Name");
        strcpy(g_sys.artist, "Debug Artist");
        strcpy(g_sys.album, "Debug Album");
        strcpy(g_sys.status, "Playing");
        strcpy(g_sys.art_path, "/tmp/opencode/fake_art.png");
        art_reload();
        {
            const char *art = getenv("JTLAB_ART_TEST");
            if (art) {
                char cache[512];
                art_cache_path(art, cache, sizeof(cache));
                snprintf(g_sys.art_path, sizeof(g_sys.art_path), "%s", cache);
                if (access(cache, R_OK) == 0)
                    art_reload();
                else
                    art_download_start(art, cache);
            }
        }
        {
            const char *hov = getenv("JTLAB_HOVER");
            if (hov) g_sys.ctrl_hover = atoi(hov);
        }
        {
            const char *s = getenv("JTLAB_SEARCH");
            if (s) {
                strncpy(g_app.search, s, sizeof(g_app.search) - 1);
                g_app.search[sizeof(g_app.search) - 1] = '\0';
                menu_filter_update();
            }
        }
        if (getenv("JTLAB_VOLDUMP")) {
            g_box.audio_present = 1;
            const char *vh = getenv("JTLAB_VOL_HOVER");
            if (vh && strncmp(vh, "tab", 3) == 0 && strlen(vh) == 4 &&
                (vh[3] == '0' || vh[3] == '1'))
                g_volpop.tab = vh[3] - '0';
            volpop_open();
            if (vh) {
                if (strcmp(vh, "tab0") == 0 || strcmp(vh, "tab1") == 0 ||
                    strcmp(vh, "tab") == 0)
                    g_volpop.hover_tab = g_volpop.tab;
                else if (strcmp(vh, "mute") == 0)
                    g_volpop.hover_mute = 1;
                else {
                    int r = atoi(vh);
                    if (r >= 0) g_volpop.hover_dev = r;
                }
            }
        } else {
            const char *whov = getenv("JTLAB_WS_HOVER");
            if (whov) {
                wspop_open();
                g_wspop.hover_row = atoi(whov);
            }
        }
        {
            const char *ctx = getenv("JTLAB_CTX");
            if (ctx && b->menu_open) {
                int app = 0;
                double cx = MENU_MARGIN_L + POWER_COL_W + 40;
                double cy = 60;
                if (sscanf(ctx, "%d,%lf,%lf", &app, &cx, &cy) >= 1)
                    ctx_open(app, cx, cy);
            }
        }
        bar_update_size(b);
        if (b->menu_open) {
            menu_surface_create(b);
            menu_layer_configure(&g_bar, b->menu_layer, 0,
                b->width, b->menu_height);
            menu_surface_render(&g_bar);
        }
    }
    {
        const char *dl = getenv("JTLAB_DUMP_DELAY");
        if (dl) {
            double secs = atof(dl);
            struct timespec ts = { (time_t)secs,
                                  (long)((secs - (time_t)secs) * 1e9) };
            nanosleep(&ts, NULL);
        }
    }
    if (dump) {
        if (!getenv("JTLAB_DUMPED")) {
            bar_commit(b);
            return;
        }
        int scale = b->scale ? b->scale : 1;
        int pw = b->width * scale, ph = b->height * scale;
        int stride = cairo_image_surface_get_stride(b->cairo);
        FILE *f = fopen(dump, "wb");
        if (f) {
            fwrite(&pw, sizeof(int), 1, f);
            fwrite(&ph, sizeof(int), 1, f);
            fwrite(&stride, sizeof(int), 1, f);
            fwrite(b->pool.data, 1, (size_t)stride * ph, f);
            fclose(f);
        }
        if (b->menu_open && b->menu_cairo) {
            char menu_dump[1024];
            snprintf(menu_dump, sizeof(menu_dump), "%s.menu", dump);
            int mscale = b->scale ? b->scale : 1;
            int mpw = b->menu_surf_width * mscale;
            int mph = b->menu_surf_height * mscale;
            int mstride = cairo_image_surface_get_stride(b->menu_cairo);
            FILE *f = fopen(menu_dump, "wb");
            if (f) {
                fwrite(&mpw, sizeof(int), 1, f);
                fwrite(&mph, sizeof(int), 1, f);
                fwrite(&mstride, sizeof(int), 1, f);
                fwrite(b->menu_pool.data, 1, (size_t)mstride * mph, f);
                fclose(f);
            }
        }
        exit(0);
    }
#endif
    bar_commit(b);
}

/* Re-allocate the bar's SHM pool + cairo surface for a new logical size.
 * Returns 0 on failure (fd<0), 1 on success. Marks the size as pending. */
static int
bar_reinit_shm(struct bar *b, int w, int h, int scale)
{
    int phys_w = w * scale;
    int phys_h = h * scale;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, phys_w);
    size_t buf_size = stride * phys_h;

    shm_pool_init(&b->pool, buf_size);
    if (b->pool.fd < 0) return 0;
    pool_cairo_init(&b->pool, &b->cairo, &b->cr, phys_w, phys_h, stride, scale);
    b->resize_pending = 1;
    g_box.resize_at_ms = now_ms();
    return 1;
}

static void
bar_update_size(struct bar *b)
{
    if (!b->layer_surface || !b->configured) return;
    int total = BAR_HEIGHT + bar_y_offset();
    int scale = b->scale ? b->scale : 1;
    int w = b->width;

    zwlr_layer_surface_v1_set_size(b->layer_surface, 0, total);

    if (total != b->prev_height) {
        if (b->frame_pending) {
            b->size_dirty = 1;
            b->needs_render = 1;
            return;
        }
        if (!bar_reinit_shm(b, w, total, scale)) return;
        b->height = total;
        b->prev_height = total;
    }

    bar_draw(b);
    bar_commit(b);
    b->needs_render = 0;
}

static void
menu_close(struct bar *b)
{
    if (!b->menu_open) return;
    b->menu_open = 0;
    b->menu_height = 0;
    menu_scroll_anim_cancel();
    menu_hover_reset();
    ctx_close();
    menu_search_clear();
    g_app.menu_sel = 0;
    menu_surface_destroy(b);
    bar_update_size(b);
}

static void
close_sibling_popups(void)
{
    gpopup_close();
    cpop_close();
    volpop_close();
    tray_menu_close();
    wspop_close();
    notifpop_close();
    calpop_close();
    wlp_close();
    ctx_close();
}

static void
close_all_popups(void)
{
    if (g_bar.menu_open)
        menu_close(&g_bar);
    else
        close_sibling_popups();
}

static void
close_all_and_render(void)
{
    close_all_popups();
    render_request();
}

static void
menu_open(struct bar *b)
{
    if (b->menu_open) return;
    close_sibling_popups();
    menu_search_clear();
    b->menu_scroll = 0;
    b->menu_anim_px = 0;
    b->menu_scroll_target_px = 0;
    menu_scroll_anim_cancel();
    g_app.menu_sel = 0;
    menu_filter_update();
    b->menu_height = calc_menu_height();
    b->menu_open = 1;
    menu_hover_reset();
    sys_stats_refresh();
    mpris_refresh();
    menu_surface_create(b);
    bar_update_size(b);
}

static void
menu_search_launch(void)
{
    if (g_app.menu_n == 0) return;
    int idx = g_app.menu_idx[g_app.menu_sel];
    run_cmd(g_app.apps[idx].exec, g_app.apps[idx].terminal);
    menu_close(&g_bar);
}

static int
menu_power_hit_test(double px, double py)
{
    if (!g_bar.menu_open) return -1;
    double mx = MENU_MARGIN_L;
    double my = 0;
    double mh_body = g_bar.menu_height - MENU_FLOAT_GAP;
    double row_base = mh_body - MENU_PADDING - 4 * POWER_ROW_H;
    if (!in_rect(px, py, mx, my + row_base, POWER_COL_W, 4 * POWER_ROW_H)) return -1;
    int row = (int)((py - my - row_base) / POWER_ROW_H);
    if (row >= 0 && row < 4) return row;
    return -1;
}

static int
app_match(const char *desktop_id, const char *app_id)
{
    if (!desktop_id || !app_id || !desktop_id[0] || !app_id[0])
        return 0;
    if (strcasecmp(desktop_id, app_id) == 0)
        return 1;
    size_t dl = strlen(desktop_id);
    size_t al = strlen(app_id);
    if (dl < al && strcasecmp(app_id + al - dl, desktop_id) == 0)
        return 1;
    if (al < dl && strcasecmp(desktop_id + dl - al, app_id) == 0)
        return 1;
    return 0;
}

static int
app_matches_id(const struct app_entry *a, const char *app_id)
{
    if (!a || !app_id) return 0;
    if (app_match(a->desktop_id, app_id)) return 1;
    if (a->wm_class && a->wm_class[0] &&
        strcasecmp(a->wm_class, app_id) == 0)
        return 1;
    return 0;
}

static int
windows_of_app(const struct app_entry *a, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_n_toplevels && n < max; i++) {
        if (!app_matches_id(a, g_toplevels[i].app_id)) continue;
        if (out) out[n] = i;
        n++;
    }
    return n;
}

static int
app_index_by_app_id(const char *app_id)
{
    if (!app_id) return -1;
    for (int i = 0; i < g_app.n_apps; i++) {
        if (app_matches_id(&g_app.apps[i], app_id))
            return i;
    }
    return -1;
}

static int
pinned_running(int pinned_idx)
{
    const struct app_entry *a = &g_app.apps[g_app.pinned[pinned_idx]];
    return windows_of_app(a, NULL, 1) > 0;
}

static int
bar_y_offset(void)
{
    int ph = g_gpopup.open ? g_gpopup.height : 0;
    int cph = g_cpop.open ? (int)g_cpop.h : 0;
    int vh = g_volpop.open ? volpop_height() : 0;
    int th = g_tm.open ? g_tm.height : 0;
    int wh = g_wspop.open ? wspop_height() : 0;
    int nh = g_npop.open ? notif_height() : 0;
    int ch = g_calpop.open ? calpop_height() : 0;
    int wlh = g_wlp.open ? wlp_height() : 0;
    return ph + cph + vh + th + wh + nh + ch + wlh;
}

static int
pill_hit_test(double px, double py)
{
    return bar_icon_hit_test(px, py, 10, MENU_PILL_W, MENU_PILL_H);
}

static int
bell_hit_test(double px, double py)
{
    return bar_icon_hit_test(px, py, g_bar.width - TRAY_EDGE_PAD - NOTIF_ICON_SIZE,
                             NOTIF_ICON_SIZE, NOTIF_ICON_SIZE);
}

static int
clock_hit_test(double px, double py)
{
    double bell_left = g_bar.width - TRAY_EDGE_PAD - NOTIF_ICON_SIZE;
    double clock_x = bell_left - TRAY_GAP - time_text_width();
    return bar_icon_hit_test(px, py, clock_x, time_text_width(), NOTIF_ICON_SIZE);
}

static int
net_hit_test(double px, double py)
{
    return bar_icon_hit_test(px, py, net_icon_x(), NOTIF_ICON_SIZE, NOTIF_ICON_SIZE);
}

static int
audio_hit_test(double px, double py)
{
    return bar_icon_hit_test(px, py, audio_icon_x(), NOTIF_ICON_SIZE, NOTIF_ICON_SIZE);
}

static int
bar_item_hit_test(double px, double py, const struct bar_item *items, int n, double icon)
{
    int bar_y = bar_y_offset();
    for (int i = 0; i < n; i++) {
        double hx = items[i].x + (items[i].w - icon) / 2.0 - 4;
        if (in_rect(px, py, hx, bar_y, icon + 8, BAR_HEIGHT))
            return i;
    }
    return -1;
}

static int
pinned_hit_test(double px, double py)
{
    return bar_item_hit_test(px, py, g_box.pl, g_box.n_pl, PINNED_ICON);
}

static int
menu_hit_test(double px, double py)
{
    if (!g_bar.menu_open) return -1;
    menu_clamp_scroll();
    double mx = MENU_MARGIN_L;
    double my = 0;
    double mh_body = g_bar.menu_height - MENU_FLOAT_GAP;
    if (!in_rect(px, py, mx + POWER_COL_W, my, MENU_WIDTH, mh_body)) return -1;
    double ry = py - my;
    double list_top, list_bottom;
    menu_list_area(mh_body, &list_top, &list_bottom);
    if (ry < list_top || ry >= list_bottom) return -1;
    double sofs = lround(g_bar.menu_anim_px);
    int pos = (int)((ry - list_top + sofs) / MENU_ROW_HEIGHT);
    if (pos >= 0 && pos < g_app.menu_n) return g_app.menu_idx[pos];
    return -1;
}

static int
toplevel_hit_test(double px, double py)
{
    int bar_y = bar_y_offset();
    for (int i = 0; i < g_box.n_taskbar_groups; i++) {
        const struct taskbar_group *g = &g_box.taskbar_groups[i];
        double hx = g->x + (g->w - TASKBAR_ICON) / 2.0 - 4;
        if (in_rect(px, py, hx, bar_y, TASKBAR_ICON + 8, BAR_HEIGHT))
            return i;
    }
    return -1;
}

static int
group_popup_hit_test(double px, double py)
{
    if (!g_gpopup.open) return -1;
    double cw = GROUP_POPUP_W;
    double ch = g_gpopup.height - MENU_FLOAT_GAP;
    if (!in_rect(px, py, g_gpopup.x, g_gpopup.y, cw, ch)) return -1;
    int idx = (int)((py - g_gpopup.y - MENU_PADDING) / MENU_ROW_HEIGHT);
    if (idx >= 0 && idx < g_gpopup.n_windows) return idx;
    return -1;
}

static int
group_popup_close_hit_test(double px, double py)
{
    if (!g_gpopup.open) return -1;
    double pw = GROUP_POPUP_W;
    double r = 9;
    for (int i = 0; i < g_gpopup.n_windows; i++) {
        double row_y = g_gpopup.y + MENU_PADDING + i * MENU_ROW_HEIGHT;
        double cx = g_gpopup.x + pw - MENU_PADDING - 8;
        double cy = row_y + MENU_ROW_HEIGHT / 2.0;
        if (in_rect(px, py, cx - r, cy - r, 2 * r, 2 * r))
            return i;
    }
    return -1;
}

static void
bar_layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                    uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    struct bar *b = &g_bar;
    int scale = b->scale ? b->scale : 1;

    int w_changed = ((int)width != b->width);
    int h_changed = ((int)height != b->prev_height);
    if (w_changed || h_changed || !b->cairo) {
        b->width = width;
        b->height = height;
        b->scale = scale;
        if (!bar_reinit_shm(b, width, height, scale)) return;
        b->prev_height = height;
    }
    b->configured = 1;
    pinned_recalc_layout();
    toplevel_recalc_layout();
    b->needs_render = 1;
}

static void
bar_layer_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    (void)data; (void)surface;
    g_app.running = 0;
}

static const struct zwlr_layer_surface_v1_listener bar_layer_listener = {
    .configure = bar_layer_configure,
    .closed = bar_layer_closed,
};


static int
on_bar_surface(void)
{
    return g_pointer.enter_surface == g_bar.surface;
}

static int
on_menu_surface(void)
{
    return g_pointer.enter_surface == g_bar.menu_surface;
}

struct bar_hover_ent {
    int *field;
    int def;
    int (*hit)(double x, double y);
};

static int
cpop_hover_refresh(int on_bar)
{
    int old = g_cpop.hover;
    if (g_cpop.open)
        g_cpop.hover = on_bar ? cpop_hit_test(g_pointer.x, g_pointer.y) : -1;
    else
        g_cpop.hover = 0;
    return old != g_cpop.hover;
}

static int
gpopup_hover_refresh(int on_bar)
{
    int old_idx = g_gpopup.hover_idx;
    int old_close = g_gpopup.hover_close;
    if (g_gpopup.open) {
        g_gpopup.hover_idx = on_bar ? group_popup_hit_test(g_pointer.x, g_pointer.y) : -1;
        g_gpopup.hover_close = on_bar ? group_popup_close_hit_test(g_pointer.x, g_pointer.y) : -1;
    } else {
        gpopup_clear_hover();
    }
    return old_idx != g_gpopup.hover_idx || old_close != g_gpopup.hover_close;
}

static int
volpop_hover_refresh(int on_bar)
{
    int old_mute = g_volpop.hover_mute;
    int old_tab = g_volpop.hover_tab;
    int old_dev = g_volpop.hover_dev;
    if (g_volpop.open) {
        g_volpop.hover_mute = on_bar ? volpop_mute_hit_test(g_pointer.x, g_pointer.y) : 0;
        g_volpop.hover_tab = on_bar ? volpop_tab_hit_test(g_pointer.x, g_pointer.y) : -1;
        g_volpop.hover_dev = on_bar ? volpop_dev_hit_test(g_pointer.x, g_pointer.y) : -1;
        if (g_volpop.dragging)
            volpop_set_from_x(g_pointer.x);
    } else {
        volpop_clear_hover();
    }
    return old_mute != g_volpop.hover_mute || old_tab != g_volpop.hover_tab ||
           old_dev != g_volpop.hover_dev;
}

static int
tm_hover_refresh(int on_bar)
{
    int old = g_tm.hover_row;
    g_tm.hover_row = g_tm.open
        ? (on_bar ? tray_menu_hit_test(g_pointer.x, g_pointer.y) : -1)
        : -1;
    return old != g_tm.hover_row;
}

static int
ctx_hover_refresh(int on_menu)
{
    int old = g_pointer.hover_ctx_idx;
    g_pointer.hover_ctx_idx = g_ctx.open
        ? (on_menu ? ctx_hit_test(g_pointer.x, g_pointer.y) : -1)
        : -1;
    return old != g_pointer.hover_ctx_idx;
}

static int
menu_hover_refresh(int on_menu)
{
    int old_idx = g_pointer.hover_menu_idx;
    int old_phover = g_pmenu.hover;
    int old_ctrl = g_sys.ctrl_hover;
    if (g_bar.menu_open && !g_ctx.open) {
        g_pointer.hover_menu_idx = on_menu ? menu_hit_test(g_pointer.x, g_pointer.y) : -1;
        g_pmenu.hover = on_menu ? menu_power_hit_test(g_pointer.x, g_pointer.y) : -1;
        g_sys.ctrl_hover = on_menu ? sys_ctrl_hit_test(g_pointer.x, g_pointer.y) : -1;
        int pos = menu_pos_of_app(g_pointer.hover_menu_idx);
        if (pos >= 0) g_app.menu_sel = pos;
    } else {
        menu_hover_reset();
    }
    return old_idx != g_pointer.hover_menu_idx || old_phover != g_pmenu.hover ||
           old_ctrl != g_sys.ctrl_hover;
}

static int
bar_hover_refresh(int on_bar)
{
    static const struct bar_hover_ent tbl[] = {
        { &g_pointer.hover_pinned_idx, -1, pinned_hit_test },
        { &g_pointer.hover_taskbar_idx, -1, toplevel_hit_test },
        { &g_pointer.hover_pill, 0, pill_hit_test },
        { &g_pointer.hover_bell, 0, bell_hit_test },
        { &g_pointer.hover_clock, 0, clock_hit_test },
        { &g_pointer.hover_net, 0, net_hit_test },
        { &g_pointer.hover_audio, 0, audio_hit_test },
        { &g_pointer.hover_tray, -1, tray_hit_test },
        { &g_pointer.hover_ws, 0, workspace_icon_hit_test },
        { &g_wspop.hover_row, -1, wspop_hit_test },
        { &g_calpop.hover_prev, 0, calprev_hit_test },
        { &g_calpop.hover_next, 0, calnext_hit_test },
    };
    int changed = 0;
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        int v = on_bar ? tbl[i].hit(g_pointer.x, g_pointer.y) : tbl[i].def;
        if (*tbl[i].field != v) {
            *tbl[i].field = v;
            changed = 1;
        }
    }
    return changed;
}

static void
pointer_refresh_hover(void)
{
    int on_bar = on_bar_surface();
    int on_menu = on_menu_surface();
    int changed = 0;

    changed |= cpop_hover_refresh(on_bar);
    changed |= gpopup_hover_refresh(on_bar);
    changed |= volpop_hover_refresh(on_bar);
    changed |= tm_hover_refresh(on_bar);
    changed |= notifpop_hover_refresh(on_bar);
    {
        int old = g_wlp.hover_idx;
        g_wlp.hover_idx = (g_wlp.open && on_bar)
            ? wlp_hit_test(g_pointer.x, g_pointer.y) : -1;
        if (old != g_wlp.hover_idx) changed = 1;
        int oldp = g_wlp.hover_power;
        g_wlp.hover_power = (g_wlp.open && on_bar)
            ? wlp_toggle_hit_test(g_pointer.x, g_pointer.y) : 0;
        if (oldp != g_wlp.hover_power) changed = 1;
    }
    changed |= ctx_hover_refresh(on_menu);
    changed |= menu_hover_refresh(on_menu);
    changed |= bar_hover_refresh(on_bar);

    if (trace_on()) {
        static unsigned long last_log;
        unsigned long t = now_ms();
        int over = g_pointer.hover_pinned_idx >= 0 || g_pointer.hover_taskbar_idx >= 0;
        if (over || t - last_log > 500) {
            last_log = t;
            fprintf(stderr,
                    "[trace] ptr x=%.0f y=%.0f on_bar=%d yoff=%d n_pl=%d "
                    "n_grp=%d hp=%d ht=%d | G0=%.0f G1=%.0f\n",
                    g_pointer.x, g_pointer.y, on_bar, bar_y_offset(),
                    g_box.n_pl, g_box.n_taskbar_groups,
                    g_pointer.hover_pinned_idx, g_pointer.hover_taskbar_idx,
                    g_box.n_taskbar_groups > 0 ? g_box.taskbar_groups[0].x : -1.0,
                    g_box.n_taskbar_groups > 1 ? g_box.taskbar_groups[1].x : -1.0);
        }
    }

    if (changed)
        render_request();
}

static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data; (void)pointer;
    g_box.last_motion_at_ms = now_ms();
    g_pointer.x = wl_fixed_to_double(sx);
    g_pointer.y = wl_fixed_to_double(sy);
    g_pointer.enter_surface = surface;
    cursor_set(pointer, serial);
    pointer_refresh_hover();
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial;
    if (surface != g_pointer.enter_surface) return;
    g_pointer.enter_surface = NULL;
    cpop_clear_hover();
    gpopup_clear_hover();
    volpop_clear_hover();
    tm_hover_refresh(0);
    notifpop_hover_refresh(0);
    ctx_hover_refresh(0);
    menu_hover_refresh(0);
    bar_hover_refresh(0);
    render_request();
}

static void
pointer_handle_motion(void *data, struct wl_pointer *pointer,
                      uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data; (void)pointer;
    g_box.last_motion_at_ms = time;
    g_pointer.x = wl_fixed_to_double(sx);
    g_pointer.y = wl_fixed_to_double(sy);
    pointer_refresh_hover();
}

static void
pointer_handle_button(void *data, struct wl_pointer *pointer,
                      uint32_t serial, uint32_t time, uint32_t button,
                      uint32_t state)
{
    (void)data; (void)pointer; (void)serial; (void)time;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (button == BTN_LEFT)
            g_volpop.dragging = 0;
        return;
    }

    if (trace_on())
        fprintf(stderr,
                "[trace] click b=%u x=%.0f y=%.0f on_bar=%d yoff=%d "
                "hp=%d ht=%d n_pl=%d n_grp=%d | G0=%.0f G1=%.0f\n",
                button, g_pointer.x, g_pointer.y, on_bar_surface(),
                bar_y_offset(), g_pointer.hover_pinned_idx,
                g_pointer.hover_taskbar_idx, g_box.n_pl,
                g_box.n_taskbar_groups,
                g_box.n_taskbar_groups > 0 ? g_box.taskbar_groups[0].x : -1.0,
                g_box.n_taskbar_groups > 1 ? g_box.taskbar_groups[1].x : -1.0);

    if (popup_coords_safe(time) && g_volpop.open && on_bar_surface()) {
        if (button == BTN_LEFT && volpop_hit_test(g_pointer.x, g_pointer.y)) {
            int tab = volpop_tab_hit_test(g_pointer.x, g_pointer.y);
            if (tab >= 0) {
                if (tab != g_volpop.tab) {
                    g_volpop.tab = tab;
                    g_volpop.dragging = 0;
                    if (g_box.audio_present)
                        audio_snapshot(&g_box.vol_snap);
                    volpop_recalc_layout();
                    bar_update_size(&g_bar);
                }
                return;
            }
            int row = volpop_dev_hit_test(g_pointer.x, g_pointer.y);
            if (row >= 0) {
                int dev = volpop_row_to_dev(row);
                if (dev >= 0) {
                    pw_set_default(dev);
                    if (g_box.audio_present)
                        audio_snapshot(&g_box.vol_snap);
                }
                render_request();
                return;
            }
            if (volpop_mute_hit_test(g_pointer.x, g_pointer.y)) {
                struct audio_dev *d = NULL;
                int new_mute = 1;
                if (g_pw.thread_loop) {
                    pw_thread_loop_lock(g_pw.thread_loop);
                    d = volpop_active_dev();
                    new_mute = d ? !d->muted : 1;
                    pw_thread_loop_unlock(g_pw.thread_loop);
                }
                pw_set_mute(new_mute);
                if (g_box.audio_present)
                    audio_snapshot(&g_box.vol_snap);
                render_request();
                return;
            } else if (volpop_slider_hit_test(g_pointer.x, g_pointer.y)) {
                g_volpop.dragging = 1;
                volpop_set_from_x(g_pointer.x);
                return;
            }
            return;
        }
        int on_audio = g_box.audio_present && button == BTN_LEFT &&
                       audio_hit_test(g_pointer.x, g_pointer.y);
        int vh = volpop_height();
        volpop_close();
        g_pointer.y -= vh;
        if (on_audio) return;
    }

    if (popup_coords_safe(time) && g_wspop.open && on_bar_surface()) {
        int row = wspop_hit_test(g_pointer.x, g_pointer.y);
        if (row == -2) {
            wspop_close();
            wlp_open();
            render_request();
            return;
        }
        if (row >= 0) {
            workspace_switch_to(row);
            render_request();
            return;
        }
        if (g_pointer.x >= g_wspop.x && g_pointer.x < g_wspop.x + g_wspop.w &&
            g_pointer.y >= g_wspop.y && g_pointer.y < g_wspop.y + g_wspop.h) {
            return;
        }
        int on_ws = workspace_icon_hit_test(g_pointer.x, g_pointer.y);
        int wh = wspop_height();
        wspop_close();
        g_pointer.y -= wh;
        if (on_ws) return;
    }

    if (popup_coords_safe(time) && g_tm.open && on_bar_surface()) {
        int row = tray_menu_hit_test(g_pointer.x, g_pointer.y);
        if (row >= 0) {
            struct tray_menu_entry *e = &g_tm.e[g_tm.row[row].e_idx];
            if (!e->is_separator) {
                tray_menu_click(row);
                tray_menu_close();
            }
            render_request();
            return;
        }
        int on_tray = tray_hit_test(g_pointer.x, g_pointer.y) >= 0;
        int th = g_tm.height;
        tray_menu_close();
        g_pointer.y -= th;
        if (on_tray) return;
    }

    if (popup_coords_safe(time) && g_gpopup.open && on_bar_surface()) {
        int cidx = group_popup_close_hit_test(g_pointer.x, g_pointer.y);
        if (cidx >= 0) {
            int tl_idx = g_gpopup.windows[cidx];
            zwlr_foreign_toplevel_handle_v1_close(g_toplevels[tl_idx].handle);
            return;
        }
        int idx = group_popup_hit_test(g_pointer.x, g_pointer.y);
        if (idx >= 0) {
            int tl_idx = g_gpopup.windows[idx];
            zwlr_foreign_toplevel_handle_v1_activate(g_toplevels[tl_idx].handle, g_wl.seat);
            gpopup_close();
            return;
        }
        int on_src = 0;
        int pidx = pinned_hit_test(g_pointer.x, g_pointer.y);
        if (pidx >= 0 && g_gpopup.n_windows > 0) {
            on_src = app_matches_id(&g_app.apps[g_app.pinned[pidx]],
                                    g_toplevels[g_gpopup.windows[0]].app_id);
        } else {
            int gidx = toplevel_hit_test(g_pointer.x, g_pointer.y);
            if (gidx >= 0)
                on_src = g_box.taskbar_groups[gidx].windows[0] == g_gpopup.windows[0];
        }
        int ph = g_gpopup.height;
        gpopup_close();
        g_pointer.y -= ph;
        if (on_src) return;
    }

    if (popup_coords_safe(time) && g_cpop.open && on_bar_surface()) {
        int row = cpop_hit_test(g_pointer.x, g_pointer.y);
        if (row >= 0) {
            int n_open = g_cpop.app_idx >= 0;
            int is_close = (g_cpop.window_idx >= 0) && (row == n_open);
            if (is_close) {
                int tl_idx = g_cpop.window_idx;
                if (tl_idx >= 0 && tl_idx < g_n_toplevels)
                    zwlr_foreign_toplevel_handle_v1_close(g_toplevels[tl_idx].handle);
            } else {
                run_cmd(g_app.apps[g_cpop.app_idx].exec, g_app.apps[g_cpop.app_idx].terminal);
            }
            cpop_close();
            return;
        }
        double ch = g_cpop.h;
        cpop_close();
        g_pointer.y -= ch;
    }

    if (popup_coords_safe(time) && g_npop.open && on_bar_surface()) {
        if (button == BTN_LEFT &&
            notifpop_clear_hit_test(g_pointer.x, g_pointer.y)) {
            notif_clear_all();
            return;
        }
        int row = notifpop_row_hit_test(g_pointer.x, g_pointer.y);
        if (row >= 0) {
            if (button == BTN_LEFT &&
                notifpop_trash_hit_test(g_pointer.x, g_pointer.y)) {
                notif_remove_idx(g_n_notifs - 1 - row, 2);
                return;
            }
            if (button == BTN_LEFT) {
                if (notif_activate(g_n_notifs - 1 - row))
                    notifpop_close();
                return;
            }
            return;
        }
        if (notifpop_hit_test(g_pointer.x, g_pointer.y))
            return;
        int on_bell = bell_hit_test(g_pointer.x, g_pointer.y);
        int nh = notif_height();
        notifpop_close();
        g_pointer.y -= nh;
        if (on_bell) return;
    }

    if (popup_coords_safe(time) && g_wlp.open && on_bar_surface()) {
        if (button == BTN_LEFT && wlp_toggle_hit_test(g_pointer.x, g_pointer.y)) {
            wlp_toggle_power();
            return;
        }
        int idx = wlp_hit_test(g_pointer.x, g_pointer.y);
        if (idx >= 0 && button == BTN_LEFT) {
            wlp_set_wallpaper_user(g_wlp.entries[idx].path);
            render_request();
            return;
        }
        if (idx >= 0)
            return;
        double body_h = g_wlp.h - MENU_FLOAT_GAP;
        if (g_pointer.x >= g_wlp.x && g_pointer.x < g_wlp.x + g_wlp.w &&
            g_pointer.y >= g_wlp.y && g_pointer.y < g_wlp.y + body_h)
            return;
        int on_ws = workspace_icon_hit_test(g_pointer.x, g_pointer.y);
        int wh = wlp_height();
        wlp_close();
        g_pointer.y -= wh;
        if (on_ws) return;
    }

    if (popup_coords_safe(time) && g_ctx.open && on_menu_surface()) {
        int idx = ctx_hit_test(g_pointer.x, g_pointer.y);
        if (idx >= 0) {
            if (idx == 0) {
                pinned_toggle(g_ctx.target_app);
                ctx_close();
            } else {
                run_cmd(g_app.apps[g_ctx.target_app].exec, g_app.apps[g_ctx.target_app].terminal);
                menu_close(&g_bar);
            }
            return;
        }
        ctx_close();
        render_request();
    }

    if (popup_coords_safe(time) && g_bar.menu_open && on_menu_surface()) {
        if (button == BTN_RIGHT) {
            int idx = menu_hit_test(g_pointer.x, g_pointer.y);
            if (idx >= 0 && idx < g_app.n_apps) {
                ctx_open(idx, g_pointer.x, g_pointer.y);
                g_pointer.hover_ctx_idx = -1;
                render_request();
                return;
            }
        } else if (button == BTN_LEFT) {
            int prow = menu_power_hit_test(g_pointer.x, g_pointer.y);
            if (prow >= 0) {
                static const char *power_cmds[4] = {
                    "systemctl poweroff",
                    "systemctl reboot",
                    "loginctl terminate-user $USER",
                    "systemctl suspend",
                };
                run_cmd(power_cmds[prow], 0);
                menu_close(&g_bar);
                return;
            }
            int ctrl = sys_ctrl_hit_test(g_pointer.x, g_pointer.y);
            if (ctrl >= 0) {
                static const char *mpris_methods[3] = {
                    "Previous", "PlayPause", "Next",
                };
                mpris_control(mpris_methods[ctrl]);
                return;
            }
            int idx = menu_hit_test(g_pointer.x, g_pointer.y);
            if (idx >= 0 && idx < g_app.n_apps) {
                run_cmd(g_app.apps[idx].exec, g_app.apps[idx].terminal);
                menu_close(&g_bar);
                return;
            }
        }
    }

    if (button == BTN_LEFT && on_bar_surface() && pill_hit_test(g_pointer.x, g_pointer.y)) {
        menu_toggle_ctl();
        return;
    }

    if (button == BTN_LEFT && on_bar_surface()) {
        int idx = pinned_hit_test(g_pointer.x, g_pointer.y);
        if (idx >= 0) {
            int app_idx = g_app.pinned[idx];
            const struct app_entry *a = &g_app.apps[app_idx];
            int wins[MAX_WINDOWS_PER_GROUP];
            int count = windows_of_app(a, wins, MAX_WINDOWS_PER_GROUP);
            if (count > 1) {
                if (g_bar.menu_open) menu_close(&g_bar);
                gpopup_open_pinned(idx, g_box.pl[idx].x, 0);
                return;
            }
            if (g_bar.menu_open) menu_close(&g_bar);
            if (count == 1)
                zwlr_foreign_toplevel_handle_v1_activate(g_toplevels[wins[0]].handle, g_wl.seat);
            else
                run_cmd(g_app.apps[app_idx].exec, g_app.apps[app_idx].terminal);
            return;
        }
    }

    if (button == BTN_RIGHT && on_bar_surface()) {
        int idx = pinned_hit_test(g_pointer.x, g_pointer.y);
        if (idx >= 0) {
            int app_idx = g_app.pinned[idx];
            const struct app_entry *a = &g_app.apps[app_idx];
            int wins[MAX_WINDOWS_PER_GROUP];
            int count = windows_of_app(a, wins, MAX_WINDOWS_PER_GROUP);
            if (count > 0) {
                close_all_popups();
                cpop_open(app_idx, count == 1 ? wins[0] : -1, g_box.pl[idx].x + g_box.pl[idx].w / 2.0, 0);
            }
            return;
        }
    }

    if (on_bar_surface()) {
        int gidx = toplevel_hit_test(g_pointer.x, g_pointer.y);
        if (gidx >= 0) {
            struct taskbar_group *g = &g_box.taskbar_groups[gidx];
            if (button == BTN_MIDDLE) {
                for (int i = 0; i < g->n_windows; i++)
                    zwlr_foreign_toplevel_handle_v1_close(g_toplevels[g->windows[i]].handle);
            } else if (button == BTN_RIGHT) {
                int app_idx = app_index_by_app_id(g_toplevels[g->windows[0]].app_id);
                int win = (g->n_windows == 1) ? g->windows[0] : -1;
                close_all_popups();
                cpop_open(app_idx, win, g->x + g->w / 2.0, 0);
            } else if (button == BTN_LEFT) {
                if (g->n_windows == 1) {
                    if (g_bar.menu_open) menu_close(&g_bar);
                    zwlr_foreign_toplevel_handle_v1_activate(g_toplevels[g->windows[0]].handle, g_wl.seat);
                } else {
                    if (g_bar.menu_open) menu_close(&g_bar);
                    gpopup_open(gidx, g->x, 0);
                }
            }
            return;
        }
    }

    if (button == BTN_LEFT && on_bar_surface() &&
        workspace_icon_hit_test(g_pointer.x, g_pointer.y)) {
        if (g_wspop.open) {
            wspop_close();
        } else {
            close_all_popups();
            wspop_open();
        }
        render_request();
        return;
    }

    if (button == BTN_LEFT && g_box.audio_present && on_bar_surface() &&
        audio_hit_test(g_pointer.x, g_pointer.y)) {
        close_all_popups();
        volpop_open();
        render_request();
        return;
    }

    if (button == BTN_LEFT && on_bar_surface() &&
        bell_hit_test(g_pointer.x, g_pointer.y)) {
        if (g_npop.open) {
            notifpop_close();
        } else {
            close_all_popups();
            notifpop_open();
        }
        render_request();
        return;
    }

    if (button == BTN_LEFT && on_bar_surface() &&
        clock_hit_test(g_pointer.x, g_pointer.y)) {
        if (g_calpop.open) {
            calpop_close();
        } else {
            close_all_popups();
            calpop_open();
        }
        render_request();
        return;
    }

    if (on_bar_surface()) {
        int tidx = tray_hit_test(g_pointer.x, g_pointer.y);
        if (tidx >= 0) {
            struct tray_item *it = &g_tray[tidx];
            close_all_popups();
            if ((button == BTN_RIGHT) ||
                (button == BTN_LEFT && it->is_menu)) {
                if (it->menu_path)
                    tray_menu_open(it, tidx);
                else
                    tray_item_call(it, "ContextMenu",
                                   (int)g_pointer.x, (int)g_pointer.y);
            } else if (button == BTN_LEFT) {
                tray_item_call(it, "Activate",
                               (int)g_pointer.x, (int)g_pointer.y);
            } else if (button == BTN_MIDDLE) {
                tray_item_call(it, "SecondaryActivate",
                               (int)g_pointer.x, (int)g_pointer.y);
            }
            render_request();
            return;
        }
    }

    if (button == BTN_LEFT && popup_coords_safe(time) && g_calpop.open &&
        on_bar_surface()) {
        if (calprev_hit_test(g_pointer.x, g_pointer.y)) {
            calpop_step(-1);
            return;
        }
        if (calnext_hit_test(g_pointer.x, g_pointer.y)) {
            calpop_step(1);
            return;
        }
        if (in_rect(g_pointer.x, g_pointer.y, g_calpop.x, g_calpop.y,
                    g_calpop.w, g_calpop.h - MENU_FLOAT_GAP))
            return;
    }

    if (on_bar_surface()) {
        close_all_and_render();
    }
}

static void
pointer_handle_axis(void *d, struct wl_pointer *p, uint32_t t,
    uint32_t a, wl_fixed_t v)
{
    (void)d; (void)p; (void)t;
    if (a != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    double delta = wl_fixed_to_double(v);

    int on_bar = on_bar_surface();
    int over_tray = on_bar ? tray_hit_test(g_pointer.x, g_pointer.y) : -1;
    if (over_tray >= 0) {
        if (!g_tray[over_tray].proxy) return;
        g_dbus_proxy_call(g_tray[over_tray].proxy, "Scroll",
            g_variant_new("(is)", delta < 0 ? -1 : 1, "vertical"),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, tray_call_finish, NULL);
        g_dbus_connection_flush_sync(g_sni.dbus_conn, NULL, NULL);
        return;
    }

    int over_audio = on_bar && g_box.audio_present && audio_hit_test(g_pointer.x, g_pointer.y);
    int over_slider = on_bar && popup_coords_safe(t) && g_volpop.open &&
                      volpop_slider_hit_test(g_pointer.x, g_pointer.y);
    if (over_audio || over_slider) {
        volpop_scroll(delta < 0 ? 5.0 : -5.0);
        return;
    }

    int over_notif = on_bar && popup_coords_safe(t) && g_npop.open &&
                     notifpop_hit_test(g_pointer.x, g_pointer.y);
    if (over_notif) {
        notifpop_scroll(delta < 0 ? -30.0 : 30.0);
        return;
    }

    int over_wlp = on_bar && popup_coords_safe(t) && g_wlp.open &&
                   wlp_hit_test(g_pointer.x, g_pointer.y) >= 0;
    if (over_wlp) {
        wlp_scroll(delta < 0 ? -40.0 : 40.0);
        return;
    }

    int over_menu = on_menu_surface() && popup_coords_safe(t) &&
                    g_bar.menu_open && !g_ctx.open &&
                    menu_hit_test(g_pointer.x, g_pointer.y) >= 0;
    if (over_menu) {
        int dir = delta < 0 ? -1 : 1;
        int old = g_bar.menu_scroll;
        menu_scroll_set_row(old + dir);
        if (g_bar.menu_scroll != old) {
            g_pointer.hover_menu_idx = menu_hit_test(g_pointer.x, g_pointer.y);
            render_request();
        }
        return;
    }
}
static void pointer_handle_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_handle_axis_source(void *d, struct wl_pointer *p, uint32_t s)
    { (void)d; (void)p; (void)s; }
static void pointer_handle_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
    { (void)d; (void)p; (void)t; (void)a; }
static void pointer_handle_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t dc)
    { (void)d; (void)p; (void)a; (void)dc; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
    .axis = pointer_handle_axis,
};

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                       int fd, uint32_t size)
{
    (void)data; (void)kb;
    if (!g_wl.xkb_ctx)
        g_wl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        struct xkb_keymap *km =
            xkb_keymap_new_from_string(g_wl.xkb_ctx, map,
                XKB_KEYMAP_FORMAT_TEXT_V1, 0);
        munmap(map, size);
        if (km) {
            struct xkb_state *st = xkb_state_new(km);
            if (st) {
                if (g_wl.xkb_keymap) xkb_keymap_unref(g_wl.xkb_keymap);
                if (g_wl.xkb_state) xkb_state_unref(g_wl.xkb_state);
                g_wl.xkb_keymap = km;
                g_wl.xkb_state = st;
            } else {
                xkb_keymap_unref(km);
            }
        }
    }
    close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *kb,
    uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
    (void)data; (void)kb; (void)serial; (void)surface; (void)keys;
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *kb,
    uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)kb; (void)serial; (void)surface;
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    (void)data; (void)kb; (void)serial; (void)time;
    if (!g_wl.xkb_state) return;
    xkb_keycode_t code = key + 8;
    xkb_state_update_key(g_wl.xkb_state, code,
        state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(g_wl.xkb_state, code);
    switch (sym) {
    case XKB_KEY_Escape:
        if (g_bar.menu_open) {
            menu_close(&g_bar);
            render_request();
        }
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (g_bar.menu_open) {
            menu_search_launch();
            render_request();
        }
        return;
    case XKB_KEY_BackSpace:
        if (g_bar.menu_open) menu_search_backspace();
        return;
    case XKB_KEY_Up:
        if (g_bar.menu_open) menu_sel_move(-1);
        return;
    case XKB_KEY_Down:
        if (g_bar.menu_open) menu_sel_move(1);
        return;
    default:
        break;
    }

    if (!g_bar.menu_open) return;

    char t[8];
    int n = xkb_state_key_get_utf8(g_wl.xkb_state, code, t, sizeof(t));
    if (n > 0 && (unsigned char)t[0] >= 0x20)
        menu_search_add(t, n);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *kb,
    uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
    uint32_t mods_locked, uint32_t group)
{
    (void)data; (void)kb; (void)serial;
    if (!g_wl.xkb_state) return;
    xkb_state_update_mask(g_wl.xkb_state, mods_depressed, mods_latched,
        mods_locked, 0, 0, group);
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *kb,
    int32_t rate, int32_t delay)
{
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_pointer.pointer) {
        g_pointer.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_pointer.pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g_pointer.pointer) {
        wl_pointer_destroy(g_pointer.pointer);
        g_pointer.pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl.keyboard) {
        g_wl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g_wl.keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && g_wl.keyboard) {
        wl_keyboard_destroy(g_wl.keyboard);
        g_wl.keyboard = NULL;
    }
}

static void seat_handle_name(void *d, struct wl_seat *s, const char *n)
    { (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static int
find_toplevel(struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    for (int i = 0; i < g_n_toplevels; i++)
        if (g_toplevels[i].handle == handle) return i;
    return -1;
}

static void toplevel_handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title);
static void toplevel_handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id);
static void toplevel_handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h);
static void toplevel_handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h);
static void toplevel_handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state);
static void toplevel_handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *parent);
static void toplevel_handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *output);
static void toplevel_handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *output);

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    .title = toplevel_handle_title,
    .app_id = toplevel_handle_app_id,
    .output_enter = toplevel_handle_output_enter,
    .output_leave = toplevel_handle_output_leave,
    .state = toplevel_handle_state,
    .done = toplevel_handle_done,
    .closed = toplevel_handle_closed,
    .parent = toplevel_handle_parent,
};

static void
toplevel_set_string(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                    const char *str, size_t field_off)
{
    (void)data;
    int idx = find_toplevel(h);
    if (idx < 0) return;
    char **dst = (char **)((char *)&g_toplevels[idx] + field_off);
    free(*dst);
    *dst = strdup(str);
}

static void
toplevel_handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title)
{
    toplevel_set_string(data, h, title, offsetof(struct toplevel_entry, title));
}

static void
toplevel_handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app_id)
{
    toplevel_set_string(data, h, app_id, offsetof(struct toplevel_entry, app_id));
}

static void
toplevel_handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *state)
{
    (void)data;
    int idx = find_toplevel(h);
    if (idx < 0) return;
    g_toplevels[idx].activated = 0;
    uint32_t *s;
    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            g_toplevels[idx].activated = 1;
            break;
        }
    }
}

static void
toplevel_handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)data;
    int idx = find_toplevel(h);
    if (idx < 0) return;
    if (!g_toplevels[idx].icon && g_toplevels[idx].app_id) {
#ifdef JTLAB_DEBUG
        fprintf(stderr, "[dbg] toplevel app_id=%s title=%s\n",
                g_toplevels[idx].app_id, g_toplevels[idx].title);
#endif
        for (int i = 0; i < g_app.n_apps; i++) {
            if (g_app.apps[i].icon && app_matches_id(&g_app.apps[i],
                                                 g_toplevels[idx].app_id)) {
                g_toplevels[idx].icon = cairo_surface_reference(g_app.apps[i].icon);
                break;
            }
        }
        if (!g_toplevels[idx].icon)
            g_toplevels[idx].icon = icon_load(g_toplevels[idx].app_id);
    }
    toplevel_recalc_layout();
    render_request();
}

static void
toplevel_remap_popups(int closed_idx, int old_last)
{
    if (g_gpopup.open) {
        int w = 0;
        for (int i = 0; i < g_gpopup.n_windows; i++) {
            int t = g_gpopup.windows[i];
            if (t == closed_idx) continue;
            if (t == old_last) t = closed_idx;
            if (t < 0 || t >= g_n_toplevels) { gpopup_close(); return; }
            g_gpopup.windows[w++] = t;
        }
        int old_h = g_gpopup.height;
        g_gpopup.n_windows = w;
        g_gpopup.height = MENU_FLOAT_GAP + MENU_PADDING * 2 +
                          g_gpopup.n_windows * MENU_ROW_HEIGHT;
        if (g_gpopup.hover_idx >= g_gpopup.n_windows) g_gpopup.hover_idx = -1;
        if (g_gpopup.n_windows == 0) {
            gpopup_close();
        } else if (g_gpopup.height != old_h && g_bar.configured) {
            bar_update_size(&g_bar);
        }
    }
    if (g_cpop.open) {
        int t = g_cpop.window_idx;
        if (t == closed_idx) {
            cpop_close();
        } else if (t == old_last) {
            g_cpop.window_idx = closed_idx;
        } else if (t >= g_n_toplevels) {
            cpop_close();
        }
    }
}

static void
toplevel_handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)data;
    int idx = find_toplevel(h);
    if (idx < 0) return;
    if (g_toplevels[idx].icon) cairo_surface_destroy(g_toplevels[idx].icon);
    free(g_toplevels[idx].app_id);
    free(g_toplevels[idx].title);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    int old_last = g_n_toplevels - 1;
    g_toplevels[idx] = g_toplevels[--g_n_toplevels];
    toplevel_remap_popups(idx, old_last);
    toplevel_recalc_layout();
    render_request();
}

static void
 toplevel_handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data; (void)h; (void)parent;
}

static void
toplevel_handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *output)
{
    (void)data; (void)h; (void)output;
}

static void
toplevel_handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *output)
{
    (void)data; (void)h; (void)output;
}

static void toplevel_manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr, struct zwlr_foreign_toplevel_handle_v1 *h);
static void toplevel_manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr);
static void toplevels_clear(void);

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
    .toplevel = toplevel_manager_toplevel,
    .finished = toplevel_manager_finished,
};

static void
toplevel_manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)data; (void)mgr;
    if (g_n_toplevels >= MAX_TOPLEVELS) return;
    int idx = g_n_toplevels++;
    memset(&g_toplevels[idx], 0, sizeof(*g_toplevels));
    g_toplevels[idx].handle = h;
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &toplevel_handle_listener, NULL);
}

static void
toplevel_manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr)
{
    (void)data;
    if (g_wl.toplevel_mgr == mgr) g_wl.toplevel_mgr = NULL;
    zwlr_foreign_toplevel_manager_v1_destroy(mgr);
    toplevels_clear();
}

static void
cleanup()
{
    if (g_bar.frame_cb) { wl_callback_destroy(g_bar.frame_cb); g_bar.frame_cb = NULL; }
    if (g_bar.cairo) { cairo_surface_destroy(g_bar.cairo); g_bar.cairo = NULL; }
    if (g_bar.cr) { cairo_destroy(g_bar.cr); g_bar.cr = NULL; }
    shm_pool_cleanup(&g_bar.pool);
    shm_pool_cleanup(&g_bar.menu_pool);
    if (g_bar.menu_frame_cb) { wl_callback_destroy(g_bar.menu_frame_cb); g_bar.menu_frame_cb = NULL; }
    if (g_bar.menu_cairo) { cairo_surface_destroy(g_bar.menu_cairo); g_bar.menu_cairo = NULL; }
    if (g_bar.menu_cr) { cairo_destroy(g_bar.menu_cr); g_bar.menu_cr = NULL; }
    if (g_bar.menu_layer) zwlr_layer_surface_v1_destroy(g_bar.menu_layer);
    if (g_bar.menu_surface) wl_surface_destroy(g_bar.menu_surface);
    if (g_bar.layer_surface) zwlr_layer_surface_v1_destroy(g_bar.layer_surface);
    if (g_bar.surface) wl_surface_destroy(g_bar.surface);
    if (g_wl.layer_shell) zwlr_layer_shell_v1_destroy(g_wl.layer_shell);
    if (g_wl.cursor_surface) wl_surface_destroy(g_wl.cursor_surface);
    if (g_wl.cursor_theme) wl_cursor_theme_destroy(g_wl.cursor_theme);
    toplevels_clear();
    if (g_wl.toplevel_mgr) zwlr_foreign_toplevel_manager_v1_destroy(g_wl.toplevel_mgr);
    if (g_pointer.pointer) wl_pointer_destroy(g_pointer.pointer);
    if (g_wl.seat) wl_seat_destroy(g_wl.seat);
    if (g_wl.shm) wl_shm_destroy(g_wl.shm);
    if (g_wl.compositor) wl_compositor_destroy(g_wl.compositor);
    if (g_wl.registry) wl_registry_destroy(g_wl.registry);
}

static void
toplevels_clear(void)
{
    for (int i = 0; i < g_n_toplevels; i++) {
        if (g_toplevels[i].handle)
            zwlr_foreign_toplevel_handle_v1_destroy(g_toplevels[i].handle);
        if (g_toplevels[i].icon) cairo_surface_destroy(g_toplevels[i].icon);
        free(g_toplevels[i].app_id);
        free(g_toplevels[i].title);
    }
    g_n_toplevels = 0;
    gpopup_close();
    cpop_close();
    toplevel_recalc_layout();
}

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
                const char *interface, uint32_t version)
{
    (void)data; (void)version;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        g_wl.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
        g_wl.compositor_name = name;
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        g_wl.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
        g_wl.shm_name = name;
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        g_wl.seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
        g_wl.seat_name = name;
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        g_wl.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 3);
        g_wl.layer_shell_name = name;
    } else if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        g_wl.toplevel_mgr = wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface, 3);
        g_wl.toplevel_mgr_name = name;
        zwlr_foreign_toplevel_manager_v1_add_listener(g_wl.toplevel_mgr, &toplevel_manager_listener, NULL);
    }
    else if (strcmp(interface, ext_workspace_manager_v1_interface.name) == 0) {
        g_wl.ext_ws_mgr = wl_registry_bind(reg, name, &ext_workspace_manager_v1_interface, 1);
        g_wl.ext_ws_mgr_name = name;
        ext_workspace_manager_v1_add_listener(g_wl.ext_ws_mgr, &ws_mgr_listener, NULL);
    }
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg;
    if (g_wl.compositor && g_wl.compositor_name == name) {
        wl_compositor_destroy(g_wl.compositor);
        g_wl.compositor = NULL;
    } else if (g_wl.shm && g_wl.shm_name == name) {
        wl_shm_destroy(g_wl.shm);
        g_wl.shm = NULL;
    } else if (g_wl.seat && g_wl.seat_name == name) {
        wl_seat_destroy(g_wl.seat);
        g_wl.seat = NULL;
        if (g_pointer.pointer) {
            wl_pointer_destroy(g_pointer.pointer);
            g_pointer.pointer = NULL;
        }
    } else if (g_wl.layer_shell && g_wl.layer_shell_name == name) {
        zwlr_layer_shell_v1_destroy(g_wl.layer_shell);
        g_wl.layer_shell = NULL;
    } else if (g_wl.toplevel_mgr && g_wl.toplevel_mgr_name == name) {
        zwlr_foreign_toplevel_manager_v1_destroy(g_wl.toplevel_mgr);
        g_wl.toplevel_mgr = NULL;
        toplevels_clear();
    } else if (g_wl.ext_ws_mgr && g_wl.ext_ws_mgr_name == name) {
        ext_workspace_manager_v1_destroy(g_wl.ext_ws_mgr);
        g_wl.ext_ws_mgr = NULL;
        g_ws.ws_proxies_n = 0;
        workspaces_placeholder_init();
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void handle_signal(int sig) { (void)sig; g_app.running = 0; }

static void
handle_crash(int sig)
{
    void *bt[32];
    int n = backtrace(bt, 32);
    static const char msg[] = "jtlab: fatal signal\n";
    if (write(2, msg, sizeof(msg) - 1) < 0) { /* ignore */ }
    backtrace_symbols_fd(bt, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void
init_signals(void)
{
    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGSEGV, handle_crash);
    signal(SIGBUS, handle_crash);
    signal(SIGABRT, handle_crash);
}

static void
state_init(void)
{
    memset(&g_bar, 0, sizeof(g_bar));
    g_bar.pool.fd = -1;
    g_bar.menu_pool.fd = -1;
    g_bar.scale = 1;
    g_pw.retry_delay_ms = 250;
    g_pointer.hover_menu_idx = -1;
    g_pointer.hover_ctx_idx = -1;
    g_pointer.hover_pinned_idx = -1;
    g_pointer.hover_taskbar_idx = -1;
}

static void
services_init(void)
{
    apps_scan();
    apps_load_icons();
    apps_watch_init();
    pinned_load();
    net_update();
    audio_update();
    pw_connect();
    tray_init();
    sys_init();
}

static int
wayland_init(void)
{
    g_wl.display = wl_display_connect(NULL);
    if (!g_wl.display) {
        fprintf(stderr, "jtlab: cannot connect to wayland display\n");
        return 0;
    }

    g_wl.registry = wl_display_get_registry(g_wl.display);
    wl_registry_add_listener(g_wl.registry, &registry_listener, NULL);
    wl_display_roundtrip(g_wl.display);

    if (!g_wl.compositor || !g_wl.shm || !g_wl.layer_shell) {
        fprintf(stderr, "jtlab: missing wayland globals\n");
        return 0;
    }

    if (!g_wl.ext_ws_mgr)
        workspaces_placeholder_init();

    cursor_init();

    if (g_wl.seat)
        wl_seat_add_listener(g_wl.seat, &seat_listener, NULL);
    return 1;
}

static void
bar_init(void)
{
    struct wl_surface *surf = wl_compositor_create_surface(g_wl.compositor);
    g_bar.surface = surf;

    struct zwlr_layer_surface_v1 *ls =
        zwlr_layer_shell_v1_get_layer_surface(g_wl.layer_shell, surf, NULL,
            ZWLR_LAYER_SHELL_V1_LAYER_TOP, "jtlab");
    g_bar.layer_surface = ls;
    zwlr_layer_surface_v1_add_listener(ls, &bar_layer_listener, NULL);

    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

    zwlr_layer_surface_v1_set_anchor(ls, anchor);
    zwlr_layer_surface_v1_set_size(ls, 0, BAR_HEIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(ls, BAR_HEIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(ls,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(surf);
}

static void
poll_init(void)
{
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    int wl_fd = wl_display_get_fd(g_wl.display);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = wl_fd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wl_fd, &ev);

    int clockfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = clockfd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, clockfd, &ev);

    int sysfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its = {
        .it_interval = { .tv_sec = 2 },
        .it_value    = { .tv_sec = 2 },
    };
    timerfd_settime(sysfd, 0, &its, NULL);
    ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = sysfd };
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sysfd, &ev);

    if (g_app.apps_fd >= 0) {
        ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = g_app.apps_fd };
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_app.apps_fd, &ev);
    }

    g_sni.main_epoll_fd = epoll_fd;
    g_sni.wl_fd = wl_fd;
    g_sni.clock_fd = clockfd;
    g_box.sys_fd = sysfd;
    time_now(g_box.time_str, sizeof(g_box.time_str));
    clock_rearm();
    g_main_context_set_poll_func(g_sni.glib_ctx, glib_epoll_poll);
}

static void
main_loop(void)
{
    while (g_app.running) {
        if (g_bar.needs_render && g_bar.configured && !g_bar.frame_pending) {
            bar_render(&g_bar);
            g_bar.needs_render = 0;
        }
        if (g_bar.menu_needs_render && g_bar.menu_surf_configured &&
            !g_bar.menu_frame_pending) {
            menu_surface_render(&g_bar);
            g_bar.menu_needs_render = 0;
        }
        g_main_context_iteration(g_sni.glib_ctx, TRUE);
        wl_display_dispatch_pending(g_wl.display);

        uint32_t perr_code = 0, perr_obj = 0;
        const struct wl_interface *perr_iface = NULL;
        perr_code = wl_display_get_protocol_error(g_wl.display, &perr_iface,
                                                  &perr_obj);
        int err = wl_display_get_error(g_wl.display);
        if (perr_code || err) {
            if (perr_code)
                fprintf(stderr,
                        "jtlab: fatal protocol error %u on %s (object %u)\n",
                        perr_code, perr_iface ? perr_iface->name : "unknown",
                        perr_obj);
            else
                fprintf(stderr, "jtlab: wayland connection lost: %s\n",
                        strerror(err));
            g_app.running = 0;
        }
    }
}

static void ctl_teardown(void);

static void
teardown(void)
{
    ctl_teardown();
    cleanup();    close(g_sni.clock_fd);
    close(g_box.sys_fd);
    if (g_app.apps_fd >= 0) close(g_app.apps_fd);
    close(g_sni.main_epoll_fd);
    wl_display_disconnect(g_wl.display);

    apps_free();
    pinned_free();
    pw_shutdown();
    art_clear();
    if (g_sys.props_sig)
        g_dbus_connection_signal_unsubscribe(g_sni.dbus_conn, g_sys.props_sig);
    if (g_sys.owner_sig)
        g_dbus_connection_signal_unsubscribe(g_sni.dbus_conn, g_sys.owner_sig);
    tray_shutdown();
}

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtlab-ctl.sock", rt);
}

static void
menu_toggle_ctl(void)
{
    if (g_bar.menu_open)
        menu_close(&g_bar);
    else
        menu_open(&g_bar);
    render_request();
}

static gboolean
ctl_incoming(GSocketService *service, GSocketConnection *conn,
             GObject *source_object, gpointer user_data)
{
    (void)service; (void)source_object; (void)user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[64];
    gssize n = g_input_stream_read(in, buf, sizeof(buf) - 1, NULL, NULL);
    if (n > 0) {
        buf[n] = '\0';
        if (g_str_has_prefix(buf, "menu"))
            menu_toggle_ctl();
        else if (g_str_has_prefix(buf, "quit"))
            g_app.running = 0;
    }
    return FALSE;
}

static void
ctl_init(void)
{
    char path[256];
    ctl_path(path, sizeof(path));
    unlink(path);

    GError *err = NULL;
    GSocketAddress *effective = NULL;
    g_sni.ctl_service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(g_sni.ctl_service), addr,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, &effective, &err)) {
        fprintf(stderr, "jtlab: control socket %s: %s\n", path,
                err ? err->message : "unknown error");
        if (err) g_error_free(err);
        g_object_unref(addr);
        g_object_unref(g_sni.ctl_service);
        g_sni.ctl_service = NULL;
        return;
    }
    g_object_unref(addr);
    g_signal_connect(g_sni.ctl_service, "incoming",
                     G_CALLBACK(ctl_incoming), NULL);
}

static void
ctl_teardown(void)
{
    if (g_sni.ctl_service) {
        g_object_unref(g_sni.ctl_service);
        g_sni.ctl_service = NULL;
    }
    char path[256];
    ctl_path(path, sizeof(path));
    unlink(path);
}

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    init_signals();
    state_init();
    services_init();

    if (!wayland_init())
        return 1;

    bar_init();
    wlp_load_wallpaper();
    poll_init();
    ctl_init();
    main_loop();
    teardown();
    return 0;
}

