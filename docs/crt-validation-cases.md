# CRT validation cases — the first wave (agent drives, no owner in the loop)

## Contract

- **DoD item 5** (240-MP `.hermes.md`): every user-visible behavior must be
  proven by the agent CRT-validation wheel — not by the owner sitting at the
  CRT. Owner CRT judgment survives only as a one-line review comment on cases
  where no invariant can be encoded.
- Each case is a replayable card: exact key sequence + expected visual
  invariants + evidence bundle (screenshot, state-log tail, vision verdict).
- The wheel is driven through the debug key channel (`240mp_key.in` on the Pi),
  with named captures (`shot <id>`), and the state log carries `view:<tag>`
  markers so the agent can assert "on screen X". Vision runs last on the
  app-initiated screenshots; the state log is ground truth for nav depth and
  key delivered/suppressed.
- The wheel is idempotent and safe: navigation/playback keys only, never writes
  config, restores the Pi to the deployed revision.

## First wave (cases in `cases/<name>.yaml`)

Run: `scripts/crt-validate.sh <case-name>` (from the repo root).

| Case | Keys | Assertions | What it proves |
|---|---|---|---|
| `navigation-walk` | up, select, shot nav01, up, select, shot nav02 | `view: module-list`, shot markers | ModuleList open → select a module → open its root view |
| `playback-loop` | shot play01, playpause, shot play02, right, shot seek01 | `view: player` + shot markers | Playback open, play/pause toggle, fine seek |
| `settings-roundtrip` | shot set01, select, left, right, shot set02 | `view: settings` + shot markers | Settings open + cycle, screen reads back |

Each case prints `PASS` only when every `expect:` grep pattern matches the
state-log tail. Evidence is produced on the Pi, pulled to
`.evidence/<case>/` in the runner sandbox, and uploaded as CI artifacts.

## How to add a case

1. `cases/<name>.yaml`: `keys:` (one per line; `shot <id>` = named capture),
   `expect:` (state-log grep patterns that must match).
2. Run locally: `cd /home/ian/src/240-MP && scripts/crt-validate.sh <name>`.

## Notes for the engine

- The wheel reads keys from `240mp_key.in` on the Pi (debug key channel).
- `shot <id>` renders `240mp_screenshot_<view-tag>_<id>.png` and writes a
  `view:<tag>` line to the state log via `AppCore::note_view`. Both were
  added in the debug-stack pass (see commit on `dev`).
- If a case goes FAIL, the evidence bundle is the gate for the next session;
  do not hand-edit a case card into PASS.
