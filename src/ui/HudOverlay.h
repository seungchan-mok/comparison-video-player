#pragma once

#include <QElapsedTimer>
#include <QTimer>

#include <array>
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
    // A/B legend: each part of the view names its video (coloured A/B chip + file name).
    void setLegendVisible(bool on);
    bool legendVisible() const { return m_legend; }
    // A large A or B in the middle of the view for a moment (flipping between the videos).
    void flashSlot(int slot);
    static QColor slotColor(int slot);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QStringList lines() const;
    QString pathText(int slot) const;
    void paintPaneLabels(QPainter& p, const QFontMetrics& fm);
    void paintLegend(QPainter& p, const QRect& avoid);
    void paintFlash(QPainter& p);
    QString slotName(int slot) const;
    static QStringList wrapLine(const QString& line, const QFontMetrics& fm, int maxWidth);
    void sampleRates();  // refresh timer: frame rates over the last second
    QString perfLine() const;

    Player* m_player;
    QTimer m_refresh;
    QTimer m_stallDelay;
    QString m_pixelText;
    // Performance figures: smoothed over about a second.
    QElapsedTimer m_rateClock;
    qint64 m_lastPositionCount = 0;
    qint64 m_positionCount = 0;
    std::array<uint64_t, 2> m_lastDecoded{};
    double m_shownFps = 0;
    std::array<double, 2> m_decodeFps{};
    double m_paintMs = 0;
    double m_uploadMs = 0;
    double m_convertMs = 0;
    bool m_legend = false;
    int m_flashSlot = -1;
    QElapsedTimer m_flashClock;
    QTimer m_flashTimer;
    bool m_info = false;
    bool m_stalled = false;
    bool m_showStall = false;
};

} // namespace scvp::ui
