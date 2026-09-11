// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.hpp"
#include "mapview.hpp"
#include "time_utils.hpp"

#include <QWidget>
#include <QDoubleSpinBox>
#include <QDateEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>
#include <cmath>

namespace {

double jdFromQDate(const QDate& d, double hour = 0.0) {
    mg::CalendarUTC t{d.year(), d.month(), d.day(), 0, 0, hour * 3600.0};
    return mg::jd_tt_from_utc(t);
}

// Same Meeus JD->Gregorian-calendar algorithm as main_cli.cpp's jd_to_string
// (kept as its own small copy here rather than shared, to keep graze_core a
// pure computation library with no GUI-string-formatting concerns).
QString jdToDisplayString(double jd_tt) {
    double jd_utc = jd_tt - 69.184 / 86400.0;
    double jd = jd_utc + 0.5;
    long Z = static_cast<long>(std::floor(jd));
    double F = jd - Z;
    long A;
    if (Z < 2299161) {
        A = Z;
    } else {
        long alpha = static_cast<long>(std::floor((Z - 1867216.25) / 36524.25));
        A = Z + 1 + alpha - alpha / 4;
    }
    long B = A + 1524;
    long C = static_cast<long>(std::floor((B - 122.1) / 365.25));
    long D = static_cast<long>(std::floor(365.25 * C));
    long E = static_cast<long>(std::floor((B - D) / 30.6001));
    double day_frac = B - D - std::floor(30.6001 * E) + F;
    int day = static_cast<int>(day_frac);
    double hh = (day_frac - day) * 24.0;
    long month = (E < 14) ? (E - 1) : (E - 13);
    long year = (month > 2) ? (C - 4716) : (C - 4715);
    int hour = static_cast<int>(hh);
    double mm = (hh - hour) * 60.0;
    int minute = static_cast<int>(mm);
    double ss = (mm - minute) * 60.0;
    if (ss >= 59.9995) { ss = 0.0; minute += 1; }
    return QString("%1-%2-%3 %4:%5:%6 UT")
        .arg(year, 4, 10, QChar('0')).arg(month, 2, 10, QChar('0')).arg(day, 2, 10, QChar('0'))
        .arg(hour, 2, 10, QChar('0')).arg(minute, 2, 10, QChar('0'))
        .arg(static_cast<int>(ss + 0.5), 2, 10, QChar('0'));
}

QString escapeXml(const QString& s) {
    QString out = s;
    out.replace('&', QStringLiteral("&amp;"));
    out.replace('<', QStringLiteral("&lt;"));
    out.replace('>', QStringLiteral("&gt;"));
    return out;
}

} // namespace

void SearchWorker::runSearch(SearchParams params) {
    cancelled_ = false;
    try {
        if (!eph_ || ephPath_ != params.eph_path) {
            emit logMessage(QStringLiteral("Loading ephemeris table %1 ...").arg(params.eph_path));
            eph_ = std::make_unique<gg::AreaEphemeris>(params.eph_path.toStdString());
            ephPath_ = params.eph_path;
        }
        if (stars_.empty() || catalogPath_ != params.catalog_path || params.mag_limit > catalogMagLimit_) {
            emit logMessage(QStringLiteral("Loading star catalogue %1 ...").arg(params.catalog_path));
            // load a little beyond the requested limit so a later, slightly
            // dimmer request doesn't force a reload
            double loadLimit = params.mag_limit + 0.5;
            stars_ = gg::load_hyg_catalog(params.catalog_path.toStdString(), loadLimit);
            catalogPath_ = params.catalog_path;
            catalogMagLimit_ = loadLimit;
        }

        std::vector<mg::Star> candidates;
        for (auto& s : stars_)
            if (s.vmag <= params.mag_limit) candidates.push_back(s);
        emit logMessage(QStringLiteral("%1 candidate stars (V<=%2)")
                             .arg(candidates.size()).arg(params.mag_limit, 0, 'f', 1));

        auto events = gg::find_grazes(
            params.site, *eph_, candidates, params.jd_start, params.jd_end, params.filters,
            [this](int done, int total) {
                emit progress(done, total);
                return !cancelled_;
            },
            [this](const std::string& msg) { emit logMessage(QString::fromStdString(msg)); });

        if (cancelled_) {
            emit logMessage(QStringLiteral("Search cancelled."));
        }
        emit finished(events);
    } catch (const std::exception& e) {
        emit failed(QString::fromStdString(e.what()));
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    qRegisterMetaType<std::vector<gg::GrazeEvent>>("std::vector<gg::GrazeEvent>");
    qRegisterMetaType<SearchParams>("SearchParams");

    worker_ = new SearchWorker();
    worker_->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &MainWindow::destroyed, &workerThread_, &QThread::quit);
    connect(worker_, &SearchWorker::progress, this, &MainWindow::onSearchProgress);
    connect(worker_, &SearchWorker::logMessage, this, &MainWindow::onLogMessage);
    connect(worker_, &SearchWorker::finished, this, &MainWindow::onSearchFinished);
    connect(worker_, &SearchWorker::failed, this, &MainWindow::onSearchFailed);
    workerThread_.start();

    buildUi();
    setWindowTitle(QStringLiteral("LunarGraze — Grazing Lunar Occultation Finder"));
    resize(1280, 820);
}

