# wnip — task checklist

Every action item derived from the original request and from
`docs/xnip-functionality.md`, in the order it was worked, with the evidence used
to call it done.

This is a historical development checklist, not the current release test
report. Earlier counts and measurements below record the runs made at the
time. For current build and test instructions, see `README.md`.

---

## 1. Requirement: run silently in the background, low memory, low CPU

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 1.1 | Windows-subsystem binary, no console window | ✅ | `-mwindows` in the release link (`Makefile`). |
| 1.2 | Resident process with an idle message loop, no timers and no polling | ✅ | `main.c` `GetMessageW` loop; the only timer is the one-shot capture delay. |
| 1.3 | No background threads while idle | ✅ | The OCR worker + its MTA apartment exist only during a recognition (`ocr.c`). |
| 1.4 | Low private memory | ✅ | **1.78 MB** private, measured 8 s after launch. |
| 1.5 | Low working set | ✅ | **10.74 MB**, 5 threads, 166 handles. |
| 1.6 | Low CPU while idle | ✅ | **0.000 s of CPU over 6 s (0.00 %)**. |
| 1.7 | No unbounded handle/memory growth | ✅ | Second full GUI pass: **+0 GDI, +0 USER, ≈0 KB private** (asserted by the smoke test). |
| 1.8 | Clean, complete shutdown | ✅ | `wnip.exe --exit` stops the resident instance; log ends with `--- wnip stopped ---`. |

## 2. Requirement: system tray icon in the Windows taskbar

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 2.1 | `Shell_NotifyIcon` icon with `NOTIFYICON_VERSION_4` | ✅ | `tray.c` `tray_init` → `NIM_ADD` + `NIM_SETVERSION`. |
| 2.2 | Icon uses the app icon at the correct small size | ✅ | `load_wnip_icon(SM_CXSMICON, SM_CYSMICON)`, DPI aware. |
| 2.3 | Tooltip | ✅ | `"wnip - screenshot & annotation"`. |
| 2.4 | Left click starts a region capture | ✅ | `NIN_SELECT` → `CMD_CAPTURE_REGION`; confirmed with a real synthetic click on the shell's own icon rectangle. |
| 2.5 | Re-added after an Explorer restart | ✅ | `TaskbarCreated` handling in `main.c` → `tray_init`. |
| 2.6 | Icon really appears in the taskbar | ✅ | Smoke test asks the shell via `Shell_NotifyIconGetRect` and gets a non-empty rectangle. |
| 2.8 | Right-click callback protocol is decoded correctly | ✅ | `NOTIFYICON_VERSION_4` puts the event in `LOWORD(lParam)` and the icon id in `HIWORD(lParam)`; the handler previously read `wParam` and rejected every message. Now asserted for both protocols, and verified with a real mouse click that pops a `#32768` menu. |
| 2.9 | A raw mouse message does not open the menu twice | ✅ | Version 4 sends `WM_RBUTTONDOWN`/`WM_RBUTTONUP` alongside `WM_CONTEXTMENU`; the raw ones decode to "no action", so exactly one menu opens. |
| 2.7 | Icon handle is not leaked | ✅ | Icons are cached per size and released in `free_wnip_icons()`; the second pass shows +0 user objects. |

## 3. Requirement: right-click opens a menu with a Settings item

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 3.1 | Right-click opens a context menu at the cursor | ✅ | `tray_show_menu`; `WM_RBUTTONDOWN/UP` and `NIN_KEYSELECT`. |
| 3.2 | Menu closes when clicking elsewhere | ✅ | `SetForegroundWindow` + `PostMessage(WM_NULL)` before `TrackPopupMenu`. |
| 3.3 | **Settings…** item present and wired | ✅ | `CMD_SETTINGS` → `settings_open(g_hwndMain)`. |
| 3.4 | Capture entries with their live hotkey labels | ✅ | Region / Window / Scrolling / Screen / All Monitors. |
| 3.5 | Colour picker entry | ✅ | "Pick Colour from Screen". |
| 3.6 | OCR and Pin entries with hotkey labels | ✅ | `Ctrl+Alt+O`, `Ctrl+Alt+P`. |
| 3.7 | Open image file, About, Exit | ✅ | `CMD_OPEN_IMAGE`, `CMD_ABOUT`, `CMD_EXIT`. |
| 3.8 | Separators / default item | ✅ | Three separators; default item = Capture Region. |
| 3.9 | Menu commands actually run | ✅ | The menu returns a command id that is dispatched as `WM_COMMAND`; the smoke test drives `CMD_SETTINGS` through that path and the settings window opens. |

