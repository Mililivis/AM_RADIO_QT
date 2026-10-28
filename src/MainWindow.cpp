#include "MainWindow.h"

#include "AudioOutput.h"
#include "BandPlan.h"
#include "RadioEngine.h"
#include "SpectrumWidget.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double minKHz = 100.0;
constexpr double maxKHz = 28800.0;

QDoubleSpinBox *makeKHzSpin(int decimals)
{
    auto *spin = new QDoubleSpinBox;
    spin->setRange(minKHz, maxKHz);
    spin->setDecimals(decimals);
    spin->setSuffix(QObject::tr(" кГц"));
    spin->setKeyboardTracking(false);
    spin->setAccelerated(true);
    return spin;
}

QTableWidgetItem *numberItem(double value, int decimals)
{
    auto *item = new QTableWidgetItem;
    double factor = std::pow(10.0, decimals);
    item->setData(Qt::DisplayRole, std::round(value * factor) / factor);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}
} // namespace

MainWindow::MainWindow(RadioEngine *engine, AudioOutput *audio, const QString &audioError, QWidget *parent)
    : QMainWindow(parent), engine_(engine), audio_(audio), audioError_(audioError)
{
    buildUi();
    loadSettings();

    connect(engine_, &RadioEngine::scanProgress, this, &MainWindow::onScanProgress);
    connect(engine_, &RadioEngine::scanFinished, this, &MainWindow::onScanFinished);
    connect(engine_, &RadioEngine::playbackChanged, this, &MainWindow::onPlaybackChanged);
    connect(engine_, &RadioEngine::liveSpectrum, this, &MainWindow::onLiveSpectrum);
    connect(engine_, &RadioEngine::levelChanged, this, &MainWindow::onLevel);
    connect(engine_, &RadioEngine::errorOccurred, this, &MainWindow::onError);

    statusTimer_ = new QTimer(this);
    connect(statusTimer_, &QTimer::timeout, this, &MainWindow::onStatusTimer);
    statusTimer_->start(500);
    onStatusTimer();
}