MainWindow::~MainWindow() {
    workerThread_.quit();
    workerThread_.wait(3000);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(central);

    // ---- left: controls ----
    auto* controlsPanel = new QWidget(this);
    controlsPanel->setMaximumWidth(340);
    auto* controlsLayout = new QVBoxLayout(controlsPanel);

    auto* siteBox = new QGroupBox(QStringLiteral("Observer site"), this);
    auto* siteForm = new QFormLayout(siteBox);
    latSpin_ = new QDoubleSpinBox(this); latSpin_->setRange(-90, 90); latSpin_->setDecimals(4);
    latSpin_->setValue(50.87);
    lonSpin_ = new QDoubleSpinBox(this); lonSpin_->setRange(-180, 180); lonSpin_->setDecimals(4);
    lonSpin_->setValue(5.43);
    elevSpin_ = new QDoubleSpinBox(this); elevSpin_->setRange(-500, 9000); elevSpin_->setValue(100);
    elevSpin_->setSuffix(QStringLiteral(" m"));
    siteForm->addRow(QStringLiteral("Latitude (deg N)"), latSpin_);
    siteForm->addRow(QStringLiteral("Longitude (deg E)"), lonSpin_);
    siteForm->addRow(QStringLiteral("Elevation"), elevSpin_);
    controlsLayout->addWidget(siteBox);

    auto* searchBox = new QGroupBox(QStringLiteral("Search"), this);
    auto* searchForm = new QFormLayout(searchBox);
    radiusSpin_ = new QDoubleSpinBox(this); radiusSpin_->setRange(1, 5000); radiusSpin_->setValue(100);
    radiusSpin_->setSuffix(QStringLiteral(" km"));
    magSpin_ = new QDoubleSpinBox(this); magSpin_->setRange(-1, 8); magSpin_->setDecimals(1);
    magSpin_->setValue(4.0);
    startDate_ = new QDateEdit(QDate::currentDate(), this); startDate_->setCalendarPopup(true);
    endDate_ = new QDateEdit(QDate::currentDate().addYears(1), this); endDate_->setCalendarPopup(true);
    searchForm->addRow(QStringLiteral("Radius (B)"), radiusSpin_);
    searchForm->addRow(QStringLiteral("Mag. limit (A)"), magSpin_);
    searchForm->addRow(QStringLiteral("Start date"), startDate_);
    searchForm->addRow(QStringLiteral("End date"), endDate_);
    controlsLayout->addWidget(searchBox);

    auto* visBox = new QGroupBox(QStringLiteral("Visibility filters"), this);
    auto* visForm = new QFormLayout(visBox);
    moonAltCheck_ = new QCheckBox(QStringLiteral("Min. Moon altitude"), this); moonAltCheck_->setChecked(true);
    moonAltSpin_ = new QDoubleSpinBox(this); moonAltSpin_->setRange(-90, 90); moonAltSpin_->setValue(0);
    moonAltSpin_->setSuffix(QStringLiteral(" deg"));
    sunAltCheck_ = new QCheckBox(QStringLiteral("Max. Sun altitude"), this); sunAltCheck_->setChecked(true);
    sunAltSpin_ = new QDoubleSpinBox(this); sunAltSpin_->setRange(-90, 90); sunAltSpin_->setValue(0);
    sunAltSpin_->setSuffix(QStringLiteral(" deg"));
    minIllumCheck_ = new QCheckBox(QStringLiteral("Min. illuminated fraction"), this);
    minIllumSpin_ = new QDoubleSpinBox(this); minIllumSpin_->setRange(0, 1); minIllumSpin_->setDecimals(2);
    minIllumSpin_->setSingleStep(0.05); minIllumSpin_->setValue(0.0);
    maxIllumCheck_ = new QCheckBox(QStringLiteral("Max. illuminated fraction"), this);
    maxIllumSpin_ = new QDoubleSpinBox(this); maxIllumSpin_->setRange(0, 1); maxIllumSpin_->setDecimals(2);
    maxIllumSpin_->setSingleStep(0.05); maxIllumSpin_->setValue(1.0);
    visForm->addRow(moonAltCheck_, moonAltSpin_);
    visForm->addRow(sunAltCheck_, sunAltSpin_);
    visForm->addRow(minIllumCheck_, minIllumSpin_);
    visForm->addRow(maxIllumCheck_, maxIllumSpin_);
    controlsLayout->addWidget(visBox);

    auto* dataBox = new QGroupBox(QStringLiteral("Data files"), this);
    auto* dataForm = new QFormLayout(dataBox);
    auto* ephRow = new QWidget(this); auto* ephRowLay = new QHBoxLayout(ephRow);
    ephRowLay->setContentsMargins(0, 0, 0, 0);
    ephPathEdit_ = new QLineEdit(QStringLiteral("area_eph_2025_2030.txt"), this);
    auto* ephBrowse = new QPushButton(QStringLiteral("..."), this);
    ephBrowse->setMaximumWidth(30);
    ephRowLay->addWidget(ephPathEdit_); ephRowLay->addWidget(ephBrowse);
    connect(ephBrowse, &QPushButton::clicked, this, &MainWindow::onBrowseEphemeris);
    auto* catRow = new QWidget(this); auto* catRowLay = new QHBoxLayout(catRow);
    catRowLay->setContentsMargins(0, 0, 0, 0);
    catalogPathEdit_ = new QLineEdit(QStringLiteral("hygdata_v41.csv"), this);
    auto* catBrowse = new QPushButton(QStringLiteral("..."), this);
    catBrowse->setMaximumWidth(30);
    catRowLay->addWidget(catalogPathEdit_); catRowLay->addWidget(catBrowse);
    connect(catBrowse, &QPushButton::clicked, this, &MainWindow::onBrowseCatalog);
    dataForm->addRow(QStringLiteral("Ephemeris table"), ephRow);
    dataForm->addRow(QStringLiteral("Star catalogue"), catRow);
    controlsLayout->addWidget(dataBox);

    // OpenStreetMap's own tile server now blocks this app's traffic outright
    // (see README.md) -- the map uses MapTiler instead, which needs a free
    // per-user API key.
    auto* mapBox = new QGroupBox(QStringLiteral("Map tiles"), this);
    auto* mapForm = new QFormLayout(mapBox);
    auto* keyRow = new QWidget(this); auto* keyRowLay = new QHBoxLayout(keyRow);
    keyRowLay->setContentsMargins(0, 0, 0, 0);
    mapApiKeyEdit_ = new QLineEdit(this);
    mapApiKeyEdit_->setPlaceholderText(QStringLiteral("paste your free MapTiler API key here"));
    mapApiKeyEdit_->setToolTip(QStringLiteral(
        "Needed for the background map to load tiles. Free, no credit card:\n"
        "sign up at maptiler.com, copy your key from the account dashboard.\n"
        "Saved locally between runs -- never sent anywhere except MapTiler's\n"
        "own tile requests."));
    connect(mapApiKeyEdit_, &QLineEdit::editingFinished, this, &MainWindow::onApiKeyChanged);
    auto* getKeyButton = new QPushButton(QStringLiteral("Get a free key..."), this);
    connect(getKeyButton, &QPushButton::clicked, this, &MainWindow::onGetApiKeyClicked);
    keyRowLay->addWidget(mapApiKeyEdit_); keyRowLay->addWidget(getKeyButton);
    mapForm->addRow(QStringLiteral("MapTiler API key"), keyRow);
    controlsLayout->addWidget(mapBox);

    runButton_ = new QPushButton(QStringLiteral("Find grazes"), this);
    runButton_->setStyleSheet(QStringLiteral("font-weight: bold; padding: 8px;"));
    connect(runButton_, &QPushButton::clicked, this, &MainWindow::onRunClicked);
    controlsLayout->addWidget(runButton_);

    auto* exportRow = new QWidget(this);
    auto* exportRowLay = new QHBoxLayout(exportRow);
    exportRowLay->setContentsMargins(0, 0, 0, 0);
    exportKmlButton_ = new QPushButton(QStringLiteral("Export KML..."), this);
    exportKmlButton_->setToolTip(QStringLiteral(
        "Save the current results as a .kml file you can open directly in\n"
        "Google Earth, or import into Google My Maps / most GIS tools."));
    exportKmlButton_->setEnabled(false);
    connect(exportKmlButton_, &QPushButton::clicked, this, &MainWindow::onExportKml);
    exportCsvButton_ = new QPushButton(QStringLiteral("Export CSV..."), this);
    exportCsvButton_->setEnabled(false);
    connect(exportCsvButton_, &QPushButton::clicked, this, &MainWindow::onExportCsv);
    exportRowLay->addWidget(exportKmlButton_);
    exportRowLay->addWidget(exportCsvButton_);
    controlsLayout->addWidget(exportRow);

    statusLabel_ = new QLabel(QStringLiteral("Ready."), this);
    statusLabel_->setWordWrap(true);
    controlsLayout->addWidget(statusLabel_);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumHeight(140);
    log_->setPlaceholderText(QStringLiteral("Progress log..."));
    controlsLayout->addWidget(log_);

    controlsLayout->addStretch(1);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(controlsPanel);
    scrollArea->setWidgetResizable(true);
    scrollArea->setMaximumWidth(360);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // ---- right: table + map ----
    auto* rightSplitter = new QSplitter(Qt::Vertical, this);

    table_ = new QTableWidget(this);
    table_->setColumnCount(8);
    table_->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Star"), QStringLiteral("Mag"),
                                        QStringLiteral("Time (UT)"), QStringLiteral("Dist (km)"),
                                        QStringLiteral("Moon alt"), QStringLiteral("Sun alt"),
                                        QStringLiteral("Illum")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { onRowSelected(row); });

    mapView_ = new MapView(this);
    connect(mapView_, &MapView::tileLoadError, this, [this](const QString& msg) {
        if (msg.isEmpty()) {
            log_->appendPlainText(QStringLiteral("Map tiles loading OK."));
        } else {
            log_->appendPlainText(QStringLiteral("Map tile error: %1").arg(msg));
        }
    });

    {
        QSettings settings;
        QString savedKey = settings.value(QStringLiteral("mapTilerApiKey")).toString();
        if (!savedKey.isEmpty()) {
            mapApiKeyEdit_->setText(savedKey);
            mapView_->setApiKey(savedKey);
        }
    }

    rightSplitter->addWidget(table_);
    rightSplitter->addWidget(mapView_);
    rightSplitter->setStretchFactor(0, 0);
    rightSplitter->setStretchFactor(1, 1);
    rightSplitter->setSizes({220, 600});

    mainLayout->addWidget(scrollArea);
    mainLayout->addWidget(rightSplitter, 1);

    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("Scroll to zoom, drag to pan the map."));

    mapView_->setSite(latSpin_->value(), lonSpin_->value(), radiusSpin_->value());
}

