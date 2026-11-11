#include "MainWindow.h"

#include "AudioOutput.h"
#include "BandPlan.h"
#include "FrequencyView.h"
#include "SpectrumWidget.h"
#include "WaterfallWidget.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double minKHz = 100.0;
constexpr double maxKHz = 1766000.0;   // R828D upper limit
constexpr double remoteSpectrumFps = 5.0;
constexpr double localSpectrumFps = 25.0;

QDoubleSpinBox *makeKHzSpin(int decimals)
{
    auto *spin = new QDoubleSpinBox;
    spin->setRange(minKHz, maxKHz);
    spin->setDecimals(decimals);
    spin->setSuffix(QObject::tr(" кГц"));
    spin->setKeyboardTracking(false);
    spin->setAccelerated(true);
    spin->setGroupSeparatorShown(true);
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

QString formatBytes(double bytes)
{
    if (bytes >= 1e9)
    {
        return QString("%1 ГБ").arg(bytes / 1e9, 0, 'f', 2);
    }
    return QString("%1 МБ").arg(bytes / 1e6, 0, 'f', 1);
}

QString formatRate(double hz)
{
    return hz >= 1e6 ? QString("%1 МГц").arg(hz / 1e6, 0, 'f', 3) : QString("%1 кГц").arg(hz / 1e3, 0, 'f', 0);
}
} // namespace

MainWindow::MainWindow(RadioController *radio, core::AudioOutput *audio, const QString &audioError, QWidget *parent)
    : QMainWindow(parent), radio_(radio), audio_(audio), audioError_(audioError)
{
    setWindowTitle(tr("AM радіо — RTL-SDR"));

    // ---------- Left: controls ----------
    auto *panel = new QWidget;
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->addWidget(buildViewGroup());
    panelLayout->addWidget(buildReceiverGroup());
    panelLayout->addWidget(buildSweepGroup());

    auto *tabs = new QTabWidget;
    tabs->addTab(buildStationsTab(), tr("Станції"));
    tabs->addTab(buildRecordGroup(), tr("Запис IQ"));
    tabs->addTab(buildNetworkGroup(), tr("Мережа"));
    panelLayout->addWidget(tabs, 1);

    auto *scroll = new QScrollArea;
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(370);
    scroll->setMaximumWidth(420);

    // ---------- Right: spectrum over waterfall, same frequency axis ----------
    view_ = new FrequencyView(this);
    spectrum_ = new SpectrumWidget(view_);
    waterfall_ = new WaterfallWidget(view_);
    spectrum_->setToolTip(tr("Колесо — масштаб, перетягування — зсув, клік — слухати, подвійний клік — усе вікно"));
    waterfall_->setToolTip(spectrum_->toolTip());

    // X11 over the network (ssh -Y: DISPLAY=localhost:10.0) sends every redrawn spectrum as a picture
    const QByteArray display = qgetenv("DISPLAY");
    const bool remoteX = QGuiApplication::platformName() == "xcb" && !display.isEmpty() && !display.startsWith(':');
    spectrum_->setMaxRefreshRate(remoteX ? remoteSpectrumFps : localSpectrumFps);

    auto *plots = new QSplitter(Qt::Vertical);
    plots->addWidget(spectrum_);
    plots->addWidget(waterfall_);
    plots->setStretchFactor(0, 2);
    plots->setStretchFactor(1, 3);

    auto *main = new QSplitter(Qt::Horizontal);
    main->addWidget(scroll);
    main->addWidget(plots);
    main->setStretchFactor(1, 1);
    setCentralWidget(main);

    if (remoteX)
    {
        statusBar()->addPermanentWidget(new QLabel(tr("X11 по мережі: спектр ≤ %1 к/с").arg(remoteSpectrumFps)));
    }
    sourceLabel_ = new QLabel(radio_->sourceDescription());
    audioLabel_ = new QLabel;
    statusBar()->addPermanentWidget(sourceLabel_);
    statusBar()->addPermanentWidget(audioLabel_);
    resize(1400, 900);

    // ---------- Wiring ----------
    connect(radio_, &RadioController::modeChanged, this, &MainWindow::onModeChanged);
    connect(radio_, &RadioController::spectrumFrame, this, &MainWindow::onSpectrumFrame);
    connect(radio_, &RadioController::sweepFinished, this, &MainWindow::onSweepFinished);
    connect(radio_, &RadioController::sweepProgress, sweepProgress_, &QProgressBar::setValue);
    connect(radio_, &RadioController::tuningChanged, this, &MainWindow::onTuningChanged);
    connect(radio_, &RadioController::errorOccurred, this,
            [this](const QString &m) { statusBar()->showMessage(tr("Помилка: %1").arg(m), 10000); });
    connect(radio_, &RadioController::gainChanged, this, [this](double db) {
        int i = gainCombo_->findData(db);
        if (i >= 0)
        {
            QSignalBlocker block(gainCombo_);
            gainCombo_->setCurrentIndex(i);
        }
    });
    connect(spectrum_, &FrequencyPlot::frequencyClicked, this, &MainWindow::onPlotClicked);
    connect(waterfall_, &FrequencyPlot::frequencyClicked, this, &MainWindow::onPlotClicked);

    statusTimer_ = new QTimer(this);
    connect(statusTimer_, &QTimer::timeout, this, &MainWindow::onStatusTimer);
    statusTimer_->start(250);

    loadSettings();
    onModeChanged(radio_->mode());
    onTuningChanged(radio_->center(), radio_->span(), radio_->vfo());
    onStatusTimer();
}

