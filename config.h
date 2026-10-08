#ifndef CONFIG_H
#define CONFIG_H

/* Hex color helpers: a color is written as one RRGGBB hex value plus a
 * separate alpha channel (0.0 - 1.0). The channels are split at compile
 * time, so there is no runtime conversion. */
#define HEX_R(hex) ((((hex) >> 16) & 0xFF) / 255.0)
#define HEX_G(hex) ((((hex) >> 8) & 0xFF) / 255.0)
#define HEX_B(hex) (((hex) & 0xFF) / 255.0)
#define COLOR(hex) HEX_R(hex), HEX_G(hex), HEX_B(hex)

/* Bar configuration */
#define BAR_HEIGHT      30

/* Font */
#define FONT_FAMILY     "Liberation"
#define FONT_SIZE       14.0

/* Icon theme ("" = auto-detect from GTK settings) */
#define ICON_THEME      "Adwaita"

/* Colors: RRGGBB hex + separate alpha (0.0 - 1.0) */
#define BG_HEX          0x111111
#define BG_A            0.98

#define FG_HEX          0xFFFFFF
#define FG_A            1.0

/* App menu pill (bar-left launcher button) */
#define MENU_PILL_W     32
#define MENU_PILL_H     12

/* Menu popup */
#define MENU_WIDTH      260
#define MENU_ROW_HEIGHT 32
#define MENU_ROW_ICON   24
#define MENU_PADDING    8
/* Padding around a panel: the gap to the left and right screen edges (via
 * popup_place_x()) and the gap to the bottom, above the bar (baked into each
 * panel's height). One knob for all three, and it covers the launcher and the
 * context menu too.
 *
 * No top padding yet: the bar only ever spawns at the bottom, so a panel can
 * never reach the screen's top edge. When the bar can spawn up there, the
 * panels' y and the strip height in bar_y_offset() are where that goes. */
#define PPADDING 8
/* Launcher's inset from the left screen edge. Follows PPADDING so the launcher
 * and the flyout panels share one padding value. */
#define MENU_MARGIN_L   PPADDING

/* Corner radius of every panel: the nine flyouts, the launcher and the context
 * menu. Was split across two constants before. */
#define PRADIUS     8

/* Menu scrolling: how many rows fit is derived from the popup height at
 * runtime, so the list just shows whatever fits and scrolls to the bottom. */

/* Menu search bar: input box at the bottom of the apps column. Type an app
 * name to filter the list; Enter launches the selected match. */
#define MENU_SEARCH_H    27
#define SEARCH_BG_HEX   0x444444
#define SEARCH_BG_A     0.1
#define SEARCH_FG_HEX   0xFFFFFF
#define SEARCH_HINT_HEX 0xFFFFFF
#define SEARCH_ACCENT_HEX 0x555555

/* Power actions: icon-only column at the bottom of the app menu (apps to
 * the right); hovering an icon shows a tooltip with its name */
#define POWER_COL_W     48
#define POWER_ROW_H     32

/* Profile photo, drawn as a circle above the power icons in that column.
 * Point this at an image file (PNG works, SVG is scaled to PROFILE_PHOTO_SIZE),
 * e.g. "/home/lynch/Pictures/me.png". Leave it empty -- or point it at a file
 * that does not exist -- to get the built-in default avatar. */
#define PROFILE_PHOTO       ""
#define PROFILE_PHOTO_SIZE  26
#define MENU_SHADOW_W   0
#define MENU_SHADOW_A   0.0
#define MENU_FG_HEX     0xFFFFFF
#define MENU_HOVER_HEX  0x404040
#define MENU_HOVER_A    1.0
#define FOCUS_HOVER_A   0.4
/* Popup border: the frame drawn around every panel, and the hue reused for the
 * dimmer inner dividers and card outlines.
 *
 * PBALPHA is the master opacity: the frame and the card outlines use it
 * directly, the dividers use a fraction of it (x0.2 / x0.4), so the hierarchy
 * between them survives instead of collapsing to one flat value. At 1.0 this
 * reproduces the old per-site literals exactly. Was split across
 * MENU_BORDER_HEX / _A / _W. */
#define PBCOLOR  0x404040
#define PBWIDTH  1
#define PBALPHA  1.0

/* System column (third column right of the app list): mini music player
 * (album-art thumbnail + track info + transport controls) and RAM/CPU/GPU
 * usage readouts. The app-list scrollbar doubles as the divider between the
 * app column and this column. */
#define SYS_COL_W       270
#define SYS_PAD         16
#define SYS_THUMB       56
#define SYS_STAT_H      28
#define SYS_CTRL_BTN    26
#define SYS_FONT_SIZE   12.0
#define SYS_FONT_SMALL  11.0

