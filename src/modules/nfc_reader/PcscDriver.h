#pragma once
#include "NfcDriver.h"

#include <QStringList>

#include <atomic>
#include <cstdint>

// PC/SC (pcsc-lite on Linux, PCSC.framework on macOS) reader driver.
//
// Reads the card UID with the FF CA 00 00 00 "Get Data" pseudo-APDU, which is
// PC/SC-standard rather than vendor-specific, so this works with any CCID
// contactless reader — not only the ACR122U this module originally targeted.
//
// SCARDCONTEXT is stored as uintptr_t so this header stays free of PC/SC
// includes and can be included on builds without it.
class PcscDriver : public NfcDriver {
public:
    ~PcscDriver() override;

    QString id() const override { return QStringLiteral("pcsc"); }
    bool ensureConnected() override;
    QString deviceName() const override { return m_readerName; }
    QString pollUid(bool &ok) override;
    void close() override;

    // SCardCancel on the live context. A thread blocked in
    // SCardGetStatusChange wakes with SCARD_E_CANCELLED (treated as "no card
    // this tick" by cardPresent()), so the worker can exit and be joined.
    // Safe to call from the backend thread while the worker is blocked.
    void cancel() override;

    // True when PC/SC was compiled in. The QML layer uses this to tell "no
    // reader plugged in" apart from "this build has no PC/SC support".
    static bool compiledIn();

private:
#if defined(MP240_NFC_PCSC_AVAILABLE) && (defined(Q_OS_LINUX) || defined(Q_OS_MAC))
    QString findReader();
    bool cardPresent(const QString &readerName);
    QString readCardUid(const QString &readerName);
    // Only so debug logging can report the reader list when it changes rather
    // than on every 500 ms poll.
    QStringList m_lastListedReaders;
#endif
    // Atomic because cancel() reads it from the backend thread while the
    // worker thread may be establishing or releasing the context.
    std::atomic<uintptr_t> m_context{0};
    QString m_readerName;
};
