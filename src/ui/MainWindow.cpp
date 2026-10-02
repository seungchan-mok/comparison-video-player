#include "ui/MainWindow.h"

#include "ui/KeyRouter.h"
#include "ui/Player.h"
#include "ui/VideoView.h"

#include <QFileInfo>
#include <QMessageBox>

namespace scvp::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_view(new VideoView(this))
    , m_player(new Player(m_view, this))
    , m_keys(new KeyRouter(this, m_view, KeyMap::defaults(), [this] { return m_player->controller().positionShown(); }, this))
{
    setCentralWidget(m_view);
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

void MainWindow::toggleFullscreen() { isFullScreen() ? showNormal() : showFullScreen(); }

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
    case Action::LoopIn: m_player->controller().setLoopStart(); m_player->refresh(); break;
    case Action::LoopOut: m_player->controller().setLoopEnd(); m_player->refresh(); break;
    case Action::LoopToggle: m_player->controller().toggleLoop(); m_player->refresh(); break;
    case Action::LoopClear: m_player->controller().clearLoop(); m_player->refresh(); break;
    case Action::ZoomFit: m_view->fitToWindow(); break;
    case Action::ZoomOneToOne: m_view->oneToOne(); break;
    case Action::ZoomIn: m_view->zoomSteps(1); break;
    case Action::ZoomOut: m_view->zoomSteps(-1); break;
    case Action::Fullscreen: toggleFullscreen(); break;
    case Action::ExitFullscreen:
        if (isFullScreen())
            showNormal();
        break;
    default: break;  // comparison, offset and pixel-inspection actions arrive with their features
    }
}

} // namespace scvp::ui
