#pragma once
#include <QString>
#include <QStringList>

// The basic mpv knobs a user can set through 240mp's own Settings, without
// hand-editing ~/.config/mpv/mpv.conf (which stays the fallback for everything
// else, including display-specific CRT tuning).
//
// Settings keys keep mpv's own option names (audio dots as underscores, the
// same convention as the existing video_output_levels row) and live in
// config.json's "app" section. This reads the file at the call site — same
// hot-edit semantics as mpv_video_args: an edited config takes effect on the
// next spawn without any rebuild.
//
// Emitted as trailing CLI options, so by mpv's precedence (command line >
// config file) a 240mp-level setting always wins over the global file. A
// value that is absent, unset, or outside its whitelist is skipped, never
// passed through — the app never argues with mpv about a bad value.
//
// Supported knobs (UI label -> mpv option, validated here):
//   Audio Normalize  -> replaygain=no|track|album
//   Sound Boost      -> audio-gain=<-20..20 dB>
//   Deinterlace      -> deinterlace=no|auto|yes
//   Audio Channels   -> audio-channels=max|stereo|1
namespace mpvopts {
QStringList basicArgs(const QString &dataRoot);
}