## 4. Requirement: a Settings GUI dialog

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 4.1 | Own settings window with seven tabs | ✅ | General · Capture · Editor · Scrolling · OCR · Hotkeys · Advanced. |
| 4.2 | Real controls bound to `WnConfig`, not placeholders | ✅ | `settings_dlg.c` `CtlDef`/`PageDef` tables + `read_all`/`write_all`. |
| 4.3 | OK / Cancel / Apply / Reset all settings | ✅ | Apply keeps the window open, OK closes, Reset restores every default. |
| 4.4 | Cancel really reverts | ✅ | The window edits a copy; the config is restored on close. |
| 4.5 | Hotkey capture fields | ✅ | Subclassed `EDIT`; modifiers + key captured, `Backspace` clears. |
| 4.6 | Folder picker for the save directory | ✅ | `SHBrowseForFolderW`. |
| 4.7 | Colour pickers for stroke/fill | ✅ | `ChooseColorW`. |
| 4.8 | Numeric validation and clamping | ✅ | Bounds per control, clamped on write. |
| 4.9 | Keyboard navigation | ✅ | Manual Tab order, `Esc` cancels, `Enter` = OK. |
| 4.10 | OCR page reports live availability | ✅ | Status text from `ocr_available()` / `ocr_engine_language()`. |
| 4.11 | Every option has an explanatory label | ✅ | Inline hints, e.g. how to fix a missing language pack. |
| 4.12 | Settings survive a restart | ✅ | INI round trip covered by the unit tests (`WNIP_TEST_CONFIG=1`). |
| 4.13 | Autostart toggle | ✅ | `HKCU\…\CurrentVersion\Run`, created/removed by `actions_set_autostart`. |

## 5. Feature checklist from `docs/xnip-functionality.md`

| Xnip capability | wnip | Status |
| --- | --- | --- |
| 1. Scrolling capture (long screenshots) | Per-row signature matching, coarse→fine SAD, vertical + horizontal, live preview panel, "Done" confirmation, optional auto-scroll by injected wheel events, height cap and match threshold. | ✅ |
| 2. Window capture, incl. multiple windows at once | Frozen-desktop hover highlight, `Shift`+click to add up to 16 windows, single composed image, synthesised drop shadow (Xnip captures the macOS shadow; Windows has none). | ✅ |
| 3. Annotation / markup suite | Rect, ellipse, line, arrow (4 head styles), pen, marker/highlighter, text, numbered steps, pixelate, blur, spotlight, crop — vector annotations over the image with a 64-step undo stack and cached effect layers. | ✅ |
| 4. Text recognition (OCR) / Text Capture | `Windows.Media.Ocr` via hand-written WinRT vtables, on-device, worker thread, result window with copy / save `.txt` / select all, optional auto-copy. | ✅ |
| 5. Pin images | Always-on-top layered tool windows: drag, wheel zoom, opacity menu, `Ctrl+C`, `Ctrl+S`, `Esc`. | ✅ |
| 6. Colour picker | Frozen-desktop picker: magnifier shows `#RRGGBB` + RGB, click copies `#RRGGBB` to the clipboard. | ✅ |
| 7. Physical unit measurement of a selection | Size label with pixel size plus cm/inches from the monitor DPI, three unit modes. | ✅ |
| 8. Annotate local image files | "Open Image File…", tray entry and `<file>` argument; also `Ctrl+V` pastes an image into the editor. | ✅ |
| 9. macOS share menu | Replaced by the Windows equivalents: clipboard (CF_DIB / CF_DIBV5 / registered `PNG` / text), save to disk, open the folder, open in the editor. | ✅ (platform-adapted) |
| 9. Custom file names | Pattern with `%Y %m %d %H %M %S %y %j %n %c`. | ✅ |
| 11. Open screenshots in Preview | "Open the containing folder after saving" + "Open Image File…". | ✅ (platform-adapted) |
| 12. Dark-mode screenshot toolbar | Dark/light toolbar plus a follow-system theme setting; dark title bars via DWM. | ✅ |
| — | Region capture, full screen, all monitors, delay | Not on the Xnip site's list but part of the same flow. | ✅ |
| — | 100 % offline / on-device | The binary links no networking library and makes no network call. | ✅ |
| — | Paste a capture into a terminal / CLI tool | The clipboard carries the image **and** the cached file's absolute path (see §9). | ✅ |
| — | Double-click the selection to copy | Same code path as the Copy button (see §10). | ✅ |
| — | No flicker while the mouse moves | Pre-dimmed base layer + 52x smaller repaints (see §11). | ✅ |

