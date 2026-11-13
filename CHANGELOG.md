# Changelog
All notable changes to this project (the fork's own work — upstream changes arrive via merges).
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## Unreleased

### Added — Virtual channels: bundled preset network & studio logos (Unit D)
- **Auto-branded preset channels**: creating a TV Network or Movie Studio preset channel now automatically assigns a matching corner logo (`set_channel_logo` / `preset_logo_for_subject`), using a normalized alias table that maps `library.db` studio/network variants (e.g. `Warner Bros. Television` → `warner-bros.png`, `Williams Street` → `adult-swim.png`) to canonical marks.
- **38 monochrome white/transparent PNG logos** (`modules/virtual_channels/assets/logos/*.png`), rendered from Wikimedia Commons Public Domain SVGs and attributed in `THANKS.md` (covering major US TV networks, animation/cable brands, and movie studios present in `library.db`).
- **Seamless user override**: `list_logos()` and `logo_path()` resolve `<dataRoot>/logos` first and fall back to the bundled `assets/logos/` directory, so user-dropped files override bundled marks by filename and bundled marks remain selectable in `LogoPicker`.
- Test surface: new `testPresetLogos()` in `VirtualChannelsBackendTest` verifying bundled fallback, user override precedence, deduplicated `list_logos()`, alias normalization, and `create_preset_channel()` auto-branding (network/studio only, never Genre). Full suite **14/14 green**.

### Added — CHANNELS: the dial reads its shelves
- **Local library index + launch walk**: the module looks its shelves over once at start ("Scan Library at Start", on by default in the new Shelves section), so the add-on lists offer only what is actually watchable — ordered by how many titles carry each category, count beside every row ("12 shows") — instead of an alphabetic catalog list of things nobody owns. A fresh app, an un-scanned app, and a viewer who turns it off keep the old lists byte for byte. A server that stays offline keeps its last-known lineup instead of darkening its channels; boxset shelves contribute their real counts; a genre nobody has stops being offered rather than sitting at a stale count.
- **Network questions the shelves answer skip TheTVDB**: a show the walk saw a network for answers from the catalog's own network listing, so a seed pays zero searches for it — cold or warm, across boots. Answers TheTVDB does provide are taught back into the index, so a seed never re-pays a question it already asked.
- **Channel settings declutter**: the Weather Channel row and the guide's Weather Music row only appear while the weather module is on — a control for something the viewer cannot turn on is clutter, not a choice.
- Built on Qt's Sql module (optional): a build without it compiles the index's unavailable-shim half and keeps the catalog path, so the index can never break a platform's build — the Pi ships it (Qt 6.8's Sql is packaged on Debian 13) and dev/CI use 6.10's.

### Fixed — Virtual channels: the index stops remembering what the shelves forgot
- **A show you deleted from a source now leaves the lineup instead of lingering forever.** The index merges tallies per (source, channel) pair: when a source is walked, its own rows follow the walk — a show the walk didn't mention drops, so a genre nobody has anymore stops being offered and the counts beside the option rows stop lying. A source that stayed dark this walk keeps its last known lineup, so one offline server can never darken a channel. A show's row drops only when **every** source that walked over it has stopped mentioning it — sources that never walked keep their shows standing through partial walks. Unit-pinned in the lab (`LibraryDbTest`: walked-unmentioned drops, dark-source kept through a Plex-only walk, dropped once Emby walked over it too).

### Fixed — Virtual channels: a genre recipe airs the shows that carry the genre