void MainWindow::onBrowseEphemeris() {
    QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Select baked area ephemeris table"),
                                              QString(), QStringLiteral("Text files (*.txt);;All files (*)"));
    if (!f.isEmpty()) ephPathEdit_->setText(f);
}

void MainWindow::onBrowseCatalog() {
    QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Select HYG star catalogue CSV"),
                                              QString(), QStringLiteral("CSV files (*.csv);;All files (*)"));
    if (!f.isEmpty()) catalogPathEdit_->setText(f);
}

void MainWindow::onApiKeyChanged() {
    QString key = mapApiKeyEdit_->text().trimmed();
    QSettings settings;
    if (key.isEmpty()) {
        settings.remove(QStringLiteral("mapTilerApiKey"));
    } else {
        settings.setValue(QStringLiteral("mapTilerApiKey"), key);
    }
    mapView_->setApiKey(key);
}

void MainWindow::onGetApiKeyClicked() {
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.maptiler.com/cloud/")));
}

void MainWindow::onRunClicked() {
    SearchParams p;
    p.site.lat_deg = latSpin_->value();
    p.site.lon_deg = lonSpin_->value();
    p.site.elevation_m = elevSpin_->value();
    p.mag_limit = magSpin_->value();
    p.radius_km = radiusSpin_->value();
    p.jd_start = jdFromQDate(startDate_->date());
    p.jd_end = jdFromQDate(endDate_->date());
    p.eph_path = ephPathEdit_->text();
    p.catalog_path = catalogPathEdit_->text();

    p.filters.mag_limit = p.mag_limit;
    p.filters.radius_km = p.radius_km;
    p.filters.filter_min_moon_alt = moonAltCheck_->isChecked();
    p.filters.min_moon_alt_deg = moonAltSpin_->value();
    p.filters.filter_max_sun_alt = sunAltCheck_->isChecked();
    p.filters.max_sun_alt_deg = sunAltSpin_->value();
    p.filters.filter_min_illum = minIllumCheck_->isChecked();
    p.filters.min_illum = minIllumSpin_->value();
    p.filters.filter_max_illum = maxIllumCheck_->isChecked();
    p.filters.max_illum = maxIllumSpin_->value();

    if (p.jd_end <= p.jd_start) {
        QMessageBox::warning(this, QStringLiteral("Invalid range"), QStringLiteral("End date must be after start date."));
        return;
    }

    log_->clear();
    table_->setRowCount(0);
    mapView_->setLines({});
    mapView_->setSite(p.site.lat_deg, p.site.lon_deg, p.radius_km);
    runButton_->setEnabled(false);
    exportKmlButton_->setEnabled(false);
    exportCsvButton_->setEnabled(false);
    lastSite_ = p.site;
    lastRadiusKm_ = p.radius_km;
    statusLabel_->setText(QStringLiteral("Searching..."));

    QMetaObject::invokeMethod(worker_, [this, p]() { worker_->runSearch(p); }, Qt::QueuedConnection);
}

