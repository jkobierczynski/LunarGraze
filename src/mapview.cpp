// SPDX-License-Identifier: GPL-3.0-or-later
#include "mapview.hpp"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QPainterPath>
#include <QTimer>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <cmath>
#include <limits>
#include <algorithm>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Standard Web Mercator slippy-map tile math (OSM wiki "Slippy map
// tilenames"). World size at zoom z is 2^z * 256 pixels; this is also the
// scene coordinate system used throughout MapView.
QPointF lonLatToWorldPixel(double lon_deg, double lat_deg, int zoom) {
    double n = std::pow(2.0, zoom);
    double x = (lon_deg + 180.0) / 360.0 * n * 256.0;
    double lat_rad = lat_deg * kPi / 180.0;
    double y = (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) / 2.0 * n * 256.0;
    return QPointF(x, y);
}

// MapTiler's raster tile API (see the TILE USAGE NOTE in mapview.hpp for
// why this isn't tile.openstreetmap.org anymore). %1/%2/%3 are z/x/y, %4 is
// the user's API key.
const char* kTileUrlTemplate = "https://api.maptiler.com/maps/streets-v4/256/%1/%2/%3.png?key=%4";

// Rounds maxKm down to a "nice" 1/2/5 x 10^n value, for a scale-bar length
// that reads like "50 km" or "200 km" rather than "63.4 km".
double niceScaleNumber(double maxVal) {
    if (maxVal <= 0) return 1.0;
    double exp = std::floor(std::log10(maxVal));
    double base = std::pow(10.0, exp);
    double f = maxVal / base;
    double nice = (f >= 5.0) ? 5.0 : (f >= 2.0) ? 2.0 : 1.0;
    return nice * base;
}
} // namespace

MapView::MapView(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::Antialiasing, true);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setBackgroundBrush(QBrush(QColor(0xdd, 0xe6, 0xf0)));
    setMinimumSize(400, 300);

    net_ = new QNetworkAccessManager(this);
    connect(net_, &QNetworkAccessManager::finished, this, &MapView::onTileReply);

    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::tempPath() + "/lunargraze_cache";
    // Scoped by provider/style so switching providers (as this app already
    // did once, OSM -> MapTiler) can never mix two providers' imagery under
    // the same z/x/y cache key.
    cacheDir_ = base + "/tiles/maptiler_streets_v4";
    QDir().mkpath(cacheDir_);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(150);
    connect(refreshTimer_, &QTimer::timeout, this, &MapView::refreshTiles);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &MapView::scheduleRefresh);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MapView::scheduleRefresh);

    // MapTiler's required attribution (see https://www.maptiler.com/copyright/)
    // -- both credits, together, visibly displayed.
    attribution_ = scene_->addSimpleText(QStringLiteral(
        "© MapTiler © OpenStreetMap contributors"));
    attribution_->setZValue(1000);
    attribution_->setBrush(QBrush(QColor(60, 60, 60)));
    QFont f = attribution_->font();
    f.setPointSize(8);
    attribution_->setFont(f);

    // A generous default world size so panning/zooming works before a site
    // is set (setSite() will re-centre once the caller has real numbers).
    scene_->setSceneRect(0, 0, std::pow(2.0, zoom_) * kTileSize, std::pow(2.0, zoom_) * kTileSize);
}

QPointF MapView::lonLatToScene(double lon_deg, double lat_deg) const {
    return lonLatToWorldPixel(lon_deg, lat_deg, zoom_);
}

void MapView::setApiKey(const QString& key) {
    if (apiKey_ == key) return;
    apiKey_ = key;
    if (!apiKey_.isEmpty()) {
        setTileError(QString()); // clear the "no key" banner immediately, don't wait for a request round-trip
        refreshTiles();
    }
}

