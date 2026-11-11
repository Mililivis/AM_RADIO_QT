#ifndef UI_SPECTRUM_WIDGET_H
#define UI_SPECTRUM_WIDGET_H

#include "FrequencyPlot.h"
#include "SpectrumAnalyzer.h"

#include <QVector>

class QTimer;

/// @brief power spectrum plot.
///
/// Each pixel shows the total power in its frequency interval (energy-conserving rebinning),
/// so the trace never loses a signal when many FFT bins fall into one pixel and wideband
/// signals are shown with all their energy. The dB axis is "dBFS in the pixel bandwidth";
/// the effective resolution is printed in the corner.
class SpectrumWidget : public FrequencyPlot
{
    Q_OBJECT

public:
    explicit SpectrumWidget(FrequencyView *view, QWidget *parent = nullptr);

    void setFrame(const core::SpectrumFramePtr &frame);
    void clear();
    void setMarkers(const QVector<double> &freqsHz);
    /// @brief receiver channel, width <= 0 hides it
    void setChannel(double centreHz, double widthHz);
    /// @brief upper limit of redraws per second (every redraw is a full picture over X11)
    void setMaxRefreshRate(double fps);

    QSize sizeHint() const override { return {800, 260}; }
    QSize minimumSizeHint() const override { return {200, 120}; }

protected:
    QRect plotRect() const override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void refresh();
    void computePixels();
    void updateScale(const std::vector<float> &db);
    /// @brief pixels touched by the mouse cursor line and its readout
    QRegion cursorArea(int x) const;

    core::SpectrumFramePtr frame_;
    std::vector<float> pixelDb_;   // energy-conserving pixels of frame_ for the current view
    bool pixelsValid_ = false;
    QTimer *refreshTimer_;
    QVector<double> markers_;
    double channelHz_ = 0.0;
    double channelWidth_ = 0.0;

    bool scaleValid_ = false;
    double minDb_ = -120.0;
    double maxDb_ = -20.0;
    int mouseX_ = -1;
};

#endif
