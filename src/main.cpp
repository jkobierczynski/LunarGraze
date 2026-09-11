// SPDX-License-Identifier: GPL-3.0-or-later
// LunarGraze (formerly graze_gui) -- Qt desktop application: find and plot
// grazing lunar occultations near any location on Earth, on a
// zoomable/pannable map, for any star-magnitude limit and date range. C++
// port of tools/graze_finder/graze_finder.py -- see grazecore.hpp and
// README.md.
#include "mainwindow.hpp"
#include <QApplication>
#include <QNetworkProxyFactory>
#include <QSettings>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("LunarGraze");
    QApplication::setOrganizationName("moon-graze");

    // One-time migration: earlier builds of this app were named "graze_gui"
    // and saved the MapTiler API key under that QSettings identity. Carry
    // it over so the rename doesn't silently blank out a key you already
    // entered -- no point making everyone re-paste it.
    {
        QSettings settings;
        if (settings.value(QStringLiteral("mapTilerApiKey")).toString().isEmpty()) {
            QSettings oldSettings(QStringLiteral("moon-graze"), QStringLiteral("graze_gui"));
            QString oldKey = oldSettings.value(QStringLiteral("mapTilerApiKey")).toString();
            if (!oldKey.isEmpty()) {
                settings.setValue(QStringLiteral("mapTilerApiKey"), oldKey);
            }
        }
    }

    // Without this, QNetworkAccessManager connects directly and ignores any
    // system/corporate HTTP(S) proxy (env vars on Linux, system settings on
    // Windows/macOS) -- map tiles then just fail to load with no obvious
    // reason. This makes the map tile fetches (the only network use in the
    // app) go through whatever proxy the OS is configured with, same as a
    // browser would.
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    MainWindow window;
    window.show();
    return app.exec();
}
