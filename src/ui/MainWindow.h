#pragma once

#include <QMainWindow>

namespace scvp::ui {

class Player;
class VideoView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openFile(int slot, const QString& path);
    Player* player() const { return m_player; }
    VideoView* view() const { return m_view; }

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    void updateTitle();

    VideoView* m_view;
    Player* m_player;
};

} // namespace scvp::ui
