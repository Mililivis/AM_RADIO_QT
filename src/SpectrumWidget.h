#ifndef SPECTRUM_WIDGET_H
#define SPECTRUM_WIDGET_H

#include <QVector>
#include <QWidget>

/// @brief power spectrum plot: frequency axis in kHz, auto-scaled dB axis, station markers,
/// highlighted channel and mouse cursor readout. Click emits the frequency under the cursor
class SpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumWidget(QWidget *parent = nullptr);

    void setSpectrum(const QVector<float> &db, double startHz, double binHz);
    void clear();
    void setMarkers(const QVector<double> &freqsHz);
    /// @brief translucent band around the tuned station, width <= 0 hides it
    void setHighlight(double centreHz, double widthHz);
    /// @brief smooth dB axis changes (for live spectrum updating ~10 times per second)
    void setSmoothScale(bool on) { smoothScale_ = on; }

    QSize sizeHint() const override { return {600, 220}; }
    QSize minimumSizeHint() const override { return {200, 120}; }

signals:
    void frequencyClicked(double hz);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF plotRect() const;
    double xToHz(double x) const;
    double hzToX(double hz) const;
    void updateScale();

    QVector<float> db_;
    double startHz_ = 0.0;
    double binHz_ = 1.0;
    QVector<double> markers_;
    double highlightHz_ = 0.0;
    double highlightWidth_ = 0.0;

    bool smoothScale_ = false;
    bool scaleValid_ = false;
    double minDb_ = -120.0;
    double maxDb_ = -20.0;

    int mouseX_ = -1;
};

#endif
