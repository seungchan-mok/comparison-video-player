#pragma once

#include <QKeyCombination>
#include <QList>
#include <QString>

#include <array>
#include <optional>

class QSettings;

namespace scvp::ui {

enum class Action {
    PlayPause,
    StepBack,
    StepForward,
    JumpBack,
    JumpForward,
    JumpBackLarge,
    JumpForwardLarge,
    OffsetMinus,
    OffsetPlus,
    GoToStart,
    GoToEnd,
    LoopIn,
    LoopOut,
    LoopToggle,
    LoopClear,
    ViewCycle,
    ViewSideBySide,
    ViewWipe,
    ViewFlip,
    ViewDifference,
    DiffGainUp,
    DiffGainDown,
    SwapAB,
    ZoomFit,
    ZoomOneToOne,
    ZoomIn,
    ZoomOut,
    PixelValues,
    PixelValueMode,
    InfoOverlay,
    Fullscreen,
    ExitFullscreen,
    SmoothScaling,
    Legend,
    LegendText,
    Count
};

// How auto-repeated key events are handled.
enum class Repeat {
    None,    // ignored (toggles)
    Gated,   // accepted only once the previous request is on screen: no backlog when released
    Always,  // every repeat counts (zoom)
};

struct ActionInfo {
    const char* id;     // settings key
    const char* label;  // shown in menus / settings
    Repeat repeat;
};

// Key bindings. Matching uses key codes, never the produced text, so layouts and IMEs (Hangul
// mode) do not matter; Shift is ignored for symbol keys because it already changes the code
// (Shift+, arrives as '<').
class KeyMap {
public:
    static KeyMap defaults();
    static const ActionInfo& info(Action a);
    static QKeyCombination normalize(int key, Qt::KeyboardModifiers mods);

    std::optional<Action> match(int key, Qt::KeyboardModifiers mods) const;
    QList<QKeyCombination> keys(Action a) const { return m_keys[size_t(a)]; }
    void setKeys(Action a, const QList<QKeyCombination>& keys);
    QString text(Action a) const;  // "Ctrl+O, F11" (native text)

    void save(QSettings& s) const;
    void load(QSettings& s);  // missing entries keep their current bindings

private:
    std::array<QList<QKeyCombination>, size_t(Action::Count)> m_keys;
};

} // namespace scvp::ui