void MapView::setSite(double lat_deg, double lon_deg, double radius_km) {
    siteLat_ = lat_deg; siteLon_ = lon_deg; radiusKm_ = radius_km;
    haveSite_ = true;

    // pick a zoom level so the radius circle comfortably fills the
    // viewport: world width at zoom z is 2^z*256 px covering 360 deg of
    // longitude, so km-per-pixel depends on latitude and zoom.
    double km_per_deg_lon = 111.32 * std::cos(lat_deg * kPi / 180.0);
    int z = 10;
    for (int cand = 3; cand <= 18; cand++) {
        double world_px = std::pow(2.0, cand) * kTileSize;
        double px_per_deg_lon = world_px / 360.0;
        double px_per_km = px_per_deg_lon / std::max(1e-6, km_per_deg_lon);
        double view_km = std::min(width(), height()) / std::max(1e-6, px_per_km);
        if (view_km < radius_km * 2.6) { z = std::max(3, cand - 1); break; }
        z = cand;
    }
    setZoom(z);
    recenterOnSite();
    redrawOverlays();
    refreshTiles();
}

void MapView::recenterOnSite() {
    if (!haveSite_) return;
    centerOn(lonLatToScene(siteLon_, siteLat_));
}

void MapView::centerOnLonLat(double lon_deg, double lat_deg) {
    centerOn(lonLatToScene(lon_deg, lat_deg));
}

void MapView::setZoom(int z, QPointF anchorScenePos) {
    z = std::max(2, std::min(19, z));
    if (z == zoom_) return;

    QPointF anchorLonLat;
    bool haveAnchor = !anchorScenePos.isNull();
    if (haveAnchor) {
        // invert current projection to recover lon/lat under the anchor
        double n = std::pow(2.0, zoom_);
        double lon = anchorScenePos.x() / (n * kTileSize) * 360.0 - 180.0;
        double yfrac = anchorScenePos.y() / (n * kTileSize);
        double lat = std::atan(std::sinh(kPi * (1.0 - 2.0 * yfrac))) * 180.0 / kPi;
        anchorLonLat = QPointF(lon, lat);
    }

    for (auto it = tileItems_.begin(); it != tileItems_.end(); ++it) {
        scene_->removeItem(it.value());
        delete it.value();
    }
    tileItems_.clear();
    pendingTiles_.clear();

    // Actually cancel in-flight requests for the old zoom instead of just
    // forgetting about them -- otherwise repeated zooming piles up
    // abandoned downloads that finish (or time out) long after they stop
    // mattering, wasting real bandwidth while never showing a tile.
    for (auto it = activeReplies_.begin(); it != activeReplies_.end(); ++it) {
        it.value()->abort();
    }
    activeReplies_.clear();

    zoom_ = z;
    double worldPx = std::pow(2.0, zoom_) * kTileSize;
    scene_->setSceneRect(0, 0, worldPx, worldPx);

    if (haveAnchor) {
        centerOn(lonLatToScene(anchorLonLat.x(), anchorLonLat.y()));
    } else if (haveSite_) {
        centerOn(lonLatToScene(siteLon_, siteLat_));
    }
    redrawOverlays();
    refreshTiles();
}

void MapView::zoomIn() { setZoom(zoom_ + 1, mapToScene(viewport()->rect().center())); }
void MapView::zoomOut() { setZoom(zoom_ - 1, mapToScene(viewport()->rect().center())); }

void MapView::wheelEvent(QWheelEvent* event) {
    int steps = event->angleDelta().y() / 120;
    if (steps == 0) { event->accept(); return; }
    setZoom(zoom_ + steps, mapToScene(event->position().toPoint()));
    event->accept();
}

void MapView::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    updateAttributionPos();
    scheduleRefresh();
}

void MapView::paintEvent(QPaintEvent* event) {
    QGraphicsView::paintEvent(event);
    // Drawn straight onto the viewport in screen (not scene) coordinates so
    // the scale bar's on-screen length is correct regardless of the current
    // zoom transform, and the error banner stays put while panning/zooming.
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawScaleBar(painter);
    drawErrorBanner(painter);
}

