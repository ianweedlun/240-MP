# 240-MP — fork work vs upstream, 2026-10-28 → 2026-10-31

Retrospective report requested in #21. Scope: everything the fork (`ianweedlun/240-MP`)
adds beyond `anthonycaccese/240-MP` (upstream `main` stays a pure mirror). All claims below
were verified by CI, Pi state logs, screenshots, or unit tests — not "code exists".

---

## 1. Major improvements by area

### RetroArch — from "module present, unusable" to full functionality
- **Playlist browsing** (RZIP detection, per-system playlists, 19 short system names
  `SYSTEM (Company) → NES, GENESIS…`, game counts per playlist, flat nav straight into
  Playlists — the Systems view proved dead weight and was deleted).
- **Launch chain end-to-end**: core resolution, `archive.zip#entry` rom paths resolved
  (QFileInfo-vs-`#` split), DisplayHandoff child ownership, verified on the CRT with
  *10-Yard Fight* (Nestopia).
- **Boxart with real layout design**: zone geometry so boxart and list never fight for
  anchors; `thumbnail_display` OFF/LEFT/RIGHT honored faithfully (OFF was the third
  branch that had always silently fallen through to RIGHT); playlist views stay plain.
- **Snappy menus**: lazy per-row thumbnail resolution — ~4,700 upfront `findThumbnail()`
  calls became on-demand (`resolveThumbnail` Q_INVOKABLE).
- **Live sort**: SYSTEM/ALPHA/COMPANY re-applied on settings change (unit-tested).
- **Module-exit navigation**: the exit-after-game-lands-on-Settings bug, root-caused to
  dead code (see §3), fixed and proven with a state-log trace + screenshot of the landing.

### Weather — black screen → working with volume OSD
Black-screen fix, OSD volume overlay behavior, AQI/UV revisit tracked; merged into dev
green on arm64 + macOS.

### Playback (mpv) — one source of truth
`~/.config/mpv/mpv.conf` becomes the single source of truth for app-level options; HID
media keys → mpv IPC. The RetroArch work deliberately did not break the mpv-assisting
shared code (DisplayHandoff caution, honored).

### The platform around the app — biggest structural win
- **CI → Pi, hands-free**: arm64 + macOS builds on any push to `feature/**`; `deploy-pi.yml`
  installs the tarball through the `ghdeploy` key-only chain (legacy restricted `no-X11-`
  options, root-side `240mp-ci-deploy` wrapper, reboot-dance = dead-connection-is-success),
  md5 gate + service-active verified. Markdown-only pushes queue zero builds
  (`paths-ignore`); macOS builds live in their own workflow so Pi deploys never wait on
  mac-runner capacity.
- **Debug stack** autostarts on boot: control server :8081 (key injection), F12 screenshots
  app-side (EGLFS/DRM reality — MJPEG viewer was useless and removed), view-tagged
  screenshot filenames.
- **Telemetry tier**: state log (`load/navigate/back/select/key-suppressed` lines with
  stack depth) → debug key channel → view-tag → pgrep/journal truth → vision last. A
  `TELEMETRY=` compile gate strips everything in release; QML kill-switch matches.
- **Offscreen fast path**: `QT_QPA_PLATFORM=offscreen` + synthetic key channel makes full
  nav loops seconds long locally; the Pi loop stays for CRT/visual truth only.

### Pipeline & process (self-imposed)
- Definition-of-done contract (screenshots gate user-visible behaviors), conventional
  commits, `.hermes.md` single-sourced on dev and blob-mirrored to every branch
  (sync-hermes workflow + proven plumbing fallback with 000644-mode pitfall fixed).
- Issue-channel comms protocol with the maintainer (confirm-before-acting), distinct
  Hermes commit identity (env-based, repo config untouched), agent-post signature markers.
- Keep a Changelog maintained alongside the work; plan contracts (A–F items) as the durable
  task record; convergence watcher for deploy ping-pong.

## 2. Blockers removed (the unglamorous half)

