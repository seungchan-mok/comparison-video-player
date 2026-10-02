#include "render/ViewTransform.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace scvp::render {
namespace {

// Zoom ladder: quarter octaves from 1/32 to just below 1, then integers up to 256.
const std::array<double, 40>& ladder()
{
    static const std::array<double, 40> l = [] {
        std::array<double, 40> v{};
        size_t n = 0;
        for (int k = -20; k <= -1; ++k)
            v[n++] = std::pow(2.0, k / 4.0);
        for (double s : {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 96, 128, 192, 256})
            v[n++] = s;
        return v;
    }();
    return l;
}

} // namespace

double ViewTransform::snapUp(double s)
{
    for (double v : ladder())
        if (v > s * (1.0 + 1e-9))
            return v;
    return kMaxScale;
}

double ViewTransform::snapDown(double s)
{
    const auto& l = ladder();
    for (auto it = l.rbegin(); it != l.rend(); ++it)
        if (*it < s * (1.0 - 1e-9))
            return *it;
    return kMinScale;
}

double ViewTransform::fitScale(QSizeF image, QSizeF viewport)
{
    if (image.width() <= 0 || image.height() <= 0 || viewport.width() <= 0 || viewport.height() <= 0)
        return 1.0;
    double s = std::min(viewport.width() / image.width(), viewport.height() / image.height());
    if (s >= 1.0)
        s = std::floor(s);  // whole multiples keep pixels exact
    return std::clamp(s, kMinScale, kMaxScale);
}

void ViewTransform::setImageSize(QSizeF size)
{
    if (size == m_image)
        return;
    m_image = size;
    if (m_fit)
        fit();
    else
        clampTranslation();
}

void ViewTransform::setViewportSize(QSizeF deviceSize)
{
    if (deviceSize == m_viewport)
        return;
    if (m_fit) {
        m_viewport = deviceSize;
        fit();
        return;
    }
    // Keep the image point at the viewport centre where it is.
    const QPointF centre = deviceToImage(QPointF(m_viewport.width() / 2.0 - 0.5, m_viewport.height() / 2.0 - 0.5));
    m_viewport = deviceSize;
    m_t = QPointF(m_viewport.width() / 2.0 - m_scale * centre.x(), m_viewport.height() / 2.0 - m_scale * centre.y());
    clampTranslation();
}

void ViewTransform::fit()
{
    m_fit = true;
    m_scale = fitScale(m_image, m_viewport);
    m_t = QPointF((m_viewport.width() - m_scale * m_image.width()) / 2.0,
                  (m_viewport.height() - m_scale * m_image.height()) / 2.0);
}

void ViewTransform::zoomTo(double scale, QPointF anchor)
{
    scale = std::clamp(scale, kMinScale, kMaxScale);
    // The image point under the anchor stays under it.
    const QPointF x = (anchor - m_t) / m_scale;
    m_scale = scale;
    m_t = anchor - x * m_scale;
    m_fit = false;
    clampTranslation();
}

void ViewTransform::zoomSteps(int steps, QPointF anchor)
{
    double s = m_scale;
    for (int i = 0; i < steps; ++i)
        s = snapUp(s);
    for (int i = 0; i > steps; --i)
        s = snapDown(s);
    zoomTo(s, anchor);
}

void ViewTransform::oneToOne(QPointF anchor) { zoomTo(1.0, anchor); }

void ViewTransform::panBy(QPointF delta)
{
    m_t += delta;
    m_fit = false;
    clampTranslation();
}

void ViewTransform::scaleDevice(double ratio)
{
    if (ratio <= 0 || ratio == 1.0)
        return;
    m_viewport *= ratio;
    if (m_fit) {
        fit();
        return;
    }
    m_scale = std::clamp(m_scale * ratio, kMinScale, kMaxScale);
    m_t *= ratio;
    clampTranslation();
}

QPointF ViewTransform::renderTranslation() const { return QPointF(std::round(m_t.x()), std::round(m_t.y())); }

QPointF ViewTransform::deviceToImage(QPointF d) const
{
    const QPointF t = renderTranslation();
    // float32 like the shader, so hit testing agrees with what is drawn
    const float x = (float(d.x()) + 0.5f - float(t.x())) / float(m_scale);
    const float y = (float(d.y()) + 0.5f - float(t.y())) / float(m_scale);
    return QPointF(x, y);
}

QPointF ViewTransform::imageToDevice(QPointF x) const { return x * m_scale + renderTranslation(); }

// Keep at least a little of the image (64 px, or all of it when smaller) inside the viewport.
void ViewTransform::clampTranslation()
{
    const double w = m_scale * m_image.width();
    const double h = m_scale * m_image.height();
    const double keepX = std::min(64.0, w);
    const double keepY = std::min(64.0, h);
    m_t.setX(std::clamp(m_t.x(), keepX - w, m_viewport.width() - keepX));
    m_t.setY(std::clamp(m_t.y(), keepY - h, m_viewport.height() - keepY));
}

} // namespace scvp::render
