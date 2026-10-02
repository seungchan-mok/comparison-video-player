#include "ui/MainWindow.h"

#include "ui/ControlBar.h"
#include "ui/HudOverlay.h"
#include "ui/KeyRouter.h"
#include "ui/Player.h"
#include "ui/Stage.h"
#include "ui/VideoView.h"

#include "ui/SettingsDialog.h"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>

namespace scvp::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_view(new VideoView(this))
    , m_player(new Player(m_view, this))
    , m_controls(new ControlBar(m_player, this))
    , m_stage(new Stage(m_view, m_controls, this))
    , m_hud(new HudOverlay(m_player, m_view))
    , m_keys(new KeyRouter(this, m_view, KeyMap::defaults(), [this] { return m_player->controller().positionShown(); }, this))
{
    setCentralWidget(m_stage);
    setAcceptDrops(true);
    connect(m_player, &Player::stalledChanged, m_hud, &HudOverlay::setStalled);
    createMenus();
    applySettings(AppSettings::load());
    if (!m_settings.geometry.isEmpty())
        restoreGeometry(m_settings.geometry);
    connect(m_keys, &KeyRouter::triggered, this, &MainWindow::handleAction);
    resize(1280, 760);
    connect(m_player, &Player::positionChanged, this, [this] { updateTitle(); });
    connect(m_player, &Player::mediaChanged, this, [this] { updateTitle(); });
    connect(m_view, &VideoView::doubleClicked, this, &MainWindow::toggleFullscreen);
    updateTitle();
}

MainWindow::~MainWindow() = default;

bool MainWindow::openFile(int slot, const QString& path)
{
    QString err;
    if (!m_player->open(slot, path, &err)) {
        QMessageBox::warning(this, tr("Cannot open file"), tr("%1\n\n%2").arg(path, err));
        return false;
    }
    updateTitle();
    return true;
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("SCVP");
    if (m_player->isOpen(0)) {
        const auto info = m_player->engine().info(0);
        t = QStringLiteral("%1 — %2 / %3 — SCVP")
                .arg(QFileInfo(QString::fromStdString(info.path)).fileName())
                .arg(m_player->controller().position())
                .arg(m_player->controller().length());
    }
    setWindowTitle(t);
}

void MainWindow::toggleFullscreen()
{
    const bool on = !isFullScreen();
    m_stage->setFullscreenMode(on);
    menuBar()->setVisible(!on);
    on ? showFullScreen() : showNormal();
    m_view->setFocus();
}

void MainWindow::handleAction(Action a, bool)
{
    switch (a) {
    case Action::PlayPause: m_player->togglePlay(); break;
    case Action::StepBack: m_player->step(-1); break;
    case Action::StepForward: m_player->step(1); break;
    case Action::JumpBack: m_player->jump(-1, false); break;
    case Action::JumpForward: m_player->jump(1, false); break;
    case Action::JumpBackLarge: m_player->jump(-1, true); break;
    case Action::JumpForwardLarge: m_player->jump(1, true); break;
    case Action::GoToStart: m_player->seek(0); break;
    case Action::GoToEnd: m_player->seek(m_player->controller().length() - 1); break;
    case Action::LoopIn: m_player->setLoopStart(); break;
    case Action::LoopOut: m_player->setLoopEnd(); break;
    case Action::LoopToggle: m_player->toggleLoop(); break;
    case Action::LoopClear: m_player->clearLoop(); break;
    case Action::ZoomFit: m_view->fitToWindow(); break;
    case Action::ZoomOneToOne: m_view->oneToOne(); break;
    case Action::ZoomIn: m_view->zoomSteps(1); break;
    case Action::ZoomOut: m_view->zoomSteps(-1); break;
    case Action::Fullscreen: toggleFullscreen(); break;
    case Action::ExitFullscreen:
        if (isFullScreen())
            toggleFullscreen();
        break;
    case Action::InfoOverlay: m_hud->setInfoVisible(!m_hud->infoVisible()); break;
    default: break;  // comparison, offset and pixel-inspection actions arrive with their features
    }
}

QAction* MainWindow::playerAction(QMenu* menu, Action a)
{
    // Keys are shown, not bound: the KeyRouter owns them (a QAction shortcut would fire twice).
    auto* act = menu->addAction(QString());
    connect(act, &QAction::triggered, this, [this, a] { handleAction(a, false); });
    m_menuActions.append({act, a});
    return act;
}

