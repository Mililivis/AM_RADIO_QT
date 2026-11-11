#ifndef UI_MAIN_WINDOW_H
#define UI_MAIN_WINDOW_H

#include "RadioController.h"

#include <QMainWindow>

#include <limits>

namespace core
{
class AudioOutput;
}
class FrequencyView;
class SpectrumWidget;
class WaterfallWidget;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;
class QTimer;

/// @brief presentation layer: widgets only, all radio logic is in RadioController
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(RadioController *radio, core::AudioOutput *audio, const QString &audioError, QWidget *parent = nullptr);
    ~MainWindow() override;

    /// @brief start live view, tune and listen (used for --tune)
    void startListening(double freqHz);
    /// @brief start IQ streaming at startup (used for --rtltcp)
    void startNetwork(quint16 port);
    /// @brief continuous panorama of any range (used for --panorama)
    void startPanorama(double fromHz, double toHz);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onModeChanged(RadioController::Mode mode);
    void onSpectrumFrame(const core::SpectrumFramePtr &frame);
    void onSweepFinished(const core::ScanResult &result, bool aborted);
    void onTuningChanged(double centerHz, double spanHz, double vfoHz);
    void onPlotClicked(double hz);
    void onStatusTimer();

private:
    QWidget *buildViewGroup();
    QWidget *buildReceiverGroup();
    QWidget *buildSweepGroup();
    QWidget *buildStationsTab();
    QWidget *buildRecordGroup();
    QWidget *buildNetworkGroup();
    void loadSettings();
    void saveSettings();

    void tune(double hz);
    void showStations();
    void updateChannelOverlay();
    void onBandChanged(int index);
    void onPrevNext(int direction);
    void toggleRecording();
    void toggleNetwork();
    double gridHz() const;
    double bandwidthHz() const;
    core::ScanParams sweepParams() const;
    static void setSpinValue(QDoubleSpinBox *spin, double value);

    RadioController *radio_;
    core::AudioOutput *audio_;
    QString audioError_;

    core::ScanResult lastSweep_;
    core::ScanParams lastParams_;
    std::vector<core::Station> stations_;
    /// @brief frequency to tune to as soon as the live view is back (click during a sweep)
    double pendingTuneHz_ = std::numeric_limits<double>::quiet_NaN();

    FrequencyView *view_;
    SpectrumWidget *spectrum_;
    WaterfallWidget *waterfall_;

    // Live view
    QPushButton *liveButton_;
    QDoubleSpinBox *centerSpin_;
    QComboBox *spanCombo_;
    QComboBox *lineRateCombo_;
    QComboBox *gainCombo_;

    // Receiver
    QDoubleSpinBox *vfoSpin_;
    QPushButton *prevButton_;
    QPushButton *nextButton_;
    QPushButton *listenButton_;
    QComboBox *bwCombo_;
    QSlider *volumeSlider_;
    QProgressBar *levelBar_;
    QLabel *levelLabel_;

    // Sweep
    QComboBox *bandCombo_;
    QDoubleSpinBox *fromSpin_;
    QDoubleSpinBox *toSpin_;
    QComboBox *gridCombo_;
    QDoubleSpinBox *thresholdSpin_;
    QPushButton *scanButton_;
    QPushButton *panoramaButton_;
    QProgressBar *sweepProgress_;
    QTableWidget *stationTable_;

    // Recording
    QLineEdit *recordDirEdit_;
    QPushButton *recordButton_;
    QLabel *recordStatus_;

    // Network
    QSpinBox *portSpin_;
    QCheckBox *allowControlCheck_;
    QPushButton *networkButton_;
    QLabel *networkStatus_;

    QLabel *sourceLabel_;
    QLabel *audioLabel_;
    QTimer *statusTimer_;
};

#endif