## 6. Quality checklist

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 6.1 | `-Wall -Wextra` clean | ✅ | Clean build from scratch: **0 warnings, 0 errors**. |
| 6.2 | Headless unit tests | ✅ | See README for current counts and optional on-disk configuration tests. |
| 6.3 | GUI smoke test | ✅ | **82 passed / 0 failed**, exit code 0. |
| 6.4 | No handle leaks | ✅ | Second full pass asserts **+0 GDI, +0 USER**. |
| 6.5 | No unbounded memory growth | ✅ | Second full pass asserts private bytes within 4 MB; observed ≈0 KB. |
| 6.6 | Window cleanup never skipped | ✅ | All window procs dispatch on `GWLP_USERDATA` and clear it in `WM_DESTROY` (12 sites). |
| 6.7 | Reference-counted bitmaps | ✅ | `img_ref` / `img_unref` on every shared image; `img_free` = unref. |
| 6.8 | All allocations paired | ✅ | 23 `xmalloc`/`xcalloc`, 4 `xrealloc`, 44 `xfree` sites reviewed; single-threaded ownership. |
| 6.9 | Buffer-overflow guards | ✅ | `img_from_bits` rejects `stride < w * 4`; `size_mul` checks every `w * h * bpp`; `img_blit` clips null/oversized source rects; the `CF_HDROP` block is sized from `strlen` and the direct write is bounds-safe. |
| 6.10 | Integer-overflow tests | ✅ | `size_mul` overflow cases are unit-tested. |
| 6.11 | No leaks from the manifest | ✅ | Exactly one embedded manifest with PerMonitorV2 + comctl32 v6 (GCC's stub is replaced via `-B`). |
| 6.12 | High-DPI correctness | ✅ | PerMonitorV2 manifest; every layout scales from `dpi_for_window`. |
| 6.13 | Verification that ASan/UBSan were *not* available | ⚠️ | This MinGW-w64 toolchain ships neither `libasan` nor `libubsan`; compensated with the handle/memory steady-state assertions and a manual allocation review. |
| 6.14 | No duplicated version number | ✅ | `src/version.h` is the single source for the numeric resource, the file/product version strings and the About box; verified with `(Get-Item wnip.exe).VersionInfo`. |
| 6.16 | The temporary-copy folder cannot grow without bound | ✅ | `cache_prune()` on start-up and after each cached copy drops files older than 7 days and enforces the configurable count (200 by default). |
| 6.15 | Incremental builds are correct | ✅ | `-MMD -MP` dependency files; touching `main.c` rebuilds 1 object + link, touching `config.h` rebuilds the 18 objects that include it + link, no-op builds do nothing. |

## 7. Documentation checklist

| # | Item | Status |
| --- | --- | --- |
| 7.1 | `README.md` — features, build, run, CLI, shortcuts, settings, files, architecture, testing, design notes | ✅ |
| 7.2 | `CHECKLIST.md` (this file) | ✅ |
| 7.3 | Build/run/test instructions reproduce from a clean tree | ✅ |
| 7.4 | Non-obvious design decisions explained (frozen backdrop, WinRT vtable order, manifest `-B` trick, icon caching) | ✅ |

## 8. Deliberately out of scope

| Item | Why |
| --- | --- |
| Watermark / subscription / licence gating | Xnip's commercial model, not a feature of the app. |
| Cloud upload or share links | The reference repeatedly stresses that Xnip is 100 % offline; wnip keeps that property. |
| macOS-style window shadow | Windows does not draw one, so wnip synthesises an equivalent drop shadow instead. |
| Screen recording | Not part of Xnip's documented capture set. |

## 9. Follow-up: pasting into a terminal or CLI tool

**Reported:** after a capture, `Ctrl`+`V` pastes the image into most programs
and websites, but in the Pi agent CLI (and terminals generally) nothing is
pasted, because a terminal can only read **text** from the clipboard.

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 9.1 | Find out why a terminal pastes nothing | ✅ | Terminals read `CF_UNICODETEXT` / `CF_TEXT`; an image-only clipboard has neither, so the paste is empty. |
| 9.2 | Write a temporary PNG per copied capture | ✅ | New `src/cache.c`: `%LOCALAPPDATA%\wnip\cache\wnip-YYYYMMDD-HHMMSS.png`, unique per capture. |
| 9.3 | Offer that path as clipboard **text** | ✅ | `clipboard_copy_image_path()` sets `CF_UNICODETEXT` plus a best-effort `CF_TEXT`. |
| 9.4 | Offer it as a dropped **file** too | ✅ | A single-entry `CF_HDROP` block, so Explorer and Windows Terminal paste the path as well. |
| 9.5 | Do not break image pasting | ✅ | Image formats (`CF_DIB`, `PNG`) are set **first**; only consumers that cannot use an image fall back to the text. Verified: `Get-Clipboard -Format Image` still returns `400x250 Format32bppRgb`. |
| 9.6 | Advertise the real file when there is one | ✅ | `DELIVER_COPY_SAVE`, "copy after saving" and the editor's Save pass the saved file's path instead of a cache copy. |
| 9.7 | Cover the editor and pinned windows | ✅ | `ed_do_copy`, `ed_do_save` and both `pin.c` copy sites now go through `actions_copy_to_clipboard()`. |
| 9.8 | Make it switchable | ✅ | **Capture → Clipboard → "Also copy the file path (for terminals and CLI tools)"**, `copy_file_path` in the ini, default **on**. |
| 9.9 | Bound the disk usage | ✅ | `cache_prune()` runs at start-up and after each cached copy: files older than 7 days are removed, and the configurable file count defaults to 200. |
| 9.10 | Unit tests | ✅ | 17 new cases: the path is absolute, the file exists, it decodes back to the same pixels, a second store never clobbers the first, a back-dated copy is pruned while a fresh one survives, and the clipboard offers the image + `CF_HDROP` + the path text. |
| 9.11 | GUI smoke test | ✅ | New `step_clipboard_path()` (6 checks): the image is still offered, `CF_HDROP` is offered, the text is the path, **the path exists on disk**, and with the option off no path is added. |
| 9.12 | End-to-end proof through the real overlay | ✅ | Scripted region drag + the overlay's **Copy** button in a live instance, then the clipboard read from outside: `hdrop=1 dib=1 text=1`, text `%LOCALAPPDATA%\wnip\cache\wnip-20260929-220803.png`, file exists, `DragQueryFile` reports exactly one file, and `Get-Clipboard` returns both the path text and a 400x250 image. |
| 9.13 | No stray files | ✅ | One capture + copy produces exactly one cache file (verified with an emptied cache folder). |

## 10. Follow-up: double-click inside a selection copies

**Requested:** a double left click on the selected area must do the same thing
as the overlay's **Copy** button.

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 10.1 | Make the overlay receive double clicks at all | ✅ | The window class had no `CS_DBLCLKS`, so `WM_LBUTTONDBLCLK` was never generated; `wc.style = CS_DBLCLKS` added and asserted in the smoke test via `GetClassLongPtrW(hwnd, GCL_STYLE)`. |
| 10.2 | Double click inside the selection | ✅ | New `WM_LBUTTONDBLCLK` case: `ov_confirm(o, DELIVER_COPY)` — literally the `TB_COPY` handler, so it also caches the PNG path for terminals. |
| 10.3 | Only *inside* the selection, only once | ✅ | `ov_pt_in_selection()` reuses the client→selection mapping; the handler returns immediately after confirming (the window is gone), and the raw mouse messages of version-4 style double clicks are not handled twice. |
| 10.4 | No accidental copy | ✅ | The press that arms the copy must have hit a selection that **already existed** (`o->dbl_ok`). Without this, a double click on the dimmed desktop snap-selected a window on the first click and copied it on the second — found by the real-input probe and now covered by the smoke test. |
| 10.5 | Colour picker untouched | ✅ | `CAP_COLOR` is excluded: it samples on press and has no selection. |
| 10.6 | Discoverable | ✅ | The overlay hints now read "Double-click inside to copy" in region and window mode. |
| 10.7 | Unit tests | ✅ | Unaffected (177 / 0). |
| 10.8 | GUI smoke test | ✅ | New `step_double_click_copy()` (9 checks): the class has `CS_DBLCLKS`, a drag leaves a selection, the clipboard starts empty, the double-click closes the overlay, the image is copied, no editor opens, and a double-click whose first click did **not** hit an existing selection copies nothing. |
| 10.9 | End-to-end proof with real OS input | ✅ | A probe drove the live overlay with `SendInput` (so the real `WM_LBUTTONDBLCLK` had to be generated): drag 420x300 with real mouse input, real double click in the middle → overlay closed and the clipboard holds a **420x300** image plus the cached path. |
| 10.10 | Negative controls with real OS input | ✅ | The same probe with a single click inside the selection: overlay stays open, clipboard empty. A real double click on the dimmed area: overlay stays open, clipboard empty. |

## 11. Bug fix: the overlay flashed while the mouse moved

**Reported:** a screen recording showed severe flickering while moving the mouse
during a capture.

**Diagnosis.** The recording was measured frame by frame: the whole screen
alternates between dimmed (frame mean 94.9) and **undimmed** (171.5), sometimes
on consecutive frames, with intermediate values in between. The code explained
it exactly:

- `ov_paint()` blitted the *bright* frozen desktop and then blended the 45 %
  dim over it. Between those two passes the window showed the undimmed screen,
  so any observer that samples mid-repaint (the compositor, the recorder) sees a
  flash. Instrumented: the bright pass alone was 3.6-4.5 ms of a ~13 ms repaint.
- `WM_MOUSEMOVE` invalidated the **entire** window whenever there was no
  selection - which is precisely the state while the user moves the mouse to
  position a region - so it repainted 8.3 megapixels (a full window, 12.7 ms)
  per mouse move, several times a second. Combined with the pass above, that is
  a flashing screen.

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 11.1 | Reproduce the reported symptom objectively | ✅ | Frame-by-frame analysis of the user's recording: dimmed 94.9 vs undimmed 171.5 alternating, partial frames in between - a full screen brightness flip. |
| 11.2 | Find the mechanism in the code | ✅ | Temporary paint instrumentation: base BitBlt 3.6-4.5 ms (bright pixels on screen), then the dim blend; a full repaint measured 12.7-17.2 ms and `WM_MOUSEMOVE` asked for one on every move. |
| 11.3 | Never paint bright content as the base layer | ✅ | The overlay now keeps a pre-dimmed copy of the frozen desktop (`img_dim_except` at capture start) and blits *that* first; the selection is then restored from the undimmed image. A partial repaint can no longer show an undimmed screen. |
| 11.4 | Stop repainting the whole window per mouse move | ✅ | New `ov_cursor_rects()` / `ov_invalidate_cursor()` / `ov_erase_cursor()` invalidate only the magnifier box and the two crosshair strips, at the old *and* the new position; `ov_invalidate_selection()` does the same for a moving selection, its handles and its size chip. |
| 11.5 | Repaint only what changed - verified | ✅ | A probe compared the changed-pixel set of the fixed build against the pre-fix build (which repainted everything, so its set is the ground truth): moving a selection - **0 pixels missed, 0 extra**; a plain mouse move - 11 vs 3 differing pixels against a same-build noise floor of 5 vs 4. No trails. |
| 11.6 | Measure the improvement | ✅ | Damage per mouse move: **8,294,400 px (100 %) -> 160,353 px (1.9 %)**, a 52x smaller repaint. A full repaint: 12.7 ms -> 5.2 ms (no dim blend). Per-move handler cost: 0.68 ms -> 0.08 ms. |
| 11.7 | Regression test | ✅ | New `step_overlay_repaint()` (5 checks) measures the real update region with `GetUpdateRgn`/`GetRegionData`: a mouse move must damage < 1/8 of the overlay, both the old and the new position must be damaged, and one paint must consume the damage (no repaint storm). Rebuilt with the pre-fix overlay: the test reports 8,294,400 px of 8,294,400 and fails, so it genuinely guards the bug. |
| 11.8 | No resource regression | ✅ | Smoke test second pass: +0 GDI, +0 USER, private bytes within the 4 MB budget. |
| 11.9 | Memory | ✅ | The pre-dimmed copy is one extra screen-sized image (33 MB at 4K) only while the overlay is open, freed in `WM_DESTROY`; not allocated at all when the dim is set to 0. |

## 12. Bug fix: the Browse... button was clipped by its own page

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 12.1 | Reproduce from the user's screenshot | ✅ | Measured the reported PNG (769x968) pixel by pixel: the tab frame sits at x=17/749, the "Saving" group frame at x=36/707, and the Browse button's borders at x=652/**749** - the button was clipped to 97 of its 111 px and was drawn over the group frame, butted against the tab frame. |
| 12.2 | Find the cause | ✅ | `PTH` declared the row as the edit only (168..414 logical) and `build_page` put the button at `x + w + S(6)` = 420..494, past the 458-wide group box and past the 470-wide page, so the parent window clipped it. |
| 12.3 | Fix the layout, not the symptom | ✅ | On a path row the declared width now means the whole row: the edit gets `w - button - gap` and the button takes the right end of it. `PTH` is 284 wide, giving a 204-wide edit (168..372) and the button at 378..452 - 6 logical px inside the group frame, so it can never hang out again. |
| 12.4 | Verify on the real window | ✅ | Captured the fixed settings window and re-measured the same edges: the button now spans x=590..698 (its full 111 px, rounded corners included), 10 px inside the group frame (708) and 52 px inside the tab frame (750). |
| 12.5 | Regression test for the whole class | ✅ | `step_settings` now walks all seven pages and their child controls and asserts every window rect fits inside its page's client rect, naming offenders. Widening the row to 400 makes it report `Edit ...` and `Button "Browse..." ... hangs out of its page` and fail, so the check genuinely guards this bug. |

## 12b. Bug fix: the magnifier readout was clipped at 130 logical px

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 12b.1 | Reproduce | ✅ | The user reported the overlay magnifier's info strip showed "1014, 716 RGB 249" with the last RGB digits cut off at 150 % DPI. |
| 12b.2 | Cause | ✅ | The magnifier width was hard-coded to `S(130)` in both `ov_magnifier_box()` and `ov_draw_magnifier()`, while `DrawTextW(DT_SINGLELINE)` with no ellipsis simply clips text wider than the strip. |
| 12b.3 | First fix | ✅ | `ov_measure_magnifier()` measures the widest possible readout ("-5120, -2160   RGB 255, 255, 255") with `DT_CALCRECT` in the real overlay font and grows the box to fit; draw and invalidation share the measured geometry, so no trails. |
| 12b.4 | First fix was a silent no-op | ✅ | The measurement ran in `overlay_begin()` before the overlay window existed, and its `!o->hwnd` guard returned early - the box stayed 130 logical px. A probe with short coordinates (300, 300) still fit, which is why the bug appeared fixed while long coordinates (1014, 716) did not. The user correctly reported it was not fixed. |
| 12b.5 | Real fix | ✅ | Measure with the desktop DC (`GetDC(NULL)`) instead, which works before the window exists; the guard no longer rejects it. |
| 12b.6 | Verified with the user's coordinates | ✅ | A probe parked the cursor at (1014, 716) on the live build: the strip is now 343 px wide (was ~197) and the full readout `1014, 716   RGB r, g, b` renders with the strip background visible after the last glyph - no clipping. |

## 13. Requirement: bounded diagnostic log (8 MB × 4 files)

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 13.1 | Rotation policy | ✅ | `util.c`: one record is appended at a time; when it would push `wnip.log` past the size limit the file is pushed down a slot (`wnip.1.log` → `.2` → `.3`) and the oldest is deleted, so 4 files × 8 MB is the hard ceiling (defaults `LOG_DEFAULT_MAX_MB 8`, `LOG_DEFAULT_MAX_FILES 4`). |
| 13.2 | Crash-safe and exact | ✅ | `log_append` opens/closes per record and measures the real UTF-8 byte count via `WideCharToMultiByte`, so the accounting is exact and a crash cannot lose the tail. |
| 13.3 | Controllable | ✅ | Settings → Advanced: "Write a diagnostic log", "Maximum size (MB)" (1-256) and "Log files to keep" (1-20), stored as `log_enabled` / `log_max_mb` / `log_max_files` and applied immediately on Apply via `settings_apply_live`. `WNIP_LOG=1` still forces it on. |
| 13.4 | Unit tested | ✅ | New `test_log()` (12 checks) drives `log_append`/`log_rotate`/`log_file_bytes` on a scratch folder: exact byte counting, several rotations keep ≤ 4 files, the newest record survives and the oldest is recycled, a single-file policy keeps one file, and nonsense arguments are rejected. |

## 14. Requirement: dedicated capture folder + cache limit (200)

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 14.1 | Default folder | ✅ | `config.c` defaults to `<Pictures>\Screenshots\wnip-capture`, so wnip's captures never mix with the screenshots other tools drop into `Screenshots`. |
| 14.2 | Folder is created | ✅ | `main.c` and `settings_apply_live()` call `ensure_dir` on the configured folder; the folder now exists on disk. |
| 14.3 | One-time migration | ✅ | `config_upgrade_save_dir()` moves a config still pointing at the legacy `<Pictures>\Screenshots` into the sub-folder; the user's ini was migrated by the first run and a custom folder is never touched. |
| 14.4 | Cache limit 200 | ✅ | `CACHE_DEFAULT_MAX_FILES 200` (was 100), configurable as `cache_max_files` (Settings → Advanced, 20-5000); `cache_prune()` trims to it on start-up, after Apply and after each cached copy. |
| 14.5 | Unit tested | ✅ | `test_config` asserts the default folder name, the 200 default, the legacy upgrade (idempotent) and that a custom folder is left alone; `test_cache` builds 6 files in a scratch folder and proves `cache_prune_dir(dir, 3, 7)` removes exactly the 3 oldest. |

## 15. Bug fix: Apply/OK silently zeroed every checkbox and combo

| # | Item | Status | Evidence |
| --- | --- | --- | --- |
| 15.1 | Found | ✅ | The new `log_enabled` checkbox turned itself off mid-selftest, and the user's ini showed every boolean at 0 (`open_in_editor`, `capture_layered`, `show_magnifier`, `snap_to_windows`, `show_physical_units`, `ocr_show_window`, `ocr_copy_after`, `copy_file_path`, `dark_toolbar`). |
| 15.2 | Cause | ✅ | `ctrl_to_value` handled every `BT_INT` by `GetWindowTextW` + `wcstol`; for a checkbox that parses the *caption* ("Write a diagnostic log..." → 0) and for a dropdown combo the selected text, so Apply/OK rewrote all of them with 0. |
| 15.3 | Fix | ✅ | `ctrl_to_value` now asks the control: `BM_GETCHECK` for checkboxes, `CB_GETCURSEL` for combos, the numeric-text path only for real edits. |
| 15.4 | User config repaired | ✅ | The zeroed keys were restored to their documented defaults (a pre-fix copy kept as `wnip.ini.pre-boolean-fix`); `ocr_show_window=0` was also why the OCR result window had stopped appearing. |
| 15.5 | Regression test | ✅ | `step_settings` clicks Apply and asserts `copy_file_path`, `capture_cursor`, `log_enabled` and `theme` are unchanged afterwards. |

## 16. Verification commands

```sh
make                                   # 0 warnings, 0 errors
make test                              # current unit tests (see README)
WNIP_TEST_CONFIG=1 make test           # optional on-disk INI round trip
WNIP_TEST_DRAW=1 ./build/wnip.exe --gui-selftest  # optional GUI drawing pass
./build/wnip.exe --exit                # stops the resident instance cleanly
```

The smoke test asserts the steady-state property directly, so it is the single
command that covers "no memory issues":

```
selftest: resources after second pass: gdi 0 user 0 private -44 KB
selftest: no gdi handle leak (second pass)           ok
selftest: no user handle leak (second pass)          ok
selftest: no runaway growth (second pass)            ok
selftest: done, passed=86 failed=0
```
