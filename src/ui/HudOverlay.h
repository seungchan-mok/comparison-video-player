#pragma once

#include <QTimer>
#include <QWidget>

class QPainter;
class QFontMetrics;

namespace scvp::ui {

class Player;

// Information overlay drawn over the video (a raster child widget, so it is composited with the
// GL frame without touching the GL pipeline): file, codec, size, pixel format, frame number,
// timestamp, picture type, cache, and a stall badge while playback waits for decoding.
class HudOverlay : public QWidget {
    Q_OBJECT
public:
    explicit HudOverlay(Player* player, QWidget* parent);

    void setInfoVisible(bool on);
    bool infoVisible() const { return m_info; }
    void setStalled(bool stalled);
    void reposition();
    void setPixelText(const QString& text);  // pixel inspector line ("" hides it)
    QString pixelText() const { return m_pixelText; }

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QStringList lines() const;
    QString pathText(int slot) const;
    void paintPaneLabels(QPainter& p, const QFontMetrics& fm);

    Player* m_player;
    QTimer m_refresh;
    QTimer m_stallDelay;
    QString m_pixelText;
    bool m_info = false;
    bool m_stalled = false;
    bool m_showStall = false;
};

} // namespace scvp::ui