void MapView::drawScaleBar(QPainter& painter) const {
    if (!haveSite_) return;

    // km-per-pixel at the site's latitude and the current zoom: measure a
    // known 1-degree-of-longitude span in scene px, then convert scene px
    // to view (screen) px via the current view transform's scale.
    double kmPerDegLon = 111.32 * std::cos(siteLat_ * kPi / 180.0);
    if (kmPerDegLon < 1e-6) return; // degenerate at the poles; skip
    QPointF s0 = lonLatToScene(siteLon_, siteLat_);
    QPointF s1 = lonLatToScene(siteLon_ + 1.0, siteLat_);
    double scenePxPerDegLon = std::fabs(s1.x() - s0.x());
    double viewPxPerDegLon = scenePxPerDegLon * transform().m11();
    double viewPxPerKm = viewPxPerDegLon / kmPerDegLon;
    if (viewPxPerKm <= 0 || !std::isfinite(viewPxPerKm)) return;

    constexpr double kTargetPx = 110.0;
    double barKm = niceScaleNumber(kTargetPx / viewPxPerKm);
    double barPx = barKm * viewPxPerKm;

    double margin = 12.0;
    double y = viewport()->height() - margin - 6;
    double x0 = margin;
    double x1 = x0 + barPx;

    QString label = (barKm >= 1.0) ? QStringLiteral("%1 km").arg(barKm, 0, 'f', barKm < 10 ? 1 : 0)
                                    : QStringLiteral("%1 m").arg(barKm * 1000.0, 0, 'f', 0);

    painter.save();
    QPen pen(QColor(30, 30, 30), 2);
    painter.setPen(pen);
    painter.drawLine(QPointF(x0, y), QPointF(x1, y));
    painter.drawLine(QPointF(x0, y - 4), QPointF(x0, y + 4));
    painter.drawLine(QPointF(x1, y - 4), QPointF(x1, y + 4));

    QFont f = painter.font();
    f.setPointSize(9);
    painter.setFont(f);
    QRectF textRect(x0, y - 20, barPx + 40, 16);
    // faint white halo behind the text so it stays legible over dark tiles
    painter.setPen(QColor(255, 255, 255, 220));
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) painter.drawText(textRect.translated(dx, dy), Qt::AlignLeft | Qt::AlignVCenter, label);
    painter.setPen(QColor(30, 30, 30));
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
    painter.restore();
}

void MapView::drawErrorBanner(QPainter& painter) const {
    if (tileErrorMsg_.isEmpty()) return;
    QString text = QStringLiteral("Map tiles failed to load: %1").arg(tileErrorMsg_);
    painter.save();
    QFont f = painter.font();
    f.setPointSize(9);
    painter.setFont(f);
    QFontMetrics fm(f);
    QRectF bar(0, 0, viewport()->width(), fm.height() + 12);
    painter.fillRect(bar, QColor(0x7a, 0x1f, 0x1f, 235));
    painter.setPen(Qt::white);
    painter.drawText(bar, Qt::AlignCenter, text);
    painter.restore();
}

void MapView::setTileError(const QString& message) {
    if (tileErrorMsg_ == message) return;
    tileErrorMsg_ = message;
    emit tileLoadError(message);
    viewport()->update();
}

void MapView::updateAttributionPos() {
    if (!attribution_) return;
    QPointF topLeftScene = mapToScene(0, viewport()->height() - 18);
    attribution_->setPos(topLeftScene);
}

void MapView::scheduleRefresh() { refreshTimer_->start(); }

void MapView::setLines(const std::vector<MapLine>& lines) {
    lines_ = lines;
    redrawOverlays();
}

