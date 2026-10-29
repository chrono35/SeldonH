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

#include "InverseCalibration.h"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <vector>

#include "CurveStore.h"
#include "../f14c/F14cMethod.h"
#include "TextUtil.h"

namespace seldon {

namespace {

double numberOf(const QJsonObject& o, const char* k)
{
    if (!o.contains(k)) return std::nan("");
    bool ok = false;
    const double v = o.value(k).toVariant().toString().remove(' ').replace(',', '.').toDouble(&ok);
    return ok && std::isfinite(v) ? v : std::nan("");
}

} // namespace

InverseResult inverseCalibration(const QJsonObject& rq)
{
    InverseResult R;
    auto fail = [&](const QString& why) { R.error = why; return R; };

    // 1. Courbe
    QString type = text::fold(rq.value("type").toString("c14")).trimmed();
    if (type == "14c" || type == "radiocarbone" || type == "radiocarbon") type = "c14";
    if (type == "archeomagnetisme" || type == "archeomag") type = "am";
    const RefCurve* c = nullptr;
    if (type == "c14") {
        R.curve = rq.value("curve").toString("intcal20").trimmed().toLower();
        c = CurveStore::get("c14", R.curve, CurveFormat::Radiocarbon, false);
        R.quantity = QStringLiteral("Âge 14C");
        R.unit = QStringLiteral("BP");
        R.curveRef = QStringLiteral("|c14|") + R.curve;
    } else if (type == "f14c") {
        R.curve = rq.value("curve").toString("bomb21nhz1").trimmed().toLower();
        c = F14cMethod::resolveCurve(R.curve);
        R.quantity = QStringLiteral("F14C");
        R.curveRef = (CurveStore::get("f14c", R.curve, CurveFormat::Radiocarbon, true) ? QStringLiteral("|f14c|") : QStringLiteral("|c14|")) + R.curve;
    } else if (type == "am") {
        QString comp = rq.value("component").toString(rq.value("process").toString()).trimmed().toUpper();
        if (comp.isEmpty() && rq.contains("curve")) comp = rq.value("curve").toString().right(1).toUpper();   // « …_i »
        if (comp != "I" && comp != "D" && comp != "F")
            return fail(QStringLiteral("composante archéomagnétique à préciser : « component » I, D ou F"));
        const char* key = comp == "I" ? "curve_i" : comp == "D" ? "curve_d" : "curve_f";
        const QString def = comp == "I" ? "gal2002sph2014_i" : comp == "D" ? "gal2002sph2014_d" : "gwh2013uni_f";
        R.curve = rq.value(key).toString(rq.value("curve").toString(def)).trimmed().toLower();
        R.curve.remove(QStringLiteral(".ref"));
        c = CurveStore::get("am", R.curve, CurveFormat::Simple);
        R.quantity = comp == "I" ? QStringLiteral("Inclinaison") : comp == "D" ? QStringLiteral("Déclinaison") : QStringLiteral("Intensité");
        R.unit = comp == "F" ? QStringLiteral("µT") : QStringLiteral("°");
        R.curveRef = QStringLiteral("%1|am|%2").arg(comp, R.curve);
    } else {
        return fail(QStringLiteral("type « %1 » inconnu pour la calibration inverse (c14, f14c ou am)").arg(rq.value("type").toString()));
    }
    if (!c) {
        const QString m = type == "am" ? QStringLiteral("am") : type;
        QStringList names = CurveStore::names(m);
        if (type == "f14c") names += CurveStore::names("c14");
        return fail(QStringLiteral("courbe inconnue : « %1 » (disponibles : %2)").arg(R.curve, names.join(", ")));
    }

    // 2. Loi de la date : fixe, gaussienne ou uniforme.
    const double t = numberOf(rq, "t"), te = numberOf(rq, "t_error"), t0 = numberOf(rq, "t_min"), t1 = numberOf(rq, "t_max");
    double lab = numberOf(rq, "lab_error");
    if (!std::isfinite(lab) || lab < 0.0) lab = 0.0;
    std::vector<double> ts, ws;
    if (std::isfinite(t0) && std::isfinite(t1)) {
        if (!(t1 > t0)) return fail(QStringLiteral("t_min doit être inférieur à t_max"));
        const int n = int(std::clamp((t1 - t0) / std::max(c->minStep / 3.0, 0.1), 50.0, 4000.0));
        for (int i = 0; i <= n; ++i) { ts.push_back(t0 + (t1 - t0) * i / n); ws.push_back(1.0); }
        R.tCentre = 0.5 * (t0 + t1);
        R.dateText = QStringLiteral("%1 → %2").arg(text::numAuto(t0, 1), text::numAuto(t1, 1));
    } else if (std::isfinite(t)) {
        R.tCentre = t;
        if (std::isfinite(te) && te > 0.0) {
            const int n = int(std::clamp(8.0 * te / std::max(c->minStep / 3.0, 0.1), 50.0, 4000.0));
            for (int i = 0; i <= n; ++i) {
                const double x = t - 4.0 * te + 8.0 * te * i / n;
                ts.push_back(x);
                ws.push_back(std::exp(-0.5 * (x - t) * (x - t) / (te * te)));
            }
            R.dateText = QStringLiteral("%1 ± %2").arg(text::numAuto(t, 1), text::numAuto(te, 1));
        } else {
            ts.push_back(t);
            ws.push_back(1.0);
            R.dateText = text::numAuto(t, 1);
        }
    } else {
        return fail(QStringLiteral("date calendaire absente : « t » (année signée, négatif = avant J.-C.), avec « t_error » "
                                   "facultatif, ou « t_min » et « t_max »"));
    }
    for (double x : ts)
        if (x < c->tmin() || x > c->tmax())
            return fail(QStringLiteral("date hors du domaine de la courbe « %1 » (%2 → %3)")
                            .arg(R.curve, text::yearSigned(c->tmin()), text::yearSigned(c->tmax())));
    double sw = 0.0;
    for (double w : ws) sw += w;
    for (double& w : ws) w /= sw;

    // 3. Mélange : moments et densité de la mesure.
    std::vector<double> ms(ts.size()), vs(ts.size());
    double mean = 0.0, m2 = 0.0, lo = 1e300, hi = -1e300;
    for (size_t i = 0; i < ts.size(); ++i) {
        double m, e;
        c->at(ts[i], m, e);
        ms[i] = m;
        vs[i] = std::max(e * e + lab * lab, 1e-24);
        mean += ws[i] * m;
        m2 += ws[i] * (vs[i] + m * m);
        lo = std::min(lo, m - 5.0 * std::sqrt(vs[i]));
        hi = std::max(hi, m + 5.0 * std::sqrt(vs[i]));
    }
    c->at(R.tCentre, R.curveMean, R.curveError);
    R.mean = mean;
    R.sd = std::sqrt(std::max(m2 - mean * mean, 0.0));
    if (!(hi > lo)) return fail(QStringLiteral("dispersion nulle : erreur de la courbe et lab_error nulles"));
    const int K = 1000;
    R.value.step = (hi - lo) / (K - 1);
    for (int k = 0; k < K; ++k) {
        const double x = lo + k * R.value.step;
        double p = 0.0;
        for (size_t i = 0; i < ts.size(); ++i) {
            const double d = x - ms[i];
            p += ws[i] * std::exp(-0.5 * d * d / vs[i]) / std::sqrt(vs[i]);
        }
        R.value.t.push_back(x);
        R.value.v.push_back(p);
    }
    const double area = densityArea(R.value);
    if (!(area > 0.0)) return fail(QStringLiteral("densité de la mesure nulle"));
    for (double& p : R.value.v) p /= area;

    if (type == "c14") {
        R.hasF14c = true;
        R.f14cMean = craToF14C(R.mean);
        R.f14cSd = errCraToErrF14C(R.sd, R.mean);
    }
    R.ok = true;
    return R;
}

} // namespace seldon
