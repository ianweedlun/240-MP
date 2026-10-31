#pragma once
#include <QString>
#include <QVariantMap>

// The basic mpv knobs a user sets through 240mp's own Settings. Single source
// of truth is ~/.config/mpv/mpv.conf itself: the menu READS the file (last
// assignment wins, "#"-prefixed lines are comments, one-level
// `include <file>` directives are followed) and WRITES it in place
// (replace the live line, or append when the key is new). The old
// "Mpv Conf" pseudo-value and the config.json app-key override layer are
// gone; legacy app-section keys migrate into mpv.conf once at startup
// (AppCore drives that).
//
// Supported knobs (settings key -> mpv option, names kept as mpv's own
// with dots/underscores as before):
//   Audio Normalize  -> replaygain=no|track|album
//   Sound Boost      -> audio-gain=<-20..20 dB; menu steps {-4,0,2,4,6,8}>
//   Deinterlace      -> deinterlace=no|auto|yes
//   Audio Channels   -> audio-channels=max|stereo|1
//
// Values present in the file but outside the menu's set display honestly
// as "Custom (<raw>)" — the UI never claims a value the file doesn't say.
namespace mpvopts {

// True when key is one of the four managed settings keys.
bool isManagedKey(const QString &key);

// UI tokens for every managed key, as effectiveTokens[key]:
// from mpvConfPath if set (mapping mpv values back to menu tokens), else
// mpv's built-in default ("Off"/"0" when that IS a menu value; otherwise
// "Custom (auto)" for defaults the menu deliberately doesn't expose, e.g.
// replaygain=auto, audio-channels=auto).
QVariantMap effectiveTokens(const QString &mpvConfPath);

// Write one menu token for key into the conf: replaces the last active
// occurrence (any of the `option` or its underscore spelling), else
// appends. Everything else in the file is byte-preserved. Returns false —
// file untouched — when the token isn't on that key's whitelist.
bool setUiValue(const QString &mpvConfPath, const QString &key,
                const QString &uiToken);

// Legacy-token (pre-migration config.json) value -> mpv value ("" = write
// nothing, e.g. the old "Mpv Conf" token). AppCore removes the key from
// config.json after migrating.
QString mpvValueForLegacyToken(const QString &key, const QString &legacyToken);

} // namespace mpvopts