void MainWindow::onSearchProgress(int done, int total) {
    statusLabel_->setText(QStringLiteral("Scanning stars: %1 / %2").arg(done).arg(total));
}

void MainWindow::onLogMessage(QString msg) {
    log_->appendPlainText(msg);
}

void MainWindow::onSearchFailed(QString error) {
    runButton_->setEnabled(true);
    statusLabel_->setText(QStringLiteral("Error: %1").arg(error));
    QMessageBox::critical(this, QStringLiteral("Search failed"), error);
}

void MainWindow::onSearchFinished(std::vector<gg::GrazeEvent> events) {
    runButton_->setEnabled(true);
    lastEvents_ = std::move(events);
    exportKmlButton_->setEnabled(!lastEvents_.empty());
    exportCsvButton_->setEnabled(!lastEvents_.empty());
    statusLabel_->setText(QStringLiteral("%1 grazing occultation(s) found.").arg(lastEvents_.size()));
    populateTable(lastEvents_);

    std::vector<MapLine> lines;
    for (size_t i = 0; i < lastEvents_.size(); i++) {
        const auto& ev = lastEvents_[i];
        MapLine ml;
        ml.label = QString::number(i + 1);
        for (const auto& pt : ev.line) ml.points.push_back({pt.lon_deg, pt.lat_deg});
        ml.tooltip = QStringLiteral("%1. %2  mag %3  %4  %5 km  moon_alt %6  sun_alt %7  illum %8")
                          .arg(i + 1)
                          .arg(QString::fromStdString(ev.star.name))
                          .arg(ev.star.vmag, 0, 'f', 2)
                          .arg(jdToDisplayString(ev.circ.jd_tt))
                          .arg(ev.dist_km, 0, 'f', 1)
                          .arg(ev.circ.moon_alt_deg, 0, 'f', 0)
                          .arg(ev.circ.sun_alt_deg, 0, 'f', 0)
                          .arg(ev.circ.illum_frac, 0, 'f', 2);
        lines.push_back(std::move(ml));
    }
    mapView_->setLines(lines);
}

