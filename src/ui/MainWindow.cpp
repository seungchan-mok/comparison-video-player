#include "ui/MainWindow.h"

#include "engine/RawVideoSource.h"

#include "ui/ControlBar.h"
#include "ui/DesktopIntegration.h"
#include "ui/HudOverlay.h"
#include "ui/KeyRouter.h"
#include "ui/OpenRawDialog.h"
#include "ui/PixelInspector.h"
#include "ui/Player.h"
#include "ui/Stage.h"
#include "ui/VideoView.h"

#include "ui/SettingsDialog.h"

#include <algorithm>

#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>

namespace scvp::ui {

render::CompareSettings nextCompare(render::CompareSettings c, Action a, render::SplitOrientation shown);

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
    connect(m_view, &VideoView::stepRequested, this, [this](int n) { handleAction(n < 0 ? Action::StepBack : Action::StepForward, false); });
    connect(m_view, &VideoView::compareChanged, m_hud, qOverload<>(&QWidget::update));
    connect(m_view, &VideoView::sampleChanged, this, [this](const render::PixelSample& s) { m_hud->setPixelText(formatPixelSample(s)); });
    connect(m_player, &Player::mediaChanged, m_hud, qOverload<>(&QWidget::update));
    updateTitle();
}

MainWindow::~MainWindow() = default;

bool MainWindow::openFile(int slot, const QString& path, bool askRaw)
{
    InputOptions opts;
    if (m_rawOverride) {
        opts.raw = m_rawOverride;
    } else if (askRaw || isRawVideoExtension(path.toUtf8().toStdString())) {
        RawVideoParams p = parseRawVideoName(path.toUtf8().toStdString());
        if (askRaw || !p.valid()) {
            const std::optional<RawVideoParams> asked = OpenRawDialog::ask(this, path, p);
            if (!asked)
                return false;
            p = *asked;
        }
        opts.raw = p;
    }
    QString err;
    if (!m_player->open(slot, path, opts, &err)) {
        QMessageBox::warning(this, tr("Cannot open file"), tr("%1\n\n%2").arg(path, err));
        return false;
    }
    updateTitle();
    return true;
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("SCVP");
    QStringList names;
    for (int slot = 0; slot < Engine::kSlots; ++slot)
        if (m_player->isOpen(slot))
            names << QFileInfo(QString::fromStdString(m_player->engine().info(slot).path)).fileName();
    if (!names.isEmpty())
        t = QStringLiteral("%1 — %2 / %3 — SCVP")
                .arg(names.join(QStringLiteral(" | ")))
                .arg(m_player->controller().position())
                .arg(m_player->controller().length());
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
    case Action::OffsetMinus: m_player->setOffset(m_player->offset() - 1); break;
    case Action::OffsetPlus: m_player->setOffset(m_player->offset() + 1); break;
    case Action::SwapAB: {
        m_player->swapSlots();
        const render::ColorOverrides a = m_view->colorOverrides(0);
        m_view->setColorOverrides(0, m_view->colorOverrides(1));
        m_view->setColorOverrides(1, a);
        updateTitle();
        break;
    }
    case Action::ViewCycle:
    case Action::ViewSideBySide:
    case Action::ViewWipe:
    case Action::ViewFlip:
    case Action::ViewDifference:
    case Action::DiffGainUp:
    case Action::DiffGainDown: setCompare(nextCompare(m_view->compare(), a, m_view->layout().orientation)); break;
    case Action::SmoothScaling:
        m_settings.smoothScaling = !m_settings.smoothScaling;
        m_view->setSmoothScaling(m_settings.smoothScaling);
        syncViewActions();
        break;
    case Action::PixelValues: {
        render::PixelOverlaySettings o = m_view->overlay();
        o.values = !o.values;
        m_view->setOverlay(o);
        break;
    }
    case Action::PixelValueMode: {
        render::PixelOverlaySettings o = m_view->overlay();
        o.mode = o.mode == render::ValueMode::Rgb ? render::ValueMode::Codes : render::ValueMode::Rgb;
        o.values = true;
        m_view->setOverlay(o);
        break;
    }
    default: break;
    }
}

