// Consumer Control drain regression tests (CONSUMER-CTRL-DRAIN-STARVE-01).
//
// The drain slot under test is the shipped function body, lifted out of
// src/input/InputManager.cpp at build time by extract_consumer_ctrl_drain.py.
// Only the three action sinks are stubbed; the read loop, skip/remap/digit,
// capture, EINTR/EAGAIN and teardown logic are the product code itself. A real
// nonblocking pipe plus QSocketNotifier drives it, and a QTimer heartbeat plus
// an independent wall-clock watchdog detect GUI-thread starvation.
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QHash>
#include <QtGui/QKeyEvent>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QSocketNotifier>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtTest/QTest>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

namespace {
std::atomic<int> g_hb{0};
std::atomic<int> g_begin{0}, g_release{0}, g_postKey{0}, g_aux{0};
std::atomic<int> g_lastKey{0}, g_lastKeyType{0};
std::atomic<long long> g_maxGapUs{0}, g_lastHbMs{0};
std::atomic<int> g_notifierActivations{0};
std::atomic<bool> g_forceEintr{false}, g_writerStop{false}, g_watchdogStop{false};
std::atomic<bool> g_slotStarted{false}, g_slotReturned{false};
std::atomic<long long> g_slotStartWallMs{0};

long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void resetCounters()
{
    g_hb.store(0);
    g_begin.store(0);
    g_release.store(0);
    g_postKey.store(0);
    g_aux.store(0);
    g_lastKey.store(0);
    g_lastKeyType.store(0);
    g_maxGapUs.store(0);
    g_notifierActivations.store(0);
    g_forceEintr.store(false);
    g_writerStop.store(false);
    g_watchdogStop.store(false);
    g_slotStarted.store(false);
    g_slotReturned.store(false);
}

input_event mk(int type, int code, int value)
{
    input_event ev{};
    ev.type = static_cast<__u16>(type);
    ev.code = static_cast<__u16>(code);
    ev.value = value;
    return ev;
}

void writeAll(int fd, const void *buf, size_t n)
{
    const char *p = static_cast<const char *>(buf);
    size_t left = n;
    while (left) {
        ssize_t w = ::write(fd, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        p += w;
        left -= static_cast<size_t>(w);
    }
}
}  // namespace

extern "C" ssize_t __real_read(int fd, void *buf, size_t n);
extern "C" ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    if (g_forceEintr.exchange(false)) {
        errno = EINTR;
        return -1;
    }
    return __real_read(fd, buf, n);
}

// Minimal host for the verbatim product function: same member names and sink
// signatures, everything else irrelevant to the drain path.
class InputManager : public QObject {
    Q_OBJECT
public:
    enum class Action { None, Up, Down, Left, Right, Select, Back, PlayPause,
                        ChannelUp, ChannelDown };

    explicit InputManager(int fd) : QObject(nullptr), m_consumerFd(fd) {}

    void armNotifier()
    {
        m_consumerNotifier = new QSocketNotifier(m_consumerFd, QSocketNotifier::Read, this);
        m_savedNotifier = m_consumerNotifier;
        connect(m_consumerNotifier, &QSocketNotifier::activated,
                this, &InputManager::onConsumerControlReadable);
        connect(m_consumerNotifier, &QSocketNotifier::activated,
                this, []() { g_notifierActivations.fetch_add(1); });
    }
    int fd() const { return m_consumerFd; }
    QSocketNotifier *savedNotifier() const { return m_savedNotifier; }
    void forgetNotifier() { m_savedNotifier = nullptr; }
    void setRemap(int id, Action a) { m_keyRemap.insert(id, a); }
    void setCapture(bool b) { m_remapCapture = b; }

signals:
    void auxButtonPressed(int extendedKeyId);

public slots:
    void onConsumerControlReadable();

private:
    void postKey(int qtKey, QEvent::Type type, bool autoRepeat);
    void beginPress(Action a);
    void releaseAction(Action a);
    static int qtDigitForEvdevCode(int code);

    int m_consumerFd = -1;
    QSocketNotifier *m_consumerNotifier = nullptr;
    QSocketNotifier *m_savedNotifier = nullptr;  // evidence only
    bool m_remapCapture = false;
    QHash<int, Action> m_keyRemap;
};

void InputManager::postKey(int key, QEvent::Type type, bool)
{
    g_postKey.fetch_add(1);
    g_lastKey.store(key);
    g_lastKeyType.store(static_cast<int>(type));
}
void InputManager::beginPress(Action) { g_begin.fetch_add(1); }
void InputManager::releaseAction(Action) { g_release.fetch_add(1); }

