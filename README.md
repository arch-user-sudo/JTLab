# jtlab

A lightweight, highly configurable Wayland status bar for wlroots-based compositors. Built with Wayland client libraries, it provides a bottom bar packed with functionality from app launching to system monitoring.

## Features

### Bar Layout
- **Bottom-docked Wayland bar** using wlr-layer-shell protocol
- **Modular layout**: Left section (app launcher, pinned apps, taskbar), right section (tray, audio, network, clock, notifications, weather)
- **HiDPI aware** with automatic scaling support
- **Configurable height, fonts, colors and padding** via `config.h`

### App Launcher & Menu
- **App launcher popup** with fuzzy-searchable application list (reads `.desktop` files)
- **Pinned apps** displayed on the bar for quick access
- **Power menu** with configurable power actions (shutdown, reboot, logout, etc.)
- **User profile photo** with custom avatar support
- **Quick search** - type in the menu to filter apps instantly
- **Context menu** for taskbar/app entries

### Window Management
- **Taskbar** showing running Wayland windows via `wlr-foreign-toplevel-management`
- **Window grouping** - click grouped apps to see all windows in that group
- **Active window highlighting**
- **Workspace indicator** with workspace popup and switching support via `ext-workspace-v1`
- **Workspace quick actions**: screenshot, screen recording, lock, calculator, terminal shortcuts, etc.

### System Controls & Info
- **Audio control** via PipeWire with device selection, volume slider and mute toggle
- **System tray** (StatusNotifierItem) support for apps like NetworkManager, Discord, etc.
- **Network status** display
- **Clock with calendar popup** - interactive calendar showing current month with day navigation
- **Notification center** with scrollable notification list, clear all, and per-notification actions
- **Weather** - current conditions and 5-day forecast using Open-Meteo API (no API key required). Location auto-resolves via geocoding with cached results.
- **System stats** in the menu/system panel: CPU usage, RAM usage, GPU usage
- **Mini music player** showing album art, track info, and playback controls (via PipeWire metadata)

### Desktop Customization
- **Wallpaper management** - integrated with [wbg](https://codeberg.org/dnkl/wbg) for setting wallpapers. Browse and select wallpapers from a configurable directory in the workspace popup.
- **Night light** (blue light filter) with adjustable color temperature (6500K down to 2500K), controlled via external `wlsunset` daemon
- **Customizable icons** with icon theme support (Adwaita by default)

### Utilities
- **External control** via `jtlabctl` unix socket - toggle menu, quit bar
- **Configurable terminal** and hold-shell behavior for launched commands
- **Minimal resource usage** with compile-time optimizations

## Required Dependencies

### Core Wayland Libraries
- [wayland-client](https://wayland.freedesktop.org/)
- [wayland-cursor](https://wayland.freedesktop.org/)
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols)
- [wlroots protocols](https://gitlab.freedesktop.org/wlroots/wlroots) - includes wlr-layer-shell, wlr-foreign-toplevel-management (protocol XML files provided in `protocols/`)

### Graphics & UI
- [Cairo](https://www.cairographics.org/) - 2D rendering
- [Pango](https://pango.gnome.org/) with [Pangocairo](https://docs.gtk.org/PangoCairo/) - text rendering
- [librsvg](https://wiki.gnome.org/Projects/LibRsvg) - SVG rendering
- [GDK-Pixbuf](https://docs.gtk.org/gdk-pixbuf/) - image loading

### System Integration
- [GLib](https://docs.gtk.org/glib/) & [GIO](https://docs.gtk.org/gio/) - main loop, desktop file parsing, etc.
- [PipeWire](https://pipewire.org/) (libpipewire-0.3) - audio control and media metadata
- [libsoup 3.0](https://libsoup.gnome.org/) - HTTP requests for weather API
- [xkbcommon](https://xkbcommon.org/) - keyboard handling

### Build Tools
- [gcc](https://gcc.gnu.org/) (or compatible C11 compiler)
- [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/)
- [wayland-scanner](https://wayland.freedesktop.org/building.html)

### Runtime Dependencies (Optional but Recommended)
- [wbg](https://codeberg.org/dnkl/wbg) - wallpaper setting (required for wallpaper functionality)
- [wlsunset](https://sr.ht/~kennylevinsen/wlsunset) - night light/blue light filter
- [foot](https://codeberg.org/dnkl/foot) or any terminal - default is `footclient` (configurable in `config.h`)
- A wlroots-based Wayland compositor (sway, hyprland, dwl, wayfire, river, niri, etc.)

## Building

Clone the repository:
```bash
git clone https://github.com/yourusername/jtlab.git
cd jtlab
```

Build:
```bash
make
```

This will generate:
- `jtlab` - the status bar daemon
- `jtlabctl` - control utility

For debug build with sanitizers:
```bash
make debug
```

Clean build artifacts:
```bash
make clean
```

## Installation

System-wide (default `/usr/local/bin`):
```bash
sudo make install
```

Or install to custom prefix:
```bash
make install PREFIX=/usr
```

Uninstall:
```bash
sudo make uninstall
```

## Configuration

All configuration is done at compile time via `config.h`. Edit it to match your preferences before building.

Key configuration options include:
- **Appearance**: `BAR_HEIGHT`, `FONT_FAMILY`, `FONT_SIZE`, colors, corner radius, padding
- **Wallpaper**: `WALLPAPER_DIR` (default: `~/Wallpapers/`) - also requires [wbg](https://codeberg.org/dnkl/wbg) installed
- **Weather**: `WEATHER_ENABLED`, `WEATHER_CITY`, refresh intervals
- **Terminal**: `TERMINAL_CMD` (default: `footclient`)
- **Night light**: Temperature range controlled via `BLUE_TEMP_OFF`/`BLUE_TEMP_MIN`, integrates with `wlsunset`
- **Profile photo**: `PROFILE_PHOTO` path to custom avatar image

After modifying `config.h`, rebuild:
```bash
make clean && make
```

## Usage

Start the bar:
```bash
jtlab
```

Add it to your compositor's autostart (e.g., sway config: `exec jtlab`, Hyprland: `exec-once = jtlab`).

Control the bar using `jtlabctl`:
```bash
jtlabctl menu  # Toggle app launcher menu
jtlabctl quit  # Exit jtlab
```

### Tips
- Bind `jtlabctl menu` to a key in your compositor (e.g., Super+D) for quick app launching
- Ensure [wbg](https://codeberg.org/dnkl/wbg) is installed if you use wallpaper features - the bar sets wallpapers using `wbg`
- If you have another wallpaper tool running, stop/disable it as jtlab manages wallpapers
- For night light to work properly, ensure `wlsunset` is installed and accessible

## Known Issues

- **Does not play well with blur** effects on some compositors - blur may interfere with the bar's rendering

## Contributing

Contributions are welcome! Feel free to open issues or submit pull requests.

## License

See the source files for license information (if not specified, check the repository for LICENSE file).

## Acknowledgements

- [wbg](https://codeberg.org/dnkl/wbg) for wallpaper setting functionality
- wlroots community for Wayland protocols and compositor ecosystem
- The Wayland, Cairo, Pango, PipeWire and other open source projects that make this possible