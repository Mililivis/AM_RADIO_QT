#include "SpectrumWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
const QColor background(16, 20, 24);
const QColor gridColor(48, 56, 64);
const QColor textColor(160, 170, 180);
const QColor traceColor(80, 200, 255);
const QColor fillColor(80, 200, 255, 40);
const QColor markerColor(255, 170, 40);
const QColor highlightColor(255, 80, 80, 50);
const QColor highlightLine(255, 80, 80);
const QColor cursorColor(220, 220, 220, 120);

constexpr double marginLeft = 44.0;
constexpr double marginRight = 8.0;
constexpr double marginTop = 8.0;
constexpr double marginBottom = 20.0;

/// @brief 1-2-5 step giving about `count` ticks over `span`
double niceStep(double span, int count)
{
    double raw = span / count;
    double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
    {
        if (raw <= m * mag)
        {
            return m * mag;
        }
    }
    return 10.0 * mag;
}
} // namespace

SpectrumWidget::SpectrumWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SpectrumWidget::setSpectrum(const QVector<float> &db, double startHz, double binHz)
{
    db_ = db;
    startHz_ = startHz;
    binHz_ = binHz > 0.0 ? binHz : 1.0;
    updateScale();
    update();
}

void SpectrumWidget::clear()
{
    db_.clear();
    markers_.clear();
    scaleValid_ = false;
    update();
}

void SpectrumWidget::setMarkers(const QVector<double> &freqsHz)
{
    markers_ = freqsHz;
    update();
}

void SpectrumWidget::setHighlight(double centreHz, double widthHz)
{
    highlightHz_ = centreHz;
    highlightWidth_ = widthHz;
    update();
}

void SpectrumWidget::updateScale()
{
    std::vector<float> values;
    values.reserve(static_cast<size_t>(db_.size()));
    for (float v : db_)
    {
        if (!std::isnan(v))
        {
            values.push_back(v);
        }
    }
    if (values.empty())
    {
        return;
    }

    auto low = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 20);
    std::nth_element(values.begin(), low, values.end());
    double lo = std::floor((*low - 10.0) / 10.0) * 10.0;
    double hi = std::ceil((*std::max_element(values.begin(), values.end()) + 5.0) / 10.0) * 10.0;
    hi = std::max(hi, lo + 30.0);

    if (smoothScale_ && scaleValid_)
    {
        minDb_ += 0.1 * (lo - minDb_);
        maxDb_ += 0.1 * (hi - maxDb_);
    }
    else
    {
        minDb_ = lo;
        maxDb_ = hi;
    }
    scaleValid_ = true;
}

QRectF SpectrumWidget::plotRect() const
{
    return QRectF(marginLeft, marginTop, width() - marginLeft - marginRight, height() - marginTop - marginBottom);
}

double SpectrumWidget::xToHz(double x) const
{
    QRectF r = plotRect();
    double span = db_.size() * binHz_;
    return startHz_ + (x - r.left()) / r.width() * span;
}

double SpectrumWidget::hzToX(double hz) const
{
    QRectF r = plotRect();
    double span = db_.size() * binHz_;
    return r.left() + (hz - startHz_) / span * r.width();
}

void SpectrumWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), background);

    QRectF r = plotRect();
    QFont font = p.font();
    font.setPointSizeF(8.0);
    p.setFont(font);

    if (db_.isEmpty() || r.width() < 10 || r.height() < 10)
    {
        p.setPen(textColor);
        p.drawText(rect(), Qt::AlignCenter, tr("немає даних"));
        return;
    }

    const double dbSpan = maxDb_ - minDb_;
    auto dbToY = [&](double db) { return r.bottom() - (db - minDb_) / dbSpan * r.height(); };

    // dB grid
    p.setPen(gridColor);
    double dbStep = dbSpan > 60 ? 20.0 : 10.0;
    for (double d = std::ceil(minDb_ / dbStep) * dbStep; d <= maxDb_; d += dbStep)
    {
        double y = dbToY(d);
        p.setPen(gridColor);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(textColor);
        p.drawText(QRectF(0, y - 8, marginLeft - 4, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(d, 'f', 0));
    }

    // Frequency grid, labels in kHz
    const double spanHz = db_.size() * binHz_;
    const double fStep = niceStep(spanHz, std::max(2, static_cast<int>(r.width() / 90)));
    for (double f = std::ceil(startHz_ / fStep) * fStep; f <= startHz_ + spanHz; f += fStep)
    {
        double x = hzToX(f);
        p.setPen(gridColor);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(textColor);
        double khz = f / 1e3;
        QString label = fStep >= 1e3 ? QString::number(khz, 'f', 0) : QString::number(khz, 'f', 1);
        p.drawText(QRectF(x - 40, r.bottom() + 2, 80, marginBottom - 2), Qt::AlignHCenter | Qt::AlignTop, label);
    }

    p.setClipRect(r);

    // Tuned channel
    if (highlightWidth_ > 0.0)
    {
        double x0 = hzToX(highlightHz_ - highlightWidth_ / 2.0);
        double x1 = hzToX(highlightHz_ + highlightWidth_ / 2.0);
        p.fillRect(QRectF(x0, r.top(), std::max(1.0, x1 - x0), r.height()), highlightColor);
        p.setPen(highlightLine);
        double xc = hzToX(highlightHz_);
        p.drawLine(QPointF(xc, r.top()), QPointF(xc, r.bottom()));
    }

    // Trace: one point per pixel column = max of bins under it
    QPainterPath line;
    QPainterPath fill;
    bool open = false;
    double lastX = 0.0;
    const int columns = static_cast<int>(r.width());
    const int n = db_.size();

    auto closeSegment = [&]() {
        if (open)
        {
            fill.lineTo(lastX, r.bottom());
            fill.closeSubpath();
            open = false;
        }
    };

    for (int c = 0; c <= columns; ++c)
    {
        int b0 = static_cast<int>(static_cast<double>(c) / columns * n);
        int b1 = std::max(b0 + 1, static_cast<int>(static_cast<double>(c + 1) / columns * n));
        b1 = std::min(b1, n);
        float v = std::numeric_limits<float>::quiet_NaN();
        for (int b = b0; b < b1; ++b)
        {
            if (!std::isnan(db_[b]) && (std::isnan(v) || db_[b] > v))
            {
                v = db_[b];
            }
        }

        double x = r.left() + c;
        if (std::isnan(v))
        {
            closeSegment();
            continue;
        }
        double y = std::clamp(dbToY(v), r.top(), r.bottom());
        if (!open)
        {
            line.moveTo(x, y);
            fill.moveTo(x, r.bottom());
            fill.lineTo(x, y);
            open = true;
        }
        else
        {
            line.lineTo(x, y);
            fill.lineTo(x, y);
        }
        lastX = x;
    }
    closeSegment();

    p.fillPath(fill, fillColor);
    p.setPen(QPen(traceColor, 1.0));
    p.drawPath(line);

    // Station markers
    p.setPen(Qt::NoPen);
    p.setBrush(markerColor);
    for (double f : markers_)
    {
        double x = hzToX(f);
        QPolygonF tri;
        tri << QPointF(x - 4, r.top()) << QPointF(x + 4, r.top()) << QPointF(x, r.top() + 7);
        p.drawPolygon(tri);
    }

    // Cursor readout
    if (mouseX_ >= r.left() && mouseX_ <= r.right())
    {
        double hz = xToHz(mouseX_);
        int idx = static_cast<int>((hz - startHz_) / binHz_);
        QString text = QString("%1 kHz").arg(hz / 1e3, 0, 'f', 1);
        if (idx >= 0 && idx < n && !std::isnan(db_[idx]))
        {
            text += QString("  %1 dB").arg(db_[idx], 0, 'f', 1);
        }

        p.setPen(cursorColor);
        p.drawLine(QPointF(mouseX_, r.top()), QPointF(mouseX_, r.bottom()));
        p.setPen(Qt::white);
        QRectF textRect(mouseX_ + 6, r.top() + 10, 150, 16);
        if (textRect.right() > r.right())
        {
            textRect.moveRight(mouseX_ - 6);
        }
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

void SpectrumWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !db_.isEmpty() && plotRect().contains(event->position()))
    {
        emit frequencyClicked(xToHz(event->position().x()));
    }
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent *event)
{
    mouseX_ = static_cast<int>(event->position().x());
    update();
}

void SpectrumWidget::leaveEvent(QEvent *)
{
    mouseX_ = -1;
    update();
}
