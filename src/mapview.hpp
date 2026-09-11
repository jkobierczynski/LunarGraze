// SPDX-License-Identifier: GPL-3.0-or-later
// LunarGraze -- a zoomable, pannable slippy map (OSM-derived raster tiles,
// standard Web Mercator "z/x/y" scheme) drawn with QGraphicsView, with the
// site marker, search-radius circle and traced graze lines overlaid on top
// in the same projection. Tiles are fetched over the network on demand and
// cached to disk; nothing else in LunarGraze needs network access.
//
// TILE USAGE NOTE: this used to point at the public tile.openstreetmap.org
// demo server, but that now actively 403s non-browser clients regardless of
// User-Agent/caching compliance (OSM operations tightened enforcement --
// see https://operations.osmfoundation.org/policies/tiles/ and
// https://wiki.osmfoundation.org/wiki/Blocked_tiles). It now points at
// MapTiler instead (api.maptiler.com), which requires a free API key (see
// setApiKey() / MainWindow's "Map tiles" section) but explicitly supports
// exactly this use case -- an app distributed to end users who each supply
// their own key. See README.md for how to get one.
#pragma once
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QGraphicsPathItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsSimpleTextItem>
#include <QNetworkAccessManager>
#include <QMap>
#include <QString>
#include <QPointF>
#include <vector>

class QPainter;
class QPaintEvent;
class QNetworkReply;

struct MapLinePoint {
    double lon_deg, lat_deg;
};

struct MapLine {
    std::vector<MapLinePoint> points; // west to east
    QString label;                    // e.g. "1"
    QString tooltip;                  // full circumstances, one line
};

class MapView : public QGraphicsView {
    Q_OBJECT
public:
    explicit MapView(QWidget* parent = nullptr);

    // Sets/updates the observer site and search radius (km); re-centres
    // the view and redraws the radius circle.
    void setSite(double lat_deg, double lon_deg, double radius_km);

    // Replaces the set of traced graze lines shown on the map.
    void setLines(const std::vector<MapLine>& lines);

    void zoomIn();
    void zoomOut();
    void recenterOnSite();
    void centerOnLonLat(double lon_deg, double lat_deg);

    // MapTiler API key (see the TILE USAGE NOTE above). Re-requests visible
    // tiles immediately if the key actually changed. An empty key means no
    // requests are sent at all -- see requestTile() -- so the map just shows
    // the error banner instead of hammering the server with doomed requests.
    void setApiKey(const QString& key);

signals:
    // Emitted when a tile request fails, or when tiles resume loading
    // successfully after a failure (message is empty in that case) -- lets
    // MainWindow surface the reason (network/SSL/DNS issue, missing/invalid
    // API key, etc.) instead of the map just silently staying blank.
    void tileLoadError(QString message);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private slots:
    // Takes the reply explicitly (matching QNetworkAccessManager::finished's
    // own signature) rather than recovering it via sender() -- sender()
    // would return the QNetworkAccessManager itself here, not the reply,
    // since it's the manager's signal that's firing, not the reply's own.
    void onTileReply(QNetworkReply* reply);
    void refreshTiles();
    void scheduleRefresh();

private:
    QPointF lonLatToScene(double lon_deg, double lat_deg) const;
    void setZoom(int z, QPointF anchorScenePos = QPointF());
    void requestTile(int z, int x, int y);
    QString tileCachePath(int z, int x, int y) const;
    void redrawOverlays();
    void updateAttributionPos();
    void drawScaleBar(QPainter& painter) const;
    void drawErrorBanner(QPainter& painter) const;
    void setTileError(const QString& message); // "" clears it

    QGraphicsScene* scene_;
    QNetworkAccessManager* net_;
    QString cacheDir_;
    QString apiKey_;

    int zoom_ = 6;
    static constexpr int kTileSize = 256;

    double siteLat_ = 0, siteLon_ = 0, radiusKm_ = 100;
    bool haveSite_ = false;

    QMap<QString, QGraphicsPixmapItem*> tileItems_; // key "z/x/y"
    QMap<QString, bool> pendingTiles_;
    QMap<QString, class QNetworkReply*> activeReplies_; // in-flight requests, so a zoom change can abort them instead of leaking bandwidth on stale tiles

    QGraphicsEllipseItem* siteMarker_ = nullptr;
    QGraphicsEllipseItem* radiusCircle_ = nullptr;
    QGraphicsSimpleTextItem* siteLabel_ = nullptr;
    QGraphicsSimpleTextItem* attribution_ = nullptr;
    std::vector<QGraphicsPathItem*> lineItems_;
    std::vector<QGraphicsSimpleTextItem*> lineLabels_;
    std::vector<QGraphicsEllipseItem*> lineDots_;

    std::vector<MapLine> lines_;
    class QTimer* refreshTimer_ = nullptr;
    QString tileErrorMsg_;
};
