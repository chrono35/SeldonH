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

// seldonh-mcp : serveur MCP (Model Context Protocol) de SeldonH, transport stdio.
//
// L'assistant (Claude Desktop, Jan, Gemini CLI…) lance ce programme et lui parle en JSON-RPC, un message par ligne sur
// l'entrée standard ; les réponses partent sur la sortie standard, les messages de diagnostic sur la sortie d'erreur.
// Les réglages de SeldonH sont relus (dossier des courbes de l'utilisateur, courbes de ChronoModel, format des dates,
// décimales) : les calculs sont les mêmes que dans l'application.
//
//   seldonh-mcp                 serveur (lancé par l'assistant)
//   seldonh-mcp --list          liste des outils (JSON), pour vérifier l'installation
//   seldonh-mcp --date-format calbp   format des dates (bcad, calbp, ka… comme les Préférences)
//   seldonh-mcp --log /tmp/seldonh-mcp.log   journal des échanges avec l'assistant (diagnostic ; aussi SELDONH_MCP_LOG)

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "CurveStore.h"
#include "DateFormat.h"
#include "McpServer.h"

using namespace seldon;

namespace {

std::mutex outMutex;

// Journal de diagnostic : ce que l'assistant envoie (initialize, tools/list, tools/call…) et ce que le serveur répond.
// Montre si l'assistant appelle vraiment les outils, et avec quels arguments.
std::mutex logMutex;
QFile* logFile = nullptr;

void logLine(const char* dir, const QByteArray& line)
{
    if (!logFile) return;
    std::lock_guard<std::mutex> lock(logMutex);
    QByteArray t = line.trimmed();
    if (t.size() > 1500) t = t.left(1500) + "… (" + QByteArray::number(line.size()) + " octets)";
    logFile->write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8() + ' ' + dir + ' ' + t + '\n');
    logFile->flush();
}

void writeLine(const QByteArray& line)
{
    logLine("<--", line);
    std::lock_guard<std::mutex> lock(outMutex);
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Mêmes dossiers de courbes que l'application (AppSettings::curveRoots), sans dépendre de Qt Widgets ni du trousseau.
QStringList curveRoots(const QSettings& s)
{
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QStringList r{ s.value("curves/dir", data + QStringLiteral("/curves")).toString() };
    const QStringList g = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    const QString cm = g.isEmpty() ? QString() : g.first() + QStringLiteral("/CNRS/ChronoModel/Calib");
    if (s.value("curves/chronomodel", true).toBool() && !cm.isEmpty() && QDir(cm).exists()) r << cm;
    return r;
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);      // « \n » seul, comme l'attend le transport stdio
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("SeldonH");          // mêmes réglages que l'application
    QCoreApplication::setApplicationName("SeldonH");
#ifdef APP_VERSION
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
#endif
    Q_INIT_RESOURCE(c14);
    Q_INIT_RESOURCE(f14c);
    Q_INIT_RESOURCE(am);

    QSettings settings;
    CurveStore::setExtraRoots(curveRoots(settings));
    dateformat::setDecimals(std::clamp(settings.value("display/decimals", -1).toInt(), -1, 6));
    DateFormat fmt = dateformat::fromId(settings.value("display/dateFormat", "bcad").toString());

    // Les champs de saisie (Jan, réglages de macOS) remplacent souvent « -- » par un tiret long « — » ou demi-cadratin « – » :
    // les deux sont acceptés comme « -- » (et « − », signe moins).
    QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        QString& a = args[i];
        if (a.startsWith(QChar(0x2014)) || a.startsWith(QChar(0x2013)) || a.startsWith(QChar(0x2212)))
            a = QStringLiteral("--") + a.mid(1).remove(QRegularExpression(QStringLiteral("^[-\\x{2013}\\x{2014}\\x{2212}]+")));
    }
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        if (a == "--help" || a == "-h") {
            std::fprintf(stderr, "seldonh-mcp : serveur MCP (stdio) des outils de datation de SeldonH.\n"
                                 "  --list               affiche la liste des outils (JSON) et quitte\n"
                                 "  --date-format <id>   format des dates : bcad, bcece, calbp, calb2k, datbp, datb2k, ka, ma, numeric\n"
                                 "  --log <fichier>      journal des échanges avec l'assistant (ou variable SELDONH_MCP_LOG)\n"
                                 "  --version            version\n");
            return 0;
        }
        if (a == "--version") { std::printf("%s\n", qPrintable(QCoreApplication::applicationVersion())); return 0; }
        if (a == "--list") {
            std::printf("%s\n", QJsonDocument(McpServer::toolList()).toJson(QJsonDocument::Indented).constData());
            return 0;
        }
        if (a == "--date-format" && i + 1 < args.size()) { fmt = dateformat::fromId(args[++i]); continue; }
        if (a == "--log" && i + 1 < args.size()) { qputenv("SELDONH_MCP_LOG", args[++i].toLocal8Bit()); continue; }
        std::fprintf(stderr, "seldonh-mcp : option inconnue « %s » (--help)\n", qPrintable(a));
        return 2;
    }

    if (const QString lp = qEnvironmentVariable("SELDONH_MCP_LOG"); !lp.isEmpty()) {
        logFile = new QFile(QDir::fromNativeSeparators(lp));
        if (!logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            std::fprintf(stderr, "seldonh-mcp : journal impossible à ouvrir : %s\n", qPrintable(lp));
            delete logFile;
            logFile = nullptr;
        } else {
            logLine("---", QStringLiteral("seldonh-mcp %1 démarré (pid %2), format des dates %3, courbes : %4")
                              .arg(QCoreApplication::applicationVersion()).arg(QCoreApplication::applicationPid())
                              .arg(dateformat::title(fmt), CurveStore::extraRoots().join(QStringLiteral(" ; "))).toUtf8());
        }
    }

    McpServer server(writeLine);
    server.setDateFormat(fmt);
    std::fprintf(stderr, "seldonh-mcp %s prêt (stdio)\n", qPrintable(QCoreApplication::applicationVersion()));

    // Lecture dans un thread à part : une annulation (notifications/cancelled) est vue pendant un calcul long.
    std::mutex qm;
    std::condition_variable cv;
    std::deque<QByteArray> queue;
    bool eof = false;
    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const QByteArray b = QByteArray::fromStdString(line);
            logLine("-->", b);
            if (b.contains("notifications/cancelled")) {
                const QJsonObject o = QJsonDocument::fromJson(b.trimmed()).object();
                if (o.value("method").toString() == QLatin1String("notifications/cancelled")) {
                    server.cancel(o.value("params").toObject().value("requestId"));
                    continue;
                }
            }
            std::lock_guard<std::mutex> lock(qm);
            queue.push_back(b);
            cv.notify_one();
        }
        std::lock_guard<std::mutex> lock(qm);
        eof = true;
        cv.notify_one();
    });

    for (;;) {
        QByteArray line;
        {
            std::unique_lock<std::mutex> lock(qm);
            cv.wait(lock, [&] { return !queue.empty() || eof; });
            if (queue.empty()) break;
            line = queue.front();
            queue.pop_front();
        }
        const QByteArray out = server.handleLine(line);
        if (!out.isEmpty()) writeLine(out);
    }
    reader.join();
    logLine("---", "fin de l'entrée standard : arrêt");
    return 0;
}
