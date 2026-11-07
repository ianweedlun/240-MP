# youtube warm-403 — decision record (2026-11-07 night)

## Status at this boundary
- REPRODUCED + ROOT CAUSE FOUND. Fix IMPLEMENTED next morning on branch
  feature/youtube-warm403-selfheal (forked from dev): invalidateCachedUrl +
  lastOpenWasWarm/retryWarm403 reactive cure in Player.qml +
  yt-url-cache-invalidate unit tests.
- Deterministic lab harness built and reusable:
  - fixture RSS: /tmp/ytfixture/feeds/videos.xml served on :8088
  - YouTubeBackend.cpp line ~276 RSS literal pointed at 127.0.0.1:8088 (LAB-ONLY, uncommitted)
  - rsroot: /tmp/240mp_rsroot (real videoIds ckEUIfrg_tU RL_q3t58R94 ewwShy3UFWo from
    youtube_sponsor.json; youtube_url_cache.json tampered: fresh expire, sig=DEADSIGGONE)
  - app run: QT_QPA_PLATFORM=offscreen APP_ROOT=/home/ian/src/240-MP-lab DATA_ROOT=/tmp/240mp_rsroot
    ./build/240mp; keys via /tmp/240mp_key.in; truth via /tmp/240mp_state.log.
  - instrumented (uncommitted QML, lab-only): Root.qml navigateTo 'yt-nav goto',
    Items.qml 'items-select', Subscriptions.qml onErrorOccurred 'yt-diag feed-error'.

## Ground truth established tonight
- YouTube RSS endpoint returned 404 for ALL real channels from this machine all evening
  (curl, exact app URL byte-for-byte) — network-layer flake, NOT an app bug. Feed
  worked once upstream answered (fixture substituted).
- Full flow green in lab: Subscriptions rows (yt-rows rows=3), Player open (goto
  Player.qml depth=2), warm path served real signed URLs, mpv plays (480p), pause
  toggle works, position saving works (youtube_history.json pos=5008), seek works
  (Right -> 00:08 to 00:15, fine-seem semantics), resume default "Ask" = overlay
  (needs vision/Ian CRT to prove).
- warm-403 CONFIRMED: cache entries clock-fresh (urlFresh true) but Google rejected
  replay of the SAME signed URL with 403. expire= wall-clock freshness is NOT a
  playback-validity oracle for googlevideo signed URLs (session/IP-bound too).
  YtUrlFreshTest.cpp unit tests all green for clock logic = they cover the
  right clock rules, but clock rules alone are not sufficient = the gap.

## Fix plan for next session (small, reuses existing machinery)
Player.qml already has the detection hook:
  Connections { target: mpvController ... onPlaybackEnded(reason==="failed") &&
  !playbackStarted } = "PLAYBACK FAILED ... check yt-dlp" path.
Self-healing warm retry:
1. YouTubeBackend: add Q_INVOKABLE void invalidateCachedUrl(videoId, resolution)
   = erase m_urlCache entry (key = cachedUrlKeyFor) + drop in-memory m_resolved.
2. Player.qml warm-open state: remember `lastOpenWasWarm` when preparePlayback()
   chose direct path (preparePlayback already notes path=warm).
3. onPlaybackEnded failed-branch, if lastOpenWasWarm: invalidateCachedUrl(videoId)
   and re-enter the cold path (preparePlayback() again -> resolvePlayback already
   fires on warm opens; playAt waits playbackUrlReady per existing design). Cap:
   retry ONCE per open (don't loop if yt-dlp itself is dead — keep yt-dlp message).
4. Also worth considering (NOT decided): preflight warm URL with a HEAD before
   direct play. Rejected for now: extra GET per play; reactive retry costs network
   only when actually broken and reuses the already-issued parallel ytdl resolve.
5. Unit-testable pure logic: invalidation semantics on seeded cache (existing
   YtUrl* tests cover fresh/expire; add one for invalidate + prune interplay).
Do: implement on a dev-forked feature branch (NOT the lab's uncommitted drift),
re-run the tonight-harness reproduction, then Ian's CRT proof for the user-visible
behavior per the definition-of-done gates.

## Gotcha to fold into 240-mp-development references
- 'Clock-fresh cached googlevideo URLs can 403 on replay (session/IP binding);
  never treat urlFresh() as a playback oracle. Reactive invalidate+re-resolve is
  the safe pattern.'
