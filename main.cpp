/* ---------------------------------------------------------------------

Copyright © CNRS	2026

Authors :
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later
--------------------------------------------------------------------- */

#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QHostAddress>
#include <QLocale>
#include <QNetworkProxy>
#include <QTranslator>

// Proxy du système pour les serveurs distants, mais jamais pour les adresses locales (Ollama, LM Studio, llama-server…) :
// sur un réseau où la découverte automatique de proxy (WPAD) échoue, cela évite aussi des erreurs « PAC script » inutiles
// et des délais pour joindre un serveur situé sur la même machine ou le même réseau local.
class LocalDirectProxyFactory : public QNetworkProxyFactory {
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& q) override
    {
        const QString host = q.peerHostName().toLower();
        if (isLocal(host)) return { QNetworkProxy(QNetworkProxy::NoProxy) };
        return QNetworkProxyFactory::systemProxyForQuery(q);
    }
private:
    static bool isLocal(const QString& host)
    {
        if (host.isEmpty() || host == "localhost" || host.endsWith(".local") || host.endsWith(".localhost") || !host.contains('.'))
            return true;
        QHostAddress a;
        if (!a.setAddress(host)) return false;
        return a.isLoopback() || a.isInSubnet(QHostAddress("10.0.0.0"), 8) || a.isInSubnet(QHostAddress("172.16.0.0"), 12)
               || a.isInSubnet(QHostAddress("192.168.0.0"), 16) || a.isInSubnet(QHostAddress("169.254.0.0"), 16);
    }
};

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName("SeldonH");
    QCoreApplication::setApplicationName("SeldonH");
    QNetworkProxyFactory::setApplicationProxyFactory(new LocalDirectProxyFactory);

    QIcon icon;
    for (int s : { 16, 32, 48, 64, 128, 256, 512 })
        icon.addFile(QString(":/icons/seldonh_%1.png").arg(s), QSize(s, s));
    a.setWindowIcon(icon);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "SeldonH_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    return QApplication::exec();
}
