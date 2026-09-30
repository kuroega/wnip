# Xnip — Functionality Reference

Consolidated reference of every documented feature and behavior of **Xnip**, the macOS
screenshot & annotation app.

- **Product:** Xnip — Screenshot & Annotation (`Xnip - 截图 & 标注`)
- **Platform:** macOS (App Store app, id `1221250572`)
- **Minimum OS documented:** macOS 13.5
- **Current documented versions:** website updates list `2.5.0`; App Store listing `2.5.1`
- **Vendor:** Toolinbox / Xnip (`xnip.app@gmail.com`)
- **Languages:** English, 简体中文

**Primary sources**

| Source | URL |
| --- | --- |
| Scrolling capture guide | https://xnipapp.com/scrolling-capture/ |
| Help / FAQs | https://xnipapp.com/help/ |
| Product / feature landing page | https://xnipapp.com/#features |
| Pricing (placeholder) | https://xnipapp.com/pricing/ |
| Updates / release notes | https://xnipapp.com/updates/ |
| Privacy policy | https://xnipapp.com/privacy/ |
| Terms of use | https://xnipapp.com/terms/ |
| App Store listing (official) | https://apps.apple.com/app/id1221250572 |

Legend: ✅ documented on the website · 🍎 documented only in the official App Store listing ·
📝 documented only as a release-note change.

---

## 1. Product overview

> "Capture on Mac. Fast and handy. From quick captures to scrolling screenshots and instant
> annotations, Xnip handles it all, effortlessly."
> — https://xnipapp.com/

Xnip is a lightweight, **100 % offline / on-device** screenshot and annotation tool for macOS.

Marketing claims on the site:

- **100% Offline** — the app has no network access permission.
- **On-device** — capture, annotation and OCR run locally.
- **10 Years on App Store** — long-running, actively maintained app.
- **"One Shortcut, Capture and Annotate Instantly."** — capture and edit in a single flow.

---

## 2. Core capabilities at a glance

| # | Capability | Source |
| --- | --- | --- |
| 1 | Scrolling capture (long screenshots) | ✅ |
| 2 | Window capture, incl. multiple windows at once | ✅ |
| 3 | Annotation / markup suite (Markup, Draw, Pixelate, Steps, Highlight) | ✅ |
| 4 | Text recognition (OCR) / Text Capture | ✅ + 📝 |
| 5 | Pin images (always-on-top floating window) | ✅ |
| 6 | Color picker | 🍎 |
| 7 | Physical unit measurement of a selection | 🍎 |
| 8 | Annotate local image files in a dedicated window | 📝 |
| 9 | macOS share menu integration | 📝 |
| 10 | Custom file names | 📝 |
| 11 | Open screenshots in Preview | 📝 |
| 12 | Dark-mode screenshot toolbar | 📝 |

---

## 3. Capturing

### 3.1 Starting a capture

Two documented entry points:

1. **Keyboard shortcut** — the site promotes "One Shortcut, Capture and Annotate Instantly"
   and says a capture can be started by shortcut.
2. **Menu-bar icon** — click the Xnip icon in the macOS menu bar, then click the capture button.

> Note: the exact default key combination is **not published** on the official pages. A keyboard
> shortcut conflict is explicitly named as a common cause of capture problems (see §10).

### 3.2 Selecting an area (region capture)

1. Start a capture.
2. Click and drag to select the capture area — a **blue box** indicates the selection.
3. For scrolling captures, the selection rules in §3.3 apply.

