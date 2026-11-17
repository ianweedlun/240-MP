#include "VolumeManager.h"

#include "audio/NoopVolumeMixer.h"
#include "audio/VolumeMixerFactory.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

constexpr int kMaxLevel = 100;

int clampLevel(int percent)
{
    return qBound(0, percent, kMaxLevel);
}

} // anonymous namespace

VolumeManager::VolumeManager(const QString &dataRoot,
                             std::unique_ptr<audio::VolumeMixer> mixer)
    : QObject()
    , m_dataRoot(dataRoot)
    , m_mixer(std::move(mixer))
{
    if (!m_mixer)
        m_mixer = audio::makeSystemVolumeMixer();
    if (!m_mixer)
        m_mixer = std::make_unique<audio::NoopVolumeMixer>();
    restore();
}

VolumeManager::~VolumeManager() = default;

void VolumeManager::setLevel(int percent)
{
    const int clamped = clampLevel(percent);
    if (clamped == level())
        return;
    if (!m_mixer->setLevel(clamped))
        return;
    persist();
    emit levelChanged();
    emit volumeChanged(level(), muted());
}

void VolumeManager::changeVolume(int delta)
{
    if (delta == 0)
        return;
    // Raising the volume is also how you turn the mute off (amplifier
    // behaviour). The mixer keeps mute independent, so this layer owns the
    // rule: a mute leaves the level alone, a raise clears the mute.
    if (delta > 0 && muted())
        setMuted(false);
    setLevel(level() + delta);
}

bool VolumeManager::setMuted(bool isMuted)
{
    if (isMuted == muted())
        return true;
    if (!m_mixer->setMuted(isMuted))
        return false;
    persist();
    emit mutedChanged();
    emit volumeChanged(level(), muted());
    return true;
}

bool VolumeManager::toggleMute()
{
    return setMuted(!muted());
}

int VolumeManager::level() const
{
    return m_mixer ? m_mixer->level() : 0;
}

bool VolumeManager::muted() const
{
    return m_mixer ? m_mixer->isMuted() : false;
}

QString VolumeManager::backendName() const
{
    return m_mixer ? m_mixer->backendName() : QStringLiteral("none");
}

bool VolumeManager::persist()
{
    if (m_dataRoot.isEmpty() || !QDir().mkpath(m_dataRoot))
        return false;
    QFile f(QDir(m_dataRoot).filePath(QStringLiteral("volume.json")));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("level"), level());
    o.insert(QStringLiteral("muted"), muted());
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.close();
    return true;
}

void VolumeManager::restore()
{
    if (m_dataRoot.isEmpty())
        return;
    QFile f(QDir(m_dataRoot).filePath(QStringLiteral("volume.json")));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return;
    const QJsonObject o = doc.object();
    // Level first, then mute: a persisted mute must not be cleared by the
    // level restore, and the mixer treats the two independently.
    if (o.contains(QStringLiteral("level")))
        setLevel(clampLevel(o.value(QStringLiteral("level")).toInt()));
    if (o.contains(QStringLiteral("muted")))
        setMuted(o.value(QStringLiteral("muted")).toBool());
}