render::CompareSettings nextCompare(render::CompareSettings c, Action a, render::SplitOrientation shown)
{
    using render::CompareMode;
    switch (a) {
    case Action::ViewCycle:
        c.mode = c.mode == CompareMode::Split ? CompareMode::Wipe
                 : c.mode == CompareMode::Wipe ? CompareMode::Flip
                 : c.mode == CompareMode::Flip ? CompareMode::Difference
                                               : CompareMode::Split;
        break;
    case Action::ViewSideBySide:
        // Again in split mode: switch between side by side and top/bottom.
        if (c.mode == CompareMode::Split)
            c.orientation = shown == render::SplitOrientation::SideBySide ? render::SplitOrientation::TopBottom
                                                                          : render::SplitOrientation::SideBySide;
        c.mode = CompareMode::Split;
        break;
    case Action::ViewWipe: c.mode = CompareMode::Wipe; break;
    case Action::ViewFlip:
        // Flip toggles which video is shown; entering it from another mode shows B first.
        c.flipShowsB = c.mode == CompareMode::Flip ? !c.flipShowsB : true;
        c.mode = CompareMode::Flip;
        break;
    case Action::ViewDifference: c.mode = CompareMode::Difference; break;
    case Action::DiffGainUp: c.diffGain = std::min(256.0, c.diffGain * 2); break;
    case Action::DiffGainDown: c.diffGain = std::max(1.0, c.diffGain / 2); break;
    default: break;
    }
    return c;
}

// Linux: SCVP opens videos from the file manager once it is the default for their types.
void MainWindow::offerDefaultPlayer()
{
    if (desktop::installedEntry().isEmpty()) {
        QMessageBox::information(this, tr("Default video player"),
                                 tr("SCVP is not installed for this user yet.\n\n"
                                    "Run install.sh in the SCVP folder once (or double-click it and choose Run): it adds SCVP "
                                    "to the applications menu and to \"Open With\" for videos."));
        return;
    }
    if (desktop::isDefault()) {
        QMessageBox::information(this, tr("Default video player"), tr("SCVP already opens videos by default."));
        return;
    }
    const QString current = desktop::applicationName(desktop::defaultFor(QStringLiteral("video/mp4")));
    const QString text = current.isEmpty()
                             ? tr("Open MP4, MKV, MOV, AVI, WebM, TS, raw H.264/HEVC/YUV and the other video types SCVP "
                                  "supports with SCVP by default?")
                             : tr("Videos currently open with %1.\n\nOpen MP4, MKV, MOV, AVI, WebM, TS, raw H.264/HEVC/YUV and "
                                  "the other video types SCVP supports with SCVP by default instead?\n\n"
                                  "(This can be changed back in Settings > Apps > Default Apps.)")
                                   .arg(current);
    if (QMessageBox::question(this, tr("Default video player"), text) != QMessageBox::Yes)
        return;
    QString err;
    if (!desktop::setAsDefault(&err))
        QMessageBox::warning(this, tr("Default video player"), tr("Could not change the default application.\n\n%1").arg(err));
    else
        QMessageBox::information(this, tr("Default video player"), tr("Videos now open with SCVP."));
}

QString MainWindow::inspectorText() const { return m_hud->pixelText(); }

