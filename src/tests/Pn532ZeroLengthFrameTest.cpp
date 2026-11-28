/*
 * NFC-PN532-ZEROLEN-FRAME-OOB-01 - checked-in two-sided CTest regression.
 *
 * readFrame() lives in an anonymous namespace, so this test compiles the real
 * product source into its own translation unit and drives the real SerialPort
 * against a POSIX PTY. No product file is patched by the test.
 *
 * The gate case feeds 00 FF 00 00 00 00 (LEN=0, LCS=0, one body byte, postamble).
 * Before the guard, readFrame() computed an empty payload and then called
 * payload.at(0) out of bounds: an assert-enabled build aborts, an assert-free
 * build reports a bogus empty Data frame. Both are failures here.
 *
 * The control cases run in a separate ctest entry so they stay green in both
 * arms and prove the gate is specific to the zero-length frame. The build
 * forces QT_FORCE_ASSERTS; there is no skip path for assert-enabled builds.
 *
 * To replay the RED arm after the guard lands, configure with
 * -DPN532_SOURCE_ROOT=<pinned pre-fix tree> and rebuild this one target.
 */
#include <QtTest/QtTest>

#include <QByteArray>
#include <QString>

#if defined(Q_OS_DARWIN)
#include <util.h>
#else
#include <pty.h>
#endif
#include <unistd.h>

#ifndef QT_FORCE_ASSERTS
#error "pn532-zerolen-frame-tests requires QT_FORCE_ASSERTS; the pre-fix at() must abort, not silently return"
#endif

#include PN532_SERIAL_DRIVER_CPP

namespace {

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(QByteArray(text));
}

const char *kindName(FrameKind kind)
{
    switch (kind) {
    case FrameKind::None:  return "None";
    case FrameKind::Ack:   return "Ack";
    case FrameKind::Nack:  return "Nack";
    case FrameKind::Error: return "Error";
    case FrameKind::Data:  return "Data";
    }
    return "Unknown";
}

struct PtyPair {
    int master = -1;
    int slave = -1;
    ~PtyPair()
    {
        if (master >= 0) ::close(master);
        if (slave >= 0) ::close(slave);
    }
};

// Feeds a frame through the PTY master and lets the real blocking SerialPort
// read path consume it. Returns false only when the rig itself failed.
bool runFrame(const QByteArray &stimulus, FrameKind &kind, QByteArray &payload)
{
    PtyPair pty;
    if (::openpty(&pty.master, &pty.slave, nullptr, nullptr, nullptr) != 0)
        return false;

    char name[256] = {0};
#if defined(Q_OS_DARWIN)
    const char *slaveName = ::ttyname(pty.slave);
    if (slaveName == nullptr)
        return false;
    const QByteArray path = QByteArray(slaveName);
#else
    if (::ttyname_r(pty.slave, name, sizeof(name)) != 0)
        return false;
    const QByteArray path = QByteArray(name);
#endif

    SerialPort port;
    if (!port.open(QString::fromLatin1(path.constData())))
        return false;

    const ssize_t written = ::write(pty.master, stimulus.constData(), size_t(stimulus.size()));
    if (written != ssize_t(stimulus.size()))
        return false;

    kind = readFrame(port, payload, 300);
    port.close();
    return true;
}

void checkFrame(const char *stimulusHex, FrameKind expected, const char *expectedPayloadHex)
{
    FrameKind kind = FrameKind::None;
    QByteArray payload;
    QVERIFY(runFrame(hex(stimulusHex), kind, payload));
    QVERIFY2(int(kind) == int(expected),
             qPrintable(QStringLiteral("kind=%1 expected=%2 stimulus=%3")
                            .arg(QString::fromLatin1(kindName(kind)))
                            .arg(QString::fromLatin1(kindName(expected)))
                            .arg(QString::fromLatin1(stimulusHex))));
    QCOMPARE(payload.toHex().constData(), expectedPayloadHex);
}

} // namespace

class Pn532ZeroLengthFrameTest : public QObject
{
    Q_OBJECT

private slots:
    void zeroLengthFrameReturnsNone();
    void ackFrameStillRecognized();
    void nackFrameStillRecognized();
    void validDataFrameRecognized();
    void badLcsReturnsNone();
    void truncatedFrameReturnsNone();
    void extendedLengthDesyncReturnsNone();
    void errorFrameRecognized();
    void wrongDcsLiteralFrameReturnsNone();
};

void Pn532ZeroLengthFrameTest::zeroLengthFrameReturnsNone()
{
    FrameKind kind = FrameKind::Data;
    QByteArray payload;
    QVERIFY(runFrame(hex("00FF00000000"), kind, payload));
    QVERIFY2(int(kind) == int(FrameKind::None),
             qPrintable(QStringLiteral("zero-length frame returned %1, not None")
                            .arg(QString::fromLatin1(kindName(kind)))));
    QVERIFY2(payload.isEmpty(), "zero-length frame must not expose an empty Data payload");
}

void Pn532ZeroLengthFrameTest::ackFrameStillRecognized()
{
    checkFrame("0000FF00FF00", FrameKind::Ack, "");
}

void Pn532ZeroLengthFrameTest::nackFrameStillRecognized()
{
    checkFrame("00FFFF0000", FrameKind::Nack, "");
}

void Pn532ZeroLengthFrameTest::validDataFrameRecognized()
{
    // DCS is -(D5 + 03 + 32) = F6; the card's literal 2E variant is the bad-DCS control below.
    checkFrame("00FF03FDD50332F600", FrameKind::Data, "d50332");
}

void Pn532ZeroLengthFrameTest::badLcsReturnsNone()
{
    checkFrame("00FF02FD", FrameKind::None, "");
}

void Pn532ZeroLengthFrameTest::truncatedFrameReturnsNone()
{
    checkFrame("00FF03FDD503", FrameKind::None, "");
}

void Pn532ZeroLengthFrameTest::extendedLengthDesyncReturnsNone()
{
    checkFrame("00FFFF0100", FrameKind::None, "");
}

void Pn532ZeroLengthFrameTest::errorFrameRecognized()
{
    checkFrame("00FF01FF7F8100", FrameKind::Error, "7f");
}

void Pn532ZeroLengthFrameTest::wrongDcsLiteralFrameReturnsNone()
{
    checkFrame("00FF03FDD503322E00", FrameKind::None, "");
}

QTEST_MAIN(Pn532ZeroLengthFrameTest)

#include "Pn532ZeroLengthFrameTest.moc"
