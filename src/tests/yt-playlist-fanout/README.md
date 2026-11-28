# YT-PLAYLIST-FETCH-FANOUT-UNBOUNDED-01 — checked-in two-sided CTest gate

Regression gate for the uncapped `youtube_playlists.txt` fan-out: a user-editable
file with N ids used to queue N yt-dlp fetches, retain N × 500 `QVariantMap`
rows, and spend O(N²) in the `QStringList seen` dedup before the first fetch.

## CTest entries

| Test | What it proves |
|---|---|
| `yt-playlist-fanout-two-sided` | Both arms across an N sweep: RED defect shape, GREEN fixed shape, controls, sanitizer-clean, real backend retention |
| `yt-playlist-retention-real` | Real `YouTubeBackend::assemblePlaylistFromBrowse()` / `readPlaylistEntries()` plateau, eviction order, emit protection, id cap |

Run:

```bash
cmake -B build-gate -DBUILD_TESTS=ON
cmake --build build-gate --parallel 8
ctest --test-dir build-gate -R 'yt-playlist-(fanout|retention)' --output-on-failure
```

`RUN_SERIAL` is set on the two-sided entry because it fits timing curves; the
plain probe is compiled `-O2 -fno-sanitize=address,undefined` so a sanitized
build configuration cannot distort the exponent.

## Two-sided construction

- **RED** compiles two sha256-pinned verbatim product fragments from the
  detached `1045ee5` torture snapshot, in namespace `yt_red`:
  - `playlistIdFromToken.pre_fix.inc` — `898f48fed1c3a8bc6cdf2c9a65fe3affbc3b3c2e9ee4afe1b7d362881c507a90`
  - `readPlaylistEntries.pre_fix.inc` — `332340ae019e5bd45f2058d4b78208120855d69eca4b568e1db4fb82ebf10ee3`
- **GREEN** compiles `yt_playlist_entries_green.inc`, generated at build time
  by `extract_yt_playlist_entries.py` from the checked-out
  `src/modules/youtube/YouTubeBackend.{cpp,h}`. The extractor lifts both
  functions and the four caps by brace matching; a missing signature is a
  hard build error, so a structural regression cannot slip past the gate.
- The orchestrator re-hashes the pinned RED files at test time. If they drift,
  the gate fails even if both behavioral arms happen to look right.

## Behavioral assertions

The orchestrator (`run_yt_playlist_fanout_case.py`) runs both arms with the
same inputs and collects failures rather than stopping at the first one.
Evidence JSON lands in `build-gate/yt-playlist-fanout-evidence.json`.

- **Dedup timing** (plain build, differential = distinct ids − identical ids,
  N = 100/200/400/800/1600, median of 11): RED exponent ≥ 1.6 (confirmed
  torture value ~1.83), GREEN exponent ≤ 1.35 (flat ≈ 0). The differential
  removes O(N) file/parse/alloc cost so it isolates the dedup scan.
- **Fanout capping**: RED accepts all N ids; GREEN accepts at most the
  extracted `kMaxPlaylistFileIds` and fetches no more than that. The extracted
  cap must lie in [1, 1000] so a huge cap cannot make the check vacuous.
- **Retention** (N = 25/100/200/400/600): RED rows exactly `N × kMaxPlaylistItems`
  and strictly monotonic; GREEN rows never exceed `kMaxRetainedPlaylistRows`
  and plateau there for the last three N values. GREEN RSS spread < 4 MB on
  Linux. Extracted budget must lie in [500, 10000].
- **Controls**: empty/zero-id file → 0 refs/0 fetches/0 rows; one playlist →
  1 ref; duplicate lines dedup; comments, blanks and URLs without `list=`
  skipped; concurrency never exceeds the extracted cap (≤ 2).
- **Sanitizers** (Linux, ASan+UBSan, LSan at exit): both arms clean, zero
  `AddressSanitizer` / `UndefinedSanitizer` / `runtime error` diagnostics.
- **Real product retention**: the Qt Test feeds 500-row fetches through
  `assemblePlaylistFromBrowse()` and asserts the plateau at every checkpoint,
  oldest-first eviction, emit-target protection, replacement/failure safety,
  and the 50-id cap through `readPlaylistEntries()`.

## Interpreting a failure

- RED exponent below 1.6 → the pinned control no longer reproduces the
  defect; check the hashes and the control files before trusting any GREEN
  result.
- GREEN exponent above 1.35 or rows above budget → a real product regression
  in the dedup or eviction path.
- `yt-playlist-retention-real` failing alone → the helper exists but is no
  longer wired into the fetch path, or the cap constant changed.
- Sanitizer arm failing → investigate the diagnostic; do not raise the
  thresholds to make it pass.

The RED arm is an intentional negative control, not a failing test: both arms
running in one CTest entry is what makes the gate two-sided.
