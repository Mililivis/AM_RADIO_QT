#include "WaterfallWidget.h"

#include "FrequencyView.h"
#include "dsp/Rebin.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
const QColor background(0, 0, 0);
const QColor channelLine(255, 80, 80, 140);
constexpr int marginTop = 0;
constexpr int marginBottom = 0;
constexpr size_t maxHistory = 1200;            // more than any screen height
constexpr double rebuildLevelChangeDb = 6.0;   // recolour history after this much drift
constexpr double floorBelowDb = 3.0;      // noise floor drawn almost black
constexpr double peakHeadroomDb = 3.0;    // strongest signal below white
constexpr double minDynamicRangeDb = 30.0;
} // namespace

WaterfallWidget::WaterfallWidget(FrequencyView *view, QWidget *parent)
    : FrequencyPlot(view, parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(view_, &FrequencyView::changed, this, [this] { imageValid_ = false; });
}

QRect WaterfallWidget::plotRect() const
{
    return QRect(marginLeft, marginTop, std::max(1, width() - marginLeft - marginRight),
                 std::max(1, height() - marginTop - marginBottom));
}

void WaterfallWidget::clear()
{
    history_.clear();
    imageValid_ = false;
    levelsValid_ = false;
    update();
}

void WaterfallWidget::updateLevels(const core::SpectrumFrame &frame)
{
    if (!autoLevels_ || !view_->isValid())
    {
        return;
    }
    // Levels from what is visible, with the same energy-conserving pixels that are drawn
    std::vector<float> power;
    dsp::rebinPower(frame.power.data(), frame.power.size(), frame.startHz, frame.binHz,
                    view_->from(), view_->to(), static_cast<size_t>(plotRect().width()), power);
    std::vector<float> db;
    db.reserve(power.size());
    for (float p : power)
    {
        if (!std::isnan(p))
        {
            db.push_back(static_cast<float>(10.0 * std::log10(p + 1e-20)));
        }
    }
    if (db.empty())
    {
        return;
    }
    auto low = db.begin() + static_cast<std::ptrdiff_t>(db.size() / 10);
    std::nth_element(db.begin(), low, db.end());
    const double lo = *low - floorBelowDb;
    const double hi = std::max(lo + minDynamicRangeDb, static_cast<double>(*std::max_element(db.begin(), db.end())) + peakHeadroomDb);

    if (!levelsValid_)
    {
        minDb_ = lo;
        maxDb_ = hi;
        levelsValid_ = true;
        imageValid_ = false;
        return;
    }
    // Slow tracking. Old lines keep their colours until the levels drifted noticeably:
    // recolouring means sending the whole picture again, which is expensive over X11
    minDb_ += 0.05 * (lo - minDb_);
    maxDb_ += 0.05 * (hi - maxDb_);
    if (std::fabs(minDb_ - renderedMinDb_) > rebuildLevelChangeDb
        || std::fabs(maxDb_ - renderedMaxDb_) > rebuildLevelChangeDb)
    {
        imageValid_ = false;
    }
}

void WaterfallWidget::setChannel(double centreHz, double widthHz)
{
    channelHz_ = centreHz;
    channelWidth_ = widthHz;
    update();
}

void WaterfallWidget::resizeEvent(QResizeEvent *)
{
    imageValid_ = false;
}

void WaterfallWidget::renderLine(const core::SpectrumFrame &frame, QRgb *row, int width)
{
    std::vector<float> power;
    dsp::rebinPower(frame.power.data(), frame.power.size(), frame.startHz, frame.binHz,
                    view_->from(), view_->to(), static_cast<size_t>(width), power);
    const double span = std::max(1.0, maxDb_ - minDb_);
    for (int x = 0; x < width; ++x)
    {
        float p = power[static_cast<size_t>(x)];
        if (std::isnan(p))
        {
            row[x] = qRgb(20, 20, 20);   // no data at this frequency in this line
            continue;
        }
        double db = 10.0 * std::log10(p + 1e-20);
        row[x] = colors_.at((db - minDb_) / span);
    }
}

void WaterfallWidget::rebuildImage()
{
    const QRect r = plotRect();
    if (image_.size() != r.size())
    {
        image_ = QImage(r.size(), QImage::Format_RGB32);
    }
    image_.fill(background);
    renderedMinDb_ = minDb_;
    renderedMaxDb_ = maxDb_;
    if (view_->isValid())
    {
        const int rows = std::min(image_.height(), static_cast<int>(history_.size()));
        for (int y = 0; y < rows; ++y)
        {
            renderLine(*history_[static_cast<size_t>(y)], reinterpret_cast<QRgb *>(image_.scanLine(y)), image_.width());
        }
    }
    imageValid_ = true;
}

void WaterfallWidget::addFrame(const core::SpectrumFramePtr &frame)
{
    history_.push_front(frame);
    if (history_.size() > maxHistory)
    {
        history_.pop_back();
    }
    updateLevels(*frame);

    if (imageValid_ && view_->isValid() && image_.height() > 1 && image_.size() == plotRect().size())
    {
        // Shift the picture down by one line and draw the new line on top.
        // QWidget::scroll moves the pixels already shown on the X server (server-side copy),
        // so over SSH only the new line travels instead of the whole picture
        const int bpl = image_.bytesPerLine();
        uchar *bits = image_.bits();
        std::memmove(bits + bpl, bits, static_cast<size_t>(bpl) * (image_.height() - 1));
        renderLine(*frame, reinterpret_cast<QRgb *>(image_.scanLine(0)), image_.width());
        scroll(0, 1, plotRect());
        return;
    }
    update();
}

void WaterfallWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), background);
    if (!imageValid_ || image_.size() != plotRect().size())
    {
        rebuildImage();
    }
    const QRect r = plotRect();
    p.drawImage(r.topLeft(), image_);

    if (history_.empty())
    {
        p.setPen(QColor(160, 170, 180));
        p.drawText(rect(), Qt::AlignCenter, tr("водоспад"));
        return;
    }

    if (channelWidth_ > 0.0 && view_->isValid())
    {
        p.setClipRect(r);
        // Solid lines: identical on every row, so they survive scrolling unchanged
        p.setPen(QPen(channelLine, 1));
        double x0 = hzToX(channelHz_ - channelWidth_ / 2.0);
        double x1 = hzToX(channelHz_ + channelWidth_ / 2.0);
        p.drawLine(QPointF(x0, r.top()), QPointF(x0, r.bottom()));
        p.drawLine(QPointF(x1, r.top()), QPointF(x1, r.bottom()));
    }
}
