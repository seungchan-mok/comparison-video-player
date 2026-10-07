#include "ui/MainWindow.h"

#include "engine/RawVideoSource.h"

#include "ui/ClipboardImage.h"
#include "ui/ControlBar.h"
#include "ui/DesktopIntegration.h"
#include "ui/ExportProgress.h"
#include "ui/HudOverlay.h"
#include "ui/KeyRouter.h"
#include "ui/LegendDialog.h"
#include "ui/OpenRawDialog.h"
#include "ui/PixelInspector.h"
#include "ui/Player.h"
#include "ui/Stage.h"
#include "ui/VideoView.h"

#include "ui/SettingsDialog.h"

#include "render/OffscreenView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QActionGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>

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
    , m_clipboard(new ClipboardImage(this))
    , m_exportProgress(new ExportProgress(this))
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
    connect(m_player, &Player::mediaChanged, this, [this] { m_exportAction->setEnabled(m_player->slotOpen(0) || m_player->slotOpen(1)); });
    connect(m_clipboard, &ClipboardImage::copied, this, [this](QSize size) {
        m_hud->showMessage(tr("View copied to the clipboard (%1 x %2)").arg(size.width()).arg(size.height()));
        emit viewCopied(size, m_copyFactor);
    });
    updateTitle();
}

MainWindow::~MainWindow() = default;