/* Context menu (right-click) */
#define CTX_WIDTH       140
#define CTX_ROW_HEIGHT  28
#define CTX_SHADOW_W    4
#define CTX_SHADOW_A    0.3

/* Pinned apps on bar */
#define PINNED_GAP      12
#define PINNED_ICON     18
#define PINNED_SPACING  8

/* Terminal */
#define TERMINAL_CMD    "footclient"
/* Shell exec'd after a "held" command finishes, so the terminal window stays
 * open and interactive. $SHELL is the user's login shell; the :- fallback keeps
 * it working in sessions where $SHELL is unset. */
#define TERMINAL_HOLD_SHELL "\"${SHELL:-/bin/sh}\""

/* Running app taskbar */
#define TASKBAR_ICON    18
#define TASKBAR_GAP     (PINNED_SPACING * 2)

/* Window-count badge */
#define BADGE_HEX       0xFFFFFF

/* Notification icon */
#define NOTIF_ICON_SIZE 16

/* Tray cluster (audio / network / clock / bell) */
#define TRAY_GAP        12
#define TRAY_SPACING    10
#define TRAY_CLUSTER_GAP 10
#define TRAY_EDGE_PAD   10
#define TRAY_ICON_MAX   14

/* Right-side status pill (tray / audio / net / clock / bell) */
#define PILL_PAD_X      8
#define PILL_PAD_Y      4
#define PILL_HEX        0x0F1011
#define PILL_A          0.0  /*Turn up to enable*/

/* Weather: current conditions, drawn *outside* the status pill on its left
 * (a glyph plus the temperature). Clicking it opens the forecast popup.
 * Sourced from Open-Meteo, which needs no API key. WEATHER_CITY is resolved to
 * coordinates once by the geocoding endpoint and the result is cached under
 * $XDG_CACHE_HOME/jtlab, so each refresh afterwards costs a single forecast
 * request. Set WEATHER_ENABLED to 0 to drop the item and close its gap again. */
#define WEATHER_ENABLED     1
#define WEATHER_CITY        "Kimberley"
#define WEATHER_REFRESH_MIN 15    /* forecast refresh interval */
#define WEATHER_RETRY_SEC   300   /* backoff after a failed request */
#define WEATHER_DAYS        5      /* forecast rows in the popup */
#define WEATHER_UA          "jtlab/1.0 (wayland bar)"

/* Weather bar item */
#define WEATHER_ICON      14
#define WEATHER_ICON_GAP  5
#define WEATHER_PAD_X     6       /* hover box padding either side of the text */
/* Stroke weight for the parts of the glyph that are strokes rather than fills
 * (sun rays, rain, snowflake arms), as a fraction of the icon box. Fills scale
 * on their own. */
#define WEATHER_STROKE_W  0.11

/* Weather forecast popup. The width is shared with the volume and workspaces
 * popups -- see popup_panel_width() in jtlab.c, which is sized from the
 * workspaces popup's content. */
#define WX_HEAD_H         52
#define WX_ROW_H          30
#define WX_COL_ICON       18
#define WX_RAIN_W         38     /* precipitation-probability column */
#define WX_FONT_SIZE      13.0
#define WX_FONT_HEAD      19.0
#define WX_FONT_SMALL     11.0

/* System tray (StatusNotifierItems) */
#define TRAY_ICON_SIZE  16
#define TRAY_MAX_ITEMS  16

/* Workspace (tags) indicator icon, left of the tray cluster */
#define WS_ICON_SIZE    16

/* Workspace popup (wallpaper button + horizontal row of numbered buttons) */
#define WS_BTN_SIZE     26
#define WS_BTN_GAP      6
#define WS_BTN_R        6
#define WS_WP_BTN_H     30
#define WS_WP_BTN_R     6
#define WS_WP_ICON      14

/* Icons in the workspace popup must all occupy the same WS_WP_ICON square.
 * Cairo strokes straddle the path, so an un-inset stroke paints WS_ICON_LW/2
 * outside the nominal box and the icon comes out ~1px larger than a filled
 * one. Stroked shapes are therefore inset by WS_ICON_INSET so that the ink of
 * every icon -- stroked or filled -- lands on the same square. */
#define WS_ICON_LW      1.3
#define WS_ICON_INSET   (WS_ICON_LW / 2.0)

