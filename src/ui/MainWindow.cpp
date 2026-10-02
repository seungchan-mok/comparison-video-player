#include "ui/MainWindow.h"

#include "ui/Player.h"
#include "ui/VideoView.h"

#include <QFileInfo>
#include <QKeyEvent>
#include <QMessageBox>

namespace scvp::ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_view(new VideoView(this))
    , m_player(new Player(m_view, this))
{
    setCentralWidget(m_view);
    resize(1280, 760);
    connect(m_player, &Player::positionChanged, this, [this] { updateTitle(); });
    connect(m_player, &Player::mediaChanged, this, [this] { updateTitle(); });
    connect(m_view, &VideoView::doubleClicked, this, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
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

// Temporary bindings until the KeyRouter lands.
void MainWindow::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Space: m_player->togglePlay(); break;
    case Qt::Key_Period: m_player->step(1); break;
    case Qt::Key_Comma: m_player->step(-1); break;
    case Qt::Key_Right: m_player->jump(1, false); break;
    case Qt::Key_Left: m_player->jump(-1, false); break;
    case Qt::Key_Up: m_player->jump(1, true); break;
    case Qt::Key_Down: m_player->jump(-1, true); break;
    case Qt::Key_0: m_view->fitToWindow(); break;
    case Qt::Key_1: m_view->oneToOne(); break;
    case Qt::Key_F: isFullScreen() ? showNormal() : showFullScreen(); break;
    case Qt::Key_Escape: if (isFullScreen()) showNormal(); break;
    default: QMainWindow::keyPressEvent(e); return;
    }
    e->accept();
}

} // namespace scvp::ui
