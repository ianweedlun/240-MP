# Virtual channels: presets + settings consolidation (2026-11-09)

Status: Unit A implemented + lab-proven on dev. Units B–D pending. Nothing ships to
beta outside Ian's hand-merged dev→beta PR.

## Locked design contract (from the interview, verbatim decisions)
- Presets **coexist** with manual customization — nothing about customization is removed.
- Preset vocabulary is **exactly three recipes**: by genre, by network, by collection.
  No fourth. "By network" is the anchor concept: the network/studio field is what makes
  a preset a real channel.
- Multi-server source union: deduped **by title** across all enabled servers.
- Server-unavailable state: **keep the last-known lineup**, mark stale at most — never
  drop or gray out the channel.
- Auto-branding: presets derive identity from their subject (name after the
  network/genre/collection, pick the matching look). Named looks belong to manual
  channels only.
- Naming trap: upstream says `auto_crop`; this fork keeps `auto_fit` / `autoFitEnabled()`.
  Never reintroduce `autoCropEnabled`.
- Settings consolidation = option **C**: section-based + deleting rows that don't belong
  to a 240px dial surface. The volume row is **removed** (media keys own volume).

## Settings consolidation (Unit A — done)
- Manifest sections: **DIAL** (media folder, weather channel, schedule-ahead) and
  **GUIDE** (single idle row, preview, weather music). 18 → 8 selectable rows.
  Removed: volume + normalize (media keys), audio/subtitle selection (per-title player
  choice), tuning/banner/logo looks (auto-branding takes it off the user),
  `rebuild_all` (the top-up already runs at boot and 04:00 — that row was the
  visible bug in the original report).
- Merged: `guide.autoscroll` + `guide.resume_seconds` → `guide.idling`
  (Keeps Crawling | Holds · 30s/1m/5m/Never). Guide reads the merged row
  (schema default runs; legacy pair kept as a safety net) and logs the resolved
  mode to the debug state channel.
- `ModuleSettings.qml` now carries the core `Settings.qml` section-row contract +
  `snapSelectable(idx, dir)` so nav never lands on or stops at a section row.

## Lab proof (pre-push, native offscreen, shared key channel + state log)
- Row stream selects: `Enabled idx=0 → Manage idx=1 → Media Folder idx=3 → Weather
  Channel idx=4 → Schedule Ahead idx=5 → While Idle idx=7` — section rows skipped both
  ways, never selected.
- Save round-trip: cycling the merged row wrote `config.json`
  `"guide": {"idling": "Holds · 1m"}`; re-entering the module re-read it and emitted
  `guide-idle auto=on resume=60s src=setting`. Boot read of the schema default emitted
  `guide-idle auto=on resume=5s src=setting` (= "Keeps Crawling" mapping).
- Screenshot artifact: 10 text-row bands, expected order, selected row distinct
  (objective pixel probe; vision-model analysis was provider-blocked that hour).
  Deeper visual truth = Ian's CRT gate when this lands through his hands.

## Next units
- **B — backend recipe channels** (pure logic, fixture-driven, covered by
  `vchan-tests`): scan library/server metadata → genre/network/collection grouping
  → preset channel with rolling schedule; union+title-dedup across enabled servers;
  stale-marking on server outage.
- **C — Manage preset add-flow + auto-brand views** (preset add = pick a recipe + a
  subject value; identity auto-derived).
- **D — art hold**: any *art* asset for look branding (logos, title cards) waits for
  Ian's libre finds; palettes themselves are color+type parameters already inside the
  Logo/Banner internals, so B/C mechanics never need art to test. Attribution rule
  rides on whatever gets dropped in later.
