#pragma once

#include "ui/KeyMap.h"

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <functional>

namespace scvp::ui {

// Application-wide key handling for player actions. Installed on qApp; for keys bound in the
// KeyMap it accepts ShortcutOverride (so QAction shortcuts never steal them) and dispatches on
// KeyPress. It stays out of the way while a popup or modal dialog is open, and while a text
// field or spin box has focus it only takes Escape/Enter (which return focus to `home`).
class KeyRouter : public QObject {
    Q_OBJECT
public:
    // `repeatAllowed` is asked for gated auto-repeats (true once the previous step is shown).
    KeyRouter(QWidget* window, QWidget* home, KeyMap map, std::function<bool()> repeatAllowed, QObject* parent = nullptr);

    void setKeyMap(const KeyMap& map) { m_map = map; }
    const KeyMap& keyMap() const { return m_map; }
    void setEnabled(bool on) { m_enabled = on; }

signals:
    void triggered(scvp::ui::Action action, bool autoRepeat);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    bool routable(QObject* obj) const;

    QPointer<QWidget> m_window;
    QPointer<QWidget> m_home;
    KeyMap m_map;
    std::function<bool()> m_repeatAllowed;
    bool m_enabled = true;
};

} // namespace scvp::ui
