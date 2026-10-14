# Changelog

**English** | [简体中文](CHANGELOG.zh-CN.md)

Notable changes in each NSDVR release. Version numbers are the app's `versionName`; a `v<version>` tag publishes the
unsigned build on GitHub with the matching section below as its release notes. The AppGallery version is built and
signed separately from the same source.

## Unreleased

### Fixed

- Picture enhancement keeps its output within the 2000-pixel limit of the system video detail enhancer. On screens
  whose 16:9 picture area is wider than 2000 pixels (4K TVs, wide tablets and PCs, phones in landscape) the enhanced
  picture is produced at 2000 × 1124 and the system scales it the rest of the way, instead of requesting a size the
  enhancer does not support.

## 1.3.2 — 2026-10-13

### Changed

- Settings that this device cannot use are listed anyway, greyed out with the reason, instead of being hidden: the
  device hotspot (no Wi-Fi Direct), picture enhancement (no video processing engine) and the mini window (no
  picture-in-picture). The guide always describes picture enhancement.

## 1.3.1 — 2026-10-13

### Changed

- Picture enhancement is shown when the system declares the video processing engine
  (`SystemCapability.Multimedia.VideoProcessingEngine`) instead of being hidden on every TV. If a device declares it
  but the library is missing, turning enhancement on reports that it is not supported and plays the stream directly.

## 1.3.0 — 2026-10-13

### Added

- TV (smart screen) support. Everything works with a remote control: the arrow keys move a focus ring between mode
  cards, rows and buttons, and OK activates. In the player, OK shows the controls and Menu opens quick settings. Back
  closes the panel, then hides the controls, then asks you to press Back again within 3 s to disconnect. Picture
  enhancement is hidden on TVs because the system there lacks the video processing library.
- The home pages follow device rotation on phones, foldables (cover and inner screen) and tablets, and respect the
  rotation lock in the control centre. While streaming, phones and tablets still lock to landscape.

### Changed

- The dedicated hotspot is described as optional: try it when your network is poor. Its setting description, the TCP
  Bridge tip and the guide were updated.
- IP and hotspot input boxes use a fixed 16 fp font, so the bottoms of letters like y and g in the placeholder are no
  longer cut off.

### Fixed

- Folding or unfolding a foldable kept the old pixel offset, so the page jumped to other content (reported in the
  1.2.0 AppGallery review). The Connect, Settings, About and Guide pages now stay on what was at the top.
- The picture enhancement library is loaded at runtime, so the app starts on systems that don't ship it.

## 1.2.0 — 2026-10-11

### Added

- Mini window (picture-in-picture) for TCP Bridge and USB: going to the home screen or another app moves the stream
  into a system mini window (on by default, switch in Settings and the quick panel); the player's "Mini window" button
  opens it by hand. Nothing reconnects, and the mini window's mute button follows the audio switch.
- Home-screen shortcuts (long-press the app icon): "Connect last" reconnects to the most recent TCP Bridge or RTSP
  device; "Hotspot + connect" turns on the dedicated hotspot and waits up to 90 s for the console to join.
- "Recent connections" under the TCP Bridge and RTSP cards: up to 5 devices per mode that connected successfully,
  one tap to reconnect, long-press to remove.
- The statistics overlay is split into Overview, Video, Audio, Network and Device tabs; the last tab is remembered.
- Power-saving refresh rate: while the stream is visible the app asks for 60 Hz so adaptive-refresh screens can step
  down (the picture is at most 30 fps). On by default.
- Network early warning (TCP Bridge over a router): when the system reports a weak signal, congestion, a predicted
  weak spot or less than 15 Mbps, smoothing pads its buffer up to the current limit until 15 s after the risk clears.
- Guide: what limits the picture (the console's recording output, fixed at 720p / 30 fps) and what the app is meant
  for (temporary play without a dock or capture card).

### Changed

- Keep alive in background: Off / 2 min / 5 min, default 2 min (30 min and 2 h were removed and become 2 min).
- The settings page is a single column on every screen size.
- "Turn off device screen while streaming" is marked experimental and explains how to turn the screen back on.
- USB guidance: a USB-C to USB-C cable may or may not be detected (both ports are dual-role); an OTG adapter is the
  reliable way.
- Extension statistics show console core 3 usage instead of its idle share.

### Fixed

- Reconnecting right after a disconnect was refused for about 10 s (over a minute with Opus audio). With Network
  optimization on, the sockets were handed to the system's network boost service, which kept them open after the app
  closed them. Flows are now marked by address and released when the connection ends.

## 1.1.1 — 2026-10-10

First AppGallery release (invite beta from 2026-10-09). The app is now called **NSDVR**. Version 1.1.0 was a public
beta build that was not approved; its changes are included here.

### Added

- Freeze on frame loss: after a lost frame the picture holds the last good frame until the next keyframe instead of
  showing corruption (gives up after 3 s). On by default.
- Picture enhancement (Standard / High): upscales the 720p picture to the screen with the system's detail
  enhancement; most visible on tablets and PCs.
- Frames are presented in step with the display's VSync.
- More statistics: frame loss by cause, freezes, stutter, present jitter, keyframe interval and the console's free
  memory (protocol 03).
- Support for the experimental NSDVR server extension: switchable audio codecs (PCM 24 kHz, IMA ADPCM, Opus) and
  per-second diagnostics. The options only appear when the connected server supports it (or with developer options);
  with the official SysDVR nothing changes.
- "Open source" card on the About page, and the libopus license.

### Changed

- Smoothing is a single setting shared by TCP Bridge and USB, defaulting to "Low latency" (≤ 80 ms buffer).
- The dedicated hotspot prefers the channel the phone is already on.

### Fixed

- Switching back to "Follow system" after choosing another language kept the old language.

## 1.0.0 — 2026-09-29

First AppGallery submission (withdrawn before release).

- TCP Bridge (Wi-Fi, auto discovery), USB (direct cable connection) and RTSP streaming with hardware decoding and
  low-latency audio.
- Dedicated 5 GHz hotspot over Wi-Fi Direct that the console can join, without a router.
- Background keep-alive with a Live View notification; returning to the app restores the picture within a second.
- Landscape player with lock, quick settings (audio, smoothing, statistics), smoothing and network optimization.
- Layouts for phones, foldables, tablets and PCs; dark mode; Simplified Chinese, Traditional Chinese and English; a
  built-in guide.