MainWindow::~MainWindow()
{
    saveSettings();
}

// ---------------------------------------------------------------------------
// Building the panel
// ---------------------------------------------------------------------------

QWidget *MainWindow::buildViewGroup()
{
    auto *box = new QGroupBox(tr("Огляд ефіру"));
    auto *form = new QFormLayout(box);

    liveButton_ = new QPushButton;
    connect(liveButton_, &QPushButton::clicked, this, [this] {
        if (radio_->mode() == RadioController::Mode::Idle)
        {
            radio_->startLive();
        }
        else
        {
            radio_->stop();
        }
    });
    form->addRow(liveButton_);

    centerSpin_ = makeKHzSpin(1);
    centerSpin_->setSingleStep(100.0);
    centerSpin_->setToolTip(tr("Центральна частота вікна огляду"));
    connect(centerSpin_, &QDoubleSpinBox::valueChanged, this, [this](double khz) { radio_->setCenter(khz * 1e3); });
    form->addRow(tr("Центр:"), centerSpin_);

    spanCombo_ = new QComboBox;
    for (double s : RadioController::supportedSpans())
    {
        spanCombo_->addItem(formatRate(s), s);
    }
    spanCombo_->setToolTip(tr("Ширина вікна огляду = частота дискретизації донгла.\n"
                              "Ширший діапазон — кнопка «Панорама» нижче"));
    connect(spanCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { radio_->setSpan(spanCombo_->currentData().toDouble()); });
    form->addRow(tr("Ширина вікна:"), spanCombo_);

    lineRateCombo_ = new QComboBox;
    for (int lps : {5, 10, 20, 30})
    {
        lineRateCombo_->addItem(tr("%1 ліній/с").arg(lps), lps);
    }
    lineRateCombo_->setToolTip(tr("Швидкість водоспаду. Усі семпли між лініями усереднюються,\n"
                                  "тож повільніше = чутливіше. Через SSH на слабкому каналі ставте 5–10"));
    connect(lineRateCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { radio_->setLineRate(lineRateCombo_->currentData().toDouble()); });
    form->addRow(tr("Водоспад:"), lineRateCombo_);

    gainCombo_ = new QComboBox;
    gainCombo_->addItem(tr("Авто"), -1.0);
    for (double g : radio_->supportedGains())
    {
        gainCombo_->addItem(tr("%1 дБ").arg(g, 0, 'f', 1), g);
    }
    connect(gainCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { radio_->setGain(gainCombo_->currentData().toDouble()); });
    form->addRow(tr("Підсилення:"), gainCombo_);

    return box;
}

