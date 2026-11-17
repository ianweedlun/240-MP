#ifndef VOLUME_KEY_MAP_H
#define VOLUME_KEY_MAP_H

#include <Qt>

#include "audio/VolumeManager.h"

// The volume-key decision, kept out of InputManager so it can be tested
// straight (#34). InputManager's eventFilter sees a QKeyEvent, asks this map
// which volume action it is, and hands the action to the app-wide owner — the
// same three calls a test makes, so a regression here fails a unit test
// instead of needing a keypress on the Pi.
namespace audio {

enum class VolumeKey { None, Up, Down, Mute };

// Qt key -> volume action. Everything that is not a volume key is None, which
// is how the caller knows to fall through to the other media-key handling.
constexpr VolumeKey volumeKeyFor(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_VolumeUp:   return VolumeKey::Up;
    case Qt::Key_VolumeDown: return VolumeKey::Down;
    case Qt::Key_VolumeMute: return VolumeKey::Mute;
    default:                 return VolumeKey::None;
    }
}

// Applies one press. Volume steps move by VolumeManager::kStep (percentage
// points, matching the tick quantisation in the QML bar and the mpv lua bar);
// mute toggles and leaves the level alone.
inline void applyVolumeKey(VolumeManager &vm, VolumeKey key)
{
    switch (key) {
    case VolumeKey::Up:   vm.changeVolume(VolumeManager::kStep); break;
    case VolumeKey::Down: vm.changeVolume(-VolumeManager::kStep); break;
    case VolumeKey::Mute: vm.toggleMute(); break;
    case VolumeKey::None: break;
    }
}

} // namespace audio

#endif // VOLUME_KEY_MAP_H