While annotating you can **resize the capture area** ("You can even resize the capture area while
you work"), i.e. region selection is not frozen once annotation begins.

### 3.3 Scrolling capture (long screenshots)

Purpose: capture entire webpages, long conversations/chat history, documents, and source code in
one seamless image, even when the content extends beyond the screen. "Capture anything that
scrolls."

**Official step-by-step guide** (https://xnipapp.com/scrolling-capture/):

1. **Start a capture** — via shortcut or the menu-bar capture button.
2. **Select an area** — click and drag; a blue box marks the region. For a scrolling capture:
   - select only **scrollable content**;
   - **do not include** any floating view, overlay, or scrollbar in the selection.
   - The guide shows a "Correct selection" vs. "Incorrect selection" comparison.
3. **Click the toolbar button** that starts scrolling capture.
4. **Scroll slowly** — use the trackpad to scroll smoothly and slowly. A **preview view beside the
   selection shows the result in real time**. The process pauses if you scroll too fast.

**Troubleshooting — a scrolling capture may stop or produce incomplete results when:**

- the selected area contains animations or videos;
- you scroll diagonally instead of purely vertically or horizontally;
- you scroll too quickly;
- you begin scrolling **upward**.

### 3.4 Window capture

- Capture any window **with its natural macOS shadow**.
- **Hold `Shift`** to select multiple windows and capture them all at once.

---

## 4. Annotation & editing

"Annotate your screenshot the moment you capture it with a rich set of tools."

### 4.1 Annotation tools

| Tool | What it does |
| --- | --- |
| **Markup** | Shapes/labels on the capture (example shows a red box, arrow, and a text label such as "Delete"). |
| **Draw** | Freehand drawing (example shows a large freehand circle). |
| **Pixelate** | Blur/pixelate sensitive content (example: a card number on an ID). |
| **Steps** | Numbered step markers to show a sequence of actions. |
| **Highlight** | Spotlights one region while dimming the rest. |

### 4.2 Related editor behaviors (from release notes 📝)

- Annotate **local image files** in a dedicated annotation window (added 2.0.0/2.0.1).
- Dark-mode support for the screenshot toolbar (added 2.0.0).
- New **arrow styles** (added 2.1.1 and 2.2.0) and improved **default style settings** (2.2.0).

---

## 5. Text Recognition (OCR) / Text Capture

- Recognize **any text on the screen in seconds**, powered by a fast **on-device** recognition
  engine.
- Positioned as "Fast and private first" — recognition stays local (consistent with 100 % offline).
- Added in release 2.4.1 as "Screenshot OCR (Text Capture)".

---

## 6. Pin Images

- **Pin** a screenshot — or any image — as an **always-on-top floating window** on the screen.
- Useful for referencing/comparing content across windows while working.
- Release 2.4.0 fixed pinned screenshots not appearing above full-screen windows.

---

## 7. Color picker 🍎

From the official App Store description:

> "Pick the color of any pixel on the screen and make pixel-perfect capture."

---

## 8. Physical unit measurement 🍎

From the official App Store / Chinese listing:

> "Measure objects by the screen with the physical unit size indicator of selection."
> / "使用物理尺寸单位表示截图区域大小，用屏幕测量物体。"

The selection displays its size in physical units, and multiple units can be switched.

---

## 9. Output, saving & sharing

Documented via release notes 📝 (not fully described on the marketing pages):

- **Custom file names** (added 2.2.1).
- **Open screenshots in Preview** — an option was added in 2.3.0/2.3.1.
- **macOS Share menu** integration (added 2.0.2).
- Implicit standard behavior: copy/save the annotated capture. (Exact export formats and
  copy-to-clipboard behavior are **not documented** on the site — see §12.)

---

## 10. Help / FAQ (as published)

Full FAQ list at https://xnipapp.com/help/ (email support: `xnip.app@gmail.com`).

### 10.1 How to take a scrolling capture?
Links to the scrolling-capture guide summarized in §3.3.

### 10.2 "Account Not In This Store" appears with a "Change Store" button
Usually caused by the current country/region differing from the App Store account region.

1. Sign out of both the App Store and iTunes.
2. Sign in with another account that uses the **same region** as your Apple ID.
3. Restart your Mac.
4. Sign out again, then sign in with your own Apple ID.
5. Try subscribing again.
6. If the issue continues, reinstall Xnip and try once more.

### 10.3 Subscription details are missing after purchase, and Restore Purchase does not work
Usually caused by an unstable network connection during purchase. Delete Xnip, reinstall it from
the App Store, then try restoring the purchase again.

### 10.4 "SKErrorDomain error 0" appears when subscribing
Before trying to subscribe again:

1. Sign out of the Mac App Store, then sign in again.
2. Sign out of iTunes, then sign in again.
3. If the issue continues, reinstall Xnip.

If that does not help, update macOS to the latest version available for your Mac, restart, and try
again.

### 10.5 Something unexpected happens during screen capture
Close every running application except Xnip and try again. If the problem disappears, Xnip is
likely conflicting with another application. Reopen apps one at a time to identify the culprit.
A **keyboard shortcut conflict is a common cause**. (The Chinese FAQ adds: click the Xnip menu-bar
icon and start capture from the pop-up menu as an alternative.)

### 10.6 Payment verification code error (Chinese help only 🇨🇳)
"支付时，需要获取验证码，然后出错" — check whether the current App Store payment method is
Alipay or WeChat Pay.

---

## 11. Privacy & security

From the privacy policy and product pages:

- Xnip is a **fully local app with no network access permission** — "there's no need to worry
  about privacy leaks."
- **100 % offline / on-device** processing (annotation + OCR).
- Release 2.3.0/2.3.1 explicitly states "**Removed network access permission**."
- **Screen Recording permission** is required for capture; release 2.4.2 improved the
  authorization flow for screen-recording permissions.
- Privacy policy collects Log Data (browser type/version, macOS version, timestamps, usage
  statistics) for the website/service and mentions cookies and third-party service providers.
- Children's privacy: service not directed to anyone under 13.
- Website governed by the laws of the People's Republic of China; where language versions differ,
  the Chinese version prevails.

---

## 12. Pricing, licensing & subscription

Official App Store description:

- **Free download** (`Free`), with a **subscription: Xnip Pro**.
- **Xnip Pro** removes the watermark in the screenshot (the Chinese description says the watermark
  in *scrolling capture*) and grants **access to all features in future updates**.
- The subscription **auto-renews**; auto-renewal can be stopped **1 day before the end of the
  current period**.
- A **one-time purchase option for lifetime feature access** was added in release 2.1.0 📝.

> ⚠️ The public `/pricing/` page currently renders only the heading **"Pricing"** with no plan
> details in the served HTML — no price tiers are published there.

---

## 13. Release history (from https://xnipapp.com/updates/)

| Version | Date | Notable changes |
| --- | --- | --- |
| 2.5.0 | Sep 9, 2026 | Optimized scrolling capture. |
| 2.4.3 | Aug 6, 2026 | Fixed known issues. |
| 2.4.2 | Jul 29, 2026 | Improved screen-recording authorization flow; fixed issues. |
| 2.4.1 | Jul 21, 2026 | Added Screenshot OCR (Text Capture); fixed issues. |
| 2.4.0 | Jun 30, 2026 | Fixed pinned screenshots not appearing above full-screen windows. |
| 2.3.4 | Sep 30, 2025 | Fixed bugs and minor improvements. |
| 2.3.3 | Sep 17, 2025 | Fixed bugs; updated app icon for macOS 26. |
| 2.3.2 | Aug 25, 2025 | Fixed app icon issue. |
| 2.3.1 | Aug 20, 2025 | Option to open screenshots in Preview; removed network access permission; fixes. |
| 2.3.0 | Aug 19, 2025 | Same as 2.3.1. |
| 2.2.6 | Nov 26, 2024 | Fixed bugs. |
| 2.2.5 | Oct 10, 2024 | Improved performance. |
| 2.2.4 | Sep 24, 2024 | Fixed bugs. |
| 2.2.3 | Jun 16, 2022 | Fixed bugs. |
| 2.2.2 | Jun 28, 2021 | Added support for macOS Big Sur; fixed bugs. |
| 2.2.1 | Jan 5, 2021 | Added custom file names; fixed bugs. |
| 2.2.0 | Aug 26, 2020 | New arrow styles; improved default style settings; fixes. |
| 2.1.1 | Feb 22, 2020 | New arrow styles; improved default style settings; fixes. |
| 2.1.0 | Feb 17, 2020 | Added one-time purchase for lifetime feature access; minor fixes. |
| 2.0.3 | Dec 29, 2019 | Fixed bugs. |
| 2.0.2 | Nov 29, 2019 | Fixed bugs; added the macOS share menu. |
| 2.0.1 | Nov 21, 2019 | Added image pinning, local-file annotation window, dark-mode toolbar. |
| 2.0.0 | Nov 19, 2019 | Added image pinning, local-file annotation window, dark-mode toolbar. |
| 1.7.4 | Sep 16, 2019 | Fixed a scrolling capture compatibility issue. |
| 1.7.3 | Aug 25, 2019 | Fixed minor bugs. |

App Store "What's New" for **2.5.1** (Sep 27, 2026): "Fixed known issues."

---

## 14. Feature availability summary

| Feature | Free | Requires Xnip Pro |
| --- | --- | --- |
| Screenshot capture (region/window) | ✅ | — |
| Scrolling capture | ✅ with watermark (per App Store wording) | Watermark removed |
| Annotation tools | ✅ | — |
| OCR / text capture | Documented as a feature; plan gating not stated | ? |
| Pin images | Documented as a feature; plan gating not stated | ? |
| Color picker / measurement | Documented as a feature; plan gating not stated | ? |

> The website does **not** publish a free-vs-Pro feature comparison. Only watermark removal (and
> "all features in future updates") is explicitly tied to the subscription.

---

## 15. Undocumented / open questions

Gaps found while reading the official material — useful if a feature-parity implementation is the
goal:

- Exact **default keyboard shortcuts** and how to customize them (only "shortcut conflict" is
  mentioned).
- **Export/save formats**, clipboard behavior, and destination-folder settings.
- Full **annotation tool parameter set** (colors, stroke widths, fonts, shadows, numbering
  reset, undo/redo behavior).
- **Scrolling capture limits** (max length, stitch behavior) beyond the four failure conditions.
- **Full-screen / all-displays capture** modes are not documented on the site.
- **OCR specifics** (languages, copy vs. recognize mode, editable text output).
- **System requirements** beyond macOS 13.5 (Apple-silicon vs. Intel).
- **Pricing tiers / prices** (the `/pricing/` page is empty in the served HTML).
- Which features are gated behind **Xnip Pro** besides watermark removal.
- Whether screenshots can be **uploaded/shared to a cloud or link** (marketing stresses offline;
  only the macOS share menu is documented).

---

## 16. Source URLs (exact)

- https://xnipapp.com/
- https://xnipapp.com/#features
- https://xnipapp.com/scrolling-capture/
- https://xnipapp.com/help/
- https://xnipapp.com/updates/
- https://xnipapp.com/pricing/
- https://xnipapp.com/privacy/
- https://xnipapp.com/terms/
- https://xnipapp.com/zh/, https://xnipapp.com/zh/scrolling-capture/, https://xnipapp.com/zh/help/
- https://apps.apple.com/app/id1221250572 (via iTunes Lookup API, `id=1221250572`)