- **TV genre recipe swept the MOVIE library and kept airing "Private Snafu"** (owner report: the genre channel always landed on the same theatrical short). Both `readPools` sites forced `wants=Films` for any genre recipe — the plan-block `Draws::Genre` case and the `genre` pool-entry kind — so a TV channel asked the server for films in that genre and whatever shorts happened to carry it won. Now `Films` is forced only when `isMovieChannel(channel)`, mirroring the Collection twin; a TV genre keeps its `Episodes` scan.
- **The enumeration then ignored the genre entirely**: the genre lives on the **series** item, not on its episodes, so an episode scan that never consults show-level genres can't narrow at all. `MediaServerSource` with a genre filter now asks `load_items` for `Series,Episode` beside the flat episode scan (same shape the Emby children call already sends as `Season,Episode`), and `GenItems` first collects the series whose `genres` intersect the request, then keeps **every** episode of a matched show — whole series in, never episode-by-episode cherry-picking — OR-ing with the existing `match`/`showIds` roads.
- Test surface: new `MediaServerSourceTest` section "a genre enumerates whole shows, not episodes" (fake show rows carry the genres; the Comedy show's two episodes are all in, the Documentary show stays out, and the call log pins `load_items(tv1,Series,Episode)`) plus new `VirtualChannelsBackendTest` "genre recipe wants films only on movie channels" (block AND entry × tv/movies kinds, 4 wants asserts). Full vchan suite **1458/1458 green** in the lab.

### Fixed — Virtual channels: preset channels read their recipe kind as Genre (whole-library presets, self-destructing re-seeds, resolver false matches)
- **"Horror aired nothing, ABC aired everything, collections were luck" — one trap**: presets stored their recipe kind as the JSON *string* `"1"`, and `QJsonValue::toInt()` on a JSON **string returns 0**. Every Network (kind 1) and Collection (kind 2) preset therefore re-typed into **Genre** on the next reload: the TheTVDB identity filter never ran (journal proved it — 21,615 tried, **0 refused**, zero searches paid), a bogus `genres` block rode into the source blocks, and the ABC preset harvested the **entire library** into 259 picks. Genre channels only survived by coincidence of the same 0. Fix: `create_preset_channel` records kind as a **number**, and every read goes through the new `presetKindOf()`, which still accepts the legacy string generation so channels already on disk re-seed correctly before they are rewritten.
- **Re-seeding a preset channel zeroed it** (Horror: 61 picks → re-seed → 0). The seed merge's un-tick inference scanned the recipe's *own prior picks* — which the merge had just removed to replace them — and concluded the viewer had un-ticked every one of them, pushing the whole pick set into `preset.off`. The un-tick memory the UI already maintains authoritatively in `preset.off` (toggle remembers, re-tick forgets) is honoured alone; the inferential pass is gone.
- **The resolver adopted unrelated first hits**: `bestMatchIndex` fell through to `result[0]` when nothing matched, so every show missing from TheTVDB inherited a stranger's network identity — with Private Snafu resolving to ABC's network id, *nothing* ever refused and the whole library passed. Scores that match nothing are now rejected outright (`-1`), which is also what makes the identity filter meaningful.
- **Added — TheTVDB network-id cache persists across boots**: the resolver gained `loadCache()`/`saveCache()` over `<dataRoot>/tvdb-network-cache.json`; seeds load it at resolver construction and save it at harvest end. Network seeds no longer re-pay one TheTVDB lookup per title per boot — the dial's lookups are stable, and building channels gets faster with every session (owner's "cache of relevant results").
- Test surface: preset-smoke's kind assertion moved to `presetKindOf` (the number contract), new cases pin **string-kind → toInt == 0 (the trap itself)**, `presetKindOf` on both generations, `bestMatchIndex` refusing unrelated hits, and the cache round-tripping through a file with zero network. Full vchan suite **1430/1430 green** in the lab.
- Also this push: `scripts/install.sh` makes the Pi's `ExecStopPost` handler restart-safe — the unit now passes `%1` (systemd's stop command: `stop|restart|kill|empty` on app exit) and `240mp-stop` keeps the Pi powered on `restart`/`kill` instead of powering off mid-restart, so dev/agent service restarts no longer reboot the box. Explicit `stop` and app exit codes keep the old semantics.

### Fixed — Module-settings channel entry no longer greys out on bare view hops (live Pi, 2026-11-11)
- Editing a channel reached through **Manage Channels** greyed out the moment a channel was chosen. Pi journal said it outright: `navigate to=ChannelSources.qml` → `file:///opt/240mp/share/240mp/ChannelSources.qml: No such file`. Cause: Settings' module submenus open the module view **app-level** (Main.qml's Loader loads `Manage.qml` directly), so Manage.qml's bare `navigateTo("ChannelSources.qml")`/`("PresetRecipe.qml")`/`("TextEntry.qml")` hops land in Main.qml's handler, where Qt's Loader resolves bare tokens against the **share root**, not the module's views dir. In-module hops were always fine — Root.qml's bridge resolves them relative to `views/`; that convention simply wasn't honoured when the hop skipped the bridge.
- Fix at the app-level choke point: Main.qml's `navigateTo` resolves bare `*.qml` tokens against the **last-loaded view's URL** via a new `AppCore::current_view_source()` getter (QString — dodging Qt.resolvedUrl's QUrl→QObject* marshalling trap and Qt6's dropped `left()`); tokens already carrying a directory ("views/", "modules/") keep resolving share-root-relative, so every existing hop is untouched.
- Lab-proven on the exact repro walk (ModuleList → Settings → CHANNELS → Manage Channels → add-preset row): the hop now emits the resolved file URL, `PresetRecipe.qml` opens clean and the kind ask starts — zero QML errors. Bundled on the same head: Guide's crawl Enter fix (highlight index modded **before** the row guard; during the crawl the model is `rows.length * copies`, and a residue index past the rows could outlive the crawl and swallow Enter on the selected row).
- pi-rc evidence: PR head `772a53b` → Pi binary md5 matches its tarball, service active. Awaiting owner CRT word on the Manage-ABC edit flow before done.