QWidget *MainWindow::buildReceiverGroup()
{
    auto *box = new QGroupBox(tr("АМ-приймач"));
    auto *form = new QFormLayout(box);

    vfoSpin_ = makeKHzSpin(1);
    vfoSpin_->setSingleStep(1.0);
    QFont big = vfoSpin_->font();
    big.setPointSizeF(big.pointSizeF() * 1.3);
    big.setBold(true);
    vfoSpin_->setFont(big);
    connect(vfoSpin_, &QDoubleSpinBox::valueChanged, this, [this](double khz) { tune(khz * 1e3); });

    prevButton_ = new QPushButton(tr("◀"));
    nextButton_ = new QPushButton(tr("▶"));
    prevButton_->setToolTip(tr("Попередня станція"));
    nextButton_->setToolTip(tr("Наступна станція"));
    prevButton_->setMaximumWidth(36);
    nextButton_->setMaximumWidth(36);
    connect(prevButton_, &QPushButton::clicked, this, [this] { onPrevNext(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { onPrevNext(+1); });
    auto *freqRow = new QHBoxLayout;
    freqRow->addWidget(vfoSpin_, 1);
    freqRow->addWidget(prevButton_);
    freqRow->addWidget(nextButton_);
    form->addRow(tr("Частота:"), freqRow);

    listenButton_ = new QPushButton(tr("Слухати"));
    listenButton_->setCheckable(true);
    connect(listenButton_, &QPushButton::toggled, this, [this](bool on) {
        radio_->setListening(on);
        listenButton_->setText(on ? tr("Вимкнути звук") : tr("Слухати"));
        if (on && radio_->mode() == RadioController::Mode::Idle)
        {
            radio_->startLive();
        }
    });
    form->addRow(listenButton_);

    bwCombo_ = new QComboBox;
    for (int khz : {3, 4, 5, 6, 7, 8, 9, 10})
    {
        bwCombo_->addItem(tr("%1 кГц").arg(khz), khz * 1000.0);
    }
    connect(bwCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        radio_->setBandwidth(bandwidthHz());
        updateChannelOverlay();
    });
    form->addRow(tr("Смуга:"), bwCombo_);

    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setRange(0, 100);
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int v) { radio_->setVolume(v / 100.0); });
    form->addRow(tr("Гучність:"), volumeSlider_);

    levelBar_ = new QProgressBar;
    levelBar_->setRange(-100, 0);
    levelBar_->setTextVisible(false);
    levelLabel_ = new QLabel(tr("— dBFS"));
    levelLabel_->setMinimumWidth(80);
    auto *levelRow = new QHBoxLayout;
    levelRow->addWidget(levelBar_, 1);
    levelRow->addWidget(levelLabel_);
    form->addRow(tr("Рівень:"), levelRow);

    return box;
}