MainWindow::~MainWindow()
{
    saveSettings();
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("AM радіо — RTL-SDR"));

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);

    // ---------- Scan ----------
    auto *scanBox = new QGroupBox(tr("Сканування ефіру"));
    auto *scanLayout = new QHBoxLayout(scanBox);

    bandCombo_ = new QComboBox;
    for (const auto &band : bandPlan())
    {
        bandCombo_->addItem(QString::fromUtf8(band.name));
    }
    bandCombo_->addItem(tr("Довільний"));

    fromSpin_ = makeKHzSpin(0);
    toSpin_ = makeKHzSpin(0);

    gridCombo_ = new QComboBox;
    for (int khz : {1, 5, 9, 10})
    {
        gridCombo_->addItem(tr("%1 кГц").arg(khz), khz * 1000.0);
    }

    thresholdSpin_ = new QDoubleSpinBox;
    thresholdSpin_->setRange(3.0, 40.0);
    thresholdSpin_->setDecimals(0);
    thresholdSpin_->setSuffix(tr(" дБ"));
    thresholdSpin_->setValue(10.0);
    thresholdSpin_->setToolTip(tr("На скільки несуча має бути вище рівня шуму"));

    gainCombo_ = new QComboBox;
    gainCombo_->addItem(tr("Авто"), -1.0);
    for (double g : engine_->supportedGains())
    {
        gainCombo_->addItem(tr("%1 дБ").arg(g, 0, 'f', 1), g);
    }

    scanButton_ = new QPushButton(tr("Сканувати"));
    scanButton_->setMinimumWidth(110);
    scanProgress_ = new QProgressBar;
    scanProgress_->setRange(0, 100);
    scanProgress_->setValue(0);
    scanProgress_->setMaximumWidth(120);

    scanLayout->addWidget(new QLabel(tr("Діапазон:")));
    scanLayout->addWidget(bandCombo_);
    scanLayout->addWidget(new QLabel(tr("від")));
    scanLayout->addWidget(fromSpin_);
    scanLayout->addWidget(new QLabel(tr("до")));
    scanLayout->addWidget(toSpin_);
    scanLayout->addWidget(new QLabel(tr("Крок:")));
    scanLayout->addWidget(gridCombo_);
    scanLayout->addWidget(new QLabel(tr("Поріг:")));
    scanLayout->addWidget(thresholdSpin_);
    scanLayout->addWidget(new QLabel(tr("Підсилення:")));
    scanLayout->addWidget(gainCombo_);
    scanLayout->addStretch();
    scanLayout->addWidget(scanButton_);
    scanLayout->addWidget(scanProgress_);

    // ---------- Stations + band spectrum ----------
    stationTable_ = new QTableWidget(0, 3);
    stationTable_->setHorizontalHeaderLabels({tr("Частота, кГц"), tr("Рівень, дБ"), tr("С/Ш, дБ")});
    stationTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stationTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stationTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stationTable_->verticalHeader()->setVisible(false);
    stationTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    stationTable_->setSortingEnabled(true);
    stationTable_->setToolTip(tr("Подвійний клік / Enter — слухати"));

    bandSpectrum_ = new SpectrumWidget;
    bandSpectrum_->setToolTip(tr("Клік — слухати найближчий канал"));

    auto *topSplit = new QSplitter(Qt::Horizontal);
    topSplit->addWidget(stationTable_);
    topSplit->addWidget(bandSpectrum_);
    topSplit->setStretchFactor(0, 0);
    topSplit->setStretchFactor(1, 1);
    topSplit->setSizes({300, 800});

    // ---------- Playback ----------
    auto *playBox = new QGroupBox(tr("Прослуховування"));
    auto *playLayout = new QVBoxLayout(playBox);
    auto *controls = new QHBoxLayout;

    freqSpin_ = makeKHzSpin(1);
    freqSpin_->setSingleStep(1.0);
    freqSpin_->setValue(999.0);
    QFont big = freqSpin_->font();
    big.setPointSizeF(big.pointSizeF() * 1.4);
    big.setBold(true);
    freqSpin_->setFont(big);
    freqSpin_->setMinimumWidth(160);

    prevButton_ = new QPushButton(tr("◀"));
    prevButton_->setToolTip(tr("Попередня станція"));
    nextButton_ = new QPushButton(tr("▶"));
    nextButton_->setToolTip(tr("Наступна станція"));
    prevButton_->setMaximumWidth(40);
    nextButton_->setMaximumWidth(40);
    playButton_ = new QPushButton(tr("Слухати"));
    playButton_->setMinimumWidth(100);

    bwCombo_ = new QComboBox;
    for (int khz : {3, 4, 5, 6, 7, 8, 9, 10})
    {
        bwCombo_->addItem(tr("%1 кГц").arg(khz), khz * 1000.0);
    }
    bwCombo_->setCurrentIndex(bwCombo_->findData(8000.0));
    bwCombo_->setToolTip(tr("Смуга фільтра каналу (вужче — менше завад, глухіший звук)"));

    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(70);
    volumeSlider_->setMaximumWidth(160);

    levelBar_ = new QProgressBar;
    levelBar_->setRange(-100, 0);
    levelBar_->setValue(-100);
    levelBar_->setTextVisible(false);
    levelBar_->setMaximumWidth(160);
    levelLabel_ = new QLabel(tr("— dBFS"));
    levelLabel_->setMinimumWidth(80);

    controls->addWidget(new QLabel(tr("Частота:")));
    controls->addWidget(freqSpin_);
    controls->addWidget(prevButton_);
    controls->addWidget(nextButton_);
    controls->addWidget(playButton_);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(tr("Смуга:")));
    controls->addWidget(bwCombo_);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(tr("Гучність:")));
    controls->addWidget(volumeSlider_);
    controls->addSpacing(16);
    controls->addWidget(new QLabel(tr("Рівень:")));
    controls->addWidget(levelBar_);
    controls->addWidget(levelLabel_);
    controls->addStretch();

    liveSpectrum_ = new SpectrumWidget;
    liveSpectrum_->setSmoothScale(true);
    liveSpectrum_->setToolTip(tr("Спектр ±72 кГц навколо станції. Клік — перейти на канал"));

    playLayout->addLayout(controls);
    playLayout->addWidget(liveSpectrum_, 1);

    auto *mainSplit = new QSplitter(Qt::Vertical);
    mainSplit->addWidget(topSplit);
    mainSplit->addWidget(playBox);
    mainSplit->setSizes({400, 300});

    root->addWidget(scanBox);
    root->addWidget(mainSplit, 1);
    setCentralWidget(central);

    deviceLabel_ = new QLabel(engine_->deviceDescription());
    audioLabel_ = new QLabel;
    statusBar()->addPermanentWidget(deviceLabel_);
    statusBar()->addPermanentWidget(audioLabel_);

    resize(1200, 760);

    // ---------- Signals ----------
    connect(bandCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onBandChanged);
    connect(scanButton_, &QPushButton::clicked, this, &MainWindow::onScanClicked);
    connect(thresholdSpin_, &QDoubleSpinBox::valueChanged, this, &MainWindow::onThresholdChanged);
    connect(gainCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        engine_->setGain(gainCombo_->currentData().toDouble());
    });
    connect(stationTable_, &QTableWidget::cellActivated, this, [this](int row, int) { onStationActivated(row); });
    connect(bandSpectrum_, &SpectrumWidget::frequencyClicked, this, &MainWindow::onSpectrumClicked);
    connect(liveSpectrum_, &SpectrumWidget::frequencyClicked, this, &MainWindow::onSpectrumClicked);
    connect(freqSpin_, &QDoubleSpinBox::valueChanged, this, &MainWindow::onFrequencyEdited);
    connect(prevButton_, &QPushButton::clicked, this, [this] { onPrevNext(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { onPrevNext(+1); });
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::onPlayClicked);
    connect(bwCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        engine_->setBandwidth(bandwidthHz());
        if (engine_->isPlaying())
        {
            bandSpectrum_->setHighlight(engine_->frequency(), bandwidthHz());
        }
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int v) { engine_->setVolume(v / 100.0); });
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void MainWindow::loadSettings()
{
    QSettings s;

    int band = s.value("scan/band", 1).toInt();   // MW by default
    bandCombo_->setCurrentIndex(std::clamp(band, 0, bandCombo_->count() - 1));
    onBandChanged(bandCombo_->currentIndex());
    if (s.contains("scan/from"))
    {
        fromSpin_->setValue(s.value("scan/from").toDouble());
        toSpin_->setValue(s.value("scan/to").toDouble());
        int g = gridCombo_->findData(s.value("scan/grid").toDouble());
        if (g >= 0)
        {
            gridCombo_->setCurrentIndex(g);
        }
    }
    thresholdSpin_->setValue(s.value("scan/threshold", 10.0).toDouble());

    int gain = gainCombo_->findData(s.value("sdr/gain", 29.7).toDouble());
    gainCombo_->setCurrentIndex(gain >= 0 ? gain : 0);
    engine_->setGain(gainCombo_->currentData().toDouble());

    int bw = bwCombo_->findData(s.value("play/bandwidth", 8000.0).toDouble());
    if (bw >= 0)
    {
        bwCombo_->setCurrentIndex(bw);
    }
    engine_->setBandwidth(bandwidthHz());

    volumeSlider_->setValue(s.value("play/volume", 70).toInt());
    engine_->setVolume(volumeSlider_->value() / 100.0);

    freqSpin_->blockSignals(true);
    freqSpin_->setValue(s.value("play/freqKHz", 999.0).toDouble());
    freqSpin_->blockSignals(false);

    // Stations of the last scan, so it is possible to listen without rescanning
    stations_.clear();
    for (const QString &line : s.value("stations/list").toStringList())
    {
        QStringList parts = line.split(';');
        if (parts.size() == 3)
        {
            stations_.push_back({parts[0].toDouble(), parts[1].toFloat(), parts[2].toFloat()});
        }
    }
    showStations();

    restoreGeometry(s.value("window/geometry").toByteArray());
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue("scan/band", bandCombo_->currentIndex());
    s.setValue("scan/from", fromSpin_->value());
    s.setValue("scan/to", toSpin_->value());
    s.setValue("scan/grid", gridHz());
    s.setValue("scan/threshold", thresholdSpin_->value());
    s.setValue("sdr/gain", gainCombo_->currentData().toDouble());
    s.setValue("play/bandwidth", bandwidthHz());
    s.setValue("play/volume", volumeSlider_->value());
    s.setValue("play/freqKHz", freqSpin_->value());

    QStringList list;
    for (const auto &st : stations_)
    {
        list << QString("%1;%2;%3").arg(st.freqHz, 0, 'f', 0).arg(st.levelDb, 0, 'f', 1).arg(st.snrDb, 0, 'f', 1);
    }
    s.setValue("stations/list", list);
    s.setValue("window/geometry", saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    engine_->abortScan();
    engine_->stop();
    saveSettings();
    event->accept();
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

double MainWindow::gridHz() const
{
    return gridCombo_->currentData().toDouble();
}

double MainWindow::bandwidthHz() const
{
    return bwCombo_->currentData().toDouble();
}

ScanParams MainWindow::currentParams() const
{
    ScanParams p;
    p.startHz = std::min(fromSpin_->value(), toSpin_->value()) * 1e3;
    p.stopHz = std::max(fromSpin_->value(), toSpin_->value()) * 1e3;
    p.gridHz = gridHz();
    p.thresholdDb = thresholdSpin_->value();
    return p;
}

void MainWindow::onBandChanged(int index)
{
    const auto &bands = bandPlan();
    if (index < 0 || index >= static_cast<int>(bands.size()))
    {
        return;   // custom: keep user values
    }
    const Band &b = bands[static_cast<size_t>(index)];
    fromSpin_->setValue(b.startKHz);
    toSpin_->setValue(b.stopKHz);
    int g = gridCombo_->findData(b.gridKHz * 1000.0);
    if (g >= 0)
    {
        gridCombo_->setCurrentIndex(g);
    }
}

void MainWindow::setScanControlsEnabled(bool enabled)
{
    for (QWidget *w : std::initializer_list<QWidget *>{bandCombo_, fromSpin_, toSpin_, gridCombo_, gainCombo_,
                                                       freqSpin_, prevButton_, nextButton_, playButton_,
                                                       stationTable_})
    {
        w->setEnabled(enabled);
    }
}

void MainWindow::onScanClicked()
{
    if (engine_->isScanning())
    {
        engine_->abortScan();
        scanButton_->setText(tr("Зупиняю..."));
        scanButton_->setEnabled(false);
        return;
    }

    lastParams_ = currentParams();
    if (lastParams_.stopHz - lastParams_.startHz < lastParams_.gridHz)
    {
        onError(tr("Діапазон сканування занадто вузький"));
        return;
    }

    setScanControlsEnabled(false);
    scanButton_->setText(tr("Зупинити"));
    scanProgress_->setValue(0);
    statusBar()->showMessage(tr("Сканування %1–%2 кГц...")
                                 .arg(lastParams_.startHz / 1e3, 0, 'f', 0)
                                 .arg(lastParams_.stopHz / 1e3, 0, 'f', 0));
    engine_->startScan(lastParams_);
}

void MainWindow::onScanProgress(int percent)
{
    scanProgress_->setValue(percent);
}

void MainWindow::onScanFinished(const ScanResult &result, bool aborted)
{
    setScanControlsEnabled(true);
    scanButton_->setEnabled(true);
    scanButton_->setText(tr("Сканувати"));

    if (!result.spectrumDb.empty())
    {
        lastScan_ = result;
        stations_ = result.stations;
        bandSpectrum_->setSpectrum(QVector<float>(result.spectrumDb.begin(), result.spectrumDb.end()),
                                   result.startHz, result.binHz);
        showStations();
        saveSettings();
    }

    statusBar()->showMessage(aborted ? tr("Сканування перервано")
                                     : tr("Знайдено станцій: %1").arg(stations_.size()), 10000);
}

void MainWindow::onThresholdChanged()
{
    if (lastScan_.spectrumDb.empty())
    {
        return;
    }
    ScanParams p = lastParams_;
    p.thresholdDb = thresholdSpin_->value();
    stations_ = detectStations(lastScan_, p);
    showStations();
}

void MainWindow::showStations()
{
    stationTable_->setSortingEnabled(false);
    stationTable_->setRowCount(static_cast<int>(stations_.size()));

    QVector<double> markers;
    for (size_t i = 0; i < stations_.size(); ++i)
    {
        const Station &st = stations_[i];
        int row = static_cast<int>(i);
        auto *freq = numberItem(st.freqHz / 1e3, 1);
        freq->setData(Qt::UserRole, st.freqHz);
        stationTable_->setItem(row, 0, freq);
        stationTable_->setItem(row, 1, numberItem(st.levelDb, 1));
        stationTable_->setItem(row, 2, numberItem(st.snrDb, 1));
        markers.push_back(st.freqHz);
    }

    stationTable_->setSortingEnabled(true);
    bandSpectrum_->setMarkers(markers);
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void MainWindow::tune(double freqHz)
{
    freqHz = std::clamp(freqHz, minKHz * 1e3, maxKHz * 1e3);
    freqSpin_->blockSignals(true);
    freqSpin_->setValue(freqHz / 1e3);
    freqSpin_->blockSignals(false);
    engine_->play(freqHz);
}

void MainWindow::onStationActivated(int row)
{
    QTableWidgetItem *item = stationTable_->item(row, 0);
    if (item)
    {
        tune(item->data(Qt::UserRole).toDouble());
    }
}

void MainWindow::onSpectrumClicked(double hz)
{
    if (engine_->isScanning())
    {
        return;
    }
    double grid = gridHz();
    tune(std::round(hz / grid) * grid);
}

void MainWindow::onPlayClicked()
{
    if (engine_->isPlaying())
    {
        engine_->stop();
    }
    else
    {
        tune(freqSpin_->value() * 1e3);
    }
}

void MainWindow::onFrequencyEdited(double khz)
{
    if (engine_->isPlaying())
    {
        engine_->play(khz * 1e3);
    }
}

void MainWindow::onPrevNext(int direction)
{
    if (stations_.empty())
    {
        return;
    }

    std::vector<double> freqs;
    for (const auto &st : stations_)
    {
        freqs.push_back(st.freqHz);
    }
    std::sort(freqs.begin(), freqs.end());

    const double current = freqSpin_->value() * 1e3;
    double target;
    if (direction > 0)
    {
        auto it = std::upper_bound(freqs.begin(), freqs.end(), current + 1.0);
        target = it != freqs.end() ? *it : freqs.front();
    }
    else
    {
        auto it = std::lower_bound(freqs.begin(), freqs.end(), current - 1.0);
        target = it != freqs.begin() ? *std::prev(it) : freqs.back();
    }
    tune(target);
}

void MainWindow::onPlaybackChanged(bool playing, double freqHz)
{
    playButton_->setText(playing ? tr("Стоп") : tr("Слухати"));

    if (!playing)
    {
        liveSpectrum_->clear();
        bandSpectrum_->setHighlight(0.0, 0.0);
        levelBar_->setValue(-100);
        levelLabel_->setText(tr("— dBFS"));
        setWindowTitle(tr("AM радіо — RTL-SDR"));
        return;
    }

    bandSpectrum_->setHighlight(freqHz, bandwidthHz());
    saveSettings();
    setWindowTitle(tr("%1 кГц — AM радіо").arg(freqHz / 1e3, 0, 'f', 1));

    // Select matching station in the table
    stationTable_->clearSelection();
    for (int row = 0; row < stationTable_->rowCount(); ++row)
    {
        QTableWidgetItem *item = stationTable_->item(row, 0);
        if (item && std::fabs(item->data(Qt::UserRole).toDouble() - freqHz) < 1.0)
        {
            stationTable_->selectRow(row);
            stationTable_->scrollToItem(item);
            break;
        }
    }
}

void MainWindow::onLiveSpectrum(const QVector<float> &db, double startHz, double binHz)
{
    if (!engine_->isPlaying())
    {
        return;
    }
    liveSpectrum_->setSpectrum(db, startHz, binHz);
    liveSpectrum_->setHighlight(engine_->frequency(), bandwidthHz());
}

void MainWindow::onLevel(float dbfs)
{
    levelBar_->setValue(std::clamp(static_cast<int>(std::lround(dbfs)), -100, 0));
    levelLabel_->setText(tr("%1 dBFS").arg(dbfs, 0, 'f', 1));
}

void MainWindow::onError(const QString &message)
{
    statusBar()->showMessage(tr("Помилка: %1").arg(message), 10000);
}

void MainWindow::onStatusTimer()
{
    if (!engine_->isStreamAlive())
    {
        engine_->stop();
        onError(tr("USB-потік зупинився. Перевірте підключення донгла"));
    }

    QString text;
    if (!audioError_.isEmpty())
    {
        text = tr("Звук: %1").arg(audioError_);
    }
    else if (!audio_->isRunning())
    {
        text = tr("Звук: зупинено (програвач закрито?)");
    }
    else
    {
        text = tr("Звук: %1, буфер %2 мс")
                   .arg(QString::fromStdString(audio_->backendName()))
                   .arg(audio_->bufferedMs());
    }
    audioLabel_->setText(text);
}