| Blocker | How it died |
|---|---|
| Pi powers off on any 240mp exit | `ExecStopPost=+/usr/local/bin/240mp-stop` on every exit → diagnosed, reboots-only rule |
| Pi unreachable from CI | no public DNS/port-forward reality → ghdeploy key-only over WAN:2222→LAN:22, agent uses LAN path |
| macOS CI failing (cmake --install --parallel) | invalid flag for that cmake → corrected; validated by green runs |
| CI "ran for hours" myth | actually minutes/round — measured, not assumed |
| macOS builds blocking Pi deploys | separate ci-macos.yml on same triggers |
| Deploy installs wrong branch (ping-pong) | `--latest` selector + convergence watcher + md5 verify gate |
| `.hermes.md` edits trigger CI | `paths-ignore: ['**.md']` across build workflows |
| sync-hermes "completed" but branches stale | never trust workflow status → blob-compare; plumbing mirror commits; 000644 → 100644 file-mode fix |
| `deploy-pi.sh --latest` grabbed probe runs / upstream repo | workflow-filtered `gh` + hardcoded `-R` fork |
| Game quit lands on Settings (twice-reported) | root cause was `eventFilter` never installed (Qt6 dropped the Qt5 global-handler idiom) → `installEventFilter` on the root window + raw-channel game gate |
| thumbnail OFF renders as RIGHT | cycler saved objects vs manifest strings; unset-vs-OFF default semantics split |
| Zip#entry games refused launch | existence-check on the composite path → split on `#` |
| Module open slow (4,700 items) | lazy thumbnail resolution, per-row |
| Screenshot log-guessing | state log + view-tagged filenames made vision a last resort |
| Key telemetry permanently silent | same root cause as the exit bug: dead eventFilter — one fix served both |

## 3. Lessons learned (transferable)
1. **Verify the mechanism, not the symptom** — "compiling is not done"; the exit bug's real
   culprit was dead code, invisible for months, found by proving which paths actually run.
2. **Cheap oracles first.** Screenshots cost vision calls; state logs, view tags, and pgrep
   answer 90% of questions free. Vision is reserved for visual truths.
3. **The environment has opinions** (ExecStopPost powering off, restricted authorized_keys,
   `restrict=` unsupported, EGLFS framebuffer invisibility) — trust-but-verify beats
   docs-belief everywhere around this Pi.
4. **Batch expensive operations.** Pi reboots are minutes-of-power operations; never for
   trivial verification.
5. **Own your branch topology.** dev is the integration branch; main never hand-pushed;
   feature branches fork dev. Deploy target = default-branch tree, or the chain silently dies.
6. **Instruments are features, not accessories** — RowTrackingListView, the debug key
   channel, and telemetry gates are first-class code with a compile-time removal path.

## 4. Knowledge and tools the agent built (inventory)
- **Skills**: `240-mp-development` (SKILL.md + references: debug-telemetry.md,
  comms-attribution.md, Pi/CI plumbing, git-identity recipes); project .hermes.md as the
  per-branch contract.
- **Helpers**: `pi-nav.sh` (control-server key loop + `local` offscreen mode), `pi-keys.py`,
  `deploy-pi.sh --latest`, `mirror-hermes.sh`, `issue-watch.sh` (2-min issue watcher),
  `converge.sh` deploy convergence poller.
- **App-side instruments**: state-log sink, `note_view()` view tagging, debug key channel,
  `TELEMETRY` gate, RowTrackingListView, F12 capture.
- **Hard-won platform facts**: Pi never permanently powered; key-only ghdeploy model;
  offscreen-is-the-fast-path rule; no secrets in worktrees/pushes.

## 5. Idea ownership (honest split)

### Maintainer's ideas
- The whole product direction: features that improve daily use — weather volume OSD,
  RetroArch browser on the CRT, "things that matter to me at home".
- Golden test rig = Pi 4B+ → CRT composite; tap-don't-hold key discipline; flat
  Playlists nav (skip Systems); boxart in its own box, no name text, LEFT/RIGHT/OFF as a
  user-facing setting; SYSTEM/ALPHA/COMPANY sort intent; short system-name headers.
- Process rules: issues live on the fork; no direct pushes to main; branch review flow
  (compare main vs upstream, split by feature); Keep a Changelog; overnight autonomous
  work with upfront context; "a fix isn't valid until validated" (the definition-of-done
  gate); screenshot-during-game ban; issue-channel protocol (confirm seen, reply required,
  one issue for progress reports); authorship markers; report request (#21).
- Physical reality only an eyewitness supplies: poweroff-loop reports, what the CRT shows,
  "this is still happening".

### Agent's ideas
- Dead-code `eventFilter` discovery and the raw-channel suppression fix; the two-stack
  (module/app) nav model as the mental frame; the telemetry hierarchy and its compile-time
  release gate; RowTrackingListView as a core primitive (aligned with the user's
  extendability philosophy); the offscreen key channel as a testing instrument;
  F12 + app-side screenshots (offered as options A/B, user picked A); CI direct-deploy
  chain design (ghdeploy/reboot-dance/md5 gate); macOS-workflow split; `paths-ignore`;
  convergence watcher; distinct commit identity for attribution audits; issue-watcher
  mechanics; lazy thumbnails; the `#`-split launch fix; RZIP parser; state log over vision
  as a standing rule.
- Gray zone (user intent + agent formulation): sort semantics, definition-of-done
  wording, "trust but verify" sharpening, plan contracts A–F.

---

*Compiled 2026-10-31 21:0x ET by the Hermes Agent from real CI/state-log/changelog records.*
