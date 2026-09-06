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
#define BG_HEX          0x0A0A0A
#define BG_A            0.99

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
#define MENU_FLOAT_GAP  12
#define MENU_MARGIN_L   10
#define MENU_CORNER_R   8

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
#define MENU_SHADOW_W   0
#define MENU_SHADOW_A   0.0
#define MENU_BG_HEX     0x0A0A0A
#define MENU_BG_A       0.99
#define MENU_FG_HEX     0xFFFFFF
#define MENU_HOVER_HEX  0x404040
#define MENU_HOVER_A    1.0
#define FOCUS_HOVER_A   0.4
#define MENU_BORDER_HEX 0x4D4D4D
#define MENU_BORDER_A   1.0
#define MENU_BORDER_W   1

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
#define CTX_CORNER_R    6
#define CTX_SHADOW_W    4
#define CTX_SHADOW_A    0.3

/* Pinned apps on bar */
#define PINNED_GAP      12
#define PINNED_ICON     20
#define PINNED_SPACING  8

/* Terminal */
#define TERMINAL_CMD    "footclient"

/* Running app taskbar */
#define TASKBAR_ICON    20
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
#define PILL_HEX        0x111111
#define PILL_A          1.0  /*Turn up to enable*/

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

/* Calendar popup (clock click) */
#define CAL_WIDTH       264
#define CAL_HEADER_H    36
#define CAL_ARROW_W     34
#define CAL_DOW_H       22
#define CAL_CELL_H      26

/* Volume popup (audio click) */
#define VOLUME_MIN      0.0
#define VOLUME_MAX      100.0
#define VOL_POPUP_WIDTH 260
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
