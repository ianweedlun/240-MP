/****************************************************************************
** Meta object code from reading C++ file 'RetroArchBackend.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.10.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../src/modules/retroarch/RetroArchBackend.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'RetroArchBackend.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.10.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN16RetroArchBackendE_t {};
} // unnamed namespace

template <> constexpr inline auto RetroArchBackend::qt_create_metaobjectdata<qt_meta_tag_ZN16RetroArchBackendE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "RetroArchBackend",
        "systemsReady",
        "",
        "QVariantList",
        "systems",
        "playlistsReady",
        "playlists",
        "gamesReady",
        "games",
        "retroarchPathChanged",
        "playlist_directory",
        "rom_directory",
        "thumbnail_directory",
        "retroarch_path",
        "refreshSystems",
        "refreshAllPlaylists",
        "refreshPlaylists",
        "systemId",
        "refreshGames",
        "playlistPath",
        "resolveThumbnail",
        "gameName",
        "playlistBasename",
        "launchGame",
        "QVariantMap",
        "game",
        "systemFromPlaylist",
        "shortNameForSystem",
        "retroarchPath"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'systemsReady'
        QtMocHelpers::SignalData<void(const QVariantList &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'playlistsReady'
        QtMocHelpers::SignalData<void(const QVariantList &)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 6 },
        }}),
        // Signal 'gamesReady'
        QtMocHelpers::SignalData<void(const QVariantList &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 8 },
        }}),
        // Signal 'retroarchPathChanged'
        QtMocHelpers::SignalData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'playlist_directory'
        QtMocHelpers::MethodData<QString() const>(10, 2, QMC::AccessPublic, QMetaType::QString),
        // Method 'rom_directory'
        QtMocHelpers::MethodData<QString() const>(11, 2, QMC::AccessPublic, QMetaType::QString),
        // Method 'thumbnail_directory'
        QtMocHelpers::MethodData<QString() const>(12, 2, QMC::AccessPublic, QMetaType::QString),
        // Method 'retroarch_path'
        QtMocHelpers::MethodData<QString() const>(13, 2, QMC::AccessPublic, QMetaType::QString),
        // Method 'refreshSystems'
        QtMocHelpers::MethodData<QVariantList()>(14, 2, QMC::AccessPublic, 0x80000000 | 3),
        // Method 'refreshAllPlaylists'
        QtMocHelpers::MethodData<QVariantList()>(15, 2, QMC::AccessPublic, 0x80000000 | 3),
        // Method 'refreshPlaylists'
        QtMocHelpers::MethodData<QVariantList(const QString &)>(16, 2, QMC::AccessPublic, 0x80000000 | 3, {{
            { QMetaType::QString, 17 },
        }}),
        // Method 'refreshGames'
        QtMocHelpers::MethodData<QVariantList(const QString &)>(18, 2, QMC::AccessPublic, 0x80000000 | 3, {{
            { QMetaType::QString, 19 },
        }}),
        // Method 'resolveThumbnail'
        QtMocHelpers::MethodData<QString(const QString &, const QString &) const>(20, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 21 }, { QMetaType::QString, 22 },
        }}),
        // Method 'launchGame'
        QtMocHelpers::MethodData<void(const QVariantMap &, const QString &)>(23, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 24, 25 }, { QMetaType::QString, 17 },
        }}),
        // Method 'systemFromPlaylist'
        QtMocHelpers::MethodData<QString(const QString &) const>(26, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 19 },
        }}),
        // Method 'shortNameForSystem'
        QtMocHelpers::MethodData<QString(const QString &) const>(27, 2, QMC::AccessPublic, QMetaType::QString, {{
            { QMetaType::QString, 17 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'retroarchPath'
        QtMocHelpers::PropertyData<QString>(28, QMetaType::QString, QMC::DefaultPropertyFlags, 3),
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<RetroArchBackend, qt_meta_tag_ZN16RetroArchBackendE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject RetroArchBackend::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN16RetroArchBackendE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN16RetroArchBackendE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN16RetroArchBackendE_t>.metaTypes,
    nullptr
} };

void RetroArchBackend::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<RetroArchBackend *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->systemsReady((*reinterpret_cast<std::add_pointer_t<QVariantList>>(_a[1]))); break;
        case 1: _t->playlistsReady((*reinterpret_cast<std::add_pointer_t<QVariantList>>(_a[1]))); break;
        case 2: _t->gamesReady((*reinterpret_cast<std::add_pointer_t<QVariantList>>(_a[1]))); break;
        case 3: _t->retroarchPathChanged(); break;
        case 4: { QString _r = _t->playlist_directory();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 5: { QString _r = _t->rom_directory();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 6: { QString _r = _t->thumbnail_directory();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 7: { QString _r = _t->retroarch_path();
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 8: { QVariantList _r = _t->refreshSystems();
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 9: { QVariantList _r = _t->refreshAllPlaylists();
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 10: { QVariantList _r = _t->refreshPlaylists((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 11: { QVariantList _r = _t->refreshGames((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QVariantList*>(_a[0]) = std::move(_r); }  break;
        case 12: { QString _r = _t->resolveThumbnail((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 13: _t->launchGame((*reinterpret_cast<std::add_pointer_t<QVariantMap>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 14: { QString _r = _t->systemFromPlaylist((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        case 15: { QString _r = _t->shortNameForSystem((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])));
            if (_a[0]) *reinterpret_cast<QString*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (RetroArchBackend::*)(const QVariantList & )>(_a, &RetroArchBackend::systemsReady, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (RetroArchBackend::*)(const QVariantList & )>(_a, &RetroArchBackend::playlistsReady, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (RetroArchBackend::*)(const QVariantList & )>(_a, &RetroArchBackend::gamesReady, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (RetroArchBackend::*)()>(_a, &RetroArchBackend::retroarchPathChanged, 3))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<QString*>(_v) = _t->retroarchPath(); break;
        default: break;
        }
    }
}

const QMetaObject *RetroArchBackend::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *RetroArchBackend::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN16RetroArchBackendE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int RetroArchBackend::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 16)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 16;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 16)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 16;
    }
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    }
    return _id;
}

// SIGNAL 0
void RetroArchBackend::systemsReady(const QVariantList & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void RetroArchBackend::playlistsReady(const QVariantList & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void RetroArchBackend::gamesReady(const QVariantList & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void RetroArchBackend::retroarchPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}
QT_WARNING_POP