/* Quick-action buttons in the workspace popup, drawn with the same row height,
 * border style and icon size (WS_WP_ICON) as the wallpaper button. Buttons are
 * grouped into rows by the ws_act_row[] table in jtlab.c: those sharing a row
 * sit side by side and split the popup's inner width evenly, and a button alone
 * in its row spans the full width. The popup is sized by the row holding the
 * most buttons, so WS_ACT_LABEL_W only has to fit the widest label within a
 * half-width button -- "Screenshot" measures ~72px at FONT_SIZE ("Speedtest"
 * and "Fastfetch" are both ~63px), with headroom so text never wraps inside
 * the fixed-height row. */
#define WS_ACT_BTN_GAP  8
#define WS_ACT_PAD      10
#define WS_ACT_LABEL_W  80

/* Night light (blue light filter) block inside the workspace popup.
 * A header box (same height/border style as the wallpaper button) holds the
 * moon icon, the "Night light" label and the on/off toggle; the temperature
 * slider sits in its own row directly below. The slider maps 0-100% intensity
 * to a color temperature from BLUE_TEMP_OFF (no filter, 6500K) down to
 * BLUE_TEMP_MIN (max filter), driving an external wlsunset daemon. */
#define BLUE_HEADER_H     (WS_WP_BTN_H)
#define BLUE_SLIDER_TOP   8
#define BLUE_SLIDER_BOT   12
#define BLUE_BOX_H        (BLUE_HEADER_H + BLUE_SLIDER_TOP + BLUE_SLIDER_H + BLUE_SLIDER_BOT)
#define BLUE_SLIDER_H     4
#define BLUE_KNOB_R       5
#define BLUE_TOGGLE_W     34
#define BLUE_TOGGLE_H     18
#define BLUE_TOGGLE_RPAD  8
#define BLUE_ACCENT_HEX   0xE8A03C
#define BLUE_TEMP_OFF     6500
#define BLUE_TEMP_MIN     2500

/* Wallpaper browser popup (click Wallpaper button in workspace popup).
 * Scans WALLPAPER_DIR for images, shows a scrollable grid of thumbnails. */
#define WALLPAPER_DIR   "/home/lynch/Wallpapers/"
#define WALL_VISIBLE_ROWS 3
#define WALL_THUMB_W    130
#define WALL_THUMB_H    80
#define WALL_THUMB_GAP  8
#define WALL_THUMB_COLS 4
#define WALL_LABEL_H    16
#define WALL_HEADER_H   32

/* Calendar popup (clock click). Width is shared with the volume, weather and
 * workspaces popups -- see popup_panel_width() in jtlab.c. The 7-column grid
 * derives its cell width from it, so it must stay wide enough for a two-digit
 * day plus the "today" circle (~19px) with room to spare. */
#define CAL_HEADER_H    36
#define CAL_ARROW_W     34
#define CAL_DOW_H       22
#define CAL_CELL_H      26

/* Volume popup (audio click). Width is shared with the calendar, weather and
 * workspaces popups -- see popup_panel_width() in jtlab.c. */
#define VOLUME_MIN      0.0
#define VOLUME_MAX      100.0
#define VOL_TAB_H       22
#define VOL_DEV_ROW_H   30
#define VOL_ROW_H       36
#define VOL_MUTE_ICON   20
#define VOL_SLIDER_H    4
#define VOL_SLIDER_BG_HEX   0x4D4D4D
#define VOL_SLIDER_BG_A     1.0
#define VOL_SLIDER_FILL_HEX 0x66B366
#define VOL_SLIDER_FILL_A   1.0
#define VOL_KNOB_R      5

/* Group popup (click on a taskbar app with multiple windows) */
#define GROUP_POPUP_W   260

/* Notification popup (bell click). Solid fixed height; the list scrolls
 * internally with a thin scrollbar when it overflows. */
#define NOTIF_POPUP_WIDTH   360
#define NOTIF_VISIBLE_ROWS  5
#define NOTIF_LIST_BOTTOM_PAD (MENU_PADDING - NOTIF_BOX_GAP / 2)
#define NOTIF_HEADER_H      40
#define NOTIF_ROW_H         62
#define NOTIF_TRASH_ICON    13
#define NOTIF_CLEAR_PAD_X   10
#define NOTIF_CLEAR_H       22
#define NOTIF_MAX           128
#define NOTIF_FONT_SIZE     13.0
#define NOTIF_BOX_HEX       0xFFFFFF
#define NOTIF_BOX_A         0.07
#define NOTIF_BOX_GAP       4
#define NOTIF_BOX_PAD       8
#define NOTIF_TEXT_PAD_Y    10
#define NOTIF_LINE_SPACING  4

/* Shared popup body height (all popups match the notification popup) */
#define POPUP_BODY_H (NOTIF_HEADER_H + 1 + 4 + NOTIF_VISIBLE_ROWS * NOTIF_ROW_H + NOTIF_LIST_BOTTOM_PAD)

#endif /* CONFIG_H */
