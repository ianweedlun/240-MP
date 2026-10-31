# RetroArch: Full Functionality — Work Plan (resume anchor)

Status: **in progress** on `feature/retroarch-full-functionality`.
Session handoff date: 2026-10-30 (session was context-compacted multiple times; this doc is the resumption contract).

## Live state (verified, not aspirational)
- Branch tip: `0ec9644` (thumbnail-DB fix + house-style logo). Pushed; CI arm64 + macOS green.
- Pi: running **that exact tip**, deployed by hand via `scripts/deploy-pi.sh` with the CI artifact
  `240-MP-feature-retroarch-full-functionality-linux-arm64-0ec96444.tar.gz`; rebooted; `240mp.service` **active**;
  on-disk binary md5 `0bf6d96553d4a98935ea8d7ace18a995` (binary-vs-tarball md5 match NOT yet re-confirmed — first task of next session).
- `dev` carries the definition-of-done contract at `f241438` (all 6 branches see it via sync-hermes.yml).
- Tracking issues (fork `ianweedlun/240-MP` — our tracker; upstream issues are NOT used): #16 this feature, #17 Weather OSD bugs, #18 Playback settings submenu. Upstream #314/#315/#316 closed as pointers-to-fork (earlier session posted upstream by gh-default mistake; `gh repo set-default ianweedlun/240-MP` now configured in this repo dir).
- Tests: retroarch suite **17/17** local green. AQI/UV weather experiment: shelved on
  `feature/weather-aqi-uv` (tip `872f5de`, unmerged, revisit later — do not delete).
- Uncommitted: this doc only (committed locally, **not pushed** — next session pushes it with the first code batch).

## Validated by user on the CRT (wins)
- Logo: official space-invader mark, house style, matches theme colors.
- Long-title marquee scrolling: works.
- Thumbnails: display (DB-dir case fix confirmed end-to-end).

## Work items (agreed order)
A. **Workflow fix — DO FIRST.** Two proven bugs:
   1. `scripts/deploy-pi.sh --latest` picked the *probe* run id and `gh` defaulted to the **upstream** repo for artifact download. Fix: filter runs by `name == "CI"` + exact head_sha match; pass `-R <fork>` (derive from origin URL).
   2. `deploy-pi.yml`'s workflow_run trigger has **never** fired a Deploy for feature-branch CI (empirical: every Deploy run to date is dev-triggered). Fix by not fighting GitHub: keep the CI-completion trigger for `dev` only, add a second `push: feature/**` trigger that resolves the CI run by exact `head_sha` and deploys its artifact. Feature previews then deploy reliably; contract flow (Pi-test-before-merge) preserved.
   Acceptance: one feature push produces a Deploy run; Pi md5 == artifact md5.

B. **Snappy loading (P0).** User observed: playlist view slow; hypothesis confirmed by code: `parsePlaylist` resolves `findThumbnail()` for **every game in all 19 playlists** (~4.7k games × 5 dirs × 2 stats + listings) at module open. Plan: parsing becomes pure — thumbnail path resolution moves to **lazy, per rendered row**: add `Q_INVOKABLE QString RetroArchBackend::resolveThumbnail(const QString &label, const QString &playlistBasename) const` (same rules as `findThumbnail`, covered by existing tests), views call it only for visible ListView rows. If journal timings show RZIP inflate dominating, also make playlist parse lazy per playlist (measure first — journal qInfo timing lines exist; get numbers, not vibes).
   Acceptance: journal timing before/after shows material improvement; UI snappy in screenshots sequence.

C. **thumbnail_display stuck on RIGHT (P0).** Reproduce first: change setting in app, diff `/home/ian/.config/240mp/config.json` (path to confirm — AppCore ConfigLocation), grep journal for the migration qInfo. Prime suspects: migration re-inserting default every boot, or Settings writing wrong section/key. Root-cause fix, then acceptance = **three screenshots**: LEFT, OFF, RIGHT behaviors visibly distinct.

D. **Game launch (P0).** User pressed Return on several games; nothing started — journal already holds the refusal reasons (`[RetroArch]` qWarnings: no ROM path / core / handoff decline). Grep journal evidence FIRST, fix the core-path/invocation chain (`resolveCorePath`, `default_core_path`, `retroarchPath`, display handoff), then the agreed R3 acceptance: **one representative title per playlist, real CRT boot + screenshot** each. Mapping gaps get fixed in code, not papered over.

E. **Playlist sort setting (P1).** New manifest `list_single` setting `playlist_sort`: `SYSTEM` (current shortName sort, default), `ALPHA` (raw playlist basenames), `COMPANY` (alpha by company prefix = text before first ` - `; no-company playlists fall back into plain alpha — generalization rule agreed). Sort keys are pure logic → unit-test like other pure logic. Acceptance: screenshot of both ALPHA and COMPANY orders.

F. **Side-panel boxart (design note from user — new).** The preview panel currently shrinks the thumbnail so the game's name prints below it. Spec: **thumbnail displays at full box size; the side panel highlights boxart only — remove the redundant name text** (name already lives on the row). Files: `modules/retroarch/views/GamePreview.qml`, `Games.qml` previewLoader, mirror in Playlists/Systems panels if same pattern. Acceptance: screenshot — full-box art, no name text in panel.

## Queued behind this feature
- Weather OSD bugs (#315): OSD appears on module open; volume wraps-to-middle at last level instead of clamping. `modules/weather/views/Weather.qml` volumeBarOverlay initial visible state + arithmetic on the 20-tick model.
- Settings "Playback" submenu (#316).
- AQI/UV weather revisit (`feature/weather-aqi-uv`).
- Definition-of-done contract in .hermes.md applies to each item above: code+tests+CI+deploy+**screenshot per user-visible behavior**+changelog+issue close.

## Pi operating notes (unchanged, load-bearing)
- Never `systemctl restart/stop 240mp.service` (ExecStopPost powers off Pi) — reboot only.
- Pi is often powered off; confirm state after reboot over SSH (ian key-only: `~/.ssh/id_pi_agent`, config stanza present).
- Control server `POST :8081/key` `{"key":…,"press":true}`; F12 → screenshot to `/tmp/240mp_screenshot_*`; SCP back; vision-check. Tap, don't hold.
- Long-title marquee test fixture: NES playlist label `Nantettatte!! Baseball + Nantettatte!! Baseball - Ko-Game Cassette - '91 Kaimaku Hen (Japan)` at item index 853.

## Resumption protocol for next session
1. Re-confirm Pi binary md5 vs artifact (A's gate).
2. Start at A (workflow), then B (measure→lazy), C (reproduce→fix), D (journal→fix→per-playlist launch), E, F — batching deploys: as many items per build as diagnosis allows, one reboot per batch.
3. Push `docs/plans/…` with the first code push.