void MainWindow::setCompare(const render::CompareSettings& c)
{
    m_view->setCompare(c);
    m_hud->update();
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
    QAction* openA = file->addAction(tr("&Open A..."), this, [this] { openDialog(0); });
    openA->setShortcut(QKeySequence::Open);
    QAction* openB = file->addAction(tr("Open &B (compare)..."), this, [this] { openDialog(1); });
    openB->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_O));
    file->addAction(tr("Open image sequence folder..."), this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Image sequence folder"), m_settings.lastDir);
        if (!dir.isEmpty()) {
            m_settings.lastDir = dir;
            openFile(m_player->slotOpen(0) ? 1 : 0, dir);
        }
    });
    file->addAction(tr("Open raw video as A..."), this, [this] { openDialog(0, true); });
    file->addAction(tr("Open raw video as B..."), this, [this] { openDialog(1, true); });
    file->addSeparator();
    QAction* close = file->addAction(tr("&Close A"), this, [this] { m_player->close(0); updateTitle(); });
    close->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_W));
    QAction* closeB = file->addAction(tr("Close B"), this, [this] { m_player->close(1); updateTitle(); });
    closeB->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_W));
    file->addSeparator();
    if (desktop::supported())
        file->addAction(tr("Make SCVP the Default Video Player..."), this, &MainWindow::offerDefaultPlayer);
    QAction* settings = file->addAction(tr("&Settings..."), this, &MainWindow::showSettings);
    settings->setShortcut(QKeySequence::Preferences);
    settings->setMenuRole(QAction::PreferencesRole);
    QAction* quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);
    quit->setMenuRole(QAction::QuitRole);
    // App commands must also work while the menu bar is hidden (fullscreen).
    addActions({openA, openB, close, closeB, settings, quit});

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* copy = edit->addAction(tr("&Copy pixel values"), this, [this] {
        const QString t = m_hud->pixelText();
        if (!t.isEmpty())
            QGuiApplication::clipboard()->setText(t);
    });
    copy->setShortcut(QKeySequence::Copy);
    addAction(copy);

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
    m_smoothAction = playerAction(view, Action::SmoothScaling);
    m_smoothAction->setCheckable(true);
    m_smoothAction->setToolTip(tr("Off: exact pixels at every zoom, also when fitted to the window"));
    view->addSeparator();
    for (Action a : {Action::PixelValues, Action::PixelValueMode})
        playerAction(view, a);
    view->addSeparator();
    for (Action a : {Action::InfoOverlay, Action::Fullscreen})
        playerAction(view, a);

    createColorMenu();

    QMenu* compare = menuBar()->addMenu(tr("&Compare"));
    for (Action a : {Action::ViewCycle, Action::ViewSideBySide, Action::ViewWipe, Action::ViewFlip, Action::ViewDifference,
                     Action::DiffGainUp, Action::DiffGainDown})
        playerAction(compare, a);
    compare->addSeparator();
    for (Action a : {Action::OffsetMinus, Action::OffsetPlus, Action::SwapAB})
        playerAction(compare, a);
}

// Colour interpretation per video: metadata is often missing or wrong in camera and test files.
void MainWindow::createColorMenu()
{
    QMenu* color = menuBar()->addMenu(tr("C&olor"));
    for (int slot = 0; slot < 2; ++slot) {
        const QString v = slot == 0 ? QStringLiteral("A") : QStringLiteral("B");
        QMenu* matrix = color->addMenu(tr("%1: YUV matrix").arg(v));
        auto* mg = new QActionGroup(matrix);
        const std::pair<const char*, std::optional<AVColorSpace>> matrices[] = {
            {"Auto (metadata)", std::nullopt}, {"BT.601", AVCOL_SPC_BT470BG}, {"BT.709", AVCOL_SPC_BT709}, {"BT.2020", AVCOL_SPC_BT2020_NCL}};
        for (const auto& [name, cs] : matrices) {
            QAction* a = matrix->addAction(tr(name));
            a->setCheckable(true);
            a->setChecked(!cs);
            mg->addAction(a);
            connect(a, &QAction::triggered, this, [this, slot, cs = cs] {
                render::ColorOverrides o = m_view->colorOverrides(slot);
                o.matrix = cs;
                m_view->setColorOverrides(slot, o);
            });
        }
        QMenu* range = color->addMenu(tr("%1: range").arg(v));
        auto* rg = new QActionGroup(range);
        const std::pair<const char*, std::optional<AVColorRange>> ranges[] = {
            {"Auto (metadata)", std::nullopt}, {"Limited (16-235)", AVCOL_RANGE_MPEG}, {"Full (0-255)", AVCOL_RANGE_JPEG}};
        for (const auto& [name, r] : ranges) {
            QAction* a = range->addAction(tr(name));
            a->setCheckable(true);
            a->setChecked(!r);
            rg->addAction(a);
            connect(a, &QAction::triggered, this, [this, slot, r = r] {
                render::ColorOverrides o = m_view->colorOverrides(slot);
                o.range = r;
                m_view->setColorOverrides(slot, o);
            });
        }
    }
    color->addSeparator();
    QAction* bilinear = color->addAction(tr("Interpolate chroma (bilinear)"));
    bilinear->setCheckable(true);
    bilinear->setToolTip(tr("Off: each pixel uses the chroma sample covering it (exact codes)"));
    connect(bilinear, &QAction::toggled, this, [this](bool on) {
        for (int slot = 0; slot < 2; ++slot) {
            render::ColorOverrides o = m_view->colorOverrides(slot);
            o.chromaBilinear = on;
            m_view->setColorOverrides(slot, o);
        }
    });
}

