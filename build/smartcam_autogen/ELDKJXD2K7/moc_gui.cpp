/****************************************************************************
** Meta object code from reading C++ file 'gui.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../include/display/gui.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'gui.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_CameraGUI_t {
    QByteArrayData data[17];
    char stringdata0[182];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_CameraGUI_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_CameraGUI_t qt_meta_stringdata_CameraGUI = {
    {
QT_MOC_LITERAL(0, 0, 9), // "CameraGUI"
QT_MOC_LITERAL(1, 10, 14), // "captureClicked"
QT_MOC_LITERAL(2, 25, 0), // ""
QT_MOC_LITERAL(3, 26, 13), // "recordToggled"
QT_MOC_LITERAL(4, 40, 5), // "start"
QT_MOC_LITERAL(5, 46, 17), // "resolutionChanged"
QT_MOC_LITERAL(6, 64, 1), // "w"
QT_MOC_LITERAL(7, 66, 1), // "h"
QT_MOC_LITERAL(8, 68, 13), // "formatChanged"
QT_MOC_LITERAL(9, 82, 11), // "PixelFormat"
QT_MOC_LITERAL(10, 94, 3), // "fmt"
QT_MOC_LITERAL(11, 98, 12), // "refreshFrame"
QT_MOC_LITERAL(12, 111, 9), // "onCapture"
QT_MOC_LITERAL(13, 121, 8), // "onRecord"
QT_MOC_LITERAL(14, 130, 24), // "onResolutionComboChanged"
QT_MOC_LITERAL(15, 155, 5), // "index"
QT_MOC_LITERAL(16, 161, 20) // "onFormatComboChanged"

    },
    "CameraGUI\0captureClicked\0\0recordToggled\0"
    "start\0resolutionChanged\0w\0h\0formatChanged\0"
    "PixelFormat\0fmt\0refreshFrame\0onCapture\0"
    "onRecord\0onResolutionComboChanged\0"
    "index\0onFormatComboChanged"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_CameraGUI[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       9,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       4,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   59,    2, 0x06 /* Public */,
       3,    1,   60,    2, 0x06 /* Public */,
       5,    2,   63,    2, 0x06 /* Public */,
       8,    1,   68,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
      11,    0,   71,    2, 0x08 /* Private */,
      12,    0,   72,    2, 0x08 /* Private */,
      13,    0,   73,    2, 0x08 /* Private */,
      14,    1,   74,    2, 0x08 /* Private */,
      16,    1,   77,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,    4,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    6,    7,
    QMetaType::Void, 0x80000000 | 9,   10,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int,   15,
    QMetaType::Void, QMetaType::Int,   15,

       0        // eod
};

void CameraGUI::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<CameraGUI *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->captureClicked(); break;
        case 1: _t->recordToggled((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 2: _t->resolutionChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 3: _t->formatChanged((*reinterpret_cast< PixelFormat(*)>(_a[1]))); break;
        case 4: _t->refreshFrame(); break;
        case 5: _t->onCapture(); break;
        case 6: _t->onRecord(); break;
        case 7: _t->onResolutionComboChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 8: _t->onFormatComboChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (CameraGUI::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CameraGUI::captureClicked)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (CameraGUI::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CameraGUI::recordToggled)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (CameraGUI::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CameraGUI::resolutionChanged)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (CameraGUI::*)(PixelFormat );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&CameraGUI::formatChanged)) {
                *result = 3;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject CameraGUI::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_CameraGUI.data,
    qt_meta_data_CameraGUI,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *CameraGUI::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *CameraGUI::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_CameraGUI.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int CameraGUI::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void CameraGUI::captureClicked()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void CameraGUI::recordToggled(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void CameraGUI::resolutionChanged(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void CameraGUI::formatChanged(PixelFormat _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
