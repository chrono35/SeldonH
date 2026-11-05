/* ---------------------------------------------------------------------

Copyright © CNRS	2014 - 2026

Authors :
	Philippe LANOS
	Helori LANOS
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This file contains dating methods derived from ChronoModel
(https://chronomodel.com), distributed under the same license.

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

#include "CurveStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <memory>
#include <vector>

namespace seldon {

namespace {

QMutex gMutex;
QStringList gRoots;                                              // dossiers supplémentaires, par priorité
QHash<QString, std::shared_ptr<RefCurve>> gCache;
std::vector<std::shared_ptr<RefCurve>> gRetired;                 // courbes remplacées : gardées en vie (pointeurs en cours d'usage)

QHash<QString, QString> gSessionPaths;                           // « méthode/nom » -> nom du fichier joint
QString extOf(const QString& methodId)
{
    if (methodId == QLatin1String("am")) return QStringLiteral("ref");
    if (methodId == QLatin1String("gauss") || methodId == QLatin1String("density")) return QStringLiteral("csv");   // Calib/Gauss, Calib/Density
    return QStringLiteral("14c");
}

QStringList dirsOf(const QString& methodId)
{
    QStringList d;
    QMutexLocker lock(&gMutex);
    for (const QString& r : std::as_const(gRoots)) d << QDir(r).filePath(CurveStore::subdir(methodId));
    d << QStringLiteral(":/methods/%1/curves").arg(methodId);
    return d;
}

QFileInfoList curveFiles(const QString& dir, const QString& methodId)
{
    QFileInfoList out;
    const QString ext = extOf(methodId);
    for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files, QDir::Name))
        if (fi.suffix().compare(ext, Qt::CaseInsensitive) == 0) out << fi;
    return out;
}

QString findFile(const QString& methodId, const QString& name)
{
    const QString want = name.toLower();
    for (const QString& dir : dirsOf(methodId))
        for (const QFileInfo& fi : curveFiles(dir, methodId))
            if (fi.completeBaseName().toLower() == want || fi.fileName().toLower() == want) return fi.filePath();
    return QString();
}

} // namespace

QStringList CurveStore::names(const QString& methodId)
{
    QStringList out;
    {
        QMutexLocker lock(&gMutex);
        for (auto it = gSessionPaths.cbegin(); it != gSessionPaths.cend(); ++it)
            if (it.key().startsWith(methodId + '/')) out << it.key().mid(methodId.size() + 1);
    }
    for (const QString& dir : dirsOf(methodId))
        for (const QFileInfo& fi : curveFiles(dir, methodId)) {
            const QString n = fi.completeBaseName().toLower();
            if (!out.contains(n)) out << n;
        }
    out.sort();
    return out;
}

QString CurveStore::subdir(const QString& methodId)
{
    if (methodId == QLatin1String("c14")) return QStringLiteral("14C");
    if (methodId == QLatin1String("f14c")) return QStringLiteral("F14C");
    if (methodId == QLatin1String("am")) return QStringLiteral("AM");
    if (methodId == QLatin1String("gauss")) return QStringLiteral("Gauss");
    if (methodId == QLatin1String("density")) return QStringLiteral("Density");
    return methodId;
}

void CurveStore::setExtraRoots(const QStringList& roots)
{
    QMutexLocker lock(&gMutex);
    gRoots = roots;
    // Courbes jointes à la conversation : gardées (elles ne viennent d'aucun dossier).
    QHash<QString, std::shared_ptr<RefCurve>> keep;
    for (auto it = gCache.begin(); it != gCache.end(); ++it) {
        if (gSessionPaths.contains(it.key().left(it.key().size() - 2))) { keep.insert(it.key(), it.value()); continue; }
        if (it.value()) gRetired.push_back(it.value());
    }
    gCache = keep;
}

QStringList CurveStore::extraRoots()
{
    QMutexLocker lock(&gMutex);
    return gRoots;
}

QString CurveStore::findNamed(const QString& methodId, const QString& text, int* pos)
{
    auto isWordChar = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    QString best;
    int bestPos = -1;
    for (const QString& n : names(methodId)) {
        if (n.size() <= best.size()) continue;
        for (int i = text.indexOf(n); i >= 0; i = text.indexOf(n, i + 1)) {
            const int e = i + int(n.size());
            if ((i == 0 || !isWordChar(text.at(i - 1))) && (e >= text.size() || !isWordChar(text.at(e)))) {
                best = n; bestPos = i;
                break;
            }
        }
    }
    if (pos) *pos = bestPos;
    return best;
}

QString CurveStore::sourceOf(const QString& methodId, const QString& name)
{
    {
        QString n = name.toLower();
        if (n.endsWith(QLatin1String(".csv"))) n.chop(4);
        QMutexLocker lock(&gMutex);
        if (gSessionPaths.contains(methodId + '/' + n)) return QStringLiteral("📎 ") + gSessionPaths.value(methodId + '/' + n);
    }
    return findFile(methodId, name);
}

bool CurveStore::addSession(const QString& methodId, const QString& fileName, const QByteArray& text, QString* error)
{
    auto c = std::make_shared<RefCurve>();
    std::string err;
    if (!parseRefCurveSimple(text.toStdString(), *c, &err)) { if (error) *error = QString::fromStdString(err); return false; }
    const QString n = QFileInfo(fileName).completeBaseName().toLower();
    c->name = n.toStdString();
    QMutexLocker lock(&gMutex);
    const QString key = methodId + '/' + n;
    for (const char* suffix : { "#a", "#f" })
        if (auto it = gCache.find(key + QLatin1String(suffix)); it != gCache.end()) { if (it.value()) gRetired.push_back(it.value()); gCache.erase(it); }
    gCache.insert(key + QStringLiteral("#a"), c);
    gSessionPaths.insert(key, QFileInfo(fileName).fileName());
    return true;
}

void CurveStore::clearSession()
{
    QMutexLocker lock(&gMutex);
    for (auto it = gSessionPaths.cbegin(); it != gSessionPaths.cend(); ++it)
        for (const char* suffix : { "#a", "#f" })
            if (auto c = gCache.find(it.key() + QLatin1String(suffix)); c != gCache.end()) { if (c.value()) gRetired.push_back(c.value()); gCache.erase(c); }
    gSessionPaths.clear();
}

bool CurveStore::looksLikeGaussRef(const QString& fileName, const QString& text)
{
    const QString f = QFileInfo(fileName).fileName().toLower();
    if (f.contains(QLatin1String("_gp_")) || f.contains(QLatin1String("_gs_"))) return false;     // vitesse, accélération
    const bool named = f.contains(QLatin1String("gauss_ref")) || f.contains(QLatin1String("gaussref"));
    const bool header = text.left(600).contains(QRegularExpression(QStringLiteral("(?:^|[\r\n])\s*X Axis\s*,\s*G\s*,\s*err"),
                                                                  QRegularExpression::CaseInsensitiveOption));
    if (!named && !header) return false;
    RefCurve c;
    return parseRefCurveSimple(text.toStdString(), c);
}

const RefCurve* CurveStore::get(const QString& methodId, const QString& name0, CurveFormat fmt, bool asF14C)
{
    QString name = name0.toLower();
    if (name.endsWith(QLatin1String(".csv"))) name.chop(4);     // plugin Gauss du .chr : « courbe.csv »
    const QString key = methodId + '/' + name + (asF14C ? "#f" : "#a");
    {
        QMutexLocker lock(&gMutex);
        const auto it = gCache.constFind(key);
        if (it != gCache.constEnd()) return it.value().get();
    }
    const QString path = findFile(methodId, name);         // hors verrou (dirsOf le prend)
    QMutexLocker lock(&gMutex);
    if (const auto it = gCache.constFind(key); it != gCache.constEnd()) return it.value().get();
    std::shared_ptr<RefCurve> curve;
    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray raw = f.readAll();
            const std::string text = raw.toStdString();      // octets bruts : seuls les commentaires sont non ASCII
            auto c = std::make_shared<RefCurve>();
            c->name = QFileInfo(path).completeBaseName().toLower().toStdString();
            const bool ok = (fmt == CurveFormat::Radiocarbon) ? parseRefCurve14C(text, asF14C, *c)
                                                              : parseRefCurveSimple(text, *c);
            if (ok) curve = c;
        }
    }
    gCache.insert(key, curve);      // met aussi en cache l'échec (nullptr)
    return curve.get();
}

} // namespace seldon