void MainWindow::syncViewActions()
{
    if (m_smoothAction)
        m_smoothAction->setChecked(m_settings.smoothScaling);
}

void MainWindow::applySettings(const AppSettings& s)
{
    const bool hwChanged = s.hwdec != m_settings.hwdec;
    m_settings = s;
    m_player->engine().setHardware(s.hwConfig());
    if (s.hwConfig().policy != HwPolicy::Off)
        HwDevices::instance().warmUp();
    m_player->engine().setMemoryBudget(s.cacheBytes());
    if (s.vramMb)
        m_player->engine().setGpuMemoryBudget(s.vramBytes());
    m_view->setSmoothScaling(s.smoothScaling);
    syncViewActions();
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

void MainWindow::overrideGpuBudget(size_t bytes) { m_player->engine().setGpuMemoryBudget(bytes); }

void MainWindow::reopenAll()
{
    const int64_t pos = m_player->position();
    for (int slot = 0; slot < Engine::kSlots; ++slot) {
        if (!m_player->slotOpen(slot))
            continue;
        const QString path = QString::fromStdString(m_player->engine().info(slot).path);
        const InputOptions opts = m_player->inputOptions(slot);
        m_player->close(slot);
        QString err;
        m_player->open(slot, path, opts, &err);
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

void MainWindow::openDialog(int slot, bool raw)
{
    const QString filter = tr("Videos and images (*.mp4 *.mkv *.mov *.avi *.webm *.ts *.m2ts *.mts *.mpg *.mpeg *.m4v *.flv "
                              "*.wmv *.mxf *.y4m *.h264 *.264 *.h265 *.265 *.hevc *.ivf *.obu *.yuv *.png *.jpg *.jpeg *.tif *.tiff);;"
                              "All files (*)");
    const QString path = QFileDialog::getOpenFileName(this, tr("Open video"), m_settings.lastDir, filter);
    if (path.isEmpty())
        return;
    m_settings.lastDir = QFileInfo(path).absolutePath();
    openFile(slot, path, raw);
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

// One file: dropped on the left (or top) half it becomes A, on the other half B. Two files: A and B.
void MainWindow::dropEvent(QDropEvent* e)
{
    QStringList files;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (files.isEmpty())
        return;
    e->acceptProposedAction();
    m_settings.lastDir = QFileInfo(files.front()).absolutePath();
    if (files.size() >= 2) {
        openFile(0, files[0]);
        openFile(1, files[1]);
        return;
    }
    const QPointF at = m_view->mapFrom(this, e->position().toPoint());
    const bool vertical = m_view->compare().mode == render::CompareMode::Split &&
                          m_view->layout().orientation == render::SplitOrientation::TopBottom && m_player->slotOpen(1);
    const bool second = vertical ? at.y() > m_view->height() / 2.0 : at.x() > m_view->width() / 2.0;
    openFile(second && m_player->slotOpen(0) ? 1 : 0, files[0]);
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
