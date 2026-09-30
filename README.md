# wnip

A Windows-native remake of the macOS screenshot & annotation app
**Xnip** — written in plain **C11** against the **Win32 API** only, with no
third-party runtime, no installer and no network access. wnip is an independent
project and is not affiliated with or endorsed by Xnip or its developers.
Licensed under the [MIT License](LICENSE).

It lives in the notification area, idles at **~2 MB private / ~11 MB working
set / 0.00 % CPU**, and does all capture, annotation and text recognition
**on-device**.

```
        ┌─ right-click the tray icon ─────────────────────────┐
        │  Capture Region            Ctrl+Alt+A               │
        │  Capture Window            Ctrl+Alt+W               │
        │  Scrolling Capture         Ctrl+Alt+S               │
        │  Capture Screen            Ctrl+Alt+F               │
        │  Capture All Monitors                               │
        │  Pick Colour from Screen                            │
        │  ─────────────────────────────────────              │
        │  OCR Text from Screen      Ctrl+Alt+O               │
        │  Pin Image from Clipboard  Ctrl+Alt+P               │
        │  ─────────────────────────────────────              │
        │  Open Image File...                                 │
        │  Settings...                                        │
        │  About wnip                                         │
        │  ─────────────────────────────────────              │
        │  Exit                                               │
        └─────────────────────────────────────────────────────┘
```

## Contents

