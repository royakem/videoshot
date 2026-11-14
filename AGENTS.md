# Videoshot working notes

Videoshot is a fork of Linux Mint's `gnome-screenshot 41.0+mint2+wilma` (GTK 3,
libhandy, C, meson) with video recording added. The git history is upstream
GNOME up to tag `41.0`; Mint's changes and the Videoshot work sit on top.
Read [README.md](README.md) first.

## Rules

- Keep upstream file names (`screenshot-*.c`) so upstream fixes still apply.
- Screenshot behaviour must stay what Mint's tool does.
- Plain commit messages. Commits are made by the maintainer, on request.
- Nothing is done until it has run for real. A build is not a test.

## Layout

| File | Role |
|---|---|
| `src/screenshot-video.c` | Recorder. Runs `ffmpeg -f x11grab`, converts between formats, makes the preview frame, picks file names. |
| `src/screenshot-application.c` | Flow. `take_screenshot_timeout()` branches to `start_take_video()`; `video_recorded_cb()` opens the result window; `video_prepare()` converts before Save or Copy. |
| `src/screenshot-interactive-dialog.c`, `data/ui/screenshot-interactive-dialog.ui` | Main dialog: the two buttons and the Length row. |
| `src/screenshot-dialog.c`, `data/ui/screenshot-dialog.ui` | Result window. `screenshot_dialog_set_video()` adds the Format row. |
| `src/screenshot-preferences.c` | Preferences window (format, folder). |
| `src/screenshot-backend-x11.c` | `screenshot_backend_x11_get_target_rect()` gives the capture rectangle in device pixels. |
| `data/com.royake.videoshot.gschema.xml` | Settings: `video-length`, `video-format`, `video-save-directory`. |

## How recording works

- Interactive recordings go to `~/.cache/videoshot/` first. Save copies the
  file to the chosen folder; Copy puts the cache file's URI on the clipboard.
  Cache files older than a day are removed when the next recording starts.
- `--file` records straight to that file and shows no window.
- ffmpeg writes to a hidden `.name.part` file that is renamed when it ends.
- The length is an input option (`-t` before `-i`).
- GIFs are recorded as a lossless Matroska master (utvideo) and converted afterwards:
  ffmpeg cannot finish a palette GIF when interrupted, so a direct GIF could
  not be stopped early.
- Stopping sends SIGINT to ffmpeg. It exits non-zero then, so success is
  judged by the output file, not the exit status.
- On Cinnamon the area selection comes from the session bus
  (`org.gnome.Shell.Screenshot.SelectArea`, owned by Cinnamon). Tests on the
  real session bus therefore pop up a selection on the real desktop.

## Build

`libhandy-1-dev` is needed. Without root it can be unpacked locally:

```sh
apt-get download libhandy-1-dev && dpkg -x libhandy-1-dev_*.deb root
# point prefix= in root/usr/lib/x86_64-linux-gnu/pkgconfig/libhandy-1.pc at root/usr,
# link root/usr/lib/x86_64-linux-gnu/libhandy-1.so to the system libhandy-1.so.0,
# then set PKG_CONFIG_PATH to that pkgconfig directory.
meson setup build --prefix="$HOME/.local" && ninja -C build
```

## Test

Test in a private display and on a private bus, not on your own session bus:

- `Xvfb :99` with `openbox`, plus something that moves (`xclock -update 1`).
- A `dbus-daemon` with a config that has no service directories.
- `GSETTINGS_BACKEND=keyfile`, own `XDG_CONFIG_HOME` and `XDG_CACHE_HOME`,
  `GSETTINGS_SCHEMA_DIR` pointing at a compiled copy of the schema.
- Drive the dialogs with `xdotool` clicks and check results with `ffprobe`
  and `xclip -selection clipboard -o -t TARGETS`.
- Window mode needs a window that takes focus (`zenity --info`), `xclock` does not.

Cases to cover after a change: each format from the command line, window and
area mode, delay, early stop (`--stop` and relaunch, also for GIF), format
change in the result window, Copy, Save, Cancel back to the main dialog,
Preferences, and a plain screenshot.

## Packages

`packaging/build-packages.sh` builds the `.deb`, `.rpm` and Arch package from
the committed tree in clean containers and runs `packaging/smoke-test.sh` in
each. Run it before a release. H.264 uses libx264, or libopenh264 where ffmpeg
has no libx264 (Fedora). The spec and PKGBUILD download the `v<version>` tag
archive from GitHub, so they only work on their own once that tag is public.

## Open points

- Wayland recording (needs the ScreenCast portal and PipeWire).
- No countdown overlay for the delay.
- The icon is still the GNOME Screenshot icon.
- New strings are untranslated; the `po` files only cover the old ones.
- The panel record icon uses `GtkStatusIcon`; it appears in full-screen videos.