void MapView::redrawOverlays() {
    // radius circle + site marker
    double kmPerDegLon = haveSite_ ? 111.32 * std::cos(siteLat_ * kPi / 180.0) : 111.32;
    if (haveSite_) {
        QPointF c = lonLatToScene(siteLon_, siteLat_);
        QPointF edge = lonLatToScene(siteLon_ + radiusKm_ / std::max(1e-6, kmPerDegLon), siteLat_);
        double rPx = std::fabs(edge.x() - c.x());

        if (!radiusCircle_) {
            radiusCircle_ = scene_->addEllipse(QRectF(), QPen(QColor(0x33, 0x55, 0xaa), 1.6, Qt::DashLine));
            radiusCircle_->setZValue(5);
        }
        radiusCircle_->setRect(c.x() - rPx, c.y() - rPx, rPx * 2, rPx * 2);

        if (!siteMarker_) {
            siteMarker_ = scene_->addEllipse(QRectF(), QPen(QColor(0xaa, 0x22, 0x22), 1.5),
                                              QBrush(QColor(0xcc, 0x22, 0x22)));
            siteMarker_->setZValue(10);
        }
        siteMarker_->setRect(c.x() - 5, c.y() - 5, 10, 10);

        if (!siteLabel_) {
            siteLabel_ = scene_->addSimpleText(QStringLiteral("your site"));
            siteLabel_->setBrush(QBrush(QColor(0xaa, 0x22, 0x22)));
            siteLabel_->setZValue(10);
        }
        siteLabel_->setPos(c.x() + 8, c.y() - 18);
    }

    for (auto* item : lineItems_) { scene_->removeItem(item); delete item; }
    for (auto* item : lineLabels_) { scene_->removeItem(item); delete item; }
    for (auto* item : lineDots_) { scene_->removeItem(item); delete item; }
    lineItems_.clear(); lineLabels_.clear(); lineDots_.clear();

    for (const auto& line : lines_) {
        if (line.points.empty()) continue;
        QPainterPath path;
        QPointF first = lonLatToScene(line.points.front().lon_deg, line.points.front().lat_deg);
        path.moveTo(first);
        for (size_t i = 1; i < line.points.size(); i++) {
            QPointF p = lonLatToScene(line.points[i].lon_deg, line.points[i].lat_deg);
            path.lineTo(p);
        }
        auto* pathItem = scene_->addPath(path, QPen(QColor(20, 20, 20), 1.6));
        pathItem->setZValue(6);
        if (!line.tooltip.isEmpty()) pathItem->setToolTip(line.tooltip);
        lineItems_.push_back(pathItem);

        // label + dot near the point closest to the site
        size_t best = 0;
        double bestD = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < line.points.size(); i++) {
            double dx = line.points[i].lon_deg - siteLon_, dy = line.points[i].lat_deg - siteLat_;
            double d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; best = i; }
        }
        QPointF p = lonLatToScene(line.points[best].lon_deg, line.points[best].lat_deg);
        auto* dot = scene_->addEllipse(p.x() - 2.5, p.y() - 2.5, 5, 5,
                                        QPen(Qt::NoPen), QBrush(QColor(20, 20, 20)));
        dot->setZValue(7);
        lineDots_.push_back(dot);

        auto* label = scene_->addSimpleText(line.label);
        QFont bold = label->font();
        bold.setBold(true);
        label->setFont(bold);
        label->setPos(p.x() + 5, p.y() + 3);
        label->setZValue(8);
        if (!line.tooltip.isEmpty()) label->setToolTip(line.tooltip);
        lineLabels_.push_back(label);
    }
    updateAttributionPos();
}

QString MapView::tileCachePath(int z, int x, int y) const {
    return QString("%1/%2/%3/%4.png").arg(cacheDir_).arg(z).arg(x).arg(y);
}

