#include "ui/KeyMap.h"

#include <QKeySequence>
#include <QSettings>
#include <QStringList>

namespace scvp::ui {
namespace {

const std::array<ActionInfo, size_t(Action::Count)> kInfo = {{
    {"play_pause", "Play / pause", Repeat::None},
    {"step_back", "Previous frame", Repeat::Gated},
    {"step_forward", "Next frame", Repeat::Gated},
    {"jump_back", "Jump back", Repeat::Gated},
    {"jump_forward", "Jump forward", Repeat::Gated},
    {"jump_back_large", "Large jump back", Repeat::Gated},
    {"jump_forward_large", "Large jump forward", Repeat::Gated},
    {"offset_minus", "B offset -1", Repeat::Gated},
    {"offset_plus", "B offset +1", Repeat::Gated},
    {"go_start", "Go to start", Repeat::None},
    {"go_end", "Go to end", Repeat::None},
    {"loop_in", "Set loop start", Repeat::None},
    {"loop_out", "Set loop end", Repeat::None},
    {"loop_toggle", "Loop on/off", Repeat::None},
    {"loop_clear", "Clear loop", Repeat::None},
    {"view_cycle", "Next view mode", Repeat::None},
    {"view_side_by_side", "Side by side / top-bottom", Repeat::None},
    {"view_wipe", "Wipe", Repeat::None},
    {"view_flip", "Flip A/B", Repeat::None},
    {"view_difference", "Difference", Repeat::None},
    {"diff_gain_up", "Difference gain x2", Repeat::Always},
    {"diff_gain_down", "Difference gain /2", Repeat::Always},
    {"swap_ab", "Swap A and B", Repeat::None},
    {"zoom_fit", "Fit to window", Repeat::None},
    {"zoom_1to1", "1:1 pixels", Repeat::None},
    {"zoom_in", "Zoom in", Repeat::Always},
    {"zoom_out", "Zoom out", Repeat::Always},
    {"pixel_values", "Pixel values", Repeat::None},
    {"pixel_value_mode", "RGB / YUV codes", Repeat::None},
    {"info_overlay", "Info overlay", Repeat::None},
    {"fullscreen", "Fullscreen", Repeat::None},
    {"exit_fullscreen", "Exit fullscreen", Repeat::None},
}};

bool isSymbolKey(int key)
{
    // Printable keys whose code already reflects Shift (',' vs '<'); letters keep Shift.
    return key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde && !(key >= Qt::Key_A && key <= Qt::Key_Z);
}

} // namespace

const ActionInfo& KeyMap::info(Action a) { return kInfo[size_t(a)]; }

QKeyCombination KeyMap::normalize(int key, Qt::KeyboardModifiers mods)
{
    mods &= ~Qt::KeypadModifier;  // macOS arrows and the numpad carry it
    mods &= Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    if (isSymbolKey(key))
        mods &= ~Qt::ShiftModifier;
    if (key == Qt::Key_Backtab) {  // Shift+Tab
        key = Qt::Key_Tab;
        mods |= Qt::ShiftModifier;
    }
    return QKeyCombination(mods, Qt::Key(key));
}

KeyMap KeyMap::defaults()
{
    KeyMap m;
    auto set = [&](Action a, std::initializer_list<QKeyCombination> k) { m.m_keys[size_t(a)] = k; };
    const auto none = Qt::NoModifier;
    const auto shift = Qt::ShiftModifier;
    const auto ctrl = Qt::ControlModifier;  // Cmd on macOS
    set(Action::PlayPause, {QKeyCombination(none, Qt::Key_Space)});
    set(Action::StepBack, {QKeyCombination(none, Qt::Key_Comma), QKeyCombination(none, Qt::Key_Back)});
    set(Action::StepForward, {QKeyCombination(none, Qt::Key_Period), QKeyCombination(none, Qt::Key_Forward)});
    set(Action::JumpBack, {QKeyCombination(none, Qt::Key_Left)});
    set(Action::JumpForward, {QKeyCombination(none, Qt::Key_Right)});
    set(Action::JumpBackLarge, {QKeyCombination(none, Qt::Key_Down)});
    set(Action::JumpForwardLarge, {QKeyCombination(none, Qt::Key_Up)});
    set(Action::OffsetMinus, {QKeyCombination(none, Qt::Key_Less)});
    set(Action::OffsetPlus, {QKeyCombination(none, Qt::Key_Greater)});
    set(Action::GoToStart, {QKeyCombination(none, Qt::Key_Home)});
    set(Action::GoToEnd, {QKeyCombination(none, Qt::Key_End)});
    set(Action::LoopIn, {QKeyCombination(none, Qt::Key_BracketLeft)});
    set(Action::LoopOut, {QKeyCombination(none, Qt::Key_BracketRight)});
    set(Action::LoopToggle, {QKeyCombination(none, Qt::Key_L)});
    set(Action::LoopClear, {QKeyCombination(shift, Qt::Key_L)});
    set(Action::ViewCycle, {QKeyCombination(none, Qt::Key_V)});
    set(Action::ViewSideBySide, {QKeyCombination(none, Qt::Key_S)});
    set(Action::ViewWipe, {QKeyCombination(none, Qt::Key_W)});
    set(Action::ViewFlip, {QKeyCombination(none, Qt::Key_Tab), QKeyCombination(none, Qt::Key_QuoteLeft)});
    set(Action::ViewDifference, {QKeyCombination(none, Qt::Key_D)});
    set(Action::DiffGainUp, {QKeyCombination(none, Qt::Key_G)});
    set(Action::DiffGainDown, {QKeyCombination(shift, Qt::Key_G)});
    set(Action::SwapAB, {QKeyCombination(none, Qt::Key_X)});
    set(Action::ZoomFit, {QKeyCombination(none, Qt::Key_0)});
    set(Action::ZoomOneToOne, {QKeyCombination(none, Qt::Key_1)});
    set(Action::ZoomIn, {QKeyCombination(none, Qt::Key_Equal), QKeyCombination(none, Qt::Key_Plus)});
    set(Action::ZoomOut, {QKeyCombination(none, Qt::Key_Minus)});
    set(Action::PixelValues, {QKeyCombination(none, Qt::Key_P)});
    set(Action::PixelValueMode, {QKeyCombination(none, Qt::Key_Y)});
    set(Action::InfoOverlay, {QKeyCombination(none, Qt::Key_I)});
#if defined(Q_OS_MACOS)
    // Ctrl+Cmd+F (Qt: Meta = Ctrl key, Control = Cmd key); F11 is Show Desktop on macOS.
    set(Action::Fullscreen, {QKeyCombination(none, Qt::Key_F), QKeyCombination(Qt::MetaModifier | ctrl, Qt::Key_F)});
#else
    set(Action::Fullscreen, {QKeyCombination(none, Qt::Key_F), QKeyCombination(none, Qt::Key_F11)});
#endif
    set(Action::ExitFullscreen, {QKeyCombination(none, Qt::Key_Escape)});
    (void)ctrl;
    return m;
}

std::optional<Action> KeyMap::match(int key, Qt::KeyboardModifiers mods) const
{
    const QKeyCombination k = normalize(key, mods);
    for (size_t a = 0; a < m_keys.size(); ++a)
        for (const QKeyCombination& c : m_keys[a])
            if (c == k)
                return Action(a);
    return std::nullopt;
}

void KeyMap::setKeys(Action a, const QList<QKeyCombination>& keys) { m_keys[size_t(a)] = keys; }

QString KeyMap::text(Action a) const
{
    QStringList parts;
    for (const QKeyCombination& c : m_keys[size_t(a)])
        parts << QKeySequence(c).toString(QKeySequence::NativeText);
    return parts.join(QStringLiteral(", "));
}

void KeyMap::save(QSettings& s) const
{
    s.beginGroup(QStringLiteral("keys"));
    for (size_t a = 0; a < m_keys.size(); ++a) {
        QStringList parts;
        for (const QKeyCombination& c : m_keys[a])
            parts << QKeySequence(c).toString(QKeySequence::PortableText);
        s.setValue(QLatin1String(kInfo[a].id), parts.join(QStringLiteral("; ")));
    }
    s.endGroup();
}

void KeyMap::load(QSettings& s)
{
    s.beginGroup(QStringLiteral("keys"));
    for (size_t a = 0; a < m_keys.size(); ++a) {
        const QString v = s.value(QLatin1String(kInfo[a].id)).toString();
        if (!s.contains(QLatin1String(kInfo[a].id)))
            continue;
        QList<QKeyCombination> keys;
        for (const QString& part : v.split(QStringLiteral("; "), Qt::SkipEmptyParts)) {
            const QKeySequence seq = QKeySequence::fromString(part, QKeySequence::PortableText);
            if (!seq.isEmpty())
                keys << seq[0];
        }
        m_keys[a] = keys;
    }
    s.endGroup();
}

} // namespace scvp::ui
