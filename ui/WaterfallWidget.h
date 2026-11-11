#ifndef UI_WATERFALL_WIDGET_H
#define UI_WATERFALL_WIDGET_H

#include "ColorMap.h"
#include "FrequencyPlot.h"
#include "SpectrumAnalyzer.h"

#include <QImage>
#include <QVector>

#include <deque>

/// @brief waterfall (spectrogram): newest line on top.
///
/// Every line is rebinned with the same energy-conserving method as the spectrum, so no signal
/// disappears at any zoom. Lines are kept as power spectra, not pixels: after zoom, pan or a
/// resize the whole history is redrawn from the data, and lines captured at another centre
/// frequency are drawn where they really were.
class WaterfallWidget : public FrequencyPlot
{
    Q_OBJECT

public:
    explicit WaterfallWidget(FrequencyView *view, QWidget *parent = nullptr);

    void addFrame(const core::SpectrumFramePtr &frame);
    void clear();
    /// @brief colour scale follows the data: noise floor dark, strongest signal not saturated
    void setAutoLevels(bool on) { autoLevels_ = on; }
    void setChannel(double centreHz, double widthHz);

    QSize sizeHint() const override { return {800, 360}; }
    QSize minimumSizeHint() const override { return {200, 80}; }

protected:
    QRect plotRect() const override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void renderLine(const core::SpectrumFrame &frame, QRgb *row, int width);
    void rebuildImage();
    void updateLevels(const core::SpectrumFrame &frame);

    std::deque<core::SpectrumFramePtr> history_;
    QImage image_;
    bool imageValid_ = false;
    ColorMap colors_;
    double minDb_ = -120.0;
    double maxDb_ = -40.0;
    bool autoLevels_ = true;
    bool levelsValid_ = false;
    double renderedMinDb_ = 0.0;   // levels the current picture was drawn with
    double renderedMaxDb_ = 0.0;
    double channelHz_ = 0.0;
    double channelWidth_ = 0.0;
};

#endif