QWidget *MainWindow::buildSweepGroup()
{
    auto *box = new QGroupBox(tr("Панорама і пошук станцій"));
    auto *layout = new QVBoxLayout(box);
    auto *form = new QFormLayout;

    bandCombo_ = new QComboBox;
    for (const auto &band : bandPlan())
    {
        bandCombo_->addItem(QString::fromUtf8(band.name));
    }
    bandCombo_->addItem(tr("Довільний"));
    connect(bandCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onBandChanged);
    form->addRow(tr("Діапазон:"), bandCombo_);

    fromSpin_ = makeKHzSpin(0);
    toSpin_ = makeKHzSpin(0);
    auto *rangeRow = new QHBoxLayout;
    rangeRow->addWidget(fromSpin_);
    rangeRow->addWidget(new QLabel(tr("—")));
    rangeRow->addWidget(toSpin_);
    form->addRow(tr("Межі:"), rangeRow);

    gridCombo_ = new QComboBox;
    for (int khz : {1, 5, 9, 10})
    {
        gridCombo_->addItem(tr("%1 кГц").arg(khz), khz * 1000.0);
    }
    thresholdSpin_ = new QDoubleSpinBox;
    thresholdSpin_->setRange(3.0, 40.0);
    thresholdSpin_->setDecimals(0);
    thresholdSpin_->setSuffix(tr(" дБ"));
    thresholdSpin_->setToolTip(tr("На скільки несуча має бути вище рівня шуму"));
    connect(thresholdSpin_, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!lastSweep_.power.empty())
        {
            core::ScanParams p = lastParams_;
            p.thresholdDb = thresholdSpin_->value();
            stations_ = core::detectStations(lastSweep_, p);
            showStations();
        }
    });
    auto *gridRow = new QHBoxLayout;
    gridRow->addWidget(gridCombo_);
    gridRow->addWidget(new QLabel(tr("Поріг:")));
    gridRow->addWidget(thresholdSpin_);
    form->addRow(tr("Крок:"), gridRow);
    layout->addLayout(form);

    scanButton_ = new QPushButton(tr("Знайти станції"));
    scanButton_->setToolTip(tr("Один ретельний прохід діапазону з пошуком несучих"));
    panoramaButton_ = new QPushButton(tr("Панорама ⟳"));
    panoramaButton_->setToolTip(tr("Безперервний огляд довільно широкого діапазону:\n"
                                   "донгл перебудовується по вікнах, кожен прохід = рядок водоспаду.\n"
                                   "Звук і запис у цьому режимі недоступні"));
    connect(scanButton_, &QPushButton::clicked, this, [this] {
        lastParams_ = sweepParams();
        radio_->startSweep(lastParams_, false);
    });
    connect(panoramaButton_, &QPushButton::clicked, this, [this] {
        if (radio_->isContinuousSweep())
        {
            radio_->stop();
            return;
        }
        lastParams_ = sweepParams();
        radio_->startSweep(lastParams_, true);
    });
    sweepProgress_ = new QProgressBar;
    sweepProgress_->setRange(0, 100);
    sweepProgress_->setValue(0);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(scanButton_);
    buttons->addWidget(panoramaButton_);
    layout->addLayout(buttons);
    layout->addWidget(sweepProgress_);


    return box;
}

QWidget *MainWindow::buildStationsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 4);
    stationTable_ = new QTableWidget(0, 3);
    stationTable_->setHorizontalHeaderLabels({tr("Частота, кГц"), tr("Рівень, дБ"), tr("С/Ш, дБ")});
    stationTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    stationTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    stationTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stationTable_->verticalHeader()->setVisible(false);
    stationTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    stationTable_->setSortingEnabled(true);
    stationTable_->setMinimumHeight(120);
    stationTable_->setToolTip(tr("Подвійний клік / Enter — слухати"));
    connect(stationTable_, &QTableWidget::cellActivated, this, [this](int row, int) {
        if (QTableWidgetItem *item = stationTable_->item(row, 0))
        {
            onPlotClicked(item->data(Qt::UserRole).toDouble());
        }
    });
    layout->addWidget(stationTable_);
    return page;
}

QWidget *MainWindow::buildRecordGroup()
{
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);
    layout->addWidget(new QLabel(tr("Формат SigMF: сирі cu8-семпли донгла + JSON-метадані")));

    recordDirEdit_ = new QLineEdit;
    auto *browse = new QPushButton(tr("…"));
    browse->setMaximumWidth(32);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(this, tr("Папка для IQ-записів"), recordDirEdit_->text());
        if (!dir.isEmpty())
        {
            recordDirEdit_->setText(dir);
        }
    });
    auto *dirRow = new QHBoxLayout;
    dirRow->addWidget(new QLabel(tr("Папка:")));
    dirRow->addWidget(recordDirEdit_, 1);
    dirRow->addWidget(browse);
    layout->addLayout(dirRow);

    recordButton_ = new QPushButton;
    recordButton_->setToolTip(tr("Пише все вікно огляду як є (cu8, 2 байти на семпл).\n"
                                 "2.4 МГц = 4.8 МБ/с = 17 ГБ/год"));
    connect(recordButton_, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    layout->addWidget(recordButton_);

    recordStatus_ = new QLabel;
    recordStatus_->setWordWrap(true);
    recordStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(recordStatus_);
    return box;
}