- [Features](#features)
- [Building](#building)
- [Running](#running)
- [Command line](#command-line)
- [Keyboard while capturing](#keyboard-while-capturing)
- [Editor](#editor)
- [Settings](#settings)
- [Files and registry](#files-and-registry)
- [Architecture](#architecture)
- [Testing](#testing)
- [Design notes](#design-notes)

## Features

### Capture

| Mode | Notes |
| --- | --- |
| Region | Drag a rectangle; snap-to-window, magnifier, pixel + physical size readout, remembered region. |
| Window | Click a window; `Shift`+click adds more and captures them together as one image, each with a synthesised drop shadow. |
| Scrolling | Stitches a long screenshot from a scrolling area, horizontally or vertically. |
| Screen | The monitor under the pointer, or the whole virtual desktop. |
| All monitors | Every monitor in one image. |
| Delay | Optional 0–10 s delay before a region capture. |
| Colour picker | Click any pixel; `#RRGGBB` (and the RGB triple) is copied to the clipboard. |
| Copy a selection fast | **Double-click inside the selection** — identical to pressing the overlay's Copy button. |

The overlay **freezes the desktop** first, so nothing moves while you select,
the magnifier is exact, and the resulting image never contains the overlay
itself.

### Annotation editor

Rectangle, ellipse, line, arrow (4 head styles), pen, highlighter, text
(inline `EDIT`, so IME/CJK work), numbered steps, pixelate, blur, spotlight and
crop. Undo/redo, zoom, pan, colour and width pickers, fill toggle, save, copy,
pin, and open a file straight into the editor.

### Text recognition (OCR)

`Windows.Media.Ocr` driven directly from C through hand-written vtable
bindings — no C++/WinRT, no .NET. Recognition runs on a worker thread in its own
MTA apartment, the result opens in a window with the recognised text (copy,
save as `.txt`, select all) and can optionally be copied to the clipboard
automatically. Everything stays on-device.

### Pin images

Any capture or clipboard image can be pinned as a border-less, always-on-top,
semi-transparent floating window: drag to move, wheel to zoom, right-click for
opacity, `Esc` to close.

### Output

PNG / JPEG / BMP / GIF, configurable save folder and file-name pattern
(`%Y %m %d %H %M %S %y %j %n %c`), JPEG quality, copy-after-save, open-folder,
shutter sound, and "after every capture" behaviour (editor / copy / save /
copy+save). The default save folder is wnip's own `%USERPROFILE%\Pictures\
Screenshots\wnip-capture`, so captures never mix with the screenshots other
tools drop into `Screenshots` (a config still pointing at the old default is
moved into the sub-folder once, on load).

### Pasting into a terminal or CLI tool

Terminals, shells and CLI agents can only read *text* from the clipboard, so an
image-only clipboard is invisible to them and `Ctrl`+`V` does nothing. Every
copy therefore also puts a **temporary PNG** and its **absolute path** on the
clipboard:

| Clipboard format | Who picks it up |
| --- | --- |
| `CF_DIB`, PNG | Editors, Paint, Word, browsers, chat apps — they still get the image. |
| `CF_HDROP` (one file) | Explorer, Windows Terminal and anything that accepts a dropped file. |
| `CF_UNICODETEXT` / `CF_TEXT` | Terminals, shells and CLI tools — they paste the path. |

The image formats are offered **first**, so image-aware applications keep
pasting the image; only consumers that cannot use an image fall back to the
path. When the capture was saved to disk that real file is advertised instead
of a cache copy. Turn it off with **Capture → Clipboard → "Also copy the file
path"** if an application prefers text over the image.

Cached copies live in `%LOCALAPPDATA%\wnip\cache` and are pruned on start-up
and after each cached copy (older than 7 days, with a configurable maximum of
200 files by default), so the folder stays bounded.

## Building

Requirements: **MinGW-w64 GCC** (tested with GCC 16.1.0, x86_64, UCRT) and
GNU Make. `windres` comes with the toolchain, and the icon is checked in.
Python 3 is only required if you want to regenerate the icon with the bundled
script.

```sh
make                 # release build  -> build/wnip.exe
make DEBUG=1         # -O0 -g3, no strip
make test            # build + run the headless unit tests
make run             # build + launch
```

The build is warning-clean with `-Wall -Wextra`, and the version number lives in
exactly one place: `src/version.h`, shared by the C sources and the resource
script.

A few build details worth knowing:

- `tools/mkicon.py` draws the original Heian-inspired folding-fan icon in 10
  sizes using only Python's standard library. `res/wnip.ico` is checked in, so
  Python is needed only to regenerate the artwork.
- Object files record their own header dependencies (`-MMD -MP`), so editing a
  header rebuilds exactly the translation units that include it. (Note that
  MSYS `make` compares timestamps with one-second granularity, so a file
  touched in the same second as its dependency may be skipped.)
- `res/wnip.manifest` requests **PerMonitorV2** DPI awareness, **comctl32 v6**
  and `longPathAware`. GCC links its own stub manifest, so
  `src/wnip_manifest.rc` is compiled to `build/mf/default-manifest.o` and
  `-B` points the linker at it. Without that trick the linker fails with
  `.rsrc merge failure: multiple non-default manifests`.
- `-lruntimeobject` provides `RoInitialize` / `RoGetActivationFactory` /
  `WindowsCreateString`.

## Running

```sh
build/wnip.exe
```

The app starts silently in the notification area. A second launch talks to the
running instance over `WM_COPYDATA` instead of starting a duplicate, so
`wnip.exe` doubles as a remote control (see below).

*Left*-clicking the tray icon starts a region capture; *right*-clicking opens
the menu above.

## Command line

| Flag | Effect |
| --- | --- |
| *(none)* | Start (or reuse) the resident instance. |
| `--capture` | Start the resident instance and begin a region capture. |
| `--capture-window` | Begin a window capture. |
| `--capture-scroll` | Begin a scrolling capture. |
| `--ocr` | Begin an OCR region capture. |
| `--settings` | Open the settings window. |
| `--pin` | Pin the current clipboard image. |
| `<file>` | Open an image file in the editor. |
| `--exit` | Ask the resident instance to quit. |
| `--gui-selftest` | Run the GUI smoke test and exit with the failure count. |

## Keyboard while capturing

| Key | Action |
| --- | --- |
| Drag | Select a region (a click picks the window under the cursor). |
| **Double-click inside the selection** | Same as the **Copy** button: the selection is cropped straight to the clipboard. |
| `Shift`+click | Add another window (window mode). |
| `Ctrl`+`A` | Select the whole monitor under the pointer. |
| Arrow keys | Nudge the selection (`Shift` = 10 px). |
| `Enter` | Confirm. |
| `Esc` / right-click | Cancel. |

## Editor

| Key | Action |
| --- | --- |
| `R` `E` `L` `A` `P` `M` `T` `N` `X` `B` `H` `C` | Rectangle, ellipse, line, arrow, pen, marker, text, numbered step, pixelate, blur, spotlight, crop. |
| `Ctrl`+`Z` / `Ctrl`+`Y` | Undo / redo (`Ctrl`+`Shift`+`Z` also redoes). |
| `Ctrl`+`C` | Copy the annotated image to the clipboard. |
| `Ctrl`+`S` / `Ctrl`+`Shift`+`S` | Save / save as. |
| `Ctrl`+`V` | Replace the image with the clipboard image. |
| `Ctrl`+`A` | Start a crop covering the whole image. |
| `Ctrl`+`0` / `Ctrl`+`+` / `Ctrl`+`-` | Fit, zoom in, zoom out. |
| `Delete` / `Backspace` | Remove the last annotation. |
| `Enter` | Apply the crop. |
| `Esc` | Cancel the annotation, then the crop, then close. |

The toolbar is owner-drawn and can be dark or light.

## Settings

Seven pages, reachable from the tray menu, the About box or `--settings`:

| Page | Contents |
| --- | --- |
| **General** | Start with Windows, shutter sound, save folder, file-name pattern, format, JPEG quality, copy/open-folder after saving, after-capture action, theme. |
| **Capture** | Include cursor, include layered windows, delay, dim amount, magnifier, snap to windows, remember last region, physical-size display and units, "also copy the file path" for terminals. |
| **Editor** | Default stroke/fill colours, line width, text size, fill toggle, arrow style, pixelate block, blur radius, highlight opacity, step size and start, toolbar theme and position. |
| **Scrolling** | Frame interval, minimum match, maximum height, automatic scrolling and wheel steps. |
| **OCR** | Language tag, copy after recognition, show result window, live availability status and a shortcut to the Windows language settings. |
| **Hotkeys** | Click a field and press a combination; `Backspace` clears; "Restore default hotkeys". |
| **Advanced** | Bounded storage: the diagnostic log (on/off, rotate at N MB, keep N files — 8 MB × 4 by default) and the clipboard image cache (keep N PNGs — 200 by default). |

`OK` / `Cancel` / `Apply` / `Reset all settings` are always available, and every
option is documented inline. Settings are validated (numeric ranges, folder
existence) and applied live where it makes sense (hotkeys, autostart).

## Files and registry

| Path | Purpose |
| --- | --- |
| `%APPDATA%\wnip\wnip.ini` | All settings, written with `WritePrivateProfileString`. |
| `%LOCALAPPDATA%\wnip\cache\` | Disposable PNG copies whose paths go to the clipboard for terminals; pruned on start-up, kept at 200 files by default (Settings → Advanced). |
| `%APPDATA%\wnip\wnip.log` | Diagnostic log, on by default and bounded: it rotates to `wnip.1.log`, `wnip.2.log`, `wnip.3.log` when it passes 8 MB and the oldest is deleted — 4 files, 8 MB each, at most (Settings → Advanced). `WNIP_LOG=1` forces it on for support. |
| `HKCU\...\CurrentVersion\Run` | `wnip` value, only when "start with Windows" is on. |

Nothing else is touched: no installer, no services, no scheduled tasks.

## Architecture

```
src/
  main.c         message loop, single instance, tray plumbing, command dispatch
  wnip.h         shared globals, class names, menu command ids
  tray.c         notification-area icon and context menu
  overlay.c      frozen-desktop selection overlay (all capture modes)
  capture.c      screen / window / virtual-desktop capture
  scrolling.c    scrolling-capture window and stitching engine
  editor.c       annotation editor (tools, undo stack, toolbar, crop)
  image.c        refcounted 32bpp DIB sections + crop/scale/effects
  gfx.c          GDI+ encode/decode, anti-aliased drawing, shadows
  ocr.c          Windows.Media.Ocr through hand-written WinRT vtables
  ocr_window.c   OCR result window
  settings_dlg.c table-driven settings UI
  config.c       defaults, INI load/save, hotkey parsing
  hotkey.c       RegisterHotKey bookkeeping
  clipboard.c    CF_DIB / CF_DIBV5 / registered "PNG" / CF_HDROP / text
  cache.c        temporary PNGs whose path is offered to terminals
  actions.c      deliver / save / open / autostart / sound / balloons
  pin.c          always-on-top pinned image windows
  about.c        about window
  util.c         DPI, geometry, strings, hashing, icons, dark mode
  tests.c        headless unit tests
  selftest.c     on-desktop GUI smoke test
```

Key choices:

- **Plain C, Win32 only.** No C++ ABI to keep stable, no runtime to ship, and
  a very small resident footprint.
- **GDI+ flat API** for encode/decode and anti-aliased drawing; **GDI** for
  capture, dimming, the toolbar and double-buffered blits.
- **`WnImage` is a top-down 32bpp DIB section with reference counting**, so
  undo snapshots and pinned windows share pixels instead of copying them.
- **`GWLP_USERDATA` carries every window's state** and the procs validate it,
  which is what keeps `WM_DESTROY` cleanup from being skipped.
- **WinRT without C++** is possible because the ABI is stable; `src/ocr.c`
  declares the handful of vtables it needs (see the note below).
- **A settings row's declared width means the whole row.** `CK_PATH` derives the
  edit's width by subtracting the Browse button from it, so the pair always fits
  inside the group box. A child that overflows its parent is silently clipped by
  it — that is how the Browse button came to look broken — so the smoke test
  additionally asserts that every control fits inside the page it belongs to.

## Testing

```sh
make test                                  # 218 headless unit tests
WNIP_TEST_CONFIG=1 make test               # +4 tests (temporarily writes the real ini file)
./build/wnip.exe --gui-selftest            # on-desktop smoke test, exit 0 on success
WNIP_TEST_DRAW=1 ./build/wnip.exe --gui-selftest  # also exercise editor drawing/undo
```

Run the GUI test only when no resident wnip instance is running: otherwise
single-instance forwarding prevents it from starting. It opens windows and
replaces the clipboard contents; it restores the saved settings after its
settings check. The optional INI round trip temporarily writes the user's
configuration before restoring it; back up important settings first.

The release check passed 218/218 unit checks and 86/86 GUI smoke checks on
Windows (including zero additional GDI and USER handles on the second GUI pass).
The unit tests cover geometry/overflow helpers, image transforms and effects,
GDI+/GDI encode-decode round trips, config parsing round trips, the clipboard
and capture helpers. The GUI smoke test additionally drives the notification
icon (registration, both callback protocols, a real tray message opening the
menu, and a menu command opening Settings), opens the settings tabs,
**every** overlay mode, drives a real drag into the editor, exercises the
editor shortcuts, pins an image, opens the scrolling panel, runs a genuine
OCR pass end-to-end, picks a screen colour, checks that a copy leaves both the
image and the cached file path on the clipboard, drives a real double-click
into the selection (and proves that clicking elsewhere never copies by
accident), checks that a mouse move damages only the small parts of the
overlay that actually move, checks that every settings control fits inside
its page and that **Apply leaves every boolean and combo untouched**, and
finally asserts that **a second
full pass consumes zero extra GDI/USER handles and no extra memory** — that
last check is what catches leaked brushes, fonts, DCs and icons in the window
procedures.

Diagnostics: wnip keeps a bounded log in `%APPDATA%\wnip\wnip.log` — on by
default, rotated at 8 MB, 4 files kept (`wnip.log`, `wnip.1.log`, `wnip.2.log`,
`wnip.3.log`; the oldest is deleted). Both limits, and the number of cached
clipboard images (200), are on the Settings → Advanced page. `WNIP_LOG=1`
forces the log on regardless of the setting.

## Design notes

### WinRT from C

`Windows.Media.Ocr` is a WinRT class. Its C++ projection
(`Windows.Foundation.h`) declares a generic instantiation
`IAsyncOperation<T>` whose **own** three members sit *before* the
`IAsyncInfo` members in the object's vtable, and which is only reachable
through `QueryInterface` as a separate interface. Authentic MIDL ordering
therefore does not describe the object you actually receive:

```
slot 6  put_Completed      slot 9  get_Id
slot 7  get_Completed      slot 10 get_Status
slot 8  GetResults         slot 11 get_ErrorCode
                              slot 12 Cancel   slot 13 Close
```

`Collections.IVectorView<T>` (`GetAt`, `get_Size`, `IndexOf`, `GetMany`, then
`IIterable::First`) follows the same rule. Both layouts are asserted by the
smoke test, so a future Windows that changes them fails loudly instead of
silently hanging.

### Why a frozen backdrop

Every capture mode freezes the virtual desktop into one bitmap first. The
overlay dims *that* bitmap, so selections are stable under animation, the
magnifier shows the pixels that will be captured, and the overlay can never end
up inside its own output.

### Why the selection overlay never flashes

The overlay is a full screen window (3840x2160 here), so anything it repaints
per mouse move has to be small, and the first pixel it paints has to be correct.
Two rules follow, and both were learned the hard way:

- **The base layer is pre-dimmed.** The overlay keeps the frozen desktop *and* a
  darkened copy of it (`img_dim_except` once, at capture start), and paints the
  dark copy first. Previously it blitted the bright desktop and then blended a
  45 % black layer over it; anything that sampled the window between those two
  passes - the DWM compositor, a screen recorder - saw the **undimmed** screen.
  With a full screen repaint triggered by every mouse move that happened several
  times a second, which is exactly the flicker a screen recording shows.
- **A mouse move damages 2 % of the window, not 100 %.** The crosshair, the
  magnifier and the size chip are the only things that follow the cursor, so
  `ov_invalidate_cursor()` invalidates just those rectangles (the old ones and
  the new ones). The same rule applies to a moving selection, whose handles and
  size chip live slightly outside the selection rectangle.

Measured on this machine with a probe that reads the window's update region:
a mouse move used to damage **8,294,400 px (100 %)**, now it damages
**160,353 px (1.9 %)** - a 52x smaller repaint - and a full repaint no longer
pays for the dimming blend (12.7 ms -> 5.2 ms). The smoke test asserts the
damage area, so a regression fails the build rather than the user's eyes.

The cost is one extra screen-sized image while the overlay is open (33 MB at
4K), released the moment the capture ends; with "dim outside the selection" set
to 0 it is not allocated at all.

### The notification-area callback protocol

`Shell_NotifyIcon` has two incompatible callback layouts and the shell picks one
when it accepts `NIM_SETVERSION`. Windows 11 accepts
`NOTIFYICON_VERSION_4`, where **`wParam` is the icon's anchor point and `lParam`
is `MAKELONG(event, iconID)`** — and a single right click arrives as *three*
messages (`WM_RBUTTONDOWN`, `WM_RBUTTONUP`, `WM_CONTEXTMENU`) with the same
`wParam`. Only the semantic event may be acted on, or the menu opens repeatedly.
Decoding that as the legacy layout (or vice versa) leaves the icon completely
inert, so `tray_decode()` is a pure function of `(version4, wParam, lParam)` and
the smoke test asserts both protocols plus the end-to-end path.

### Low footprint

The resident process creates no timers, no polling loops and no background
threads; it wakes only for hotkeys, tray messages and window messages. The OCR
worker thread and its MTA apartment exist only for the duration of a
recognition. Icons, fonts and brushes are cached per size and released on
shutdown, never per window.