void MainWindow::createMenus()
{
    QMenu* file = menuBar()->addMenu(tr("&File"));
    QAction* openA = file->addAction(tr("&Open..."), this, [this] { openDialog(0); });
    openA->setShortcut(QKeySequence::Open);
    file->addSeparator();
    QAction* close = file->addAction(tr("&Close"), this, [this] { m_player->close(0); updateTitle(); });
    close->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_W));
    file->addSeparator();
    QAction* settings = file->addAction(tr("&Settings..."), this, &MainWindow::showSettings);
    settings->setShortcut(QKeySequence::Preferences);
    settings->setMenuRole(QAction::PreferencesRole);
    QAction* quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    // App commands must also work while the menu bar is hidden (fullscreen).
    addActions({openA, close, settings, quit});

    QMenu* play = menuBar()->addMenu(tr("&Playback"));
    for (Action a : {Action::PlayPause, Action::StepForward, Action::StepBack, Action::JumpForward, Action::JumpBack,
                     Action::JumpForwardLarge, Action::JumpBackLarge, Action::GoToStart, Action::GoToEnd})
        playerAction(play, a);
    play->addSeparator();
    for (Action a : {Action::LoopIn, Action::LoopOut, Action::LoopToggle, Action::LoopClear})
        playerAction(play, a);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (Action a : {Action::ZoomFit, Action::ZoomOneToOne, Action::ZoomIn, Action::ZoomOut})
        playerAction(view, a);
    view->addSeparator();
    for (Action a : {Action::InfoOverlay, Action::Fullscreen})
        playerAction(view, a);
}

void MainWindow::applySettings(const AppSettings& s)
{
    const bool hwChanged = s.hwdec != m_settings.hwdec;
    m_settings = s;
    m_player->engine().setHardware(s.hwConfig());
    if (s.hwConfig().policy != HwPolicy::Off)
        HwDevices::instance().warmUp();
    m_player->engine().setMemoryBudget(s.cacheBytes());
    m_player->setJumpSettings(s.jumps);
    m_controls->setJumpSettings(s.jumps);
    m_keys->setKeyMap(s.keys);
    for (const auto& [act, a] : m_menuActions) {
        const QString keys = s.keys.text(a);
        act->setText(keys.isEmpty() ? tr(KeyMap::info(a).label) : tr(KeyMap::info(a).label) + QLatin1Char('\t') + keys);
    }
    if (hwChanged)
        reopenAll();
}

void MainWindow::overrideHardware(const HwConfig& hw) { m_player->engine().setHardware(hw); }

void MainWindow::reopenAll()
{
    const int64_t pos = m_player->position();
    for (int slot = 0; slot < Engine::kSlots; ++slot) {
        if (!m_player->slotOpen(slot))
            continue;
        const QString path = QString::fromStdString(m_player->engine().info(slot).path);
        m_player->close(slot);
        QString err;
        m_player->open(slot, path, &err);
    }
    m_player->seek(pos);
}

void MainWindow::overrideCacheBudget(size_t bytes) { m_player->engine().setMemoryBudget(bytes); }

void MainWindow::showSettings()
{
    auto frameInfo = [this]() -> std::pair<size_t, int> {
        if (!m_player->slotOpen(0))
            return {0, 0};
        const FramePtr f = m_player->engine().frame(0, int32_t(m_player->position()));
        const FrameIndexPtr idx = m_player->engine().index(0);
        return {f ? f->bytes : 0, idx ? int(idx->meanGopLength() + 0.5) : 0};
    };
    SettingsDialog dlg(m_settings, frameInfo, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    AppSettings s = dlg.result();
    s.lastDir = m_settings.lastDir;
    s.geometry = m_settings.geometry;
    applySettings(s);
    s.save();
}

void MainWindow::openDialog(int slot)
{
    const QString filter = tr("Videos and images (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.m2ts *.mts *.mpg *.mpeg *.m4v *.flv "
                              "*.wmv *.mxf *.y4m *.h264 *.264 *.h265 *.265 *.hevc *.ivf *.obu *.yuv *.png *.jpg *.jpeg *.tif *.tiff);;"
                              "All files (*)");
    const QString path = QFileDialog::getOpenFileName(this, tr("Open video"), m_settings.lastDir, filter);
    if (path.isEmpty())
        return;
    m_settings.lastDir = QFileInfo(path).absolutePath();
    openFile(slot, path);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) {
                e->acceptProposedAction();
                return;
            }
}

void MainWindow::dropEvent(QDropEvent* e)
{
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) {
            openFile(0, u.toLocalFile());
            m_settings.lastDir = QFileInfo(u.toLocalFile()).absolutePath();
            e->acceptProposedAction();
            return;
        }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (isFullScreen())
        toggleFullscreen();
    m_settings.geometry = saveGeometry();
    m_settings.save();
    QMainWindow::closeEvent(e);
}

} // namespace scvp::ui
