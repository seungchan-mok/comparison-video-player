#include "ui/KeyRouter.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

namespace scvp::ui {
namespace {

bool isTextInput(const QWidget* w)
{
    if (!w)
        return false;
    if (qobject_cast<const QLineEdit*>(w) || qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QTextEdit*>(w) ||
        qobject_cast<const QPlainTextEdit*>(w))
        return true;
    if (auto* c = qobject_cast<const QComboBox*>(w))
        return c->isEditable();
    return false;
}

} // namespace

KeyRouter::KeyRouter(QWidget* window, QWidget* home, KeyMap map, std::function<bool()> repeatAllowed, QObject* parent)
    : QObject(parent)
    , m_window(window)
    , m_home(home)
    , m_map(std::move(map))
    , m_repeatAllowed(std::move(repeatAllowed))
{
    qApp->installEventFilter(this);
}

bool KeyRouter::routable(QObject* obj) const
{
    if (!m_enabled || !m_window)
        return false;
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget())
        return false;
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w || w->window() != m_window)
        return false;
    return QApplication::activeWindow() == m_window;
}

bool KeyRouter::eventFilter(QObject* obj, QEvent* e)
{
    const QEvent::Type t = e->type();
    if (t != QEvent::ShortcutOverride && t != QEvent::KeyPress)
        return false;
    if (!routable(obj))
        return false;
    auto* ke = static_cast<QKeyEvent*>(e);

    QWidget* focus = QApplication::focusWidget();
    if (isTextInput(focus)) {
        // Editing keys belong to the field; Escape/Enter hand the keyboard back to the player.
        const int k = ke->key();
        if (k == Qt::Key_Escape || k == Qt::Key_Return || k == Qt::Key_Enter) {
            if (t == QEvent::KeyPress && m_home)
                m_home->setFocus(Qt::ShortcutFocusReason);  // focus-out commits a spin box value
            if (k == Qt::Key_Escape) {
                e->accept();
                return true;
            }
        }
        return false;
    }

    const std::optional<Action> a = m_map.match(ke->key(), ke->modifiers());
    if (!a)
        return false;
    if (t == QEvent::ShortcutOverride) {
        e->accept();  // ours: deliver as a key press instead of a QAction shortcut
        return true;
    }
    const Repeat policy = KeyMap::info(*a).repeat;
    if (ke->isAutoRepeat()) {
        if (policy == Repeat::None || (policy == Repeat::Gated && m_repeatAllowed && !m_repeatAllowed())) {
            e->accept();
            return true;  // dropped: releasing the key leaves nothing queued
        }
    }
    emit triggered(*a, ke->isAutoRepeat());
    e->accept();
    return true;
}

} // namespace scvp::ui
