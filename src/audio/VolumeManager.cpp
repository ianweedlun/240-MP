#include "VolumeManager.h"

#include "audio/NoopVolumeMixer.h"
#include "audio/VolumeMixerFactory.h"
#include "../util/AtomicFile.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRunnable>

namespace {

constexpr int kMaxLevel = 100;

int clampLevel(int percent)
{
    return qBound(0, percent, kMaxLevel);
}

// The persisted shape, unchanged since #34: compact {"level":N,"muted":B}.
// Shared by the synchronous flush and the off-thread debounced writes.
bool writeVolumeJson(const QString &dataRoot, int level, bool muted)
{
    if (dataRoot.isEmpty() || !QDir().mkpath(dataRoot))
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("level"), level);
    o.insert(QStringLiteral("muted"), muted);
    return atomicfile::writeBytes(
        QDir(dataRoot).filePath(QStringLiteral("volume.json")),
        QJsonDocument(o).toJson(QJsonDocument::Compact),
        /*durable=*/true, "volume.json");
}

// One debounced write. Captures VALUES only (no VolumeManager pointer), so it
// is safe even if it outlives the owner mid-shutdown; the pool's single thread
// runs tasks FIFO, which makes the newest snapshot win by construction.
class PersistTask : public QRunnable
{
public:
    PersistTask(QString dataRoot, int level, bool muted)
        : m_dataRoot(std::move(dataRoot)), m_level(level), m_muted(muted) {}
    void run() override { writeVolumeJson(m_dataRoot, m_level, m_muted); }

private:
    QString m_dataRoot;
    int m_level;
    bool m_muted;
};

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
    // Cached accessors on every backend — construction never spawns.
    m_level = m_mixer->level();
    m_muted = m_mixer->isMuted();

    m_writePool.setMaxThreadCount(1);
    m_persistTimer.setSingleShot(true);
    m_persistTimer.setInterval(kPersistDebounceMs);
    connect(&m_persistTimer, &QTimer::timeout, this, [this] {
        m_dirty = false;
        if (m_dataRoot.isEmpty())
            return;
        m_writePool.start(new PersistTask(m_dataRoot, m_level, m_muted));
    });

    // Reconciliation seam: async backends (ALSA/PipeWire on the process seam)
    // are QObjects emitting stateChanged(int,bool) with generation-current,
    // hardware-confirmed observations. The no-op mixer is not a QObject and
    // simply never reconciles. String-based connect because the VolumeMixer
    // interface itself is not a QObject; both async backends declare the
    // identical signal (see their ASYNC CONTRACT headers).
    if (auto *mixerObj = dynamic_cast<QObject *>(m_mixer.get())) {
        connect(mixerObj, SIGNAL(stateChanged(int,bool)),
                this, SLOT(onMixerStateChanged(int,bool)));
    }

    restore();
}

VolumeManager::~VolumeManager()
{
    // Shutdown flush: stop new debounces, drain any in-flight (older)
    // snapshot, then write the current state synchronously so a restart
    // restores exactly what the UI last showed. Bounded: one small JSON write.
    m_persistTimer.stop();
    m_writePool.waitForDone();
    if (m_dirty)
        writeVolumeJson(m_dataRoot, m_level, m_muted);
}

void VolumeManager::setLevel(int percent)
{
    if (submitLevel(clampLevel(percent)))
        notifyAndPersist();
}

void VolumeManager::changeVolume(int delta)
{
    if (delta == 0)
        return;
    const int newLevel = clampLevel(m_level + delta);
    // Raising the volume is also how you turn the mute off (amplifier
    // behaviour). The mixer keeps mute independent, so this layer owns the
    // rule: a mute leaves the level alone, a raise clears the mute. Both
    // dimensions are submitted before ONE coherent volumeChanged goes out —
    // a single press must never paint the OSD twice.
    const bool newMuted = delta > 0 ? false : m_muted;
    const bool muteMoved = submitMuted(newMuted);
    const bool levelMoved = submitLevel(newLevel);
    if (muteMoved || levelMoved)
        notifyAndPersist();
}

bool VolumeManager::setMuted(bool isMuted)
{
    if (isMuted == m_muted)
        return true;
    if (!submitMuted(isMuted))
        return false;
    notifyAndPersist();
    return true;
}

bool VolumeManager::toggleMute()
{
    return setMuted(!m_muted);
}

int VolumeManager::level() const
{
    return m_level;
}

bool VolumeManager::muted() const
{
    return m_muted;
}

QString VolumeManager::backendName() const
{
    return m_mixer ? m_mixer->backendName() : QStringLiteral("none");
}

bool VolumeManager::submitLevel(int clamped)
{
    if (clamped == m_level)
        return false;
    // Async backends return "accepted" immediately and confirm via
    // stateChanged(); a rejection means the dimension did not move.
    if (!m_mixer->setLevel(clamped))
        return false;
    m_level = clamped;
    emit levelChanged();
    return true;
}

bool VolumeManager::submitMuted(bool isMuted)
{
    if (isMuted == m_muted)
        return false;
    if (!m_mixer->setMuted(isMuted))
        return false;
    m_muted = isMuted;
    emit mutedChanged();
    return true;
}

void VolumeManager::notifyAndPersist()
{
    emit volumeChanged(m_level, m_muted);
    schedulePersist();
}

void VolumeManager::schedulePersist()
{
    if (m_dataRoot.isEmpty())
        return;
    m_dirty = true;
    m_persistTimer.start(kPersistDebounceMs);   // restarts: one write per burst
}

bool VolumeManager::persist()
{
    m_persistTimer.stop();
    m_writePool.waitForDone();   // an older snapshot must not land after this one
    m_dirty = false;
    return writeVolumeJson(m_dataRoot, m_level, m_muted);
}

void VolumeManager::onMixerStateChanged(int level, bool muted)
{
    // Hardware-confirmed observation. The backend guarantees it is
    // generation-current (a stale read/echo over a newer request is discarded
    // at the mixer layer), so adoption here can never resurrect an old value.
    // Equal values are a plain confirmation: no signal, no write, no flicker.
    const int clamped = clampLevel(level);
    const bool levelMoved = clamped != m_level;
    const bool muteMoved = muted != m_muted;
    if (!levelMoved && !muteMoved)
        return;
    if (levelMoved) {
        m_level = clamped;
        emit levelChanged();
    }
    if (muteMoved) {
        m_muted = muted;
        emit mutedChanged();
    }
    emit volumeChanged(m_level, m_muted);
    schedulePersist();
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
    // level restore, and the mixer treats the two independently. No persist
    // here — the values came from disk; the first real change writes. (The
    // property notifies keep QML bindings correct; volumeChanged during
    // construction was never observable — consumers connect afterwards.)
    if (o.contains(QStringLiteral("level")))
        submitLevel(clampLevel(o.value(QStringLiteral("level")).toInt()));
    if (o.contains(QStringLiteral("muted")))
        submitMuted(o.value(QStringLiteral("muted")).toBool());
}