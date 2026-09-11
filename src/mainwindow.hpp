// SPDX-License-Identifier: GPL-3.0-or-later
// LunarGraze -- main window: search parameters on the left, results table +
// zoomable/pannable map on the right. The actual search runs on a worker
// QThread (grazecore's find_grazes can take the better part of a second to
// tens of seconds depending on magnitude limit / date range / star count,
// and must not block the UI).
#pragma once
#include <QMainWindow>
#include <QThread>
#include <vector>
#include <memory>
#include "grazecore.hpp"
#include "areaephem.hpp"
#include "starcatalog.hpp"

class QDoubleSpinBox;
class QDateEdit;
class QCheckBox;
class QPushButton;
class QTableWidget;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class MapView;

struct SearchParams {
    gg::Site site;
    double mag_limit;
    double radius_km;
    double jd_start, jd_end;
    gg::Filters filters;
    QString eph_path;
    QString catalog_path;
};

Q_DECLARE_METATYPE(std::vector<gg::GrazeEvent>)
Q_DECLARE_METATYPE(SearchParams)

// Runs on a worker thread. Owns nothing long-lived across calls except
// caches of the last-loaded ephemeris/catalog (reloading a multi-MB HYG
// catalogue on every search would be wasteful since only mag_limit, which
// only shrinks the set, changes run to run).
class SearchWorker : public QObject {
    Q_OBJECT
public:
    explicit SearchWorker(QObject* parent = nullptr) : QObject(parent) {}

public slots:
    void runSearch(SearchParams params);
    void cancel() { cancelled_ = true; }

signals:
    void progress(int done, int total);
    void logMessage(QString msg);
    void finished(std::vector<gg::GrazeEvent> events);
    void failed(QString error);

private:
    std::unique_ptr<gg::AreaEphemeris> eph_;
    QString ephPath_;
    std::vector<mg::Star> stars_;
    QString catalogPath_;
    double catalogMagLimit_ = -100;
    bool cancelled_ = false;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onRunClicked();
    void onSearchFinished(std::vector<gg::GrazeEvent> events);
    void onSearchFailed(QString error);
    void onSearchProgress(int done, int total);
    void onLogMessage(QString msg);
    void onRowSelected(int row);
    void onBrowseEphemeris();
    void onBrowseCatalog();
    void onExportKml();
    void onExportCsv();
    void onApiKeyChanged();
    void onGetApiKeyClicked();

private:
    void buildUi();
    void populateTable(const std::vector<gg::GrazeEvent>& events);
    bool writeKml(const QString& path) const;
    bool writeCsv(const QString& path) const;

    QDoubleSpinBox* latSpin_;
    QDoubleSpinBox* lonSpin_;
    QDoubleSpinBox* elevSpin_;
    QDoubleSpinBox* radiusSpin_;
    QDoubleSpinBox* magSpin_;
    QDateEdit* startDate_;
    QDateEdit* endDate_;

    QCheckBox* moonAltCheck_;
    QDoubleSpinBox* moonAltSpin_;
    QCheckBox* sunAltCheck_;
    QDoubleSpinBox* sunAltSpin_;
    QCheckBox* minIllumCheck_;
    QDoubleSpinBox* minIllumSpin_;
    QCheckBox* maxIllumCheck_;
    QDoubleSpinBox* maxIllumSpin_;

    QLineEdit* ephPathEdit_;
    QLineEdit* catalogPathEdit_;
    QLineEdit* mapApiKeyEdit_;

    QPushButton* runButton_;
    QPushButton* exportKmlButton_;
    QPushButton* exportCsvButton_;
    QTableWidget* table_;
    QLabel* statusLabel_;
    QPlainTextEdit* log_;
    MapView* mapView_;

    QThread workerThread_;
    SearchWorker* worker_;

    std::vector<gg::GrazeEvent> lastEvents_;
    gg::Site lastSite_;
    double lastRadiusKm_ = 0;
};
