#include "ui/EngineBridge.h"

#include <QMetaObject>

namespace scvp::ui {

EngineBridge::EngineBridge(Engine& engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
{
    m_engine.setFramesCallback([this](int) {
        if (!m_framesPending.exchange(true))
            QMetaObject::invokeMethod(this, [this] {
                m_framesPending = false;
                emit framesArrived();
            }, Qt::QueuedConnection);
    });
    m_engine.setIndexCallback([this](int) {
        if (!m_indexPending.exchange(true))
            QMetaObject::invokeMethod(this, [this] {
                m_indexPending = false;
                emit indexChanged();
            }, Qt::QueuedConnection);
    });
}

EngineBridge::~EngineBridge()
{
    m_engine.setFramesCallback({});
    m_engine.setIndexCallback({});
}

} // namespace scvp::ui
