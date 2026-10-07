#pragma once

#include "ui/KeyMap.h"
#include "ui/Settings.h"

#include "render/ColorParams.h"
#include "render/CompareLayout.h"

#include <QMainWindow>

#include <array>
#include <optional>

class QActionGroup;

namespace scvp::ui {

class ClipboardImage;
class ControlBar;
class HudOverlay;
class KeyRouter;
class Stage;
class Player;
class VideoView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Raw video files without their geometry in the name ask for it (askRaw forces the dialog);
    // `raw` opens the file as that raw video (command line --raw).
    bool openFile(int slot, const QString& path, bool askRaw = false, const std::optional<RawVideoParams>& raw = std::nullopt);
    Player* player() const { return m_player; }
    VideoView* view() const { return m_view; }
    HudOverlay* hud() const { return m_hud; }

    void handleAction(Action a, bool autoRepeat);
    void toggleFullscreen();
    void openDialog(int slot, bool raw = false);
    void showSettings();
    void applySettings(const AppSettings& s);
    void overrideCacheBudget(size_t bytes);  // command line: this run only
    void overrideHardware(const HwConfig& hw);
    void overrideGpuBudget(size_t bytes);
    void reopenAll();  // after a decoding change: same files, same frame
    void setCompare(const render::CompareSettings& c);
    // Colour interpretation of one video; the Color menu follows.
    void setColorOverrides(int slot, const render::ColorOverrides& o);
    QString inspectorText() const;
    // The legend shows `text` for this video instead of its file name ("" = the file name).
    // Naming a video shows the legend for this run (the saved setting stays).
    void setLegendText(int slot, const QString& text);
    // The legend for this run only; the L key and the legend dialog also save it.
    void showLegend(bool on);
    // The view as it looks, as an image on the clipboard (Ctrl+C).
    void copyView();
    void offerDefaultPlayer();

signals:
    // The copy is on the clipboard; factor: its size over the screen's device pixels.
    void viewCopied(QSize size, double factor);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    void updateTitle();
    void createMenus();
    QAction* playerAction(QMenu* menu, Action a);
    void createColorMenu();
    void syncViewActions();
    void syncColorActions();
    void setLegendVisible(bool on);  // and save it
    void editLegendText();

    VideoView* m_view;
    Player* m_player;
    ControlBar* m_controls;
    Stage* m_stage;
    HudOverlay* m_hud;
    KeyRouter* m_keys;
    AppSettings m_settings;
    QList<QPair<QAction*, Action>> m_menuActions;
    QAction* m_smoothAction = nullptr;
    QAction* m_legendAction = nullptr;
    // Color menu per video; each action's data is the AVColorSpace / AVColorRange, -1 = auto.
    std::array<QActionGroup*, 2> m_matrixGroup{};
    std::array<QActionGroup*, 2> m_rangeGroup{};
    ClipboardImage* m_clipboard;
    double m_copyFactor = 1.0;
};

} // namespace scvp::ui
