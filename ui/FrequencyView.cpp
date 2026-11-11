#include "FrequencyView.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double minWidthHz = 2e3;
}

void FrequencyView::setDataRange(double from, double to)
{
    if (std::fabs(from - dataFrom_) < 1.0 && std::fabs(to - dataTo_) < 1.0)
    {
        return;
    }
    dataFrom_ = from;
    dataTo_ = to;
    reset();
}

void FrequencyView::setRange(double from, double to)
{
    double width = std::clamp(to - from, minWidthHz, dataTo_ - dataFrom_);
    from = std::clamp(from, dataFrom_, dataTo_ - width);
    if (from == from_ && from + width == to_)
    {
        return;
    }
    from_ = from;
    to_ = from + width;
    emit changed();
}

void FrequencyView::zoom(double factor, double anchorHz)
{
    if (!isValid())
    {
        return;
    }
    double newWidth = width() * factor;
    double rel = (anchorHz - from_) / width();
    setRange(anchorHz - rel * newWidth, anchorHz + (1.0 - rel) * newWidth);
}

void FrequencyView::pan(double deltaHz)
{
    setRange(from_ + deltaHz, to_ + deltaHz);
}

void FrequencyView::reset()
{
    from_ = dataFrom_;
    to_ = dataTo_;
    emit changed();
}
