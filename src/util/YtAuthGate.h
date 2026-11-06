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

#include <QtGlobal>

namespace ytauth {

inline constexpr qint64 kAuthSlackS = 120; // refresh this far before nominal expiry

// Credential ladder (beta contract, user-locked 2026-11-04; axe 2026-11-06):
// a pasted browser cookie (~1y life, no refresh plumbing) is the most stable
// carrier when the user has provided one, so cookie wins when present; the
// device-flow OAuth chain is the recovery path. With NEITHER, the account
// surface shows nothing and the views point at pairing — the legacy subs-text
// file is a pre-pairing roster seed only and is deliberately NOT a credential
// mode anymore (upstream's file-fed display path is gone from the ladder).
// Pure decision function — unit-tested; the backend just feeds the two bools.
enum class CredentialMode { Cookie, OAuth, SignedOut };

constexpr inline CredentialMode credentialMode(bool cookiePresent, bool oauthUsable)
{
    if (cookiePresent)
        return CredentialMode::Cookie;
    if (oauthUsable)
        return CredentialMode::OAuth;
    return CredentialMode::SignedOut;
}

// expiresEpoch 0 (or negative) = unknown TTL — treat as never expired, the
// legacy "no expiry recorded" state from paired-auth docs.
inline constexpr bool accessExpired(qint64 expiresEpoch, qint64 nowSecs)
{
    return expiresEpoch > 0 && nowSecs + kAuthSlackS >= expiresEpoch;
}

} // namespace ytauth