QWidget *MainWindow::buildNetworkGroup()
{
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);
    layout->addWidget(new QLabel(tr("Сервер rtl_tcp: IQ-потік для SDR-програм на іншому хості")));

    portSpin_ = new QSpinBox;
    portSpin_->setRange(1024, 65535);
    allowControlCheck_ = new QCheckBox(tr("Дозволити керування"));
    allowControlCheck_->setToolTip(tr("Клієнт може змінювати частоту, ширину вікна і підсилення"));
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Порт:")));
    row->addWidget(portSpin_);
    row->addWidget(allowControlCheck_, 1);
    layout->addLayout(row);

    networkButton_ = new QPushButton;
    connect(networkButton_, &QPushButton::clicked, this, &MainWindow::toggleNetwork);
    layout->addWidget(networkButton_);

    networkStatus_ = new QLabel;
    networkStatus_->setWordWrap(true);
    networkStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(networkStatus_);
    return box;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void MainWindow::setSpinValue(QDoubleSpinBox *spin, double value)
{
    QSignalBlocker block(spin);
    spin->setValue(value);
}

void MainWindow::loadSettings()
{
    QSettings s;

    lineRateCombo_->setCurrentIndex(std::max(0, lineRateCombo_->findData(s.value("view/lineRate", 10).toInt())));
    radio_->setLineRate(lineRateCombo_->currentData().toDouble());

    int gain = gainCombo_->findData(s.value("sdr/gain", 29.7).toDouble());
    gainCombo_->setCurrentIndex(gain >= 0 ? gain : 0);
    radio_->setGain(gainCombo_->currentData().toDouble());

    int span = spanCombo_->findData(s.value("view/span", 1.152e6).toDouble());
    {
        QSignalBlocker block(spanCombo_);
        spanCombo_->setCurrentIndex(span >= 0 ? span : 2);
    }
    radio_->setSpan(spanCombo_->currentData().toDouble());
    radio_->tune(s.value("receiver/freq", 999e3).toDouble());

    bwCombo_->setCurrentIndex(std::max(0, bwCombo_->findData(s.value("receiver/bandwidth", 8000.0).toDouble())));
    radio_->setBandwidth(bandwidthHz());
    volumeSlider_->setValue(s.value("receiver/volume", 70).toInt());
    radio_->setVolume(volumeSlider_->value() / 100.0);

    bandCombo_->setCurrentIndex(std::clamp(s.value("sweep/band", 1).toInt(), 0, bandCombo_->count() - 1));
    onBandChanged(bandCombo_->currentIndex());
    if (s.contains("sweep/from"))
    {
        setSpinValue(fromSpin_, s.value("sweep/from").toDouble());
        setSpinValue(toSpin_, s.value("sweep/to").toDouble());
        int g = gridCombo_->findData(s.value("sweep/grid").toDouble());
        if (g >= 0)
        {
            gridCombo_->setCurrentIndex(g);
        }
    }
    setSpinValue(thresholdSpin_, s.value("sweep/threshold", 10.0).toDouble());

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

    recordDirEdit_->setText(s.value("record/dir", QDir::homePath() + "/iq_records").toString());
    portSpin_->setValue(s.value("network/port", 1234).toInt());
    allowControlCheck_->setChecked(s.value("network/allowControl", false).toBool());

    restoreGeometry(s.value("window/geometry").toByteArray());
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue("view/lineRate", lineRateCombo_->currentData());
    s.setValue("view/span", spanCombo_->currentData());
    s.setValue("sdr/gain", gainCombo_->currentData());
    s.setValue("receiver/freq", radio_->vfo());
    s.setValue("receiver/bandwidth", bandwidthHz());
    s.setValue("receiver/volume", volumeSlider_->value());
    s.setValue("sweep/band", bandCombo_->currentIndex());
    s.setValue("sweep/from", fromSpin_->value());
    s.setValue("sweep/to", toSpin_->value());
    s.setValue("sweep/grid", gridHz());
    s.setValue("sweep/threshold", thresholdSpin_->value());

    QStringList list;
    for (const auto &st : stations_)
    {
        list << QString("%1;%2;%3").arg(st.freqHz, 0, 'f', 0).arg(st.levelDb, 0, 'f', 1).arg(st.snrDb, 0, 'f', 1);
    }
    s.setValue("stations/list", list);
    s.setValue("record/dir", recordDirEdit_->text());
    s.setValue("network/port", portSpin_->value());
    s.setValue("network/allowControl", allowControlCheck_->isChecked());
    s.setValue("window/geometry", saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    radio_->stopRecording();
    radio_->stopNetwork();
    radio_->stop();
    event->accept();
}

// ---------------------------------------------------------------------------
// Radio events
// ---------------------------------------------------------------------------

void MainWindow::onModeChanged(RadioController::Mode mode)
{
    using Mode = RadioController::Mode;
    const bool sweep = mode == Mode::Sweep;
    const bool tunable = radio_->isTunable();

    liveButton_->setText(mode == Mode::Live ? tr("■ Зупинити огляд")
                         : sweep           ? tr("■ Зупинити панораму")
                                           : tr("▶ Старт огляду"));
    centerSpin_->setEnabled(!sweep && tunable);
    spanCombo_->setEnabled(!sweep && tunable);
    gainCombo_->setEnabled(!sweep && radio_->canSweep());
    scanButton_->setEnabled(!sweep && radio_->canSweep());
    panoramaButton_->setEnabled(radio_->canSweep() && (!sweep || radio_->isContinuousSweep()));
    panoramaButton_->setText(radio_->isContinuousSweep() ? tr("■ Стоп панорами") : tr("Панорама ⟳"));

    // Live lines and panorama lines have different extents: start a clean picture
    waterfall_->clear();
    spectrum_->clear();
    updateChannelOverlay();

    if (mode == Mode::Live && !std::isnan(pendingTuneHz_))
    {
        double f = pendingTuneHz_;
        pendingTuneHz_ = std::numeric_limits<double>::quiet_NaN();
        tune(f);
    }
    if (!sweep)
    {
        sweepProgress_->setValue(0);
    }
}

void MainWindow::onSpectrumFrame(const core::SpectrumFramePtr &frame)
{
    view_->setDataRange(frame->startHz, frame->stopHz());
    spectrum_->setFrame(frame);
    waterfall_->addFrame(frame);
}

void MainWindow::onSweepFinished(const core::ScanResult &result, bool aborted)
{
    if (aborted && result.power.empty())
    {
        return;
    }
    lastSweep_ = result;
    core::ScanParams p = lastParams_;
    p.thresholdDb = thresholdSpin_->value();
    stations_ = core::detectStations(lastSweep_, p);
    showStations();
    statusBar()->showMessage(aborted ? tr("Прохід перервано") : tr("Знайдено станцій: %1").arg(stations_.size()), 5000);
}

void MainWindow::onTuningChanged(double centerHz, double spanHz, double vfoHz)
{
    setSpinValue(centerSpin_, centerHz / 1e3);
    setSpinValue(vfoSpin_, vfoHz / 1e3);
    {
        QSignalBlocker block(spanCombo_);
        int i = spanCombo_->findData(spanHz);
        if (i >= 0)
        {
            spanCombo_->setCurrentIndex(i);
        }
    }
    updateChannelOverlay();
    setWindowTitle(tr("%1 кГц — AM радіо").arg(vfoHz / 1e3, 0, 'f', 1));

    stationTable_->clearSelection();
    for (int row = 0; row < stationTable_->rowCount(); ++row)
    {
        QTableWidgetItem *item = stationTable_->item(row, 0);
        if (item && std::fabs(item->data(Qt::UserRole).toDouble() - vfoHz) < 1.0)
        {
            stationTable_->selectRow(row);
            stationTable_->scrollToItem(item);
            break;
        }
    }
}

void MainWindow::updateChannelOverlay()
{
    const bool show = radio_->mode() == RadioController::Mode::Live;
    spectrum_->setChannel(radio_->vfo(), show ? bandwidthHz() : 0.0);
    waterfall_->setChannel(radio_->vfo(), show ? bandwidthHz() : 0.0);
}

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

void MainWindow::tune(double hz)
{
    radio_->tune(hz);
}

void MainWindow::onPlotClicked(double hz)
{
    const double grid = gridHz();
    const double f = std::round(hz / grid) * grid;

    switch (radio_->mode())
    {
    case RadioController::Mode::Sweep:
        // Click on the panorama: switch to live view around this frequency and listen
        pendingTuneHz_ = f;
        listenButton_->setChecked(true);
        radio_->startLive();
        break;
    case RadioController::Mode::Idle:
        tune(f);
        listenButton_->setChecked(true);
        radio_->startLive();
        break;
    case RadioController::Mode::Live:
        tune(f);
        break;
    }
}

void MainWindow::startListening(double freqHz)
{
    tune(freqHz);
    listenButton_->setChecked(true);
    radio_->startLive();
}

void MainWindow::startPanorama(double fromHz, double toHz)
{
    {
        QSignalBlocker block(bandCombo_);
        bandCombo_->setCurrentIndex(bandCombo_->count() - 1);   // custom
    }
    setSpinValue(fromSpin_, fromHz / 1e3);
    setSpinValue(toSpin_, toHz / 1e3);
    lastParams_ = sweepParams();
    radio_->startSweep(lastParams_, true);
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

    const double current = radio_->vfo();
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
    onPlotClicked(target);
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

double MainWindow::gridHz() const
{
    return gridCombo_->currentData().toDouble();
}

double MainWindow::bandwidthHz() const
{
    return bwCombo_->currentData().toDouble();
}

core::ScanParams MainWindow::sweepParams() const
{
    core::ScanParams p;
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
    setSpinValue(fromSpin_, b.startKHz);
    setSpinValue(toSpin_, b.stopKHz);
    int g = gridCombo_->findData(b.gridKHz * 1000.0);
    if (g >= 0)
    {
        gridCombo_->setCurrentIndex(g);
    }
}

void MainWindow::showStations()
{
    stationTable_->setSortingEnabled(false);
    stationTable_->setRowCount(static_cast<int>(stations_.size()));

    QVector<double> markers;
    for (size_t i = 0; i < stations_.size(); ++i)
    {
        const auto &st = stations_[i];
        const int row = static_cast<int>(i);
        auto *freq = numberItem(st.freqHz / 1e3, 1);
        freq->setData(Qt::UserRole, st.freqHz);
        stationTable_->setItem(row, 0, freq);
        stationTable_->setItem(row, 1, numberItem(st.levelDb, 1));
        stationTable_->setItem(row, 2, numberItem(st.snrDb, 1));
        markers.push_back(st.freqHz);
    }
    stationTable_->setSortingEnabled(true);
    spectrum_->setMarkers(markers);
}

// ---------------------------------------------------------------------------
// Recording and network
// ---------------------------------------------------------------------------

void MainWindow::toggleRecording()
{
    if (radio_->isRecording())
    {
        radio_->stopRecording();
    }
    else
    {
        QString dir = recordDirEdit_->text().trimmed();
        if (dir.startsWith("~"))
        {
            dir = QDir::homePath() + dir.mid(1);
        }
        QString error;
        if (!radio_->startRecording(dir, error))
        {
            statusBar()->showMessage(tr("Запис: %1").arg(error), 8000);
        }
        else if (radio_->mode() == RadioController::Mode::Idle)
        {
            radio_->startLive();
        }
    }
    onStatusTimer();
}

void MainWindow::toggleNetwork()
{
    if (radio_->networkStatus().running)
    {
        radio_->stopNetwork();
    }
    else
    {
        startNetwork(static_cast<quint16>(portSpin_->value()));
    }
    onStatusTimer();
}

void MainWindow::startNetwork(quint16 port)
{
    portSpin_->setValue(port);
    QString error;
    if (!radio_->startNetwork(port, allowControlCheck_->isChecked(), error))
    {
        statusBar()->showMessage(tr("rtl_tcp: %1").arg(error), 8000);
        return;
    }
    if (radio_->mode() == RadioController::Mode::Idle)
    {
        radio_->startLive();
    }
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void MainWindow::onStatusTimer()
{
    // Receiver level
    if (radio_->mode() == RadioController::Mode::Live)
    {
        float db = radio_->level();
        levelBar_->setValue(std::clamp(static_cast<int>(std::lround(db)), -100, 0));
        levelLabel_->setText(db <= -119.0f ? tr("поза вікном") : tr("%1 dBFS").arg(db, 0, 'f', 1));
    }
    else
    {
        levelBar_->setValue(-100);
        levelLabel_->setText(tr("— dBFS"));
    }

    // Recording
    const core::RecorderStatus rec = radio_->recordingStatus();
    recordButton_->setText(radio_->isRecording() ? tr("■ Зупинити запис") : tr("● Записати IQ"));
    portSpin_->setEnabled(!radio_->networkStatus().running);
    if (radio_->isRecording() || !rec.dataPath.empty() || !rec.error.empty())
    {
        QString text;
        if (radio_->isRecording() && !rec.writing)
        {
            text = radio_->mode() == RadioController::Mode::Live ? tr("відкриваю файл…")
                                                                  : tr("очікую огляд ефіру (запис іде лише в режимі огляду)");
        }
        else
        {
            text = tr("%1\n%2, %3 с%4")
                       .arg(QString::fromStdString(rec.dataPath))
                       .arg(formatBytes(static_cast<double>(rec.bytes)))
                       .arg(rec.seconds, 0, 'f', 1)
                       .arg(rec.lostSamples ? tr(", втрачено %1 семплів").arg(rec.lostSamples) : QString());
            if (rec.freeBytes > 0 && radio_->isRecording())
            {
                text += tr(", вільно %1").arg(formatBytes(static_cast<double>(rec.freeBytes)));
            }
        }
        if (!rec.error.empty())
        {
            text += tr("\nПомилка: %1").arg(QString::fromStdString(rec.error));
        }
        recordStatus_->setText(text);
    }

    // Network
    const auto net = radio_->networkStatus();
    networkButton_->setText(net.running ? tr("■ Зупинити сервер") : tr("Запустити сервер rtl_tcp"));
    if (net.running)
    {
        QString text = tr("Слухаю порт %1, клієнтів: %2").arg(net.port).arg(net.clients.size());
        for (const auto &c : net.clients)
        {
            text += tr("\n%1: %2 передано%3")
                        .arg(QString::fromStdString(c.address))
                        .arg(formatBytes(static_cast<double>(c.bytesSent)))
                        .arg(c.bytesDropped ? tr(", скинуто %1").arg(formatBytes(static_cast<double>(c.bytesDropped)))
                                            : QString());
        }
        networkStatus_->setText(text);
    }
    else
    {
        networkStatus_->setText(tr("Клієнт (SDR++, GQRX, SDR#): джерело rtl_tcp, адреса <IP Пі>:%1,\n"
                                   "частота дискретизації = ширина вікна")
                                    .arg(portSpin_->value()));
    }

    // Audio
    QString audioText;
    if (!audioError_.isEmpty())
    {
        audioText = tr("Звук: %1").arg(audioError_);
    }
    else if (!audio_->isRunning())
    {
        audioText = tr("Звук: зупинено (програвач закрито?)");
    }
    else
    {
        audioText = tr("Звук: %1, буфер %2 мс")
                        .arg(QString::fromStdString(audio_->backendName()))
                        .arg(audio_->bufferedMs());
    }
    audioLabel_->setText(audioText);
}
