/****************************************************************************
** Meta object code from reading C++ file 'streamsession.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../app/streaming/streamsession.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'streamsession.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
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
struct qt_meta_tag_ZN13StreamSessionE_t {};
} // unnamed namespace

template <> constexpr inline auto StreamSession::qt_create_metaobjectdata<qt_meta_tag_ZN13StreamSessionE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "StreamSession",
        "stageStarting",
        "",
        "stage",
        "stageFailed",
        "errorCode",
        "failingPorts",
        "connectionStarted",
        "displayLaunchError",
        "text",
        "quitStarting",
        "sessionFinished",
        "protocolResult",
        "readyForDeletion",
        "launchWarningsChanged",
        "initialize",
        "QQuickWindow*",
        "qtWindow",
        "start",
        "interrupt",
        "launchWarnings",
        "State",
        "Created",
        "Initialized",
        "Starting",
        "Running",
        "Finished",
        "ReadyForDeletion",
        "FailedToInitialize"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'stageStarting'
        QtMocHelpers::SignalData<void(QString)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'stageFailed'
        QtMocHelpers::SignalData<void(QString, int, QString)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 }, { QMetaType::Int, 5 }, { QMetaType::QString, 6 },
        }}),
        // Signal 'connectionStarted'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'displayLaunchError'
        QtMocHelpers::SignalData<void(QString)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 9 },
        }}),
        // Signal 'quitStarting'
        QtMocHelpers::SignalData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'sessionFinished'
        QtMocHelpers::SignalData<void(int)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 12 },
        }}),
        // Signal 'readyForDeletion'
        QtMocHelpers::SignalData<void()>(13, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'launchWarningsChanged'
        QtMocHelpers::SignalData<void()>(14, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'initialize'
        QtMocHelpers::MethodData<bool(QQuickWindow *)>(15, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { 0x80000000 | 16, 17 },
        }}),
        // Method 'start'
        QtMocHelpers::MethodData<void()>(18, 2, QMC::AccessPublic, QMetaType::Void),
        // Method 'interrupt'
        QtMocHelpers::MethodData<void()>(19, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'launchWarnings'
        QtMocHelpers::PropertyData<QStringList>(20, QMetaType::QStringList, QMC::DefaultPropertyFlags, 7),
    };
    QtMocHelpers::UintData qt_enums {
        // enum 'State'
        QtMocHelpers::EnumData<enum State>(21, 21, QMC::EnumIsScoped).add({
            {   22, State::Created },
            {   23, State::Initialized },
            {   24, State::Starting },
            {   25, State::Running },
            {   26, State::Finished },
            {   27, State::ReadyForDeletion },
            {   28, State::FailedToInitialize },
        }),
    };
    return QtMocHelpers::metaObjectData<StreamSession, qt_meta_tag_ZN13StreamSessionE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject StreamSession::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13StreamSessionE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13StreamSessionE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13StreamSessionE_t>.metaTypes,
    nullptr
} };

void StreamSession::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<StreamSession *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->stageStarting((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->stageFailed((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 2: _t->connectionStarted(); break;
        case 3: _t->displayLaunchError((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 4: _t->quitStarting(); break;
        case 5: _t->sessionFinished((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 6: _t->readyForDeletion(); break;
        case 7: _t->launchWarningsChanged(); break;
        case 8: { bool _r = _t->initialize((*reinterpret_cast<std::add_pointer_t<QQuickWindow*>>(_a[1])));
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        case 9: _t->start(); break;
        case 10: _t->interrupt(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 8:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< QQuickWindow* >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)(QString )>(_a, &StreamSession::stageStarting, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)(QString , int , QString )>(_a, &StreamSession::stageFailed, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)()>(_a, &StreamSession::connectionStarted, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)(QString )>(_a, &StreamSession::displayLaunchError, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)()>(_a, &StreamSession::quitStarting, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)(int )>(_a, &StreamSession::sessionFinished, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)()>(_a, &StreamSession::readyForDeletion, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (StreamSession::*)()>(_a, &StreamSession::launchWarningsChanged, 7))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<QStringList*>(_v) = _t->launchWarnings(); break;
        default: break;
        }
    }
}

const QMetaObject *StreamSession::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *StreamSession::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13StreamSessionE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int StreamSession::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
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
void StreamSession::stageStarting(QString _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void StreamSession::stageFailed(QString _t1, int _t2, QString _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3);
}

// SIGNAL 2
void StreamSession::connectionStarted()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void StreamSession::displayLaunchError(QString _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void StreamSession::quitStarting()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void StreamSession::sessionFinished(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void StreamSession::readyForDeletion()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void StreamSession::launchWarningsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}
QT_WARNING_POP