void MainWindow::populateTable(const std::vector<gg::GrazeEvent>& events) {
    table_->setRowCount(static_cast<int>(events.size()));
    for (size_t i = 0; i < events.size(); i++) {
        const auto& ev = events[i];
        int row = static_cast<int>(i);
        table_->setItem(row, 0, new QTableWidgetItem(QString::number(i + 1)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(ev.star.name)));
        table_->setItem(row, 2, new QTableWidgetItem(QString::number(ev.star.vmag, 'f', 2)));
        table_->setItem(row, 3, new QTableWidgetItem(jdToDisplayString(ev.circ.jd_tt)));
        table_->setItem(row, 4, new QTableWidgetItem(QString::number(ev.dist_km, 'f', 1)));
        table_->setItem(row, 5, new QTableWidgetItem(QString::number(ev.circ.moon_alt_deg, 'f', 0)));
        table_->setItem(row, 6, new QTableWidgetItem(QString::number(ev.circ.sun_alt_deg, 'f', 0)));
        table_->setItem(row, 7, new QTableWidgetItem(QString::number(ev.circ.illum_frac, 'f', 2)));
    }
}

void MainWindow::onRowSelected(int row) {
    if (row < 0 || row >= static_cast<int>(lastEvents_.size())) return;
    const auto& ev = lastEvents_[row];
    if (ev.line.empty()) return;
    // centre the map on the line's point nearest the site
    double siteLat = latSpin_->value(), siteLon = lonSpin_->value();
    size_t best = 0;
    double bestD = 1e18;
    for (size_t i = 0; i < ev.line.size(); i++) {
        double dx = ev.line[i].lon_deg - siteLon, dy = ev.line[i].lat_deg - siteLat;
        double d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    mapView_->centerOnLonLat(ev.line[best].lon_deg, ev.line[best].lat_deg);
}

// ---------------------------------------------------------------------
// KML / CSV export
//
// KML is Google Earth's native format and also imports cleanly into Google
// My Maps and most GIS tools (QGIS, Google Maps' "Your Places" via My
// Maps, etc). We write: an Observer-site Point placemark, the search
// radius as a LineString polygon ring (same flat-Earth approximation the
// rest of the tool already uses via gg::km_per_deg_lon -- consistent with
// how the radius filter itself was applied), and one LineString placemark
// per graze line with a description giving the full circumstances.
// ---------------------------------------------------------------------

bool MainWindow::writeKml(const QString& path) const {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    const double siteLat = lastSite_.lat_deg;
    const double siteLon = lastSite_.lon_deg;
    const double kmPerDegLon = gg::km_per_deg_lon(siteLat);

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n";
    out << "<Document>\n";
    out << "  <name>" << escapeXml(QStringLiteral("LunarGraze export")) << "</name>\n";

    out << "  <Style id=\"siteStyle\"><IconStyle><color>ff0000ff</color><scale>1.2</scale></IconStyle></Style>\n";
    out << "  <Style id=\"radiusStyle\"><LineStyle><color>800000ff</color><width>2</width></LineStyle>"
           "<PolyStyle><fill>0</fill></PolyStyle></Style>\n";
    out << "  <Style id=\"lineStyle\"><LineStyle><color>ff00a5ff</color><width>3</width></LineStyle></Style>\n";

    // Observer site.
    out << "  <Placemark>\n";
    out << "    <name>" << escapeXml(QStringLiteral("Observer site")) << "</name>\n";
    out << "    <styleUrl>#siteStyle</styleUrl>\n";
    out << "    <Point><coordinates>" << QString::number(siteLon, 'f', 6) << ","
        << QString::number(siteLat, 'f', 6) << ",0</coordinates></Point>\n";
    out << "  </Placemark>\n";

    // Search radius, drawn as a closed ring (not a filled polygon, so it
    // doesn't obscure the map underneath in Google Earth).
    if (lastRadiusKm_ > 0) {
        out << "  <Placemark>\n";
        out << "    <name>" << escapeXml(QStringLiteral("Search radius (%1 km)").arg(lastRadiusKm_, 0, 'f', 0)) << "</name>\n";
        out << "    <styleUrl>#radiusStyle</styleUrl>\n";
        out << "    <LineString>\n      <coordinates>\n";
        constexpr int kRingPoints = 72;
        for (int i = 0; i <= kRingPoints; i++) {
            double angle = 2.0 * M_PI * i / kRingPoints;
            double lat = siteLat + (lastRadiusKm_ / 111.32) * std::cos(angle);
            double lon = siteLon + (lastRadiusKm_ / kmPerDegLon) * std::sin(angle);
            out << "        " << QString::number(lon, 'f', 6) << "," << QString::number(lat, 'f', 6) << ",0\n";
        }
        out << "      </coordinates>\n    </LineString>\n";
        out << "  </Placemark>\n";
    }

    // One placemark per graze line.
    for (size_t i = 0; i < lastEvents_.size(); i++) {
        const auto& ev = lastEvents_[i];
        if (ev.line.empty()) continue;
        QString title = QStringLiteral("%1. %2 (mag %3)")
                             .arg(i + 1).arg(QString::fromStdString(ev.star.name)).arg(ev.star.vmag, 0, 'f', 2);
        QString desc = QStringLiteral(
                            "Star: %1 (V=%2)\n"
                            "Closest approach: %3\n"
                            "Distance to site: %4 km\n"
                            "Moon altitude: %5 deg\n"
                            "Sun altitude: %6 deg\n"
                            "Illuminated fraction: %7\n"
                            "Star alt/az: %8 / %9 deg")
                            .arg(QString::fromStdString(ev.star.name))
                            .arg(ev.star.vmag, 0, 'f', 2)
                            .arg(jdToDisplayString(ev.circ.jd_tt))
                            .arg(ev.dist_km, 0, 'f', 1)
                            .arg(ev.circ.moon_alt_deg, 0, 'f', 1)
                            .arg(ev.circ.sun_alt_deg, 0, 'f', 1)
                            .arg(ev.circ.illum_frac, 0, 'f', 3)
                            .arg(ev.circ.star_alt_deg, 0, 'f', 1)
                            .arg(ev.circ.star_az_deg, 0, 'f', 1);

        out << "  <Placemark>\n";
        out << "    <name>" << escapeXml(title) << "</name>\n";
        out << "    <styleUrl>#lineStyle</styleUrl>\n";
        // CDATA is verbatim text -- no entity-escaping needed (or wanted).
        out << "    <description><![CDATA[" << desc << "]]></description>\n";
        out << "    <LineString>\n      <coordinates>\n";
        for (const auto& pt : ev.line) {
            out << "        " << QString::number(pt.lon_deg, 'f', 6) << "," << QString::number(pt.lat_deg, 'f', 6) << ",0\n";
        }
        out << "      </coordinates>\n    </LineString>\n";
        out << "  </Placemark>\n";
    }

    out << "</Document>\n</kml>\n";
    file.close();
    return true;
}

bool MainWindow::writeCsv(const QString& path) const {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    out << "n,star,mag,time_utc,dist_km,moon_alt_deg,sun_alt_deg,illum_frac,star_alt_deg,star_az_deg\n";
    for (size_t i = 0; i < lastEvents_.size(); i++) {
        const auto& ev = lastEvents_[i];
        out << (i + 1) << "," << QString::fromStdString(ev.star.name) << ","
            << QString::number(ev.star.vmag, 'f', 2) << "," << jdToDisplayString(ev.circ.jd_tt) << ","
            << QString::number(ev.dist_km, 'f', 1) << "," << QString::number(ev.circ.moon_alt_deg, 'f', 1) << ","
            << QString::number(ev.circ.sun_alt_deg, 'f', 1) << "," << QString::number(ev.circ.illum_frac, 'f', 3) << ","
            << QString::number(ev.circ.star_alt_deg, 'f', 1) << "," << QString::number(ev.circ.star_az_deg, 'f', 1) << "\n";
    }
    file.close();
    return true;
}

void MainWindow::onExportKml() {
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export as KML"),
                                                 QStringLiteral("graze_events.kml"),
                                                 QStringLiteral("KML files (*.kml);;All files (*)"));
    if (path.isEmpty()) return;
    if (writeKml(path)) {
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 5000);
    } else {
        QMessageBox::critical(this, QStringLiteral("Export failed"),
                               QStringLiteral("Could not write %1").arg(path));
    }
}

void MainWindow::onExportCsv() {
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export as CSV"),
                                                 QStringLiteral("graze_events.csv"),
                                                 QStringLiteral("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty()) return;
    if (writeCsv(path)) {
        statusBar()->showMessage(QStringLiteral("Wrote %1").arg(path), 5000);
    } else {
        QMessageBox::critical(this, QStringLiteral("Export failed"),
                               QStringLiteral("Could not write %1").arg(path));
    }
}
