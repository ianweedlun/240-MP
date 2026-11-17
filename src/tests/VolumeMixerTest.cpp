// Verifies the app-wide system-volume abstraction (issue #34): the
// VolumeMixer contract and the factory's runtime backend detection.
// No real audio hardware is needed — on WSL/CI the factory falls back to the
// no-op mixer, which is exactly the path exercised here.
#include <QTest>

#include <memory>

#include "audio/ALSAVolumeMixer.h"
#include "audio/NoopVolumeMixer.h"
#include "audio/PipeWireVolumeMixer.h"
#include "audio/VolumeMixerFactory.h"

using namespace audio;

namespace {

// Stand-in for a sound card: answers `scontrols`/`sget` and applies (or
// ignores) `set` exactly like amixer would, so the mixer's read-back logic runs
// for real without any audio hardware.
struct AmixerStub {
    QString volumeControl = QStringLiteral("PCM");
    QString switchControl = QStringLiteral("PCM Playback Switch");
    bool switchPresent = false; // combined control (a Pi) by default
    bool hardwareMuted = false;
    bool applySet = true;       // false = amixer errors out, card unchanged
    bool sgetReturnsEmpty = false;
    QStringList calls;

    QString run(const QStringList &args) {
        calls << args.join(QLatin1Char(' '));
        const QString cmd = args.value(0);

        if (cmd == QStringLiteral("scontrols")) {
            QString out = QStringLiteral("Simple mixer control '%1',0\n").arg(volumeControl);
            if (switchPresent)
                out += QStringLiteral("Simple mixer control '%1',0\n").arg(switchControl);
            return out;
        }
        if (cmd == QStringLiteral("set")) {
            const bool mute = args.value(2) == QStringLiteral("mute");
            if (applySet) hardwareMuted = mute;
            return QStringLiteral("Simple mixer control '%1',0\n  Mono: Playback [%2]\n")
                .arg(args.value(1), hardwareMuted ? QStringLiteral("off") : QStringLiteral("on"));
        }
        if (cmd == QStringLiteral("sget")) {
            if (sgetReturnsEmpty) return QString(); // amixer failed: stderr only
            return QStringLiteral("Simple mixer control '%1',0\n"
                                  "  Capabilities: pswitch pswitch-joined\n"
                                  "  Playback channels: Mono\n"
                                  "  Mono: Playback [%2]\n")
                .arg(args.value(1), hardwareMuted ? QStringLiteral("off") : QStringLiteral("on"));
        }
        return QString();
    }

    bool sawCall(const QString &exact) const { return calls.contains(exact); }
    int countCalls(const QString &prefix) const {
        int n = 0;
        for (const QString &c : calls)
            if (c.startsWith(prefix)) ++n;
        return n;
    }
};

ALSAVolumeMixer::AmixerRunner stubRunner(const std::shared_ptr<AmixerStub> &stub) {
    return [stub](const QStringList &args) { return stub->run(args); };
}

} // namespace

class VolumeMixerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // setLevel clamps out-of-range values into 0..100.
    void testNoopClampsLevel() {
        NoopVolumeMixer m;
        QVERIFY(m.setLevel(150));
        QCOMPARE(m.level(), 100);
        QVERIFY(m.setLevel(-10));
        QCOMPARE(m.level(), 0);
        QCOMPARE(m.setLevel(42), true);
        QCOMPARE(m.level(), 42);
    }

    // Mute is tracked independently and survives level changes.
    void testNoopMuteState() {
        NoopVolumeMixer m;
        QVERIFY(!m.isMuted());
        QVERIFY(m.setMuted(true));
        QVERIFY(m.isMuted());
        // Turning the volume up is how you un-mute (matches a physical button).
        m.setLevel(80);
        QVERIFY(m.isMuted()); // no-op keeps whatever it was told
    }

    // Every backend reports a distinct, stable name.
    void testBackendNames() {
        QCOMPARE(NoopVolumeMixer().backendName(), QStringLiteral("none"));
        QCOMPARE(PipeWireVolumeMixer().backendName(), QStringLiteral("pipewire"));
        QCOMPARE(ALSAVolumeMixer().backendName(), QStringLiteral("alsa"));
    }

    // The factory always returns a usable mixer and never a null pointer.
    void testFactoryAlwaysSucceeds() {
        auto mixer = makeSystemVolumeMixer();
        QVERIFY(mixer != nullptr);
        // On machines without audio daemons (WSL/CI containers) it falls back to none;
        // on real hardware with ALSA or PipeWire, it selects that backend.
        const QString name = mixer->backendName();
        QVERIFY(name == QStringLiteral("none") ||
                name == QStringLiteral("alsa") ||
                name == QStringLiteral("pipewire"));
    }

    // The factory's choice is stable across repeated calls.
    void testFactoryDeterministic() {
        QCOMPARE(makeSystemVolumeMixer()->backendName(),
                 makeSystemVolumeMixer()->backendName());
    }

    // Auto-detection must land on a control that actually exists: a desktop's
    // 'Master', a Pi's 'PCM'/'Headphone', and a sane fallback when the card
    // exposes nothing we recognise (issue #34 — volume keys were a no-op on
    // hardware whose card has no 'Master').
    void testAlsaControlDetection() {
        // Pi bcm2835 (composite/headphone out).
        const QString pi = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "Simple mixer control 'Headphone',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(pi), QStringLiteral("PCM"));

        // Desktop card.
        const QString desktop = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "Simple mixer control 'Master',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(desktop), QStringLiteral("Master"));

        // Only a headphone control (some Pi configs).
        const QString phone = QStringLiteral("Simple mixer control 'Headphone',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(phone), QStringLiteral("Headphone"));

        // Nothing usable -> historical default, not an arbitrary control.
        QCOMPARE(ALSAVolumeMixer::pickControl(QStringLiteral("Simple mixer control 'Mic',0\n")),
                 QStringLiteral("Master"));
        QCOMPARE(ALSAVolumeMixer::pickControl(QStringLiteral("")), QStringLiteral("Master"));
    }

    // amixer's `get` output carries the level as "[71%]" — the old regex looked
    // for a leading '%' and never matched, so level() always echoed its cache.
    void testAlsaLevelParsing() {
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Front Left: Playback 50 [71%] [-10.00dB] [on]\n"
                     "Front Right: Playback 50 [71%] [-10.00dB] [on]\n")), 71);
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Mono: Playback 0 [0%] [-51.00dB] [off]")), 0);
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(QStringLiteral(
                     "Simple mixer control 'PCM',0")), -1);  // no level -> cache
    }

    // Split-control cards name the elements separately instead of exposing a
    // combined 'Master': `scontrols` then lists 'Master Playback Volume' and
    // 'Master Playback Switch'. The volume half must be picked up by the same
    // detection, and the switch half paired with it — otherwise mute targets a
    // volume element and does nothing.
    void testAlsaSplitControlPairing() {
        const QString split = QStringLiteral(
            "Simple mixer control 'Master Playback Volume',0\n"
            "Simple mixer control 'Master Playback Switch',0\n"
            "Simple mixer control 'Mic',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(split), QStringLiteral("Master Playback Volume"));
        QCOMPARE(ALSAVolumeMixer::pickSwitchControl(split, QStringLiteral("Master Playback Volume")),
                 QStringLiteral("Master Playback Switch"));

        // Same pairing when the pinned name is the bare control.
        const QString pcmSplit = QStringLiteral(
            "Simple mixer control 'PCM Playback Volume',0\n"
            "Simple mixer control 'PCM Playback Switch',0\n");
        QCOMPARE(ALSAVolumeMixer::pickSwitchControl(pcmSplit, QStringLiteral("PCM")),
                 QStringLiteral("PCM Playback Switch"));

        // A volume control with no matching switch resolves to nothing, so the
        // caller keeps driving the control it already has.
        const QString volumeOnly = QStringLiteral("Simple mixer control 'PCM Playback Volume',0\n");
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(volumeOnly, QStringLiteral("PCM Playback Volume")).isEmpty());
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(QString(), QStringLiteral("PCM")).isEmpty());
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(pcmSplit, QString()).isEmpty());
    }

    // Playback switch read-back: [on]/[off], plus the "any channel off counts
    // as muted" rule and the no-value case that must not be mistaken for audio
    // that is playing.
    void testAlsaSwitchParsing() {
        bool ok = false;
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(QStringLiteral("  Mono: Playback [on]"), &ok));
        QVERIFY(ok);

        QVERIFY(ALSAVolumeMixer::parseSwitchMuted(QStringLiteral("  Mono: Playback [off]"), &ok));
        QVERIFY(ok);

        // Stereo rows, one side muted -> mute is on.
        QVERIFY(ALSAVolumeMixer::parseSwitchMuted(QStringLiteral(
                    "  Front Left: Playback [off]\n"
                    "  Front Right: Playback [on]\n"), &ok));
        QVERIFY(ok);

        // No switch reading at all (capture-only output): unknown, not "off".
        ok = true;
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(QStringLiteral(
                    "Simple mixer control 'Mic',0\n"
                    "  Capture channels: Mono\n"
                    "  Mono: Capture [on]\n"), &ok));
        QVERIFY(!ok);
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(QString(), &ok));
        QVERIFY(!ok);
    }

    // The project's Pi card, verbatim from the hardware: `amixer scontrols`
    // lists one combined 'PCM' control (pvolume + pswitch), and the split
    // 'PCM Playback Switch' name is only a raw numid — `amixer sget 'PCM
    // Playback Switch'` fails there. Mute must therefore read back from the
    // combined control rather than assume the split pair.
    void testAlsaPiCombinedControlFixture() {
        const QString piScontrols = QStringLiteral("Simple mixer control 'PCM',0\n");
        QCOMPARE(ALSAVolumeMixer::pickControl(piScontrols), QStringLiteral("PCM"));
        QVERIFY(ALSAVolumeMixer::pickSwitchControl(piScontrols, QStringLiteral("PCM")).isEmpty());

        const QString piOn = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "  Capabilities: pvolume pvolume-joined pswitch pswitch-joined\n"
            "  Playback channels: Mono\n"
            "  Limits: Playback -10239 - 400\n"
            "  Mono: Playback -2260 [75%] [-22.60dB] [on]\n");
        const QString piOff = QStringLiteral(
            "Simple mixer control 'PCM',0\n"
            "  Capabilities: pvolume pvolume-joined pswitch pswitch-joined\n"
            "  Playback channels: Mono\n"
            "  Limits: Playback -10239 - 400\n"
            "  Mono: Playback -2260 [75%] [-22.60dB] [off]\n");

        bool ok = false;
        QVERIFY(!ALSAVolumeMixer::parseSwitchMuted(piOn, &ok));
        QVERIFY(ok);
        QVERIFY(ALSAVolumeMixer::parseSwitchMuted(piOff, &ok));
        QVERIFY(ok);
        // The same output still yields the level — the [on]/[off] token must not
        // be confused with the "[75%]" reading.
        QCOMPARE(ALSAVolumeMixer::parseLevelPercent(piOff), 75);
    }

    // Mute on a split-control card: the write and the read-back both address the
    // SWITCH element, the cached flag follows hardware, and a mute applied
    // outside the app is visible immediately.
    void testAlsaMuteReadBackTargetsSwitch() {
        auto stub = std::make_shared<AmixerStub>();
        stub->volumeControl = QStringLiteral("PCM Playback Volume");
        stub->switchControl = QStringLiteral("PCM Playback Switch");
        stub->switchPresent = true;

        ALSAVolumeMixer mixer(QString(), stubRunner(stub));
        QCOMPARE(stub->calls.value(0), QStringLiteral("scontrols"));
        QVERIFY(!mixer.isMuted());
        QVERIFY(stub->sawCall(QStringLiteral("sget PCM Playback Switch")));

        // Someone else mutes the card: isMuted() must report it, cache or not.
        stub->hardwareMuted = true;
        QVERIFY(mixer.isMuted());

        // Unmute through the mixer: it must talk to the switch, never to the
        // volume element, and confirm the result from hardware.
        stub->calls.clear();
        QVERIFY(mixer.setMuted(false));
        QVERIFY(stub->sawCall(QStringLiteral("set PCM Playback Switch unmute")));
        QVERIFY(stub->sawCall(QStringLiteral("sget PCM Playback Switch")));
        QCOMPARE(stub->countCalls(QStringLiteral("set PCM Playback Volume")), 0);
        QVERIFY(!mixer.isMuted());

        QVERIFY(mixer.setMuted(true));
        QVERIFY(stub->hardwareMuted);
        QVERIFY(mixer.isMuted());
    }

    // A `set` that does not take effect must not be reported as success, and
    // must not leave the cached flag believing the new state (the old code
    // assigned the flag before checking anything).
    void testAlsaSetMutedRequiresConfirmation() {
        auto stub = std::make_shared<AmixerStub>();
        ALSAVolumeMixer mixer(QString(), stubRunner(stub));
        QVERIFY(!mixer.isMuted());

        stub->applySet = false; // amixer echoes but the card ignores the write
        QVERIFY(!mixer.setMuted(true));
        QVERIFY(stub->sawCall(QStringLiteral("set PCM mute"))); // the write was attempted
        QVERIFY(!mixer.isMuted());                              // still unmuted

        // Read-back unavailable entirely: state unknown, so report failure and
        // keep the last hardware-observed value rather than the requested one.
        stub->hardwareMuted = true;
        QVERIFY(mixer.isMuted()); // establishes a known-good cache
        stub->sgetReturnsEmpty = true;
        QVERIFY(!mixer.setMuted(false));
        QVERIFY(mixer.isMuted()); // falls back to the last value read from hardware
    }

    // No separate switch (the Pi's combined control): mute is driven on the
    // volume control itself, which carries the pswitch, and still read back.
    void testAlsaMuteCombinedControlFallback() {
        auto stub = std::make_shared<AmixerStub>();
        stub->volumeControl = QStringLiteral("PCM");
        stub->switchPresent = false;

        ALSAVolumeMixer mixer(QString(), stubRunner(stub));
        QVERIFY(stub->sawCall(QStringLiteral("scontrols")));
        QVERIFY(!mixer.isMuted());

        QVERIFY(mixer.setMuted(true));
        QVERIFY(stub->sawCall(QStringLiteral("set PCM mute")));
        QVERIFY(stub->sawCall(QStringLiteral("sget PCM")));
        QVERIFY(mixer.isMuted());

        QVERIFY(mixer.setMuted(false));
        QVERIFY(stub->sawCall(QStringLiteral("set PCM unmute")));
        QVERIFY(!mixer.isMuted());
    }
};

QTEST_MAIN(VolumeMixerTest)
#include "VolumeMixerTest.moc"
