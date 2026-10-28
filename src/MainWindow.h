#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "Scanner.h"

#include <QMainWindow>

class AudioOutput;
class RadioEngine;
class SpectrumWidget;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTableWidget;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(RadioEngine *engine, AudioOutput *audio, const QString &audioError, QWidget *parent = nullptr);
    ~MainWindow() override;

    /// @brief tune and start listening (used for --tune command line option)
    void startPlayback(double freqHz) { tune(freqHz); }

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onBandChanged(int index);
    void onScanClicked();
    void onScanProgress(int percent);
    void onScanFinished(const ScanResult &result, bool aborted);
    void onThresholdChanged();
    void onStationActivated(int row);
    void onSpectrumClicked(double hz);
    void onPlayClicked();
    void onFrequencyEdited(double khz);
    void onPrevNext(int direction);
    void onPlaybackChanged(bool playing, double freqHz);
    void onLiveSpectrum(const QVector<float> &db, double startHz, double binHz);
    void onLevel(float dbfs);
    void onError(const QString &message);
    void onStatusTimer();

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void setScanControlsEnabled(bool enabled);
    void showStations();
    void tune(double freqHz);
    double gridHz() const;
    double bandwidthHz() const;
    ScanParams currentParams() const;

    RadioEngine *engine_;
    AudioOutput *audio_;
    QString audioError_;

    ScanResult lastScan_;
    ScanParams lastParams_;
    std::vector<Station> stations_;

    // Scan controls
    QComboBox *bandCombo_;
    QDoubleSpinBox *fromSpin_;
    QDoubleSpinBox *toSpin_;
    QComboBox *gridCombo_;
    QDoubleSpinBox *thresholdSpin_;
    QComboBox *gainCombo_;
    QPushButton *scanButton_;
    QProgressBar *scanProgress_;

    QTableWidget *stationTable_;
    SpectrumWidget *bandSpectrum_;

    // Playback controls
    QDoubleSpinBox *freqSpin_;
    QPushButton *prevButton_;
    QPushButton *nextButton_;
    QPushButton *playButton_;
    QComboBox *bwCombo_;
    QSlider *volumeSlider_;
    QProgressBar *levelBar_;
    QLabel *levelLabel_;
    SpectrumWidget *liveSpectrum_;

    QLabel *deviceLabel_;
    QLabel *audioLabel_;
    QTimer *statusTimer_;
};

#endif