### Added — Virtual channels: preset channels (stand up a whole channel from a category)
- A viewer can now build an entire channel from a category instead of one folder or series: `Manage.qml` gains an "Add Preset Channel" row on the dial, leading a two-step picker (`PresetRecipe` → `PresetSubject`). Pick a recipe (by genre, by network, or by collection), then the specific subject; selecting one calls `VirtualChannelsBackend::create_preset_channel()` and drops onto `ChannelSources`, exactly where a manually-added channel lands. The channel's identity is derived from the subject (a "by network" preset for AMC becomes the "AMC" channel); the recipe is recorded under the channel's `preset` key so a future generator can rebuild it.
- **A preset stands its content up on the channel at creation** — the whole point, so new channel creation is a pick instead of a hand-build. The flow asks what the channel airs (TV shows or movies) before the category, so the recipe looks for content among the right kind from the start. Selecting a subject calls `create_preset_channel()` and then `seed_preset_channel()`: the recipe runs through the generation machinery against **every** connected source (Plex/Jellyfin/Emby; a network recipe with no server-side filter scans broad and keeps only what TheTVDB resolves to that network), the picks are deduped by title and written into the channel's `programmes` pools as concrete series/film entries marked with the recipe. The viewer lands on `ChannelSources` with **everything already ticked** — the Series/Films browser shows the library with the picks checked, and removing one show is one press. The recipe's block fields are retired once the picks are written, so the recipe never decides what airs on its own; `preset.off` remembers what the viewer took out, and the new **Seed From Recipe** row (`N OFF`) re-runs the recipe and adds what the library has gained since without putting those back. A recipe the library cannot fill creates nothing — the subject picker answers with the reason instead of parking an empty channel on the dial.
- **Movie presets ask by studio, and every server carries it**: on a movie channel the "by network" recipe slot reads as *studio* — the subject list is the connected library itself (browse the films, tally the studio strings), and the pick rides into Plex, Jellyfin and Emby blocks as a `studios` server-side filter (Plex's `Studio` attribute, Jellyfin's `Studios` array, Emby's `Studios` with a bare-`Studio` fallback for older servers). Only TV networks keep the TheTVDB resolver; a studio channel needs no apikey. The picker screens follow the mode: the recipe row relabels per kind ask, the subject picker offers only studios, `ChannelSources` grows a Studios row only on movie channels that carry the filter, and a local-files channel refuses the studio picker outright.
- Backend: new `Catalog` (`src/modules/virtual_channels/Catalog.{h,cpp}`) is the TheTVDB subject-matter source (`parseGenres` / `parseNetworks`) and emits the `[{id,label}]` shape the QML picker expects. `VirtualChannelsBackend` gains a lazily-built TheTVDB resolver (constructed once a stored apikey is known), `load_catalog()` for genre/network (TheTVDB) and collection (rides the connected media-server browse), and `catalogReady` / `catalogFailed` delivery. `TVDBNetworkResolver::listGenres()` / `listNetworks()` now delegate to `Catalog`.
- Test surface: `CatalogTest` + a preset-channel case in `VirtualChannelsBackendTest` (pure logic). A standalone offscreen smoke test loads both views with stubbed backends and asserts 3 recipe rows, the 2-row kind ask opening the screen, a 2-row catalog populate, `create_preset_channel(0,"389","AMC","tv")`, that activating also calls `seed_preset_channel()` on the channel it just made, and that the seeded channel navigates to `ChannelSources` — proves the wiring and data flow, not pixel layout. Full vchan suite green; CI arm64 + macOS green.

### Fixed — Virtual channels: network presets no longer return an empty catalog (issue #64)
- `Catalog::parseNetworks` matched the v4 company type against the literal `"network"`, but the API returns it capitalised (`"Network"`), so **every** network was filtered out and a "By Network" preset listed nothing. The compare is now case-insensitive, and `CatalogTest` covers the real-world `"Network"` casing so it cannot silently regress. Genre and network catalogs now load live with the stored TheTVDB v4 key (35 genres / 500 companies verified against the live API); the key lives in `<dataRoot>/thetvdb_apikey` (0600).
- **A seeded genre channel swept the whole library in**: the seed pass writes its recipe as a `genres` filter in the source block, but `readPools` never read that field back, so the harvest job asked for nothing and aired everything the section held. Block reads now include `genres` (and the new `studios`), `bookingWants` gates studio strings case-blind, and `channel_source_config` reports the block's filter among the channel's genres — the builder screen used to answer NONE under a channel that had named the genre when it was built.

### Fixed — Weather location reads no longer fail silently outside the module scope (issue #53)
- `views/ModuleSettings.qml:141` and `views/LocationPicker.qml:31-32` called `weatherBackend.primaryLocationLine()` / `.looksLikeCoordinates()` directly, but `weatherBackend` is a **module-scope** context property that is null outside the weather module's QQml tree — so the app-level views read a null receiver and got empty strings, silently. Root cause was deeper: the proxy in `AppCore` used `Q_RETURN_ARG(QVariant, ret)` against `WeatherBackend`'s `Q_INVOKABLE`s, and `QMetaMethod::invokeMethod` logs a "return type mismatch" and returns empty — `QVariant` never matched the `QString`/`bool` signatures.
- Fix: `AppCore` gains `weatherPrimaryLocationLine()` / `weatherLooksLikeCoordinates()` (`Q_INVOKABLE`, cross-scope via `QMetaObject::invokeMethod` on the `com.240mp.weather` backend in `m_backends`), and the two views call those instead. Return types now match exactly (`Q_RETURN_ARG(QString, ret)` / `Q_RETURN_ARG(bool, ret)`).
- Test surface: new `src/tests/AppCoreWeatherProxyTest.cpp` (6 checks) exercises the proxy directly under `QT_QPA_PLATFORM=offscreen` — no QML engine, no network. Full suite 12/12, app builds 0 warnings under `-Wall -Wextra`.

### Fixed — TVDB network resolver no longer double-frees its network manager (issue #32 production-readiness review)
- `TVDBNetworkResolver` allocated its `QNetworkAccessManager` with `new QNetworkAccessManager(parent)` yet deleted it in the destructor — a non-null parent (the future backend owner) would **double-free** on destroy. The resolver isn't a `QObject`; the parent was only ever for QNAM, so it was dead weight that turned into a crash once the resolver is wired into `VirtualChannelsBackend`.
- Fix: drop the `parent` constructor param, make `m_nam` a value member (RAII, no manual delete, no double-free possible), reorder members to clear `-Wreorder`. Added a construct/destroy lifecycle test that would have caught the double-free.
- Verified ASan-clean (0 memory errors); the `invalid nullptr` connect warnings are pre-existing Qt-internal on a headless QNAM — a bare `QNetworkAccessManager` reproduces them, so they're not from this code. Committed `a948dac`.

### Added — App-wide system-volume mixer abstraction (issue #34 spike)
- New `src/audio/` `VolumeMixer` interface + factory so every module can route volume through one source of truth (the #34 goal of "consistent across the board"). Factory picks at runtime: **PipeWire** (`wpctl`) > **ALSA** (`amixer`) > **no-op stub**, so the app never crashes on a box with no audio control. No real hardware needed to test — the factory falls back to no-op on WSL/CI.
- `VolumeMixer`: 0..100 level + separate mute (mute survives level changes, matching a physical button). `PipeWireVolumeMixer`: `wpctl set-volume`/`set-mute`, float-fraction spec. `ALSAVolumeMixer`: `amixer sset`/`get`, parses the echoed state. `NoopVolumeMixer`: succeeds but changes nothing. `VolumeMixerFactory`: `findExecutable`-based detection.
- CMake: audio TUs added to the app; new `volume-mixer-tests` target (7 checks, all pass). Full suite 13/13, app builds 0 warnings under `-Wall -Wextra`. **Not yet wired into any module** — next step is the central `VolumeManager` that owns one of these and feeds the OSD + player + weather mpv.

### Changed — Channel settings wall consolidated (two sections; the 18-row wall gone)
- `modules/virtual_channels/manifest.json` settings now use the core `Settings.qml` section-row contract: **DIAL** (media folder, weather channel, schedule-ahead) and **GUIDE** (single idle row, preview, weather music) — 18 selectable rows → 8. Removed rows: volume + `normalize_volume` (media keys already own volume), `audio.language`/`subtitles.*` (per-title player choice), `tuning_screen`/`banner`/`logo` looks (channel identity is auto-branded from its subject — presets take that off the user), and `rebuild_all` (schedules already top up at boot and 04:00 — that row *was* the visible bug in the original report).
- Merged: `guide.autoscroll` + `guide.resume_seconds` → one `guide.idling` row (`Keeps Crawling | Holds · 30s/1m/5m/Never`); `Guide.qml` prefers the merged row (manifest schema default answers first) and keeps the legacy pair as a safety net. The resolved mode is logged to the debug state channel: lab offscreen drive saved `"guide": {"idling": "Holds · 1m"}` through the real UI and re-entry emitted `guide-idle auto=on resume=60s src=setting`; boot read of the schema default emitted `auto=on resume=5s`.
- `views/ModuleSettings.qml` gains the section-row rendering (mirrors core Settings, which already skipped sections — that asymmetry was the user-visible half of the original complaint) and `snapSelectable(idx, dir)`: the row stream in the drive read `Enabled → Manage → Media Folder → … → While Idle` with section indexes never landed on or stopped at, both directions.
- Screenshot artifact of the settings screen: objective pixel probe shows exactly 10 text-row bands in expected order with the selected row's full-height accent distinct; pixel-level visual truth waits for Ian's CRT gate. Naming note for the merge: fork settings keys stay `auto_fit`/`autoFitEnabled()` — upstream's `auto_crop` rename never comes into this fork.

### Added — Virtual channels: the library becomes a TV schedule (upstream's module merged to dev)
- `modules/virtual_channels` (schedule parsing that drops rather than crashes, pool generation across interstitials/bumpers/ident cards/rotations, guide grid, channel dial) merged onto dev via PR #58; enabled in the default `settings.json` (`modules/virtual-channels`, `virtual-channels.settings`).
- Integration constraints upstream never had to know: our number-collision rule (a virtual channel claiming a built-in's number **re-homes the built-in**, never the viewer's setting — unit case: "the weather channel shared number 1; moved to 2") and the `osd-channel` gate (switching a channel off frees its number **exactly** = its configured position, unit case: "off numbers the dial has taken").
- Test surface: `t2test` target `vchan-tests` — 1311 checks, 0 failures at dev tip (schedule tolerance, pool plans, guide slivers, tuner joins, subtitle/audio track selection, holds across surfing, weather/guide re-homing).
- `InputManager` accepts `channel_up`/`channel_down` as debug-key tokens (synthesized to `Key_PageUp/Key_PageDown`); lab drive through the offscreen key channel proves the whole zap artifact: guide rows built from a fixture schedule (`rows=3`), `tuneFromGuide(±1)` reaches the Guide key handler, the Player opens and the channel's ident banner lands (log across the zap sequence: `channel banner queued: 0` then `1`). Wave evidence trail: PR #62 (label `pi-test` = RC deploy of the PR head on the Pi; official beta rev stays put).
- Channel previews degrade without QtMultimedia: "no video preview available; showing the still instead" is a handled path, not an error wall (the qml module warning itself is a lab-desktop build gap; the Pi's module set gets checked in the PR's RC evidence, and the still fallback works either way).
- The remote's Channel Up/Down keys were delivered-but-dead: `InputManager` synthesized `Qt.Key_ChannelUp`, a key undefined in QML, so the event could never match any view handler. Synthesis now emits `PageUp/PageDown` — the alias every channel view already accepts. Lab offscreen full-drive proof (green pre-push): guide↔channel zaps in **both** directions (`nav-internal to=Player stack=2` at both ends), ChannelUp from inside a channel returning to the guide, dial Up/Down walking the wrap `0→2→1→0` with `select idx=` at every step, and a full unwind at every stack level with zero skew. Same drive instrumented the CHANNELS module root (the only module root without `nav-internal` logging — RetroArch precedent) and moved the Guide dial to the core `RowTrackingListView` primitive. Wave evidence trail: PR #63 (`pi-test` label = RC deploy of the PR head; beta rev stays put).

### Fixed — All palette role pairs now clear the WCAG AA contrast floor (was: 12 palettes failed)
- A mechanical walk over every view (fill-bearing items with Text descendants; conditional branches paired by identical conditions; translucent fills and dead code skipped) yields exactly five rendered text/fill role pairs — `tertiary/primary/secondary` on `surface`, `surface` on `accent`, `accent` on `surface`. Audited across all 17 palettes: **12 failed the WCAG AA floor 4.5** (worst: Solarized Dark accent text 2.68, Monokai red header text 2.93, SMPTE indigo labels 2.96, late-night grays 3.42–3.86, T-120 secondary 3.45). Video 1 (4.33), Dracula (4.10) and the two Game-Boy palettes round it out.
- Fix: each failing role value moves minimally **along its own published hue** (constant hue and saturation, lightness shifted toward the AA floor; saturation only reduced when no lightness alone reaches it) — published spec hues stay recognizable, values stop being unreadable. 18 role values changed across 12 palettes; every changed line carries an inline `// was #XXXXXX …` derivation note. `Amber` and the five added palettes already passed untouched.
- Contract is now enforced where the table is parsed: `src/util/tests/ColorSchemesTest.cpp` gains a `renderedRolePairsClearAaFloor` test (WCAG luminance math inline, pair list hardcoded from the walk) — it fails red on the old table and green on the fixed one, both runs verified on the lab machine before any push. Audit tool (same table, pair counts per file): `/home/ian/src/vibes/theme_audit.py`.


### Fixed — Empty playlists settle with a visible empty-state message (spinner was permanent)
- `modules/retroarch/views/Games.qml` gated its LOADING spinner on `!Component.onCompletedCalled` — a property that **was never defined anywhere in the app** (qmllint: `Member "onCompletedCalled" not found on QQmlComponentAttached`; repo grep zero hits). Reading an undefined property is silently falsy forever, so any playlist with 0 games stayed LOADING indefinitely and the companion "No games in this playlist" Column (gated on `!spinner.visible`) was unreachable. Same pattern shadowed in `_base_games`/`_base_playlists`.
- Fix: real root-level `property bool listLoaded` set in the view's `Component.onCompleted` after the model assignment; spinner and empty-state text now branch on it. Lab-proven by screenshot band-analysis on an empty playlist (purple spinner box → gray text line, pixel counts objective), pure-QML change (no new unit surface); qmllint confirms `listLoaded` resolves and no `onCompletedCalled` member warning remains (only pre-existing unresolved-import noise).

### Added — Pre-merge PR verification on the Pi (`pi-test` label + `pi-rc.yml`)
- A dev→beta PR labeled `pi-test` gets its PR-head build RC-deployed to the Pi (rebuild on the arm64 runner, `240mp-ci-deploy` dance, service+md5 verify) with an evidence comment (marker, md5, boot state-log) posted on the PR. Beta stays the Pi's daily driver otherwise: unlabeled PRs never touch it. Closing such a PR without merging auto-restores the previously deployed beta build (one extra reboot). Ian's approving review + merge stay manual; the beta-push deploy remains the only official deploy.

### Fixed — Module roots survive shutdown teardown (null-appCore re-eval, was a silent TypeError)
- At process teardown the QML tree re-evaluates view property bindings while context
  properties are already nulled: `modules/{plex,local_files,retroarch}/views/Root.qml:14`
  ran `appCore.get_module_info(...)` against a null `appCore` during shutdown — noisy
  console TypeError on every quit, invisible live but polluting runlogs/diagnostics.
  Now the established guard parity all other module roots already use:
  `appCore ? appCore.get_module_info(moduleId) : ({})` (nfc/weather/scripts/emby/
  ambient/youtube roots already carry it; these three predate the pattern).

### Fixed — YouTube warm opens self-heal when a clock-fresh cached URL still 403s (warm-403 cure)
- A cached googlevideo URL can pass the `expire=` clock check yet be rejected 403 on
  replay: the signature binds to the minting session **and IP**, so `urlFresh()` is not a
  playback oracle. A warm launch that dies before the first position event now evicts that
  cache entry (`invalidateCachedUrl`, exact `videoId|format` key) and re-enters the cold
  resolve once, minting a URL bound to THIS session. One reactive retry per open
  (`lastOpenWasWarm`/`retryWarm403`, reset by user retry/resume/playlist advance) so a
  genuinely dead yt-dlp still ends at its message instead of looping. New pure-logic test
  target `yt-url-invalidate` pins eviction semantics: exact-key removal, format isolation,
  idempotent miss, no resurrection through prune.

### Fixed — SponsorBlock settings subview opens clean (QML contract, was a 4-error cascade)
- `SponsorSettings.qml` lacked the mandatory file-scoped `property var navParams: ({})`: every
  open of the subview fired `Main.qml` "Cannot assign to non-existent property", a
  ReferenceError poisoning `moduleId`, and a null-receiver `AppBar{}` icon binding — four
  QML error lines per open, silently. Now mirrors the sibling-view pattern (all 15 other
  views declare it; SignIn's `_selfInfo` file-scope snapshot feeds the AppBar icon, since
  context props like `appCore` aren't visible inside imported-component bindings).
  Lab drive of the full Settings→YouTube→SponsorBlock walk: 4 errors → 0, clean stack return.

### Changed — YouTube credential ladder sheds upstream's file-fed last stand (legacy axe burst)
- `CredentialMode` loses the legacy mode: `FileFallback` → `SignedOut`. With neither
  a pasted cookie nor a live device-flow chain, the account surface now shows **nothing**
  and the views point at pairing — upstream's file-fed display is gone from the ladder.
  `youtube_subscriptions.txt` stays as a **pre-pairing roster seed** (bootstrap channels
  before pairing), and legacy pos-only resume entries stay tolerated (save-file compat,
  a different axis). The unit test `ladderLastStand` became `ladderSignedOutNoLastStand`.
- `Items.qml` cold-start text leads with pairing ("PAIR YOUTUBE IN SIGN-IN SETTINGS")
  and offers the two txt files as the fallback bootstrap, not the other way round.
- Roster dead-end retry comment made honest: it retries on the SEEDED roster, not a
  credential-mode file path.

### Changed — Deploy-to-Pi gate: only a genuine push into beta deploys
- The deploy job gains `if: github.event.workflow_run.event == 'push'`. Trigger-level
  `branches: [beta]` alone also matches PR-triggered CI completions whose payload
  `head_branch` is the **PR head** (observed with PR #51 headed on beta) — those could
  deploy a pre-merge sha. A merge into beta is a push event and still deploys ✓.

### Fixed — YouTube cold-play waits end at once when the resolve already failed (#28 polish wave)
- Every case where the background resolve **completes but stores nothing** — dead network,
  bot-gated 403, ytdl spawn failure, no yt-dlp binary, entry already expired at store time —
  left the Player waiting out its full 15 s cap, and only then did mpv's `ytdl_hook` pay a
  second full resolve: worst cold path ~30 s. `playbackResolveDone(videoId)` now ends that
  wait at once, and `Player.qml` routes all three cold failures (timeout / empty resolve /
  stale pair) through one `takeYtdlFallback(reason)` path instead of one inlined copy.
- The URL-cache **store gate now runs the same pure `urlFresh` freshness check** (same
  120 s margin) as the read side: short-window entries stop being stored-but-never-served
  dead weight, and the ready-signal dead-end state can no longer happen.
- A transient SponsorBlock failure now **emits `sponsorReady` on the failure path** —
  the next playback retries instead of the Player waiting out the full 6 s ceiling.
  Failures stay uncached.
- Hygiene in the same touch: three `%d`-vs-`qsizetype` format sites in `YouTubeBackend.cpp`
  made exact, so the TU compiles warning-free on the CI lanes.
Gates: unit suites green (yt-url-cache 7/7, yt-sponsor-edl 10/10), TU compiles clean; the
runtime behavior lands with the Pi test.

### Changed — macOS CI cache lane warm + per-TU APP_VERSION define
- macOS CI re-ran from zero on every identity re-push and re-queued PR build — the chronic
  capacity bottleneck. Two-part fix, one deploy:
- `CMakeLists.txt`: `-DAPP_VERSION` moved from a target-wide define (which put it on every
  TU's compile line, so ccache hits were zero by construction) to a per-SOURCE define on
  the only two TUs that use it (`main.cpp`, `WeatherBackend.cpp`). Verified on a fresh
  Ninja configure: exactly those two object rules carry the flag, both compile green;
  test targets touch neither file.
- `ci-macos.yml`: `actions/cache` restore@v6 + save@v6 split so the store step sits
  directly after Build (a test failure still banks the compile cache); `CCACHE_DIR` under
  `RUNNER_TEMP`; key = event+ref with prefix fallback. ccache hashes inputs, so
  cross-commit reuse is always safe.

### Fixed — YouTube review pair: sign-in cookie row and OSC info garbage
- The SignIn subview drew a `COOKIE FALLBACK: OFF - OPTIONAL SSH PASTE OF cookies.txt`
  line under "SIGNED IN" on every signed-in visit — setup instructions no viewer acts
  on, crowding the status headline. The row is gone; the credential-ladder tier stays
  journal-only (`auth-mode=cookie|oauth` in `check_signin()` telemetry).
- The mpv OSC track-info row printed the external `--audio-file=` sidecar's mpv-derived
  title, which for YouTube's split googlevideo pair is the raw **URL query tail**
  (`webm&rqh=1&clen=...&sig=...`), wrapping the top-left corner into a wall of debug
  text after every seek and SponsorBlock cut (Ian report, live-probed on the Pi).
  `mpv-osc.lua` now drops URL-junk titles (`clean_title`), keeps real track names
  (Plex/Jellyfin commentary, `subinfo` friendly names) intact, and caps each info
  row to one line (`fit_oneline`). YouTube's row reads `AUDIO: ENG OPUS 2CH 48000 HZ`.
  Logic gate: 6/6 in a lua harness on the Pi against the patched file (live stream
  values mocked); nav-menu timing/UX untouched.

### Fixed — YouTube signed-in layer dying one hour after pairing (Playlists row vanished)
- YouTube access tokens live ~1 h. `check_signin()` reported only liveness, so an
  hour after pairing the account layer — the Playlists row, account feed and
  Channels — hid itself, and the refresh plumbing lived *behind* those gates:
  the app could never revive and silently fell back to file mode forever
  (2026-11-04 "where did the playlists view go?").
- A credential with a refresh token is now *usable*: expiry, slack, and revive
  semantics live in one predicate (`util/YtAuthGate.h`, unit-tested); the first
  auth touch refreshes before expiry, one swap at a time, and deferred account
  fetches (feed lists, playlist videos, FEchannels Channels) replay when the
  swap lands. A declined refresh falls back to file views with a journal
  nudge to re-pair on the CRT.

### Fixed — YouTube player stuck at "Loading…" forever
- The cold-path playback hold waited on `sponsorReady` with no expiry: when the SponsorBlock fetch fails transiently (journal: `sponsor transient err=5`) the backend gives up quietly and the signal never fires — the view stayed in "Loading…" forever, mpv never spawned, and Back/Escape went into dead IPC. Reproduced deterministically in the local lab (offscreen app + key channel + `/etc/hosts` bind poisoning only `sponsor.ajay.app` → same `err` → 20 s of silence, pre-fix).
- `Player.qml`: a 6 s `sponsorWaitTimer` restarted at the hold site now expires the hold and launches **without** segment EDL (late segments still apply via the property-filter path), and Back/Escape on a view whose playback never started returns natively via `goBack()` instead of shouting into a disconnected IPC socket.
- Verified with a local A/B race (identical cold path, poisoned domain): pre-fix = deadlock; post-fix = `wait-expired` at +6.0 s and mpv launches.

### Fixed — mpv playback overlay on the CRT
- The mpv 0.40.0 bump renamed `--stats` → `--load-stats-overlay` and **flipped its default to on**: every playback painted the stats/debug text (frame timings, A-V drift, cache state) on screen and spammed the journal with a line per second. The app now passes `--load-stats-overlay=no` explicitly (app-owned mpv v0.40.0 everywhere, so the flag is universal; lab harnesses that want the stats signal pass it themselves).

### Fixed — cold re-resolve per pick on live/rebroadcast rows
- Picking a wits.tv-style rebroadcast paid a fresh ~4 s yt-dlp resolve every time and cached nothing: the `<=480 avc1` format spec falls through to `best` for live sources, and yt-dlp hands back an HLS **manifest URL whose expiry lives in slash-delimited path segments** (`/expire/<epoch>/`) — the freshness parser only spoke query dialect (`?expire=`/`&expire=`), so the warm store silently refused (`stored=0`). Parser now accepts both dialects at the same strictness (3 new unit tests); first watch still resolves once, re-watches inside the expiry window open instantly.

### Changed — mpv OSC button rename (beta)
- On-screen playback control button relabeled `CROP` → `FILL` (both OSC scripts): with the geometry-driven auto-fill rule it toggles panscan fill on/off, and the old label lied about what it did. Internal `hide-crop` script opt and code comments aligned to the new name.

### Fixed — YouTube Subscriptions freshness (feature/youtube-finish)
- Signed-in Subscriptions stopped riding YouTube's TV-client `FEsubscriptions` browse: that feed is a **server-precomputed cache** — live probe 2026-11-04 showed 28/30 sampled channels' newest uploads entirely absent from its 43 unordered tiles (oldest-of-newest tile was 13 h late) while the official mobile app showed them. Signed-in feed now = one fresh `FEchannels` roster hop + wave-capped (32 concurrent) per-channel RSS union sorted by publish date — same freshness source the official app uses, TTL-cached (15 min), signed-out file path untouched. State-log rows label now `union` when signed in.

### Fixed — YouTube playback judder on the CRT rig (2026-11-03, operational)
- Long uploads ("YOU CAN SEE EVERYTHING", 76 min) stuttered while audio and position tracked normally: `video-sync=display-resample` in the Pi's `~/.config/mpv/mpv.conf` fought the vc4/CRT refresh (blend cost + unreliable vsync timing — the same subsystem that once froze startup). Switched to `video-sync=audio` (framedrop default): telemetry showed RTF 1.02 and demux cache 150–160 s both before and after, so the judder was presentation-level, not network or decode starvation. Runtime config change; no rebuild, re-read per mpv spawn.

### Fixed — YouTube Channels naming (feature/youtube-finish)
- Channels view fell back to raw UC IDs whenever per-channel RSS name fetches failed (311 round-trips on open). Signed-in path now uses one InnerTube `FEchannels` browse: names + IDs for all subscribed channels in a single request (live probe: 313 tiles); RSS stays the legacy signed-out fallback. `ytui::extractChannelSyncRows` handles TV tileRenderer + web channelRenderer + base64-protobuf params id fallback (2 new unit tests).

### Added — YouTube SponsorBlock + latency telemetry (feature/youtube-finish)
- SponsorBlock per-category controls (`SponsorBlock: <Category>` rows, Off/Mark/Skip): sponsor and self-promo default to Skip, the rest to Off. Skips are cut out of an mpv EDL virtual timeline (both media halves identically, audio stays in sync across jumps); Mark keeps playback whole with titled chapter boundaries in the OSC. Segments fetched k-anonymously (4-hex SHA-256 prefix, exact videoID filter, Red doctrine), cached per video, pre-warmed for whole list pages. 10/10 yt-sponsor-edl tests + live mpv IPC probes prove the EDL arithmetic.
- Journal telemetry for playback + browse (`yt-play path=warm|cold`, `yt-resolve`, `yt-fallback`, `yt-sponsor`, `yt-rows path=account|file rows=N` in the state log; `[Youtube] resolve/warm/sponsor` lines with ms in the service journal) — per-pick latency tables replace anecdote rounds; unit suite registered and green (53 checks across five test binaries).

### Added — YouTube SponsorBlock OSC marks (feature/youtube-finish)
- SponsorBlock segments now paint on the mpv OSD in SponsorBlock's own default colors (green sponsor, yellow self-promo, cyan intro, blue outro/intermission/preview teal, purple filler, violet interaction — hex table ported from ajayyy/SponsorBlock `src/config.ts`, GPL-3.0 prior art): Skip lands a solid category tick at the jump point, Mark paints a dim band over the segment. Ranges travel to the OSC as a per-launch `sb-ranges` script-opts hint (raw source seconds; the script subtracts skip-cut lengths to sit on the cut virtual timeline).
- SponsorBlock settings moved from eight module-settings rows to one "SponsorBlock" submenu row with short per-category labels; sponsor + self-promo skip silently by default, everything else off.
- `[Youtube]` service-journal lines switched to qWarning so the release build actually shows them (qDebug is filtered); explains the missing resolve/sponsor evidence in earlier runs.

### Added — YouTube account playlists (feature/youtube-account-playlists)
- "Playlists" row between Channels and Watch Later is now account-driven when signed in via the TV device flow: the account's Playlists tab (YouTube's own tab order, recent activity first) with drill-down into a playlist's videos through the account browse; file playlists (youtube_playlists.txt) unchanged when signed out, and every account failure falls back to the file path. 18/18 yt-browse-parse tests green.

### Added — RetroArch full functionality (feature/retroarch-full-functionality)
- Live playlist sort (SYSTEM/ALPHA/COMPANY) re-applied on settings change; short-name section headers; playlist game counts.
- thumbnail_display OFF/LEFT/RIGHT zone geometry for boxart placement (game list only); playlist views stay plain.
- Module-exit navigation: exiting the module (or after a game) returns to the main menu, not Settings; game-key suppression while a RetroArch child owns the display.
- Lazy thumbnail resolution (snappy opening of ~4,700-item playlists); archive rom paths (`zip#entry`) resolve + launch (verified on CRT with '10-Yard Fight').
- Debug telemetry behind a TELEMETRY compile gate: /tmp/240mp_state.log (loads/navigate/back/row-select/key-suppression lines), screenshot view-tagging, debug key channel; RowTrackingListView core primitive. 23/23 unit tests green.

### Added — Tooling & pipeline
- Offscreen validation fast path (QT_QPA_PLATFORM=offscreen + synthetic key channel; pi-nav.sh `local`, pi-keys.py).
- CI direct-deploy chain to the Pi (ghdeploy key-only + reboot dance), paths-ignore so markdown-only pushes queue no builds; separate ci-macos.yml; convergence watcher.
- Comms pipeline: fork-issue channel with the maintainer (kickoff #19) + distinct Hermes commit identity (env-based; documented in .hermes.md and skill references).

### Changed
- .hermes.md single-sourced on dev, blob-mirrored to every branch (sync-hermes.yml + proven plumbing fallback).

