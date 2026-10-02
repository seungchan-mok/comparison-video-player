#pragma once

#include "engine/Engine.h"

#include <QObject>

#include <atomic>

namespace scvp::ui {

// Turns Engine callbacks (decode and scanner threads) into Qt signals on the GUI thread.
// Bursts are coalesced: at most one queued call is pending per kind at any time.
class EngineBridge : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(Engine& engine, QObject* parent = nullptr);
    ~EngineBridge() override;

signals:
    void framesArrived();
    void indexChanged();

private:
    Engine& m_engine;
    std::atomic<bool> m_framesPending{false};
    std::atomic<bool> m_indexPending{false};
};

} // namespace scvp::ui
