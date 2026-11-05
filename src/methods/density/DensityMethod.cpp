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

#include "DensityMethod.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

#include "../common/CurveStore.h"
#include "../common/TextUtil.h"

namespace seldon {

QStringList DensityMethod::curveNames() const { return CurveStore::names(id()); }

QRegularExpression DensityMethod::context() const
{
    static const QRegularExpression re(R"((densite tabulee|fichier de densite|plugin density|\bdensity\b|\bdensite\b))");
    return re;
}

QVector<QJsonObject> DensityMethod::parse(const QString& question) const
{
    const QString q = text::fold(question);
    if (!context().match(q).hasMatch()) return {};
    const QString name = CurveStore::findNamed(id(), q);          // fichier du dossier Density nommé dans la question
    if (name.isEmpty()) return {};
    return { QJsonObject{ { "curve", name } } };
}

bool DensityMethod::load(const QJsonObject& p, Density& out, QString& file, QString& error)
{
    QString name = p.value("curve").toString(p.value("file").toString()).trimmed();
    if (name.isEmpty()) { error = QStringLiteral("densité tabulée sans fichier (« curve » : nom du fichier CSV du dossier Calib/Density)"); return false; }
    QString path;
    if (QFileInfo(name).isAbsolute()) path = name;
    else path = CurveStore::sourceOf(QStringLiteral("density"), name);
    QFile fh(path);
    if (path.isEmpty() || !fh.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList known = CurveStore::names(QStringLiteral("density"));
        error = QStringLiteral("fichier de densité « %1 » introuvable dans les dossiers Density (dossier de l'utilisateur, Calib/Density "
                               "de ChronoModel)%2").arg(name, known.isEmpty() ? QStringLiteral(" ; l'enregistrer d'abord avec le lien « Enregistrer le fichier… »")
                                                                                 : QStringLiteral(" ; disponibles : ") + known.join(", "));
        return false;
    }
    file = path;
    std::vector<double> nums;
    QString all = QString::fromUtf8(fh.readAll());
    all.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace('\r', '\n');
    for (QString line : all.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("//")) || line.startsWith('#')) continue;
        if (!line.contains('.')) line.replace(',', '.');
        bool ok = false;
        const double x = line.section(QRegularExpression(QStringLiteral("[;\\t ]")), 0, 0).toDouble(&ok);
        if (!ok || !std::isfinite(x)) { error = QStringLiteral("fichier de densité « %1 » : valeur illisible « %2 »").arg(name, line.left(40)); return false; }
        nums.push_back(x);
    }
    if (nums.size() < 4 || !(nums[1] > nums[0])) {
        error = QStringLiteral("fichier de densité « %1 » : il faut tmin, tmax (tmax > tmin) puis au moins deux valeurs").arg(name);
        return false;
    }
    const double tmin = nums[0], tmax = nums[1];
    const size_t n = nums.size() - 2;
    out = Density();
    out.step = (tmax - tmin) / double(n - 1);
    for (size_t i = 0; i < n; ++i) { out.t.push_back(tmin + out.step * double(i)); out.v.push_back(std::max(0.0, nums[i + 2])); }
    const double a = densityArea(out);
    if (!(a > 0.0)) { error = QStringLiteral("fichier de densité « %1 » : densité nulle partout").arg(name); out = Density(); return false; }
    for (double& v : out.v) v /= a;
    return true;
}

MethodResult DensityMethod::compute(const QJsonObject& p) const
{
    Density d;
    QString file, err;
    if (!load(p, d, file, err)) return MethodResult::failure(err);
    return MethodResult::fromDensity(d, {}, false);
}

QString DensityMethod::describe(const QJsonObject& p) const
{
    QString c = QFileInfo(p.value("curve").toString(p.value("file").toString())).fileName();
    return QStringLiteral("Densité tabulée : %1").arg(c);
}

} // namespace seldon
