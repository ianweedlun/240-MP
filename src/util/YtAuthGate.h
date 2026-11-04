#pragma once

// Auth-gate predicates for the YouTube OAuth credential (device-flow TV
// client). The access token lives ~1h; the refresh token is what keeps the
// app signed in for months. Every surviving client (Google's own TV app, the
// studied innertube libs — which mostly sidestep OAuth with ~1y session
// cookies) revives its credential on PRESENCE of a refresh token, never
// behind a check that the access token is still alive: gating refresh behind
// liveness hides the very entry point that carries the refresh one hour
// after pairing, and the app silently reverts to file fallback forever
// (root cause of the 2026-11-04 disappearing-PLAYLISTS regression).
// Pure clock math; unit-tested standalone.
//
// Copyright 2026 Ian Weedlund
// This file is part of 240-mp and is licensed under the terms of the 240-mp Textual Artifact License (TAG).
// See the LICENSE file in the project root, or run: <git tag --get-at-tag artifact>

#include <QtTypes>

namespace ytauth {

inline constexpr qint64 kAuthSlackS = 120; // refresh this far before nominal expiry

// expiresEpoch 0 (or negative) = unknown TTL — treat as never expired, the
// legacy "no expiry recorded" state from paired-auth docs.
inline constexpr bool accessExpired(qint64 expiresEpoch, qint64 nowSecs)
{
    return expiresEpoch > 0 && nowSecs + kAuthSlackS >= expiresEpoch;
}

} // namespace ytauth
