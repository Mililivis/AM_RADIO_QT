#ifndef UI_FREQUENCY_VIEW_H
#define UI_FREQUENCY_VIEW_H

#include <QObject>

/// @brief visible frequency window shared by the spectrum and the waterfall (zoom and pan stay in sync)
class FrequencyView : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    double from() const { return from_; }
    double to() const { return to_; }
    double width() const { return to_ - from_; }
    double dataFrom() const { return dataFrom_; }
    double dataTo() const { return dataTo_; }
    bool isValid() const { return to_ > from_; }
    bool isZoomed() const { return from_ > dataFrom_ || to_ < dataTo_; }

    /// @brief extent of the data; when it changes the view is reset to show all of it
    void setDataRange(double from, double to);
    void setRange(double from, double to);
    /// @param factor < 1 zooms in, > 1 zooms out, anchorHz stays under the cursor
    void zoom(double factor, double anchorHz);
    void pan(double deltaHz);
    void reset();

signals:
    void changed();

private:
    double dataFrom_ = 0.0;
    double dataTo_ = 0.0;
    double from_ = 0.0;
    double to_ = 0.0;
};

#endif
