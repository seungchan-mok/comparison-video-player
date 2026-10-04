#include "render/DigitAtlas.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>

namespace scvp::render {
namespace {

// 5x7 fallback when no font renders (headless systems without fonts).
constexpr const char* kBitmap[DigitAtlas::kGlyphs][7] = {
    {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}, {"00100", "01100", "00100", "00100", "00100", "00100", "01110"},
    {"01110", "10001", "00001", "00010", "00100", "01000", "11111"}, {"11111", "00010", "00100", "00010", "00001", "10001", "01110"},
    {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}, {"11111", "10000", "11110", "00001", "00001", "10001", "01110"},
    {"00110", "01000", "10000", "11110", "10001", "10001", "01110"}, {"11111", "00001", "00010", "00100", "01000", "01000", "01000"},
    {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}, {"01110", "10001", "10001", "01111", "00001", "00010", "01100"},
    {"00000", "00000", "00000", "11111", "00000", "00000", "00000"},
};

bool hasInk(const QImage& img)
{
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            if (row[x] > 128)
                return true;
    }
    return false;
}

} // namespace

std::array<QImage, DigitAtlas::kGlyphs> DigitAtlas::render()
{
    std::array<QImage, kGlyphs> out;
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setWeight(QFont::DemiBold);
    // Size the font so a digit is ~80 % of the cell high and fits its width.
    font.setPixelSize(100);
    {
        const QFontMetricsF fm(font);
        const QRectF zero = fm.tightBoundingRect(QStringLiteral("0"));
        const double byHeight = 100.0 * (kHeight * 0.8) / std::max(1.0, zero.height());
        const double byWidth = 100.0 * (kWidth * 0.9) / std::max(1.0, fm.horizontalAdvance(QLatin1Char('0')));
        font.setPixelSize(std::max(8, int(std::min(byHeight, byWidth))));
    }
    const QFontMetricsF fm(font);
    const QRectF zero = fm.tightBoundingRect(QStringLiteral("0"));
    const double baseline = (kHeight - zero.height()) / 2.0 - zero.top();
    bool fontOk = true;
    for (int i = 0; i < kGlyphs && fontOk; ++i) {
        QImage img(kWidth, kHeight, QImage::Format_Grayscale8);
        img.fill(0);
        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font);
        p.setPen(Qt::white);
        const QChar c = i == kMinus ? QLatin1Char('-') : QChar(QLatin1Char(char('0' + i)));
        p.drawText(QPointF((kWidth - fm.horizontalAdvance(c)) / 2.0, baseline), QString(c));
        p.end();
        fontOk = hasInk(img);
        out[size_t(i)] = img;
    }
    if (fontOk)
        return out;
    for (int i = 0; i < kGlyphs; ++i) {
        QImage img(kWidth, kHeight, QImage::Format_Grayscale8);
        img.fill(0);
        const int cw = kWidth / 6, ch = kHeight / 8;  // 5x7 dots, centred
        const int ox = (kWidth - 5 * cw) / 2, oy = (kHeight - 7 * ch) / 2;
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 5; ++x)
                if (kBitmap[i][y][x] == '1')
                    for (int yy = 0; yy < ch; ++yy)
                        std::fill_n(img.scanLine(oy + y * ch + yy) + ox + x * cw, cw, uchar(255));
        out[size_t(i)] = img;
    }
    return out;
}

bool DigitAtlas::create(GL* gl)
{
    if (m_tex)
        return true;
    m_glyphs = render();
    gl->glGenTextures(1, &m_tex);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, m_tex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, kWidth, kHeight, kGlyphs, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    for (int i = 0; i < kGlyphs; ++i) {
        const QImage& g = m_glyphs[size_t(i)];
        for (int y = 0; y < kHeight; ++y)
            gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, y, i, kWidth, 1, 1, GL_RED, GL_UNSIGNED_BYTE, g.constScanLine(y));
    }
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    gl->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return true;
}

void DigitAtlas::release(GL* gl)
{
    if (m_tex)
        gl->glDeleteTextures(1, &m_tex);
    m_tex = 0;
}

} // namespace scvp::render