void MapView::requestTile(int z, int x, int y) {
    QString key = QString("%1/%2/%3").arg(z).arg(x).arg(y);
    if (tileItems_.contains(key) || pendingTiles_.contains(key)) return;

    QString cachePath = tileCachePath(z, x, y);
    if (QFile::exists(cachePath)) {
        QPixmap pm(cachePath);
        if (!pm.isNull()) {
            auto* item = scene_->addPixmap(pm);
            item->setOffset(0, 0);
            item->setPos(x * kTileSize, y * kTileSize);
            item->setZValue(0);
            tileItems_[key] = item;
            setTileError(QString()); // a tile is available, so at least the cache works
            return;
        }
    }

    if (apiKey_.isEmpty()) {
        // Don't fire a request we already know will fail -- MapTiler
        // rejects unkeyed requests outright, and hammering it with doomed
        // fetches is exactly the kind of traffic that got the old OSM
        // endpoint to block this app in the first place.
        setTileError(QStringLiteral(
            "no MapTiler API key configured -- enter a free key under \"Map tiles\" "
            "on the left (see README.md for how to get one)"));
        return;
    }

    pendingTiles_[key] = true;
    QUrl url(QString(kTileUrlTemplate).arg(z).arg(x).arg(y).arg(apiKey_));
    QNetworkRequest req(url);
    // Not strictly required by MapTiler the way it was for OSM's raw tile
    // server, but a real identifying User-Agent is good practice regardless
    // (and costs nothing).
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("LunarGraze/1.0 (grazing-occultation map tool)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // QNetworkAccessManager's HTTP/2 support has long-standing bugs where a
    // stream to certain CDN configurations (seen against both Fastly and
    // Cloudflare -- i.e. not specific to one tile provider) stalls
    // indefinitely mid-multiplex and never completes, WITHOUT the transfer
    // timeout below catching it (the hang is inside the h2 stream, not at
    // the connection level). curl on the same host succeeds instantly over
    // HTTP/2, so this is a Qt client-side issue, not a network/server one.
    // Forcing HTTP/1.1 sidesteps it entirely -- slightly less efficient for
    // many small parallel requests, but reliable, and tile fetches are tiny
    // enough that it doesn't matter in practice.
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // Without a timeout, a connection that completes its TLS handshake but
    // then never finishes the HTTP response (e.g. a proxy that holds it
    // open) leaves the tile "pending" forever -- no error, no success, no
    // sign anything is wrong. Force it to fail visibly instead. (Belt and
    // braces alongside the HTTP/2 fix above -- this one is reliable for
    // plain HTTP/1.1 connections.)
    req.setTransferTimeout(15000);
    QNetworkReply* reply = net_->get(req);
    reply->setProperty("tileKey", key);
    reply->setProperty("tileZ", z);
    reply->setProperty("tileX", x);
    reply->setProperty("tileY", y);
    activeReplies_[key] = reply;
}

void MapView::onTileReply(QNetworkReply* reply) {
    if (!reply) return;
    reply->deleteLater();
    QString key = reply->property("tileKey").toString();
    pendingTiles_.remove(key);
    activeReplies_.remove(key);

    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        // We aborted this ourselves (setZoom() cancelling a stale
        // in-flight request) -- expected, not worth alarming over.
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        // Surface the real reason instead of just leaving the map blank --
        // e.g. a missing/broken TLS backend, no DNS/network route, a bad/
        // missing API key, or a firewall/proxy all look the same from here
        // (a blank background) unless we say why.
        setTileError(QStringLiteral("%1 (HTTP %2)").arg(reply->errorString()).arg(httpStatus));
        return;
    }
    QByteArray data = reply->readAll();
    QPixmap pm;
    if (!pm.loadFromData(data)) {
        // The request "succeeded" at the HTTP level but the body isn't a
        // decodable image -- e.g. a CDN/proxy interstitial or error page
        // returned with a 200.
        setTileError(QStringLiteral("server returned HTTP %1 but the body wasn't an image (%2 bytes, %3)")
                          .arg(httpStatus).arg(data.size()).arg(contentType.isEmpty() ? QStringLiteral("no content-type") : contentType));
        return;
    }

    int z = reply->property("tileZ").toInt();
    int x = reply->property("tileX").toInt();
    int y = reply->property("tileY").toInt();

    QString cachePath = tileCachePath(z, x, y);
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QFile f(cachePath);
    if (f.open(QIODevice::WriteOnly)) { f.write(data); f.close(); }

    setTileError(QString());
    if (z != zoom_) return; // user zoomed away before this arrived; drop it
    auto* item = scene_->addPixmap(pm);
    item->setPos(x * kTileSize, y * kTileSize);
    item->setZValue(0);
    tileItems_[key] = item;
}

void MapView::refreshTiles() {
    QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    double worldPx = std::pow(2.0, zoom_) * kTileSize;
    int nTiles = static_cast<int>(worldPx / kTileSize);

    int x0 = static_cast<int>(std::floor(visible.left() / kTileSize)) - 1;
    int x1 = static_cast<int>(std::floor(visible.right() / kTileSize)) + 1;
    int y0 = static_cast<int>(std::floor(visible.top() / kTileSize)) - 1;
    int y1 = static_cast<int>(std::floor(visible.bottom() / kTileSize)) + 1;
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(nTiles - 1, x1); y1 = std::min(nTiles - 1, y1);

    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            requestTile(zoom_, x, y);

    // drop tiles well outside the visible area to bound memory use
    QStringList toRemove;
    for (auto it = tileItems_.begin(); it != tileItems_.end(); ++it) {
        QStringList parts = it.key().split('/');
        int tx = parts[1].toInt(), ty = parts[2].toInt();
        if (tx < x0 - 4 || tx > x1 + 4 || ty < y0 - 4 || ty > y1 + 4) toRemove << it.key();
    }
    for (const auto& k : toRemove) {
        scene_->removeItem(tileItems_[k]);
        delete tileItems_[k];
        tileItems_.remove(k);
    }
    updateAttributionPos();
}
