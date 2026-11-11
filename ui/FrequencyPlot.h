#ifndef UI_FREQUENCY_PLOT_H
#define UI_FREQUENCY_PLOT_H

#include <QPoint>
#include <QWidget>

class FrequencyView;

/// @brief base of the spectrum and waterfall: common plot geometry (pixels line up between them)
/// and mouse handling: wheel = zoom, drag = pan, click = tune, double click = show everything
class FrequencyPlot : public QWidget
{
    Q_OBJECT

public:
    static constexpr int marginLeft = 48;
    static constexpr int marginRight = 8;

    explicit FrequencyPlot(FrequencyView *view, QWidget *parent = nullptr);

    FrequencyView *view() const { return view_; }

signals:
    void frequencyClicked(double hz);

protected:
    /// @brief area of the plot inside the widget
    virtual QRect plotRect() const = 0;
    double xToHz(double x) const;
    double hzToX(double hz) const;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

    FrequencyView *view_;

private:
    QPoint pressPos_;
    double lastDragX_ = 0.0;
    bool pressed_ = false;
    bool dragging_ = false;
};

#endif