#include "consumer_ctrl_drain.inc"

namespace {
struct Rig {
    int fds[2] = {-1, -1};
    InputManager *mgr = nullptr;

    explicit Rig(bool arm = true)
    {
        if (::pipe(fds) != 0)
            return;
        ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
        ::fcntl(fds[0], F_SETPIPE_SZ, 1 << 20);
        mgr = new InputManager(fds[0]);
        if (arm)
            mgr->armNotifier();
        QObject::connect(mgr, &InputManager::auxButtonPressed, mgr,
                         [](int) { g_aux.fetch_add(1); });
    }
    ~Rig()
    {
        delete mgr;
        ::close(fds[0]);
        ::close(fds[1]);
    }
};
}  // namespace

class ConsumerControlDrainTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init() { resetCounters(); }

    void floodValidEventsYields()
    {
        runFlood(true, 1);
    }

    void floodSkippedEventsYields()
    {
        runFlood(false, 0);
    }

    void normalBurstDrainsFully()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);
        std::vector<input_event> burst;
        for (int i = 0; i < 4; ++i) {
            burst.push_back(mk(EV_KEY, KEY_PLAYPAUSE, 1));
            burst.push_back(mk(EV_KEY, KEY_PLAYPAUSE, 0));
        }
        writeAll(rig.fds[1], burst.data(), burst.size() * sizeof(input_event));
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QCOMPARE(g_begin.load(), 4);
        QCOMPARE(g_release.load(), 4);
    }

    void emptyPipeReturnsImmediately()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QVERIFY(timer.elapsed() < 20);
        QCOMPARE(g_begin.load(), 0);
        QCOMPARE(g_release.load(), 0);
    }

    void eintrIsRetried()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);
        const input_event ev = mk(EV_KEY, KEY_PLAYPAUSE, 1);
        writeAll(rig.fds[1], &ev, sizeof(ev));
        g_forceEintr.store(true);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QVERIFY(timer.elapsed() < 20);
        QCOMPARE(g_begin.load(), 1);
    }

    void eofTearsDownNotifierAndFd()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);
        const input_event ev = mk(EV_KEY, KEY_PLAYPAUSE, 1);
        writeAll(rig.fds[1], &ev, sizeof(ev));
        ::close(rig.fds[1]);
        rig.fds[1] = -1;
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QCOMPARE(rig.mgr->fd(), -1);
        QVERIFY(rig.mgr->savedNotifier() != nullptr);
        QVERIFY(!rig.mgr->savedNotifier()->isEnabled());
        QCOMPARE(g_begin.load(), 1);
        rig.mgr->forgetNotifier();
    }

    void skippedEventsDispatchNothing()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);
        const input_event abs = mk(EV_ABS, ABS_X, 5);
        const input_event repeat = mk(EV_KEY, KEY_PLAYPAUSE, 2);
        const input_event press = mk(EV_KEY, KEY_PLAYPAUSE, 1);
        writeAll(rig.fds[1], &abs, sizeof(abs));
        writeAll(rig.fds[1], &repeat, sizeof(repeat));
        writeAll(rig.fds[1], &press, sizeof(press));
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QCOMPARE(g_begin.load(), 1);
        QCOMPARE(g_postKey.load(), 0);
    }

    void digitKeyIsPosted()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        const input_event ev = mk(EV_KEY, KEY_1, 1);
        writeAll(rig.fds[1], &ev, sizeof(ev));
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QCOMPARE(g_postKey.load(), 1);
        QCOMPARE(g_lastKey.load(), static_cast<int>(Qt::Key_1));
        QCOMPARE(g_lastKeyType.load(), static_cast<int>(QEvent::KeyPress));
    }

    void remappedPressAndRelease()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);
        const input_event press = mk(EV_KEY, KEY_PLAYPAUSE, 1);
        const input_event release = mk(EV_KEY, KEY_PLAYPAUSE, 0);
        writeAll(rig.fds[1], &press, sizeof(press));
        writeAll(rig.fds[1], &release, sizeof(release));
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QCOMPARE(g_begin.load(), 1);
        QCOMPARE(g_release.load(), 1);
    }

    void captureModeSignalsOnly()
    {
        Rig rig;
        QVERIFY(rig.mgr);
        rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);
        rig.mgr->setCapture(true);
        const input_event press = mk(EV_KEY, KEY_PLAYPAUSE, 1);
        writeAll(rig.fds[1], &press, sizeof(press));
        QVERIFY(QMetaObject::invokeMethod(rig.mgr, "onConsumerControlReadable", Qt::DirectConnection));
        QCOMPARE(g_aux.load(), 1);
        QCOMPARE(g_begin.load(), 0);
        QCOMPARE(g_postKey.load(), 0);
    }

