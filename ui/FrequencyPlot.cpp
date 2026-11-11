#include "FrequencyPlot.h"

#include "FrequencyView.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <cmath>

namespace
{
constexpr int dragThresholdPx = 4;
constexpr double zoomStep = 0.8;
} // namespace

FrequencyPlot::FrequencyPlot(FrequencyView *view, QWidget *parent)
    : QWidget(parent), view_(view)
{
    setCursor(Qt::CrossCursor);
    connect(view_, &FrequencyView::changed, this, qOverload<>(&QWidget::update));
}

double FrequencyPlot::xToHz(double x) const
{
    QRect r = plotRect();
    return view_->from() + (x - r.left()) / r.width() * view_->width();
}

double FrequencyPlot::hzToX(double hz) const
{
    QRect r = plotRect();
    return r.left() + (hz - view_->from()) / view_->width() * r.width();
}

void FrequencyPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        return;
    }
    pressed_ = true;
    dragging_ = false;
    pressPos_ = event->position().toPoint();
    lastDragX_ = event->position().x();
}

void FrequencyPlot::mouseMoveEvent(QMouseEvent *event)
{
    if (!pressed_ || !view_->isValid())
    {
        return;
    }
    if (!dragging_ && std::abs(event->position().toPoint().x() - pressPos_.x()) > dragThresholdPx)
    {
        dragging_ = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (dragging_)
    {
        double dx = event->position().x() - lastDragX_;
        lastDragX_ = event->position().x();
        view_->pan(-dx / plotRect().width() * view_->width());
    }
}

void FrequencyPlot::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !pressed_)
    {
        return;
    }
    pressed_ = false;
    if (dragging_)
    {
        dragging_ = false;
        setCursor(Qt::CrossCursor);
        return;
    }
    if (view_->isValid() && plotRect().contains(event->position().toPoint()))
    {
        emit frequencyClicked(xToHz(event->position().x()));
    }
}

void FrequencyPlot::mouseDoubleClickEvent(QMouseEvent *)
{
    view_->reset();
}

void FrequencyPlot::wheelEvent(QWheelEvent *event)
{
    if (!view_->isValid())
    {
        return;
    }
    double steps = event->angleDelta().y() / 120.0;
    view_->zoom(std::pow(zoomStep, steps), xToHz(event->position().x()));
    event->accept();
}
