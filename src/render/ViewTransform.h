#pragma once

#include <QPointF>
#include <QSizeF>

namespace scvp::render {

// Image -> device-pixel mapping of a viewport:  device = scale * image + translation, both with
// a top-left origin and y pointing down. All values are device pixels (physical, HiDPI aware).
//
// Zoom steps follow a ladder: integers at and above 1:1 (so pixels stay square blocks) and
// quarter octaves below. Fit uses exactly the largest scale that shows the whole image, so the
// image fills the viewport in one direction (the renderer smooths such fractional zooms); the
// next zoom step lands on the ladder again. The translation is kept exact; rendering uses
// renderTranslation(), rounded to whole device pixels so pixel edges line up.
class ViewTransform {
public:
    void setImageSize(QSizeF size);
    void setViewportSize(QSizeF deviceSize);  // keeps the centre point in manual mode, refits in fit mode
    QSizeF imageSize() const { return m_image; }
    QSizeF viewportSize() const { return m_viewport; }

    void fit();
    bool isFit() const { return m_fit; }
    void zoomTo(double scale, QPointF deviceAnchor);
    void zoomSteps(int steps, QPointF deviceAnchor);  // +1 = one ladder step in
    void oneToOne(QPointF deviceAnchor);              // 1 image px = 1 device px
    void panBy(QPointF deviceDelta);
    // Rescales after a devicePixelRatio change (same logical view).
    void scaleDevice(double ratio);

    double scale() const { return m_scale; }
    QPointF translation() const { return m_t; }
    QPointF renderTranslation() const;

    // Same float math as the display shader: device pixel `d` (its centre is d + 0.5).
    QPointF deviceToImage(QPointF devicePixel) const;
    QPointF imageToDevice(QPointF image) const;

    static double snapUp(double scale);    // next ladder value above
    static double snapDown(double scale);  // next ladder value below
    static double fitScale(QSizeF image, QSizeF viewport);
    static constexpr double kMinScale = 1.0 / 32.0;
    static constexpr double kMaxScale = 256.0;

private:
    void clampTranslation();

    QSizeF m_image{1, 1};
    QSizeF m_viewport{1, 1};
    double m_scale = 1.0;
    QPointF m_t{0, 0};
    bool m_fit = true;
};

} // namespace scvp::render