private:
    void runFlood(bool validEvents, int expectedMinimum);
};

void ConsumerControlDrainTest::runFlood(bool validEvents, int expectedMinimum)
{
    Rig rig;
    QVERIFY(rig.mgr);
    rig.mgr->setRemap(kEvdevKeyBase + KEY_PLAYPAUSE, InputManager::Action::PlayPause);

    QElapsedTimer hbClock;
    hbClock.start();
    qint64 lastNs = 0;
    QTimer heartbeat;
    heartbeat.setInterval(20);
    QObject::connect(&heartbeat, &QTimer::timeout, [&]() {
        const qint64 now = hbClock.nsecsElapsed();
        const qint64 gap = now - lastNs;
        lastNs = now;
        if (gap / 1000 > g_maxGapUs.load())
            g_maxGapUs.store(gap / 1000);
        g_lastHbMs.store(nowMs());
        g_hb.fetch_add(1);
    });
    heartbeat.start();
    g_lastHbMs.store(nowMs());

    const input_event shape = validEvents ? mk(EV_KEY, KEY_PLAYPAUSE, 1)
                                          : mk(EV_ABS, ABS_X, 5);
    std::vector<input_event> prefill(2400, shape);
    if (!validEvents) {
        for (size_t i = 0; i < prefill.size(); ++i)
            prefill[i] = (i % 2) ? mk(EV_KEY, KEY_PLAYPAUSE, 2) : mk(EV_ABS, ABS_X, 5);
    }
    writeAll(rig.fds[1], prefill.data(), prefill.size() * sizeof(input_event));

    std::thread writer([&]() {
        std::vector<input_event> chunk(100, shape);
        while (!g_writerStop.load()) {
            ssize_t w = ::write(rig.fds[1], chunk.data(), chunk.size() * sizeof(input_event));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0)
                break;
        }
    });
    writer.detach();

    // The slot runs on the GUI thread, as it does in production. If it never
    // yields, the heartbeat below cannot be sampled; this independent clock
    // reports the starvation and fails the process rather than hanging ctest.
    std::thread watchdog([]() {
        for (int i = 0; i < 80; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (g_watchdogStop.load())
                return;
            if (g_slotStarted.load() && !g_slotReturned.load()
                && nowMs() - g_slotStartWallMs.load() > 1500) {
                std::fprintf(stderr,
                             "FAIL: Consumer Control drain starved the event loop "
                             "(no return within 1500 ms)\n");
                std::fflush(stderr);
                ::_exit(1);
            }
        }
    });
    watchdog.detach();

    QElapsedTimer slotTimer;
    slotTimer.start();
    g_slotStarted.store(true);
    g_slotStartWallMs.store(nowMs());
    const bool invoked = QMetaObject::invokeMethod(
        rig.mgr, "onConsumerControlReadable", Qt::DirectConnection);
    const qint64 slotMs = slotTimer.elapsed();
    g_slotReturned.store(true);
    g_writerStop.store(true);

    QVERIFY(invoked);
    QVERIFY2(slotMs < 1000, qPrintable(QStringLiteral("slot took %1 ms").arg(slotMs)));
    QVERIFY2(g_begin.load() >= expectedMinimum,
             qPrintable(QStringLiteral("only %1 events dispatched").arg(g_begin.load())));
    if (!validEvents)
        QCOMPARE(g_begin.load(), 0);

    // Level-triggered notifier re-arms on the leftover data; the heartbeat must
    // keep ticking throughout those activations.
    QTest::qWait(250);
    heartbeat.stop();
    g_watchdogStop.store(true);

    QVERIFY2(g_hb.load() >= 5,
             qPrintable(QStringLiteral("heartbeat ticks %1").arg(g_hb.load())));
    QVERIFY2(g_maxGapUs.load() <= 200000,
             qPrintable(QStringLiteral("max heartbeat gap us %1").arg(g_maxGapUs.load())));
    QVERIFY2(g_notifierActivations.load() > 1,
             qPrintable(QStringLiteral("notifier activations %1").arg(g_notifierActivations.load())));

    if (validEvents)
        QTRY_VERIFY_WITH_TIMEOUT(g_begin.load() >= 2400, 2000);

    rig.mgr->forgetNotifier();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ConsumerControlDrainTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ConsumerControlDrainTest.moc"