bool MainWindow::openFile(int slot, const QString& path, bool askRaw, const std::optional<RawVideoParams>& raw)
{
    InputOptions opts;
    if (raw) {
        opts.raw = raw;
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
    const QString before = m_player->slotOpen(slot) ? QString::fromStdString(m_player->engine().info(slot).path) : QString();
    QString err;
    if (!m_player->open(slot, path, opts, &err)) {
        // Also for whoever started SCVP from a script and cannot see the dialog.
        std::fprintf(stderr, "scvp: cannot open %s: %s\n", qPrintable(path), qPrintable(err));
        QMessageBox::warning(this, tr("Cannot open file"), tr("%1\n\n%2").arg(path, err));
        return false;
    }
    // The legend text named the file that was here: another file starts from its own name.
    if (QFileInfo(before) != QFileInfo(path))
        m_hud->setLegendText(slot, QString());
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
        syncColorActions();
        const QString legendA = m_hud->legendText(0);  // names follow their videos
        m_hud->setLegendText(0, m_hud->legendText(1));
        m_hud->setLegendText(1, legendA);
        updateTitle();
        break;
    }
    case Action::ViewCycle:
    case Action::ViewSideBySide:
    case Action::ViewWipe:
    case Action::ViewFlip:
    case Action::ViewDifference:
    case Action::DiffGainUp:
    case Action::DiffGainDown: {
        const render::CompareSettings before = m_view->compare();
        const render::CompareSettings after = nextCompare(before, a, m_view->layout().orientation);
        setCompare(after);
        // Flipping: say clearly which video is on screen now.
        const bool flipped = after.mode == render::CompareMode::Flip &&
                             (before.mode != render::CompareMode::Flip || before.flipShowsB != after.flipShowsB);
        if (flipped && m_player->slotOpen(0) && m_player->slotOpen(1))
            m_hud->flashSlot(after.flipShowsB ? 1 : 0);
        break;
    }
    case Action::Legend: setLegendVisible(!m_hud->legendVisible()); break;
    case Action::LegendText: editLegendText(); break;
    case Action::CopyView: copyView(); break;
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

void MainWindow::setLegendVisible(bool on)
{
    m_settings.legend = on;
    showLegend(on);
}

void MainWindow::showLegend(bool on)
{
    m_hud->setLegendVisible(on);
    syncViewActions();
}

void MainWindow::setLegendText(int slot, const QString& text)
{
    m_hud->setLegendText(slot, text);
    if (!text.isEmpty())
        showLegend(true);  // naming a video is asking to see the names
}

// What is on screen, sharper where the screen loses detail: the video area is drawn again at
// a resolution that shows a minified video 1:1 (VideoView::capture), and the overlay (legend,
// labels, info box, pixel values line) is painted over it at that resolution, not scaled up.
void MainWindow::copyView()
{
    if (!m_player->slotOpen(0) && !m_player->slotOpen(1))
        return;
    double factor = 1.0;
    QImage img = m_view->capture(&factor);
    if (img.isNull()) {
        m_hud->showMessage(tr("Could not capture the view"));
        return;
    }
    const double scale = m_view->devicePixelRatioF() * factor;  // image px per logical px
    img.setDevicePixelRatio(scale);
    {
        QPainter p(&img);
        m_hud->paintCapture(p);
    }
    img.setDevicePixelRatio(1.0);
    // The resolution says how large it was on screen (like a HiDPI screenshot), for programs
    // that size pictures by it.
    const int dpm = int(std::lround(m_view->logicalDpiX() * scale / 0.0254));
    img.setDotsPerMeterX(dpm);
    img.setDotsPerMeterY(dpm);
    m_copyFactor = factor;
    m_hud->showMessage(tr("Copying the view (%1 x %2)...").arg(img.width()).arg(img.height()), 10000);
    m_clipboard->copy(img);
}

void MainWindow::editLegendText()
{
    std::array<LegendDialog::Entry, 2> entries;
    for (int slot = 0; slot < Engine::kSlots; ++slot)
        entries[size_t(slot)] = {m_hud->legendText(slot), m_hud->fileName(slot), m_player->slotOpen(slot)};
    if (!entries[0].open && !entries[1].open)
        return;
    const std::optional<LegendDialog::Texts> texts = LegendDialog::ask(this, entries);
    if (!texts)
        return;
    bool named = false;
    for (int slot = 0; slot < Engine::kSlots; ++slot)
        if (entries[size_t(slot)].open) {
            setLegendText(slot, (*texts)[size_t(slot)]);
            named |= !(*texts)[size_t(slot)].isEmpty();
        }
    if (named)
        setLegendVisible(true);  // named here: keep the legend on in later runs, as the L key does
}

void MainWindow::setCompare(const render::CompareSettings& c)
{
    m_view->setCompare(c);
    m_hud->update();
}

void MainWindow::setColorOverrides(int slot, const render::ColorOverrides& o)
{
    m_view->setColorOverrides(slot, o);
    syncColorActions();
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
    m_exportAction = file->addAction(tr("&Export Comparison Video..."), this, &MainWindow::exportVideo);
    m_exportAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_E));
    m_exportAction->setEnabled(false);
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
    addActions({openA, openB, close, closeB, m_exportAction, settings, quit});

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    playerAction(edit, Action::CopyView);
    QAction* copy = edit->addAction(tr("Copy &pixel values"), this, [this] {
        const QString t = m_hud->pixelText();
        if (!t.isEmpty())
            QGuiApplication::clipboard()->setText(t);
    });
    copy->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_C));
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
    m_legendAction = playerAction(view, Action::Legend);
    m_legendAction->setCheckable(true);
    playerAction(view, Action::LegendText);
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
        m_matrixGroup[size_t(slot)] = mg;
        const std::pair<const char*, std::optional<AVColorSpace>> matrices[] = {
            {"Auto (metadata)", std::nullopt}, {"BT.601", AVCOL_SPC_BT470BG}, {"BT.709", AVCOL_SPC_BT709}, {"BT.2020", AVCOL_SPC_BT2020_NCL}};
        for (const auto& [name, cs] : matrices) {
            QAction* a = matrix->addAction(tr(name));
            a->setCheckable(true);
            a->setData(cs ? int(*cs) : -1);
            mg->addAction(a);
            connect(a, &QAction::triggered, this, [this, slot, cs = cs] {
                render::ColorOverrides o = m_view->colorOverrides(slot);
                o.matrix = cs;
                setColorOverrides(slot, o);
            });
        }
        QMenu* range = color->addMenu(tr("%1: range").arg(v));
        auto* rg = new QActionGroup(range);
        m_rangeGroup[size_t(slot)] = rg;
        const std::pair<const char*, std::optional<AVColorRange>> ranges[] = {
            {"Auto (metadata)", std::nullopt}, {"Limited (16-235)", AVCOL_RANGE_MPEG}, {"Full (0-255)", AVCOL_RANGE_JPEG}};
        for (const auto& [name, r] : ranges) {
            QAction* a = range->addAction(tr(name));
            a->setCheckable(true);
            a->setData(r ? int(*r) : -1);
            rg->addAction(a);
            connect(a, &QAction::triggered, this, [this, slot, r = r] {
                render::ColorOverrides o = m_view->colorOverrides(slot);
                o.range = r;
                setColorOverrides(slot, o);
            });
        }
    }
    syncColorActions();
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
    if (m_legendAction)
        m_legendAction->setChecked(m_hud->legendVisible());  // the command line may show it for this run only
}

