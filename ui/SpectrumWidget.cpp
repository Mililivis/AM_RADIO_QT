#include "SpectrumWidget.h"

#include "FrequencyView.h"
#include "dsp/Rebin.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
const QColor background(16, 20, 24);
const QColor gridColor(48, 56, 64);
const QColor textColor(160, 170, 180);
const QColor traceColor(80, 200, 255);
const QColor fillColor(80, 200, 255, 45);
const QColor markerColor(255, 170, 40);
const QColor channelFill(255, 80, 80, 50);
const QColor channelLine(255, 80, 80);
const QColor cursorColor(220, 220, 220, 120);

constexpr int marginTop = 8;
constexpr int marginBottom = 20;

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

QString formatHz(double hz)
{
    if (hz >= 1e6)
    {
        return QString("%1 МГц").arg(hz / 1e6, 0, 'f', hz >= 10e6 ? 2 : 3);
    }
    if (hz >= 1e3)
    {
        return QString("%1 кГц").arg(hz / 1e3, 0, 'f', hz >= 100e3 ? 0 : 1);
    }
    return QString("%1 Гц").arg(hz, 0, 'f', 0);
}
} // namespace

SpectrumWidget::SpectrumWidget(FrequencyView *view, QWidget *parent)
    : FrequencyPlot(view, parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    // New frames only mark the data; the picture is redrawn at most maxRefreshRate times per second
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(40);
    connect(refreshTimer_, &QTimer::timeout, this, &SpectrumWidget::refresh);
    connect(view_, &FrequencyView::changed, this, [this] { pixelsValid_ = false; });
}

void SpectrumWidget::setMaxRefreshRate(double fps)
{
    refreshTimer_->setInterval(static_cast<int>(1000.0 / std::max(1.0, fps)));
}

void SpectrumWidget::setFrame(const core::SpectrumFramePtr &frame)
{
    frame_ = frame;
    pixelsValid_ = false;
    if (!refreshTimer_->isActive())
    {
        refreshTimer_->start();
    }
}

void SpectrumWidget::refresh()
{
    computePixels();
    updateScale(pixelDb_);
    update();
}

void SpectrumWidget::computePixels()
{
    const QRect r = plotRect();
    const size_t pixels = static_cast<size_t>(r.width());
    pixelDb_.assign(pixels, std::numeric_limits<float>::quiet_NaN());
    pixelsValid_ = true;
    if (!frame_ || !view_->isValid())
    {
        return;
    }
    std::vector<float> power;
    dsp::rebinPower(frame_->power.data(), frame_->power.size(), frame_->startHz, frame_->binHz,
                    view_->from(), view_->to(), pixels, power);
    for (size_t i = 0; i < pixels; ++i)
    {
        pixelDb_[i] = std::isnan(power[i]) ? power[i] : static_cast<float>(10.0 * std::log10(power[i] + 1e-20));
    }
}

void SpectrumWidget::resizeEvent(QResizeEvent *)
{
    pixelsValid_ = false;
}

void SpectrumWidget::clear()
{
    frame_.reset();
    pixelsValid_ = false;
    scaleValid_ = false;
    update();
}

void SpectrumWidget::setMarkers(const QVector<double> &freqsHz)
{
    markers_ = freqsHz;
    update();
}

void SpectrumWidget::setChannel(double centreHz, double widthHz)
{
    channelHz_ = centreHz;
    channelWidth_ = widthHz;
    update();
}

QRect SpectrumWidget::plotRect() const
{
    return QRect(marginLeft, marginTop, std::max(1, width() - marginLeft - marginRight),
                 std::max(1, height() - marginTop - marginBottom));
}

void SpectrumWidget::updateScale(const std::vector<float> &db)
{
    std::vector<float> values;
    values.reserve(db.size());
    for (float v : db)
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
    hi = std::max(hi, lo + 40.0);

    // Smooth changes for the live view, jump for the first frame
    if (scaleValid_)
    {
        minDb_ += 0.15 * (lo - minDb_);
        maxDb_ += 0.15 * (hi - maxDb_);
    }
    else
    {
        minDb_ = lo;
        maxDb_ = hi;
        scaleValid_ = true;
    }
}

void SpectrumWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), background);
    QFont font = p.font();
    font.setPointSizeF(8.0);
    p.setFont(font);

    const QRect r = plotRect();
    if (!frame_ || !view_->isValid())
    {
        p.setPen(textColor);
        p.drawText(rect(), Qt::AlignCenter, tr("немає даних — запустіть огляд або панораму"));
        return;
    }

    // Energy-conserving pixels, cached: a partial repaint (mouse cursor) must not change the picture
    if (!pixelsValid_ || pixelDb_.size() != static_cast<size_t>(r.width()))
    {
        computePixels();
    }
    if (!scaleValid_)
    {
        updateScale(pixelDb_);
    }
    const std::vector<float> &db = pixelDb_;
    const size_t pixels = db.size();

    const double dbSpan = maxDb_ - minDb_;
    auto dbToY = [&](double v) { return r.bottom() - (v - minDb_) / dbSpan * r.height(); };

    // dB grid
    const double dbStep = dbSpan > 80 ? 20.0 : 10.0;
    for (double d = std::ceil(minDb_ / dbStep) * dbStep; d <= maxDb_; d += dbStep)
    {
        double y = dbToY(d);
        p.setPen(gridColor);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(textColor);
        p.drawText(QRectF(0, y - 8, marginLeft - 4, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(d, 'f', 0));
    }

    // Frequency grid
    const double fStep = niceStep(view_->width(), std::max(2, r.width() / 100));
    for (double f = std::ceil(view_->from() / fStep) * fStep; f <= view_->to(); f += fStep)
    {
        double x = hzToX(f);
        p.setPen(gridColor);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(textColor);
        int decimals = fStep >= 1e3 ? 0 : (fStep >= 100 ? 1 : 2);
        p.drawText(QRectF(x - 45, r.bottom() + 2, 90, marginBottom - 2), Qt::AlignHCenter | Qt::AlignTop,
                   QString::number(f / 1e3, 'f', decimals));
    }

    p.setClipRect(r);

    // Receiver channel
    if (channelWidth_ > 0.0)
    {
        double x0 = hzToX(channelHz_ - channelWidth_ / 2.0);
        double x1 = hzToX(channelHz_ + channelWidth_ / 2.0);
        p.fillRect(QRectF(x0, r.top(), std::max(1.0, x1 - x0), r.height()), channelFill);
        p.setPen(channelLine);
        double xc = hzToX(channelHz_);
        p.drawLine(QPointF(xc, r.top()), QPointF(xc, r.bottom()));
    }

    // Trace with fill, gaps where there is no data
    QPainterPath line;
    QPainterPath fill;
    bool open = false;
    double lastX = 0.0;
    auto closeSegment = [&]() {
        if (open)
        {
            fill.lineTo(lastX, r.bottom());
            fill.closeSubpath();
            open = false;
        }
    };
    for (size_t i = 0; i < pixels; ++i)
    {
        double x = r.left() + static_cast<double>(i) + 0.5;
        if (std::isnan(db[i]))
        {
            closeSegment();
            continue;
        }
        double y = std::clamp(dbToY(db[i]), static_cast<double>(r.top()), static_cast<double>(r.bottom()));
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
        if (f < view_->from() || f > view_->to())
        {
            continue;
        }
        double x = hzToX(f);
        QPolygonF tri;
        tri << QPointF(x - 4, r.top()) << QPointF(x + 4, r.top()) << QPointF(x, r.top() + 7);
        p.drawPolygon(tri);
    }

    // Effective resolution: pixel bandwidth or FFT bin, whichever is wider
    p.setPen(textColor);
    const double pixelHz = view_->width() / r.width();
    const double rbw = std::max(pixelHz, frame_->binHz);
    QString info = tr("дБFS у смузі %1 · FFT %2 · %3")
                       .arg(formatHz(rbw), formatHz(frame_->binHz),
                            frame_->sweep ? tr("панорама") : tr("ширина %1").arg(formatHz(frame_->sampleRate)));
    p.drawText(QRectF(r.left() + 6, r.top() + 4, r.width() - 12, 16), Qt::AlignRight | Qt::AlignTop, info);

    // Cursor readout
    if (mouseX_ >= r.left() && mouseX_ <= r.right())
    {
        double hz = xToHz(mouseX_);
        size_t i = static_cast<size_t>(std::clamp(mouseX_ - r.left(), 0, r.width() - 1));
        QString text = QString("%1 кГц").arg(hz / 1e3, 0, 'f', 2);
        if (!std::isnan(db[i]))
        {
            text += QString("  %1 дБ").arg(db[i], 0, 'f', 1);
        }
        p.setPen(cursorColor);
        p.drawLine(QPointF(mouseX_, r.top()), QPointF(mouseX_, r.bottom()));
        p.setPen(Qt::white);
        QRectF textRect(mouseX_ + 6, r.top() + 20, 170, 16);
        if (textRect.right() > r.right())
        {
            textRect.moveRight(mouseX_ - 6);
        }
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

QRegion SpectrumWidget::cursorArea(int x) const
{
    if (x < 0)
    {
        return {};
    }
    const QRect r = plotRect();
    QRegion area(QRect(x - 2, r.top(), 5, r.height()));
    area += QRect(x - 185, r.top() + 18, 370, 20);   // readout may be left or right of the line
    return area;
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent *event)
{
    // Only the old and the new cursor strips are redrawn, not the whole plot
    const int old = mouseX_;
    mouseX_ = static_cast<int>(event->position().x());
    FrequencyPlot::mouseMoveEvent(event);
    update(cursorArea(old) + cursorArea(mouseX_));
}

void SpectrumWidget::leaveEvent(QEvent *)
{
    const int old = mouseX_;
    mouseX_ = -1;
    update(cursorArea(old));
}
