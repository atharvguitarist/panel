# Panel — Phone Control Page

**The remote-control surface for the Panel smart wall switchboard.**
A single static page that gives every installed panel a native-feeling phone
app — with no accounts, no backend, no app store, and nothing stored on any
server.

> This repository contains the control page only. Device firmware is
> maintained separately and is not published here.

---

## Contents

- [Overview](#overview)
- [System architecture](#system-architecture)
- [How a panel is paired](#how-a-panel-is-paired)
- [Privacy by design](#privacy-by-design)
- [Security model and threat analysis](#security-model-and-threat-analysis)
- [Features](#features)
- [Installed-app behaviour](#installed-app-behaviour)
- [Design system](#design-system)
- [Deployment](#deployment)
- [Compatibility](#compatibility)
- [Repository policy](#repository-policy)
- [License](#license)

---

## Overview

Panel is a wall-mounted smart switchboard built on an ESP32-S3 with a 7-inch
capacitive touch display. It replaces a conventional switchboard anywhere in a
home — front door, living room, bedroom — and controls connected lighting with
motion-aware automation, on-device weather, activity logging, and push
notifications to the household's phones.

This page is the piece that lives on the phone. It presents four controls —
**All Lights Off**, **Check Status**, **Restart Panel**, and
**Allow Update** — and the notification set-up for the household. It is
deliberately a single HTML file with no framework, no build step, and no
external dependencies beyond the messaging relay, so that it can be audited in
one sitting and hosted anywhere static files are served.

The design goal was a control surface that a non-technical family member can
set up in under two minutes from a QR code on the panel itself, and that
continues to work from anywhere in the world without the household ever
creating an account.

---

## System architecture

Three parties, one public relay, zero application servers.

```
┌───────────────────────┐        publish          ┌──────────────────┐
│  Phone                │ ──────────────────────▶ │                  │
│  (this page, added    │   POST  <topic>-cmd     │     ntfy.sh      │
│   to home screen)     │                         │   public relay   │
│                       │ ◀────────────────────── │                  │
└───────────────────────┘   push  <topic>         └────────┬─────────┘
          ▲                   (ntfy app)                   │
          │                                                │ subscribe
          │ scan QR                                        │ (held-open stream)
          │                                                ▼
┌─────────┴─────────────┐                         ┌──────────────────┐
│  Panel display        │                         │  Panel firmware  │
│  Settings › Network › │                         │  (ESP32-S3)      │
│  Phone setup          │                         │                  │
└───────────────────────┘                         └──────────────────┘
```

**Identity is a topic name.** Each panel mints a unique notification topic on
first boot and uses two addresses derived from it:

| Address | Direction | Purpose |
|---|---|---|
| `<topic>` | Panel → phones | Doorbell and motion alerts; command confirmations |
| `<topic>-cmd` | Phones → panel | Command channel the four buttons publish to |

The panel holds one persistent subscription to `<topic>-cmd` and executes a
strict whitelist of commands. Every command is acknowledged back on
`<topic>`, so a household subscribes to **exactly one** topic and sees alerts
and confirmations in the same stream.

**This page is stateless and universal.** One copy serves every panel ever
manufactured. The per-device part — the topic — arrives in the URL fragment
and never leaves the phone.

---

## How a panel is paired

1. During first-time set-up, after WiFi is configured, the panel shows a
   **Phone Setup** screen: a QR code and the panel's topic name.
2. The QR encodes `https://<this-page>/#t=<topic>`.
3. Scanning it opens this page with the four controls live.
4. The user adds the page to their home screen (iOS: Share › Add to Home
   Screen; Android: the install prompt). It then launches full-screen as a
   standalone app with its own icon.
5. The same screen walks them through installing the free **ntfy** app and
   subscribing to the topic for notifications.

The Phone Setup screen remains available in the panel's settings, so a second
family member — or a replaced phone — can be paired at any time without a
reset.

---

## Privacy by design

The hosting server for this page learns **nothing** about any panel or
household. This is a property of the architecture, not a policy promise:

- **The topic travels in the URL fragment** (`#t=…`). Browsers never transmit
  fragments to the server in any HTTP request. The host sees a request for
  `index.html` and nothing more.
- **No cookies, no analytics, no third-party scripts, no fonts fetched.** The
  page is self-contained; its only outbound network activity is the command
  POST to the relay when a button is pressed.
- **No server-side state anywhere.** There is no database, no user table, no
  session. The relay (ntfy.sh) holds messages transiently and is not operated
  by this project.
- **Persistence is local to the phone.** The topic is remembered in the
  browser's `localStorage` solely so the installed app can launch without the
  fragment. It is never synchronised anywhere.

A reviewer can confirm all of the above by reading `index.html` — it is the
only file with behaviour.

---

## Security model and threat analysis

This section is written to be accurate rather than reassuring.

**The topic name is the credential.** The relay is a public service: any party
who knows a topic name can subscribe to it or publish to it. The system's
security therefore rests entirely on topic names being unguessable.

**Topic generation.** The firmware mints topics from the ESP32's hardware
random-number generator in the form `word-word-xxxx`: two words drawn from
curated 64-entry lists plus a four-character suffix from a 31-symbol alphabet
that omits visually ambiguous characters. The space is 64 × 64 × 31⁴ ≈
**3.8 × 10⁹** combinations. Topics are never chosen by users, which removes
the dictionary-word and reuse failure modes that user-chosen names exhibit.

**What a compromised topic allows.** An attacker who learns a panel's topic
can read its notifications and send the four whitelisted commands: switch
lights off, request a status report, trigger a restart, or open a firmware
upload window that is reachable **only on the panel's local network** for
five minutes. There is no command that unlocks anything, reads stored data,
or exposes credentials. The firmware ignores all commands during an active
firmware write and never replays queued messages after a reboot, which
precludes command-induced boot loops.

**What this page cannot do.** It cannot read notifications (it only
publishes), cannot discover topics, and cannot reach a panel except through
the relay. Loss of the hosting server disables the buttons but has no effect
on the panel, its automation, or its notifications.

**Known limits and the upgrade path.** Because the relay is public, anyone
who *observes* a topic — on the panel's Phone Setup screen, or in a shared
control link — holds that panel's credential. The designed mitigation for
volume deployment is the relay's reserved-topic access control (available
on ntfy's paid tier or a self-hosted instance), under which the panel
authenticates with a token and unauthenticated parties can neither read nor
publish. This requires a small firmware change and no change to this page.

**Transport.** All traffic between this page and the relay is HTTPS.

---

## Features

| Feature | Detail |
|---|---|
| Four controls | **All Lights Off** (primary), **Check Status**, **Restart Panel**, **Allow Update · 5 min** |
| Deliberate hierarchy | The daily action is a large primary button; the three administrative actions are visually secondary |
| Confirm-twice on destructive actions | Restart and Allow Update require a second tap within 3.5 s, so an accidental pocket press cannot reboot a panel |
| Honest feedback | The status line reports *Sending…*, *Sent — watch for the confirmation*, or *No connection* — the panel itself confirms execution via notification |
| Notification set-up | Topic displayed with one-tap copy; direct links to the ntfy app on both stores |
| Topic persistence | Remembered locally so the installed app launches correctly; re-pairing simply overwrites |
| Defensive parsing | Topics are sanitised to the relay's legal character set before use; hostile fragments cannot form an unintended request |
| Dark mode | Full palette redefined under `prefers-color-scheme: dark` |
| No-panel state | Opening the page without a pairing shows guidance instead of a broken UI |

Command delivery uses a normal `fetch` first and falls back to `no-cors` mode
if cross-origin headers are absent, so a command always goes through; the
panel's push confirmation is the source of truth for execution.

---

## Installed-app behaviour

The page ships with a web app manifest and platform icons so that "Add to
Home Screen" produces a genuine app experience:

- **Standalone display** — launches without browser chrome on both iOS and
  Android.
- **Icons** — 192 px and 512 px, plus a maskable variant for Android's
  adaptive icon shapes and an Apple touch icon.
- **Fragment handling** — a manifest's `start_url` cannot carry a per-device
  fragment, so the topic is stored on first open and restored on launch. iOS
  additionally preserves the fragment in the saved bookmark. Both routes are
  handled.

---

## Design system

The page shares a visual language with the panel's on-device UI so that the
two read as one product.

| Role | Value | Notes |
|---|---|---|
| Background | `#F6E9CC` | Warm cream, matches the panel's settings screens |
| Card | `#FFFDF6` | Near-white, hairline border `#E3D8B8` |
| Primary action | `#7E102C` | Burgundy — the palette's true value |
| Accent | `#C9A85C` | Gold, used sparingly |
| Text | `#3D3830` / `#5F4712` | Warm ink and gold-text for headings |

A deliberate detail: the device firmware uses slightly *counter-tinted*
variants of these colours to compensate for the display panel's colour shift.
Phone screens render accurately, so this page uses the palette's true values —
the two surfaces therefore match in perception rather than in hex.

Typography is the system font stack. No web fonts are loaded, by design: it
removes a network dependency, a privacy leak, and a render delay.

---

## Deployment

Static hosting, no build.

```
index.html               The entire application
manifest.json            Web app manifest (standalone display, icons)
icon-192.png             Home-screen icon
icon-512.png             High-resolution icon
icon-maskable-512.png    Android adaptive-icon safe-zone variant
apple-touch-icon.png     iOS home-screen icon (180 px)
.nojekyll                Disables Jekyll processing on GitHub Pages
README.md                This document
```

All paths are relative, so the page works at any URL. The panel firmware
carries the base URL as a single constant, with a per-device override
available through the panel's provisioning portal — a hosting move is
therefore recoverable on installed units without reflashing.

Served via GitHub Pages from the `main` branch with HTTPS enforced.

---

## Compatibility

| Platform | Browser | Standalone install | Notes |
|---|---|---|---|
| iOS 15+ | Safari | Yes (Share › Add to Home Screen) | Fragment preserved in bookmark |
| Android 8+ | Chrome | Yes (install prompt) | Maskable icon used |
| Any | Modern browser | Works as a normal page | Standalone not required for function |

Requires JavaScript. No polyfills are included; the page targets browsers from
the last five years.

---

## Repository policy

This is a product component, not an open-source project.

- Contributions, pull requests, and issues are **not accepted**. Issues,
  Discussions, Wikis, and Projects are disabled, and interaction is limited to
  collaborators.
- The `main` branch is protected against deletion, force-push, and non-linear
  history.
- Workflows are restricted to GitHub's own Pages deployment; no third-party
  actions can run.
- The repository is public solely because GitHub Pages requires it. Public
  visibility exposes nothing beyond the page itself, as described under
  [Privacy by design](#privacy-by-design).

---

## License

Copyright © 2026. **All rights reserved.**

No licence is granted to use, copy, modify, or distribute any part of this
repository. The page is published for the exclusive use of Panel devices and
their owners.