// The checks follow the view (swapping A and B, the command line), not only the menu's own clicks.
void MainWindow::syncColorActions()
{
    for (int slot = 0; slot < 2; ++slot) {
        const render::ColorOverrides& o = m_view->colorOverrides(slot);
        if (QActionGroup* g = m_matrixGroup[size_t(slot)])
            for (QAction* a : g->actions())
                a->setChecked(a->data().toInt() == (o.matrix ? int(*o.matrix) : -1));
        if (QActionGroup* g = m_rangeGroup[size_t(slot)])
            for (QAction* a : g->actions())
                a->setChecked(a->data().toInt() == (o.range ? int(*o.range) : -1));
    }
}

void MainWindow::applySettings(const AppSettings& s)
{
    const bool hwChanged = s.hwdec != m_settings.hwdec;
    // Only a changed setting replaces a legend shown for this run (command line).
    const bool legendChanged = s.legend != m_settings.legend;
    m_settings = s;
    m_player->engine().setHardware(s.hwConfig());
    if (s.hwConfig().policy != HwPolicy::Off)
        HwDevices::instance().warmUp();
    m_player->engine().setMemoryBudget(s.cacheBytes());
    if (s.vramMb)
        m_player->engine().setGpuMemoryBudget(s.vramBytes());
    m_view->setSmoothScaling(s.smoothScaling);
    if (legendChanged)
        m_hud->setLegendVisible(s.legend);
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
    if (m_export) {
        if (QMessageBox::question(this, tr("Export running"), tr("An export is still running. Stop it and quit?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        m_export.reset();  // cancels, removes the partial output and waits for its threads
    }
    if (isFullScreen())
        toggleFullscreen();
    m_settings.geometry = saveGeometry();
    m_settings.save();
    QMainWindow::closeEvent(e);
}

// The export of the videos and the view as they are now. Full frames: the comparison mode with
// the videos 1:1 (scaled by the size option), labels as large as on a 1080-line screen. As on
// screen: what Ctrl+C would copy, as large as a copy is drawn, for every frame of the range.
std::optional<ExportSpec> MainWindow::exportSpec(const ExportOptions& o, QString* error)
{
    ExportSpec spec;
    Engine& engine = m_player->engine();
    for (int slot = 0; slot < Engine::kSlots; ++slot)
        if (m_player->slotOpen(slot))
            spec.sources[size_t(slot)] = engine.source(slot);
    if (!spec.sources[0] && !spec.sources[1]) {
        if (error)
            *error = tr("Open a video first.");
        return std::nullopt;
    }
    spec.hw = engine.hardware();
    PlaybackController& pc = m_player->controller();
    spec.mapper = pc.mapper();
    const int64_t length = pc.length();
    spec.first = std::clamp<int64_t>(o.first, 0, std::max<int64_t>(0, length - 1));
    spec.last = o.last < o.first ? length - 1 : std::min<int64_t>(o.last, length - 1);
    const double scale = std::clamp(o.scale, 1, 100) / 100.0;
    render::RenderState screen = m_view->renderState();
    double textScale = 1.0;
    if (o.picture == ExportPicture::Screen) {
        // As large as a copy (no shown video minified), then the size option.
        const double f = render::Renderer::captureFactor(screen, render::Renderer::kMaxCaptureSide, render::Renderer::kMaxCapturePixels) * scale;
        spec.view = render::Renderer::captureState(screen, f);
        textScale = screen.devicePixelRatio * f;
    } else {
        const VideoInfo info = engine.info(spec.sources[0] ? 0 : 1);
        const QSizeF image = info.width > 0 ? QSizeF(info.width, info.height) : screen.view.imageSize();
        spec.view = render::OffscreenView::fullFrameState(screen, image, spec.sources[0] != nullptr, spec.sources[1] != nullptr, scale);
        textScale = std::max(1.0, image.height() * scale / 1080.0);
    }
    spec.view.frames = {};
    spec.labels = o.labels;
    spec.legend = m_hud->legendVisible();
    spec.frameInfo = o.frameInfo;
    spec.scene = m_hud->scene();
    spec.scene.dpr = textScale;
    spec.mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    spec.encoder.format = o.format;
    spec.encoder.encoder = o.encoder.toStdString();
    spec.encoder.quality = o.quality;
    spec.encoder.frameRate = av_d2q(spec.mapper.fps(), 1001000);
    spec.path = o.path.toUtf8().toStdString();
    return spec;
}

bool MainWindow::startExport(const ExportOptions& o, QString* error)
{
    QString err;
    std::optional<ExportSpec> spec;
    if (m_export)
        err = tr("An export is already running.");
    else if (o.path.isEmpty())
        err = tr("No file to export to.");
    else
        spec = exportSpec(o, &err);
    // Frame numbers are final only once every video is indexed.
    for (int slot = 0; spec && slot < Engine::kSlots; ++slot)
        if (spec->sources[size_t(slot)] && !spec->sources[size_t(slot)]->indexComplete()) {
            err = tr("The videos are still being indexed.");
            spec.reset();
        }
    if (!spec) {
        if (error)
            *error = err;
        return false;
    }
    m_export = std::make_unique<ExportJob>(std::move(*spec));
    connect(m_export.get(), &ExportJob::finished, this, &MainWindow::exportDone);
    m_exportProgress->start(m_export.get(), o.path);
    m_export->start();
    return true;
}

void MainWindow::exportDone(bool ok, const QString& message)
{
    m_lastExport = m_export->stats();
    m_exportProgress->finish(ok, message, m_lastExport);
    const QString name = QFileInfo(QString::fromStdString(m_export->spec().path)).fileName();
    m_hud->showMessage(ok                     ? tr("Exported %1").arg(name)
                       : m_lastExport.cancelled ? tr("Export cancelled")
                                                : tr("Export failed: %1").arg(message),
                       4000);
    m_export.release()->deleteLater();  // its threads are done; it is still emitting
    emit exportFinished(ok, message);
}

void MainWindow::exportVideo()
{
    if (m_export) {
        m_exportProgress->show();
        m_exportProgress->raise();
        return;
    }
    if (!m_player->slotOpen(0) && !m_player->slotOpen(1))
        return;
    ExportOptions initial = ExportOptions::load();
    QString dir = initial.lastDir();
    if (dir.isEmpty())
        dir = m_settings.lastDir.isEmpty() ? QDir::homePath() : m_settings.lastDir;
    QStringList names;
    for (int slot = 0; slot < Engine::kSlots; ++slot)
        if (m_player->slotOpen(slot))
            names << QFileInfo(QString::fromStdString(m_player->engine().info(slot).path)).completeBaseName();
    initial.path = QDir(dir).filePath(names.join(QStringLiteral("_vs_")) + QLatin1Char('.') + QLatin1String(VideoEncoder::extension(initial.format)));
    auto timeline = [this] {
        ExportTimeline t;
        PlaybackController& pc = m_player->controller();
        t.length = pc.length();
        for (int slot = 0; slot < Engine::kSlots; ++slot)
            if (m_player->slotOpen(slot)) {
                const SlotStats st = m_player->engine().stats(slot);
                t.final = t.final && st.indexComplete;
                t.indexProgress = std::min(t.indexProgress, st.indexProgress);
            }
        t.hasLoop = pc.hasLoop();
        t.loopFirst = pc.loopFirst();
        t.loopLast = pc.loopLast();
        t.fps = pc.frameRate();
        return t;
    };
    auto size = [this](const ExportOptions& o) {
        const std::optional<ExportSpec> s = exportSpec(o, nullptr);
        return s ? s->view.framebufferSize : QSize();
    };
    const std::optional<ExportOptions> chosen = ExportDialog::ask(this, initial, timeline, size);
    if (!chosen)
        return;
    chosen->save();
    QString err;
    if (!startExport(*chosen, &err))
        QMessageBox::warning(this, tr("Cannot export"), err);
}

} // namespace scvp::ui
