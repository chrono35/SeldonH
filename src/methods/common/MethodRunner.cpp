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

#include "MethodRunner.h"

#include <QLocale>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

#include "Combination.h"
#include "CurveStore.h"
#include "CurveTraces.h"
#include "DataTable.h"
#include "DensityFile.h"
#include "../c14/C14Method.h"
#include "../f14c/F14cMethod.h"
#include "../density/DensityMethod.h"
#include "EventMcmc.h"
#include "InverseCalibration.h"
#include "MethodRegistry.h"
#include "TextUtil.h"
#include "FuzzyWords.h"
#include "Tools.h"
#include "Traces.h"

namespace seldon {

namespace {

QString intervalsText(const std::vector<Interval>& v, DateFormat fmt)
{
    QStringList items;
    for (const Interval& iv : v) {
        if (iv.prob < 0.005) continue;          // miettes de bruit numérique
        const QString from = dateformat::format(iv.from, fmt), to = dateformat::format(iv.to, fmt);
        items << (from == to ? from : QStringLiteral("%1 → %2").arg(from, to))
                     + QStringLiteral(" : %1 %").arg(text::num(iv.prob * 100.0, 1));
    }
    return items.join("\n");
}

QJsonObject plotSeries(const QString& name, const Density& d, DateFormat fmt)
{
    // Ne garde que la partie utile de la densité (> 1e-3 du maximum, marges de 20 pas), ≤ ~400 points.
    const double mx = *std::max_element(d.v.begin(), d.v.end());
    size_t i0 = 0, i1 = d.v.size() - 1;
    while (i0 < i1 && d.v[i0] < mx * 1e-3) ++i0;
    while (i1 > i0 && d.v[i1] < mx * 1e-3) --i1;
    i0 = i0 > 20 ? i0 - 20 : 0;
    i1 = std::min(d.v.size() - 1, i1 + 20);
    const size_t step = std::max<size_t>(1, (i1 - i0 + 1) / 400);
    QJsonArray xs, ys;
    for (size_t k = i0; k <= i1; k += step) {
        xs.append(dateformat::convert(d.t[k], fmt));
        ys.append(d.v[k]);
    }
    QJsonObject s;
    s["name"] = name;
    s["x"] = xs;
    s["y"] = ys;
    return s;
}

// Courbes de référence d'un calcul (Method::referenceCurves, « rôle|méthode|nom ») en clair, avec leur origine :
// « IntCal20 (intégrée) », « I : GAL2002sph2014_I (ChronoModel) ; F : GwH2013uni_F (ChronoModel) ».
QStringList curveLabels(const QStringList& refs)
{
    QStringList out;
    for (const QString& r : refs) {
        const QStringList p = r.split('|');
        if (p.size() != 3) continue;
        const QString path = CurveStore::sourceOf(p[1], p[2]);
        QString title = path.isEmpty() ? p[2] : QFileInfo(path).completeBaseName();
        if (p[1] != QLatin1String("am") && QRegularExpression(QStringLiteral("^(intcal|shcal|marine)\\d")).match(p[2]).hasMatch())
            title = C14Method::curveTitle(p[2]);
        const QString origin = path.isEmpty() ? QStringLiteral("introuvable")
                             : path.startsWith(QLatin1String(":/")) ? QStringLiteral("intégrée")
                             : path.contains(QLatin1String("/CNRS/ChronoModel/")) ? QStringLiteral("ChronoModel")
                             : QStringLiteral("dossier de l'utilisateur");
        out << (p[0].isEmpty() ? QString() : p[0] + QStringLiteral(" : ")) + QStringLiteral("%1 (%2)").arg(title, origin);
    }
    out.removeDuplicates();
    return out;
}

// Graphiques des courbes de référence d'une datation simple (14C, F14C, archéomagnétisme I, D, F) : la courbe (moyenne et
// enveloppe à 95 %, ± 1,96 σ) sur la plage utile de la datation, la mesure (trait) et son erreur à 1σ (bande et tirets),
// et la densité calibrée en bas du graphique (échelle libre), comme la vue de calibration de ChronoModel. Une figure par
// courbe ; vide pour une datation sans courbe (Gauss, uniforme, TL) ou si « curve_plot » vaut false.
QVector<QJsonObject> referenceCurvePlots(const QJsonObject& rq, const Density& calib, const QString& head, DateFormat fmt)
{
    QVector<QJsonObject> out;
    if (rq.contains("curve_plot") && !rq.value("curve_plot").toBool(true)) return out;
    if (!calib.valid()) return out;
    const MethodRegistry& reg = MethodRegistry::instance();
    const QString method = rq.value("method").toString().toLower();
    const QStringList refs = reg.referenceCurves(rq);
    if (refs.isEmpty()) return out;

    // Plage utile de la datation : densité ≥ 1e-3 du maximum, élargie d'un quart (au moins 30 ans de chaque côté).
    const double vmax = *std::max_element(calib.v.begin(), calib.v.end());
    size_t i0 = 0, i1 = calib.v.size() - 1;
    while (i0 < i1 && calib.v[i0] < vmax * 1e-3) ++i0;
    while (i1 > i0 && calib.v[i1] < vmax * 1e-3) --i1;
    const double pad = std::max(30.0, 0.25 * (calib.t[i1] - calib.t[i0]));
    const double inc = rq.value("inc").toDouble();
    constexpr double kRad = 3.14159265358979323846 / 180.0;

    for (const QString& ref : refs) {
        const QStringList p = ref.split('|');
        if (p.size() != 3) continue;
        const QString role = p[0], dir = p[1], name = p[2];
        const RefCurve* c = nullptr;
        double m = std::nan(""), e = std::nan("");
        QString what, meas;
        auto n = [](double v, int d) { return text::numAuto(v, d); };
        if (method == "c14") {
            c = CurveStore::get("c14", name, CurveFormat::Radiocarbon, false);
            const double dR = rq.value("delta_r").toDouble(), dRe = rq.value("delta_r_error").toDouble();
            m = rq.value("age").toDouble() - dR;
            e = std::sqrt(std::pow(rq.value("error").toDouble(), 2) + dRe * dRe);
            what = QStringLiteral("Âge 14C (BP)");
            meas = dR != 0.0 || dRe != 0.0 ? QStringLiteral("Mesure − ΔR = %1 ± %2 BP").arg(n(m, 1), n(e, 1))
                                           : QStringLiteral("Mesure %1 ± %2 BP").arg(n(m, 1), n(e, 1));
        } else if (method == "f14c") {
            c = F14cMethod::resolveCurve(name);
            m = rq.value("f14c").toDouble();
            e = rq.value("error").toDouble();
            what = QStringLiteral("F14C");
            meas = QStringLiteral("Mesure F14C = %1 ± %2").arg(n(m, 4), n(e, 4));
        } else if (method == "gauss") {
            c = CurveStore::get("gauss", name, CurveFormat::Simple);
            m = rq.value("age").toDouble();
            e = rq.value("error").toDouble();
            what = QStringLiteral("Mesure (courbe %1)").arg(name);
            meas = QStringLiteral("Mesure %1 ± %2").arg(n(m, 4), n(e, 4));
        } else if (method == "am") {
            c = CurveStore::get("am", name, CurveFormat::Simple);
            const double a95 = rq.value("alpha95").toDouble();
            if (role == "I") {
                m = inc; e = a95 / 2.448;
                what = QStringLiteral("Inclinaison (°)");
                meas = QStringLiteral("I = %1° ± %2° (α95 / 2,448)").arg(n(m, 2), n(e, 2));
            } else if (role == "D") {
                m = rq.value("dec").toDouble(); e = a95 / (2.448 * std::cos(inc * kRad));
                what = QStringLiteral("Déclinaison (°)");
                meas = QStringLiteral("D = %1° ± %2° (α95 / (2,448 cos I))").arg(n(m, 2), n(e, 2));
            } else if (role == "F") {
                m = rq.value("field").toDouble(); e = rq.value("error_f").toDouble();
                what = QStringLiteral("Intensité (µT)");
                meas = QStringLiteral("F = %1 ± %2 µT").arg(n(m, 2), n(e, 2));
            }
        }
        if (!c || !c->valid() || !std::isfinite(m) || !(e >= 0)) continue;
        const double a = std::max(c->tmin(), calib.t[i0] - pad), b = std::min(c->tmax(), calib.t[i1] + pad);
        if (!(b > a)) continue;

        // Courbe : ses nœuds dans la fenêtre (≤ 600 points), plus les deux bords interpolés.
        std::vector<double> ts;
        ts.push_back(a);
        size_t k0 = size_t(std::lower_bound(c->t.begin(), c->t.end(), a) - c->t.begin());
        const size_t k1 = size_t(std::upper_bound(c->t.begin(), c->t.end(), b) - c->t.begin());
        const size_t stride = std::max<size_t>(1, (k1 - std::min(k0, k1)) / 600);
        for (size_t k = k0; k < k1; k += stride) if (c->t[k] > a && c->t[k] < b) ts.push_back(c->t[k]);
        ts.push_back(b);
        QJsonArray xs, mean, lo, hi;
        double ylo = m - e, yhi = m + e;
        for (double t : ts) {
            double mu, sg;
            c->at(t, mu, sg);
            xs.append(dateformat::convert(t, fmt));
            mean.append(mu); lo.append(mu - 1.96 * sg); hi.append(mu + 1.96 * sg);
            ylo = std::min(ylo, mu - 1.96 * sg);
            yhi = std::max(yhi, mu + 1.96 * sg);
        }
        const double span = yhi - ylo > 0 ? yhi - ylo : std::max(1.0, std::abs(m));
        // Densité calibrée en bas : 25 % de la hauteur, sous la courbe.
        const double base = ylo - 0.32 * span, top = ylo - 0.04 * span;
        QJsonArray dx, dy;
        const size_t dstep = std::max<size_t>(1, (i1 - i0 + 1) / 400);
        for (size_t k = 0; k < calib.t.size(); k += dstep) {
            if (calib.t[k] < a || calib.t[k] > b) continue;
            dx.append(dateformat::convert(calib.t[k], fmt));
            dy.append(base + (top - base) * calib.v[k] / vmax);
        }
        const QJsonArray xEnds{ dateformat::convert(a, fmt), dateformat::convert(b, fmt) };
        QString curveTitle = curveLabels({ ref }).value(0, name);
        if (!role.isEmpty() && curveTitle.startsWith(role + QStringLiteral(" : "))) curveTitle = curveTitle.mid(role.size() + 3);
        const QString shortCurve = curveTitle.section(QStringLiteral(" ("), 0, 0);       // sans l'origine
        // Charte de ChronoModel (Plugin14CRefView, CalibrationView) : courbe G en mainColorDark, enveloppe ± 1,96 σ grise en
        // tirets (180, 180, 180 ; fond alpha 30), mesure en gris-bleu (98, 113, 123) : trait, tirets à ± σ et densité gaussienne
        // de la mesure sur l'axe vertical (fond alpha 50) ; densité calibrée en mainColorDark, région HPD 95 % remplie en
        // mainColorLight (alpha 100).
        const QString cG = QStringLiteral("#1f5080"), cLight = QStringLiteral("#3170b0"), cEnv = QStringLiteral("#b4b4b4"),
                      cMeas = QStringLiteral("#62717b");
        QJsonArray S{
            QJsonObject{ { "name", QStringLiteral("Courbe ± 1,96 σ") }, { "x", xs }, { "y", lo }, { "y2", hi }, { "fill", false },
                         { "color", cEnv }, { "alpha", 30.0 / 255.0 }, { "width", 1.0 }, { "dash", true } },
            QJsonObject{ { "name", shortCurve }, { "x", xs }, { "y", mean }, { "fill", false }, { "width", 1.6 }, { "color", cG } },
            QJsonObject{ { "name", meas }, { "x", xEnds }, { "y", QJsonArray{ m, m } }, { "fill", false }, { "width", 1.4 }, { "color", cMeas } },
            QJsonObject{ { "name", QString() }, { "x", xEnds }, { "y", QJsonArray{ m - e, m - e } }, { "fill", false },
                         { "dash", true }, { "width", 1.0 }, { "color", cMeas } },
            QJsonObject{ { "name", QString() }, { "x", xEnds }, { "y", QJsonArray{ m + e, m + e } }, { "fill", false },
                         { "dash", true }, { "width", 1.0 }, { "color", cMeas } } };
        // Densité de la mesure, verticale, sur le bord gauche du graphique (± 3σ, 10 % de la largeur), comme ChronoModel.
        if (e > 0) {
            const double xa = dateformat::convert(a, fmt), xb = dateformat::convert(b, fmt);
            const bool rev = dateformat::isAge(fmt);                      // axe inversé : le bord gauche est la plus grande valeur
            const double x0 = rev ? std::max(xa, xb) : std::min(xa, xb), w = 0.10 * std::abs(xb - xa) * (rev ? -1.0 : 1.0);
            QJsonArray gx, gy, gx0;
            for (int k = 0; k <= 120; ++k) {
                const double yy = m - 3.0 * e + 6.0 * e * k / 120.0;
                gy.append(yy);
                gx.append(x0 + w * std::exp(-0.5 * std::pow((yy - m) / e, 2)));
                gx0.append(x0);
            }
            S.append(QJsonObject{ { "name", QString() }, { "x", gx }, { "y", gy }, { "x2", gx0 }, { "y2", gy }, { "fill", false },
                                  { "color", cMeas }, { "alpha", 50.0 / 255.0 }, { "width", 1.0 } });
        }
        if (dx.size() >= 2) {
            S.append(QJsonObject{ { "name", method == "am" && refs.size() > 1 ? QStringLiteral("Densité calibrée (%1)").arg(rq.value("process").toString())
                                                                             : QStringLiteral("Densité calibrée") },
                                  { "x", dx }, { "y", dy }, { "fill", false }, { "width", 1.2 }, { "color", cG } });
            // Région HPD à 95 % remplie
            for (const Interval& iv : hpdIntervals(calib, 95.0)) {
                QJsonArray hx, hy, hb;
                for (size_t k = 0; k < calib.t.size(); ++k) {
                    if (calib.t[k] < std::max(a, iv.from) || calib.t[k] > std::min(b, iv.to)) continue;
                    hx.append(dateformat::convert(calib.t[k], fmt));
                    hy.append(base + (top - base) * calib.v[k] / vmax);
                    hb.append(base);
                }
                if (hx.size() >= 2)
                    S.append(QJsonObject{ { "name", QString() }, { "x", hx }, { "y", hb }, { "y2", hy }, { "fill", false },
                                          { "color", cLight }, { "alpha", 100.0 / 255.0 } });
            }
        }
        QJsonObject plot{ { "title", QStringLiteral("Courbe de référence %1%2%3").arg(role.isEmpty() ? QString() : role + QStringLiteral(" : "),
                                                                                       curveTitle, head.isEmpty() ? QString() : QStringLiteral(" — ") + head) },
                          { "xlabel", dateformat::axisLabel(fmt) }, { "ylabel", what }, { "fill", false }, { "yaxis", true }, { "yzero", false },
                          { "layout", QStringLiteral("overlay") }, { "series", S }, { "kind", QStringLiteral("reference_curve") } };
        if (dateformat::isAge(fmt)) plot["xreverse"] = true;
        out << plot;
    }
    return out;
}

// « 95 », « 99,7 »
QString levelText(double l)
{
    return std::abs(l - std::round(l)) < 1e-9 ? QString::number(std::lround(l)) : text::numAuto(l, 2);
}

// Niveaux affichés : celui demandé, puis 95 % et 68 % (sans doublon), du plus large au plus étroit.
std::vector<double> displayLevels(double level)
{
    std::vector<double> v{ level };
    for (double l : { 95.0, 68.0 })
        if (std::abs(l - level) > 1e-9) v.push_back(l);
    std::sort(v.begin(), v.end(), std::greater<double>());
    return v;
}

std::function<bool(const QString&, double)>& progressHandler()
{
    static std::function<bool(const QString&, double)> h;
    return h;
}

// Niveau de confiance d'une demande (95 % par défaut) ; `warning` non vide si la valeur est invalide.
double readLevel(const QJsonObject& rq, QString& warning)
{
    if (!rq.contains("level")) return MethodRunner::kDefaultLevel;
    const double l = rq.value("level").toVariant().toString().replace(',', '.').toDouble();
    if (l > 0 && l < 100) return l;
    warning = QStringLiteral("niveau de confiance « %1 » invalide (il doit être compris entre 0 et 100 %) : 95 % utilisé")
                  .arg(rq.value("level").toVariant().toString());
    return MethodRunner::kDefaultLevel;
}

// Tableau HPD (niveau demandé, 95 %, 68 %), MAP, moyenne, quartiles : fiche HTML et texte pour le modèle.
void appendSummary(QString& html, QString& prompt, const Density& d, const std::vector<Interval>& h95,
                   const std::vector<Interval>& h68, const DensityStats& st, double level, DateFormat fmt)
{
    const dateformat::PrecisionScope precision(st.sd);       // décimales automatiques : selon l'écart-type du résultat
    QString rows, promptHpd;
    for (double l : displayLevels(level)) {
        const std::vector<Interval> iv = std::abs(l - 95.0) < 1e-9 ? h95 : std::abs(l - 68.0) < 1e-9 ? h68 : hpdIntervals(d, l);
        const QString t = intervalsText(iv, fmt), lt = levelText(l);
        const bool asked = std::abs(l - level) < 1e-9 && std::abs(level - MethodRunner::kDefaultLevel) > 1e-9;
        rows += QStringLiteral("<tr><td valign='top'><b>HPD %1 %</b>%2</td><td>%3</td></tr>")
                    .arg(lt, asked ? QStringLiteral(" <i>(demandé)</i>") : QString(), QString(t).replace('\n', "<br>"));
        promptHpd += QStringLiteral("Intervalles HPD à %1 %%2 :\n- %3\n")
                         .arg(lt, asked ? QStringLiteral(" (niveau demandé par l'utilisateur)") : QString(), QString(t).replace("\n", "\n- "));
    }
    html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1"
                           "<tr><td><b>MAP (mode)</b></td><td>%2</td></tr>"
                           "<tr><td><b>Moyenne ; écart-type</b></td><td>%3 ; %4</td></tr>"
                           "<tr><td><b>Q1 ; Q2 (médiane) ; Q3</b></td><td>%5 ; %6 ; %7</td></tr>"
                           "</table>")
                .arg(rows, dateformat::format(st.mode, fmt), dateformat::format(st.mean, fmt), dateformat::spread(st.sd, fmt),
                     dateformat::format(st.q1, fmt), dateformat::format(st.median, fmt), dateformat::format(st.q3, fmt));
    prompt += promptHpd;
    prompt += QStringLiteral("MAP (mode) : %1. Moyenne : %2, écart-type de la densité : %3 (ce n'est pas un intervalle). "
                             "Quartiles : Q1 = %4, Q2 (médiane) = %5, Q3 = %6.\n")
                  .arg(dateformat::format(st.mode, fmt), dateformat::format(st.mean, fmt), dateformat::spread(st.sd, fmt),
                       dateformat::format(st.q1, fmt), dateformat::format(st.median, fmt), dateformat::format(st.q3, fmt))
                  .replace(QStringLiteral("J.-C.."), QStringLiteral("J.-C."));
}

// ------------------------------------------------------------------ événement (MCMC)

int intOf(const QJsonObject& o, std::initializer_list<const char*> keys, int def)
{
    for (const char* k : keys)
        if (o.contains(k)) {
            const double v = o.value(k).toVariant().toString().remove(' ').replace(',', '.').toDouble();
            if (std::isfinite(v)) return int(std::lround(v));
        }
    return def;
}

double numOf(const QJsonObject& o, std::initializer_list<const char*> keys, double def)
{
    for (const char* k : keys)
        if (o.contains(k)) {
            bool ok = false;
            const double v = o.value(k).toVariant().toString().remove(' ').replace(',', '.').toDouble(&ok);
            if (ok && std::isfinite(v)) return v;
        }
    return def;
}

// Clés du bloc « mcmc » d'un projet .chr (num_proc = nombre de chaînes), avec quelques synonymes.
McmcSettings mcmcFromJson(const QJsonObject& m)
{
    McmcSettings s;
    s.numChains        = intOf(m, { "num_proc", "chains", "num_chains" }, s.numChains);
    s.numBurn          = intOf(m, { "num_burn", "burn", "burn_in" }, s.numBurn);
    s.maxBatches       = intOf(m, { "max_batches" }, s.maxBatches);
    s.iterPerBatch     = intOf(m, { "iter_per_batch" }, s.iterPerBatch);
    s.numIter          = intOf(m, { "num_iter", "iterations" }, s.numIter);
    s.thinning         = intOf(m, { "thinning_interval", "thinning" }, s.thinning);
    s.mixingLevel      = numOf(m, { "mixing_level", "mixing" }, s.mixingLevel);
    s.annealTemp       = intOf(m, { "anneal_temp" }, s.annealTemp);
    s.annealRecurrence = intOf(m, { "anneal_recurrence" }, s.annealRecurrence);
    s.annealDwell      = intOf(m, { "anneal_dwell" }, s.annealDwell);
    const QJsonValue sv = m.value("seeds");
    QStringList parts;
    if (sv.isArray()) for (const QJsonValue& v : sv.toArray()) parts << v.toVariant().toString();
    else if (sv.isString()) parts = sv.toString().split(QRegularExpression("[;,\\s]+"), Qt::SkipEmptyParts);
    else if (sv.isDouble()) parts << QString::number(sv.toDouble(), 'f', 0);
    for (const QString& p : parts) { bool ok = false; const uint v = p.trimmed().toUInt(&ok); if (ok && v > 0) s.seeds.push_back(v); }
    return s;
}

bool wiggleFromJson(const QJsonValue& v, Wiggle& w, QString& err)
{
    w = Wiggle();
    if (v.isUndefined() || v.isNull()) return true;
    if (v.isDouble()) { w.type = Wiggle::Fixed; w.fixed = v.toDouble(); return true; }
    const QJsonObject o = v.toObject();
    const QString t = o.value("type").toString().toLower();
    if (t.isEmpty() || t == "none" || t == "aucun") return true;
    if (t == "fixed" || t == "fixe") {
        w.type = Wiggle::Fixed; w.fixed = numOf(o, { "value", "delta", "fixed" }, std::nan(""));
        if (std::isnan(w.fixed)) { err = QStringLiteral("décalage fixe sans « value »"); return false; }
    } else if (t == "range" || t == "intervalle" || t == "uniform") {
        w.type = Wiggle::Range; w.min = numOf(o, { "min" }, std::nan("")); w.max = numOf(o, { "max" }, std::nan(""));
        if (std::isnan(w.min) || std::isnan(w.max) || w.min >= w.max) { err = QStringLiteral("décalage « range » : min < max requis"); return false; }
    } else if (t == "gaussian" || t == "gauss" || t == "normal") {
        w.type = Wiggle::Gaussian; w.mean = numOf(o, { "mean", "average", "value" }, std::nan("")); w.error = numOf(o, { "error", "sd" }, std::nan(""));
        if (std::isnan(w.mean) || !(w.error > 0)) { err = QStringLiteral("décalage gaussien : « mean » et « error » > 0 requis"); return false; }
    } else {
        err = QStringLiteral("type de décalage inconnu « %1 » (fixed, range ou gaussian)").arg(t);
        return false;
    }
    return true;
}

QString wiggleText(const Wiggle& w)
{
    switch (w.type) {
    case Wiggle::Fixed:    return QStringLiteral("décalage δ = %1 ans").arg(text::numAuto(w.fixed, 2));
    case Wiggle::Range:    return QStringLiteral("décalage δ ∈ [%1 ; %2] ans").arg(text::numAuto(w.min, 2), text::numAuto(w.max, 2));
    case Wiggle::Gaussian: return QStringLiteral("décalage δ ~ N(%1 ; %2) ans").arg(text::numAuto(w.mean, 2), text::numAuto(w.error, 2));
    default:               return QString();
    }
}

QString convergenceText(Convergence c)
{
    switch (c) {
    case Convergence::Good:    return QStringLiteral("✅ convergence satisfaisante");
    case Convergence::Warning: return QStringLiteral("⚠️ convergence douteuse, à surveiller");
    case Convergence::Bad:     return QStringLiteral("❌ convergence non atteinte");
    default:                   return QStringLiteral("convergence non évaluée (R̂ demande au moins deux chaînes)");
    }
}

QString essText(double e) { return std::isfinite(e) ? text::num(e, 0) : QStringLiteral("—"); }
QString rhatText(double r) { return std::isfinite(r) ? (r > 0 ? text::num(r, 4) : QStringLiteral("—")) : QStringLiteral("∞"); }

struct EventRun {
    bool ok = false;
    QString html, prompt;
    QJsonArray series;
    QJsonObject trace;
    QString name;                    // nom donné à l'événement (vide sinon) : titre du graphique
    bool curves = false;             // au moins une datation utilise une courbe de référence
    double level = MethodRunner::kDefaultLevel;
    Density theta;                   // θ a posteriori (comparaison de deux événements : runOrder)
};

EventRun runEvent(const QJsonObject& rq, DateFormat fmt)
{
    EventRun E;
    const MethodRegistry& reg = MethodRegistry::instance();
    const QString name = rq.value("name").toString().trimmed().isEmpty() ? QStringLiteral("Événement") : rq.value("name").toString().trimmed();
    const QJsonArray dates = rq.value("dates").toArray();
    const bool named = !rq.value("name").toString().trimmed().isEmpty();
    if (named) E.name = name;
    const QString eventColor = MethodRunner::colorOf(rq);
    const QString head = QStringLiteral("%1 — combinaison bayésienne de %2 datation%3 (modèle d'événement de ChronoModel, MCMC)")
                             .arg(named ? QStringLiteral("Événement « %1 »").arg(name) : name).arg(dates.size()).arg(dates.size() > 1 ? "s" : "");
    E.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
    E.prompt += QStringLiteral("\n%1\n").arg(head);
    const int headHtmlEnd = E.html.size(), headPromptEnd = E.prompt.size();   // la période d'étude viendra juste après
    auto fail = [&](const QString& why) {
        E.html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(why.toHtmlEscaped());
        E.prompt += QStringLiteral("Calcul impossible : %1.\n").arg(why);
        return E;
    };
    if (dates.isEmpty()) return fail(QStringLiteral("l'événement ne contient aucune datation (« dates »)"));

    // 1. Chaque datation est calibrée par sa méthode.
    std::vector<EventDateInput> inputs;
    std::vector<MethodResult> calibs;
    QStringList curveRefs;           // courbes de référence de toutes les datations
    QStringList names;               // nom de la date et ce qu'elle est (« Ly-1234 — 14C : 2500 ± 30 BP — IntCal20 ») : texte
    QStringList plotNames;           // nom de la date seul : légende des graphiques
    QStringList colors;              // couleur de chaque datation (sinon celle de l'événement, sinon défaut)
    double lo = 1e300, hi = -1e300;
    for (int k = 0; k < dates.size(); ++k) {
        QJsonObject d = MethodRunner::normalizeRequest(dates[k].toObject());
        const QString dc = MethodRunner::colorOf(d);
        colors << (dc.isEmpty() ? eventColor : dc);
        const QString method = d.value("method").toString().toLower();
        if (method.isEmpty() || method == "event") return fail(QStringLiteral("la datation %1 n'a pas de « method » valide").arg(k + 1));
        d["method"] = method;
        if (method == "am") { d["process"] = d.value("process").toString("I").toUpper(); if (!d.contains("integration_steps")) d["integration_steps"] = 500; }
        if (d.contains("curve")) d["curve"] = d.value("curve").toString().toLower();
        if (method == "c14" && d.contains("delta_r")) d["has_delta_r"] = true;
        // Une date nommée comme l'événement (le modèle de langage a recopié son nom) n'a pas de nom propre.
        QString dateName = d.value("name").toString().trimmed();
        if (dateName.compare(name, Qt::CaseInsensitive) == 0) dateName.clear();
        const QString desc = reg.describe(d);
        const QString what = desc.isEmpty() ? QStringLiteral("Date %1").arg(k + 1) : desc;
        const QString label = dateName.isEmpty() ? what : desc.isEmpty() ? dateName : dateName + QStringLiteral(" — ") + desc;
        const MethodResult r = reg.compute(d);
        if (!r.ok) return fail(QStringLiteral("datation %1 (%2) : %3").arg(k + 1).arg(label, r.error));
        curveRefs << reg.referenceCurves(d);
        Wiggle w;
        QString werr;
        if (!wiggleFromJson(d.value("wiggle"), w, werr)) return fail(QStringLiteral("datation %1 (%2) : %3").arg(k + 1).arg(label, werr));
        double dlo = 0, dhi = 0;
        if (w.type == Wiggle::Fixed) dlo = dhi = w.fixed;
        else if (w.type == Wiggle::Range) { dlo = w.min; dhi = w.max; }
        else if (w.type == Wiggle::Gaussian) { dlo = w.mean - 3 * w.error; dhi = w.mean + 3 * w.error; }
        lo = std::min(lo, r.density.t.front() + dlo);
        hi = std::max(hi, r.density.t.back() + dhi);
        inputs.push_back({ (dateName.isEmpty() ? what : dateName).toStdString(), r.density, w });     // variables du MCMC (R̂, ESS)
        // Affichage : densité de t + δ (calibration « wiggle » de ChronoModel) ; le MCMC garde la vraisemblance sans décalage.
        MethodResult shown = r;
        if (w.type != Wiggle::None) {
            shown.density = wiggleDensity(r.density, w);
            shown.hpd95 = hpdIntervals(shown.density, 95.0);
        }
        calibs.push_back(shown);
        names << label;
        plotNames << (dateName.isEmpty() ? what : dateName);
    }

    if (const QStringList cl = curveLabels(curveRefs); !cl.isEmpty()) {
        E.html += QStringLiteral("<p><i>Courbe%1 de référence : %2</i></p>").arg(cl.size() > 1 ? "s" : "", cl.join(" ; ").toHtmlEscaped());
        E.prompt += QStringLiteral("Courbe%1 de référence : %2.\n").arg(cl.size() > 1 ? "s" : "", cl.join(" ; "));
        E.curves = true;
    }

    // 2. Période d'étude (loi a priori uniforme de θ).
    double tmin = numOf(rq, { "tmin" }, std::nan("")), tmax = numOf(rq, { "tmax" }, std::nan(""));
    const QJsonObject st = rq.value("settings").toObject();
    if (std::isnan(tmin)) tmin = numOf(st, { "tmin" }, std::nan(""));
    if (std::isnan(tmax)) tmax = numOf(st, { "tmax" }, std::nan(""));
    const bool autoPeriod = std::isnan(tmin) || std::isnan(tmax);
    if (autoPeriod) {
        const double margin = std::max(50.0, 0.1 * (hi - lo));
        tmin = std::floor((lo - margin) / 10.0) * 10.0;
        tmax = std::ceil((hi + margin) / 10.0) * 10.0;
    }
    if (!(tmax > tmin)) return fail(QStringLiteral("période d'étude invalide (tmin doit être inférieur à tmax)"));
    // Affichée en tête, juste après le titre : elle situe la datation avant les résultats.
    const QString period = QStringLiteral("%1 → %2").arg(dateformat::format(tmin, fmt), dateformat::format(tmax, fmt));
    const QString periodWhy = autoPeriod ? QStringLiteral("calculée d'après les datations") : QStringLiteral("fixée dans la demande");
    E.html.insert(headHtmlEnd, QStringLiteral("<p><b>Période d'étude</b> : %1 <i>(a priori uniforme de θ, %2)</i></p>")
                                   .arg(period.toHtmlEscaped(), periodWhy));
    E.prompt.insert(headPromptEnd, QStringLiteral("Période d'étude (a priori uniforme de θ, %1) : %2.\n").arg(periodWhy, period));

    // 3. MCMC
    QString levelWarning;
    E.level = readLevel(rq, levelWarning);
    const McmcSettings ms = mcmcFromJson(rq.value("mcmc").toObject());
    McmcProgress prog;
    if (progressHandler()) {
        prog = [name](double f) { return progressHandler()(QStringLiteral("MCMC de l'événement « %1 »").arg(name), f); };
    }
    const EventMcmcResult R = runEventMcmc(inputs, ms, tmin, tmax, prog);
    if (!R.ok) return fail(QString::fromStdString(R.error));

    // 4. θ a posteriori
    const std::vector<Interval> t95 = hpdIntervals(R.theta, 95.0), t68 = hpdIntervals(R.theta, 68.0);
    const DensityStats ts = densityStats(R.theta);
    E.theta = R.theta;
    E.html += QStringLiteral("<p><b>θ a posteriori</b> (date de l'événement)</p>");
    E.prompt += QStringLiteral("Date de l'événement θ (densité a posteriori) :\n");
    appendSummary(E.html, E.prompt, R.theta, t95, t68, ts, E.level, fmt);
    // Lissage de la densité de θ (estimation par noyau gaussien des tirages MCMC) : h fait partie du résultat.
    const QString hRule = R.thetaBandwidthSJ ? QStringLiteral("Sheather-Jones") : QStringLiteral("Silverman (repli : l'équation de Sheather-Jones n'a pas de solution)");
    const QString hTheta = dateformat::spread(R.thetaBandwidth, fmt);
    E.html += QStringLiteral("<p><small>Densité de θ lissée par un noyau gaussien de largeur de bande <b>h = %1</b> (règle de %2), "
                             "sur %3 tirages.</small></p>")
                  .arg(hTheta.toHtmlEscaped(), hRule.toHtmlEscaped(),
                       QLocale(QLocale::French).toString(qlonglong(R.thetaTraces.empty() ? 0 : R.thetaTraces.size() * R.thetaTraces.front().size())));
    E.prompt += QStringLiteral("Densité de θ lissée par noyau gaussien : h = %1 (règle de %2).\n").arg(hTheta, hRule);
    if (!levelWarning.isEmpty()) {
        E.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(levelWarning.toHtmlEscaped());
        E.prompt += QStringLiteral("Avertissement : %1.\n").arg(levelWarning);
    }

    // 5. Les datations : calibration seule et a posteriori dans l'événement
    QString rows;
    E.prompt += QStringLiteral("Datations de l'événement (calibration individuelle → valeur a posteriori t_i dans l'événement ; "
                               "σ_i = dispersion de la date autour de θ, grande si la date est discordante). Pour une date avec "
                               "décalage δ (wiggle), la calibration donnée est celle de t + δ, et t_i + δ_i a posteriori est la date "
                               "ramenée à l'événement, t_i étant la date de l'échantillon lui-même :\n");
    for (size_t k = 0; k < inputs.size(); ++k) {
        const bool wig = inputs[k].wiggle.type != Wiggle::None;
        const dateformat::PrecisionScope precision(densityStats(R.ti[k]).sd);
        const QString c95 = intervalsText(calibs[k].hpd95, fmt), p95 = intervalsText(hpdIntervals(R.ti[k], 95.0), fmt);
        const QString y95 = wig ? intervalsText(hpdIntervals(R.yi[k], 95.0), fmt) : QString();
        const QString wt = wiggleText(inputs[k].wiggle);
        const QString sg = std::isfinite(R.sigmaMedian[k]) ? dateformat::spread(R.sigmaMedian[k], fmt) : QStringLiteral("—");
        QString post = QString(p95).replace('\n', "<br>");
        if (wig) post = QStringLiteral("t : %1<br>t + δ : %2").arg(post, QString(y95).replace('\n', "<br>"));
        // Largeur de bande du noyau qui lisse la densité a posteriori (règle de repli signalée)
        auto hText = [&](double h, bool sj) {
            return dateformat::spread(h, fmt) + (sj ? QString() : QStringLiteral(" (Silverman)"));
        };
        const QString hTi = k < R.tiBandwidth.size() ? hText(R.tiBandwidth[k], R.tiBandwidthSJ[k]) : QStringLiteral("—");
        const QString hYi = wig && k < R.yiBandwidth.size() ? hText(R.yiBandwidth[k], R.yiBandwidthSJ[k]) : QString();
        const QString hCell = wig ? QStringLiteral("t : %1<br>t + δ : %2").arg(hTi.toHtmlEscaped(), hYi.toHtmlEscaped()) : hTi.toHtmlEscaped();
        rows += QStringLiteral("<tr><td valign='top'>%1%2</td><td valign='top'>%3</td><td valign='top'>%4</td><td valign='top'>%5</td><td valign='top'>%6</td></tr>")
                    .arg(names[int(k)].toHtmlEscaped(), wt.isEmpty() ? QString() : QStringLiteral("<br><i>%1</i>").arg(wt.toHtmlEscaped()),
                         QString(c95).replace('\n', "<br>"), post, sg, hCell);
        E.prompt += QStringLiteral("- %1%2 : calibration%6 HPD 95 % = %3 ; t a posteriori HPD 95 % = %4%7 ; σ médian = %5 ; h du lissage = %8\n")
                        .arg(names[int(k)], wt.isEmpty() ? QString() : " (" + wt + ")", QString(c95).replace('\n', " ; "),
                             QString(p95).replace('\n', " ; "), sg, wig ? QStringLiteral(" de t + δ") : QString(),
                             wig ? QStringLiteral(" ; t + δ a posteriori HPD 95 % = %1").arg(QString(y95).replace('\n', " ; ")) : QString(),
                             wig ? QStringLiteral("%1 (t), %2 (t + δ)").arg(hTi, hYi) : hTi);
    }
    E.html += QStringLiteral("<table cellspacing='0' cellpadding='3'><tr><th align='left'>Datation</th><th align='left'>Calibration HPD 95 %</th>"
                             "<th align='left'>a posteriori HPD 95 %</th><th align='left'>σ médian</th><th align='left'>h (lissage)</th></tr>%1</table>").arg(rows);

    // 6. Convergence
    const size_t M = R.chains.size();
    QString label = convergenceText(R.status);
    if (R.status != Convergence::Unknown)
        label += QStringLiteral(" (R̂ max = %1 ; ESS min = %2)").arg(rhatText(R.maxRhat), essText(R.minEss));
    // Variables sous les seuils « satisfaisants » (souvent les dispersions σ_i et S0, moins bien mélangées que θ).
    QStringList weak;
    for (const VarDiagnostic& v : R.diagnostics)
        if (M >= 2 && (!std::isfinite(v.rhat) || v.rhat >= mcmcdiag::kRhatGood
                       || !(std::min(v.bulkEss, v.tailEss) >= mcmcdiag::kEssGoodPerChain * double(M))))
            weak << QString::fromStdString(v.name);
    if (!weak.isEmpty()) label += QStringLiteral(" — variables sous les seuils : %1").arg(weak.join(", "));
    E.html += QStringLiteral("<p><b>Convergence</b> : %1</p>").arg(label.toHtmlEscaped());
    QString drows;
    for (const VarDiagnostic& v : R.diagnostics) {
        const QString color = !std::isfinite(v.rhat) || v.rhat >= mcmcdiag::kRhatWarning ? "#c0392b"
                            : v.rhat >= mcmcdiag::kRhatGood ? "#d68910" : QString();
        const QString rh = color.isEmpty() ? rhatText(v.rhat) : QStringLiteral("<font color='%1'><b>%2</b></font>").arg(color, rhatText(v.rhat));
        drows += QStringLiteral("<tr><td>%1</td><td align='right'>%2</td><td align='right'>%3</td><td align='right'>%4</td><td align='right'>%5</td></tr>")
                     .arg(QString::fromStdString(v.name).toHtmlEscaped(), rh, essText(v.bulkEss), essText(v.tailEss),
                          v.acceptRate >= 0 ? text::num(100.0 * v.acceptRate, 1) + " %" : QStringLiteral("—"));
    }
    E.html += QStringLiteral("<table cellspacing='0' cellpadding='3'><tr><th align='left'>Variable</th><th>R̂</th><th>ESS bulk</th>"
                             "<th>ESS tail</th><th>Acceptation</th></tr>%1</table>").arg(drows);
    const QLocale fr(QLocale::French);
    E.html += QStringLiteral("<p><small><i>Lecture (seuils de ChronoModel) : R̂ &lt; 1,01 et ESS ≥ %1 (500 par chaîne) → satisfaisant ; "
                             "R̂ &lt; 1,05 et ESS ≥ %2 (100 par chaîne) → à surveiller ; au-delà → non convergé. "
                             "Acceptation visée ≈ 30–46 % (marche aléatoire du bloc (t, σ), décalage rigide de θ).</i></small></p>")
                  .arg(fr.toString(500 * int(M)), fr.toString(100 * int(M)));
    E.prompt += QStringLiteral("Convergence : %1. Seuils : R̂ < 1,01 et ESS ≥ %2 satisfaisant ; R̂ < 1,05 et ESS ≥ %3 douteux ; sinon non convergé.\n")
                    .arg(label).arg(text::num(500.0 * double(M), 0), text::num(100.0 * double(M), 0));
    for (const VarDiagnostic& v : R.diagnostics)
        E.prompt += QStringLiteral("- %1 : R̂ = %2, ESS bulk = %3, ESS tail = %4%5\n")
                        .arg(QString::fromStdString(v.name), rhatText(v.rhat), essText(v.bulkEss), essText(v.tailEss),
                             v.acceptRate >= 0 ? QStringLiteral(", acceptation %1 %").arg(text::num(100.0 * v.acceptRate, 1)) : QString());

    // 7. Paramètres MCMC et chaînes
    QStringList seeds, adapt;
    for (size_t c = 0; c < M; ++c) {
        seeds << QString::number(R.chains[c].seed);
        if (!R.chains[c].adapted)
            adapt << QStringLiteral("chaîne %1 non adaptée après %2 lots").arg(c + 1).arg(R.chains[c].adaptBatches);
    }
    McmcSettings used = ms;
    used.numChains = int(M);
    const QString annealText = used.annealRecurrence > 0
        ? QStringLiteral("recuit T max = 2^%1 tous les %2 itérations (dwell %3)").arg(used.annealTemp).arg(used.annealRecurrence).arg(used.annealDwell)
        : QStringLiteral("sans recuit (anneal_recurrence = 0)");
    const QString mcmcLine = QStringLiteral("%1 chaînes ; burn-in %2 ; adaptation ≤ %3 lots de %4 ; acquisition %5 itérations (thinning %6, "
                                            "%7 tirages par chaîne) ; mixing %8 ; %9")
                                 .arg(M).arg(used.numBurn).arg(used.maxBatches).arg(used.iterPerBatch).arg(used.numIter).arg(used.thinning)
                                 .arg(R.thetaTraces.empty() ? 0 : int(R.thetaTraces.front().size()))
                                 .arg(text::numAuto(used.mixingLevel, 4)).arg(annealText);
    // Tableau des paramètres, une ligne par étape du MCMC (dans l'ordre où elles se déroulent).
    const int kept = R.thetaTraces.empty() ? 0 : int(R.thetaTraces.front().size());
    auto mrow = [](const QString& k, const QString& v) {
        return QStringLiteral("<tr><td valign='top'>%1</td><td valign='top'>%2</td></tr>").arg(k, v);
    };
    QString mrows;
    mrows += mrow(QStringLiteral("Chaînes"), QString::number(M));
    mrows += mrow(QStringLiteral("Burn-in"), QStringLiteral("%1 itérations").arg(fr.toString(used.numBurn)));
    mrows += mrow(QStringLiteral("Adaptation des pas"),
                  QStringLiteral("jusqu'à %1 lots de %2 itérations — %3")
                      .arg(fr.toString(used.maxBatches), fr.toString(used.iterPerBatch),
                           adapt.isEmpty() ? QStringLiteral("toutes les chaînes adaptées") : adapt.join(", ").toHtmlEscaped()));
    mrows += mrow(QStringLiteral("Acquisition"),
                  QStringLiteral("%1 itérations, 1 tirage gardé sur %2 (thinning) → %3 tirages par chaîne")
                      .arg(fr.toString(used.numIter), fr.toString(used.thinning), fr.toString(kept)));
    mrows += mrow(QStringLiteral("Mixing"), text::numAuto(used.mixingLevel, 4).toHtmlEscaped());
    {
        QStringList hs;
        for (size_t k = 0; k < R.tiBandwidth.size() && k < size_t(names.size()); ++k) {
            QString hk = dateformat::spread(R.tiBandwidth[k], fmt);
            if (k < R.tiBandwidthSJ.size() && !R.tiBandwidthSJ[k]) hk += QStringLiteral(" (Silverman)");
            if (k < R.yiBandwidth.size() && inputs[k].wiggle.type != Wiggle::None) {
                hk += QStringLiteral(", t + δ : ") + dateformat::spread(R.yiBandwidth[k], fmt);
                if (!R.yiBandwidthSJ[k]) hk += QStringLiteral(" (Silverman)");
            }
            hs << QStringLiteral("%1 : %2").arg(plotNames[int(k)].toHtmlEscaped(), hk.toHtmlEscaped());
        }
        mrows += mrow(QStringLiteral("Lissage des densités"),
                      QStringLiteral("noyau gaussien, règle de %1 ; h(θ) = <b>%2</b>%3")
                          .arg(hRule.toHtmlEscaped(), hTheta.toHtmlEscaped(),
                               hs.isEmpty() ? QString() : QStringLiteral(" ; h des t<sub>i</sub> : ") + hs.join(", ")));
    }
    mrows += mrow(QStringLiteral("Recuit simulé"),
                  used.annealRecurrence > 0
                      ? QStringLiteral("toutes les %1 itérations, température jusqu'à 2<sup>%2</sup> pendant %3 itérations")
                            .arg(fr.toString(used.annealRecurrence)).arg(used.annealTemp).arg(fr.toString(used.annealDwell))
                      : QStringLiteral("désactivé (anneal_recurrence = 0)"));
    mrows += mrow(QStringLiteral("Graines"), QStringLiteral("%1 <i>(à réutiliser pour reproduire le calcul)</i>").arg(seeds.join(";")));
    mrows += mrow(QStringLiteral("Durée du calcul"), QStringLiteral("%1 s").arg(text::num(R.elapsedMs / 1000.0, 1)));
    E.html += QStringLiteral("<p><b>Paramètres du MCMC</b></p><table cellspacing='0' cellpadding='3'>%1</table>").arg(mrows);
    E.prompt += QStringLiteral("MCMC : %1. Graines : %2. %3.\n")
                    .arg(mcmcLine, seeds.join(";"), adapt.isEmpty() ? QStringLiteral("Toutes les chaînes sont adaptées") : adapt.join(", "));

    // 8. Figures : θ (rempli) + calibrations (traits) ; traces de θ par chaîne.
    QJsonObject ts0 = plotSeries(QStringLiteral("%1 : θ a posteriori").arg(name), R.theta, fmt);
    if (!eventColor.isEmpty()) ts0["color"] = eventColor;
    E.series.append(ts0);
    for (size_t k = 0; k < inputs.size(); ++k) {
        QJsonObject s = plotSeries(plotNames[int(k)], calibs[k].density, fmt);
        s["fill"] = false;
        s["width"] = 1.2;
        if (!colors[int(k)].isEmpty()) s["color"] = colors[int(k)];
        E.series.append(s);
    }
    // Graphique des traces de θ : seulement sur demande (« traces »: true). Long à dessiner et peu lisible ;
    // les traces restent calculées (R̂, ESS).
    if (!rq.value("traces").toBool()) {
        E.ok = true;
        return E;
    }
    QJsonArray traces;
    for (size_t c = 0; c < R.thetaTraces.size(); ++c) {
        const std::vector<double>& tr = R.thetaTraces[c];
        const size_t step = std::max<size_t>(1, tr.size() / 600);
        QJsonArray xs, ys;
        for (size_t i = 0; i < tr.size(); i += step) { xs.append(double(i * size_t(used.thinning))); ys.append(dateformat::convert(tr[i], fmt)); }
        traces.append(QJsonObject{ { "name", QStringLiteral("chaîne %1").arg(c + 1) },
                                   { "x", xs }, { "y", ys }, { "width", 1.0 } });
    }
    E.trace = QJsonObject{ { "title", QStringLiteral("Traces de θ — %1").arg(name) }, { "xlabel", QStringLiteral("Itération (acquisition)") },
                           { "ylabel", QStringLiteral("θ") }, { "yzero", false },
                           { "yaxis", true }, { "series", traces } };
    E.ok = true;
    return E;
}

// ------------------------------------------------------------------ combinaison de datations (produit des vraisemblances)

// Densité restreinte à la période d'étude [tmin ; tmax] (bornes facultatives) et ramenée à une aire de 1.
// `note` : « période d'étude … » ; `kept` : part de la probabilité gardée. Densité non valide si rien ne reste.
Density restrictToPeriod(const Density& in, const QJsonObject& rq, DateFormat fmt, QString& note, double& kept)
{
    note.clear();
    kept = 1.0;
    const double pmin = numOf(rq, { "tmin" }, std::nan("")), pmax = numOf(rq, { "tmax" }, std::nan(""));
    if (!std::isfinite(pmin) && !std::isfinite(pmax)) return in;
    const double lo = std::isfinite(pmin) ? pmin : -std::numeric_limits<double>::infinity();
    const double hi = std::isfinite(pmax) ? pmax : std::numeric_limits<double>::infinity();
    note = QStringLiteral("période d'étude %1 → %2")
               .arg(std::isfinite(pmin) ? dateformat::format(pmin, fmt) : QStringLiteral("…"),
                    std::isfinite(pmax) ? dateformat::format(pmax, fmt) : QStringLiteral("…"));
    Density d;
    d.step = in.step;
    for (size_t i = 0; i < in.t.size(); ++i)
        if (in.t[i] >= lo && in.t[i] <= hi) { d.t.push_back(in.t[i]); d.v.push_back(in.v[i]); }
    const double full = densityArea(in), part = d.valid() ? densityArea(d) : 0.0;
    if (!(part > 0.0)) { kept = 0.0; return Density(); }
    for (double& y : d.v) y /= part;
    kept = full > 0.0 ? part / full : 1.0;
    return d;
}

struct CombineRun {
    bool ok = false;
    QString html, prompt;
    QJsonArray series;
    QString name;                    // nom donné à la combinaison (vide sinon) : titre du graphique
    bool curves = false;
    double level = MethodRunner::kDefaultLevel;
    Density combined;                // densité combinée (comparaison : runOrder)
};

CombineRun runCombine(const QJsonObject& rq, DateFormat fmt)
{
    CombineRun C;
    const MethodRegistry& reg = MethodRegistry::instance();
    const bool named = !rq.value("name").toString().trimmed().isEmpty();
    const QString name = named ? rq.value("name").toString().trimmed() : QStringLiteral("Combinaison");
    if (named) C.name = name;
    const QJsonArray dates = rq.value("dates").toArray();
    const QString head = QStringLiteral("%1 — combinaison de %2 datation%3 (produit des vraisemblances, « Combine » de ChronoModel)")
                             .arg(named ? QStringLiteral("Combinaison « %1 »").arg(name) : name).arg(dates.size())
                             .arg(dates.size() > 1 ? "s" : "");
    C.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
    C.prompt += QStringLiteral("\n%1\n").arg(head);
    auto fail = [&](const QString& why) {
        C.html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(why.toHtmlEscaped());
        C.prompt += QStringLiteral("Calcul impossible : %1.\n").arg(why);
        return C;
    };
    if (dates.size() < 2) return fail(QStringLiteral("une combinaison demande au moins deux datations (« dates »)"));

    QString levelWarning;
    C.level = readLevel(rq, levelWarning);
    const QString combColor = MethodRunner::colorOf(rq);

    // 1. Chaque datation : densité NON tronquée (vraisemblance entière), décalée par son wiggle (densité de t + δ).
    std::vector<Density> dens;
    std::vector<QJsonObject> plain;  // datations normalisées, pour le test de Ward et Wilson
    QStringList labels, colors, wiggles, curveRefs;
    for (int k = 0; k < dates.size(); ++k) {
        QJsonObject d = MethodRunner::normalizeRequest(dates[k].toObject());
        const QString method = d.value("method").toString().toLower();
        if (method.isEmpty() || method == "event" || method == "combine" || method == "relation" || method == "inverse"
            || method == "density_file")
            return fail(QStringLiteral("la datation %1 n'a pas de « method » de datation valide (c14, f14c, tl, gauss, unif, am)").arg(k + 1));
        d["method"] = method;
        if (method == "am") { d["process"] = d.value("process").toString("I").toUpper(); if (!d.contains("integration_steps")) d["integration_steps"] = 500; }
        if (d.contains("curve")) d["curve"] = d.value("curve").toString().toLower();
        if (method == "c14" && d.contains("delta_r")) d["has_delta_r"] = true;
        const QString dc = MethodRunner::colorOf(d);
        colors << dc;
        QString label = d.value("name").toString().trimmed();
        const QString desc = reg.describe(d);
        if (label.isEmpty()) label = desc.isEmpty() ? QStringLiteral("Date %1").arg(k + 1) : desc;
        Wiggle w;
        QString werr;
        if (!wiggleFromJson(d.value("wiggle"), w, werr)) return fail(QStringLiteral("datation %1 (%2) : %3").arg(k + 1).arg(label, werr));
        QJsonObject full = d;
        full["untruncated"] = true;
        full.remove("tmin");
        full.remove("tmax");
        const MethodResult r = reg.compute(full);
        if (!r.ok) return fail(QStringLiteral("datation %1 (%2) : %3").arg(k + 1).arg(label, r.error));
        curveRefs << reg.referenceCurves(d);
        dens.push_back(w.type == Wiggle::None ? r.density : wiggleDensity(r.density, w));
        plain.push_back(d);
        labels << label;
        wiggles << wiggleText(w);
    }
    if (const QStringList cl = curveLabels(curveRefs); !cl.isEmpty()) {
        C.html += QStringLiteral("<p><i>Courbe%1 de référence : %2</i></p>").arg(cl.size() > 1 ? "s" : "", cl.join(" ; ").toHtmlEscaped());
        C.prompt += QStringLiteral("Courbe%1 de référence : %2.\n").arg(cl.size() > 1 ? "s" : "", cl.join(" ; "));
        C.curves = true;
    }

    // 2. Produit sur le domaine commun, puis période d'étude éventuelle.
    QString err;
    Density comb = combineDensities(dens, &err);
    if (!comb.valid()) return fail(err);
    // Domaine commun des datations à courbe ou à loi non bornée : les bornes d'une loi uniforme sont voulues.
    double lo = -1e300, hi = 1e300;
    for (size_t k = 0; k < dens.size(); ++k)
        if (plain[k].value("method").toString() != QLatin1String("unif")) { lo = std::max(lo, dens[k].t.front()); hi = std::min(hi, dens[k].t.back()); }
    QStringList warn;
    {
        const double vmax = *std::max_element(comb.v.begin(), comb.v.end());
        if (comb.t.front() <= lo + 2.0 * comb.step && comb.v.front() > 0.02 * vmax)
            warn << QStringLiteral("la densité combinée atteint la limite ancienne du domaine commun des datations (%1) : résultat tronqué")
                        .arg(dateformat::format(lo, fmt));
        if (comb.t.back() >= hi - 2.0 * comb.step && comb.v.back() > 0.02 * vmax)
            warn << QStringLiteral("la densité combinée atteint la limite récente du domaine commun des datations (%1) : résultat tronqué")
                        .arg(dateformat::format(hi, fmt));
    }
    QString periodNote;
    double kept = 1.0;
    comb = restrictToPeriod(comb, rq, fmt, periodNote, kept);
    if (!comb.valid()) return fail(QStringLiteral("aucune probabilité sur la %1").arg(periodNote));
    if (!periodNote.isEmpty()) {
        C.html += QStringLiteral("<p><i>Résultat restreint à la %1.</i></p>").arg(periodNote.toHtmlEscaped());
        C.prompt += QStringLiteral("Résultat restreint à la %1.\n").arg(periodNote);
        if (kept < 0.99)
            warn << QStringLiteral("la %1 ne contient que %2 % de la probabilité combinée : résultat restreint à cette période")
                        .arg(periodNote, text::numAuto(100.0 * kept, 1));
    }

    // 3. Résultat
    C.html += QStringLiteral("<p><b>Densité combinée</b> (date commune des datations)</p>");
    C.prompt += QStringLiteral("Densité combinée (produit des vraisemblances des datations) :\n");
    appendSummary(C.html, C.prompt, comb, hpdIntervals(comb, 95.0), hpdIntervals(comb, 68.0), densityStats(comb), C.level, fmt);
    if (!levelWarning.isEmpty()) warn.prepend(levelWarning);
    for (const QString& w : warn) {
        C.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());
        C.prompt += QStringLiteral("Avertissement : %1.\n").arg(w);
    }

    // 4. Les datations seules (calibration de t + δ pour une datation à décalage)
    const QString lt = levelText(C.level);
    QString rows;
    C.prompt += QStringLiteral("Datations combinées (densité seule, HPD %1 %) :\n").arg(lt);
    for (int k = 0; k < labels.size(); ++k) {
        const QString iv = intervalsText(hpdIntervals(dens[size_t(k)], C.level), fmt);
        rows += QStringLiteral("<tr><td valign='top'>%1%2</td><td valign='top'>%3</td></tr>")
                    .arg(labels[k].toHtmlEscaped(),
                         wiggles[k].isEmpty() ? QString() : QStringLiteral("<br><i>%1 (densité de t + δ)</i>").arg(wiggles[k].toHtmlEscaped()),
                         QString(iv).replace('\n', "<br>"));
        C.prompt += QStringLiteral("- %1%2 : %3\n").arg(labels[k], wiggles[k].isEmpty() ? QString() : QStringLiteral(" (%1, densité de t + δ)").arg(wiggles[k]),
                                                      QString(iv).replace('\n', " ; "));
    }
    C.html += QStringLiteral("<table cellspacing='0' cellpadding='3'><tr><th align='left'>Datation</th><th align='left'>HPD %1 %</th></tr>%2</table>")
                  .arg(lt, rows);

    // 5. Test de Ward et Wilson : information sur l'accord des mesures, pas un critère de validité.
    QStringList skipped;
    const std::vector<CompatibilityTest> tests = wardWilsonTests(plain, labels, &skipped);
    if (!tests.empty() || !skipped.isEmpty()) {
        QString t;
        C.prompt += QStringLiteral("Test de Ward et Wilson (information sur l'accord des mesures, PAS un critère de validité de la combinaison) :\n");
        for (const CompatibilityTest& x : tests) {
            const QString u = x.unit.isEmpty() ? QString() : QStringLiteral(" ") + x.unit;
            // Décimales selon la grandeur : années à l'unité, F14C à 4, degrés et µT à 2.
            const int dec = x.group.contains(QLatin1String("F14C")) ? 4 : (x.unit == QStringLiteral("°") || x.unit == QStringLiteral("µT")) ? 2 : 0;
            const QString line = QStringLiteral("%1 (%2) : moyenne pondérée %3 ± %4%5 ; T = %6 pour χ²(%7) à 5 % = %8 (p = %9) → %10")
                                     .arg(x.group, x.names.join(", "), text::num(x.mean, dec), text::num(x.meanError, dec), u,
                                          text::numAuto(x.T, 2), QString::number(x.df), text::numAuto(x.threshold, 2),
                                          text::numAuto(x.pValue, 3),
                                          x.compatible() ? QStringLiteral("mesures statistiquement compatibles")
                                                         : QStringLiteral("dispersion supérieure aux erreurs annoncées"));
            t += QStringLiteral("<li>%1</li>").arg(line.toHtmlEscaped());
            C.prompt += QStringLiteral("- %1\n").arg(line);
        }
        if (!skipped.isEmpty()) {
            const QString s = QStringLiteral("datation%1 à décalage (wiggle) hors test : %2").arg(skipped.size() > 1 ? "s" : "", skipped.join(", "));
            t += QStringLiteral("<li><i>%1</i></li>").arg(s.toHtmlEscaped());
            C.prompt += QStringLiteral("- %1\n").arg(s);
        }
        C.html += QStringLiteral("<p><b>Test de Ward et Wilson</b> <i>(à titre d'information, pas un critère de validité)</i></p><ul>%1</ul>").arg(t);
    }

    // 6. Figure : combinaison en haut, puis chaque datation sur sa ligne.
    QJsonObject s0 = plotSeries(QStringLiteral("%1 (combinaison)").arg(name), comb, fmt);
    if (!combColor.isEmpty()) s0["color"] = combColor;
    C.series.append(s0);
    for (int k = 0; k < labels.size(); ++k) {
        QJsonObject s = plotSeries(wiggles[k].isEmpty() ? labels[k] : QStringLiteral("%1 (t + δ)").arg(labels[k]), dens[size_t(k)], fmt);
        if (!colors[k].isEmpty()) s["color"] = colors[k];
        C.series.append(s);
    }
    C.combined = comb;
    C.ok = true;
    return C;
}

// ------------------------------------------------------------------ calibration inverse (mesure attendue pour une date)

struct InverseRun {
    bool ok = false;
    QString html, prompt;
    QJsonObject plot;                // densité de la mesure attendue (axe : valeur de la mesure)
    bool curves = false;
    double level = MethodRunner::kDefaultLevel;
};

InverseRun runInverse(const QJsonObject& rq, DateFormat fmt)
{
    InverseRun V;
    QString levelWarning;
    V.level = readLevel(rq, levelWarning);
    const InverseResult R = inverseCalibration(rq);
    // Date telle que demandée, dans le format d'affichage
    const double t = numOf(rq, { "t" }, std::nan("")), te = numOf(rq, { "t_error" }, std::nan(""));
    const double t0 = numOf(rq, { "t_min" }, std::nan("")), t1 = numOf(rq, { "t_max" }, std::nan(""));
    const QString when = std::isfinite(t0) && std::isfinite(t1)
        ? QStringLiteral("une date uniforme entre %1 et %2").arg(dateformat::format(t0, fmt), dateformat::format(t1, fmt))
        : std::isfinite(te) && te > 0.0 ? QStringLiteral("une date de %1 ± %2").arg(dateformat::format(t, fmt), dateformat::spread(te, fmt))
        : std::isfinite(t) ? QStringLiteral("la date %1").arg(dateformat::format(t, fmt)) : QStringLiteral("une date non précisée");
    const QString head = QStringLiteral("Calibration inverse — %1 attendu%2 pour %3")
                             .arg(R.quantity.isEmpty() ? QStringLiteral("mesure") : R.quantity,
                                  R.quantity == QLatin1String("Inclinaison") || R.quantity == QStringLiteral("Déclinaison")
                                      || R.quantity == QStringLiteral("Intensité") ? QStringLiteral("e") : QString(), when);
    V.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
    V.prompt += QStringLiteral("\n%1\n").arg(head);
    if (!R.ok) {
        V.html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(R.error.toHtmlEscaped());
        V.prompt += QStringLiteral("Calcul impossible : %1.\n").arg(R.error);
        return V;
    }
    if (const QStringList cl = curveLabels({ R.curveRef }); !cl.isEmpty()) {
        V.html += QStringLiteral("<p><i>Courbe de référence : %1</i></p>").arg(cl.join(" ; ").toHtmlEscaped());
        V.prompt += QStringLiteral("Courbe de référence : %1.\n").arg(cl.join(" ; "));
        V.curves = true;
    }
    // Décimales selon la grandeur : âge 14C à l'année, F14C à 4 décimales, degrés et µT à 2.
    const int dec = R.unit == QLatin1String("BP") ? 0 : R.quantity == QLatin1String("F14C") ? 4 : 2;
    const QString u = R.unit.isEmpty() ? QString() : QStringLiteral(" ") + R.unit;
    auto val = [&](double x) { return text::num(x, dec) + u; };
    auto pm = [&](double m, double s) { return QStringLiteral("%1 ± %2%3").arg(text::num(m, dec), text::num(s, dec), u); };
    const double lab = numOf(rq, { "lab_error" }, 0.0);
    QString rows = QStringLiteral("<tr><td><b>Courbe à %1</b></td><td>%2 <i>(1σ de la courbe)</i></td></tr>")
                       .arg(dateformat::format(R.tCentre, fmt), pm(R.curveMean, R.curveError));
    QString what = QStringLiteral("mesure attendue");
    QStringList parts;
    if (std::isfinite(te) && te > 0.0) parts << QStringLiteral("incertitude de la date");
    if (std::isfinite(t0) && std::isfinite(t1)) parts << QStringLiteral("étendue de la période");
    parts << QStringLiteral("erreur de la courbe");
    if (lab > 0.0) parts << QStringLiteral("erreur de mesure %1").arg(val(lab));
    rows += QStringLiteral("<tr><td><b>Mesure attendue</b></td><td>%1 <i>(%2)</i></td></tr>").arg(pm(R.mean, R.sd), parts.join(", ").toHtmlEscaped());
    V.prompt += QStringLiteral("Valeur de la courbe à %1 : %2 (1σ de la courbe). Mesure attendue (%3) : %4.\n")
                    .arg(dateformat::format(R.tCentre, fmt), pm(R.curveMean, R.curveError), parts.join(", "), pm(R.mean, R.sd));
    for (double l : displayLevels(V.level)) {
        QStringList items;
        for (const Interval& iv : hpdIntervals(R.value, l)) {
            if (iv.prob < 0.005) continue;
            items << QStringLiteral("%1 → %2 : %3 %").arg(val(iv.from), val(iv.to), text::num(iv.prob * 100.0, 1));
        }
        const bool asked = std::abs(l - V.level) < 1e-9 && std::abs(V.level - MethodRunner::kDefaultLevel) > 1e-9;
        rows += QStringLiteral("<tr><td valign='top'><b>HPD %1 %</b>%2</td><td>%3</td></tr>")
                    .arg(levelText(l), asked ? QStringLiteral(" <i>(demandé)</i>") : QString(), items.join("<br>"));
        V.prompt += QStringLiteral("Intervalle HPD à %1 % de la %2 : %3\n").arg(levelText(l), what, items.join(" ; "));
    }
    if (R.hasF14c) {
        rows += QStringLiteral("<tr><td><b>Équivalent F14C</b></td><td>%1 ± %2</td></tr>").arg(text::num(R.f14cMean, 4), text::num(R.f14cSd, 4));
        V.prompt += QStringLiteral("Équivalent en F14C : %1 ± %2.\n").arg(text::num(R.f14cMean, 4), text::num(R.f14cSd, 4));
    }
    V.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
    if (!levelWarning.isEmpty()) {
        V.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(levelWarning.toHtmlEscaped());
        V.prompt += QStringLiteral("Avertissement : %1.\n").arg(levelWarning);
    }
    QJsonArray xs, ys;
    for (size_t k = 0; k < R.value.t.size(); k += 2) { xs.append(R.value.t[k]); ys.append(R.value.v[k]); }
    V.plot = QJsonObject{ { "title", QStringLiteral("Calibration inverse — %1 pour %2").arg(R.quantity, when) },
                          { "xlabel", R.unit.isEmpty() ? R.quantity : QStringLiteral("%1 (%2)").arg(R.quantity, R.unit) },
                          { "ylabel", QStringLiteral("Densité de probabilité") }, { "fill", true }, { "yaxis", false },
                          { "hpd", V.level }, { "layout", QStringLiteral("rows") },
                          { "series", QJsonArray{ QJsonObject{ { "name", QStringLiteral("%1 attendu%2").arg(R.quantity, R.unit == QLatin1String("BP") || R.quantity == QLatin1String("F14C") ? QString() : QStringLiteral("e")) },
                                                               { "x", xs }, { "y", ys } } } } };
    V.ok = true;
    return V;
}

// ------------------------------------------------------------------ ordre de deux datations (antéro-postérité)

// Densité d'une des deux datations comparées : datation seule (avec décalage et période d'étude), combinaison ou
// événement (θ a posteriori). `html` / `prompt` : description courte (datation seule) ou fiche complète (événement,
// combinaison) ; `error` non vide si le calcul est impossible.
struct OrderItem {
    Density d;
    QString label, html, prompt, error;
    bool curves = false;
};

// Datation « density » (plugin Density de ChronoModel) : fichier tabulé cherché dans <racine>/Density des dossiers de
// courbes (dossier de l'utilisateur, Calib de ChronoModel), nom comparé en minuscules, « .csv » ajouté s'il manque ;
// un chemin absolu est accepté. Format de PluginDensity::loadRefFile : lignes « // » de description, tmin, tmax, puis
// une valeur par ligne sur une grille régulière ; densité renormalisée, nulle hors de [tmin ; tmax].
bool loadDensityDating(const QJsonObject& d, Density& out, QString& file, QString& error)
{
    return DensityMethod::load(d, out, file, error);
}

OrderItem orderItem(const QJsonObject& in, const QJsonObject& parent, const QString& fallback, DateFormat fmt)
{
    OrderItem I;
    const MethodRegistry& reg = MethodRegistry::instance();
    QJsonObject d = MethodRunner::normalizeRequest(in);
    for (const char* k : { "tmin", "tmax" })                 // période d'étude commune, si la datation n'en a pas
        if (!d.contains(k) && parent.contains(k)) d[k] = parent.value(k);
    d.remove("level");
    const QString m = d.value("method").toString().toLower();
    if (m == QLatin1String("event")) {
        if (d.value("name").toString().trimmed().isEmpty()) d["name"] = fallback;
        const EventRun E = runEvent(d, fmt);
        I.html = E.html; I.prompt = E.prompt; I.curves = E.curves;
        I.label = d.value("name").toString().trimmed();
        if (!E.ok || !E.theta.valid()) I.error = QStringLiteral("l'événement « %1 » n'a pas pu être calculé").arg(I.label);
        else I.d = E.theta;
        return I;
    }
    if (m == QLatin1String("combine")) {
        if (d.value("name").toString().trimmed().isEmpty()) d["name"] = fallback;
        const CombineRun C = runCombine(d, fmt);
        I.html = C.html; I.prompt = C.prompt; I.curves = C.curves;
        I.label = d.value("name").toString().trimmed();
        if (!C.ok || !C.combined.valid()) I.error = QStringLiteral("la combinaison « %1 » n'a pas pu être calculée").arg(I.label);
        else I.d = C.combined;
        return I;
    }
    if (m == QLatin1String("density")) {
        const QString name = d.value("name").toString().trimmed();
        I.label = name.isEmpty() ? fallback : name;
        Density dd;
        QString file, err;
        if (!loadDensityDating(d, dd, file, err)) { I.error = QStringLiteral("%1 : %2").arg(I.label, err); return I; }
        Wiggle w;
        QString werr;
        if (!wiggleFromJson(d.value("wiggle"), w, werr)) { I.error = QStringLiteral("%1 : %2").arg(I.label, werr); return I; }
        QString head = QStringLiteral("%1 — Densité tabulée (plugin Density) : %2").arg(I.label, QFileInfo(file).fileName());
        if (w.type != Wiggle::None) { dd = wiggleDensity(dd, w); head += QStringLiteral(" — avec %1 (densité de t + δ)").arg(wiggleText(w)); }
        QString note;
        double kept = 1.0;
        I.d = restrictToPeriod(dd, d, fmt, note, kept);
        if (!I.d.valid()) { I.error = QStringLiteral("%1 : aucune probabilité sur la %2").arg(I.label, note); return I; }
        if (!note.isEmpty()) head += QStringLiteral(" — %1").arg(note);
        const dateformat::PrecisionScope precision(densityStats(I.d).sd);
        const QString hpd = intervalsText(hpdIntervals(I.d, 95.0), fmt);
        I.html = QStringLiteral("<p><b>%1</b><br><small>HPD 95 % : %2</small></p>")
                     .arg(head.toHtmlEscaped(), hpd.toHtmlEscaped().replace('\n', QStringLiteral(" ; ")));
        I.prompt = QStringLiteral("%1. HPD à 95 % : %2.\n").arg(head, QString(hpd).replace('\n', QStringLiteral(" ; ")));
        return I;
    }
    if (m.isEmpty() || !reg.byId(m)) {
        I.label = fallback;
        I.error = QStringLiteral("« %1 » n'a pas de « method » de datation valide (c14, f14c, tl, gauss, unif, am, density, event ou combine)").arg(fallback);
        return I;
    }
    d["method"] = m;
    if (m == QLatin1String("am")) { d["process"] = d.value("process").toString("I").toUpper(); if (!d.contains("integration_steps")) d["integration_steps"] = 500; }
    if (d.contains("curve")) d["curve"] = d.value("curve").toString().toLower();
    if (m == QLatin1String("c14") && d.contains("delta_r")) d["has_delta_r"] = true;
    const QString desc = reg.describe(d);
    const QString name = d.value("name").toString().trimmed();
    I.label = name.isEmpty() ? fallback : name;
    MethodResult r = reg.compute(d);
    if (!r.ok) { I.error = QStringLiteral("%1 : %2").arg(I.label, r.error); return I; }
    Wiggle w;
    QString werr;
    if (!wiggleFromJson(d.value("wiggle"), w, werr)) { I.error = QStringLiteral("%1 : %2").arg(I.label, werr); return I; }
    QString head = QStringLiteral("%1 — %2").arg(I.label, desc);
    if (w.type != Wiggle::None) { r.density = wiggleDensity(r.density, w); head += QStringLiteral(" — avec %1 (densité de t + δ)").arg(wiggleText(w)); }
    QString note;
    double kept = 1.0;
    I.d = restrictToPeriod(r.density, d, fmt, note, kept);
    if (!I.d.valid()) { I.error = QStringLiteral("%1 : aucune probabilité sur la %2").arg(I.label, note); return I; }
    if (!note.isEmpty()) head += QStringLiteral(" — %1").arg(note);
    if (const QStringList cl = curveLabels(reg.referenceCurves(d)); !cl.isEmpty()) {
        head += QStringLiteral(" (courbe : %1)").arg(cl.join(QStringLiteral(" ; ")));
        I.curves = true;
    }
    const dateformat::PrecisionScope precision(densityStats(I.d).sd);
    const QString hpd = intervalsText(hpdIntervals(I.d, 95.0), fmt);
    I.html = QStringLiteral("<p><b>%1</b><br><small>HPD 95 % : %2</small></p>")
                 .arg(head.toHtmlEscaped(), hpd.toHtmlEscaped().replace('\n', QStringLiteral(" ; ")));
    I.prompt = QStringLiteral("%1. HPD à 95 % : %2.\n").arg(head, QString(hpd).replace('\n', QStringLiteral(" ; ")));
    for (const QString& wr : r.warnings) {
        I.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(wr.toHtmlEscaped());
        I.prompt += QStringLiteral("Avertissement : %1.\n").arg(wr);
    }
    return I;
}

// Valeur d'une densité en x (interpolation linéaire, 0 hors du domaine).
double densityAt(const Density& d, double x)
{
    if (!d.valid() || x < d.t.front() || x > d.t.back()) return 0.0;
    const double h = d.t[1] - d.t[0];
    const double u = (x - d.t.front()) / h;
    const size_t i = std::min(d.t.size() - 2, size_t(std::floor(u)));
    const double f = u - double(i);
    return d.v[i] * (1.0 - f) + d.v[i + 1] * f;
}

} // namespace

// D = B − A pour deux dates indépendantes : f_D(x) = ∫ f_A(a) f_B(a + x) da (convolution discrète sur une grille
// commune au plus petit des deux pas, élargi si le calcul dépasse ~4·10⁷ produits).
Density differenceDensity(const Density& A, const Density& B)
{
    Density D;
    if (!A.valid() || !B.valid()) return D;
    double h = std::min(A.t[1] - A.t[0], B.t[1] - B.t[0]);
    if (!(h > 0.0)) h = 1.0;
    auto count = [&](const Density& X) { return std::floor((X.t.back() - X.t.front()) / h) + 1.0; };
    while (count(A) * count(B) > 4e7) h *= 2.0;
    auto grid = [&](const Density& X) {
        std::vector<double> g(size_t(count(X)));
        for (size_t i = 0; i < g.size(); ++i) g[i] = densityAt(X, X.t.front() + double(i) * h);
        return g;
    };
    const std::vector<double> ga = grid(A), gb = grid(B);
    const size_t na = ga.size(), nb = gb.size();
    std::vector<double> f(na + nb - 1, 0.0);
    for (size_t i = 0; i < na; ++i) {
        if (ga[i] == 0.0) continue;
        for (size_t j = 0; j < nb; ++j) f[j + na - 1 - i] += ga[i] * gb[j];
    }
    const double x0 = (B.t.front() - A.t.front()) - double(na - 1) * h;
    // Queues nulles retirées (marge de 2 pas)
    const double mx = *std::max_element(f.begin(), f.end());
    if (!(mx > 0.0)) return D;
    size_t k0 = 0, k1 = f.size() - 1;
    while (k0 < k1 && f[k0] < mx * 1e-12) ++k0;
    while (k1 > k0 && f[k1] < mx * 1e-12) --k1;
    k0 = k0 >= 2 ? k0 - 2 : 0;
    k1 = std::min(f.size() - 1, k1 + 2);
    for (size_t k = k0; k <= k1; ++k) { D.t.push_back(x0 + double(k) * h); D.v.push_back(f[k]); }
    D.step = h;
    if (D.t.size() < 2) { D.t.push_back(D.t.back() + h); D.v.push_back(0.0); }
    const double area = densityArea(D);
    if (area > 0.0) for (double& y : D.v) y /= area;
    return D;
}

double probabilityBefore(const Density& A, const Density& B)
{
    const Density D = differenceDensity(A, B);
    if (!D.valid()) return std::nan("");
    return std::clamp(1.0 - densityCdf(D, 0.0), 0.0, 1.0);
}

namespace {

// ------------------------------------------------------------------ relation entre datations et traces MCMC
//
// Un seul calcul (« relation ») pour toutes les questions sur deux ou trois termes : densité d'un terme (« value »),
// durée B − A (« duration »), date comprise entre A et B (« between »), antéro-postérité (« order »), appartenance de
// C à [A ; B] (« inside »). Chaque terme est :
//   - une colonne de traces MCMC exportées par ChronoModel (tirages liés aux autres colonnes du même export) ;
//   - une datation indépendante (c14, f14c, tl, gauss, unif, am, density, combinaison, événement calculé par SeldonH) ;
//   - une année fixe (borne d'intervalle, origine d'une durée).
// Calcul exact sur les densités quand il n'y a que des datations indépendantes et des années (convolution pour
// l'écart, fonctions de répartition pour l'appartenance) ; sinon tirage par tirage : les colonnes donnent leurs
// tirages, chaque datation indépendante est tirée par inverse de sa fonction de répartition (tirage stratifié).

// Intervalles d'une densité de durée, en années (« 120 → 340 ans : 95,0 % »).
QString durationIntervalsText(const std::vector<Interval>& v, DateFormat fmt)
{
    QStringList items;
    for (const Interval& iv : v) {
        if (iv.prob < 0.005) continue;
        items << QStringLiteral("%1 → %2 : %3 %").arg(dateformat::spread(iv.from, fmt), dateformat::spread(iv.to, fmt),
                                                       text::num(iv.prob * 100.0, 1));
    }
    return items.join("\n");
}

// Probabilité estimée sur les tirages, avec son erreur Monte-Carlo sqrt(p(1 − p) / ESS) (ESS de l'indicatrice, toutes
// chaînes) : dit si le nombre de tirages suffit pour la précision annoncée.
QString probabilityText(double p, double ess)
{
    QString t = text::num(100.0 * p, 1) + QStringLiteral(" %");
    if (ess > 0.0 && p > 0.0 && p < 1.0) t += QStringLiteral(" (± %1 %)").arg(text::num(100.0 * std::sqrt(p * (1.0 - p) / ess), 1));
    else if (ess > 0.0) t += QStringLiteral(" (aucun tirage contraire : < %1 %)").arg(text::numAuto(100.0 / ess, 2));
    return t;
}

// ESS d'une indicatrice (0/1) par chaîne ; nombre de tirages si une seule chaîne ou chaînes trop courtes.
double indicatorEss(const std::vector<std::vector<double>>& byChain, size_t kept)
{
    size_t len = std::numeric_limits<size_t>::max();
    for (const std::vector<double>& c : byChain) len = std::min(len, c.size());
    if (byChain.size() < 2 || len < 4) return double(kept);
    std::vector<std::vector<double>> eq;
    for (const std::vector<double>& c : byChain) eq.emplace_back(c.begin(), c.begin() + std::ptrdiff_t(len));
    const mcmcdiag::RhatEss re = mcmcdiag::computeRhatAndEss(eq);
    return std::isfinite(re.bulkEss) && re.bulkEss > 0.0 ? re.bulkEss : double(kept);
}

// « 97,3 % », « > 99,9 % », « < 0,1 % »
QString percentText(double p)
{
    if (p > 0.9995) return QStringLiteral("> 99,9 %");
    if (p < 0.0005) return QStringLiteral("< 0,1 %");
    return text::num(100.0 * p, 1) + QStringLiteral(" %");
}

// Fonction de répartition tabulée d'une densité (trapèzes) : F(x) et F⁻¹(u) par interpolation linéaire.
struct CdfTable {
    std::vector<double> t, F;
    explicit CdfTable(const Density& d)
    {
        if (!d.valid()) return;
        t = d.t;
        F.assign(t.size(), 0.0);
        for (size_t i = 1; i < t.size(); ++i) F[i] = F[i - 1] + 0.5 * (d.v[i - 1] + d.v[i]) * (t[i] - t[i - 1]);
        const double total = F.back();
        if (total > 0.0) for (double& f : F) f /= total;
    }
    double at(double x) const
    {
        if (t.empty() || x <= t.front()) return 0.0;
        if (x >= t.back()) return 1.0;
        const size_t i = size_t(std::upper_bound(t.begin(), t.end(), x) - t.begin());      // t[i-1] <= x < t[i]
        return F[i - 1] + (x - t[i - 1]) / (t[i] - t[i - 1]) * (F[i] - F[i - 1]);
    }
    double inverse(double u) const
    {
        const size_t i = size_t(std::lower_bound(F.begin(), F.end(), u) - F.begin());
        if (i == 0) return t.front();
        if (i >= F.size()) return t.back();
        const double dF = F[i] - F[i - 1];
        return dF > 0.0 ? t[i - 1] + (u - F[i - 1]) / dF * (t[i] - t[i - 1]) : t[i];
    }
};

// n tirages d'une densité : inverse de la fonction de répartition sur des quantiles stratifiés ((k + U) / n), dans un
// ordre aléatoire (les tirages ne sont liés ni entre eux ni aux autres termes).
std::vector<double> sampleDensity(const Density& d, size_t n, std::mt19937& rng)
{
    const CdfTable C(d);
    std::vector<size_t> perm(n);
    std::iota(perm.begin(), perm.end(), size_t(0));
    std::shuffle(perm.begin(), perm.end(), rng);
    std::uniform_real_distribution<double> unif(0.0, 1.0);
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = C.inverse((double(perm[i]) + unif(rng)) / double(n));
    return x;
}

// Densité de s·X + c (s = +1 ou −1) : écart entre une datation et une année fixe.
Density affineDensity(const Density& d, double s, double c)
{
    Density r;
    r.step = d.step;
    const size_t n = d.t.size();
    r.t.resize(n);
    r.v.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t j = s > 0.0 ? i : n - 1 - i;
        r.t[i] = c + s * d.t[j];
        r.v[i] = d.v[j];
    }
    return r;
}

// Graphique à part d'une durée ou d'un écart (axe en années, pas en dates).
QJsonObject spreadPlot(const QString& title, const QString& word, const QString& series, const Density& D, double level, DateFormat fmt)
{
    QJsonArray xs, ys;
    const size_t step = std::max<size_t>(1, D.t.size() / 400);
    for (size_t k = 0; k < D.t.size(); k += step) { xs.append(D.t[k] / dateformat::unitYears(fmt)); ys.append(D.v[k]); }
    const QString unit = dateformat::unitLabel(fmt).isEmpty() ? QStringLiteral("années") : dateformat::unitLabel(fmt);
    return QJsonObject{ { "title", title }, { "xlabel", QStringLiteral("%1 (%2)").arg(word, unit) },
                        { "ylabel", QStringLiteral("Densité de probabilité") }, { "fill", true }, { "yaxis", false },
                        { "hpd", level }, { "layout", QStringLiteral("rows") },
                        { "series", QJsonArray{ QJsonObject{ { "name", series }, { "x", xs }, { "y", ys } } } } };
}

// Un terme de la relation.
struct Operand {
    enum Kind { Column, Independent, Fixed } kind = Fixed;
    QString label, html, prompt, color;
    const TraceSet* set = nullptr;
    const std::vector<double>* col = nullptr;
    Density d;                       // datation indépendante
    double year = std::nan("");      // année fixe
    bool curves = false;
    bool isEvent = false;            // événement calculé par SeldonH (pas un Event du modèle ChronoModel)
};

// Valeur d'un terme dans la demande : objet (datation), texte (colonne, année, ou objet JSON écrit en texte), nombre.
QJsonValue operandValue(const QJsonObject& rq, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        const QJsonValue v = rq.value(QLatin1String(k));
        if (v.isObject()) return v;
        if (v.isDouble()) return QString::number(v.toDouble(), 'g', 12);
        if (!v.isString()) continue;
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) continue;
        if (s.startsWith('{')) {
            const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
            if (doc.isObject()) return doc.object();
        }
        return s;
    }
    return QJsonValue(QJsonValue::Undefined);
}

// Datation écrite en texte par un modèle de langage (« 2450 ± 30 BP ») : lue comme une datation, pas comme une année.
QJsonObject datingFromText(const QString& s)
{
    static const QRegularExpression marks(QStringLiteral(R"(±|\+/-|\+-|\bBP\b|\bF14C\b|\bTL\b|\bOSL\b)"), QRegularExpression::CaseInsensitiveOption);
    if (!marks.match(s).hasMatch()) return {};
    const QVector<QJsonObject> rq = MethodRegistry::instance().parse(text::erasToAstronomical(s));
    return rq.size() == 1 ? rq.first() : QJsonObject();
}

bool resolveOperand(const QJsonValue& v, const QJsonObject& rq, const QString& fallback, int preferred, DateFormat fmt,
                    Operand& o, QString& error)
{
    QJsonObject obj = v.isObject() ? v.toObject() : datingFromText(v.toString());
    if (!obj.isEmpty()) {
        const QString m = MethodRunner::normalizeRequest(obj).value("method").toString();
        if (m == QLatin1String("relation") || m == QLatin1String("inverse") || m == QLatin1String("density_file") || m == QLatin1String("table")) {
            o.label = fallback;
            error = QStringLiteral("« %1 » doit être une datation (c14, f14c, tl, gauss, unif, am, density, event ou combine), une colonne "
                                   "des traces ou une année, pas un calcul « %2 »").arg(fallback, m);
            return false;
        }
        if (!obj.contains("name") && v.isString()) obj["name"] = v.toString().trimmed();
        const OrderItem I = orderItem(obj, rq, fallback, fmt);
        o.kind = Operand::Independent;
        o.label = I.label;
        o.html = I.html;
        o.prompt = I.prompt;
        o.curves = I.curves;
        o.color = MethodRunner::colorOf(obj);
        o.isEvent = m == QLatin1String("event");
        if (!I.error.isEmpty()) { error = I.error; return false; }
        o.d = I.d;
        return true;
    }
    const QString name = v.toString().trimmed();
    int s = -1, c = -1;
    QString err;
    if (traces::resolve(name, s, c, &err, preferred)) {
        const TraceSet& ts = traces::loaded()[s];
        o.kind = Operand::Column;
        o.set = &ts;
        o.col = &ts.values[size_t(c)];
        o.label = ts.columns[c];
        return true;
    }
    const double y = text::yearFromText(name);
    if (std::isfinite(y)) {
        o.kind = Operand::Fixed;
        o.year = y;
        o.label = dateformat::format(y, fmt);
        return true;
    }
    o.label = name;
    error = err;
    return false;
}

struct RelationRun {
    bool ok = false;
    bool curves = false;
    bool posterior = false;          // au moins une colonne de traces : résultat a posteriori d'un modèle ChronoModel
    QString html, prompt, name;
    QString title;                   // titre de la fiche : opération et termes tels que SeldonH les a compris
    QJsonArray series;               // densités de dates (graphique des datations)
    QJsonObject extraPlot;           // durée ou écart : graphique à part (axe en années)
    QVector<QJsonObject> scatterPlots; // corrélation : nuage de points de chaque paire (graphiques à part)
    double level = MethodRunner::kDefaultLevel;
};

// Nom d'une datation sans nom d'après sa mesure (« 1700 ± 50 ans avant 2025 », « 1650 ± 30 BP ») ; vide pour un
// événement, une combinaison ou une méthode inconnue.
QString measureName(const QJsonObject& o)
{
    const MethodRegistry& reg = MethodRegistry::instance();
    const QJsonObject n = MethodRunner::normalizeRequest(o);
    if (!reg.byId(n.value("method").toString())) return {};
    QString d = reg.describe(n);
    if (const qsizetype k = d.indexOf(QStringLiteral(" — ")); k > 0) d = d.left(k);
    if (const qsizetype k = d.indexOf(QStringLiteral(" : ")); k > 0 && k < 30) d = d.mid(k + 3);
    if (const qsizetype k = d.lastIndexOf(QStringLiteral(" (")); k > 0 && d.endsWith(')')) d = d.left(k);   // « (années calendaires signées) »
    return d.trimmed();
}

// Mesure d'une datation préfixée par sa méthode (« 14C 1650 ± 30 BP », « TL 1700 ± 50 ans avant 2025 ») : titres de fiche.
QString prefixedMeasureName(const QJsonObject& o)
{
    const QString mn = measureName(o);
    if (mn.isEmpty()) return {};
    const QString m = MethodRunner::normalizeRequest(o).value("method").toString();
    static const QHash<QString, QString> prefix = { { "c14", "14C" }, { "f14c", "F14C" }, { "tl", "TL" }, { "am", "AM" },
                                                    { "gauss", "Gauss" }, { "unif", "Unif" } };
    return prefix.contains(m) && !mn.startsWith(prefix.value(m)) ? prefix.value(m) + ' ' + mn : mn;
}

// Colonne de traces pour un titre : « Phase A Begin » → début de « Phase A », « Phase A End » → fin de « Phase A ».
QString prettyColumn(const QString& c)
{
    if (c.endsWith(QLatin1String(" Begin"))) return QStringLiteral("début de « %1 »").arg(c.chopped(6));
    if (c.endsWith(QLatin1String(" End"))) return QStringLiteral("fin de « %1 »").arg(c.chopped(4));
    return QStringLiteral("« %1 »").arg(c);
}

RelationRun runRelation(const QJsonObject& rq, DateFormat fmt)
{
    RelationRun R;
    QString levelWarning;
    R.level = readLevel(rq, levelWarning);
    QString done;                    // fiches des datations, déjà écrites
    auto fail = [&](const QString& why) {
        R.html += QStringLiteral("<p><b>Relation entre datations ou traces MCMC</b></p>%1<p><i>Calcul impossible : %2.</i></p>")
                      .arg(done, why.toHtmlEscaped());
        R.prompt += QStringLiteral("\nRelation entre datations ou traces MCMC — calcul impossible : %1.\n").arg(why);
        R.ok = false;
        return R;
    };

    // 1. Opération
    QString op = text::fold(rq.value("op").toString()).trimmed();
    static const QHash<QString, QString> ops = {
        { "duration", "duration" }, { "duree", "duration" }, { "ecart", "duration" }, { "difference", "duration" }, { "interval", "duration" },
        { "gap", "duration" }, { "span", "duration" },
        { "between", "between" }, { "entre", "between" }, { "date entre", "between" }, { "date_between", "between" },
        { "value", "value" }, { "valeur", "value" }, { "date", "value" }, { "densite", "value" }, { "density", "value" },
        { "column", "value" }, { "colonne", "value" },
        { "order", "order" }, { "ordre", "order" }, { "anteriorite", "order" }, { "posteriorite", "order" }, { "antero-posteriorite", "order" },
        { "before", "order" }, { "avant", "order" }, { "precedence", "order" }, { "compare", "order" }, { "comparer", "order" },
        { "inside", "inside" }, { "within", "inside" }, { "appartenance", "inside" }, { "dans", "inside" }, { "membership", "inside" },
        { "correlation", "correlation" }, { "correlations", "correlation" }, { "correl", "correlation" }, { "matrice", "correlation" },
        { "matrix", "correlation" }, { "covariance", "correlation" }, { "scatter", "correlation" }, { "nuage", "correlation" },
        { "dependance", "correlation" }, { "dependence", "correlation" } };
    QJsonValue vFrom = operandValue(rq, { "from", "a", "start" }), vTo = operandValue(rq, { "to", "b", "end" });
    QJsonValue vOf = operandValue(rq, { "of", "column", "variable", "c" });
    if (ops.contains(op)) op = ops.value(op);
    else if (op.isEmpty()) op = !vOf.isUndefined() ? QStringLiteral("value") : QStringLiteral("order");
    else return fail(QStringLiteral("opération « %1 » inconnue (duration, between, value, order, inside ou correlation)").arg(rq.value("op").toString()));
    if (vFrom.isUndefined() && vTo.isUndefined() && rq.value("dates").isArray()) {     // « dates » : [A, B] (ou [C, A, B] pour inside)
        const QJsonArray ds = rq.value("dates").toArray();
        const int k = op == QLatin1String("inside") && ds.size() == 3 && vOf.isUndefined() ? 1 : 0;
        if (k == 1) vOf = ds.at(0);
        if (ds.size() >= 2 + k) { vFrom = ds.at(k); vTo = ds.at(k + 1); }
    }
    if (op == QLatin1String("value") && vOf.isUndefined()) vOf = vFrom;

    // 2. Termes
    struct Wanted { QJsonValue v; QString fallback; };
    QVector<Wanted> wanted;
    if (op == QLatin1String("value")) {
        if (vOf.isUndefined()) return fail(QStringLiteral("terme à analyser absent (« of » : colonne des traces ou datation)"));
        wanted = { { vOf, QStringLiteral("A") } };
    } else if (op == QLatin1String("correlation")) {
        // Liste « terms » (ou « dates », « variables »), sinon from, to et of : 2 à 8 termes.
        QJsonArray list;
        for (const char* k : { "terms", "variables", "dates" })
            if (rq.value(QLatin1String(k)).isArray()) { list = rq.value(QLatin1String(k)).toArray(); break; }
        QVector<QJsonValue> vs;
        if (!list.isEmpty()) {
            for (const QJsonValue& x : list) {
                if (x.isString() && x.toString().trimmed().startsWith('{')) {
                    const QJsonDocument doc = QJsonDocument::fromJson(x.toString().toUtf8());
                    vs << (doc.isObject() ? QJsonValue(doc.object()) : x);
                } else vs << (x.isDouble() ? QJsonValue(QString::number(x.toDouble(), 'g', 12)) : x);
            }
        } else
            for (const QJsonValue& x : { vFrom, vTo, vOf }) if (!x.isUndefined()) vs << x;
        if (vs.size() < 2) return fail(QStringLiteral("il faut au moins deux termes pour une corrélation (« terms » : [A, B, …], ou « from » et « to »)"));
        if (vs.size() > 8) return fail(QStringLiteral("au plus 8 termes pour une matrice de corrélation (%1 demandés)").arg(vs.size()));
        for (int k = 0; k < vs.size(); ++k) wanted.append({ vs[k], QString(QChar(u'A' + k)) });
    } else if (op == QLatin1String("inside")) {
        if (vOf.isUndefined() || vFrom.isUndefined() || vTo.isUndefined())
            return fail(QStringLiteral("il faut le terme testé (« of ») et les deux bornes de l'intervalle (« from », « to »)"));
        wanted = { { vOf, QStringLiteral("C") }, { vFrom, QStringLiteral("A") }, { vTo, QStringLiteral("B") } };
    } else {
        if (vFrom.isUndefined() || vTo.isUndefined())
            return fail(QStringLiteral("il faut deux termes : « from » (A) et « to » (B), colonnes des traces ou datations"));
        wanted = { { vFrom, QStringLiteral("A") }, { vTo, QStringLiteral("B") } };
    }
    QStringList columnNames;                 // un même fichier qui contient toutes les colonnes est préféré
    for (const Wanted& w : wanted)
        if (w.v.isString()) { int s = -1, c = -1; if (traces::resolve(w.v.toString(), s, c)) columnNames << w.v.toString(); }
    const int preferred = traces::setWithAll(columnNames);
    // Avec des colonnes de traces (« Phase A End »), une datation sans nom n'est pas appelée « A », « B » ou « C » : nom
    // d'après sa mesure, préfixé par la méthode (« TL 1700 ± 50 ans avant 2025 »).
    if (!columnNames.isEmpty())
        for (Wanted& w : wanted) {
            if (!w.v.isObject() || !w.v.toObject().value("name").toString().trimmed().isEmpty()) continue;
            QJsonObject o = w.v.toObject();
            const QString mn = measureName(o);
            if (mn.isEmpty()) continue;
            const QString m = MethodRunner::normalizeRequest(o).value("method").toString();
            static const QHash<QString, QString> prefix = { { "c14", "14C" }, { "f14c", "F14C" }, { "tl", "TL" }, { "am", "AM" },
                                                            { "gauss", "Gauss" }, { "unif", "Unif" } };
            o["name"] = prefix.contains(m) && !mn.startsWith(prefix.value(m)) ? prefix.value(m) + ' ' + mn : mn;
            w.v = o;
        }
    std::vector<Operand> X(size_t(wanted.size()));
    for (int k = 0; k < wanted.size(); ++k) {
        QString err;
        const bool ok = resolveOperand(wanted[k].v, rq, wanted[k].fallback, preferred, fmt, X[size_t(k)], err);
        done += X[size_t(k)].html;
        R.prompt += X[size_t(k)].prompt;
        if (!ok) return fail(err);
    }
    for (size_t k = 1; k < X.size(); ++k)                        // noms en double (deux datations sans nom)
        for (size_t j = 0; j < k; ++j)
            if (X[k].label == X[j].label) X[k].label += QStringLiteral(" (%1)").arg(k + 1);
    const TraceSet* set = nullptr;
    bool hasIndep = false;
    QStringList indepNames;
    bool anyEvent = false;
    for (const Operand& o : X) {
        if (o.kind == Operand::Independent) { hasIndep = true; indepNames << o.label; anyEvent = anyEvent || o.isEvent; }
        R.curves = R.curves || o.curves;
        if (o.kind != Operand::Column) continue;
        if (set && (o.set->rows() != set->rows() || o.set->chainStarts != set->chainStarts))
            return fail(QStringLiteral("« %1 » et « %2 » viennent d'exports différents (nombre de tirages ou chaînes différents) : "
                                       "exporter les deux depuis le même calcul").arg(X[0].label, o.label));
        if (!set) set = o.set;
    }
    const bool hasColumn = set != nullptr;
    {
        // Titre de la fiche : l'opération réellement calculée et les termes tels que SeldonH les a compris (colonne des
        // traces, datation indépendante, année), pour contrôler le choix de l'outil fait par le modèle de langage.
        QStringList T;
        for (size_t k = 0; k < X.size(); ++k) {
            const Operand& o = X[k];
            const QJsonValue& v = wanted[int(k)].v;
            QString t;
            if (o.kind == Operand::Column) t = prettyColumn(o.label);
            else if (o.kind == Operand::Independent && o.isEvent) t = QStringLiteral("Event « %1 » (calculé par SeldonH)").arg(o.label);
            else if (o.kind == Operand::Independent && v.isObject() && v.toObject().value("name").toString().trimmed().isEmpty())
                t = prefixedMeasureName(v.toObject());
            else if (o.kind == Operand::Fixed) t = QStringLiteral("l'année %1").arg(o.label);
            if (t.isEmpty()) t = QStringLiteral("« %1 »").arg(o.label);
            T << t;
        }
        auto list = [](const QStringList& l) {
            return l.size() < 2 ? l.join(QString()) : l.mid(0, l.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" et ") + l.last();
        };
        R.title = op == "value" ? QStringLiteral("Densité de %1").arg(T.value(0))
                : op == "duration" ? QStringLiteral("Durée entre %1 et %2").arg(T.value(0), T.value(1))
                : op == "between" ? QStringLiteral("Date entre %1 et %2").arg(T.value(0), T.value(1))
                : op == "order" ? QStringLiteral("Ordre de %1 et %2").arg(T.value(0), T.value(1))
                : op == "correlation" ? QStringLiteral("Corrélation entre %1").arg(list(T))
                : QStringLiteral("Appartenance de %1 à [%2 ; %3]").arg(T.value(0), T.value(1), T.value(2));
        if (hasColumn) R.title += QStringLiteral(" (traces MCMC)");
    }
    R.posterior = hasColumn;
    if (op == QLatin1String("correlation"))
        for (const Operand& o : X)
            if (o.kind == Operand::Fixed)
                return fail(QStringLiteral("« %1 » est une année fixe : elle ne varie pas, sa corrélation n'est pas définie").arg(o.label));
    if ((op == QLatin1String("value") || op == QLatin1String("inside")) && X[0].kind == Operand::Fixed)
        return fail(QStringLiteral("« %1 » est une année fixe : il faut une colonne des traces chargées ou une datation").arg(X[0].label));
    if (op != QLatin1String("value") && op != QLatin1String("inside") && op != QLatin1String("correlation")
        && X[0].kind == Operand::Fixed && X[1].kind == Operand::Fixed)
        return fail(QStringLiteral("« %1 » et « %2 » sont deux années fixes : il faut au moins une colonne des traces ou une datation")
                        .arg(X[0].label, X[1].label));
    const bool exact = !hasColumn && op != QLatin1String("between") && op != QLatin1String("correlation");
    const QString& A = X[op == QLatin1String("inside") ? 1 : 0].label;
    const QString B = op == QLatin1String("value") ? QString() : X[op == QLatin1String("inside") ? 2 : 1].label;
    QStringList allLabels;
    for (const Operand& o : X) allLabels << o.label;
    const QString corrList = QStringLiteral("« %1 »").arg(allLabels.join(QStringLiteral(" », « ")));
    const QString Cn = op == QLatin1String("inside") ? X[0].label : QString();
    const QString iv = QStringLiteral("[%1 ; %2]").arg(A, B);

    // 3. En-tête
    const QString opText = op == "value" ? QStringLiteral("densité de « %1 »").arg(A)
                         : op == "duration" ? QStringLiteral("durée de « %1 » à « %2 » (%2 − %1)").arg(A, B)
                         : op == "between" ? QStringLiteral("date comprise entre « %1 » et « %2 » (uniforme entre les deux, tirage par tirage)").arg(A, B)
                         : op == "order" ? QStringLiteral("antéro-postérité de « %1 » et « %2 »").arg(A, B)
                         : op == "correlation" ? QStringLiteral("corrélation entre %1 (tirage par tirage)").arg(corrList)
                         : QStringLiteral("appartenance de « %1 » à l'intervalle %2").arg(Cn, iv);
    const QString userName = rq.value("name").toString().trimmed();
    R.name = !userName.isEmpty() ? userName
           : op == "value" ? A : op == "duration" ? QStringLiteral("%1 → %2").arg(A, B) : op == "between" ? QStringLiteral("entre %1 et %2").arg(A, B)
           : op == "order" ? QStringLiteral("%1 / %2").arg(A, B)
           : op == "correlation" ? QStringLiteral("Corrélation %1").arg(allLabels.join(QStringLiteral(" / ")))
           : QStringLiteral("%1 dans %2").arg(Cn, iv);
    QString head;
    QStringList files;                       // exports du même calcul (Chain_all_Events.csv et Chain_all_Phases.csv…)
    for (const Operand& o : X)
        if (o.kind == Operand::Column && !files.contains(o.set->file)) files << o.set->file;
    if (hasColumn) head = QStringLiteral("Traces MCMC « %1 » — %2%3").arg(files.join(QStringLiteral(" » et « ")), opText,
                                hasIndep ? QStringLiteral(" — avec datation indépendante du modèle") : QString());
    else if (op == QLatin1String("order")) head = QStringLiteral("Ordre de « %1 » et « %2 » (datations indépendantes)").arg(A, B);
    else if (op == QLatin1String("correlation")) head = QStringLiteral("Corrélation entre datations indépendantes — %1").arg(opText);
    else head = QStringLiteral("Relation entre datations indépendantes — %1").arg(opText);
    R.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped()) + done;
    R.prompt = QStringLiteral("\n%1\n").arg(head) + R.prompt;
    if (hasColumn && hasIndep) {
        const QString w = QStringLiteral("%1 supposée%2 indépendante%2 du modèle ChronoModel des traces : si elle%3 en fait déjà partie, elle%3 "
                                         "est comptée deux fois ; prendre plutôt sa trace a posteriori (export des chaînes des dates) ou "
                                         "relancer le modèle avec elle%3.%4")
                              .arg(indepNames.size() > 1 ? QStringLiteral("Datations « %1 »").arg(indepNames.join(QStringLiteral(" », « ")))
                                                         : QStringLiteral("Datation « %1 »").arg(indepNames.first()),
                                   indepNames.size() > 1 ? QStringLiteral("s") : QString(),
                                   indepNames.size() > 1 ? QStringLiteral("s") : QString(),
                                   anyEvent ? QStringLiteral(" Un événement calculé par SeldonH n'a ni les contraintes ni les phases du modèle.") : QString());
        R.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());
        R.prompt += QStringLiteral("Avertissement : %1\n").arg(w);
    }
    const QLocale fr(QLocale::French);
    const double infv = std::numeric_limits<double>::infinity();
    // Densité d'un terme pour la figure (colonne : lissage des tirages ; datation : sa densité).
    auto operandSeries = [&](const Operand& o) {
        Density dc;
        if (o.kind == Operand::Independent) dc = o.d;
        else if (o.kind == Operand::Column) {
            std::vector<double> v;
            for (double y : *o.col) if (std::isfinite(y)) v.push_back(y);
            dc = kernelDensity(v, -infv, infv, Bandwidth::SheatherJones);
        }
        if (!dc.valid()) return;
        QJsonObject s = plotSeries(o.label, dc, fmt);
        if (!o.color.isEmpty()) s["color"] = o.color;
        R.series.append(s);
    };
    auto finish = [&]() {
        if (!levelWarning.isEmpty()) {
            R.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(levelWarning.toHtmlEscaped());
            R.prompt += QStringLiteral("Avertissement : %1.\n").arg(levelWarning);
        }
        R.ok = true;
        return R;
    };
    static const QString independence = QStringLiteral(
        "Méthode : datations supposées indépendantes (aucune contrainte stratigraphique ni phase entre elles). Si une stratigraphie, une "
        "phase ou d'autres dates les relient, il faut un modèle ChronoModel et le calcul sur ses traces exportées.\n");

    // Tableau d'une durée (densité D, en années) : HPD, MAP, moyenne, quartiles, P(B postérieur à A).
    auto durationTable = [&](const Density& d, double pPos) {
        const DensityStats st = densityStats(d);
        const dateformat::PrecisionScope precision(st.sd);
        QString rows, promptHpd;
        for (double l : displayLevels(R.level)) {
            const QString t = durationIntervalsText(hpdIntervals(d, l), fmt), lt = levelText(l);
            const bool asked = std::abs(l - R.level) < 1e-9 && std::abs(R.level - MethodRunner::kDefaultLevel) > 1e-9;
            rows += QStringLiteral("<tr><td valign='top'><b>HPD %1 %</b>%2</td><td>%3</td></tr>")
                        .arg(lt, asked ? QStringLiteral(" <i>(demandé)</i>") : QString(), QString(t).replace('\n', "<br>"));
            promptHpd += QStringLiteral("Intervalles HPD de la durée à %1 %%2 :\n- %3\n")
                             .arg(lt, asked ? QStringLiteral(" (niveau demandé par l'utilisateur)") : QString(), QString(t).replace("\n", "\n- "));
        }
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1"
                                 "<tr><td><b>MAP (mode)</b></td><td>%2</td></tr>"
                                 "<tr><td><b>Moyenne ; écart-type</b></td><td>%3 ; %4</td></tr>"
                                 "<tr><td><b>Q1 ; Q2 (médiane) ; Q3</b></td><td>%5 ; %6 ; %7</td></tr>"
                                 "<tr><td><b>P(« %8 » postérieur à « %9 »)</b></td><td>%10 %</td></tr></table>")
                      .arg(rows, dateformat::spread(st.mode, fmt), dateformat::spread(st.mean, fmt), dateformat::spread(st.sd, fmt),
                           dateformat::spread(st.q1, fmt), dateformat::spread(st.median, fmt), dateformat::spread(st.q3, fmt),
                           B.toHtmlEscaped(), A.toHtmlEscaped(), text::num(100.0 * pPos, 1));
        R.prompt += promptHpd;
        R.prompt += QStringLiteral("Durée : MAP %1, moyenne %2, écart-type %3, Q1 %4, médiane %5, Q3 %6. Une durée négative signifie que "
                                   "« %7 » est antérieur à « %8 ». Probabilité que « %7 » soit postérieur à « %8 » : %9 %.\n")
                        .arg(dateformat::spread(st.mode, fmt), dateformat::spread(st.mean, fmt), dateformat::spread(st.sd, fmt),
                             dateformat::spread(st.q1, fmt), dateformat::spread(st.median, fmt), dateformat::spread(st.q3, fmt),
                             B, A, text::num(100.0 * pPos, 1));
        R.extraPlot = spreadPlot(QStringLiteral("Durée — %1").arg(R.name), QStringLiteral("Durée"), R.name, d, R.level, fmt);
    };

    // 4a. Calcul exact : datations indépendantes et années fixes seulement.
    if (exact) {
        if (op == QLatin1String("value")) {
            const Density& d = X[0].d;
            appendSummary(R.html, R.prompt, d, hpdIntervals(d, 95.0), hpdIntervals(d, 68.0), densityStats(d), R.level, fmt);
            operandSeries(X[0]);
            return finish();
        }
        if (op == QLatin1String("inside")) {
            // P(C dans [min(A,B) ; max(A,B)]) = ∫ f_C(c) [F_A(c)(1 − F_B(c)) + F_B(c)(1 − F_A(c))] dc ; avant : (1 − F_A)(1 − F_B) ; après : F_A F_B.
            const CdfTable ta(X[1].d), tb(X[2].d);
            auto F = [&](const Operand& o, const CdfTable& t, double x) { return o.kind == Operand::Fixed ? (x >= o.year ? 1.0 : 0.0) : t.at(x); };
            const Density& c = X[0].d;
            double pb = 0.0, pa = 0.0, mass = 0.0;
            for (size_t i = 0; i + 1 < c.t.size(); ++i) {
                const double m = 0.5 * (c.v[i] + c.v[i + 1]) * (c.t[i + 1] - c.t[i]), x = 0.5 * (c.t[i] + c.t[i + 1]);
                const double fa = F(X[1], ta, x), fb = F(X[2], tb, x);
                pb += m * (1.0 - fa) * (1.0 - fb);
                pa += m * fa * fb;
                mass += m;
            }
            if (!(mass > 0.0)) return fail(QStringLiteral("densité vide pour « %1 »").arg(Cn));
            pb /= mass; pa /= mass;
            const double pi = std::clamp(1.0 - pb - pa, 0.0, 1.0);
            R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>"
                                     "<tr><td><b>P(« %1 » dans %2)</b></td><td><b>%3</b></td></tr>"
                                     "<tr><td>P(« %1 » avant l'intervalle)</td><td>%4</td></tr>"
                                     "<tr><td>P(« %1 » après l'intervalle)</td><td>%5</td></tr></table>")
                          .arg(Cn.toHtmlEscaped(), iv.toHtmlEscaped(), percentText(pi), percentText(pb), percentText(pa));
            R.html += QStringLiteral("<p><small>Calcul exact sur les densités (fonctions de répartition), datations supposées "
                                     "<b>indépendantes</b> ; les bornes sont prises à chaque fois dans l'ordre (la plus ancienne en premier). "
                                     "Probabilité a posteriori, pas un test de signification.</small></p>");
            R.prompt += QStringLiteral("P(« %1 » dans %2) = %3 ; P(avant) = %4 ; P(après) = %5 (calcul exact sur les densités).\n")
                            .arg(Cn, iv, percentText(pi), percentText(pb), percentText(pa));
            R.prompt += independence;
            for (const Operand& o : X) operandSeries(o);
            return finish();
        }
        // Écart D = B − A (order, duration)
        const Operand& a = X[0];
        const Operand& b = X[1];
        const Density D = a.kind == Operand::Fixed ? affineDensity(b.d, 1.0, -a.year)
                        : b.kind == Operand::Fixed ? affineDensity(a.d, -1.0, b.year)
                                                   : differenceDensity(a.d, b.d);
        if (!D.valid()) return fail(QStringLiteral("écart « %1 » − « %2 » impossible à calculer").arg(B, A));
        const double pAB = std::clamp(1.0 - densityCdf(D, 0.0), 0.0, 1.0), pBA = 1.0 - pAB;
        const QString method = a.kind == Operand::Independent && b.kind == Operand::Independent
            ? QStringLiteral("convolution, pas %1").arg(dateformat::spread(D.step, fmt)) : QStringLiteral("densité décalée de l'année fixe");
        if (op == QLatin1String("duration")) {
            durationTable(D, pAB);
            R.html += QStringLiteral("<p><small>Calcul exact sur les densités (%1) en les supposant <b>indépendantes</b> : aucune contrainte "
                                     "stratigraphique ni phase.</small></p>").arg(method.toHtmlEscaped());
            R.prompt += independence;
            for (const Operand& o : X) operandSeries(o);
            return finish();
        }
        const DensityStats st = densityStats(D);
        const dateformat::PrecisionScope precision(st.sd);
        QString rows, promptHpd;
        rows += QStringLiteral("<tr><td><b>P(« %1 » antérieur à « %2 »)</b></td><td><b>%3</b></td></tr>")
                    .arg(A.toHtmlEscaped(), B.toHtmlEscaped(), percentText(pAB));
        rows += QStringLiteral("<tr><td><b>P(« %1 » antérieur à « %2 »)</b></td><td><b>%3</b></td></tr>")
                    .arg(B.toHtmlEscaped(), A.toHtmlEscaped(), percentText(pBA));
        for (double l : displayLevels(R.level)) {
            const QString t = durationIntervalsText(hpdIntervals(D, l), fmt), lt = levelText(l);
            rows += QStringLiteral("<tr><td valign='top'>Écart « %1 » − « %2 », HPD %3 %</td><td>%4</td></tr>")
                        .arg(B.toHtmlEscaped(), A.toHtmlEscaped(), lt, t.toHtmlEscaped().replace('\n', QStringLiteral("<br>")));
            promptHpd += QStringLiteral("Écart « %1 » − « %2 » (années), intervalles HPD à %3 % :\n- %4\n")
                             .arg(B, A, lt, QString(t).replace("\n", "\n- "));
        }
        rows += QStringLiteral("<tr><td>Écart : médiane ; moyenne ± écart-type</td><td>%1 ; %2 ± %3</td></tr>")
                    .arg(dateformat::spread(st.median, fmt), dateformat::spread(st.mean, fmt), dateformat::spread(st.sd, fmt));
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
        R.html += QStringLiteral("<p><small>Calcul exact sur les deux densités (%1) en les supposant <b>indépendantes</b> : "
                                 "aucune contrainte stratigraphique ni phase. Un écart positif signifie « %2 » plus récent que « %3 ». "
                                 "Dans un modèle ChronoModel, l'ordre se lit sur les traces exportées (calcul tirage par tirage).</small></p>")
                      .arg(method.toHtmlEscaped(), B.toHtmlEscaped(), A.toHtmlEscaped());
        R.prompt += QStringLiteral("P(« %1 » antérieur à « %2 ») = %3 ; P(« %2 » antérieur à « %1 ») = %4.\n")
                        .arg(A, B, percentText(pAB), percentText(pBA));
        R.prompt += promptHpd;
        R.prompt += QStringLiteral("Écart « %1 » − « %2 » : médiane %3, moyenne %4, écart-type %5 (écart positif = « %1 » plus récent).\n")
                        .arg(B, A, dateformat::spread(st.median, fmt), dateformat::spread(st.mean, fmt), dateformat::spread(st.sd, fmt));
        R.prompt += QStringLiteral("Méthode : calcul exact sur les densités calibrées, les deux datations étant supposées indépendantes "
                                   "(P(A antérieur à B) = ∫ f_A(a) · P(B > a) da ; écart = densité de B − A). Consigne : donne les deux "
                                   "probabilités telles quelles. C'est une probabilité a posteriori, pas un test de signification ni une "
                                   "p-value : ne fixe pas de seuil à la place de l'utilisateur. Explique que des intervalles HPD qui se "
                                   "recouvrent n'empêchent pas une probabilité nette, et que des intervalles séparés la rendent proche de "
                                   "100 %. Rappelle que l'indépendance suppose qu'aucune autre information ne relie les deux datations : si "
                                   "une stratigraphie, une phase ou d'autres dates les relient, il faut un modèle ChronoModel (avec ou sans "
                                   "la contrainte) et le calcul sur ses traces exportées ; une contrainte imposée donne 100 % par "
                                   "construction. Une calibration multimodale (plateau de la courbe) peut rendre l'ordre incertain même "
                                   "avec des mesures précises.\n");
        for (const Operand& o : X) operandSeries(o);
        R.extraPlot = spreadPlot(QStringLiteral("Écart %1 − %2").arg(B, A), QStringLiteral("Écart"), QStringLiteral("%1 − %2").arg(B, A), D, R.level, fmt);
        return finish();
    }

    // 4b. Tirage par tirage : colonnes des traces telles quelles, datations indépendantes tirées, années répétées.
    constexpr size_t kSimulated = 100000;            // tirages Monte-Carlo sans traces (date entre deux datations)
    const size_t N = hasColumn ? set->rows() : kSimulated;
    const std::vector<size_t> chainStarts = hasColumn ? set->chainStarts : std::vector<size_t>{ 0 };
    std::mt19937 rng(12345);                         // reproductible
    std::vector<std::vector<double>> owned(X.size());
    std::vector<const std::vector<double>*> cols(X.size(), nullptr);
    for (size_t k = 0; k < X.size(); ++k) {
        if (X[k].kind == Operand::Column) { cols[k] = X[k].col; continue; }
        owned[k] = X[k].kind == Operand::Independent ? sampleDensity(X[k].d, N, rng) : std::vector<double>(N, X[k].year);
        cols[k] = &owned[k];
    }
    QString basis = hasColumn ? QStringLiteral("%1 tirages a posteriori (%2 chaîne%3)").arg(fr.toString(qlonglong(N))).arg(chainStarts.size())
                                    .arg(chainStarts.size() > 1 ? "s" : "")
                              : QStringLiteral("%1 tirages Monte-Carlo des datations indépendantes").arg(fr.toString(qlonglong(N)));
    const QString drawn = hasIndep ? QStringLiteral("chaque datation indépendante tirée par inverse de sa fonction de répartition, en tirage stratifié")
                                   : QString();

    // Corrélation des termes `idx` sur les tirages communs : matrice (r de Pearson au-dessus de la diagonale, avec un
    // intervalle approché à 95 % par la transformation de Fisher sur la taille d'échantillon effective, ρ de Spearman
    // au-dessous) ; avec `plots`, nuage de points de chaque paire (3 au plus) avec la droite « B = A ».
    auto correlationBlock = [&](const std::vector<size_t>& idx, bool plots) {
        const size_t k = idx.size();
        if (k < 2) return;
        std::vector<size_t> rowsKept;
        std::vector<size_t> chainOf;
        size_t ch = 0;
        for (size_t i = 0; i < N; ++i) {
            while (ch + 1 < chainStarts.size() && i >= chainStarts[ch + 1]) ++ch;
            bool ok = true;
            for (size_t j : idx) ok = ok && std::isfinite((*cols[j])[i]);
            if (ok) { rowsKept.push_back(i); chainOf.push_back(ch); }
        }
        const size_t n = rowsKept.size();
        if (n < 10) return;
        std::vector<std::vector<double>> v(k, std::vector<double>(n)), rk(k, std::vector<double>(n));
        std::vector<double> ess(k, double(n));
        std::vector<bool> constant(k, false);
        for (size_t a = 0; a < k; ++a) {
            for (size_t i = 0; i < n; ++i) v[a][i] = (*cols[idx[a]])[rowsKept[i]];
            std::vector<size_t> ord(n);
            std::iota(ord.begin(), ord.end(), size_t(0));
            std::sort(ord.begin(), ord.end(), [&](size_t x, size_t y) { return v[a][x] < v[a][y]; });
            for (size_t i = 0; i < n;) {                           // rangs, moyenne des rangs pour les ex aequo
                size_t j = i;
                while (j + 1 < n && v[a][ord[j + 1]] == v[a][ord[i]]) ++j;
                for (size_t t = i; t <= j; ++t) rk[a][ord[t]] = 0.5 * double(i + j) + 1.0;
                i = j + 1;
            }
            constant[a] = v[a][ord.front()] == v[a][ord.back()];
            // Taille d'échantillon effective d'une colonne (autocorrélation des chaînes) ; datation tirée : n.
            if (X[idx[a]].kind == Operand::Column && chainStarts.size() >= 2) {
                std::vector<std::vector<double>> byCh(chainStarts.size());
                for (size_t i = 0; i < n; ++i) byCh[chainOf[i]].push_back(v[a][i]);
                size_t len = std::numeric_limits<size_t>::max();
                for (const auto& c : byCh) len = std::min(len, c.size());
                if (len >= 4) {
                    for (auto& c : byCh) c.resize(len);
                    const mcmcdiag::RhatEss re = mcmcdiag::computeRhatAndEss(byCh);
                    if (std::isfinite(re.bulkEss) && re.bulkEss > 0) ess[a] = std::min(double(n), re.bulkEss);
                }
            }
        }
        auto pearson = [&](const std::vector<double>& x, const std::vector<double>& y) {
            double mx = 0, my = 0;
            for (size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
            mx /= double(n); my /= double(n);
            double sxy = 0, sxx = 0, syy = 0;
            for (size_t i = 0; i < n; ++i) { const double dx = x[i] - mx, dy = y[i] - my; sxy += dx * dy; sxx += dx * dx; syy += dy * dy; }
            return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : std::nan("");
        };
        auto r3 = [](double r) { return std::isfinite(r) ? text::num(std::abs(r) < 0.005 ? 0.0 : r, 2) : QStringLiteral("—"); };
        QString head = QStringLiteral("<tr><td></td>");
        for (size_t a = 0; a < k; ++a) head += QStringLiteral("<td><b>%1</b></td>").arg(X[idx[a]].label.toHtmlEscaped());
        QString rowsHtml, pairs;
        bool withIndep = false;
        std::vector<std::vector<double>> Rm(k, std::vector<double>(k, 1.0));
        for (size_t a = 0; a < k; ++a) {
            rowsHtml += QStringLiteral("<tr><td><b>%1</b></td>").arg(X[idx[a]].label.toHtmlEscaped());
            for (size_t b = 0; b < k; ++b) {
                if (a == b) { rowsHtml += QStringLiteral("<td align='center'>1</td>"); continue; }
                const size_t lo = std::min(a, b), hi = std::max(a, b);
                const double r = constant[lo] || constant[hi] ? std::nan("") : pearson(v[lo], v[hi]);
                const double rho = constant[lo] || constant[hi] ? std::nan("") : pearson(rk[lo], rk[hi]);
                Rm[a][b] = r;
                if (a < b) {                                           // au-dessus : Pearson et intervalle
                    const double ne = std::min(ess[a], ess[b]);
                    QString ci;
                    if (std::isfinite(r) && ne > 4 && std::abs(r) < 1.0) {
                        const double z = std::atanh(r), se = 1.0 / std::sqrt(ne - 3.0);
                        ci = QStringLiteral("[%1 ; %2]").arg(r3(std::tanh(z - 1.96 * se)), r3(std::tanh(z + 1.96 * se)));
                    }
                    rowsHtml += QStringLiteral("<td align='center'>r = %1<br><small>%2</small></td>").arg(r3(r), ci);
                    const bool indep = (X[idx[a]].kind == Operand::Independent) != (X[idx[b]].kind == Operand::Independent)
                                       || (X[idx[a]].kind == Operand::Independent && X[idx[b]].kind == Operand::Independent);
                    withIndep = withIndep || indep;
                    pairs += QStringLiteral("- « %1 » et « %2 » : r de Pearson = %3%4 ; ρ de Spearman = %5 (n_eff ≈ %6)%7.\n")
                                 .arg(X[idx[a]].label, X[idx[b]].label, r3(r),
                                      ci.isEmpty() ? QString() : QStringLiteral(" (intervalle approché à 95 % : %1)").arg(ci), r3(rho),
                                      fr.toString(qlonglong(std::llround(ne))),
                                      indep ? QStringLiteral(" — nulle par construction (datation tirée indépendamment)") : QString());
                } else {
                    rowsHtml += QStringLiteral("<td align='center'><i>ρ = %1</i></td>").arg(r3(rho));
                }
            }
            rowsHtml += QStringLiteral("</tr>");
        }
        R.html += QStringLiteral("<p><b>Corrélation entre les termes</b> (%1 tirages communs)</p>"
                                 "<table cellspacing='0' cellpadding='3' border='1' style='border-collapse:collapse'>%2</tr>%3</table>"
                                 "<p><small>Au-dessus de la diagonale : r de Pearson (corrélation linéaire), avec entre crochets l'intervalle à 95 % "
                                 "de sa vraie valeur, qui tient compte du nombre limité de tirages indépendants (ESS). Au-dessous : ρ de "
                                 "Spearman (corrélation des rangs). Une corrélation positive signifie que les deux dates bougent ensemble d'un tirage à l'autre "
                                 "(contrainte, phase ou données communes) ; elle mesure la dépendance a posteriori, pas une causalité.%4</small></p>")
                      .arg(fr.toString(qlonglong(n)), head, rowsHtml,
                           withIndep ? QStringLiteral(" Une datation indépendante est tirée sans lien avec les autres termes : sa "
                                                      "corrélation avec eux est nulle par construction (aux fluctuations près).")
                                     : QString());
        R.prompt += QStringLiteral("Corrélation entre les termes (sur %1 tirages communs) :\n%2").arg(fr.toString(qlonglong(n)), pairs);
        if (!plots) return;
        int made = 0;
        const size_t maxPts = 2500, stride = std::max<size_t>(1, n / maxPts);
        for (size_t a = 0; a < k && made < 3; ++a)
            for (size_t b = a + 1; b < k && made < 3; ++b) {
                if (constant[a] || constant[b]) continue;
                QJsonArray xs, ys;
                double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                for (size_t i = 0; i < n; i += stride) {
                    xs.append(dateformat::convert(v[a][i], fmt));
                    ys.append(dateformat::convert(v[b][i], fmt));
                    lo = std::min({ lo, v[a][i], v[b][i] });
                    hi = std::max({ hi, v[a][i], v[b][i] });
                }
                const QString la = X[idx[a]].label, lb = X[idx[b]].label;
                const QJsonObject pts{ { "name", QStringLiteral("Tirages (%1)").arg(fr.toString(qlonglong(xs.size()))) },
                                       { "x", xs }, { "y", ys }, { "points", true } };
                const QJsonObject diag{ { "name", QStringLiteral("%1 = %2").arg(lb, la) },
                                        { "x", QJsonArray{ dateformat::convert(lo, fmt), dateformat::convert(hi, fmt) } },
                                        { "y", QJsonArray{ dateformat::convert(lo, fmt), dateformat::convert(hi, fmt) } },
                                        { "dash", true }, { "width", 1.0 }, { "color", QStringLiteral("#e1572a") }, { "fill", false } };
                QJsonObject plot{ { "title", QStringLiteral("Corrélation %1 / %2 — r = %3").arg(la, lb, r3(Rm[a][b])) },
                                  { "xlabel", QStringLiteral("%1 — %2").arg(la, dateformat::axisLabel(fmt)) },
                                  { "ylabel", QStringLiteral("%1 — %2").arg(lb, dateformat::axisLabel(fmt)) },
                                  { "fill", false }, { "yaxis", true }, { "yzero", false }, { "layout", QStringLiteral("overlay") },
                                  { "series", QJsonArray{ pts, diag } } };
                if (dateformat::isAge(fmt)) { plot["xreverse"] = true; plot["yreverse"] = true; }
                R.scatterPlots << plot;
                ++made;
            }
        if (made > 0)
            R.prompt += QStringLiteral("Nuage%1 de points des tirages (une paire par graphique, droite « B = A » en tirets : au-dessus, "
                                       "le second terme est plus récent) DÉJÀ tracé%1 dans un onglet à part.\n").arg(made > 1 ? "s" : "");
    };

    if (op == QLatin1String("correlation")) {
        std::vector<size_t> all(X.size());
        std::iota(all.begin(), all.end(), size_t(0));
        correlationBlock(all, true);
        if (R.scatterPlots.isEmpty() && !R.html.contains(QStringLiteral("Corrélation entre les termes")))
            return fail(QStringLiteral("moins de 10 tirages exploitables (valeurs vides dans les colonnes)"));
        R.html += QStringLiteral("<p><small>Calcul sur %1%2.</small></p>").arg(basis.toHtmlEscaped(),
                      drawn.isEmpty() ? QString() : QStringLiteral(", ") + drawn.toHtmlEscaped());
        R.prompt += QStringLiteral("Calcul sur %1 (dépendance entre colonnes respectée).\n").arg(basis);
        if (!hasColumn) R.prompt += independence;
        for (const Operand& o : X) operandSeries(o);
        return finish();
    }
    // Les autres opérations tirage par tirage donnent aussi la corrélation des termes variables (sans graphique).
    auto autoCorrelation = [&]() {
        std::vector<size_t> var;
        for (size_t j = 0; j < X.size(); ++j) if (X[j].kind != Operand::Fixed) var.push_back(j);
        if (var.size() >= 2 && hasColumn) correlationBlock(var, false);
    };

    if (op == QLatin1String("order") || op == QLatin1String("inside")) {
        const bool inside = op == QLatin1String("inside");
        std::vector<std::vector<double>> byChain(chainStarts.size());
        size_t chain = 0, kept = 0, n1 = 0, n2 = 0, n3 = 0;       // order : A<B, B<A, égaux ; inside : avant, dedans, après
        for (size_t i = 0; i < N; ++i) {
            while (chain + 1 < chainStarts.size() && i >= chainStarts[chain + 1]) ++chain;
            bool finite = true;
            for (const auto* c : cols) finite = finite && std::isfinite((*c)[i]);
            if (!finite) continue;
            ++kept;
            double ind = 0.0;
            if (!inside) {
                const double a = (*cols[0])[i], b = (*cols[1])[i];
                if (a < b) { ++n1; ind = 1.0; } else if (b < a) ++n2; else ++n3;
            } else {
                const double c = (*cols[0])[i];
                const double lo = std::min((*cols[1])[i], (*cols[2])[i]), hi = std::max((*cols[1])[i], (*cols[2])[i]);
                if (c < lo) ++n1; else if (c > hi) ++n3; else { ++n2; ind = 1.0; }
            }
            byChain[chain].push_back(ind);
        }
        if (kept < 10) return fail(QStringLiteral("moins de 10 tirages exploitables (valeurs vides dans les colonnes)"));
        const double ess = indicatorEss(byChain, kept);
        QString rows;
        if (!inside) {
            const double pAB = double(n1) / double(kept), pBA = double(n2) / double(kept);
            rows += QStringLiteral("<tr><td><b>P(« %1 » antérieur à « %2 »)</b></td><td>%3</td></tr>").arg(A.toHtmlEscaped(), B.toHtmlEscaped(), probabilityText(pAB, ess));
            rows += QStringLiteral("<tr><td><b>P(« %1 » antérieur à « %2 »)</b></td><td>%3</td></tr>").arg(B.toHtmlEscaped(), A.toHtmlEscaped(), probabilityText(pBA, ess));
            if (n3 > 0) rows += QStringLiteral("<tr><td>Tirages égaux</td><td>%1</td></tr>").arg(fr.toString(qlonglong(n3)));
            R.prompt += QStringLiteral("P(« %1 » antérieur à « %2 ») = %3 ; P(« %2 » antérieur à « %1 ») = %4 (proportion des tirages, erreur "
                                       "Monte-Carlo entre parenthèses).%5\n")
                            .arg(A, B, probabilityText(pAB, ess), probabilityText(pBA, ess),
                                 n3 > 0 ? QStringLiteral(" %1 tirages égaux.").arg(n3) : QString());
        } else {
            const double pb = double(n1) / double(kept), pi = double(n2) / double(kept), pa = double(n3) / double(kept);
            rows += QStringLiteral("<tr><td><b>P(« %1 » dans %2)</b></td><td>%3</td></tr>").arg(Cn.toHtmlEscaped(), iv.toHtmlEscaped(), probabilityText(pi, ess));
            rows += QStringLiteral("<tr><td>P(« %1 » avant l'intervalle)</td><td>%2</td></tr>").arg(Cn.toHtmlEscaped(), probabilityText(pb, ess));
            rows += QStringLiteral("<tr><td>P(« %1 » après l'intervalle)</td><td>%2</td></tr>").arg(Cn.toHtmlEscaped(), probabilityText(pa, ess));
            R.prompt += QStringLiteral("P(« %1 » dans %2) = %3 ; P(avant) = %4 ; P(après) = %5 (proportions des tirages, bornes prises à chaque "
                                       "tirage, la plus ancienne en premier ; erreur Monte-Carlo entre parenthèses).\n")
                            .arg(Cn, iv, probabilityText(pi, ess), probabilityText(pb, ess), probabilityText(pa, ess));
        }
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
        basis = QStringLiteral("%1 tirages %2(%3 chaîne%4, ESS ≈ %5)")
                    .arg(fr.toString(qlonglong(kept)), hasColumn ? QStringLiteral("a posteriori ") : QString()).arg(chainStarts.size())
                    .arg(chainStarts.size() > 1 ? "s" : "").arg(fr.toString(qlonglong(std::llround(ess))));
        if (!drawn.isEmpty()) basis += QStringLiteral(", ") + drawn;
        R.html += QStringLiteral("<p><small>Proportion des tirages, calculée tirage par tirage sur %1 : probabilité a posteriori%2, "
                                 "pas un test de signification.</small></p>")
                      .arg(basis.toHtmlEscaped(), hasColumn ? QStringLiteral(" dans le modèle ChronoModel (contraintes et phases comprises)") : QString());
        R.prompt += QStringLiteral("Calcul sur %1 (dépendance entre colonnes respectée). Probabilité a posteriori, pas un test de signification "
                                   "ni une p-value : ne fixe pas de seuil à la place de l'utilisateur.\n").arg(basis);
        for (const Operand& o : X) if (o.kind != Operand::Fixed) operandSeries(o);
        autoCorrelation();
        return finish();
    }

    // value, duration, between : densité de la quantité dérivée
    std::vector<double> x;
    std::vector<std::vector<double>> byChain(chainStarts.size());
    std::uniform_real_distribution<double> unif(0.0, 1.0);
    const int draws = op == "between" ? int(std::clamp<size_t>(200000 / std::max<size_t>(N, 1), 1, 10)) : 1;
    size_t chain = 0, kept = 0, positive = 0;
    for (size_t i = 0; i < N; ++i) {
        while (chain + 1 < chainStarts.size() && i >= chainStarts[chain + 1]) ++chain;
        const double a = (*cols[0])[i], b = cols.size() > 1 ? (*cols[1])[i] : 0.0;
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        ++kept;
        if (op == "value") { x.push_back(a); byChain[chain].push_back(a); }
        else if (op == "duration") { x.push_back(b - a); byChain[chain].push_back(b - a); if (b > a) ++positive; }
        else {
            const double lo = std::min(a, b), hi = std::max(a, b);
            for (int k = 0; k < draws; ++k) x.push_back(lo + unif(rng) * (hi - lo));
        }
    }
    if (kept < 10) return fail(QStringLiteral("moins de 10 tirages exploitables (valeurs vides dans les colonnes)"));
    double h = 0.0;
    Bandwidth rule = Bandwidth::SheatherJones;
    const Density d = kernelDensity(x, -infv, infv, Bandwidth::SheatherJones, &h, &rule);
    if (!d.valid()) return fail(QStringLiteral("densité impossible : tous les tirages ont la même valeur (colonne fixée ?)"));
    basis = hasColumn ? QStringLiteral("%1 tirages a posteriori (%2 chaîne%3)").arg(fr.toString(qlonglong(kept))).arg(chainStarts.size())
                            .arg(chainStarts.size() > 1 ? "s" : "")
                      : QStringLiteral("%1 tirages Monte-Carlo des datations indépendantes").arg(fr.toString(qlonglong(kept)));
    if (op == "between") basis += QStringLiteral(", %1 dates tirées par tirage").arg(draws);
    if (!drawn.isEmpty()) basis += QStringLiteral(", ") + drawn;
    if (hasColumn) {
        R.html += QStringLiteral("<p><i>Calcul tirage par tirage sur %1 : la dépendance entre les colonnes (contraintes, phases) est respectée.</i></p>")
                      .arg(basis.toHtmlEscaped());
        R.prompt += QStringLiteral("Calcul tirage par tirage sur %1 (dépendance entre colonnes respectée).\n").arg(basis);
    } else {
        R.html += QStringLiteral("<p><i>Calcul par tirages sur %1, datations supposées indépendantes.</i></p>").arg(basis.toHtmlEscaped());
        R.prompt += QStringLiteral("Calcul par tirages sur %1.\n").arg(basis);
        R.prompt += independence;
    }
    if (op == "duration") {
        durationTable(d, double(positive) / double(kept));
    } else {
        appendSummary(R.html, R.prompt, d, hpdIntervals(d, 95.0), hpdIntervals(d, 68.0), densityStats(d), R.level, fmt);
        R.series.append(plotSeries(op == "between" ? QStringLiteral("Date entre %1 et %2").arg(A, B) : A, d, fmt));
        if (op == "between") for (const Operand& o : X) if (o.kind != Operand::Fixed) operandSeries(o);
    }

    // Lissage et convergence de la quantité calculée
    const QString hRule = rule == Bandwidth::SheatherJones ? QStringLiteral("Sheather-Jones") : QStringLiteral("Silverman (repli)");
    QString conv;
    size_t len = std::numeric_limits<size_t>::max();
    for (const std::vector<double>& c : byChain) len = std::min(len, c.size());
    if (hasColumn && op != "between" && byChain.size() >= 2 && len >= 4) {
        std::vector<std::vector<double>> eq;
        for (const std::vector<double>& c : byChain) eq.emplace_back(c.begin(), c.begin() + std::ptrdiff_t(len));
        const mcmcdiag::RhatEss re = mcmcdiag::computeRhatAndEss(eq);
        const double M = double(eq.size());
        const bool good = re.rhat < mcmcdiag::kRhatGood && std::min(re.bulkEss, re.tailEss) >= mcmcdiag::kEssGoodPerChain * M;
        const bool warnc = re.rhat < mcmcdiag::kRhatWarning && std::min(re.bulkEss, re.tailEss) >= mcmcdiag::kEssWarningPerChain * M;
        conv = QStringLiteral("%1 (R̂ = %2 ; ESS bulk = %3 ; ESS tail = %4)")
                   .arg(good ? QStringLiteral("✅ satisfaisante") : warnc ? QStringLiteral("⚠️ à surveiller") : QStringLiteral("❌ insuffisante"),
                        rhatText(re.rhat), essText(re.bulkEss), essText(re.tailEss));
    }
    R.html += QStringLiteral("<p><small>Densité lissée par noyau gaussien, h = %1 (règle de %2)%3.</small></p>")
                  .arg(dateformat::spread(h, fmt).toHtmlEscaped(), hRule,
                       conv.isEmpty() ? QString() : QStringLiteral(" ; convergence de cette quantité : %1").arg(conv.toHtmlEscaped()));
    R.prompt += QStringLiteral("Densité lissée par noyau gaussien : h = %1 (règle de %2).%3\n")
                    .arg(dateformat::spread(h, fmt), hRule, conv.isEmpty() ? QString() : QStringLiteral(" Convergence de cette quantité : %1.").arg(conv));
    autoCorrelation();
    return finish();
}

// ------------------------------------------------------------------ courbes de ChronoModel (splines)
//
// Calcul « curve » sur une courbe estimée par ChronoModel (mode courbe : 1 à 3 composantes en fonction du temps), à
// partir de la trace des splines exportée (Curve_X_Splines.csv) : chaque tirage est une spline complète, évaluée comme
// dans ChronoModel (valeurs_G_GP_GS). Opérations :
//   value       : loi de G(t) à une date t (année, colonne des traces, datation), P(G < y), P(ymin ≤ G ≤ ymax) ;
//   derivative  : loi de la vitesse instantanée G′(t) ;
//   rate        : vitesse moyenne entre deux dates, (G(t2) − G(t1)) / (t2 − t1), tirage par tirage ;
//   crossing    : date où la courbe atteint la valeur y (première, dernière ou toutes) ;
//   rate_values : temps mis pour aller de y1 à y2 (première atteinte de y1, puis première atteinte de y2 ensuite) et
//                 vitesse moyenne (y2 − y1) / (t2 − t1).
// Sans trace des splines, une carte de densité (Curve_X_Map.csv, Curve_X_GP_Map.csv) suffit pour value et derivative
// (loi de G ou G′ dans une colonne de la carte) ; les autres opérations relient plusieurs dates d'une même spline et
// demandent la trace.

struct CurveRun {
    bool ok = false;
    bool posterior = true;
    QString html, prompt, title, name;
    QJsonArray series;                 // densités de dates (date d'atteinte d'une valeur) : graphique des datations
    QVector<QJsonObject> plots;        // graphiques à part : loi de la grandeur, courbe et son enveloppe
    double level = MethodRunner::kDefaultLevel;
};

QString curveOp(const QString& raw)
{
    static const QHash<QString, QString> ops = {
        { "value", "value" }, { "valeur", "value" }, { "densite", "value" }, { "density", "value" }, { "probabilite", "value" },
        { "probability", "value" }, { "g", "value" }, { "loi", "value" },
        { "derivative", "derivative" }, { "derivee", "derivative" }, { "vitesse_instantanee", "derivative" }, { "instant_rate", "derivative" },
        { "speed_at", "derivative" }, { "gp", "derivative" }, { "slope", "derivative" }, { "pente", "derivative" },
        { "rate", "rate" }, { "vitesse", "rate" }, { "speed", "rate" }, { "velocity", "rate" }, { "taux", "rate" },
        { "vitesse_moyenne", "rate" }, { "mean_rate", "rate" }, { "rate_dates", "rate" }, { "vitesse_dates", "rate" },
        { "crossing", "crossing" }, { "date", "crossing" }, { "quand", "crossing" }, { "when", "crossing" }, { "atteinte", "crossing" },
        { "reach", "crossing" }, { "inverse", "crossing" }, { "temps", "crossing" }, { "time", "crossing" },
        { "calibrate", "calibrate" }, { "calibration", "calibrate" }, { "calibrer", "calibrate" }, { "dater", "calibrate" },
        { "datation", "calibrate" }, { "date_mesure", "calibrate" }, { "measure", "calibrate" }, { "mesure", "calibrate" },
        { "rate_values", "rate_values" }, { "vitesse_valeurs", "rate_values" }, { "rate_between_values", "rate_values" },
        { "between_values", "rate_values" }, { "vitesse_entre_valeurs", "rate_values" }, { "duree_valeurs", "rate_values" },
        { "duration_values", "rate_values" },
        { "acceleration", "acceleration" }, { "acceleration_instantanee", "acceleration" }, { "gs", "acceleration" },
        { "derivee_seconde", "acceleration" }, { "second_derivative", "acceleration" }, { "gpp", "acceleration" },
        { "plot", "plot" }, { "tracer", "plot" }, { "trace", "plot" }, { "draw", "plot" }, { "graph", "plot" }, { "graphique", "plot" },
        { "courbes", "plot" }, { "curves", "plot" }, { "dessiner", "plot" }, { "envelope", "plot" }, { "enveloppe", "plot" },
        { "extremum", "extremum" }, { "extreme", "extremum" }, { "extremes", "extremum" }, { "extrema", "extremum" },
        { "max", "extremum" }, { "maximum", "extremum" }, { "min", "extremum" }, { "minimum", "extremum" },
        { "argmax", "extremum" }, { "argmin", "extremum" }, { "peak", "extremum" }, { "pic", "extremum" },
        { "max_variation", "extremum" }, { "variation_max", "extremum" }, { "variation_maximale", "extremum" },
        { "variation_la_plus_forte", "extremum" }, { "plus_forte_variation", "extremum" }, { "strongest", "extremum" },
        { "steepest", "extremum" }, { "max_speed", "extremum" }, { "vitesse_max", "extremum" }, { "vitesse_maximale", "extremum" },
        { "max_rate", "extremum" }, { "date_max", "extremum" }, { "date_extremum", "extremum" } };
    const QString k = text::fold(raw).trimmed().replace(' ', '_').replace('-', '_');
    return ops.value(k);
}

// Grandeur de la courbe : 0 = G (valeur), 1 = G′ (vitesse ; taux d'accumulation pour une courbe âge-profondeur),
// 2 = G″ (accélération). -1 si vide, -2 si inconnue.
int curveQuantity(const QString& raw)
{
    const QString k = text::fold(raw).trimmed().replace(' ', '_').replace('-', '_');
    if (k.isEmpty()) return -1;
    static const QHash<QString, int> q = {
        { "value", 0 }, { "valeur", 0 }, { "g", 0 }, { "courbe", 0 }, { "curve", 0 }, { "0", 0 },
        { "derivative", 1 }, { "derivee", 1 }, { "vitesse", 1 }, { "speed", 1 }, { "velocity", 1 }, { "rate", 1 }, { "taux", 1 },
        { "gp", 1 }, { "slope", 1 }, { "pente", 1 }, { "taux_d_accumulation", 1 }, { "taux_accumulation", 1 }, { "accumulation", 1 },
        { "accumulation_rate", 1 }, { "sedimentation", 1 }, { "taux_de_sedimentation", 1 }, { "1", 1 },
        { "acceleration", 2 }, { "gs", 2 }, { "gpp", 2 }, { "derivee_seconde", 2 }, { "second_derivative", 2 }, { "2", 2 } };
    return q.value(k, -2);
}

// Nombre d'une demande (nombre ou texte « 1,5 ») ; NaN si absent ou illisible.
double curveNumber(const QJsonObject& rq, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        const QJsonValue v = rq.value(QLatin1String(k));
        if (v.isDouble()) return v.toDouble();
        if (v.isString()) {
            QString s = v.toString().trimmed();
            s.remove(QChar(0x00A0)).remove(' ');
            s.replace(',', '.').replace(QChar(0x2212), '-');
            bool ok = false;
            const double d = s.toDouble(&ok);
            if (ok) return d;
        }
    }
    return std::nan("");
}

// Valeur d'une grandeur de la courbe (G, G′, vitesse) : nombre en clair, avec les décimales automatiques.
QString curveNum(double v) { return dateformat::format(v, DateFormat::Numeric); }

// Graphique à part de la loi d'une grandeur (axe des valeurs, pas des dates).
QJsonObject valuePlot(const QString& title, const QString& xlabel, const QString& series, const Density& D, double level,
                      double marker = std::nan(""), double marker2 = std::nan(""), const QString& markerName = QString())
{
    // Partie utile de la densité : au-dessus de 1e-3 du maximum, entre les quantiles 0,1 % et 99,9 % (queues lointaines
    // de quelques tirages isolés exclues), ≤ ~400 points.
    QJsonArray xs, ys;
    const double vmax = D.v.empty() ? 0.0 : *std::max_element(D.v.begin(), D.v.end());
    const CdfTable C(D);
    const double qlo = C.inverse(0.001), qhi = C.inverse(0.999), pad = 0.05 * (qhi - qlo);
    size_t i0 = 0, i1 = D.t.empty() ? 0 : D.t.size() - 1;
    while (i0 < i1 && (D.v[i0] < vmax * 1e-3 || D.t[i0] < qlo - pad)) ++i0;
    while (i1 > i0 && (D.v[i1] < vmax * 1e-3 || D.t[i1] > qhi + pad)) --i1;
    const size_t step = std::max<size_t>(1, (i1 - i0 + 1) / 400);
    double ymax = 0.0;
    for (size_t k = i0; k <= i1 && k < D.t.size(); k += step) { xs.append(D.t[k]); ys.append(D.v[k]); ymax = std::max(ymax, D.v[k]); }
    QJsonArray S{ QJsonObject{ { "name", series }, { "x", xs }, { "y", ys } } };
    for (double m : { marker, marker2 })
        if (std::isfinite(m))
            S.append(QJsonObject{ { "name", markerName.isEmpty() ? curveNum(m) : QStringLiteral("%1 = %2").arg(markerName, curveNum(m)) }, { "x", QJsonArray{ m, m } }, { "y", QJsonArray{ 0.0, ymax } },
                                  { "dash", true }, { "width", 1.0 }, { "color", QStringLiteral("#e1572a") }, { "fill", false } });
    return QJsonObject{ { "title", title }, { "xlabel", xlabel }, { "ylabel", QStringLiteral("Densité de probabilité") }, { "fill", true },
                        { "yaxis", false }, { "hpd", level }, { "layout", QStringLiteral("overlay") }, { "series", S } };
}

// Chaînes de la trace des splines : celles d'un export de traces (Chain_all_Events.csv) du même calcul, reconnu au
// même nombre de tirages et à une colonne égale à un nœud du premier tirage ; sinon une seule chaîne.
std::vector<size_t> curveChains(const SplineTrace& S, QString* from)
{
    for (const TraceSet& ts : traces::loaded()) {
        if (ts.rows() != S.rows || ts.chainStarts.size() < 2) continue;
        for (const std::vector<double>& col : ts.values)
            for (int k = 0; k < S.knots; ++k)
                if (!col.empty() && std::abs(col.front() - S.theta[size_t(k)]) < 1e-6 * std::max(1.0, std::abs(col.front()))) {
                    if (from) *from = ts.file;
                    return ts.chainStarts;
                }
    }
    return { 0 };
}

CurveRun runCurve(const QJsonObject& rq, DateFormat fmt)
{
    CurveRun R;
    QString levelWarning;
    R.level = readLevel(rq, levelWarning);
    auto fail = [&](const QString& why) {
        R.html += QStringLiteral("<p><b>Courbe de ChronoModel</b></p><p><i>Calcul impossible : %1.</i></p>").arg(why.toHtmlEscaped());
        R.prompt += QStringLiteral("\nCourbe de ChronoModel — calcul impossible : %1.\n").arg(why);
        R.ok = false;
        return R;
    };
    const QLocale fr(QLocale::French);
    const double infv = std::numeric_limits<double>::infinity();

    // 1. Opération
    const QString rawOp = rq.value("op").toString();
    QString op = curveOp(rawOp);
    if (op.isEmpty()) {
        if (!rawOp.trimmed().isEmpty())
            return fail(QStringLiteral("opération « %1 » inconnue (value, derivative, acceleration, rate, crossing, calibrate, rate_values, extremum ou plot)").arg(rawOp));
        const bool hasY1 = std::isfinite(curveNumber(rq, { "y1" })), hasY = std::isfinite(curveNumber(rq, { "y", "value" }));
        op = hasY1 ? QStringLiteral("rate_values") : rq.contains("from") || rq.contains("t1") ? QStringLiteral("rate")
           : hasY && !rq.contains("t") ? QStringLiteral("crossing") : QStringLiteral("value");
    }
    // Grandeur : valeur G, vitesse G′ ou accélération G″ (« quantity »). « derivative » = vitesse, « acceleration » = G″ ;
    // « value » d'une vitesse = « derivative ». « plot » : liste de grandeurs (défaut : les trois).
    QString qText;
    for (const char* k : { "quantity", "grandeur", "of_curve", "courbe_de", "what" })
        if (rq.value(QLatin1String(k)).isString()) { qText = rq.value(QLatin1String(k)).toString(); break; }
    int order = -1;
    std::vector<int> plotOrders;
    if (op == "plot") {
        QStringList parts;
        for (const char* k : { "quantities", "grandeurs", "quantity", "grandeur" }) {
            const QJsonValue v = rq.value(QLatin1String(k));
            if (v.isArray()) { for (const QJsonValue& x : v.toArray()) parts << x.toString(); break; }
            if (v.isString()) { parts = v.toString().split(QRegularExpression(QStringLiteral("[,;/+]|\\s+et\\s+|\\s+and\\s+")), Qt::SkipEmptyParts); break; }
        }
        for (const QString& x : parts) {
            const int o = curveQuantity(x);
            if (o == -2) return fail(QStringLiteral("grandeur « %1 » inconnue (value, derivative ou acceleration)").arg(x.trimmed()));
            if (o >= 0 && std::find(plotOrders.begin(), plotOrders.end(), o) == plotOrders.end()) plotOrders.push_back(o);
        }
        if (plotOrders.empty()) plotOrders = { 0, 1, 2 };
        std::sort(plotOrders.begin(), plotOrders.end());
        order = plotOrders.front();
    } else {
        order = curveQuantity(qText);
        if (order == -2) return fail(QStringLiteral("grandeur « %1 » inconnue (value, derivative ou acceleration)").arg(qText.trimmed()));
        if (op == "extremum" && order < 0) order = 1;           // « variation la plus forte » : sur la vitesse G′
        if (op == "acceleration") { op = QStringLiteral("derivative"); if (order < 0) order = 2; }
        if (op == "derivative" && order < 0) order = 1;
        if (op == "derivative" && order == 0) op = QStringLiteral("value");
        if (op == "value" && order > 0) op = QStringLiteral("derivative");
        if (order < 0) order = 0;
        if ((op == "rate" || op == "rate_values") && order > 0)
            return fail(QStringLiteral("la vitesse moyenne se calcule sur la courbe elle-même (grandeur « value ») ; pour la vitesse ou "
                                       "l'accélération à une date, utiliser l'opération « derivative » ou « acceleration »"));
    }

    // 2. Composante et source (trace des splines, sinon carte de densité)
    const QString dimText = [&] {
        for (const char* k : { "dim", "component", "composante", "dimension" })
            if (rq.value(QLatin1String(k)).isString() && !rq.value(QLatin1String(k)).toString().trimmed().isEmpty())
                return rq.value(QLatin1String(k)).toString().trimmed();
        return QString();
    }();
    int dim = dimText.isEmpty() ? 0 : curves::dimIndex(dimText);
    if (dim < 0 && curves::splines().isEmpty())
        return fail(QStringLiteral("composante « %1 » inconnue (X, Y ou Z ; inclinaison, déclinaison ou intensité)").arg(dimText));
    QString dimNote;
    const QString wantedFile = rq.value("file").toString().trimmed();
    const SplineTrace* S = nullptr;
    for (auto it = curves::splines().crbegin(); it != curves::splines().crend(); ++it)        // la dernière jointe d'abord
        if (wantedFile.isEmpty() || it->file.compare(QFileInfo(wantedFile).fileName(), Qt::CaseInsensitive) == 0) { S = &*it; break; }
    if (S && !dimText.isEmpty()) {
        const int k = S->dimOf(dimText);
        if (k >= 0) dim = k;
        else if (S->dims == 1) {
            dimNote = QStringLiteral("la trace « %1 » n'a qu'une composante (%2) : « %3 » est lu comme cette composante")
                          .arg(S->file, S->names.value(0), dimText);
            dim = 0;
        } else {
            return fail(QStringLiteral("composante « %1 » absente de « %2 » (composantes : %3)").arg(dimText, S->file, S->names.join(QStringLiteral(", "))));
        }
    } else if (S) {
        dim = 0;
    }
    // calibrate : carte possible (G ou G′) ; l'accélération et le tracé demandent la trace.
    const bool needsTrace = op == "rate" || op == "crossing" || op == "rate_values" || op == "plot" || op == "extremum" || order == 2;
    const CurveMap* Mp = nullptr;
    if (!S) {
        if (curves::maps().isEmpty())
            return fail(QStringLiteral("aucune courbe chargée : joindre la trace des splines exportée par ChronoModel (Curve_X_Splines.csv)"));
        if (needsTrace)
            return fail(order == 2 ? QStringLiteral("l'accélération (dérivée seconde) demande la trace des splines (Curve_X_Splines.csv) : "
                                                    "ChronoModel n'exporte pas de carte de G″")
                        : op == "plot" ? QStringLiteral("le tracé des courbes de valeur, de vitesse et d'accélération demande la trace des "
                                                        "splines (Curve_X_Splines.csv)")
                        : op == "extremum" ? QStringLiteral("la date de l'extremum (variation la plus forte, maximum, minimum) se cherche "
                                                            "courbe par courbe : il faut la trace des splines (Curve_X_Splines.csv)")
                        : QStringLiteral("cette opération relie plusieurs dates d'une même spline : il faut la trace des splines "
                                         "(Curve_X_Splines.csv), une carte de densité ne donne que la loi de la courbe date par date"));
        const bool wantGP = order == 1;                    // vitesse (derivative, calibrate de G′) : carte de G′
        const CurveMap* any = nullptr;
        for (auto it = curves::maps().crbegin(); it != curves::maps().crend(); ++it) {
            const CurveMap& m = *it;
            if (m.derivative != wantGP) continue;
            if (!wantedFile.isEmpty() && m.file.compare(QFileInfo(wantedFile).fileName(), Qt::CaseInsensitive) != 0) continue;
            if (!any) any = &m;
            if (dimText.isEmpty() || curves::dimIndex(m.component) == dim) { Mp = &m; break; }
        }
        if (!Mp && any && curves::maps().size() == 1) { Mp = any; dimNote = QStringLiteral("une seule carte chargée : « %1 »").arg(any->file); }
        if (!Mp)
            return fail(wantGP ? QStringLiteral("pas de carte de vitesse (Curve_X_GP_Map.csv) chargée pour cette composante ; la trace des splines "
                                                "(Curve_X_Splines.csv) donne la vitesse exacte")
                               : QStringLiteral("pas de carte de densité (Curve_X_Map.csv) chargée pour cette composante"));
        if (!Mp->usable) return fail(Mp->notes.join(QStringLiteral(" ; ")));
        if (!Mp->component.isEmpty()) dim = std::max(0, curves::dimIndex(Mp->component));
    }
    const QString D = S ? S->names.value(dim) : !Mp->component.isEmpty() ? Mp->component : curves::dimName(dim);
    const QString unit = S ? S->unitOf(dim) : Mp && (Mp->component.startsWith(QLatin1String("Inc")) || Mp->component.startsWith(QLatin1String("Dec")))
                                                   ? QStringLiteral("°") : QString();
    double per = curveNumber(rq, { "per" });                 // vitesse par an (défaut) ou par siècle (per = 100)
    if (!(per > 0)) per = 1.0;
    const QString perText = per == 1.0 ? QStringLiteral("par an") : per == 100.0 ? QStringLiteral("par siècle")
                          : per == 1000.0 ? QStringLiteral("par millénaire") : QStringLiteral("pour %1 ans").arg(curveNum(per));
    const QString unitRate = unit == QStringLiteral("°") ? QStringLiteral("degrés %1").arg(perText) : QStringLiteral("unités de %1 %2").arg(D, perText);
    const QString perText2 = per == 1.0 ? QStringLiteral("par an²") : per == 100.0 ? QStringLiteral("par siècle²")
                           : per == 1000.0 ? QStringLiteral("par millénaire²") : QStringLiteral("pour (%1 ans)²").arg(curveNum(per));
    const QString unitAcc = unit == QStringLiteral("°") ? QStringLiteral("degrés %1").arg(perText2) : QStringLiteral("unités de %1 %2").arg(D, perText2);
    // Grandeur d'ordre k : symbole (X, X′, X″), mot, unité, facteur d'échelle (vitesse × per, accélération × per²).
    auto qSym = [&](int k) { return k == 0 ? D : k == 1 ? D + QChar(0x2032) : D + QChar(0x2033); };
    auto qWord = [](int k) { return k == 0 ? QStringLiteral("valeur") : k == 1 ? QStringLiteral("vitesse") : QStringLiteral("accélération"); };
    auto qUnit = [&](int k) { return k == 0 ? unit : k == 1 ? unitRate : unitAcc; };
    auto qScale = [&](int k) { return k == 0 ? 1.0 : k == 1 ? per : per * per; };
    const double scale = qScale(order);
    const QString Q = qSym(order);                          // grandeur de la demande

    // 3. Termes « date » (année fixe, colonne des traces, datation indépendante) et valeurs y
    auto dateTerm = [&](std::initializer_list<const char*> keys, const QString& fallback, Operand& o, QString& err) {
        const QJsonValue v = operandValue(rq, keys);
        if (v.isUndefined()) {
            err = QStringLiteral("date « %1 » absente (pour la date de la variation la plus forte, d'un maximum ou d'un minimum : op "
                                 "« extremum » ; pour tracer la courbe : op « plot »)").arg(fallback);
            return false;
        }
        return resolveOperand(v, rq, fallback, -1, fmt, o, err);
    };
    Operand T1, T2;
    QString err;
    const double y = curveNumber(rq, { "y", "value", "valeur" });
    const double ymin = curveNumber(rq, { "ymin", "y_min" }), ymax = curveNumber(rq, { "ymax", "y_max" });
    const double y1 = curveNumber(rq, { "y1", "from_value", "y_from", "value1" }), y2 = curveNumber(rq, { "y2", "to_value", "y_to", "value2" });
    const double yErr = curveNumber(rq, { "error", "y_error", "sigma", "erreur", "err" });
    if (op == "crossing" && yErr > 0) op = QStringLiteral("calibrate");       // valeur mesurée avec son erreur : date de la mesure
    if (op == "calibrate" && yErr < 0) return fail(QStringLiteral("l'erreur de la mesure (« error ») doit être positive ou nulle"));
    const bool exactValue = op == "calibrate" && !(yErr > 0);   // valeur exacte : limite σ → 0
    // Vitesse ou accélération demandée sans date (prompt réel du 05/11/2026 : « où est la variation la plus forte », le modèle
    // a demandé {"op":"derivative"} sans « t », trois fois de suite) : date de la vitesse la plus forte (extremum).
    QString extremumNote;
    if (op == "derivative" && operandValue(rq, { "t", "at", "date", "of", "time" }).isUndefined() && !std::isfinite(y)) {
        op = QStringLiteral("extremum");
        extremumNote = QStringLiteral("aucune date t donnée : SeldonH cherche la date où la %1 est la plus forte (en valeur absolue) ; pour "
                                      "la %1 à une date précise, donner cette date").arg(order == 2 ? QStringLiteral("accélération") : QStringLiteral("vitesse"));
    }
    // Sens de l'extremum : abs (la plus forte en valeur absolue, défaut pour une vitesse ou une accélération), max, min,
    // absmin (la plus faible en valeur absolue : la courbe la plus plate).
    QString sense;
    if (op == "extremum") {
        QString sv;
        for (const char* k : { "sense", "extremum", "kind", "type", "which", "sens" })
            if (rq.value(QLatin1String(k)).isString() && !rq.value(QLatin1String(k)).toString().trimmed().isEmpty()) {
                sv = text::fold(rq.value(QLatin1String(k)).toString()).trimmed().replace(' ', '_').replace('-', '_');
                break;
            }
        const QString ro = text::fold(rawOp).trimmed();
        if (sv.isEmpty() && (ro == "max" || ro == "maximum" || ro == "argmax" || ro == "peak" || ro == "pic")) sv = QStringLiteral("max");
        if (sv.isEmpty() && (ro == "min" || ro == "minimum" || ro == "argmin")) sv = QStringLiteral("min");
        static const QHash<QString, QString> senses = {
            { "max", "max" }, { "maximum", "max" }, { "maximal", "max" }, { "maximale", "max" }, { "highest", "max" }, { "plus_grand", "max" },
            { "plus_grande", "max" }, { "plus_haut", "max" }, { "croissance", "max" }, { "increase", "max" }, { "top", "max" },
            { "min", "min" }, { "minimum", "min" }, { "minimal", "min" }, { "minimale", "min" }, { "lowest", "min" }, { "plus_petit", "min" },
            { "plus_petite", "min" }, { "plus_bas", "min" }, { "decroissance", "min" }, { "decrease", "min" },
            { "abs", "abs" }, { "absmax", "abs" }, { "abs_max", "abs" }, { "max_abs", "abs" }, { "plus_forte", "abs" }, { "strongest", "abs" },
            { "largest", "abs" }, { "variation", "abs" }, { "steepest", "abs" }, { "absolu", "abs" }, { "absolue", "abs" },
            { "absmin", "absmin" }, { "abs_min", "absmin" }, { "min_abs", "absmin" }, { "plus_faible", "absmin" }, { "weakest", "absmin" },
            { "slowest", "absmin" }, { "flattest", "absmin" }, { "plus_plat", "absmin" }, { "plus_lente", "absmin" } };
        if (sv.isEmpty()) sense = order == 0 ? QStringLiteral("max") : QStringLiteral("abs");
        else if (senses.contains(sv)) sense = senses.value(sv);
        else return fail(QStringLiteral("sens de l'extremum « %1 » inconnu (abs, max, min ou absmin)").arg(sv));
    }
    if (op == "value" || op == "derivative") {
        if (!dateTerm({ "t", "at", "date", "of", "time" }, QStringLiteral("t"), T1, err)) return fail(err.isEmpty() ? QStringLiteral("date t illisible") : err);
    } else if (op == "rate") {
        if (!dateTerm({ "from", "t1", "a", "start" }, QStringLiteral("t1"), T1, err)) return fail(err);
        if (!dateTerm({ "to", "t2", "b", "end" }, QStringLiteral("t2"), T2, err)) return fail(err);
    } else if (op == "crossing" || op == "calibrate") {
        if (!std::isfinite(y)) return fail(QStringLiteral("valeur « y » absente"));
    } else if (op == "plot" || op == "extremum") {
        // pas de date ni de valeur : la fenêtre (tmin, tmax) suffit
    } else if (!std::isfinite(y1) || !std::isfinite(y2)) {
        return fail(QStringLiteral("il faut les deux valeurs « y1 » et « y2 »"));
    } else if (y1 == y2) {
        return fail(QStringLiteral("y1 et y2 sont égales : la vitesse entre deux valeurs demande deux valeurs différentes"));
    }
    R.html += T1.html + T2.html;
    R.prompt += T1.prompt + T2.prompt;
    auto termText = [&](const Operand& o) {
        return o.kind == Operand::Fixed ? o.label : o.kind == Operand::Column ? prettyColumn(o.label) : QStringLiteral("« %1 »").arg(o.label);
    };

    // Fenêtre de recherche des atteintes : période d'étude, ou [tmin ; tmax] de la demande.
    double wa = S ? S->tmin : 0.0, wb = S ? S->tmax() : 0.0;
    if (S) {
        const double a = numOf(rq, { "tmin" }, std::nan("")), b = numOf(rq, { "tmax" }, std::nan(""));
        if (std::isfinite(a)) wa = std::max(wa, a);
        if (std::isfinite(b)) wb = std::min(wb, b);
        if (!(wb > wa)) return fail(QStringLiteral("fenêtre [tmin ; tmax] vide ou hors de la période d'étude de la courbe"));
    }
    QString which = text::fold(rq.value("which").toString()).trimmed();
    if (which.isEmpty() || which == "premiere" || which == "premier") which = QStringLiteral("first");
    if (which == "derniere" || which == "dernier") which = QStringLiteral("last");
    if (which == "toutes" || which == "tous") which = QStringLiteral("all");
    if (op != "extremum" && which != "first" && which != "last" && which != "all") return fail(QStringLiteral("« which » : first, last ou all"));

    // Titre et en-tête
    const QString src = S ? QStringLiteral("splines") : QStringLiteral("carte de densité");
    R.title = op == "value" ? QStringLiteral("Courbe %1 : valeur à %2").arg(D, termText(T1))
            : op == "derivative" ? QStringLiteral("Courbe %1 : %3 instantanée à %2").arg(D, termText(T1), qWord(order))
            : op == "extremum" ? QStringLiteral("Courbe %1 : date de la %2").arg(D, [&] {
                  const QString w = qWord(order);
                  return sense == "abs" ? (order == 0 ? QStringLiteral("valeur la plus forte (en valeur absolue)") : QStringLiteral("%1 la plus forte").arg(w))
                       : sense == "absmin" ? QStringLiteral("%1 la plus faible (en valeur absolue)").arg(w)
                       : sense == "max" ? QStringLiteral("%1 maximale").arg(w) : QStringLiteral("%1 minimale").arg(w); }())
            : op == "plot" ? QStringLiteral("Courbe %1 : tracé (%2)").arg(D, [&] { QStringList w; for (int k : plotOrders) w << qWord(k); return w.join(QStringLiteral(", ")); }())
            : op == "rate" ? QStringLiteral("Courbe %1 : vitesse moyenne entre %2 et %3").arg(D, termText(T1), termText(T2))
            : op == "crossing" ? QStringLiteral("Courbe %1 : date où %3 = %2").arg(D, curveNum(y), Q)
            : op == "calibrate" && !(yErr > 0) ? QStringLiteral("Courbe %1 : date pour %3 = %2").arg(D, text::numAuto(y, 6), Q)
            : op == "calibrate" ? QStringLiteral("Courbe %1 : date pour %4 = %2 ± %3").arg(D, text::numAuto(y, 6), text::numAuto(yErr, 6), Q)
            : QStringLiteral("Courbe %1 : vitesse entre %1 = %2 et %1 = %3").arg(D, curveNum(y1), curveNum(y2));
    R.title += QStringLiteral(" (%1 ChronoModel)").arg(src);
    const QString userName = rq.value("name").toString().trimmed();
    R.name = !userName.isEmpty() ? userName
           : op == "value" ? QStringLiteral("%1(%2)").arg(D, T1.label)
           : op == "derivative" ? QStringLiteral("%1(%2)").arg(Q, T1.label)
           : op == "plot" ? QStringLiteral("Courbe %1").arg(D)
           : op == "extremum" ? QStringLiteral("Date du %1").arg(sense == "abs" ? QStringLiteral("maximum de |%1|").arg(Q) : sense == "absmin" ? QStringLiteral("minimum de |%1|").arg(Q)
                                                                 : sense == "max" ? QStringLiteral("maximum de %1").arg(Q) : QStringLiteral("minimum de %1").arg(Q))
           : op == "rate" ? QStringLiteral("Vitesse %1 de %2 à %3").arg(D, T1.label, T2.label)
           : op == "crossing" ? QStringLiteral("Date où %1 = %2").arg(Q, curveNum(y))
           : op == "calibrate" && !(yErr > 0) ? QStringLiteral("Date pour %1 = %2").arg(Q, text::numAuto(y, 6))
           : op == "calibrate" ? QStringLiteral("Date pour %1 = %2 ± %3").arg(Q, text::numAuto(y, 6), text::numAuto(yErr, 6))
           : QStringLiteral("De %1 = %2 à %1 = %3").arg(D, curveNum(y1), curveNum(y2));
    const QString file = S ? S->file : Mp->file;
    R.html += QStringLiteral("<p><b>Courbe de ChronoModel « %1 » — composante %2</b></p>").arg(file.toHtmlEscaped(), D);
    R.prompt += QStringLiteral("\nCourbe de ChronoModel « %1 », composante %2 : %3.\n").arg(file, D, R.title);
    if (!dimNote.isEmpty()) {
        R.html += QStringLiteral("<p><i>%1.</i></p>").arg(dimNote.toHtmlEscaped());
        R.prompt += dimNote + QStringLiteral(".\n");
    }
    if (!extremumNote.isEmpty()) {
        R.html += QStringLiteral("<p><i>%1.</i></p>").arg(extremumNote.toHtmlEscaped());
        R.prompt += extremumNote + QStringLiteral(".\n");
    }
    if (order > 0 && op != "plot") {
        const QString u = QStringLiteral("%1 = %2 de %3, en %4%5").arg(Q, order == 1 ? QStringLiteral("vitesse (dérivée)") : QStringLiteral("accélération (dérivée seconde)"),
                                                                       D, qUnit(order),
                                                                       order == 1 ? QStringLiteral(" ; pour une courbe âge-profondeur, c'est le taux d'accumulation") : QString());
        R.html += QStringLiteral("<p><i>%1.</i></p>").arg(u.toHtmlEscaped());
        R.prompt += u + QStringLiteral(".\n");
    }
    auto finish = [&]() {
        if (!levelWarning.isEmpty()) {
            R.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(levelWarning.toHtmlEscaped());
            R.prompt += QStringLiteral("Avertissement : %1.\n").arg(levelWarning);
        }
        R.ok = true;
        return R;
    };
    // Loi d'une grandeur numérique (densité lissée) : HPD, MAP, moyenne, quartiles.
    auto numericSummary = [&](const Density& d, const QString& what) {
        R.html += QStringLiteral("<p><b>%1</b></p>").arg(what.toHtmlEscaped());
        R.prompt += what + QStringLiteral(" :\n");
        appendSummary(R.html, R.prompt, d, hpdIntervals(d, 95.0), hpdIntervals(d, 68.0), densityStats(d), R.level, DateFormat::Numeric);
    };
    auto probRow = [&](const QString& label, const QString& value) {
        return QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(label.toHtmlEscaped(), value.toHtmlEscaped());
    };
    // Lignes de tableau en texte pour le modèle : « libellé : valeur ».
    auto rowsPrompt = [](QString p) {
        p.replace(QStringLiteral("</td><td>"), QStringLiteral(" : ")).replace(QStringLiteral("</tr>"), QStringLiteral("\n"));
        static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
        p.remove(tags);
        return p.replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"))
                .replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    };

    // Courbe d'une grandeur (G, G′ ou G″) et son enveloppe : médiane et intervalle de crédibilité à 95 % des tirages
    // (à queues égales : quantiles 2,5 – 97,5 %, date par date ; pas une HPD), sur une
    // grille de 240 dates de la période d'étude (ou de [tmin ; tmax]), avec P(grandeur > 0) à chaque date.
    struct Envelope { std::vector<double> t, med, lo, hi, ppos; };
    auto envelopeOf = [&](int k, double a, double b) {
        Envelope E;
        const size_t N = S->rows;
        const size_t nt = 240, stride = std::max<size_t>(1, N / 2000);
        const double sc = qScale(k);
        std::vector<double> col;
        for (size_t j = 0; j < nt; ++j) {
            const double t = a + (b - a) * double(j) / double(nt - 1);
            col.clear();
            size_t pos = 0;
            for (size_t i = 0; i < N; i += stride) {
                const double v = S->quantity(i, dim, t, k) * sc;
                if (!std::isfinite(v)) continue;
                col.push_back(v);
                if (v > 0) ++pos;
            }
            if (col.empty()) col.push_back(std::nan(""));
            std::sort(col.begin(), col.end());
            auto qt = [&](double p) { return col[std::min(col.size() - 1, size_t(p * double(col.size() - 1) + 0.5))]; };
            E.t.push_back(t);
            E.med.push_back(qt(0.5)); E.lo.push_back(qt(0.025)); E.hi.push_back(qt(0.975));
            E.ppos.push_back(double(pos) / double(std::max<size_t>(1, col.size())));
        }
        return E;
    };
    // `marks` : dates (t1, t2) et valeurs (y, y ± σ…) de la question sur cette courbe.
    auto addEnvelope = [&](int k, bool marks) {
        if (!S) return;
        const double a = op == "plot" ? wa : S->tmin, b = op == "plot" ? wb : S->tmax();
        const Envelope E = envelopeOf(k, a, b);
        QJsonArray xs, med, lo, hi;
        double gmin = infv, gmax = -infv;
        for (size_t j = 0; j < E.t.size(); ++j) {
            xs.append(dateformat::convert(E.t[j], fmt));
            med.append(E.med[j]); lo.append(E.lo[j]); hi.append(E.hi[j]);
            if (std::isfinite(E.lo[j])) gmin = std::min(gmin, E.lo[j]);
            if (std::isfinite(E.hi[j])) gmax = std::max(gmax, E.hi[j]);
        }
        const QString col = k == 0 ? QStringLiteral("#1f5080") : k == 1 ? QStringLiteral("#b5541c") : QStringLiteral("#6a3d9a");
        const QString col2 = k == 0 ? QStringLiteral("#3170b0") : k == 1 ? QStringLiteral("#e1863a") : QStringLiteral("#9b6fc4");
        QJsonArray S2{ QJsonObject{ { "name", QStringLiteral("Médiane des tirages") }, { "x", xs }, { "y", med }, { "fill", false }, { "width", 2.0 },
                                    { "color", col } },
                       QJsonObject{ { "name", QStringLiteral("Intervalle de crédibilité à 95 % (quantiles 2,5 – 97,5 %)") }, { "x", xs }, { "y", lo }, { "fill", false },
                                    { "dash", true }, { "width", 1.0 }, { "color", col2 } },
                       QJsonObject{ { "name", QString() }, { "x", xs }, { "y", hi }, { "fill", false }, { "dash", true }, { "width", 1.0 },
                                    { "color", col2 } } };
        auto vline = [&](double t, const QString& name) {
            S2.append(QJsonObject{ { "name", name }, { "x", QJsonArray{ dateformat::convert(t, fmt), dateformat::convert(t, fmt) } },
                                   { "y", QJsonArray{ gmin, gmax } }, { "dash", true }, { "width", 1.0 }, { "color", QStringLiteral("#e1572a") },
                                   { "fill", false } });
        };
        auto hline = [&](double v, const QString& name, const QString& c = QStringLiteral("#2a9d8f")) {
            S2.append(QJsonObject{ { "name", name }, { "x", QJsonArray{ dateformat::convert(a, fmt), dateformat::convert(b, fmt) } },
                                   { "y", QJsonArray{ v, v } }, { "dash", true }, { "width", 1.0 }, { "color", c },
                                   { "fill", false } });
        };
        if (k > 0 && gmin < 0 && gmax > 0) hline(0.0, QString(), QStringLiteral("#888888"));     // vitesse ou accélération nulle
        if (marks) {
            for (const Operand* o : { &T1, &T2 })                  // termes non utilisés : année NaN
                if (o->kind == Operand::Fixed && std::isfinite(o->year)) vline(o->year, QStringLiteral("t = %1").arg(o->label));
            const QString Qk = qSym(k);
            if (k == order) {                                      // valeurs de la question : sur la courbe de leur grandeur
                if (op == "crossing") hline(y, QStringLiteral("%1 = %2").arg(Qk, curveNum(y)));
                if (op == "rate_values") { hline(y1, QStringLiteral("%1 = %2").arg(Qk, curveNum(y1))); hline(y2, QStringLiteral("%1 = %2").arg(Qk, curveNum(y2))); }
                if ((op == "value" || op == "derivative") && std::isfinite(y)) hline(y, QStringLiteral("%1 = %2").arg(Qk, curveNum(y)));
                if (op == "calibrate") {
                    hline(y, QStringLiteral("%1 = %2").arg(Qk, curveNum(y)));
                    if (yErr > 0) {
                        hline(y - yErr, QStringLiteral("%1 ± %2").arg(text::numAuto(y, 6), text::numAuto(yErr, 6)));
                        hline(y + yErr, QString());
                    }
                }
            }
        }
        const QString title = k == 0 ? QStringLiteral("Courbe %1 — %2").arg(D, S->file)
                            : k == 1 ? QStringLiteral("Vitesse %1 — %2").arg(qSym(1), S->file)
                                     : QStringLiteral("Accélération %1 — %2").arg(qSym(2), S->file);
        const QString ylabel = k == 0 ? (unit.isEmpty() ? D : QStringLiteral("%1 (%2)").arg(D, unit))
                                      : QStringLiteral("%1 (%2)").arg(qSym(k), qUnit(k));
        QJsonObject plot{ { "title", title }, { "xlabel", dateformat::axisLabel(fmt) },
                          { "ylabel", ylabel }, { "fill", false }, { "yaxis", true }, { "yzero", false }, { "layout", QStringLiteral("overlay") },
                          { "series", S2 } };
        if (dateformat::isAge(fmt)) plot["xreverse"] = true;
        R.plots << plot;
    };

    // 3b. Tracé des courbes de la valeur G, de la vitesse G′ (taux d'accumulation d'une courbe âge-profondeur) et de
    //     l'accélération G″ : médiane et intervalle de crédibilité à 95 % des tirages, date par date. La fiche résume, pour chaque grandeur,
    //     les extrêmes de la médiane et, pour G′ et G″, les périodes où leur signe est établi (P > 95 % ou < 5 %).
    if (op == "plot") {
        const QLocale frl(QLocale::French);
        R.html += QStringLiteral("<p>Fenêtre : %1 → %2 ; %3 tirages ; 240 dates.</p>")
                      .arg(dateformat::format(wa, fmt), dateformat::format(wb, fmt), frl.toString(qlonglong(S->rows)));
        R.prompt += QStringLiteral("Tracé sur %1 → %2 (%3 tirages ; à chaque date, médiane et intervalle de crédibilité à 95 % des tirages, quantiles 2,5 – 97,5 %, pas une HPD).\n")
                        .arg(dateformat::format(wa, fmt), dateformat::format(wb, fmt), frl.toString(qlonglong(S->rows)));
        QString rows;
        for (int k : plotOrders) {
            const Envelope E = envelopeOf(k, wa, wb);
            size_t jmin = 0, jmax = 0;
            for (size_t j = 0; j < E.t.size(); ++j) {
                if (E.med[j] < E.med[jmin]) jmin = j;
                if (E.med[j] > E.med[jmax]) jmax = j;
            }
            const QString u = qUnit(k).isEmpty() ? QString() : QStringLiteral(" ") + qUnit(k);
            // Décimales : 2 chiffres significatifs sur la largeur typique (médiane) de l'intervalle à 95 %, ÷ 4.
            std::vector<double> wid;
            for (size_t j = 0; j < E.t.size(); ++j) if (std::isfinite(E.hi[j] - E.lo[j])) wid.push_back(E.hi[j] - E.lo[j]);
            std::nth_element(wid.begin(), wid.begin() + std::ptrdiff_t(wid.size() / 2), wid.end());
            const double typ = wid.empty() ? 0.0 : wid[wid.size() / 2] / 4.0;
            auto val = [&](double x) {
                if (typ > 0 && typ < 1e-4) return frl.toString(x, 'g', 3);     // très petites valeurs (accélération par an²)
                const dateformat::PrecisionScope ps(typ > 0 ? typ : 1.0);
                return curveNum(x);
            };
            const QString label = k == 0 ? QStringLiteral("Valeur %1").arg(D) : k == 1 ? QStringLiteral("Vitesse %1").arg(qSym(1))
                                                                                       : QStringLiteral("Accélération %1").arg(qSym(2));
            QString line = QStringLiteral("médiane de %1%3 (en %2) à %4%6 (en %5)")
                               .arg(val(E.med[jmin]), dateformat::format(E.t[jmin], fmt), u, val(E.med[jmax]),
                                    dateformat::format(E.t[jmax], fmt), u);
            if (k > 0) {
                // Périodes où le signe est établi : P(> 0) ≥ 95 % (positive) ou ≤ 5 % (négative), morceaux contigus de la grille.
                QStringList posP, negP;
                auto flush = [&](int sign, size_t j0, size_t j1) {
                    const QString iv = QStringLiteral("%1 → %2").arg(dateformat::format(E.t[j0], fmt), dateformat::format(E.t[j1], fmt));
                    (sign > 0 ? posP : negP) << iv;
                };
                int cur = 0;
                size_t start = 0;
                for (size_t j = 0; j <= E.t.size(); ++j) {
                    const int sg = j == E.t.size() ? 0 : E.ppos[j] >= 0.95 ? 1 : E.ppos[j] <= 0.05 ? -1 : 0;
                    if (sg != cur) {
                        if (cur != 0) flush(cur, start, j - 1);
                        cur = sg;
                        start = j;
                    }
                }
                const QString pw = k == 1 ? QStringLiteral("croissante") : QStringLiteral("vitesse croissante");
                const QString nw = k == 1 ? QStringLiteral("décroissante") : QStringLiteral("vitesse décroissante");
                line += QStringLiteral(" ; %1 (P ≥ 95 %) : %2 ; %3 (P ≥ 95 %) : %4")
                            .arg(pw, posP.isEmpty() ? QStringLiteral("aucune période") : posP.join(QStringLiteral(", ")),
                                 nw, negP.isEmpty() ? QStringLiteral("aucune période") : negP.join(QStringLiteral(", ")));
            }
            rows += probRow(label, line);
            addEnvelope(k, false);
        }
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
        R.prompt += rowsPrompt(rows);
        const QString note = QStringLiteral("Chaque tirage est une spline complète, évaluée comme dans ChronoModel : vitesse = dérivée G′ (pour une "
                                            "courbe âge-profondeur, taux d'accumulation), accélération = dérivée seconde G″ (nulle hors des "
                                            "Events, où la spline est prolongée en ligne droite ; elle est continue mais anguleuse aux Events). "
                                            "Les extrêmes donnés sont ceux de la courbe médiane, date par date, pas la loi d'un extrême.");
        R.html += QStringLiteral("<p><small>%1</small></p>").arg(note.toHtmlEscaped());
        R.prompt += note + QStringLiteral(" Les courbes sont DÉJÀ tracées, une par onglet.\n");
        R.posterior = true;
        return finish();
    }

    // 4. Date d'une mesure y ± σ sur la courbe estimée (« calibration » sur la courbe a posteriori) :
    //    f(t) ∝ (1/n) Σ_i φ_σ(y − G_i(t)), a priori uniforme sur la période ; l'incertitude de la courbe est celle des
    //    tirages (pas d'erreur « gaussienne » ajoutée). Sur chaque pas de la grille, chaque courbe est prise linéaire et la
    //    vraisemblance y est MOYENNÉE exactement : |Φ((y − G0)/σ) − Φ((y − G1)/σ)| / |G1 − G0|. Le résultat reste juste
    //    quand σ est très petit devant la variation de la courbe sur un pas (limite σ → 0 : somme des passages par y,
    //    pondérés par 1/|G′|), là où une évaluation ponctuelle ne verrait que quelques courbes et donnerait des pics.
    //    Carte : moyenne de la même vraisemblance sur la colonne (calcul approché).
    if (op == "calibrate") {
        const double a = S ? wa : Mp->t.front(), b = S ? wb : Mp->t.back();
        const int nt = int(std::clamp((b - a), 400.0, 3000.0));
        Density d;
        d.step = (b - a) / double(nt);
        d.t.resize(size_t(nt));
        d.v.assign(size_t(nt), 0.0);
        for (int k = 0; k < nt; ++k) d.t[size_t(k)] = a + d.step * (k + 0.5);     // milieux des pas
        // Carte de G′ : en unités par an ; la mesure (par `per` ans) y est ramenée.
        const double sigMap = Mp && Mp->y.size() > 1 ? 0.5 * (Mp->y.back() - Mp->y.front()) / double(Mp->y.size() - 1) : 0.0;
        const double sig = exactValue ? sigMap : (S ? yErr : yErr / scale);   // carte et valeur exacte : demi-case de la carte
        const double yMap = S ? y : y / scale;
        std::vector<double> crossT, crossSlope;                    // passages des courbes par y (lissage)
        double smoothing = 0.0;
        const double inv2 = sig > 0 ? 1.0 / (2.0 * sig * sig) : 0.0;
        size_t used = 0;
        if (S) {
            const size_t stride = std::max<size_t>(1, size_t(double(S->rows) * nt / 3.0e7));
            constexpr double invSqrt2 = 0.70710678118654752440;
            std::vector<double> g(size_t(nt) + 1);
            for (size_t i = 0; i < S->rows; i += stride, ++used) {
                for (int k = 0; k <= nt; ++k) g[size_t(k)] = S->quantity(i, dim, a + d.step * k, order) * scale;
                for (int k = 0; k < nt; ++k) {
                    const double g0 = g[size_t(k)], g1 = g[size_t(k) + 1], dg = std::abs(g1 - g0);
                    if (dg > 0 && y >= std::min(g0, g1) && y < std::max(g0, g1) && crossT.size() < 400000) {   // passage par y
                        crossT.push_back(a + d.step * (k + (y - g0) / (g1 - g0)));
                        crossSlope.push_back(dg / d.step);
                    }
                    double c;
                    if (exactValue) {                                       // passage exact par y : 1/|ΔG| si y est dans le pas
                        c = dg > 0 && y >= std::min(g0, g1) && y < std::max(g0, g1) ? 1.0 / dg : 0.0;
                    } else if (dg > 1e-9 * yErr) {
                        // moyenne de φ_σ(y − G) sur le pas : (Φ(z0) − Φ(z1)) / ΔG, Φ par erfc
                        const double z0 = (y - g0) / yErr, z1 = (y - g1) / yErr;
                        c = std::abs(std::erfc(-z0 * invSqrt2) - std::erfc(-z1 * invSqrt2)) * 0.5 / dg;
                    } else {
                        const double r = y - 0.5 * (g0 + g1);
                        c = std::exp(-r * r * inv2) / (yErr * 2.5066282746310002);
                    }
                    d.v[size_t(k)] += c;
                }
            }
            for (double& v : d.v) v /= double(std::max<size_t>(used, 1));
            // Lissage : avec une erreur petite devant la variation de la courbe, la densité n'est qu'un histogramme des passages
            // des courbes tirées par y (bruit d'échantillonnage). On la lisse comme une densité de tirages (noyau gaussien,
            // largeur de Sheather-Jones h sur les dates des passages), en ne gardant que ce que l'erreur ne lisse pas déjà :
            // h_ajout = √(h² − σ_t²), σ_t = σ / |pente| médiane de la courbe aux passages.
            if (crossT.size() >= 20) {
                const double h = kernelBandwidth(crossT, Bandwidth::SheatherJones);
                std::vector<double> sl = crossSlope;
                std::nth_element(sl.begin(), sl.begin() + std::ptrdiff_t(sl.size() / 2), sl.end());
                const double slope = sl[sl.size() / 2];
                const double sigT = slope > 0 && yErr > 0 ? yErr / slope : 0.0;
                const double add = h > sigT ? std::sqrt(h * h - sigT * sigT) : 0.0;
                if (std::isfinite(add) && add > 0.5 * d.step) {
                    smoothing = add;
                    const int w = int(std::ceil(4.0 * add / d.step));
                    std::vector<double> ker(size_t(2 * w + 1));
                    for (int j = -w; j <= w; ++j) ker[size_t(j + w)] = std::exp(-0.5 * std::pow(j * d.step / add, 2));
                    std::vector<double> sm(d.v.size(), 0.0);
                    for (int k = 0; k < nt; ++k) {                       // bords : noyau renormalisé (pas de fuite hors de la fenêtre)
                        double sum = 0.0, wsum = 0.0;
                        for (int j = -w; j <= w; ++j) {
                            const int kk = k + j;
                            if (kk < 0 || kk >= nt) continue;
                            sum += ker[size_t(j + w)] * d.v[size_t(kk)];
                            wsum += ker[size_t(j + w)];
                        }
                        sm[size_t(k)] = wsum > 0 ? sum / wsum : 0.0;
                    }
                    d.v = sm;
                }
            }
        } else {
            const CurveMap& M = *Mp;
            for (int k = 0; k < nt; ++k) {
                const double t = d.t[size_t(k)];
                const size_t c1 = std::min(M.t.size() - 1, size_t(std::upper_bound(M.t.begin(), M.t.end(), t) - M.t.begin()));
                const size_t c0 = c1 > 0 ? c1 - 1 : 0;
                const double w = c1 > c0 && M.t[c1] > M.t[c0] ? (t - M.t[c0]) / (M.t[c1] - M.t[c0]) : 0.0;
                double sum = 0.0, tot = 0.0;
                for (size_t r = 0; r < M.y.size(); ++r) {
                    const double n = (1.0 - w) * M.at(c0, r) + w * M.at(c1, r), z = yMap - M.y[r];
                    sum += n * std::exp(-z * z * inv2);
                    tot += n;
                }
                d.v[size_t(k)] = tot > 0 ? sum / tot : 0.0;
            }
        }
        double area = 0.0, peak = 0.0;
        for (size_t k = 0; k + 1 < d.v.size(); ++k) area += 0.5 * (d.v[k] + d.v[k + 1]) * d.step;
        for (double v : d.v) peak = std::max(peak, v);
        if (!(area > 0) || peak < 1e-12)
            return fail(QStringLiteral("la courbe ne passe jamais près de %1 = %2 ± %3 sur %4 → %5")
                            .arg(Q, text::numAuto(y, 6), text::numAuto(yErr, 6), dateformat::format(a, fmt), dateformat::format(b, fmt)));
        for (double& v : d.v) v /= area;
        const QString qu = order > 0 ? QStringLiteral(" %1").arg(qUnit(order)) : QString();
        const QString meas = exactValue ? QStringLiteral("%1 = %2%3 (valeur exacte)").arg(Q, text::numAuto(y, 6), qu)
                                        : QStringLiteral("%1 = %2 ± %3%4").arg(Q, text::numAuto(y, 6), text::numAuto(yErr, 6), qu);
        R.html += QStringLiteral("<p><b>Date pour %1</b></p>").arg(meas.toHtmlEscaped());
        R.prompt += QStringLiteral("Date pour %1%2, sur la courbe estimée par ChronoModel :\n").arg(meas, exactValue ? QString() : QStringLiteral(" (1σ)"));
        const DensityStats dst = densityStats(d);
        appendSummary(R.html, R.prompt, d, hpdIntervals(d, 95.0), hpdIntervals(d, 68.0), dst, R.level, fmt);
        // Écart-type et queues : une petite part de probabilité loin du mode (passages de la courbe ailleurs, surtout hors des
        // Events, où la courbe est prolongée en ligne droite) gonfle l'écart-type sans changer les HPD ni les quartiles.
        {
            QStringList notes;
            const double iqrSd = (dst.q3 - dst.q1) / 1.349;           // écart-type d'une gaussienne de même écart interquartile
            if (iqrSd > 0 && dst.sd > 1.5 * iqrSd) {
                double outer = 0.0;
                for (size_t k = 0; k < d.t.size(); ++k)
                    if (std::abs(d.t[k] - dst.median) > 3.0 * iqrSd) outer += d.v[k] * d.step;
                notes << QStringLiteral("l'écart-type (%1) est gonflé par des queues lointaines : %2 de la probabilité est à plus de %3 de la "
                                        "médiane (la partie centrale correspond à un écart-type d'environ %4) ; résumer par les HPD et les quartiles")
                             .arg(dateformat::spread(dst.sd, fmt), percentText(outer), dateformat::spread(3.0 * iqrSd, fmt),
                                  dateformat::spread(iqrSd, fmt));
            }
            if (S) {                                                   // étendue des Events (nœuds) : médianes du premier et du dernier
                std::vector<double> first, last;
                const size_t st = std::max<size_t>(1, S->rows / 4000);
                for (size_t i = 0; i < S->rows; i += st) {
                    first.push_back(S->theta[i * size_t(S->knots)]);
                    last.push_back(S->theta[i * size_t(S->knots) + size_t(S->knots) - 1]);
                }
                std::nth_element(first.begin(), first.begin() + std::ptrdiff_t(first.size() / 2), first.end());
                std::nth_element(last.begin(), last.begin() + std::ptrdiff_t(last.size() / 2), last.end());
                const double k1 = first[first.size() / 2], kn = last[last.size() / 2];
                double out = 0.0;
                for (size_t k = 0; k < d.t.size(); ++k) if (d.t[k] < k1 || d.t[k] > kn) out += d.v[k] * d.step;
                if (out > 0.01)
                    notes << QStringLiteral("%1 de la probabilité est hors de l'étendue des Events (%2 → %3), là où la courbe est prolongée en "
                                            "ligne droite : cette partie dépend de l'extrapolation, à interpréter avec prudence (tmin, tmax "
                                            "pour restreindre la fenêtre)")
                                 .arg(percentText(out), dateformat::format(k1, fmt), dateformat::format(kn, fmt));
            }
            for (const QString& n : notes) {
                R.html += QStringLiteral("<p><i>⚠ %1.</i></p>").arg(n.toHtmlEscaped());
                R.prompt += QStringLiteral("Avertissement : %1.\n").arg(n);
            }
        }
        R.series.append(plotSeries(R.name, d, fmt));
        addEnvelope(order, true);
        if (order > 0) addEnvelope(0, true);
        QString how = S && exactValue
            ? QStringLiteral("valeur exacte : chaque passage d'une courbe tirée par la valeur compte en proportion du temps qu'elle y passe "
                             "(1/|pente|), sur %1 courbes tirées a posteriori évaluées comme dans ChronoModel").arg(QLocale(QLocale::French).toString(qlonglong(used)))
            : S
            ? QStringLiteral("vraisemblance gaussienne de la mesure (σ = %1), moyennée sur chaque pas de temps, puis sur %2 courbes tirées a "
                             "posteriori, chaque courbe évaluée comme dans ChronoModel : l'incertitude de la courbe est prise en compte par les tirages")
                  .arg(text::numAuto(yErr, 6), QLocale(QLocale::French).toString(qlonglong(used)))
            : QStringLiteral("vraisemblance gaussienne de la mesure (σ = %1) moyennée sur la carte de densité (calcul approché : les courbes "
                             "hors de la fenêtre de la carte ne sont pas comptées)").arg(text::numAuto(sig, 6));
        if (smoothing > 0) {
            how += QStringLiteral(" ; densité lissée (noyau gaussien de %1, largeur de Sheather-Jones sur les dates des passages des courbes "
                                  "par la valeur, diminuée de l'étalement déjà dû à l'erreur)").arg(dateformat::spread(smoothing, fmt));
        }
        R.html += QStringLiteral("<p><small>Densité de la date : %1 ; a priori uniforme sur %2 → %3, pas de %4. Comme une calibration "
                                 "archéomagnétique, mais sur la courbe a posteriori de votre modèle ; plusieurs morceaux de HPD signalent "
                                 "plusieurs passages de la courbe par la valeur.</small></p>")
                      .arg(how.toHtmlEscaped(), dateformat::format(a, fmt), dateformat::format(b, fmt), dateformat::spread(d.step, fmt));
        R.prompt += QStringLiteral("Méthode : %1 ; a priori uniforme sur %2 → %3. Le graphique de la densité (avec les autres dates) et celui "
                                   "de la courbe sont DÉJÀ tracés.\n").arg(how, dateformat::format(a, fmt), dateformat::format(b, fmt));
        return finish();
    }

    // 4a. Carte de densité : loi de G (ou G′) dans la colonne de la date t, interpolée entre les deux colonnes voisines.
    if (!S) {
        if (T1.kind != Operand::Fixed)
            return fail(QStringLiteral("avec une carte de densité, la date doit être une année fixe (une colonne des traces ou une datation "
                                       "demande la trace des splines, Curve_X_Splines.csv)"));
        const CurveMap& M = *Mp;
        const double t = T1.year;
        if (t < M.t.front() || t > M.t.back())
            return fail(QStringLiteral("la date %1 est hors de la carte (%2 → %3)")
                            .arg(T1.label, dateformat::format(M.t.front(), fmt), dateformat::format(M.t.back(), fmt)));
        const size_t c1 = std::min(M.t.size() - 1, size_t(std::upper_bound(M.t.begin(), M.t.end(), t) - M.t.begin()));
        const size_t c0 = c1 > 0 ? c1 - 1 : 0;
        const double w = c1 > c0 && M.t[c1] > M.t[c0] ? (t - M.t[c0]) / (M.t[c1] - M.t[c0]) : 0.0;
        const size_t ny = M.y.size();
        Density d;
        d.t = M.y;
        d.v.assign(ny, 0.0);
        d.step = ny > 1 ? (M.y.back() - M.y.front()) / double(ny - 1) : 1.0;
        double total = 0.0;
        for (size_t r = 0; r < ny; ++r) { d.v[r] = (1.0 - w) * M.at(c0, r) + w * M.at(std::max(c0, c1), r); total += d.v[r]; }
        if (!(total > 0)) return fail(QStringLiteral("aucun tirage dans la carte à la date %1").arg(T1.label));
        for (double& v : d.v) v /= total * d.step;
        const QString qty = op == "value" ? QStringLiteral("Loi de %1 à la date %2").arg(D, T1.label)
                                          : QStringLiteral("Loi de la vitesse %1′ à la date %2 (unités de %1 par an)").arg(D, T1.label);
        numericSummary(d, qty);
        QString rows;
        const CdfTable C(d);
        if (std::isfinite(y)) {
            rows += probRow(QStringLiteral("P(%1 < %2)").arg(D, curveNum(y)), percentText(C.at(y)));
            rows += probRow(QStringLiteral("P(%1 > %2)").arg(D, curveNum(y)), percentText(1.0 - C.at(y)));
            rows += probRow(QStringLiteral("Densité en %1 = %2").arg(D, curveNum(y)), text::numAuto(densityAt(d, y), 6));
        }
        if (std::isfinite(ymin) && std::isfinite(ymax))
            rows += probRow(QStringLiteral("P(%1 ≤ %2 ≤ %3)").arg(curveNum(std::min(ymin, ymax)), D, curveNum(std::max(ymin, ymax))),
                            percentText(C.at(std::max(ymin, ymax)) - C.at(std::min(ymin, ymax))));
        if (!rows.isEmpty()) {
            R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
            R.prompt += rowsPrompt(rows);
        }
        const double edge = (d.v.front() + d.v.back()) * d.step;
        R.html += QStringLiteral("<p><small>Lecture de la carte de densité exportée par ChronoModel (%1 tirages dans cette colonne, "
                                 "interpolation linéaire entre les dates %2 et %3 de la grille, histogramme au pas de %4). Les tirages "
                                 "hors de la fenêtre de la carte (%5 → %6) n'y sont pas comptés : la loi est celle des valeurs dans la "
                                 "fenêtre.%7 %8</small></p>")
                      .arg(fr.toString(qlonglong(std::llround(total))), dateformat::format(M.t[c0], fmt), dateformat::format(M.t[c1], fmt),
                           curveNum(d.step), curveNum(M.y.front()), curveNum(M.y.back()),
                           edge > 0.01 ? QStringLiteral(" ⚠ Des tirages touchent le bord de la fenêtre : la loi est sans doute tronquée.") : QString(),
                           M.notes.isEmpty() ? QString() : QStringLiteral("Lecture : ") + M.notes.join(QStringLiteral(" ; ")).toHtmlEscaped() + '.');
        R.prompt += QStringLiteral("Calcul approché sur la carte de densité (%1 tirages dans la fenêtre de la carte, de %2 à %3 ; les valeurs "
                                   "hors de la fenêtre ne sont pas comptées). Pour un calcul exact, joindre Curve_X_Splines.csv.\n")
                        .arg(fr.toString(qlonglong(std::llround(total))), curveNum(M.y.front()), curveNum(M.y.back()));
        R.plots << valuePlot(qty, op == "value" ? D : QStringLiteral("%1′ (unités par an)").arg(D), R.name, d, R.level, y, std::nan(""), D);
        R.posterior = true;
        return finish();
    }

    // 4b. Trace des splines : tirage par tirage
    const size_t N = S->rows;
    QString chainsFrom;
    const std::vector<size_t> chainStarts = curveChains(*S, &chainsFrom);
    std::mt19937 rng(12345);
    auto samplesOf = [&](const Operand& o, std::vector<double>& out, QString& e) {
        if (o.kind == Operand::Fixed) { out.assign(N, o.year); return true; }
        if (o.kind == Operand::Column) {
            if (o.set->rows() != N) {
                e = QStringLiteral("la colonne « %1 » (%2 tirages) ne vient pas du même calcul que la trace des splines (%3 tirages)")
                        .arg(o.label).arg(o.set->rows()).arg(N);
                return false;
            }
            out = *o.col;
            return true;
        }
        out = sampleDensity(o.d, N, rng);
        return true;
    };
    std::vector<double> t1s, t2s;
    if (op == "value" || op == "derivative" || op == "rate") {
        if (!samplesOf(T1, t1s, err)) return fail(err);
        if (op == "rate" && !samplesOf(T2, t2s, err)) return fail(err);
    }
    for (const Operand* o : { &T1, &T2 }) {
        if (o->kind != Operand::Independent) continue;
        const QString w = QStringLiteral("Date incertaine « %1 » : tirée à chaque tirage selon sa loi, indépendamment de la courbe ; le "
                                         "résultat combine l'incertitude de la date et celle de la courbe. Si c'est la date d'un Event du "
                                         "modèle, donner plutôt son nom (trace jointe), sinon elle compterait deux fois.").arg(o->label);
        R.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());
        R.prompt += QStringLiteral("Avertissement : %1\n").arg(w);
    }
    for (const Operand* o : { &T1, &T2 })
        if (o->kind == Operand::Fixed && std::isfinite(o->year) && (o->year < S->tmin || o->year > S->tmax())) {
            const QString w = QStringLiteral("la date %1 est hors de la période d'étude (%2 → %3) : la spline y est prolongée en ligne droite")
                                  .arg(o->label, dateformat::format(S->tmin, fmt), dateformat::format(S->tmax(), fmt));
            R.html += QStringLiteral("<p><i>⚠ %1.</i></p>").arg(w.toHtmlEscaped());
            R.prompt += QStringLiteral("Avertissement : %1.\n").arg(w);
        }

    // Grandeur par tirage (NaN si non définie dans ce tirage), et par chaîne pour la convergence.
    std::vector<double> q(N, std::nan("")), q2;                  // q2 : durée (rate_values)
    size_t multi = 0, reached = 0, totalCross = 0;
    if (op == "rate_values") q2.assign(N, std::nan(""));
    std::vector<double> allCross;                               // crossing / which = all
    for (size_t i = 0; i < N && op != "extremum"; ++i) {
        double G, GP;
        if (op == "value") { S->eval(i, dim, t1s[i], G, GP); q[i] = G; }
        else if (op == "derivative") q[i] = S->quantity(i, dim, t1s[i], order) * scale;
        else if (op == "rate") {
            const double dt = t2s[i] - t1s[i];
            if (dt != 0.0) q[i] = (S->value(i, dim, t2s[i]) - S->value(i, dim, t1s[i])) / dt * per;
        } else if (op == "crossing") {
            const std::vector<double> c = curves::crossings(*S, i, dim, y / scale, wa, wb, 2000, order);
            if (c.empty()) continue;
            ++reached;
            totalCross += c.size();
            if (c.size() > 1) ++multi;
            q[i] = which == "last" ? c.back() : c.front();
            if (which == "all") allCross.insert(allCross.end(), c.begin(), c.end());
        } else {
            const std::vector<double> c1 = curves::crossings(*S, i, dim, y1, wa, wb);
            if (c1.empty()) continue;
            const std::vector<double> c2 = curves::crossings(*S, i, dim, y2, c1.front(), wb);
            if (c1.size() > 1 || c2.size() > 1) ++multi;
            if (c2.empty()) continue;
            ++reached;
            const double dt = c2.front() - c1.front();
            if (dt > 0) { q2[i] = dt; q[i] = (y2 - y1) / dt * per; }
        }
    }
    auto finiteOf = [](const std::vector<double>& v) {
        std::vector<double> o;
        for (double x : v) if (std::isfinite(x)) o.push_back(x);
        return o;
    };
    auto convergenceOf = [&](const std::vector<double>& v) -> QString {
        if (chainStarts.size() < 2) return {};
        std::vector<std::vector<double>> byChain(chainStarts.size());
        size_t ch = 0;
        for (size_t i = 0; i < N; ++i) {
            while (ch + 1 < chainStarts.size() && i >= chainStarts[ch + 1]) ++ch;
            if (std::isfinite(v[i])) byChain[ch].push_back(v[i]);
        }
        size_t len = std::numeric_limits<size_t>::max();
        for (const auto& c : byChain) len = std::min(len, c.size());
        if (len < 4) return {};
        for (auto& c : byChain) c.resize(len);
        const mcmcdiag::RhatEss re = mcmcdiag::computeRhatAndEss(byChain);
        const double M = double(byChain.size());
        const bool good = re.rhat < mcmcdiag::kRhatGood && std::min(re.bulkEss, re.tailEss) >= mcmcdiag::kEssGoodPerChain * M;
        const bool warnc = re.rhat < mcmcdiag::kRhatWarning && std::min(re.bulkEss, re.tailEss) >= mcmcdiag::kEssWarningPerChain * M;
        return QStringLiteral("%1 (R̂ = %2 ; ESS bulk = %3 ; ESS tail = %4)")
            .arg(good ? QStringLiteral("✅ satisfaisante") : warnc ? QStringLiteral("⚠️ à surveiller") : QStringLiteral("❌ insuffisante"),
                 rhatText(re.rhat), essText(re.bulkEss), essText(re.tailEss));
    };
    // 4c. Date de l'extremum (« variation la plus forte », maximum, minimum) : pour chaque courbe tirée, date où la grandeur
    //     (G, G′ ou G″) atteint son extremum sur l'étendue de SES Events (premier → dernier nœud de ce tirage), intersectée
    //     avec [tmin ; tmax] : hors des nœuds la spline est une droite (G′ constante, G″ nulle), l'extremum n'y a pas de date
    //     propre. Grille de 400 pas, puis section dorée sur les deux pas voisins. On obtient la loi a posteriori de la date
    //     et celle de la valeur extrême ; un extremum au bord de l'intervalle est compté à part.
    if (op == "extremum") {
        const size_t stride = std::max<size_t>(1, N / 20000);
        const int ng = 400;
        std::vector<double> dates(N, std::nan("")), vals, datesF;
        size_t used = 0, atEdge = 0, positive = 0;
        auto score = [&](double v) { return sense == "abs" ? std::abs(v) : sense == "absmin" ? -std::abs(v) : sense == "max" ? v : -v; };
        for (size_t i = 0; i < N; i += stride) {
            const double k1 = S->theta[i * size_t(S->knots)], kn = S->theta[i * size_t(S->knots) + size_t(S->knots) - 1];
            double a = std::max(wa, k1), b = std::min(wb, kn);
            if (S->knots < 2 || !(b > a)) { a = wa; b = wb; }       // nœuds hors de la fenêtre : toute la fenêtre
            const double dt = (b - a) / ng;
            int kb = 0;
            double best = -infv;
            for (int k = 0; k <= ng; ++k) {
                const double v = score(S->quantity(i, dim, a + dt * k, order));
                if (std::isfinite(v) && v > best) { best = v; kb = k; }
            }
            if (!std::isfinite(best)) continue;
            double lo = a + dt * std::max(0, kb - 1), hi = a + dt * std::min(ng, kb + 1);
            constexpr double gr = 0.61803398874989485;
            double c = hi - gr * (hi - lo), e = lo + gr * (hi - lo);
            double fc = score(S->quantity(i, dim, c, order)), fe = score(S->quantity(i, dim, e, order));
            for (int it = 0; it < 40 && hi - lo > 1e-6 * (b - a); ++it) {
                if (fc > fe) { hi = e; e = c; fe = fc; c = hi - gr * (hi - lo); fc = score(S->quantity(i, dim, c, order)); }
                else { lo = c; c = e; fc = fe; e = lo + gr * (hi - lo); fe = score(S->quantity(i, dim, e, order)); }
            }
            double te = 0.5 * (lo + hi);
            const double fa = score(S->quantity(i, dim, a, order)), fb = score(S->quantity(i, dim, b, order)), fm = score(S->quantity(i, dim, te, order));
            if (fa >= fm && fa >= fb) te = a;
            else if (fb >= fm && fb > fa) te = b;
            const double v = S->quantity(i, dim, te, order) * scale;
            ++used;
            if (te <= a + 1e-3 * (b - a) || te >= b - 1e-3 * (b - a)) ++atEdge;   // à une extrémité (premier ou dernier Event, ou fenêtre)
            dates[i] = te;
            datesF.push_back(te);
            vals.push_back(v);
            if (v > 0) ++positive;
        }
        if (datesF.size() < 10) return fail(QStringLiteral("moins de 10 tirages exploitables"));
        const QString what = sense == "abs" ? QStringLiteral("%1 la plus forte en valeur absolue (|%2| maximale)").arg(qWord(order), Q)
                           : sense == "absmin" ? QStringLiteral("%1 la plus faible en valeur absolue (|%2| minimale)").arg(qWord(order), Q)
                           : sense == "max" ? QStringLiteral("%1 maximale (%2 maximale)").arg(qWord(order), Q)
                                            : QStringLiteral("%1 minimale (%2 minimale)").arg(qWord(order), Q);
        double hd = 0.0;
        Bandwidth ruleD = Bandwidth::SheatherJones;
        const Density dd = kernelDensity(datesF, -infv, infv, Bandwidth::SheatherJones, &hd, &ruleD);
        if (!dd.valid()) return fail(QStringLiteral("toutes les dates d'extremum sont identiques : densité impossible"));
        R.html += QStringLiteral("<p><b>Date de la %1</b></p>").arg(what.toHtmlEscaped());
        R.prompt += QStringLiteral("Date de la %1, dans chaque courbe tirée, sur l'étendue de ses Events :\n").arg(what);
        appendSummary(R.html, R.prompt, dd, hpdIntervals(dd, 95.0), hpdIntervals(dd, 68.0), densityStats(dd), R.level, fmt);
        R.series.append(plotSeries(R.name, dd, fmt));
        const Density dv = kernelDensity(vals, -infv, infv, Bandwidth::SheatherJones);
        if (dv.valid()) {
            numericSummary(dv, QStringLiteral("Valeur de %1 à cette date, en %2").arg(Q, qUnit(order).isEmpty() ? QStringLiteral("unités de %1").arg(D) : qUnit(order)));
            R.plots << valuePlot(QStringLiteral("%1 à l'extremum — %2").arg(Q, S->file), QStringLiteral("%1 (%2)").arg(Q, qUnit(order)), R.name, dv, R.level,
                                 order > 0 ? 0.0 : std::nan(""));
        }
        QString rows;
        const double nIn = double(datesF.size());
        rows += probRow(QStringLiteral("Tirages dont l'extremum est à une extrémité (premier ou dernier Event, ou bord de [tmin ; tmax])"),
                        percentText(double(atEdge) / double(std::max<size_t>(used, 1))));
        if (order > 0 && sense == "abs")
            rows += probRow(QStringLiteral("P(la variation la plus forte est une %1)").arg(order == 1 ? QStringLiteral("croissance (%1 > 0)").arg(Q)
                                                                                                  : QStringLiteral("accélération positive (%1 > 0)").arg(Q)),
                            percentText(double(positive) / nIn));
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
        R.prompt += rowsPrompt(rows);
        if (atEdge > used / 4) {
            const QString w = QStringLiteral("dans %1 des tirages l'extremum est à une extrémité (premier ou dernier Event, ou bord de la "
                                             "fenêtre) : la date est alors celle de cette extrémité, pas d'un pic intérieur ; au-delà des "
                                             "Events la spline est prolongée en ligne droite (vitesse constante, accélération nulle)")
                                  .arg(percentText(double(atEdge) / double(used)));
            R.html += QStringLiteral("<p><i>⚠ %1.</i></p>").arg(w.toHtmlEscaped());
            R.prompt += QStringLiteral("Avertissement : %1.\n").arg(w);
        }
        addEnvelope(order, false);
        if (order > 0) addEnvelope(0, false);
        const QString conv = convergenceOf(dates);
        const QString chains = chainStarts.size() > 1
            ? QStringLiteral("%1 chaînes, repérées dans « %2 »").arg(chainStarts.size()).arg(chainsFrom)
            : QStringLiteral("chaînes non séparées : joindre aussi Chain_all_Events.csv du même calcul pour la convergence");
        const QString how = QStringLiteral("pour chacune des %1 courbes tirées a posteriori (évaluées comme dans ChronoModel), date où %2 est "
                                           "extrême sur l'étendue de ses Events (premier → dernier nœud de ce tirage%3), grille de 400 pas "
                                           "puis section dorée ; densité de ces dates lissée par noyau gaussien (h = %4, %5) ; %6")
                                .arg(fr.toString(qlonglong(used)), sense == "abs" || sense == "absmin" ? QStringLiteral("|%1|").arg(Q) : Q,
                                     (wa > S->tmin || wb < S->tmax()) ? QStringLiteral(", dans %1 → %2").arg(dateformat::format(wa, fmt), dateformat::format(wb, fmt)) : QString(),
                                     dateformat::spread(hd, fmt), ruleD == Bandwidth::SheatherJones ? QStringLiteral("Sheather-Jones") : QStringLiteral("Silverman"),
                                     chains);
        R.html += QStringLiteral("<p><small>Méthode : %1%2. C'est la loi de la date de l'extremum de chaque courbe, pas la date de "
                                 "l'extremum de la courbe médiane ; plusieurs morceaux de HPD signalent plusieurs pics comparables.</small></p>")
                      .arg(how.toHtmlEscaped(), conv.isEmpty() ? QString() : QStringLiteral(" ; convergence de la date : %1").arg(conv.toHtmlEscaped()));
        R.prompt += QStringLiteral("Méthode : %1.%2 C'est la loi de la date de l'extremum de chaque courbe tirée, pas celle de la courbe médiane. "
                                   "Les graphiques (densité de la date, loi de la valeur extrême, courbes médianes et leurs intervalles de crédibilité à 95 %) "
                                   "sont DÉJÀ tracés.\n")
                        .arg(how, conv.isEmpty() ? QString() : QStringLiteral(" Convergence de la date : %1.").arg(conv));
        R.posterior = true;
        return finish();
    }
    const std::vector<double> qf = finiteOf(op == "crossing" && which == "all" ? allCross : q);
    if (op == "crossing" || op == "rate_values") {
        const double pr = double(reached) / double(N);
        const QString what = op == "crossing" ? QStringLiteral("P(la courbe atteint %1 = %2 entre %3 et %4)").arg(Q, curveNum(y))
                                                    .arg(dateformat::format(wa, fmt), dateformat::format(wb, fmt))
                                              : QStringLiteral("P(la courbe va de %1 = %2 à %1 = %3 entre %4 et %5)").arg(D, curveNum(y1), curveNum(y2))
                                                    .arg(dateformat::format(wa, fmt), dateformat::format(wb, fmt));
        QString rows = probRow(what, probabilityText(pr, double(N)));
        if (multi > 0)
            rows += probRow(op == "crossing" ? QStringLiteral("Tirages qui atteignent la valeur plusieurs fois") : QStringLiteral("Tirages qui passent plusieurs fois par y1 ou y2"),
                            percentText(double(multi) / double(N)));
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
        R.prompt += QStringLiteral("%1 : %2.%3\n").arg(what, probabilityText(pr, double(N)),
                        multi > 0 ? QStringLiteral(" %1 des tirages passent plusieurs fois par la valeur (courbe non monotone).")
                                        .arg(percentText(double(multi) / double(N))) : QString());
    }
    if (qf.size() < 10) {
        if (op == "crossing" || op == "rate_values")
            return fail(QStringLiteral("la courbe n'atteint pas %1 dans moins de 10 tirages : valeur hors de la courbe sur %2 → %3")
                            .arg(op == "crossing" ? QStringLiteral("%1 = %2").arg(Q, curveNum(y)) : QStringLiteral("les deux valeurs"),
                                 dateformat::format(wa, fmt), dateformat::format(wb, fmt)));
        return fail(QStringLiteral("moins de 10 tirages exploitables"));
    }
    double h = 0.0;
    Bandwidth rule = Bandwidth::SheatherJones;
    const Density d = kernelDensity(qf, -infv, infv, Bandwidth::SheatherJones, &h, &rule);
    if (!d.valid()) return fail(QStringLiteral("tous les tirages ont la même valeur : densité impossible"));
    const DensityStats st = densityStats(d);
    QString rows;
    double marker = std::nan(""), marker2 = std::nan("");
    if (op == "value") {
        numericSummary(d, QStringLiteral("Loi de %1 à %2").arg(D, termText(T1)));
        size_t lo = 0, in = 0;
        if (std::isfinite(y)) {
            for (double x : qf) if (x < y) ++lo;
            rows += probRow(QStringLiteral("P(%1 < %2)").arg(D, curveNum(y)), percentText(double(lo) / double(qf.size())));
            rows += probRow(QStringLiteral("P(%1 > %2)").arg(D, curveNum(y)), percentText(1.0 - double(lo) / double(qf.size())));
            rows += probRow(QStringLiteral("Densité en %1 = %2").arg(D, curveNum(y)), text::numAuto(densityAt(d, y), 6));
            marker = y;
        }
        if (std::isfinite(ymin) && std::isfinite(ymax)) {
            const double a = std::min(ymin, ymax), b = std::max(ymin, ymax);
            for (double x : qf) if (x >= a && x <= b) ++in;
            rows += probRow(QStringLiteral("P(%1 ≤ %2 ≤ %3)").arg(curveNum(a), D, curveNum(b)), percentText(double(in) / double(qf.size())));
            marker = a;
            marker2 = b;
        }
    } else if (op == "derivative" || op == "rate") {
        numericSummary(d, op == "derivative" && order == 2 ? QStringLiteral("Accélération instantanée %1 à %2, en %3").arg(Q, termText(T1), unitAcc)
                        : op == "derivative" ? QStringLiteral("Vitesse instantanée %1′ à %2, en %3").arg(D, termText(T1), unitRate)
                                             : QStringLiteral("Vitesse moyenne entre %1 et %2, (%3(t2) − %3(t1)) / (t2 − t1), en %4")
                                                   .arg(termText(T1), termText(T2), D, unitRate));
        size_t pos = 0;
        for (double x : qf) if (x > 0) ++pos;
        if (order == 2) {
            rows += probRow(QStringLiteral("P(%1 > 0 : vitesse croissante, courbe convexe)").arg(Q), percentText(double(pos) / double(qf.size())));
            rows += probRow(QStringLiteral("P(%1 < 0 : vitesse décroissante, courbe concave)").arg(Q), percentText(1.0 - double(pos) / double(qf.size())));
        } else {
            rows += probRow(QStringLiteral("P(%1 croissante)").arg(D), percentText(double(pos) / double(qf.size())));
            rows += probRow(QStringLiteral("P(%1 décroissante)").arg(D), percentText(1.0 - double(pos) / double(qf.size())));
        }
        marker = 0.0;
        // Seuil de vitesse (« y » ou « speed ») : P(vitesse > seuil), et P(|vitesse| > seuil) pour un seuil positif.
        const double v = std::isfinite(curveNumber(rq, { "speed", "threshold", "seuil" })) ? curveNumber(rq, { "speed", "threshold", "seuil" }) : y;
        if (std::isfinite(v)) {
            size_t above = 0, absAbove = 0;
            for (double x : qf) { if (x > v) ++above; if (std::abs(x) > std::abs(v)) ++absAbove; }
            const QString vw = order == 2 ? QStringLiteral("accélération") : QStringLiteral("vitesse");
            const QString vu = order == 2 ? unitAcc : unitRate;
            rows += probRow(QStringLiteral("P(%1 > %2 %3)").arg(vw, curveNum(v), vu), percentText(double(above) / double(qf.size())));
            if (v != 0.0)
                rows += probRow(QStringLiteral("P(|%1| > %2 %3)").arg(vw, curveNum(std::abs(v)), vu),
                                percentText(double(absAbove) / double(qf.size())));
            marker2 = v;
        }
    } else if (op == "crossing") {
        R.html += QStringLiteral("<p><b>Date où %1 = %2 (%3)</b></p>").arg(Q, curveNum(y),
                      which == "first" ? QStringLiteral("première atteinte") : which == "last" ? QStringLiteral("dernière atteinte")
                                                                                  : QStringLiteral("toutes les atteintes"));
        R.prompt += QStringLiteral("Date où la courbe atteint %1 = %2 (%3), dans les tirages où elle l'atteint :\n").arg(Q, curveNum(y),
                        which == "first" ? QStringLiteral("première atteinte") : which == "last" ? QStringLiteral("dernière atteinte")
                                                                                    : QStringLiteral("toutes les atteintes"));
        appendSummary(R.html, R.prompt, d, hpdIntervals(d, 95.0), hpdIntervals(d, 68.0), st, R.level, fmt);
        QJsonObject s = plotSeries(R.name, d, fmt);
        R.series.append(s);
    } else {
        // Durée (années) puis vitesse
        const std::vector<double> df = finiteOf(q2);
        double h2 = 0.0;
        const Density dd = kernelDensity(df, -infv, infv, Bandwidth::SheatherJones, &h2);
        if (dd.valid()) {
            R.html += QStringLiteral("<p><b>Temps mis pour aller de %1 = %2 à %1 = %3 (années)</b></p>").arg(D, curveNum(y1), curveNum(y2));
            R.prompt += QStringLiteral("Temps mis pour aller de %1 = %2 à %1 = %3, en années :\n").arg(D, curveNum(y1), curveNum(y2));
            appendSummary(R.html, R.prompt, dd, hpdIntervals(dd, 95.0), hpdIntervals(dd, 68.0), densityStats(dd), R.level, DateFormat::Numeric);
            R.plots << valuePlot(QStringLiteral("Durée — %1").arg(R.name), QStringLiteral("Durée (années)"), R.name, dd, R.level);
        }
        numericSummary(d, QStringLiteral("Vitesse moyenne (%1 − %2) / (t2 − t1), en %3")
                              .arg(curveNum(y2), y1 < 0 ? QStringLiteral("(%1)").arg(curveNum(y1)) : curveNum(y1), unitRate));
    }
    if (!rows.isEmpty()) {
        R.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
        R.prompt += rowsPrompt(rows);
    }
    if (op != "crossing")
        R.plots.prepend(valuePlot(R.title.section(QStringLiteral(" ("), 0, 0),
                                  op == "value" ? D : QStringLiteral("%1 (%2)").arg(op == "rate_values" || op == "rate" ? QStringLiteral("Vitesse moyenne")
                                                                                    : order == 2 ? QStringLiteral("Accélération") : QStringLiteral("Vitesse"),
                                                                                    order == 2 ? unitAcc : unitRate),
                                  R.name, d, R.level, marker, marker2, op == "value" ? D : order == 2 ? QStringLiteral("accélération") : QStringLiteral("vitesse")));

    if (order > 0) addEnvelope(order, true);
    addEnvelope(0, true);

    // Méthode, lissage, convergence
    const QString conv = convergenceOf(q);
    const QString hRule = rule == Bandwidth::SheatherJones ? QStringLiteral("Sheather-Jones") : QStringLiteral("Silverman (repli)");
    const QString chains = chainStarts.size() > 1
        ? QStringLiteral("%1 chaînes, repérées dans « %2 »").arg(chainStarts.size()).arg(chainsFrom)
        : QStringLiteral("chaînes non séparées : joindre aussi Chain_all_Events.csv du même calcul pour la convergence");
    const QString method = op == "crossing" || op == "rate_values"
        ? QStringLiteral("atteintes cherchées sur %1 → %2 (grille de 2000 pas, puis bissection)").arg(dateformat::format(wa, fmt), dateformat::format(wb, fmt))
        : QString();
    R.html += QStringLiteral("<p><small>Calcul exact tirage par tirage sur %1 splines a posteriori ; %2. Chaque spline est évaluée comme "
                             "dans ChronoModel (spline cubique, prolongée en ligne droite hors des nœuds)%3. Densité lissée par noyau "
                             "gaussien, h = %4 (règle de %5)%6.</small></p>")
                  .arg(fr.toString(qlonglong(N)), chains.toHtmlEscaped(), method.isEmpty() ? QString() : QStringLiteral(" ; ") + method.toHtmlEscaped(),
                       curveNum(h), hRule, conv.isEmpty() ? QString() : QStringLiteral(" ; convergence de cette grandeur : %1").arg(conv.toHtmlEscaped()));
    R.prompt += QStringLiteral("Calcul exact tirage par tirage sur %1 splines a posteriori de ChronoModel ; %2%3.%4 Les graphiques (loi de la "
                               "grandeur, courbe médiane et son intervalle de crédibilité à 95 %, quantiles 2,5 – 97,5 % date par date) sont DÉJÀ tracés.\n")
                    .arg(fr.toString(qlonglong(N)), chains, method.isEmpty() ? QString() : QStringLiteral(" ; ") + method,
                         conv.isEmpty() ? QString() : QStringLiteral(" Convergence de cette grandeur : %1.").arg(conv));
    if (op == "rate" || op == "rate_values" || op == "derivative")
        R.prompt += QStringLiteral("Vitesse en %1 ; positive = la courbe croît avec le temps.\n").arg(unitRate);
    return finish();
}

} // namespace

void MethodRunner::setProgressHandler(std::function<bool(const QString&, double)> handler)
{
    progressHandler() = std::move(handler);
}

bool MethodRunner::asksEvent(const QString& question)
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(event|events|evenements?|regroupe\w*|meme evenement|same event)\b)"));
    return re.match(text::fold(question)).hasMatch();
}

bool MethodRunner::asksCombine(const QString& question)
{
    // « Combine » de ChronoModel (produit des vraisemblances) ; un événement cité l'emporte (MCMC).
    static const QRegularExpression re(QStringLiteral(
        R"(\b(combine|combiner|combinaisons?|combinees?|combines|combined|combining|combination)\b)"));
    return !asksEvent(question) && re.match(text::fold(question)).hasMatch();
}

bool MethodRunner::asksOrder(const QString& question)
{
    // « avant » seul ne suffit pas (« 500 avant J.-C. ») : il faut une tournure de comparaison.
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:avant|apres) l['’ ]\s*autre\b)"
        R"(|\b(?:laquelle|lequel|quelle|quel)\b[^?.!;]{0,60}\b(?:plus ancien(?:ne)?s?|plus recent(?:e)?s?|plus vieu(?:x|ille)|plus jeune|anterieure?s?|posterieure?s?|en premier|d['’ ]\s*abord|precede\w*)\b)"
        R"(|\b(?:est|sont|serait|seraient)[- ](?:(?:elle|il|elles|ils)s?\s+)?(?:bien\s+|vraiment\s+)?(?:anterieure?s?|posterieure?s?|plus ancien(?:ne)?s?|plus recent(?:e)?s?)\b)"
        R"(|\b(?:anteriorite|posteriorite|antero-?posteriorite)\b)"
        R"(|\bordre\s+(?:chronologique\s+|relatif\s+)?(?:des|de ces|de mes|entre|de deux|de la|du)\b)"
        R"(|\bplus (?:ancien(?:ne)?s?|recent(?:e)?s?|vieu(?:x|ille)|jeune) que\b)"
        R"(|\bprobabilite (?:que|qu['’ ])[^?.!;]{0,60}\b(?:avant|apres|anterieure?|posterieure?|precede|plus ancien\w*|plus recent\w*)\b)"
        R"(|\bwhich (?:one|date|sample|event)?\s*(?:is|was|came)\s+(?:older|earlier|younger|later|first)\b)"
        R"(|\b(?:older|earlier|younger|later) than\b|\bbefore or after\b|\b(?:chronological|relative) order\b)"
        R"(|\bprobability (?:that )?[^?.!;]{0,60}\b(?:before|after|older|earlier|younger|later|precedes)\b)"));
    return re.match(text::fold(question)).hasMatch();
}

bool MethodRunner::asksTraces(const QString& question)
{
    // Demande explicite des traces MCMC. « traces » seul ne suffit pas : en archéologie c'est aussi un nom courant
    // (« traces d'occupation », « traces de feu ») et « trace » est le verbe « tracer » (« trace la courbe »).
    //   - traces de θ / de l'événement / des chaînes / MCMC / des paramètres ;
    //   - « avec / affiche / montre / dessine / trace / ajoute les traces » ;
    //   - anglais : trace plot, traceplot, chain traces, MCMC traces, show the traces.
    static const QRegularExpression re(QStringLiteral(
        R"(\btraces? (?:de |des |du |d['’]\s*)?(?:θ|theta|l['’]\s*evenement|la chaine|chaines?|mcmc|parametres?)(?![a-z0-9]))"
        R"(|\b(?:avec|affiche\w*|montre\w*|dessine\w*|trace\w*|ajoute\w*|voir|show|plot) (?:aussi |also )?(?:les |la |the )?traces\b)"
        R"(|\b(?:trace ?plots?|chain traces?|mcmc traces?)\b)"));
    return re.match(text::fold(question)).hasMatch();
}

bool MethodRunner::asksHowTo(const QString& question)
{
    const QString q = text::fold(question).simplified();
    static const QRegularExpression re(QStringLiteral(
        R"(\bcomment\s+(?:faire|fait[- ]?on|on\s+fait|fais[- ]je|je\s+fais|puis[- ]je|je\s+peux|peut[- ]on|on\s+peut|dois[- ]je|)"
        R"(utiliser|utilise[- ]t[- ]on|s'utilise|dater|calibrer|creer|importer|exporter|joindre|charger|lire|interpreter|)"
        R"(proceder|obtenir|tracer|ajouter|enregistrer|ouvrir|poser|demander|preparer|ecrire|marche|fonctionne|ca\s+marche|cela\s+marche)\b)"
        R"(|\b(?:que|qu'est[- ]ce\s+que)\s+(?:tu\s+)?(?:peux|sais|sait)[- ]?(?:tu\s+)?(?:faire|calculer)\b|\ba\s+quoi\s+sers[- ]tu\b)"
        R"(|\bmode\s+d'emploi\b|\bmanuel\b|\btutoriel\b|\bquelles?\s+(?:questions?|fonctions?|fonctionnalites?)\b)"
        R"(|\bhow\s+(?:do|can|should|would)\s+(?:i|we|you|one)\b|\bhow\s+to\b|\bwhat\s+can\s+you\s+(?:do|compute|calculate)\b)"
        R"(|\buser\s+guide\b|\bmanual\b|^\s*(?:aide|help)\s*[?!.]*\s*$)"));
    return re.match(q).hasMatch();
}

QJsonObject MethodRunner::withAgeUnit(QJsonObject d, const QString& method)
{
    const QString unit = d.value("unit").toString(d.value("age_unit").toString()).trimmed();
    const QString origin = d.value("origin").toString().trimmed();
    if (unit.isEmpty() && origin.isEmpty()) return d;
    for (const char* k : { "unit", "age_unit", "origin" }) d.remove(k);
    text::AgeUnit u = text::ageUnitAt(QStringLiteral(" ") + text::fold(unit.isEmpty() ? QStringLiteral("ans") : unit)
                                          + (origin.isEmpty() ? QString() : QStringLiteral(" ") + text::fold(origin)), 0);
    if (u.length == 0) {                                   // « ans », « a », « years » sans origine : années
        const QString f = text::fold(unit);
        if (!(f.isEmpty() || f == "a" || f == "an" || f == "ans" || f == "annees" || f == "year" || f == "years" || f == "yr")) return d;
        u.scale = 1.0;
        u.label = QStringLiteral("ans");
    }
    const double v = d.value("age").toDouble(), e = d.value("error").toDouble();
    if (u.scale != 1.0 || std::isfinite(u.origin))
        d["input"] = QStringLiteral("%1 ± %2 %3").arg(text::numAuto(v, 4), text::numAuto(e, 4), u.label);
    if (method == QLatin1String("tl")) { d["age"] = v * u.scale; d["error"] = e * u.scale; return d; }
    if (d.value("mode").toString() == QLatin1String("curve")) return d;          // mesure sur une courbe : pas un âge
    d["age"] = (std::isfinite(u.origin) ? u.origin : 1950.0) - v * u.scale;     // ka BP par défaut
    d["error"] = e * u.scale;
    return d;
}

bool MethodRunner::asksCurveList(const QString& question)
{
    const QString q = text::fold(question).simplified();
    static const QRegularExpression verb(QStringLiteral(
        R"(\b(?:liste\w*|quelles?|affiche\w*|montre\w*|donne\w*|cite\w*|which|list|show|what)\b.{0,40}\b(?:courbes?|curves?|densites?|densities)\b)"));
    static const QRegularExpression kind(QStringLiteral(
        R"(\b(?:ref|refs|reference|references|disponibles?|calibration|available|installees?|gauss|density|densites? tabulees?|plugin)\b)"));
    static const QRegularExpression bare(QStringLiteral(R"(^(?:les\s+)?(?:courbes?|curves?)\s+(?:de\s+)?(?:ref\w*|disponibles?|calibration|available)\s*[?.!]*$)"));
    return (verb.match(q).hasMatch() && kind.match(q).hasMatch()) || bare.match(q).hasMatch();
}

QString MethodRunner::helpHint()
{
    return QStringLiteral("[Question sur l'utilisation de SeldonH : réponds en quelques phrases simples, avec 2 à 4 exemples de "
                          "questions que l'utilisateur peut taper telles quelles, en langage naturel, entre guillemets (par exemple "
                          "« Calibre 2500 ± 30 BP avec IntCal20 »). N'écris AUCUN bloc de code (ni calcul, ni JSON, ni chronomodel, "
                          "ni plot), aucun nom d'outil ni de champ technique, et ne lance aucun calcul : les valeurs des exemples ne "
                          "sont pas des données de l'utilisateur. Si c'est utile, renvoie à la section de l'aide (menu Aide, touche "
                          "F1) par son titre.]\n");
}

QString MethodRunner::stripCodeBlocks(const QString& answer)
{
    static const QRegularExpression fence(QStringLiteral(R"(\n?```[^\n]*\n.*?(?:\n```|$)[^\n]*)"),
                                          QRegularExpression::DotMatchesEverythingOption);
    QString s = answer;
    s.remove(fence);
    static const QRegularExpression blank(QStringLiteral(R"(\n{3,})"));
    s.replace(blank, QStringLiteral("\n\n"));
    // Phrase d'annonce restée orpheline (« Voici un exemple : »)
    s = s.trimmed();
    static const QRegularExpression lead(QStringLiteral(R"((?:\n|^)[^\n]*(?:voici|exemple|example|here is)[^\n]*:\s*$)"),
                                         QRegularExpression::CaseInsensitiveOption);
    s.remove(lead);
    return s.trimmed();
}

bool MethodRunner::asksCommentary(const QString& question)
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(expliqu\w*|explication\w*|interpret\w*|comment(?:e|es|ez|er|aire|aires)|pourquoi|que (?:faut.il |dois.je |peut.on )?penser|qu.en penser|)"
        R"(que signifi\w*|signification|veut dire|coheren\w*|incoheren\w*|compatib\w*|conseil\w*|recommand\w*|discut\w*|analys\w*|)"
        R"((?:ton|votre) avis|plausib\w*|fiab\w*|est.ce (?:normal|correct|coherent|possible|plausible)|compar\w*|)"
        R"(explain\w*|why|comment on|what does|meaning|consistent|advice|advise|recommend\w*|discuss\w*|analy[sz]\w*|opinion|)"
        R"(reliab\w*|should)\b)"));
    return re.match(text::fold(question)).hasMatch();
}

QString MethodRunner::shownResultHeader()
{
    return QStringLiteral("[Résultat de calcul de SeldonH — valeurs exactes, DÉJÀ AFFICHÉES à l'utilisateur dans une fiche avec le "
                          "graphique : ne les recalcule pas, ne les contredis pas et ne les recopie pas (ni liste, ni tableau, ni "
                          "résumé des intervalles) ; cite seulement la valeur dont ton commentaire a besoin, à l'identique]\n");
}

QString MethodRunner::externalResultHeader()
{
    return QStringLiteral("[Résultat de calcul de SeldonH — valeurs exactes, à reprendre telles quelles sans les recalculer ni les contredire]\n");
}

bool MethodRunner::asksOverlay(const QString& question)
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(superpos\w*|en superposition|(?:sur )?(?:le |la )?mem(?:e)? (?:graphe|graphique|figure|axe|ligne|cadre)|une seule ligne|overlay|overlaid|superimpos\w*|(?:on )?(?:the )?same (?:graph|plot|figure|axis|axes|panel|chart))\b)"));
    return re.match(text::fold(question)).hasMatch();
}

QString MethodRunner::colorOf(const QJsonObject& o)
{
    const QString c = o.value("color").toString().trimmed();
    if (!c.isEmpty()) return c;
    if (o.value("red").isDouble() && o.value("green").isDouble() && o.value("blue").isDouble()) {
        auto ch = [&](const char* k) { return std::clamp(o.value(k).toInt(), 0, 255); };
        return QStringLiteral("#%1%2%3").arg(ch("red"), 2, 16, QChar('0')).arg(ch("green"), 2, 16, QChar('0'))
                                        .arg(ch("blue"), 2, 16, QChar('0'));
    }
    return {};
}

QJsonObject MethodRunner::mcmcFromQuestion(const QString& question, double* tmin, double* tmax)
{
    QString q = text::fold(question);
    QJsonObject m;
    auto num = [](QString s) { s.remove(QRegularExpression("[\\s.\\x{00A0}\\x{202F}]")); return s.toLongLong(); };
    static const QRegularExpression burn(QStringLiteral(R"(burn[\s-]?in\s*(?:de\s*|=\s*|:\s*)?(\d[\d\s.\x{00A0}\x{202F}]*\d|\d))"));
    if (const auto mm = burn.match(q); mm.hasMatch()) { m["num_burn"] = double(num(mm.captured(1))); q.remove(mm.capturedStart(), mm.capturedLength()); }
    static const QRegularExpression iter(QStringLiteral(R"((\d[\d\s.\x{00A0}\x{202F}]*\d|\d)\s*(?:iterations?|iter)\b)"));
    if (const auto mm = iter.match(q); mm.hasMatch()) m["num_iter"] = double(num(mm.captured(1)));
    static const QRegularExpression chains(QStringLiteral(R"((\d{1,2})\s*(?:chaines?|chains?)\b)"));
    if (const auto mm = chains.match(q); mm.hasMatch()) m["num_proc"] = mm.captured(1).toInt();
    static const QRegularExpression thin(QStringLiteral(R"((?:thinning|intervalle d'echantillonnage)\s*(?:de\s*|=\s*|:\s*)?(\d+))"));
    if (const auto mm = thin.match(q); mm.hasMatch()) m["thinning_interval"] = mm.captured(1).toInt();
    // « pas de recuit », « sans recuit », « désactive le recuit », « annealing = 0 » : recuit simulé désactivé, comme
    // ChronoModel (MCMCSettingsDialog : « Set Recurrence to 0 to disable simulated annealing »).
    static const QRegularExpression noAnneal(QStringLiteral(
        R"(\b(?:pas|sans|aucun|desactiv\w*|supprim\w*|enlev\w*|coup\w*|no|without|disable\w*|remove|turn\w*\s+off|switch\w*\s+off)\s+(?:de\s+|du\s+|le\s+|l'|the\s+)?(?:simulated\s+)?(?:recuit|annealing)\b)"
        R"(|\b(?:recuit|annealing)\s*(?:=|:)?\s*0\b)"
        R"(|\b(?:recuit|annealing)\s+(?:desactiv\w*|disabled|off)\b)"));
    if (noAnneal.match(q).hasMatch()) m["anneal_recurrence"] = 0;
    static const QRegularExpression seeds(QStringLiteral(R"((?:graines?|seeds?)\s*(?:=|:)?\s*(\d+(?:\s*[;,]\s*\d+)*))"));
    if (const auto mm = seeds.match(q); mm.hasMatch()) m["seeds"] = mm.captured(1);
    static const QRegularExpression period(QStringLiteral(
        R"((?:periode d'etude|study period)\s*(?:de|from|of|:|=)?\s*(-?\d+)\s*(?:a|-|–|→|et|to|and)\s*(-?\d+))"));
    if (const auto mm = period.match(q); mm.hasMatch() && tmin && tmax) {
        *tmin = mm.captured(1).toDouble(); *tmax = mm.captured(2).toDouble();
    }
    return m;
}

double MethodRunner::requestedLevel(const QString& question)
{
    QString q = text::fold(question);
    // α95 (cône de confiance archéomagnétique) n'est pas un niveau de confiance d'intervalle : on l'efface d'abord.
    static const QRegularExpression alpha(QStringLiteral(R"(cone de confiance|cone of confidence|(?:alpha|α|\ba)\s*-?\s*95\b)"));
    q.remove(alpha);
    // 1) « hpd 99 », « confiance de 90 % », « crédibilité 99,7 » : « % » facultatif
    static const QRegularExpression kw(QStringLiteral(
        R"(\b(?:hpd|confiance|credibilite|confidence|credib\w*)\b[^\d\n%]{0,30}?(\d{1,2}(?:[.,]\d{1,2})?)\s*(?:%|pour ?cent|\b))"));
    // 2) « niveau / seuil / probabilité / intervalle / région / à N % » : « % » obligatoire
    static const QRegularExpression pct(QStringLiteral(
        R"((?:\b(?:niveau|seuil|probabilite|intervalles?|regions?|a|level|threshold|probability|intervals?|at)\b)[^\d\n%]{0,20}?(\d{1,2}(?:[.,]\d{1,2})?)\s*(?:%|pour ?cent))"));
    for (const QRegularExpression* re : { &kw, &pct }) {
        auto it = re->globalMatch(q);
        while (it.hasNext()) {
            const double l = text::toNumber(it.next().captured(1));
            if (l >= 50 && l < 100) return l;           // un niveau de confiance utile est entre 50 et 100 %
        }
    }
    return 0.0;
}

RunOutcome MethodRunner::run(const QVector<QJsonObject>& requests, DateFormat fmt)
{
    RunOutcome out;
    if (requests.isEmpty()) return out;
    out.found = true;
    out.count = requests.size();
    for (const QJsonObject& r : requests) out.requests << normalizeRequest(r);

    const MethodRegistry& reg = MethodRegistry::instance();
    QString html;                    // le bandeau de la fiche (cardMarker) est ajouté à la fin, avec les opérations calculées
    QStringList ops;                 // opérations réellement calculées, en clair : titre de la fiche
    QString prompt = shownResultHeader();
    QJsonArray series;
    QStringList plotNames;           // noms des datations / événements tracés : titre du graphique
    QVector<double> levelsUsed;
    if (fmt == DateFormat::BcAd)
        prompt += QStringLiteral("Années astronomiques signées (l'année 0 existe : -499 = 500 av. J.-C.), comme ChronoModel : écris-les "
                                 "telles quelles (« -499 »), sans les convertir en av. J.-C.\n");
    else
        prompt += QStringLiteral("Format des dates : %1. Toutes les dates ci-dessous sont déjà exprimées dans ce format.\n").arg(dateformat::title(fmt));

    bool anyEvent = false;
    bool anyCombine = false;
    bool anyPosterior = false;
    bool anyCurveTrace = false;      // courbe de ChronoModel (splines ou carte de densité)
    int curveRuns = 0;
    bool anyInverse = false;         // calibration inverse : mesure attendue pour une date       // calcul sur des traces MCMC exportées par ChronoModel
    bool anyTraces = false;
    bool anyCurve = false;           // au moins une courbe de référence : le modèle doit la rappeler
    bool overlay = false;            // « layout »: « overlay » demandé : densités superposées ; sinon une par ligne
    bool anyColor = false;
    int refPlotCount = 0;            // graphiques de courbes de référence des datations simples
    // Un modèle de langage met parfois un calcul « traces » ou « inverse » dans un événement ou une combinaison
    // ({"method":"event","dates":[{"method":"traces",…}]}) : ce ne sont pas des datations, ils sont exécutés seuls.
    QVector<QJsonObject> todo;
    // Demandes sur le tableau courant ({"method":"table",…}) : sélection, puis extraction, datations, événement,
    // combinaison ou modèle. Les valeurs viennent du tableau lu par SeldonH.
    QString tableHtml, tablePrompt;
    int tableDates = 0;
    auto expandTable = [&](const QJsonObject& r) {
        if (!datatable::hasCurrent()) {
            tableHtml += QStringLiteral("<p><i>Aucun tableau n'est chargé : joignez un fichier (.xlsx, .ods, .csv) ou collez le tableau dans la question.</i></p>");
            tablePrompt += QStringLiteral("Calcul impossible : aucun tableau n'est chargé. Demande à l'utilisateur de joindre le fichier "
                                          "(.xlsx, .ods, .csv) ou de coller le tableau ; n'écris pas les valeurs toi-même.\n");
            return;
        }
        const datatable::Table& T = datatable::current();
        const datatable::Selection S = datatable::select(T, r);
        if (!S.error.isEmpty()) {
            tableHtml += QStringLiteral("<p><i>Sélection impossible dans le tableau « %1 » : %2.</i></p>").arg(T.title().toHtmlEscaped(), S.error.toHtmlEscaped());
            tablePrompt += QStringLiteral("Sélection impossible dans le tableau « %1 » : %2. Corrige la sélection (noms de colonnes et "
                                          "valeurs du résumé du tableau).\n").arg(T.title(), S.error);
            return;
        }
        QString action = text::fold(r.value("action").toString()).trimmed();
        static const QHash<QString, QString> actions = {
            { "", "extract" }, { "extract", "extract" }, { "extraire", "extract" }, { "liste", "extract" }, { "list", "extract" },
            { "date", "date" }, { "dater", "date" }, { "calibrate", "date" }, { "calibrer", "date" }, { "event", "event" },
            { "evenement", "event" }, { "combine", "combine" }, { "combiner", "combine" }, { "model", "model" }, { "modele", "model" } };
        if (!actions.contains(action)) {
            tablePrompt += QStringLiteral("Action « %1 » inconnue : extract, date, event, combine ou model.\n").arg(action);
            tableHtml += QStringLiteral("<p><i>Action « %1 » inconnue pour le tableau.</i></p>").arg(action.toHtmlEscaped());
            return;
        }
        action = actions.value(action);
        QStringList skipped;
        const QVector<QJsonObject> dates = datatable::requestsOf(T, S.rows, &skipped);
        static const QHash<QString, QString> verb = { { "extract", "extraction" }, { "date", "datation ligne par ligne" },
                                                      { "event", "événement" }, { "combine", "combinaison" }, { "model", "modèle ChronoModel" } };
        ops << QStringLiteral("Tableau « %1 » : %2").arg(T.title(), verb.value(action));
        tableHtml += QStringLiteral("<p><b>📋 Tableau « %1 »</b> — %2 : %3 → <b>%4 ligne(s)</b>%5.</p>")
                         .arg(T.title().toHtmlEscaped(), verb.value(action), S.criteria.toHtmlEscaped()).arg(S.rows.size())
                         .arg(action == QLatin1String("extract") ? QString() : QStringLiteral(", %1 datable(s)").arg(dates.size()));
        tablePrompt += QStringLiteral("Tableau « %1 », %2 : sélection « %3 » → %4 ligne(s), %5 datable(s).\n")
                           .arg(T.title(), verb.value(action), S.criteria).arg(S.rows.size()).arg(dates.size());
        if (!skipped.isEmpty() && action != QLatin1String("extract")) {
            tableHtml += QStringLiteral("<p><i>Lignes non datables écartées : %1.</i></p>").arg(skipped.join(QStringLiteral(" ; ")).toHtmlEscaped());
            tablePrompt += QStringLiteral("Lignes non datables écartées : %1.\n").arg(skipped.join(QStringLiteral(" ; ")));
        }
        if (action == QLatin1String("extract")) {
            tableHtml += datatable::rowsHtml(T, S.rows);
            const QString csv = datatable::toCsv(T, S.rows);
            if (csv.size() < 400000) {
                const QByteArray b64 = csv.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
                tableHtml += QStringLiteral("<p><a href=\"seldon://tablecsv?data=%1\">💾 Enregistrer ces lignes en CSV…</a></p>").arg(QString::fromLatin1(b64));
            }
            QStringList names;
            for (int i : S.rows) {
                const QJsonObject& q = T.requests[i];
                names << QStringLiteral("#%1%2").arg(i + 1).arg(q.contains("name") ? QStringLiteral(" ") + q.value("name").toString() : QString());
            }
            QString list = names.join(QStringLiteral(", "));
            if (list.size() > 1500) list = list.left(1500) + QStringLiteral(" …");
            tablePrompt += QStringLiteral("Lignes extraites, DÉJÀ AFFICHÉES en tableau par SeldonH (avec un lien d'export CSV) : %1. Ne recopie "
                                          "pas le tableau.\n").arg(list);
            return;
        }
        if (dates.isEmpty()) {
            tableHtml += QStringLiteral("<p><i>Aucune ligne datable dans cette sélection.</i></p>");
            tablePrompt += QStringLiteral("Calcul impossible : aucune ligne datable dans cette sélection.\n");
            return;
        }
        if (action == QLatin1String("model")) {
            QString err;
            const QJsonObject spec = datatable::modelSpec(T, S.rows, r.value("group_by").toString(), r.value("name").toString(), &err);
            if (spec.isEmpty()) {
                tableHtml += QStringLiteral("<p><i>Modèle impossible : %1.</i></p>").arg(err.toHtmlEscaped());
                tablePrompt += QStringLiteral("Modèle impossible : %1.\n").arg(err);
                return;
            }
            out.chrSpec = spec;
            QStringList evs;
            for (const QJsonValue& e : spec.value("events").toArray())
                evs << QStringLiteral("%1 (%2 datation(s))").arg(e.toObject().value("name").toString()).arg(e.toObject().value("dates").toArray().size());
            const QByteArray b64 = QJsonDocument(spec).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
            tableHtml += QStringLiteral("<p>🗺 Modèle ChronoModel préparé : %1 événement(s) — %2. Ni phase ni contrainte. "
                                        "<a href=\"seldon://tablechr?spec=%3\">💾 Enregistrer le modèle (.chr)…</a></p>")
                             .arg(evs.size()).arg(evs.join(QStringLiteral(", ")).toHtmlEscaped(), QString::fromLatin1(b64));
            tablePrompt += QStringLiteral("Modèle ChronoModel préparé par SeldonH avec les valeurs du tableau, DÉJÀ PROPOSÉ à l'enregistrement : "
                                          "%1 événement(s) : %2. Ni phase ni contrainte : si l'utilisateur en veut, dis-lui de les décrire "
                                          "(événements nommés comme ci-dessus). Ne recopie pas les valeurs.\n")
                               .arg(evs.size()).arg(evs.join(QStringLiteral(", ")));
            return;
        }
        auto carry = [&](QJsonObject& o, std::initializer_list<const char*> keys) {
            for (const char* k : keys) if (r.contains(k) && !o.contains(k)) o[k] = r.value(k);
        };
        if (action == QLatin1String("date")) {
            int k = 0;
            for (int i : S.rows) {
                if (T.requests[i].isEmpty()) continue;
                QJsonObject d = dates[k++];
                d["_table_row"] = i + 1;
                carry(d, { "level", "tmin", "tmax", "layout" });
                todo << d;
                ++tableDates;
            }
            return;
        }
        QJsonArray arr;
        for (const QJsonObject& d : dates) arr.append(d);
        QJsonObject e{ { "method", action }, { "dates", arr } };
        e["name"] = r.value("name").toString().trimmed().isEmpty() ? S.criteria : r.value("name").toString().trimmed();
        carry(e, { "level", "tmin", "tmax", "mcmc", "traces", "layout", "color" });
        todo << e;
    };
    for (const QJsonObject& r : requests) {
        const QString rawMethod = text::fold(r.value("method").toString()).trimmed();
        if (rawMethod == QLatin1String("table") || rawMethod == QLatin1String("tableau")) { expandTable(r); continue; }
        const QJsonObject n = normalizeRequest(r);
        const QString m = n.value("method").toString();
        if ((m == QLatin1String("event") || m == QLatin1String("combine")) && n.value("dates").isArray() && !n.value("dates").toArray().isEmpty()) {
            QVector<QJsonObject> inner;
            for (const QJsonValue& d : n.value("dates").toArray()) {
                const QString dm = d.toObject().value("method").toString();
                if (dm == QLatin1String("relation") || dm == QLatin1String("inverse")) inner << d.toObject();
            }
            if (inner.size() == n.value("dates").toArray().size()) {
                for (QJsonObject d : inner) { if (n.contains("level") && !d.contains("level")) d["level"] = n.value("level"); todo << d; }
                continue;
            }
        }
        todo << r;
    }
    out.count = todo.size();
    html += tableHtml;
    prompt += tablePrompt;
    const int recapHtmlPos = html.size(), recapPromptPos = prompt.size();
    const bool tableCompact = tableDates > 12;       // beaucoup de lignes : récapitulatif seul, sans fiche par datation
    struct RecapRow { int row; QString name, measure, hpd, median, note; };
    QVector<RecapRow> recap;
    QStringList recapCurves;
    double recapLevel = kDefaultLevel;
    for (const QJsonObject& rq0 : std::as_const(todo)) {
        if (rq0.value("layout").toString() == QLatin1String("overlay")) overlay = true;
        if (!colorOf(rq0).isEmpty()) anyColor = true;
        const QJsonObject rq = normalizeRequest(rq0);
        if (rq.value("method").toString() != QLatin1String("relation") && rq.value("method").toString() != QLatin1String("curve"))
            ops << operationTitle(rq);
        if (const QString bad = obsoleteFieldError(rq0); !bad.isEmpty()) {     // nom de champ d'une ancienne version
            html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(bad.toHtmlEscaped());
            prompt += QStringLiteral("Calcul impossible : %1. Réécris la demande avec ces noms.\n").arg(bad);
            continue;
        }
        if (rq.value("method").toString() == QLatin1String("density_file")) {
            // Fichier de densité pour le plugin Density de ChronoModel (loi classique placée sur l'axe du temps).
            const DensityFileResult F = buildDensityFile(rq);
            const QString head = F.ok ? QStringLiteral("Fichier de densité pour ChronoModel — %1%2")
                                            .arg(F.description, F.placement.isEmpty() ? QString() : QStringLiteral(" ; ") + F.placement)
                                      : QStringLiteral("Fichier de densité pour ChronoModel");
            html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
            prompt += QStringLiteral("\n%1\n").arg(head);
            if (!F.ok) {
                html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(F.error.toHtmlEscaped());
                prompt += QStringLiteral("Calcul impossible : %1.\n").arg(F.error);
                continue;
            }
            levelsUsed << kDefaultLevel;
            appendSummary(html, prompt, F.density, hpdIntervals(F.density, 95.0), hpdIntervals(F.density, 68.0),
                          densityStats(F.density), kDefaultLevel, fmt);
            // Le lien porte la spécification (et non les valeurs) : le fichier est recalculé à l'enregistrement,
            // ce qui marche aussi dans une conversation rouverte depuis l'historique.
            const QByteArray spec = QJsonDocument(rq).toJson(QJsonDocument::Compact)
                                        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
            html += QStringLiteral("<p>📄 <b>%1</b> : %2 valeurs de %3 à %4 (pas %5). "
                                   "<a href=\"seldon://densityfile?spec=%6\">Enregistrer le fichier…</a></p>")
                        .arg(F.fileName.toHtmlEscaped()).arg(F.points)
                        .arg(QString::number(F.tmin, 'g', 10), QString::number(F.tmax, 'g', 10), QString::number(F.step, 'g', 6),
                             QString::fromLatin1(spec));
            for (const QString& w : F.warnings) {
                html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());
                prompt += QStringLiteral("Avertissement : %1\n").arg(w);
            }
            prompt += QStringLiteral("Fichier « %1 » préparé par SeldonH (%2 valeurs de %3 à %4, format du plugin Density : tmin, tmax, "
                                     "puis les valeurs). L'utilisateur l'enregistre avec le lien « Enregistrer le fichier… » ; il doit être "
                                     "placé dans le dossier Calib/Density de ChronoModel (ChronoModel relancé). Dans un modèle : "
                                     "{\"type\":\"density\",\"curve\":\"%1\"}. Ne recopie pas les valeurs du fichier.\n")
                          .arg(F.fileName).arg(F.points).arg(QString::number(F.tmin, 'g', 10), QString::number(F.tmax, 'g', 10));
            QJsonObject ser = plotSeries(F.fileName, F.density, fmt);
            if (const QString col = colorOf(rq); !col.isEmpty()) ser["color"] = col;
            series.append(ser);
            if (!plotNames.contains(F.fileName)) plotNames << F.fileName;
            continue;
        }
        if (rq.value("method").toString() == "inverse") {
            const InverseRun V = runInverse(rq, fmt);
            html += V.html;
            prompt += V.prompt;
            if (V.ok) {
                anyInverse = true;
                anyCurve = anyCurve || V.curves;
                out.extraPlots << V.plot;
            }
            continue;
        }
        if (rq.value("method").toString() == "relation") {
            const RelationRun Rl = runRelation(rq, fmt);
            ops << (Rl.title.isEmpty() ? QStringLiteral("Relation entre dates") : Rl.title);
            html += Rl.html;
            prompt += Rl.prompt;
            if (Rl.ok) {
                anyPosterior = anyPosterior || Rl.posterior;
                anyCurve = anyCurve || Rl.curves;
                levelsUsed << Rl.level;
                for (const QJsonValue& s : Rl.series) series.append(s);
                if (!Rl.series.isEmpty() && !plotNames.contains(Rl.name)) plotNames << Rl.name;
                if (!Rl.extraPlot.isEmpty()) out.extraPlots << Rl.extraPlot;
                out.extraPlots += Rl.scatterPlots;
            }
            continue;
        }
        if (rq.value("method").toString() == "curve") {
            const CurveRun Cv = runCurve(rq, fmt);
            ops << (Cv.title.isEmpty() ? QStringLiteral("Courbe de ChronoModel") : Cv.title);
            html += Cv.html;
            prompt += Cv.prompt;
            ++curveRuns;
            if (Cv.ok) {
                anyCurveTrace = true;
                levelsUsed << Cv.level;
                for (const QJsonValue& s : Cv.series) series.append(s);
                if (!Cv.series.isEmpty() && !plotNames.contains(Cv.name)) plotNames << Cv.name;
                out.extraPlots += Cv.plots;
            }
            continue;
        }
        if (rq.value("method").toString() == "combine") {
            const CombineRun C = runCombine(rq, fmt);
            html += C.html;
            prompt += C.prompt;
            if (C.ok) {
                anyCombine = true;
                anyCurve = anyCurve || C.curves;
                levelsUsed << C.level;
                for (const QJsonValue& s : C.series) series.append(s);
                if (!C.name.isEmpty() && !plotNames.contains(C.name)) plotNames << C.name;
            }
            continue;
        }
        if (rq.value("method").toString() == "event") {
            const EventRun E = runEvent(rq, fmt);
            html += E.html;
            prompt += E.prompt;
            if (E.ok) {
                anyEvent = true;
                anyCurve = anyCurve || E.curves;
                levelsUsed << E.level;
                for (const QJsonValue& s : E.series) series.append(s);
                if (!E.name.isEmpty() && !plotNames.contains(E.name)) plotNames << E.name;
                if (!E.trace.isEmpty()) { out.extraPlots << E.trace; anyTraces = true; }
            }
            continue;
        }
        // Ligne d'un tableau : résumée dans le récapitulatif ; fiche détaillée seulement s'il y a peu de lignes.
        const int tableRow = rq0.value("_table_row").toInt(-1);
        struct Restore {
            QString& h; QString& p; qsizetype hl, pl; bool on;
            ~Restore() { if (on) { h.truncate(hl); p.truncate(pl); } }
        } restore{ html, prompt, html.size(), prompt.size(), tableCompact && tableRow >= 0 };
        QString head = reg.describe(rq);
        // Nom de la datation (champ « name », notamment d'un fichier .chr) : en tête de la fiche et dans le titre du graphique.
        const QString dateName = rq.value("name").toString().trimmed();
        if (!dateName.isEmpty()) head = head.isEmpty() ? dateName : QStringLiteral("%1 — %2").arg(dateName, head);
        MethodResult r = reg.compute(rq);
        const Density measureDensity = r.ok ? r.density : Density();     // densité de la mesure seule : graphique de la courbe

        // Décalage (wiggle) d'une date seule : comme ChronoModel (Date::calibrateWiggle), la densité de t + δ est la
        // convolution de la densité calibrée par la loi de δ ; HPD et statistiques sont recalculées sur cette densité.
        Wiggle wg;
        QString werr;
        if (r.ok && !wiggleFromJson(rq.value("wiggle"), wg, werr)) r = MethodResult::failure(werr);
        if (r.ok && wg.type != Wiggle::None) {
            head += QStringLiteral(" — avec %1 (densité de t + δ)").arg(wiggleText(wg));
            r.density = wiggleDensity(r.density, wg);
            r.hpd95 = hpdIntervals(r.density, 95.0);
            r.hpd68 = hpdIntervals(r.density, 68.0);
            r.stats = densityStats(r.density);
        }

        // Période d'étude (tmin / tmax, années calendaires signées) : comme ChronoModel, la densité est calculée sur
        // cette période seulement, puis ramenée à une aire de 1. Sans période : toute la courbe de référence.
        QString periodNote;
        const double pmin = numOf(rq, { "tmin" }, std::nan("")), pmax = numOf(rq, { "tmax" }, std::nan(""));
        if (r.ok && (std::isfinite(pmin) || std::isfinite(pmax))) {
            const double lo = std::isfinite(pmin) ? pmin : -std::numeric_limits<double>::infinity();
            const double hi = std::isfinite(pmax) ? pmax : std::numeric_limits<double>::infinity();
            Density d;
            d.step = r.density.step;
            for (size_t i = 0; i < r.density.t.size(); ++i)
                if (r.density.t[i] >= lo && r.density.t[i] <= hi) { d.t.push_back(r.density.t[i]); d.v.push_back(r.density.v[i]); }
            const double full = densityArea(r.density), kept = d.valid() ? densityArea(d) : 0.0;
            periodNote = QStringLiteral("période d'étude %1 → %2")
                             .arg(std::isfinite(pmin) ? dateformat::format(pmin, fmt) : QStringLiteral("…"),
                                  std::isfinite(pmax) ? dateformat::format(pmax, fmt) : QStringLiteral("…"));
            if (!(kept > 0)) {
                r = MethodResult::failure(QStringLiteral("aucune probabilité sur la %1").arg(periodNote));
            } else {
                const bool cutOld = d.t.front() > r.density.t.front() + 0.5 * r.density.step;
                const bool cutNew = d.t.back() < r.density.t.back() - 0.5 * r.density.step;
                for (double& y : d.v) y /= kept;
                QStringList warn;
                for (const QString& w : r.warnings)        // la limite de la courbe n'est plus en cause si la période la coupe avant
                    if (!(cutOld && w.contains(QLatin1String("limite ancienne"))) && !(cutNew && w.contains(QStringLiteral("limite récente"))))
                        warn << w;
                if (full > 0 && kept / full < 0.99)
                    warn << QStringLiteral("la %1 ne contient que %2 % de la probabilité calculée sur toute la courbe : résultat restreint à cette période")
                                .arg(periodNote, text::numAuto(100.0 * kept / full, 1));
                r.density = d;
                r.hpd95 = hpdIntervals(d, 95.0);
                r.hpd68 = hpdIntervals(d, 68.0);
                r.stats = densityStats(d);
                r.warnings = warn;
            }
            head += QStringLiteral(" — %1").arg(periodNote);
        }

        html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
        prompt += QStringLiteral("\n%1\n").arg(head);
        if (const QStringList cl = curveLabels(reg.referenceCurves(rq)); !cl.isEmpty()) {
            html += QStringLiteral("<p><i>Courbe%1 de référence : %2</i></p>").arg(cl.size() > 1 ? "s" : "", cl.join(" ; ").toHtmlEscaped());
            prompt += QStringLiteral("Courbe%1 de référence : %2.\n").arg(cl.size() > 1 ? "s" : "", cl.join(" ; "));
            anyCurve = true;
            if (tableRow >= 0) for (const QString& c : cl) if (!recapCurves.contains(c)) recapCurves << c;
        }

        if (!r.ok) {
            html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(r.error.toHtmlEscaped());
            prompt += QStringLiteral("Calcul impossible : %1.\n").arg(r.error);
            if (tableRow >= 0) recap << RecapRow{ tableRow, dateName, reg.describe(rq), QString(), QString(), QStringLiteral("calcul impossible : ") + r.error };
            continue;
        }

        // Niveau de confiance demandé (95 % par défaut) ; 95 % et 68 % restent donnés en complément.
        QString levelWarning;
        const double level = readLevel(rq, levelWarning);
        if (!levelWarning.isEmpty()) {
            html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(levelWarning.toHtmlEscaped());
            prompt += QStringLiteral("Avertissement : %1.\n").arg(levelWarning);
        }
        levelsUsed << level;
        appendSummary(html, prompt, r.density, r.hpd95, r.hpd68, r.stats, level, fmt);
        if (tableRow >= 0) {
            recapLevel = level;
            recap << RecapRow{ tableRow, dateName, reg.describe(rq) + (wg.type != Wiggle::None ? QStringLiteral(" + δ") : QString()),
                               intervalsText(std::abs(level - 95.0) < 1e-9 ? r.hpd95 : hpdIntervals(r.density, level), fmt).replace('\n', QStringLiteral(" ; ")),
                               dateformat::format(r.stats.median, fmt), r.warnings.join(QStringLiteral(" ; ")) };
        }
        for (const QString& w : r.warnings) html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());
        for (const QString& w : r.warnings) prompt += QStringLiteral("Avertissement : %1.\n").arg(w);

        QJsonObject ser = plotSeries(head, r.density, fmt);
        if (const QString col = colorOf(rq); !col.isEmpty()) ser["color"] = col;
        series.append(ser);
        if (!dateName.isEmpty() && !plotNames.contains(dateName)) plotNames << dateName;

        // Courbe(s) de référence de la datation, avec la mesure et son erreur (onglet à part ; 8 au plus par calcul, pas pour
        // les lignes d'un grand tableau).
        if (refPlotCount < 8 && !(tableRow >= 0 && tableDates > 3)) {
            const QVector<QJsonObject> rp = referenceCurvePlots(rq, measureDensity, dateName, fmt);
            for (const QJsonObject& pl : rp) {
                if (refPlotCount >= 8) break;
                out.extraPlots << pl;
                ++refPlotCount;
            }
            if (!rp.isEmpty()) {
                const QString w = wg.type != Wiggle::None ? QStringLiteral(" (mesure seule, sans le décalage δ)") : QString();
                html += QStringLiteral("<p><small>📈 Courbe%1 de référence tracée%1 avec la mesure et son erreur (1σ), et la densité "
                                       "calibrée%2 : onglet%1 à part.</small></p>").arg(rp.size() > 1 ? "s" : "", w.toHtmlEscaped());
                prompt += QStringLiteral("Le graphique de la courbe de référence (moyenne ± 1,96 σ), avec la mesure en trait et son erreur à "
                                         "1σ, est DÉJÀ tracé dans un onglet à part%1.\n").arg(w);
            }
        }
    }

    // Récapitulatif des lignes d'un tableau datées une par une (au début de la fiche).
    if (!recap.isEmpty()) {
        const QString lv = text::numAuto(recapLevel, 2);
        QString h = QStringLiteral("<p><b>Récapitulatif</b> (%1 datation(s)%2)</p><table border=\"1\" cellspacing=\"0\" cellpadding=\"3\">"
                                   "<tr><th>#</th><th>Nom</th><th>Mesure</th><th>HPD %3 %</th><th>Médiane</th><th>Remarque</th></tr>")
                        .arg(recap.size()).arg(tableCompact ? QStringLiteral(", sans fiche détaillée au-delà de 12 lignes") : QString(), lv);
        QString p = QStringLiteral("Récapitulatif des datations du tableau (HPD %1 %, médiane) :\n").arg(lv);
        for (const RecapRow& x : std::as_const(recap)) {
            h += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td></tr>")
                     .arg(x.row).arg(x.name.toHtmlEscaped(), x.measure.toHtmlEscaped(), x.hpd.toHtmlEscaped(), x.median.toHtmlEscaped(), x.note.toHtmlEscaped());
            p += QStringLiteral("#%1 %2 : %3%4%5\n").arg(x.row).arg(x.name, x.hpd.isEmpty() ? QString() : QStringLiteral("HPD ") + x.hpd,
                                                                      x.median.isEmpty() ? QString() : QStringLiteral(" ; médiane ") + x.median,
                                                                      x.note.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(x.note));
        }
        h += QStringLiteral("</table>");
        if (tableCompact && !recapCurves.isEmpty()) {
            h += QStringLiteral("<p><i>Courbe(s) de référence : %1</i></p>").arg(recapCurves.join(QStringLiteral(" ; ")).toHtmlEscaped());
            p += QStringLiteral("Courbe(s) de référence : %1.\n").arg(recapCurves.join(QStringLiteral(" ; ")));
        }
        html.insert(recapHtmlPos, h);
        prompt.insert(recapPromptPos, p);
    }
    // Beaucoup de densités : un graphique lisible garde les 40 premières.
    if (series.size() > 40) {
        const int n = int(series.size());
        while (series.size() > 40) series.removeLast();
        html += QStringLiteral("<p><i>Graphique limité aux 40 premières datations sur %1.</i></p>").arg(n);
    }

    // Remplissage du graphique : niveau demandé s'il est le même pour toutes les dates, sinon 95 %.
    double plotLevel = levelsUsed.isEmpty() ? kDefaultLevel : levelsUsed.first();
    for (double l : levelsUsed) if (std::abs(l - plotLevel) > 1e-9) plotLevel = kDefaultLevel;
    const bool custom = std::any_of(levelsUsed.begin(), levelsUsed.end(), [](double l) { return std::abs(l - kDefaultLevel) > 1e-9; });
    const QString summary = custom
        ? QStringLiteral("Résume d'abord par la HPD au niveau demandé par l'utilisateur, puis par les HPD à 95 % et 68 % pour comparaison")
        : QStringLiteral("Résume par les HPD à 95 % et 68 %");
    if (curveRuns > 0 && curveRuns == todo.size()) {
        prompt += QStringLiteral("\nConsigne : présente ces résultats pour un archéologue ou un géologue : ce sont des probabilités a "
                                 "posteriori sur la courbe estimée par ChronoModel (chaque tirage est une courbe complète, avec les "
                                 "contraintes et les données du modèle). Les graphiques (loi de la grandeur, courbe médiane et son "
                                 "intervalle de crédibilité à 95 %, quantiles 2,5 – 97,5 % date par date, pas une HPD) sont DÉJÀ tracés dans des onglets à part : n'écris aucun bloc « plot ». %1 Une "
                                 "vitesse est en unités de la composante par an (ou par la durée indiquée) ; une vitesse moyenne entre "
                                 "deux dates n'est pas la vitesse instantanée. Une accélération (dérivée seconde) est en unités par an² (ou par la durée "
                                 "indiquée, au carré) ; pour une courbe âge-profondeur, la vitesse est le taux d'accumulation. Une date d'atteinte d'une valeur peut être multiple si la "
                                 "courbe n'est pas monotone : signale-le s'il est indiqué. Signale une convergence insuffisante si elle "
                                 "est indiquée. N'invente aucune autre valeur.\n")
                      .arg(fmt == DateFormat::BcAd ? QStringLiteral("Les années sont des années astronomiques signées.")
                                                   : QStringLiteral("Les dates sont exprimées en %1.").arg(dateformat::title(fmt)));
    } else
    prompt += QStringLiteral("\nConsigne : présente ces résultats (intervalles, éventuelle multimodalité, précision) "
                             "pour un archéologue et rappelle les hypothèses de la méthode. Le graphique est DÉJÀ tracé par l'application "
                             "dans le panneau de droite (%4, région HPD à %2 % remplie, sans échelle verticale) : n'écris aucun "
                             "bloc « plot » pour ces densités et ne décris aucun autre élément graphique (couleurs, légende, intervalles "
                             "en rouge…). %1 %3 : jamais de « 1σ » ou « 2σ » pour une densité calibrée. N'invente aucune autre valeur.\n")
                  .arg(fmt == DateFormat::BcAd ? QStringLiteral("Les années sont des années astronomiques signées, dans la fiche comme dans les graphiques.")
                                               : QStringLiteral("Les dates sont exprimées en %1.").arg(dateformat::title(fmt)),
                       levelText(plotLevel), summary,
                       overlay ? QStringLiteral("densités superposées, avec une légende")
                       : series.size() > 1 ? QStringLiteral("une densité par ligne, sur un axe des dates commun, chacune nommée")
                       : anyColor ? QStringLiteral("courbe de la couleur indiquée") : QStringLiteral("courbe bleue"));
    if (anyCurve)
        prompt += QStringLiteral("Rappelle dans ta réponse la ou les courbes de référence utilisées, telles qu'indiquées ci-dessus "
                                 "(nom et origine : intégrée à SeldonH, ChronoModel ou dossier de l'utilisateur).\n");
    if (anyInverse)
        prompt += QStringLiteral("Pour la calibration inverse : c'est la mesure qu'on obtiendrait pour un échantillon de cette date, lue sur "
                                 "la courbe de référence (valeur ± erreur de la courbe, plus l'incertitude de la date et l'erreur de mesure "
                                 "si elles sont données) ; le graphique (à part) est sur l'axe de la mesure, pas des dates. Signale les "
                                 "plateaux ou inversions de la courbe s'ils rendent la date peu discriminante.\n");
    if (anyCurveTrace && curveRuns < todo.size())
        prompt += QStringLiteral("Pour la courbe de ChronoModel : probabilités a posteriori sur les courbes tirées (une courbe complète par "
                                 "tirage) ; les graphiques de la courbe sont dans des onglets à part.\n");
    if (anyPosterior)
        prompt += QStringLiteral("Pour les traces MCMC : ces valeurs sont calculées sur les tirages a posteriori du modèle ChronoModel de "
                                 "l'utilisateur, tirage par tirage ; elles dépendent de ce modèle (contraintes, phases). Une durée est en "
                                 "années (graphique à part) ; une « date entre A et B » suppose une date uniforme entre A et B à chaque "
                                 "tirage. Une probabilité d'antériorité ou d'appartenance est la proportion des tirages : c'est une "
                                 "probabilité a posteriori dans ce modèle, pas un test de signification ni une p-value ; si une contrainte "
                                 "stratigraphique impose l'ordre, elle vaut 100 % par construction. Signale une convergence insuffisante si "
                                 "elle est indiquée.\n");
    if (anyCombine)
        prompt += QStringLiteral("Pour la combinaison : la densité combinée (en haut du graphique) est le produit des vraisemblances des "
                                 "datations, qui datent toutes le même moment (pour une datation à décalage δ, t + δ) ; chaque datation "
                                 "est tracée seule sur sa ligne en dessous. Le test de Ward et Wilson n'est qu'une information sur l'accord "
                                 "des mesures : ne le présente jamais comme une condition pour combiner, ni comme une preuve. Contrairement "
                                 "à un événement (MCMC), une combinaison ne prévoit aucune dispersion propre à chaque date : une date "
                                 "discordante pèse autant que les autres.\n");
    if (anyEvent)
        prompt += QStringLiteral("Pour l'événement : la densité a posteriori de θ est la courbe remplie, les calibrations individuelles des "
                                 "datations sont tracées en traits%1. Explique ce que "
                                 "signifie θ (date commune de l'événement), commente l'accord entre les datations (σ_i grand = date "
                                 "discordante, peu prise en compte) et la convergence (R̂, ESS) ; si la convergence n'est pas satisfaisante, "
                                 "conseille d'augmenter le nombre d'itérations ou de chaînes.\n")
                      .arg(anyTraces ? QStringLiteral(", et les traces des chaînes MCMC sont dans un autre onglet")
                                     : QStringLiteral(" ; les traces des chaînes MCMC ne sont pas tracées (l'utilisateur peut les demander)"));

    if (!series.isEmpty()) {
        out.plot["title"] = tools::datingPlotTitle(plotNames, int(series.size()));
        out.plot["dates"] = QJsonArray::fromStringList(plotNames);     // repris par tools::mergePlots pour refaire le titre
        out.plot["xlabel"] = dateformat::axisLabel(fmt);
        if (dateformat::isAge(fmt)) out.plot["xreverse"] = true;      // ordre chronologique de gauche à droite
        out.plot["ylabel"] = QStringLiteral("Densité de probabilité");
        out.plot["fill"] = true;
        out.plot["yaxis"] = false;       // pas d'échelle verticale pour une densité
        out.plot["hpd"] = plotLevel;     // seule la région HPD (95 % ou niveau demandé) est remplie
        out.plot["series"] = series;
        out.plot["layout"] = overlay ? QStringLiteral("overlay") : QStringLiteral("rows");
    }
    // Titre : opérations dans l'ordre, les identiques regroupées (« Calibration 14C ×3 · Event « Four » (MCMC, 3 datations) »).
    QStringList uniq;
    QHash<QString, int> times;
    for (const QString& o : std::as_const(ops)) { if (!times.contains(o)) uniq << o; ++times[o]; }
    QStringList parts;
    for (const QString& o : std::as_const(uniq)) parts << (times.value(o) > 1 ? QStringLiteral("%1 ×%2").arg(o).arg(times.value(o)) : o);
    out.title = parts.join(QStringLiteral(" · "));
    if (out.title.size() > 200) out.title = out.title.left(199) + QChar(0x2026);
    out.noticeHtml = cardMarker(out.title) + html;
    out.promptBlock = prompt;
    return out;
}

QString MethodRunner::cardMarker(const QString& title)
{
    QString t = title.toHtmlEscaped();
    t.replace(QStringLiteral("--"), QStringLiteral("–"));      // un commentaire HTML ne peut pas contenir « -- »
    return QStringLiteral("<!--seldon-calc:%1-->").arg(t);
}

QString MethodRunner::operationTitle(const QJsonObject& request)
{
    const QJsonObject n = normalizeRequest(request);
    const QString m = n.value("method").toString();
    const QString name = n.value("name").toString().trimmed();
    const int nd = int(n.value("dates").toArray().size());
    const QString s = nd > 1 ? QStringLiteral("s") : QString();
    if (m == QLatin1String("c14")) return QStringLiteral("Calibration 14C");
    if (m == QLatin1String("f14c")) return QStringLiteral("Calibration F14C");
    if (m == QLatin1String("tl")) return QStringLiteral("Datation TL/OSL");
    if (m == QLatin1String("am")) {
        const QString p = n.value("process").toString();
        return p.isEmpty() ? QStringLiteral("Calibration archéomagnétique") : QStringLiteral("Calibration archéomagnétique (%1)").arg(p);
    }
    if (m == QLatin1String("gauss"))
        return n.value("mode").toString() == QLatin1String("eq") ? QStringLiteral("Date gaussienne calibrée par équation")
                                                                 : QStringLiteral("Date gaussienne");
    if (m == QLatin1String("unif")) return QStringLiteral("Date uniforme");
    if (m == QLatin1String("density")) return QStringLiteral("Densité tabulée");
    if (m == QLatin1String("event"))
        return QStringLiteral("%1 (MCMC, %2 datation%3)")
            .arg(name.isEmpty() ? QStringLiteral("Event") : QStringLiteral("Event « %1 »").arg(name)).arg(nd).arg(s);
    if (m == QLatin1String("combine"))
        return QStringLiteral("Combinaison de %1 datation%2%3").arg(nd).arg(s, name.isEmpty() ? QString() : QStringLiteral(" « %1 »").arg(name));
    if (m == QLatin1String("inverse")) return QStringLiteral("Calibration inverse");
    if (m == QLatin1String("density_file")) return QStringLiteral("Fichier de densité pour ChronoModel");
    if (m == QLatin1String("relation")) return QStringLiteral("Relation entre dates");
    if (m == QLatin1String("curve")) return QStringLiteral("Courbe de ChronoModel");
    if (m == QLatin1String("information")) return QStringLiteral("Courbes disponibles");
    return m.isEmpty() ? QStringLiteral("Calcul") : QStringLiteral("Calcul « %1 »").arg(m);
}

QString MethodRunner::obsoleteFieldError(const QJsonObject& request)
{
    static const QHash<QString, QString> renamed = {
        { "refYear", "ref_year" }, { "refyear", "ref_year" }, { "errorF", "error_f" }, { "errorf", "error_f" },
        { "deltaR", "delta_r" }, { "deltaRerr", "delta_r_error" }, { "deltaR_error", "delta_r_error" },
        { "curveI", "curve_i" }, { "curveD", "curve_d" }, { "curveF", "curve_f" }, { "steps", "integration_steps" } };
    QStringList bad;
    for (auto it = request.begin(); it != request.end(); ++it)
        if (renamed.contains(it.key()))
            bad << QStringLiteral("champ « %1 » inconnu : écrire « %2 »").arg(it.key(), renamed.value(it.key()));
    for (const QJsonValue& d : request.value("dates").toArray())
        if (const QString e = obsoleteFieldError(d.toObject()); !e.isEmpty()) bad << e;
    bad.removeDuplicates();
    return bad.join(QStringLiteral(" ; "));
}

QJsonObject MethodRunner::expandPlotLaws(const QJsonObject& plotIn, QString* err)
{
    QJsonObject plot = plotIn;
    QJsonArray series = plot.value("series").toArray();
    if (series.isEmpty() && (plot.contains("law") || plot.contains("loi"))) {       // forme courte : loi à la racine
        QJsonObject s;
        for (const char* k : { "law", "loi", "mean", "sd", "meanlog", "sdlog", "median", "factor", "min", "max", "mode", "mode_min",
                               "mode_max", "rate", "shape", "scale", "alpha", "beta", "offset", "reverse", "tmin", "tmax", "step",
                               "points", "name", "color" })
            if (plot.contains(k)) { s[k] = plot.value(k); if (QLatin1String(k) != QLatin1String("color")) plot.remove(k); }
        series.append(s);
    }
    const QJsonValue rootX = plot.value("x");
    bool any = false;
    QString firstDesc;
    for (int i = 0; i < series.size(); ++i) {
        QJsonObject s = series[i].toObject();
        if (!s.contains("law") && !s.contains("loi")) {
            if (!s.contains("x") && rootX.isArray()) s["x"] = rootX;     // la série garde l'abscisse commune
            series[i] = s;
            continue;
        }
        QJsonObject spec = s;
        spec.remove("x"); spec.remove("y"); spec.remove("name");
        if (!spec.contains("points") && !spec.contains("step")) spec["points"] = 801;
        const DensityFileResult d = buildDensityFile(spec);
        if (!d.ok) {
            if (err) *err = d.error;
            return {};
        }
        QJsonArray xs, ys;
        for (size_t k = 0; k < d.density.t.size(); ++k) { xs.append(d.density.t[k]); ys.append(d.density.v[k]); }
        QJsonObject out;
        for (const char* k : { "color", "fill", "width" }) if (s.contains(k)) out[k] = s.value(k);
        out["name"] = s.value("name").toString(d.description);
        out["x"] = xs;
        out["y"] = ys;
        series[i] = out;
        if (firstDesc.isEmpty()) firstDesc = d.description;
        any = true;
    }
    if (!any) return plotIn;
    plot.remove("x");
    plot["series"] = series;
    if (!plot.contains("title")) plot["title"] = firstDesc;
    if (!plot.contains("fill")) plot["fill"] = true;
    return plot;
}

bool MethodRunner::isModelRequest(const QString& question)
{
    const QString q = text::fold(question);
    static const QRegularExpression model(QStringLiteral(R"(\b(modele|modeles|modeliser|modelisation|model|models|modelling|modeling)\b|\.chr\b|projet chronomodel|chronomodel project)"));
    static const QRegularExpression context(QStringLiteral(
        R"(\b(chronomodel|evenement|evenements|phase|phases|contrainte|contraintes|stratigraph\w*|fais|faire|cree|creer|construi\w*|genere|generer|prepare|exporte\w*)"
        R"(|event|events|constraints?|bounds?|create|build|make|generate|export\w*)\b)"));
    // Courbe âge-profondeur ou tableau CSV de ChronoModel : c'est un modèle, même sans le mot « modèle ».
    if (isAgeDepthRequest(question)) return true;
    return model.match(q).hasMatch() && context.match(q).hasMatch();
}

QStringList MethodRunner::datingMethodsIn(const QString& question)
{
    const QString q = text::fold(question);
    static const QVector<QPair<QString, QRegularExpression>> pats = {
        { "c14", QRegularExpression(QStringLiteral(R"(\b(?:14\s?c|c\s?-?14|radiocarbones?|radiocarbon|carbone 14|carbon 14|intcal\w*|shcal\w*|marine\d*)\b|\d\s*bp\b|\bbp\b)")) },
        { "f14c", QRegularExpression(QStringLiteral(R"(\bf14c\b|fraction moderne|post-?bombe?|\bbomb\w*|\bpmc\b)")) },
        { "tl", QRegularExpression(QStringLiteral(R"(\b(?:tl|osl|irsl|luminescence|thermoluminescence)\b)")) },
        { "am", QRegularExpression(QStringLiteral(R"(\b(?:am|archeomag\w*|archaeomag\w*|paleomag\w*|palaeomag\w*|inclinaisons?|declinaisons?|inclinations?|declinations?|alpha\s?95|a95|paleointensite\w*|intensites?)\b|α95)")) },
        { "gauss", QRegularExpression(QStringLiteral(R"(\b(?:gauss\w*|unif\w*|normale|tpq|taq|terminus|dendro\w*|monnaies?|typolog\w*|numismat\w*)\b|\bentre\s+-?\d+\s+et\s+-?\d+)")) },
        { "density", QRegularExpression(QStringLiteral(R"(densite\s+tabulee|fichier\s+de\s+densite|plugin\s+density|\bdensity\b)")) } };
    QStringList out;
    for (const auto& p : pats)
        if (p.second.match(q).hasMatch()) out << p.first;
    return out;
}

bool MethodRunner::isAgeDepthRequest(const QString& question)
{
    const QString q = text::fold(question);
    static const QRegularExpression ageDepth(QStringLiteral(
        R"(\bage[\s-]*(?:profondeur|depth)\b|\bcourbe\s+(?:de\s+)?profondeur\b|\bdepth[\s-]*(?:age|curve)\b|\bmodele\s+d'age\b)"));
    // En-tête du tableau d'import de ChronoModel : « method;dating name/code;param a;… ».
    static const QRegularExpression csvHeader(QStringLiteral(R"(\bmethod\s*[;,\t]\s*dating\s+name)"));
    return ageDepth.match(q).hasMatch() || csvHeader.match(q).hasMatch();
}

static QStringList fuzzyKeepNames();

RunOutcome MethodRunner::analyzeQuestion(const QString& questionIn, DateFormat format)
{
    QString question = questionIn, fixes;
    QVector<QJsonObject> rq = requestsFromQuestionExact(question);
    if (rq.isEmpty()) {                      // mots déclencheurs mal orthographiés : question corrigée (FuzzyWords)
        QVector<fuzzy::Fix> fx;
        const QString corrected = fuzzy::correct(question, &fx, fuzzyKeepNames());
        if (!fx.isEmpty()) {
            rq = requestsFromQuestionExact(corrected);
            if (!rq.isEmpty()) { question = corrected; fixes = fuzzy::describe(fx); }
        }
    }
    if (rq.isEmpty()) return RunOutcome();
    DateFormat fmt = dateformat::detect(question, format);
    // Mesures toutes en ka (ou Ma), sans format demandé et réglage en années calendaires : résultats en ka BP (Ma BP), l'usage
    // des géologues (05/11/2026).
    if (fmt == format && (format == DateFormat::BcAd || format == DateFormat::Historic || format == DateFormat::BceCe)) {
        const double sc = text::questionAgeScale(text::fold(question));
        if (sc == 1e3) fmt = DateFormat::KaBP;
        else if (sc == 1e6) fmt = DateFormat::MaBP;
    }
    double a = 0, b = 0;
    const bool curveOnly = rq.size() == 1 && rq.first().value("method").toString() == QLatin1String("curve");   // « entre » = dates ou valeurs
    if (!curveOnly && periodFromQuestion(question, a, b)) {
        const double c1 = dateformat::toCalendar(a, fmt), c2 = dateformat::toCalendar(b, fmt);
        for (QJsonObject& o : rq) { o["tmin"] = std::min(c1, c2); o["tmax"] = std::max(c1, c2); }
    }
    RunOutcome out = run(rq, fmt);
    if (!fixes.isEmpty()) {
        // Correction visible dans la fiche, après la marque de titre (« <!--seldon-calc:…--> ») que lit ChatView.
        const QString note = QStringLiteral("<p><i>↻ Compris : %1.</i></p>").arg(fixes.toHtmlEscaped());
        qsizetype at = 0;
        if (out.noticeHtml.startsWith(QLatin1String("<!--seldon-calc:"))) {
            const qsizetype e = out.noticeHtml.indexOf(QLatin1String("-->"));
            if (e >= 0) at = e + 3;
        }
        out.noticeHtml.insert(at, note);
        out.promptBlock += QStringLiteral("\nNote : avant le calcul, SeldonH a corrigé des mots mal orthographiés de la question : %1.\n").arg(fixes);
    }
    return out;
}

namespace {

// Tous les passages « wiggle … » d'une question, dans l'ordre : position, longueur et décalage lu.
struct WigglePassage { int start = 0, length = 0; QJsonObject wiggle; };
QVector<WigglePassage> wigglePassages(const QString& question, QString* masked = nullptr)
{
    QVector<WigglePassage> out;
    QString t = question;
    for (int guard = 0; guard < 12; ++guard) {
        int s = -1, l = 0;
        const QJsonObject w = MethodRunner::wiggleFromQuestion(t, &s, &l);
        if (w.isEmpty() || s < 0 || l <= 0) break;
        out.push_back({ s, l, w });
        t.replace(s, l, QString(l, QLatin1Char(' ')));       // passage masqué : le suivant est cherché après
    }
    std::sort(out.begin(), out.end(), [](const WigglePassage& a, const WigglePassage& b) { return a.start < b.start; });
    if (masked) *masked = t;
    return out;
}

// Décalage de chaque datation : un passage « wiggle … » vaut pour la paire « valeur ± erreur » qui le précède (la
// première s'il est écrit avant toute datation). Clé : valeur et erreur de la paire, après conversion des années av.
// J.-C., comme les lit la méthode. Clés sans décalage absentes.
struct WiggleKey { double value = 0, error = 0; QJsonObject wiggle; };
QVector<WiggleKey> wigglesByDate(const QString& masked, const QVector<WigglePassage>& passages)
{
    QVector<WiggleKey> out;
    if (passages.isEmpty()) return out;
    const QVector<text::ValueError> pairs = text::findValueErrors(text::fold(masked));    // fold garde les positions (texte NFC)
    if (pairs.isEmpty()) return out;
    for (int k = 0; k < pairs.size(); ++k) {
        const int a = pairs[k].start, b = k + 1 < pairs.size() ? pairs[k + 1].start : int(masked.size()) + 1;
        QJsonObject w;
        for (const WigglePassage& p : passages)
            if ((p.start >= a && p.start < b) || (k == 0 && p.start < a)) { w = p.wiggle; break; }
        if (w.isEmpty()) continue;
        // Valeur telle que la méthode la lira : « 500 ± 20 av. J.-C. » -> -499 ± 20.
        const QString chunk = text::fold(text::erasToAstronomical(masked.mid(a, b - a)));
        const QVector<text::ValueError> cp = text::findValueErrors(chunk);
        out.push_back({ cp.isEmpty() ? pairs[k].value : cp.first().value, cp.isEmpty() ? pairs[k].error : cp.first().error, w });
    }
    return out;
}

} // namespace

bool MethodRunner::periodFromQuestion(const QString& question, double& from, double& to)
{
    QString t;
    wigglePassages(question, &t);                            // « décalage entre 10 et 30 » n'est pas une période
    t = text::erasToAstronomical(t);                         // « 1000 av. J.-C. » -> -999 (année astronomique)
    const QString q = text::fold(t);
    const QString num = QStringLiteral(R"((-?\d+(?:[.,]\d+)?)\s*(av\.?\s*j\.?\s*-?\s*c\.?|avant\s+j\.?\s*-?\s*c\.?|bce?\b|apr\.?\s*j\.?\s*-?\s*c\.?|ad\b|ce\b)?)");
    const QString sep = QStringLiteral(R"(\s*(?:a|au|jusqu'a|et|-|–|→|to|and)\s*)");
    static const QRegularExpression explicitRe(QStringLiteral(
        R"(\b(?:periode\s+d'etude|study\s+period|periode|period|support|domaine|intervalle\s+d'etude|time\s+range)\s*(?:de|du|from|of|:|=|sur|entre|between)?\s*)")
        + num + sep + num);
    static const QRegularExpression rangeRe(QStringLiteral(R"(\b(?:entre|between|de|from)\s+)") + num + sep + num);
    auto val = [](const QString& n, const QString& era) {
        double v = text::toNumber(n);
        if (era.startsWith(QLatin1String("av")) || era.startsWith(QLatin1String("bc"))) v = 1.0 - std::abs(v);   // année astronomique
        return v;
    };
    QRegularExpressionMatch m = explicitRe.match(q);
    if (!m.hasMatch() && !q.contains(QLatin1String("unif"))) m = rangeRe.match(q);
    if (!m.hasMatch()) return false;
    from = val(m.captured(1), m.captured(2));
    to = val(m.captured(3), m.captured(4));
    return from != to;
}

QVector<QJsonObject> MethodRunner::followUpRequests(const QString& question, const QVector<QJsonObject>& previous, DateFormat format)
{
    // « Et à 50 % ? », « refais la datation entre 0 et 2000 » : pas de nouvelle datation, seulement un autre niveau
    // et/ou une période d'étude. On reprend la dernière datation calculée (hors événement : son MCMC serait relancé
    // avec d'autres tirages).
    const double level = requestedLevel(question);
    double a = 0, b = 0;
    const bool period = periodFromQuestion(question, a, b);
    if ((level <= 0 && !period) || previous.isEmpty() || !requestsFromQuestion(question).isEmpty() || isModelRequest(question)) return {};
    const DateFormat fmt = dateformat::detect(question, format);
    QVector<QJsonObject> out;
    for (QJsonObject o : previous) {
        o = normalizeRequest(o);
        if (o.value("method").toString() == QLatin1String("event") || o.value("method").toString() == QLatin1String("density_file")) continue;
        if (level > 0) o["level"] = level;
        if (period) {
            const double c1 = dateformat::toCalendar(a, fmt), c2 = dateformat::toCalendar(b, fmt);
            o["tmin"] = std::min(c1, c2);
            o["tmax"] = std::max(c1, c2);
        }
        out << o;
    }
    return out;
}

namespace {

// Mots d'une question sur deux ou trois termes (colonnes des traces, datations), sur le texte passé par fold().
struct RelationWords { bool duration = false, between = false, value = false, inside = false, order = false, correlation = false; };
RelationWords relationWords(const QString& q)
{
    static const QRegularExpression duration(QStringLiteral(
        R"(\b(?:durees?|ecarts?|ecoule\w*|combien d'annees|combien de temps|temps (?:entre|ecoule|separant|qui separe)|separ\w*|)"
        R"(intervalle de temps|difference|duration|how long|time between|elapsed|time span)\b)"));
    static const QRegularExpression between(QStringLiteral(
        R"(\b(?:date|niveau|couche|evenement|event|episode|moment|occupation|sol|structure)\b.{0,60}\b(?:entre|between)\b|\bintermediaire\b|\bsitue\w* entre\b|\bin between\b)"));
    static const QRegularExpression value(QStringLiteral(
        R"(\b(?:debut|fin|commencement|begin\w*|end|start|hpd|densite|date|quand|when|intervalle)\b)"));
    // Appartenance à un intervalle (deux termes, une phase, ou deux années) ; antéro-postérité de deux termes.
    static const QRegularExpression inside(QStringLiteral(
        R"(\b(?:appartien\w*|appartenance|dans l'intervalle|inclu\w*|compris\w* (?:entre|dans)|contenu\w*|a l'interieur|pendant|durant|)"
        R"(au cours de|fait partie|dans la phase|within|inside|during|belong\w*|est(?:-(?:il|elle)| (?:il|elle))?\s+(?:bien\s+)?entre|)"
        R"(se (?:situe|trouve)\w*(?:-t-(?:il|elle))? (?:entre|dans)|lies? between|is .{0,30} between)\b)"));
    static const QRegularExpression order(QStringLiteral(
        R"(\b(?:anterieur\w*|posterieur\w*|anteriorite|posteriorite|antero-?posteriorite|precede\w*|avant|apres|plus anciens?|)"
        R"(plus ancienne|plus recents?|plus recente|older|younger|earlier|later|before|after)\b)"));
    static const QRegularExpression correlation(QStringLiteral(
        R"(\b(?:correl\w*|covarian\w*|nuages? de points|scatter\s*plots?|dependance entre|dependence between|lien entre|)"
        R"(varient ensemble|vary together)\b)"));
    RelationWords w;
    w.correlation = correlation.match(q).hasMatch();
    w.duration = duration.match(q).hasMatch();
    w.between = between.match(q).hasMatch();
    w.value = value.match(q).hasMatch();
    w.inside = inside.match(q).hasMatch();
    w.order = order.match(q).hasMatch();
    return w;
}

// « compris entre -500 et -400 » : deux années fixes (texte en années astronomiques, passé par fold()).
bool fixedYearsIn(const QString& q, QString& from, QString& to)
{
    static const QRegularExpression years(QStringLiteral(R"(\b(?:entre|between|de|from)\s+(-?\d+(?:[.,]\d+)?)\s*(?:et|a|and|to|-)\s*(-?\d+(?:[.,]\d+)?))"));
    const auto m = years.match(q);
    if (!m.hasMatch()) return false;
    from = m.captured(1);
    to = m.captured(2);
    return true;
}

// Demande « relation » sur des termes rangés dans l'ordre de la question ; vide si l'opération n'est pas reconnue.
// `phase` : phase citée (bornes « X Begin » / « X End ») ; `fixed` : années fixes de la question pour « inside ».
QJsonObject relationRequest(const RelationWords& w, const QVector<QJsonValue>& t, const QString& phase, const QString& folded,
                            bool preferOrder)
{
    QJsonObject o{ { "method", "relation" } };
    QString y0, y1;
    if (w.correlation && t.size() >= 2) {                    // « corrélation entre US 12 et Four »
        o["op"] = QStringLiteral("correlation");
        QJsonArray terms;
        for (const QJsonValue& x : t) terms.append(x);
        o["terms"] = terms;
        return o;
    }
    if (preferOrder && t.size() == 2 && w.order && !w.between) {
        o["op"] = QStringLiteral("order");
        o["from"] = t[0]; o["to"] = t[1];
    } else if (t.size() >= 2 && w.duration) {
        o["op"] = QStringLiteral("duration");
        o["from"] = t[0]; o["to"] = t[1];
    } else if (w.inside && (t.size() >= 3 || (!t.isEmpty() && !phase.isEmpty()))) {
        // « N(300) appartient-il à la phase A ? », « US 5 est-il entre US 2 et US 8 ? »
        o["op"] = QStringLiteral("inside");
        o["of"] = t[0];
        if (t.size() >= 3) { o["from"] = t[1]; o["to"] = t[2]; }
        else { o["from"] = phase + QStringLiteral(" Begin"); o["to"] = phase + QStringLiteral(" End"); }
    } else if (w.inside && t.size() == 1 && fixedYearsIn(folded, y0, y1)) {
        // « N(300) est-il compris entre -500 et -400 ? » : bornes fixes
        o["op"] = QStringLiteral("inside");
        o["of"] = t[0]; o["from"] = y0; o["to"] = y1;
    } else if (t.size() >= 2 && w.order && !w.between) {
        // « N(500) est-il antérieur à N(200) ? »
        o["op"] = QStringLiteral("order");
        o["from"] = t[0]; o["to"] = t[1];
    } else if (t.size() >= 2 && w.between) {
        o["op"] = QStringLiteral("between");
        o["from"] = t[0]; o["to"] = t[1];
    } else if (t.size() == 1 && w.value) {
        o["op"] = QStringLiteral("value");
        o["of"] = t[0];
    } else {
        return {};
    }
    return o;
}

// Durée demandée entre deux datations simples (« combien de temps sépare A de B ? ») : du plus ancien au plus récent,
// d'après la médiane de chaque densité, pour une durée positive. Sans effet sur une autre opération ou un autre terme.
void durationOldestFirst(QJsonObject& rel)
{
    if (rel.value("op").toString() != QLatin1String("duration") || !rel.value("from").isObject() || !rel.value("to").isObject()) return;
    const MethodRegistry& reg = MethodRegistry::instance();
    auto median = [&](const QJsonObject& o) {
        QJsonObject d = MethodRunner::normalizeRequest(o);
        if (!reg.byId(d.value("method").toString())) return std::nan("");
        if (d.value("method").toString() == QLatin1String("am")) { d["process"] = d.value("process").toString("I").toUpper(); d["integration_steps"] = 500; }
        const MethodResult r = reg.compute(d);
        return r.ok && r.density.valid() ? densityStats(r.density).median : std::nan("");
    };
    const double a = median(rel.value("from").toObject()), b = median(rel.value("to").toObject());
    if (std::isfinite(a) && std::isfinite(b) && b < a) { const QJsonValue f = rel.value("from"); rel["from"] = rel.value("to"); rel["to"] = f; }
}

// Position dans `q` (texte passé par fold()) de la valeur principale d'une datation, à partir de `from` ; -1 sinon.
int datingPosition(const QJsonObject& d, const QString& q, int from)
{
    for (const char* k : { "age", "f14c", "inc", "dec", "field", "min", "max" }) {
        if (!d.value(k).isDouble()) continue;
        const double v = d.value(k).toDouble();
        for (const QString& s : { QString::number(v, 'g', 12), QString::number(v, 'g', 12).replace('.', ','),
                                  QString::number(std::abs(v), 'g', 12) }) {
            static const QRegularExpression digitsAround(QStringLiteral(R"(\d)"));
            int p = int(q.indexOf(s, from));
            while (p >= 0) {
                const bool before = p > 0 && digitsAround.match(q.mid(p - 1, 1)).hasMatch();
                const bool after = p + s.size() < q.size() && digitsAround.match(q.mid(p + s.size(), 1)).hasMatch();
                if (!before && !after) return p;
                p = int(q.indexOf(s, p + 1));
            }
        }
    }
    return -1;
}

} // namespace

QVector<QJsonObject> MethodRunner::relationWithPrevious(const QString& question, const QVector<QJsonObject>& previous)
{
    if (previous.isEmpty() || isModelRequest(question) || asksEvent(question) || asksCombine(question)) return {};
    const QString q = text::fold(question);
    static const QRegularExpression ref(QStringLiteral(
        R"(\b(?:precedente?s?|d'avant|derniere?s?|ces deux|les deux|deux (?:dates|datations|mesures|ages|echantillons)|celle-ci|celle-la|)"
        R"(premiere date|date d'avant|previous|last one|these two|both|the two|former|latter)\b)"));
    if (!ref.match(q).hasMatch()) return {};
    const MethodRegistry& reg = MethodRegistry::instance();
    const QVector<QJsonObject> now = requestsFromQuestion(question);
    for (const QJsonObject& o : now)
        if (!reg.byId(o.value("method").toString())) return {};          // déjà une relation, un événement, un tableau…
    // Terme repris de la conversation : nommé d'après sa mesure (« 3560 ± 20 BP ») s'il n'a pas de nom.
    auto term = [&](QJsonObject o) {
        if (o.contains("_column")) return QJsonValue(o.value("_column").toString());     // colonne des traces jointes
        for (const char* k : { "level", "layout" }) o.remove(k);
        if (o.value("name").toString().trimmed().isEmpty()) {
            QString d = reg.describe(normalizeRequest(o));
            if (const qsizetype k = d.indexOf(QStringLiteral(" — ")); k > 0) d = d.left(k);      // « 14C : 3560 ± 20 BP »
            if (const qsizetype k = d.indexOf(QStringLiteral(" : ")); k > 0 && k < 12) d = d.mid(k + 3);   // « 3560 ± 20 BP »
            if (!d.isEmpty()) o["name"] = d;
        }
        return QJsonValue(o);
    };
    QVector<QJsonValue> t;
    if (now.size() == 1) t = { term(previous.last()), term(now.first()) };
    else if (now.isEmpty() && previous.size() >= 2) t = { term(previous[previous.size() - 2]), term(previous.last()) };
    else return {};
    RelationWords w = relationWords(q);
    w.order = asksOrder(question);
    w.value = w.inside = false;
    QJsonObject rel = relationRequest(w, t, QString(), q, true);
    if (rel.isEmpty()) return {};
    durationOldestFirst(rel);
    if (const double lv = requestedLevel(question); lv > 0) rel["level"] = lv;
    return { rel };
}

bool MethodRunner::asksRelation(const QString& question)
{
    const RelationWords w = relationWords(text::fold(question));
    return w.duration || w.between || w.inside || w.correlation || asksOrder(question);
}

QVector<QJsonObject> MethodRunner::tracesFromQuestion(const QString& question)
{
    if (traces::loaded().isEmpty()) return {};
    const QVector<traces::Mention> ms = traces::mentions(question);
    const QStringList phases = traces::phasesIn(question);
    if (ms.isEmpty() && phases.isEmpty()) return {};
    const QString q = text::fold(question);
    const RelationWords w = relationWords(q);

    // Datations écrites dans la question (« Ly-1234 2450 ± 30 BP ») avec les colonnes : relation mixte. Les colonnes
    // citées sont masquées avant de lire les datations (« N(500) » ressemble à une loi normale).
    QVector<QPair<int, QJsonValue>> terms;
    for (const traces::Mention& m : ms) terms.append({ m.pos, QJsonValue(m.column) });
    if (q.size() == question.size()) {                       // fold() garde les positions : masque sur le texte d'origine
        QString masked = question;
        for (const traces::Mention& m : ms)
            for (int i = m.pos; i < std::min<int>(int(masked.size()), m.pos + int(m.column.size())); ++i) masked[i] = QLatin1Char(' ');
        static const QRegularExpression measured(QStringLiteral(R"(±|\+/-|\+-|\bBP\b|\bF14C\b|\balpha\s*95\b|α95|uniforme?\b)"),
                                                 QRegularExpression::CaseInsensitiveOption);
        if (measured.match(masked).hasMatch()) {
            const QString maskedFold = text::fold(masked);
            int from = 0;
            for (QJsonObject d : MethodRegistry::instance().parse(text::erasToAstronomical(masked))) {
                const int p = datingPosition(d, maskedFold, from);
                d.remove("level");
                terms.append({ p >= 0 ? p : int(q.size()) + int(terms.size()), QJsonValue(d) });
                if (p >= 0) from = p + 1;
            }
        }
    }
    std::stable_sort(terms.begin(), terms.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QVector<QJsonValue> t;
    for (const auto& x : terms) t << x.second;
    // « Durée de la phase A » : de son début à sa fin, si l'export contient les deux bornes.
    if (t.isEmpty() && phases.size() == 1 && w.duration && !w.correlation) {
        int s0 = -1, c0 = -1;
        const QString b = phases.first() + QStringLiteral(" Begin"), e = phases.first() + QStringLiteral(" End");
        if (traces::resolve(b, s0, c0) && traces::resolve(e, s0, c0))
            return { QJsonObject{ { "method", "relation" }, { "op", "duration" }, { "from", b }, { "to", e } } };
    }
    if (t.isEmpty()) return {};
    const QJsonObject o = relationRequest(w, t, phases.value(0), text::fold(text::erasToAstronomical(question)), false);
    if (o.isEmpty()) return {};
    // Sans colonne citée, seule l'appartenance d'une datation à une phase des traces concerne les traces.
    if (ms.isEmpty() && !(o.value("op").toString() == QLatin1String("inside") && o.value("to").toString().endsWith(QLatin1String(" End"))))
        return {};
    return { o };
}

QJsonObject MethodRunner::curveFromQuestion(const QString& question)
{
    if (!curves::anyLoaded()) return {};
    QString q = text::fold(text::erasToAstronomical(question));
    q.replace(QChar(0x2212), '-').replace(QChar(0x00A0), ' ').replace(QChar(0x2019), '\'');
    static const QString num = QStringLiteral(R"((-?\d+(?:[.,]\d+)?))");
    static const QRegularExpression curveWord(QStringLiteral(
        R"(\b(?:courbes?|splines?|curves?|composante|dimension|component|inclinaison|declinaison|intensite)\b|\b(?:[xyzidft]|inc|dec)\s*=)"));
    static const QRegularExpression rateWord(QStringLiteral(
        R"(vitesse|taux de variation|\bpentes?\b|derivee|\brates?\b|\bspeed|velocity|rythme|accumulation|taux de sedimentation)"));
    // Accélération (dérivée seconde) et demande de tracé des courbes (05/11/2026).
    static const QRegularExpression accWord(QStringLiteral(R"(acceleration|derivee seconde|second derivative|\bcourbure\b)"));
    static const QRegularExpression plotWord(QStringLiteral(
        R"(\b(?:trace|tracer|tracez|traces? moi|dessine\w*|affiche\w*|montre\w*|plot|draw|graphiques?|graphs?)\b)"));
    static const QRegularExpression crossWord(QStringLiteral(
        R"(\bquand\b|a quelle date|a quel moment|date a laquelle|\bdates? ou\b|\bwhen\b|\batteint|\breach|passe par)"));
    static const QRegularExpression valueWord(QStringLiteral(
        R"(valeur|probabilite|densite|\bvaut\b|\bvalue\b|probability|\bloi\b|distribution)"));
    // « quelle date pour x = 0 », « probabilité de date pour X = 10 » : date où la courbe vaut la valeur.
    static const QRegularExpression dateFor(QStringLiteral(R"(\bdates?\s+(?:pour|de|d'|ou|quand)|\bprobabilite\s+de\s+(?:la\s+)?date|\bdater\b|\bdate\s+for\b)"));
    const bool isAcc = accWord.match(q).hasMatch();
    const bool isRate = rateWord.match(q).hasMatch() && !(isAcc && !q.contains(QLatin1String("vitesse et")) && !q.contains(QLatin1String("vitesse,"))
                                                         && !q.contains(QLatin1String("speed and")));
    const bool isRateOrAcc = isRate || isAcc;
    // « date pour une mesure à 45 ± 1 » (prompt réel du 05/11/2026) : sans nom de méthode de datation, une mesure à dater
    // vise la courbe jointe.
    static const QRegularExpression measureAsk(QStringLiteral(R"(\b(?:dates?|dater|probabilite de date)\b.*\b(?:mesure|mesuree|measured?|measurement)\b)"));
    static const QRegularExpression methodWord(QStringLiteral(R"(\b(?:c14|14c|bp|f14c|tl|osl|luminescence|archeomag\w*|am|gauss\w*|uniforme?|radiocarbone)\b)"));
    const bool curveMeasure = measureAsk.match(q).hasMatch() && !methodWord.match(q).hasMatch();
    // Datation avec une méthode nommée ou une courbe de RÉFÉRENCE (« date une gaussienne à x = 45 ± 1 avec la courbe de
    // référence curve_x_gauss_ref.csv ») : pas la courbe estimée jointe.
    static const QRegularExpression refCurve(QStringLiteral(R"(courbes? de reference|reference curves?|courbes? de calibration|calibration curves?)"));
    if (methodWord.match(q).hasMatch() || refCurve.match(q).hasMatch()) return {};
    // Date d'un extremum (prompt réel du 05/11/2026 : « où est la variation la plus forte », « quand date la variation la plus
    // grande ») : « vitesse maximale », « pente la plus forte », « maximum de la courbe », « varie le plus », « steepest change ».
    static const QRegularExpression extremeWord(QStringLiteral(
        R"(\bvariations?\s+(?:la\s+)?plus\s+(?:forte|grande|importante|rapide|brutale|marquee|faible|lente)\b)"
        R"(|\bplus\s+(?:forte|grande|importante|rapide|brutale)\s+(?:variation|pente|vitesse|croissance|decroissance|acceleration|changement|evolution|hausse|baisse)\b)"
        R"(|\b(?:pente|vitesse|acceleration|croissance|decroissance|evolution|changement|hausse|baisse|taux d'accumulation|variation)\s+(?:est(?:-elle|-il)?\s+|etait\s+|a ete\s+)?(?:la\s+)?plus\s+(?:forte|grande|elevee|importante|rapide|raide|brutale|faible|lente)\b)"
        R"(|\b(?:vitesse|pente|acceleration|taux d'accumulation|variation|derivee)\s+(?:est\s+|etait\s+)?(?:maximale|maximum|max|minimale|minimum|min|extreme)\b)"
        R"(|\b(?:maximum|minimum|max|min|pic|extremum|sommet)\s+(?:de\s+la\s+|du\s+|de\s+|of\s+the\s+)?(?:courbe|vitesse|pente|derivee|acceleration|taux|variation|curve|speed|rate|slope)\b)"
        R"(|\bcourbe\s+(?:est\s+)?(?:maximale|minimale|au maximum|au minimum)\b)"
        R"(|\b(?:varie|change|evolue)(?:-t-(?:elle|il))?\s+le\s+plus\b|\b(?:monte|croit|descend|decroit|baisse)\s+le\s+plus\s+vite\b)"
        R"(|\b(?:steepest|fastest|largest|strongest|biggest|greatest)\s+(?:change|variation|slope|increase|decrease|rate)\b)"
        R"(|\b(?:max(?:imum)?|min(?:imum)?|peak)\s+(?:rate|speed|slope|velocity)\b)"));
    const bool isExtreme = extremeWord.match(q).hasMatch();
    // Valeur nommée par ce qu'elle mesure (courbe âge-profondeur : « date le niveau 50 », « quelle date pour la profondeur
    // 120 ± 5 ? », « date à la cote 2,5 ») ; pas « niveau de confiance », ni « niveau 95 % ».
    static const QRegularExpression levelRe(QStringLiteral(
        R"(\b(?:niveau|profondeur|cote|hauteur|altitude|depth|level|height)\s+(?:de\s+|d'|a\s+|=\s*|:\s*)?)") + num
        + QStringLiteral(R"((?:\s*)") + text::plusMinus() + QStringLiteral(R"(\s*(\d+(?:[.,]\d+)?))?(?!\s*%|[.,]?\d))"));
    static const QRegularExpression confidence(QStringLiteral(R"(\b(?:niveau|level)\s+(?:de\s+)?(?:confiance|credibilite|probabilite|confidence|credibility))"));
    const bool isLevel = levelRe.match(q).hasMatch() && !confidence.match(q).hasMatch();
    if (!curveWord.match(q).hasMatch() && !isRateOrAcc && !curveMeasure && !isExtreme && !isLevel) return {};

    // Valeurs de la composante : « Y = 1,5 », « vaut 10 », « atteint (la valeur) 0 », « valeur 12 ».
    // Valeur éventuellement mesurée avec son erreur : « x = 0 ± 1 », « mesure de 55 +- 2 ».
    struct Hit { double v; qsizetype pos, len; double err = 0.0; };
    QVector<Hit> ys;
    QString dimLetter;
    const QString errPart = QStringLiteral(R"((?:\s*)") + text::plusMinus() + QStringLiteral(R"(\s*(\d+(?:[.,]\d+)?))?)");
    static const QRegularExpression yEq(QStringLiteral(R"(\b([xyzidf]|inc|dec)\s*=\s*)") + num + errPart);
    for (auto it = yEq.globalMatch(q); it.hasNext();) {
        const auto m = it.next();
        ys.append({ text::toNumber(m.captured(2)), m.capturedStart(), m.capturedLength(),
                    m.captured(3).isEmpty() ? 0.0 : text::toNumber(m.captured(3)) });
        if (dimLetter.isEmpty()) dimLetter = m.captured(1).toUpper();
    }
    if (ys.isEmpty() && isRateOrAcc) {
        // « une vitesse de 0,05 », « accélération égale à 0 », « taux d'accumulation = 1,2 ± 0,1 » ; « vitesse nulle »,
        // « la vitesse s'annule » : 0.
        static const QRegularExpression rv(QStringLiteral(R"(\b(?:vitesse|acceleration|taux d'accumulation|taux de sedimentation|speed|rate)\s+(?:de|d'|=|egale a|valant|vaut|of|equal to|equals)\s*)")
                                           + num + errPart + QStringLiteral(R"((?!\s*(?:av|apr|ans?\b|bc|ad\b)))"));
        for (auto it = rv.globalMatch(q); it.hasNext();) {
            const auto m = it.next();
            ys.append({ text::toNumber(m.captured(1)), m.capturedStart(), m.capturedLength(),
                        m.captured(2).isEmpty() ? 0.0 : text::toNumber(m.captured(2)) });
        }
        static const QRegularExpression zero(QStringLiteral(R"(\b(?:vitesse|acceleration|speed)\s+(?:nulle|s'annule|zero|is zero)|s'annule)"));
        if (ys.isEmpty()) if (const auto m = zero.match(q); m.hasMatch()) ys.append({ 0.0, m.capturedStart(), m.capturedLength(), 0.0 });
    }
    if (ys.isEmpty() && isLevel) {
        for (auto it = levelRe.globalMatch(q); it.hasNext();) {
            const auto m = it.next();
            ys.append({ text::toNumber(m.captured(1)), m.capturedStart(), m.capturedLength(),
                        m.captured(2).isEmpty() ? 0.0 : text::toNumber(m.captured(2)) });
        }
    }
    if (ys.isEmpty()) {
        static const QRegularExpression yWord(QStringLiteral(R"(\b(?:vaut|atteint|atteindre|atteigne|valeur|mesure|mesuree|measured?|reaches?|equals?)\s+(?:la valeur\s+|de\s+|a\s+)?)") + num
                                              + errPart + QStringLiteral(R"((?!\s*(?:av|apr|ans?\b|bc|ad\b)))"));
        for (auto it = yWord.globalMatch(q); it.hasNext();) {
            const auto m = it.next();
            ys.append({ text::toNumber(m.captured(1)), m.capturedStart(), m.capturedLength(),
                        m.captured(2).isEmpty() ? 0.0 : text::toNumber(m.captured(2)) });
        }
    }
    // « entre A et B » / « de A à B »
    struct Pair { double a, b; qsizetype pos, len; bool valueCtx; };
    QVector<Pair> pairs;
    static const QRegularExpression between(QStringLiteral(R"(\b(?:entre|between|de|from)\s+(?:les (?:dates|annees|valeurs)\s+|l'an\s+)?)") + num
                                            + QStringLiteral(R"(\s*(?:et|and|a|to|-)\s+(?:l'an\s+)?)") + num);
    for (auto it = between.globalMatch(q); it.hasNext();) {
        const auto m = it.next();
        const QString before = q.mid(std::max<qsizetype>(0, m.capturedStart() - 30), std::min<qsizetype>(30, m.capturedStart()));
        const bool valueCtx = before.contains(QLatin1String("valeur")) || before.contains(QLatin1String("value"))
                              || m.captured(0).contains(QLatin1String("valeurs"));
        pairs.append({ text::toNumber(m.captured(1)), text::toNumber(m.captured(2)), m.capturedStart(), m.capturedLength(), valueCtx });
    }
    // Dates isolées : « en 200 », « à t = -100 », « à la date 50 », « en l'an 1200 ».
    QString masked = q;
    for (const Hit& h : ys) masked.replace(h.pos, h.len, QString(h.len, ' '));
    for (const Pair& p : pairs) masked.replace(p.pos, p.len, QString(p.len, ' '));
    // Une date peut avoir une incertitude (« t = 100 ± 20 ») : elle devient une date gaussienne, tirée à chaque tirage.
    QStringList times;
    static const QRegularExpression timeRe(QStringLiteral(
        R"((?:\bt\s*=\s*|\ben\s+(?:l'an\s+|l'annee\s+)?|\ba la date\s+(?:de\s+)?|\bdate\s+|\bl'an\s+|\bannee\s+|\bat\s+(?:year\s+)?|\bin\s+(?:year\s+)?|\bvers\s+))") + num
        + QStringLiteral(R"((?!\d|[.,]\d))") + errPart);
    for (auto it = timeRe.globalMatch(masked); it.hasNext();) {
        const auto m = it.next();
        const double t = text::toNumber(m.captured(1)), e = m.captured(2).isEmpty() ? 0.0 : text::toNumber(m.captured(2));
        if (e > 0) {
            const QJsonObject g{ { "method", "gauss" }, { "age", t }, { "error", e },
                                 { "name", QStringLiteral("t = %1 ± %2").arg(text::numAuto(t, 6), text::numAuto(e, 6)) } };
            times << QString::fromUtf8(QJsonDocument(g).toJson(QJsonDocument::Compact));
        } else {
            times << QString::number(t, 'g', 12);
        }
    }
    // Dates d'Events des traces jointes citées par leur nom.
    QStringList cols;
    for (const traces::Mention& mn : traces::mentions(question)) cols << mn.column;

    QJsonObject rq{ { "method", "curve" } };
    static const QRegularExpression dimRe(QStringLiteral(R"(\b(?:composante|dimension|component|courbe)\s+([xyzidf])(?![\w'’]))"));
    static const QRegularExpression dimOfRe(QStringLiteral(R"(\b(?:probabilite|loi|valeur|densite|vitesse|distribution)\s+(?:de|d')\s*([xyzidf])(?![\w'’]))"));
    if (const auto m = dimRe.match(q); m.hasMatch()) rq["dim"] = m.captured(1).toUpper();
    else if (const auto m2 = dimOfRe.match(q); m2.hasMatch()) rq["dim"] = m2.captured(1).toUpper();
    else if (q.contains(QLatin1String("inclinaison")) || q.contains(QLatin1String("inclination"))) rq["dim"] = QStringLiteral("I");
    else if (q.contains(QLatin1String("declinaison")) || q.contains(QLatin1String("declination"))) rq["dim"] = QStringLiteral("D");
    else if (q.contains(QLatin1String("intensite")) || q.contains(QLatin1String("intensity"))) rq["dim"] = QStringLiteral("F");
    else if (!dimLetter.isEmpty()) rq["dim"] = dimLetter;
    if (q.contains(QLatin1String("par siecle")) || q.contains(QLatin1String("per century"))) rq["per"] = 100;
    else if (q.contains(QLatin1String("par millenaire"))) rq["per"] = 1000;
    auto year = [](double v) { return QString::number(v, 'g', 12); };
    auto term = [](const QString& t) -> QJsonValue {             // date incertaine écrite en JSON : objet
        if (t.startsWith('{')) { const QJsonDocument d = QJsonDocument::fromJson(t.toUtf8()); if (d.isObject()) return d.object(); }
        return t;
    };

    const QString qty = isAcc && !isRate ? QStringLiteral("acceleration") : QStringLiteral("vitesse");
    if (isExtreme) {
        rq["op"] = QStringLiteral("extremum");
        static const QRegularExpression valueExt(QStringLiteral(
            R"(\b(?:maximum|minimum|max|min|pic|sommet|extremum)\s+(?:de\s+la\s+|du\s+|de\s+|of\s+the\s+)?(?:courbe|curve)\b|\bcourbe\s+(?:est\s+)?(?:maximale|minimale|au maximum|au minimum)\b)"));
        const bool onValue = valueExt.match(q).hasMatch() && !isRateOrAcc;
        rq["quantity"] = onValue ? QStringLiteral("value") : isAcc && !isRate ? QStringLiteral("acceleration") : QStringLiteral("derivative");
        static const QRegularExpression minW(QStringLiteral(R"(\b(?:decroissance|descend|decroit|baisse|decrease|minimum|minimale|min)\b)"));
        static const QRegularExpression maxW(QStringLiteral(R"(\b(?:croissance|monte|croit|hausse|increase|maximum|maximale|max|pic|sommet|peak)\b)"));
        static const QRegularExpression weakW(QStringLiteral(R"(\bplus\s+(?:faible|lente)\b|\bslowest\b|\bflattest\b)"));
        static const QRegularExpression strongW(QStringLiteral(R"(\bvariations?\b|\bchange|\bvarie\b|\bevolu|\bplus\s+(?:forte|raide|brutale|marquee|importante)\b|steepest|strongest|largest|biggest|greatest)"));
        static const QRegularExpression absW(QStringLiteral(R"(valeur absolue|\babsolute\b|\ben absolu\b)"));
        if (weakW.match(q).hasMatch()) rq["sense"] = QStringLiteral("absmin");
        else if (absW.match(q).hasMatch()) rq["sense"] = q.contains(QLatin1String("minim")) ? QStringLiteral("absmin") : QStringLiteral("abs");
        else if (minW.match(q).hasMatch() && !strongW.match(q).hasMatch()) rq["sense"] = QStringLiteral("min");
        else if (maxW.match(q).hasMatch() && (onValue || !strongW.match(q).hasMatch())) rq["sense"] = QStringLiteral("max");
        else if (q.contains(QLatin1String("decroissance")) || q.contains(QLatin1String("baisse"))) rq["sense"] = QStringLiteral("min");
        else if (q.contains(QLatin1String("croissance")) || q.contains(QLatin1String("hausse"))) rq["sense"] = QStringLiteral("max");
        else rq["sense"] = onValue ? QStringLiteral("max") : QStringLiteral("abs");
        for (const Pair& p : pairs) if (!p.valueCtx) { rq["tmin"] = std::min(p.a, p.b); rq["tmax"] = std::max(p.a, p.b); break; }
        return rq;
    }
    // Tracé des courbes de valeur, de vitesse, d'accélération : « trace les courbes de vitesse et d'accélération »,
    // « affiche le taux d'accumulation entre -500 et 300 ».
    if (plotWord.match(q).hasMatch() && ys.isEmpty() && times.isEmpty() && cols.isEmpty()) {
        rq["op"] = QStringLiteral("plot");
        QJsonArray qs;
        const bool wantValue = valueWord.match(q).hasMatch() && !q.contains(QLatin1String("probabilite"));
        if (wantValue || (!rateWord.match(q).hasMatch() && !isAcc)) qs.append(QStringLiteral("value"));
        if (rateWord.match(q).hasMatch()) qs.append(QStringLiteral("derivative"));
        if (isAcc) qs.append(QStringLiteral("acceleration"));
        if (!rateWord.match(q).hasMatch() && !isAcc && !wantValue) qs = QJsonArray{ "value", "derivative", "acceleration" };
        rq["quantities"] = qs;
        for (const Pair& p : pairs) if (!p.valueCtx) { rq["tmin"] = std::min(p.a, p.b); rq["tmax"] = std::max(p.a, p.b); break; }
        return rq;
    }
    // Date d'une vitesse ou d'une accélération mesurée, ou date où elle atteint une valeur (« quand la vitesse s'annule »).
    if (isRateOrAcc && ys.size() == 1 && times.isEmpty() && cols.isEmpty() && pairs.isEmpty()) {
        rq["quantity"] = qty;
        rq["y"] = ys[0].v;
        if (crossWord.match(q).hasMatch() && !(ys[0].err > 0)) {
            rq["op"] = QStringLiteral("crossing");
            if (q.contains(QLatin1String("derniere")) || q.contains(QLatin1String("last"))) rq["which"] = QStringLiteral("last");
            else if (q.contains(QLatin1String("toutes")) || q.contains(QLatin1String("chaque fois"))) rq["which"] = QStringLiteral("all");
        } else {
            rq["op"] = QStringLiteral("calibrate");
            rq["error"] = ys[0].err;
        }
        return rq;
    }
    if (isAcc && !isRate) {                                          // accélération à une date
        QStringList tv = times;
        tv += cols;
        if (tv.size() == 1) { rq["op"] = QStringLiteral("acceleration"); rq["t"] = term(tv[0]); return rq; }
        return {};
    }
    if (isRate) {
        QVector<double> yv;
        for (const Hit& h : ys) yv << h.v;
        for (const Pair& p : pairs) if (p.valueCtx || !ys.isEmpty()) { yv << p.a << p.b; }
        if (yv.size() >= 2) { rq["op"] = QStringLiteral("rate_values"); rq["y1"] = yv[0]; rq["y2"] = yv[1]; return rq; }
        QStringList tv;
        for (const Pair& p : pairs) if (!p.valueCtx) tv << year(p.a) << year(p.b);
        tv += times;
        tv += cols;
        if (tv.size() >= 2) { rq["op"] = QStringLiteral("rate"); rq["from"] = term(tv[0]); rq["to"] = term(tv[1]); return rq; }
        if (tv.size() == 1) { rq["op"] = QStringLiteral("derivative"); rq["t"] = term(tv[0]); return rq; }
        return {};
    }
    QStringList tv = times;
    tv += cols;
    // Valeur mesurée, sans date : date de la mesure sur la courbe (« quelle date pour x = 0 ± 1 ? », « date pour x = 40 »,
    // ou une valeur seule en suite de question, « et x = 50 ») ; sans erreur, valeur exacte (limite σ → 0).
    const bool bareValue = tv.isEmpty() && ys.size() == 1 && !valueWord.match(q).hasMatch() && pairs.isEmpty();
    if (tv.isEmpty() && ys.size() == 1 && !crossWord.match(q).hasMatch()
        && (ys[0].err > 0 || dateFor.match(q).hasMatch() || bareValue)) {
        rq["op"] = QStringLiteral("calibrate");
        rq["y"] = ys[0].v;
        rq["error"] = ys[0].err;
        return rq;
    }
    if (crossWord.match(q).hasMatch() && tv.isEmpty() && ys.size() == 1) {
        rq["op"] = QStringLiteral("crossing");
        rq["y"] = ys[0].v;
        if (q.contains(QLatin1String("derniere")) || q.contains(QLatin1String("last"))) rq["which"] = QStringLiteral("last");
        else if (q.contains(QLatin1String("toutes")) || q.contains(QLatin1String("chaque fois")) || q.contains(QLatin1String("every time")))
            rq["which"] = QStringLiteral("all");
        return rq;
    }
    if (tv.size() == 1 && (valueWord.match(q).hasMatch() || !ys.isEmpty() || !pairs.isEmpty())) {
        rq["op"] = QStringLiteral("value");
        rq["t"] = term(tv[0]);
        if (ys.size() == 1) rq["y"] = ys[0].v;
        if (ys.size() >= 2) { rq["ymin"] = std::min(ys[0].v, ys[1].v); rq["ymax"] = std::max(ys[0].v, ys[1].v); }
        else if (!pairs.isEmpty()) { rq["ymin"] = std::min(pairs[0].a, pairs[0].b); rq["ymax"] = std::max(pairs[0].a, pairs[0].b); }
        return rq;
    }
    return {};
}

QJsonObject MethodRunner::inverseFromQuestion(const QString& question)
{
    QString q = text::fold(question);
    // Déclencheurs : « calibration inverse », ou « quel âge / quelle inclinaison … pour (une date | l'an | un échantillon) … ».
    static const QRegularExpression explicitRe(QStringLiteral(R"(\b(?:calibration inverse|inverse calibration|back.?calibrat\w*)\b)"));
    static const QRegularExpression askRe(QStringLiteral(
        R"(\b(?:quel(?:le)?s?|what|which)\s+(?:age|ages|f14c|fraction|inclinaison|declinaison|intensite|champ|inclination|declination|intensity|field)\b)"));
    static const QRegularExpression dateWordRe(QStringLiteral(
        R"(\b(?:attend\w*|correspond\w*|mesurer\w*|obtiendr\w*|expected|une date|la date|date de|date calendaire|l'an|l'annee|annee|an |echantillon de|echantillon date|sample from|for a date|for the year|for a sample|dat\w* (?:de|du|entre))\b)"));
    static const QRegularExpression eraRe(QStringLiteral(R"((?:av\.?\s*j\.?\s*-?\s*c\.?|avant\s+j\.?\s*-?\s*c\.?|\bbce?\b|apr\.?\s*j\.?\s*-?\s*c\.?|\bad\b|\bce\b))"));
    const bool explicitAsk = explicitRe.match(q).hasMatch();
    if (!explicitAsk && !(askRe.match(q).hasMatch() && (dateWordRe.match(q).hasMatch() || eraRe.match(q).hasMatch()))) return {};
    // Une mesure « 2500 ± 30 BP » est une datation à calibrer, pas une date calendaire.
    static const QRegularExpression measured(QStringLiteral(R"(\d\s*(?:±|\+\s*/?\s*-)\s*\d+(?:[.,]\d+)?\s*(?:ans\s*)?bp\b)"));
    if (measured.match(q).hasMatch()) return {};

    QJsonObject o{ { "method", "inverse" } };
    // Grandeur
    static const QRegularExpression incRe(QStringLiteral(R"(\b(?:inclinaison|inclination)\b)")), decRe(QStringLiteral(R"(\b(?:declinaison|declination)\b)")),
                                     intRe(QStringLiteral(R"(\b(?:intensite|champ|intensity|field|paleointensite)\b)")), f14Re(QStringLiteral(R"(\b(?:f14c|fraction moderne|fraction 14c)\b)"));
    if (incRe.match(q).hasMatch()) { o["type"] = "am"; o["component"] = "I"; }
    else if (decRe.match(q).hasMatch()) { o["type"] = "am"; o["component"] = "D"; }
    else if (intRe.match(q).hasMatch()) { o["type"] = "am"; o["component"] = "F"; }
    else if (f14Re.match(q).hasMatch()) o["type"] = "f14c";
    else o["type"] = "c14";
    // Courbe nommée : retirée du texte avant de lire les nombres (« intcal20 » contient « 20 »).
    const QString type = o.value("type").toString();
    for (const QString& m : type == "am" ? QStringList{ "am" } : type == "f14c" ? QStringList{ "f14c", "c14" } : QStringList{ "c14" }) {
        int pos = -1;
        const QString c = CurveStore::findNamed(m, q, &pos);
        if (c.isEmpty()) continue;
        o[type == "am" ? (o.value("component").toString() == "I" ? "curve_i" : o.value("component").toString() == "D" ? "curve_d" : "curve_f") : "curve"] = c;
        if (pos >= 0) q.replace(pos, c.size(), QString(c.size(), QLatin1Char(' ')));
        break;
    }
    q.remove(QRegularExpression(QStringLiteral(R"(\b(?:intcal|shcal|marine|bomb)\d+\w*)")));
    q.remove(QRegularExpression(QStringLiteral(R"(\b14\s*c\b|\bc\s*14\b|\bf14c\b)")));
    // Erreur de mesure prévue (« erreur de laboratoire de 25 ans », « lab error 30 »), retirée elle aussi.
    static const QRegularExpression labRe(QStringLiteral(R"(\b(?:erreur (?:de (?:laboratoire|mesure|labo)|labo\w*)|lab(?:oratory)? error|measurement error)\s*(?:de|of|=|:)?\s*(\d+(?:[.,]\d+)?))"));
    if (const auto m = labRe.match(q); m.hasMatch()) { o["lab_error"] = text::toNumber(m.captured(1)); q.replace(m.capturedStart(), m.capturedLength(), QString(m.capturedLength(), ' ')); }

    // Années : « 5850 BC », « 6300 av. J.-C. » (année astronomique 1 − N, comme l'affichage), « l'an 1200 », « -450 ».
    const QString num = QStringLiteral(R"((-?\d+(?:[.,]\d+)?)\s*(av\.?\s*j\.?\s*-?\s*c\.?|avant\s+j\.?\s*-?\s*c\.?|bce?\b|apr\.?\s*j\.?\s*-?\s*c\.?|ad\b|ce\b)?)");
    auto year = [](const QString& n, const QString& era) {
        const double v = text::toNumber(n);
        if (era.startsWith(QLatin1String("av")) || era.startsWith(QLatin1String("bc"))) return 1.0 - std::abs(v);
        return v;
    };
    static const QRegularExpression rangeRe(QStringLiteral(R"(\b(?:entre|between|de|from)\s+)") + num + QStringLiteral(R"(\s*(?:et|a|au|and|to|-|–)\s*)") + num);
    static const QRegularExpression gaussRe(num + QStringLiteral(R"(\s*)") + text::plusMinus() + QStringLiteral(R"(\s*(\d+(?:[.,]\d+)?))"));
    static const QRegularExpression oneRe(num);
    if (const auto m = rangeRe.match(q); m.hasMatch()) {
        // « entre 800 et 900 av. J.-C. » : l'ère écrite à la fin vaut pour les deux bornes
        const QString e1 = m.captured(2).isEmpty() ? m.captured(4) : m.captured(2);
        const double a = year(m.captured(1), e1), b = year(m.captured(3), m.captured(4));
        if (a == b) return {};
        o["t_min"] = std::min(a, b);
        o["t_max"] = std::max(a, b);
    } else if (const auto g = gaussRe.match(q); g.hasMatch()) {
        o["t"] = year(g.captured(1), g.captured(2));
        o["t_error"] = text::toNumber(g.captured(3));
    } else if (const auto n = oneRe.match(q); n.hasMatch()) {
        o["t"] = year(n.captured(1), n.captured(2));
    } else {
        return {};
    }
    return o;
}

// Noms à ne jamais corriger : colonnes des traces (Events, phases), fichiers de courbes et de traces joints.
static QStringList fuzzyKeepNames()
{
    QStringList keep;
    for (const TraceSet& ts : traces::loaded()) { keep += ts.columns; keep << ts.file; }
    for (const SplineTrace& s : curves::splines()) keep << s.file;
    for (const CurveMap& m : curves::maps()) keep << m.file;
    return keep;
}

QVector<QJsonObject> MethodRunner::requestsFromQuestion(const QString& question, QString* fixes)
{
    if (fixes) fixes->clear();
    QVector<QJsonObject> r = requestsFromQuestionExact(question);
    if (!r.isEmpty()) return r;
    QVector<fuzzy::Fix> fx;
    const QString corrected = fuzzy::correct(question, &fx, fuzzyKeepNames());
    if (fx.isEmpty()) return r;
    r = requestsFromQuestionExact(corrected);
    if (!r.isEmpty() && fixes) *fixes = fuzzy::describe(fx);
    return r;
}

QVector<QJsonObject> MethodRunner::requestsFromQuestionExact(const QString& question)
{
    // Appel d'outil écrit par l'utilisateur (« calibrer_radiocarbone(age=2500, error=30) », « courbe_chronomodel
    // {"op":"plot"} ») : exécuté tel quel s'il est complet ; sinon le modèle de langage s'en charge (il voit l'outil).
    {
        QString toolName;
        QJsonObject toolArgs;
        if (tools::callFromText(question, toolName, toolArgs)) {
            const QJsonObject r = tools::requestFor(toolName, toolArgs);
            if (!r.isEmpty()) return { r };
        }
    }
    // Tableau courant : « extrais les 14C du tableau », « date les TL », « un modèle avec le tableau »… (sélection simple).
    if (QJsonObject t = datatable::requestFromQuestion(question); !t.isEmpty()) {
        if (const double lv = requestedLevel(question); lv > 0) t["level"] = lv;
        return { t };
    }
    // Tableau collé dans la question : jamais lu comme des datations isolées (les colonnes se mélangeraient). SeldonH le lit
    // avec le lecteur de tableaux (datatable) avant d'arriver ici.
    if (datatable::looksLikeTable(question)) return {};
    if (isModelRequest(question)) return {};
    // Courbe de ChronoModel jointe (trace des splines, carte de densité) et question sur elle : valeur, vitesse, atteinte.
    if (QJsonObject cv = curveFromQuestion(question); !cv.isEmpty()) {
        if (const double lv = requestedLevel(question); lv > 0) cv["level"] = lv;
        return { cv };
    }
    // Traces MCMC jointes et colonnes citées (« durée entre N(500) et N(200) ») : calcul sur les tirages, avant toute
    // lecture de datations (« N(500) » ressemble à une loi normale, « Event » à un événement à calculer).
    if (QVector<QJsonObject> tr = tracesFromQuestion(question); !tr.isEmpty()) {
        if (const double lv = requestedLevel(question); lv > 0) tr[0]["level"] = lv;
        return tr;
    }
    // « Quel âge 14C attendre pour une date de 5850 BC ? » : calibration inverse, faite par l'application.
    if (QJsonObject inv = inverseFromQuestion(question); !inv.isEmpty()) {
        if (const double lv = requestedLevel(question); lv > 0) inv["level"] = lv;
        return { inv };
    }
    // Décalages (wiggle) écrits dans la question : leurs passages sont masqués avant de lire les datations, sinon
    // « décalage de 20 ± 5 ans » serait pris pour une datation de plus.
    QString masked;
    const QVector<WigglePassage> wiggles = wigglePassages(question, &masked);
    // « 500 av. J.-C. » -> -499 (années astronomiques, comme les courbes de calibration et l'affichage).
    const QString datesText = text::erasToAstronomical(masked);
    QVector<QJsonObject> rq = MethodRegistry::instance().parse(datesText);
    // Fragment JSON collé dans la question (bloc chronomodel, .chr) : ses valeurs sont exactes, elles priment.
    const QVector<QJsonObject> js = jsonRequests(question);
    if (!js.isEmpty()) rq = js;
    if (!wiggles.isEmpty()) {
        // Chaque décalage va à la datation qui le précède (« 14C 6589 ± 30 avec un wiggle fixe de 500 ans et 14C 5879 ± 35
        // avec un wiggle range -300 et -200 ») ; une seule datation reçoit le décalage où qu'il soit écrit.
        if (rq.size() == 1) {
            if (!rq[0].contains("wiggle")) rq[0]["wiggle"] = wiggles.first().wiggle;
        } else if (js.isEmpty()) {                       // datations en JSON : leurs décalages sont dans le JSON
            QVector<WiggleKey> keys = wigglesByDate(masked, wiggles);
            for (QJsonObject& o : rq) {
                const double v = o.contains("age") ? o.value("age").toDouble() : o.value("f14c").toDouble(std::nan(""));
                const double e = o.value("error").toDouble(std::nan(""));
                for (int k = 0; k < keys.size(); ++k)
                    if (std::abs(keys[k].value - v) < 1e-9 && std::abs(keys[k].error - e) < 1e-9) {
                        if (!o.contains("wiggle")) o["wiggle"] = keys[k].wiggle;
                        keys.removeAt(k);
                        break;
                    }
            }
        }
    }
    const double level = requestedLevel(question);
    // Deux ou trois datations reliées par la question : une seule demande « relation » (calcul exact, datations
    // indépendantes). « Laquelle est la plus ancienne ? » : ordre (P(A antérieure à B) et écart) ; « combien de temps
    // entre… » : durée ; « date d'un niveau situé entre… » : date entre ; « C est-elle comprise entre A et B ? » ou
    // « entre 800 et 500 av. J.-C. ? » : appartenance.
    if (!rq.isEmpty() && rq.size() <= 3 && !asksEvent(question) && !asksCombine(question)) {
        const QString q = text::fold(datesText);
        RelationWords w = relationWords(q);
        w.order = asksOrder(question);
        w.value = false;                                  // une datation seule reste une datation
        if (rq.size() == 1 && !rq[0].value("method").toString().startsWith(QLatin1String("unif"))) w.duration = w.between = w.order = false;
        else if (rq.size() == 1) w = RelationWords();     // « uniforme entre 800 et 500 » : la datation elle-même
        if (rq.size() == 2) w.inside = false;
        if (rq.size() == 3) { const bool in = w.inside; w = RelationWords(); w.inside = in; }
        QVector<QJsonValue> t;
        for (QJsonObject o : rq) { o.remove("level"); t << QJsonValue(o); }
        if (QJsonObject rel = relationRequest(w, t, QString(), q, true); !rel.isEmpty()) { durationOldestFirst(rel); rq = { rel }; }
    }
    // « Combine ces datations » : une seule demande « combine » (produit des vraisemblances, « Combine » de ChronoModel).
    if (rq.size() >= 2 && asksCombine(question)) {
        QJsonObject cb{ { "method", "combine" } };
        static const QRegularExpression nameRe(QStringLiteral(
            R"RX((?:combinaison|combination)\s*(?:«\s*([^»]{1,40}?)\s*»|"([^"]{1,40})"|(?:nomm[ée]e?|appel[ée]e?|named|called)\s+([^\s,;:.]{1,40})))RX"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = nameRe.match(question); m.hasMatch())
            cb["name"] = (m.captured(1) + m.captured(2) + m.captured(3)).trimmed();
        QJsonArray ds;
        for (QJsonObject o : rq) { o.remove("level"); ds.append(o); }
        cb["dates"] = ds;
        rq = { cb };
    }
    // « Calcule l'événement à partir de ces datations » : une seule demande « event » qui les combine (MCMC).
    if (rq.size() >= 2 && asksEvent(question)) {
        QJsonObject ev{ { "method", "event" } };
        static const QRegularExpression nameRe(QStringLiteral(
            R"RX((?:event|[ée]v[éè]nement)\s*(?:«\s*([^»]{1,40}?)\s*»|"([^"]{1,40})"|(?:nomm[ée]|appel[ée]|named|called)\s+([^\s,;:.]{1,40})))RX"),
            QRegularExpression::CaseInsensitiveOption);
        if (const auto m = nameRe.match(question); m.hasMatch())
            ev["name"] = (m.captured(1) + m.captured(2) + m.captured(3)).trimmed();
        QJsonArray ds;
        for (QJsonObject o : rq) { o.remove("level"); ds.append(o); }
        ev["dates"] = ds;
        double tmin = std::nan(""), tmax = std::nan("");
        const QJsonObject mcmc = mcmcFromQuestion(question, &tmin, &tmax);
        if (!mcmc.isEmpty()) ev["mcmc"] = mcmc;
        if (std::isfinite(tmin) && std::isfinite(tmax)) { ev["tmin"] = std::min(tmin, tmax); ev["tmax"] = std::max(tmin, tmax); }
        if (asksTraces(question)) ev["traces"] = true;
        rq = { ev };
    }
    if (level > 0)
        for (QJsonObject& o : rq) if (!o.contains("level")) o["level"] = level;
    if (asksOverlay(question))
        for (QJsonObject& o : rq) o["layout"] = QStringLiteral("overlay");
    return rq;
}

namespace {

// Nombre écrit en texte : « 35 », « 2,5 », « 1 200 » (espaces fines comprises). NaN sinon.
double numberFromText(QString s)
{
    s = s.trimmed();
    s.remove(QChar(0x00A0)).remove(QChar(0x202F)).remove(' ');
    s.replace(',', '.');
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok ? d : std::nan("");
}

// Objets JSON de premier niveau d'un texte quelconque (accolades équilibrées, chaînes respectées).
QVector<QJsonObject> topLevelObjects(const QString& t)
{
    QVector<QJsonObject> out;
    int depth = 0, start = -1;
    bool inStr = false, esc = false;
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"' && depth > 0) inStr = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}' && depth > 0 && --depth == 0) {
            const QJsonDocument d = QJsonDocument::fromJson(t.mid(start, i - start + 1).toUtf8());
            if (d.isObject()) out << d.object();
        }
    }
    return out;
}

// Contenu d'un bloc : objet, tableau, ou objets séparés par des virgules / retours à la ligne.
QVector<QJsonObject> objectsOfBlock(const QString& body)
{
    QVector<QJsonObject> out;
    const QJsonDocument d = QJsonDocument::fromJson(body.toUtf8());
    if (d.isObject()) out << d.object();
    else if (d.isArray()) { for (const QJsonValue& v : d.array()) if (v.isObject()) out << v.toObject(); }
    else out = topLevelObjects(body);
    return out;
}

} // namespace

QJsonObject MethodRunner::fillDateNames(QJsonObject request, const QVector<QJsonObject>& known)
{
    if (known.isEmpty() || !request.value("dates").isArray()) return request;
    static const char* const values[] = { "age", "error", "f14c", "min", "max", "inc", "dec", "alpha95", "field", "error_f", "ref_year" };
    QVector<QJsonObject> pool;
    for (const QJsonObject& k : known) {
        const QJsonObject n = normalizeRequest(k);
        if (!n.value("name").toString().trimmed().isEmpty()) pool << n;
    }
    QVector<bool> used(pool.size(), false);
    const QString eventName = request.value("name").toString().trimmed();
    QJsonArray dates = request.value("dates").toArray();
    for (int i = 0; i < dates.size(); ++i) {
        QJsonObject d = dates[i].toObject();
        const QString own = d.value("name").toString().trimmed();
        if (!own.isEmpty() && own.compare(eventName, Qt::CaseInsensitive) != 0) continue;
        const QJsonObject n = normalizeRequest(d);
        int best = -1;
        for (int j = 0; j < pool.size(); ++j) {
            if (used[j] || pool[j].value("method").toString() != n.value("method").toString()) continue;
            int same = 0;
            bool differ = false;
            for (const char* v : values) {
                if (!n.contains(v)) continue;
                if (!pool[j].contains(v) || std::abs(pool[j].value(v).toDouble() - n.value(v).toDouble()) > 1e-9) { differ = true; break; }
                ++same;
            }
            if (differ || same < 2) continue;
            if (best < 0 || (pool[j].value("wiggle") == n.value("wiggle") && pool[best].value("wiggle") != n.value("wiggle"))) best = j;
        }
        if (best < 0) continue;
        used[best] = true;
        d["name"] = pool[best].value("name").toString().trimmed();
        dates[i] = d;
    }
    request["dates"] = dates;
    return request;
}

QJsonObject MethodRunner::normalizeRequest(QJsonObject o)
{
    // Datation au format .chr : {"plugin_id":"14c","name":…,"data":{"age":…,"error":…,"ref_curve":"intcal20.14c",…}}
    if (o.value("data").isObject() && (o.contains("plugin_id") || !o.contains("age"))) {
        const QJsonObject d = o.value("data").toObject();
        for (auto it = d.begin(); it != d.end(); ++it)
            if (!o.contains(it.key())) o[it.key()] = it.value();
        o.remove("data");
    }
    if (!o.contains("method") && o.contains("plugin_id")) o["method"] = o.value("plugin_id");
    // Bloc chronomodel de SeldonH : la méthode d'une date est dans « type » ({"type":"gauss","age":250,"error":10}).
    if (!o.contains("method") && o.value("type").isString()) {
        const QString t = text::fold(o.value("type").toString()).trimmed();
        static const QStringList known = { "c14", "14c", "f14c", "tl", "tl/osl", "osl", "gauss", "unif", "uniforme", "am", "radiocarbone", "density" };
        if (known.contains(t)) { o["method"] = t; o.remove("type"); }
    }
    o.remove("plugin_id");
    // Décalage au format .chr (Date::DeltaType : -1 aucun, 0 fixe, 1 gaussien, 2 intervalle). Seul delta_type compte :
    // ChronoModel garde les anciennes valeurs delta_* quand on repasse à « aucun ».
    if (o.contains("delta_type") && !o.contains("wiggle")) {
        const int dt = o.value("delta_type").toInt(-1);
        if (dt == 0) o["wiggle"] = QJsonObject{ { "type", "fixed" }, { "value", o.value("delta_fixed").toDouble() } };
        else if (dt == 1) o["wiggle"] = QJsonObject{ { "type", "gaussian" }, { "mean", o.value("delta_average").toDouble() },
                                                     { "error", o.value("delta_error").toDouble() } };
        else if (dt == 2) o["wiggle"] = QJsonObject{ { "type", "range" }, { "min", o.value("delta_min").toDouble() },
                                                     { "max", o.value("delta_max").toDouble() } };
    }
    for (const char* k : { "delta_type", "delta_fixed", "delta_average", "delta_error", "delta_min", "delta_max" }) o.remove(k);
    // Archéomagnétisme au format .chr : process_type 1..6, error_f, refX_curve "….ref", integration_steps.
    if (o.value("process_type").isDouble() && !o.contains("process")) {
        static const char* const pn[] = { "", "I", "D", "F", "ID", "IF", "IDF" };
        const int pt = o.value("process_type").toInt();
        if (pt >= 1 && pt <= 6) o["process"] = QString::fromLatin1(pn[pt]);
        o.remove("process_type");
    }
    // Noms du .chr pour les courbes (refI_curve…) : mêmes noms que la spécification (curve_i…), sans « .ref ».
    static const char* const amKeys[][2] = { { "refI_curve", "curve_i" }, { "refD_curve", "curve_d" }, { "refF_curve", "curve_f" } };
    for (const auto& r : amKeys)
        if (o.contains(r[0]) && !o.contains(r[1])) { o[r[1]] = o.value(r[0]); o.remove(r[0]); }
    for (const char* k : { "curve_i", "curve_d", "curve_f" })
        if (o.value(k).isString())
            o[k] = o.value(k).toString().trimmed().toLower().remove(QRegularExpression(QStringLiteral("\\.ref$")));
    static const char* const renames[][2] = { { "ref_curve", "curve" }, { "err", "error" }, { "sigma", "error" },
                                              { "fraction", "f14c" } };       // plugin f14c du .chr : « fraction »
    for (const auto& r : renames)
        if (o.contains(r[0]) && !o.contains(r[1])) { o[r[1]] = o.value(r[0]); o.remove(r[0]); }
    if (o.value("curve").isString()) {
        QString c = o.value("curve").toString().trimmed().toLower();
        c.remove(QRegularExpression(QStringLiteral("\\.14c$")));
        o["curve"] = c;
    }

    // Méthode : alias et noms d'outils (le modèle écrit parfois « calculer_evenement » ou « 14C »).
    QString m = text::fold(o.value("method").toString()).trimmed();
    // Anciens noms (« traces », « order ») et noms d'outils : même calcul « relation », avec l'opération par défaut de
    // chacun (traces : durée, ou densité d'une colonne avec « of » ; order : antéro-postérité).
    QString defaultOp;
    static const QHash<QString, QString> relationAlias = {
        { "traces", "traces" }, { "trace", "traces" }, { "chaines", "traces" }, { "chains", "traces" }, { "analyser_traces", "traces" },
        { "order", "order" }, { "ordre", "order" }, { "anteriorite", "order" }, { "comparer", "order" }, { "comparaison", "order" },
        { "compare", "order" }, { "comparer_dates", "order" },
        { "relation", "relation" }, { "relations", "relation" } };
    if (relationAlias.contains(m)) {
        const QString legacy = relationAlias.value(m);
        if (legacy == QLatin1String("traces")) defaultOp = o.contains("of") ? QStringLiteral("value") : QStringLiteral("duration");
        else if (legacy == QLatin1String("order")) defaultOp = QStringLiteral("order");
        m = QStringLiteral("relation");
    }
    if (const tools::ToolDef* def = tools::find(m); def && !def->methodId.isEmpty()) m = def->methodId;
    if (!defaultOp.isEmpty() && o.value("op").toString().trimmed().isEmpty()) o["op"] = defaultOp;
    static const QHash<QString, QString> alias = {
        { "14c", "c14" }, { "c-14", "c14" }, { "radiocarbone", "c14" }, { "radiocarbon", "c14" }, { "carbone 14", "c14" },
        { "tl/osl", "tl" }, { "osl", "tl" }, { "luminescence", "tl" },
        { "gaussienne", "gauss" }, { "normal", "gauss" }, { "normale", "gauss" },
        { "uniforme", "unif" }, { "uniform", "unif" },
        { "archeomagnetisme", "am" }, { "archeomag", "am" },
        { "densite tabulee", "density_file" }, { "fichier densite", "density_file" }, { "fichier de densite", "density_file" },
        { "density file", "density_file" }, { "densityfile", "density_file" },
        { "evenement", "event" }, { "events", "event" },
        { "combinaison", "combine" }, { "combination", "combine" }, { "combiner", "combine" }, { "combined", "combine" },
        { "calibration inverse", "inverse" }, { "inverse calibration", "inverse" }, { "inverse_calibration", "inverse" },
        { "courbe", "curve" }, { "courbes", "curve" }, { "curves", "curve" }, { "spline", "curve" }, { "splines", "curve" },
        { "courbe chronomodel", "curve" }, { "chronomodel curve", "curve" } };
    if (alias.contains(m)) m = alias.value(m);
    if (!m.isEmpty()) o["method"] = m;

    // Nombres écrits en texte.
    static const char* const numeric[] = { "age", "error", "delta_r", "delta_r_error", "ref_year", "min", "max", "inc", "dec", "alpha95",
                                           "field", "error_f", "f14c", "level", "tmin", "tmax", "t", "t_error", "t_min", "t_max",
                                           "lab_error", "known_fixed" };
    for (const char* k : numeric)
        if (o.value(k).isString()) {
            double d = numberFromText(o.value(k).toString());
            if (!std::isfinite(d)) d = text::yearFromText(o.value(k).toString());   // « 500 av. J.-C. » -> -499
            if (std::isfinite(d)) o[k] = d;
        }

    if (m == "tl" || m == "gauss") o = withAgeUnit(o, m);
    if ((m == "event" || m == "combine" || m == "relation") && o.value("dates").isArray()) {
        QJsonArray ds;
        for (const QJsonValue& v : o.value("dates").toArray()) ds.append(v.isObject() ? QJsonValue(normalizeRequest(v.toObject())) : v);
        o["dates"] = ds;
    }
    if (m == "curve")
        for (const char* k : { "t", "from", "to", "t1", "t2" })
            if (o.value(k).isObject()) o[k] = normalizeRequest(o.value(k).toObject());
    if (m == "relation")
        for (const char* k : { "a", "b", "c", "from", "to", "of" })
            if (o.value(k).isObject()) o[k] = normalizeRequest(o.value(k).toObject());
    return o;
}

QJsonObject MethodRunner::wiggleFromQuestion(const QString& question, int* start, int* length)
{
    // Sur le texte d'origine (pas de fold) pour pouvoir masquer le passage : accents tolérés dans les motifs.
    constexpr auto CI = QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption;
    static const QRegularExpression kw(QStringLiteral(
        R"(\b(?:wiggle(?:[- ]matching)?|d[ée]calage|offset|shift|delta(?!\s*r\b))\b|δ(?!\s*r\b))"), CI);
    const auto k = kw.match(question);
    if (!k.hasMatch()) return {};
    const int from = int(k.capturedEnd());
    const QString tail = question.mid(from, 80);
    const QString num = QStringLiteral(R"(([-+−]?\d+(?:[.,]\d+)?))");
    auto done = [&](const QRegularExpressionMatch& m, const QJsonObject& w) {
        if (start) *start = int(k.capturedStart());
        if (length) *length = from + int(m.capturedEnd()) - int(k.capturedStart());
        return w;
    };
    // « entre 100 et 300 », « de 100 à 300 », « [100 ; 300] », « range -300 et -200 », « intervalle 10 à 40 »
    static const QRegularExpression range(QStringLiteral(
        R"(^\D{0,25}?(?:entre\s+|de\s+|between\s+|from\s+|\[\s*|(?:range|intervalle|uniforme?|uniform)\s*(?:de\s+|entre\s+|:\s*|=\s*)?))") + num
                                          + QStringLiteral(R"(\s*(?:et|à|a|and|to|;|,|-)\s*)") + num + QStringLiteral(R"(\s*\]?)"), CI);
    if (const auto m = range.match(tail); m.hasMatch()) {
        double a = text::toNumber(m.captured(1)), b = text::toNumber(m.captured(2));
        if (a > b) std::swap(a, b);
        return done(m, QJsonObject{ { "type", "range" }, { "min", a }, { "max", b } });
    }
    // « 20 ± 5 » : gaussien
    static const QRegularExpression gauss(QStringLiteral(R"(^\D{0,25}?)") + num + QStringLiteral(R"(\s*)") + text::plusMinus()
                                          + QStringLiteral(R"(\s*(\d+(?:[.,]\d+)?))"), CI);
    if (const auto m = gauss.match(tail); m.hasMatch())
        return done(m, QJsonObject{ { "type", "gaussian" }, { "mean", text::toNumber(m.captured(1)) }, { "error", text::toNumber(m.captured(2)) } });
    // « wiggle de 20 ans » : fixe
    static const QRegularExpression fixed(QStringLiteral(R"(^\D{0,15}?)") + num, CI);
    if (const auto m = fixed.match(tail); m.hasMatch())
        return done(m, QJsonObject{ { "type", "fixed" }, { "value", text::toNumber(m.captured(1)) } });
    return {};
}

QVector<QJsonObject> MethodRunner::jsonRequests(const QString& text)
{
    QVector<QJsonObject> out;
    for (const QJsonObject& raw : topLevelObjects(text)) {
        const QJsonObject o = normalizeRequest(raw);
        const QString m = o.value("method").toString();
        static const QStringList extra = { "event", "combine", "relation", "inverse", "density_file" };
        if (!m.isEmpty() && (extra.contains(m) || MethodRegistry::instance().byId(m)) && out.size() < 12) out << o;
    }
    return out;
}

namespace {

// Objet JSON de premier niveau d'un texte, avec sa position (accolades équilibrées, chaînes respectées).
struct JsonSpan { qsizetype start = 0, length = 0; QJsonObject object; };
QVector<JsonSpan> jsonSpans(const QString& t)
{
    QVector<JsonSpan> out;
    int depth = 0;
    qsizetype start = -1;
    bool inStr = false, esc = false;
    for (qsizetype i = 0; i < t.size(); ++i) {
        const QChar c = t[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"' && depth > 0) inStr = true;
        else if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}' && depth > 0 && --depth == 0) {
            const QJsonDocument d = QJsonDocument::fromJson(t.mid(start, i - start + 1).toUtf8());
            if (d.isObject()) out.push_back({ start, i - start + 1, d.object() });
        }
    }
    return out;
}

// Demande de calcul reconnue : « method » d'une méthode de datation ou d'un calcul de SeldonH.
bool isCalcRequest(const QJsonObject& raw)
{
    const QString m = MethodRunner::normalizeRequest(raw).value("method").toString();
    static const QStringList extra = { "event", "combine", "relation", "inverse", "density_file" };
    return !m.isEmpty() && (extra.contains(m) || MethodRegistry::instance().byId(m));
}

} // namespace

bool MethodRunner::lastCalcBlock(const QString& md, qsizetype* start, qsizetype* end)
{
    static const QRegularExpression block(QStringLiteral("^```[ \\t]*(calcul|json)[ \\t]*\\n(.*?)^```[ \\t]*$"),
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption |
                                          QRegularExpression::CaseInsensitiveOption);
    bool found = false;
    auto it = block.globalMatch(md);
    while (it.hasNext()) {
        const auto m = it.next();
        bool calc = m.captured(1).compare(QLatin1String("calcul"), Qt::CaseInsensitive) == 0;
        if (!calc) for (const QJsonObject& o : objectsOfBlock(m.captured(2))) calc = calc || isCalcRequest(o);
        if (!calc) continue;
        found = true;
        if (start) *start = m.capturedStart();
        if (end) *end = m.capturedEnd();
    }
    return found;
}

QString MethodRunner::hideCalcBlocks(const QString& md, bool showJson)
{
    if (showJson) return md;
    const QString note = QStringLiteral("*Calcul confié à SeldonH.*");
    // 1. Blocs ```calcul, et ```json qui contiennent une demande de calcul (pas un modèle ChronoModel).
    static const QRegularExpression block(QStringLiteral("^```[ \\t]*(calcul|json)[ \\t]*\\n(.*?)^```[ \\t]*$"),
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption |
                                          QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype pos = 0;
    auto it = block.globalMatch(md);
    while (it.hasNext()) {
        const auto m = it.next();
        bool calc = m.captured(1).compare(QLatin1String("calcul"), Qt::CaseInsensitive) == 0;
        if (!calc) for (const QJsonObject& o : objectsOfBlock(m.captured(2))) calc = calc || isCalcRequest(o);
        if (!calc) continue;
        out += md.mid(pos, m.capturedStart() - pos) + note;
        pos = m.capturedEnd();
    }
    QString rest = md.mid(pos);
    // Bloc ```calcul encore ouvert (réponse en cours) : rien de partiel à l'écran.
    static const QRegularExpression open(QStringLiteral("^```[ \\t]*calcul[ \\t]*$"), QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    if (const auto mo = open.match(rest); mo.hasMatch()) rest = rest.left(mo.capturedStart()) + QStringLiteral("*Préparation du calcul…*");
    out += rest;
    // 2. JSON de calcul écrit dans le texte, sans balise : retiré (l'utilisateur n'a pas à écrire de requête).
    const QVector<JsonSpan> spans = jsonSpans(out);
    for (auto s = spans.crbegin(); s != spans.crend(); ++s)
        if (isCalcRequest(s->object)) {
            const bool inCode = s->start > 0 && out[s->start - 1] == QLatin1Char('`') && s->start + s->length < out.size()
                                && out[s->start + s->length] == QLatin1Char('`');
            const qsizetype a = inCode ? s->start - 1 : s->start, n = inCode ? s->length + 2 : s->length;
            out.replace(a, n, note);
        }
    return out;
}

QVector<QJsonObject> MethodRunner::extractCalcBlocks(const QString& answer)
{
    QVector<QJsonObject> out;
    static const QRegularExpression re("^```[ \\t]*(calcul|json)[ \\t]*\\n(.*?)^```",
                                       QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    auto it = re.globalMatch(answer);
    while (it.hasNext()) {
        const auto mm = it.next();
        const bool calcFence = mm.captured(1) == QLatin1String("calcul");
        for (const QJsonObject& raw : objectsOfBlock(mm.captured(2))) {
            if (out.size() >= 6) break;
            // Bloc ```json : seulement s'il contient une demande de calcul (« method » connue), pour ne pas prendre un modèle.
            if (raw.value("method").toString().isEmpty() && !(calcFence && raw.contains("plugin_id"))) continue;
            const QJsonObject o = normalizeRequest(raw);
            const QString m = o.value("method").toString();
            static const QStringList extra = { "event", "combine", "relation", "inverse", "density_file" };
            if (!calcFence && !extra.contains(m) && !MethodRegistry::instance().byId(m)) continue;
            out << o;
        }
    }
    // Sans bloc : demande écrite dans le texte ({"method":"inverse",…} au milieu d'une phrase), exécutée de même.
    if (out.isEmpty()) {
        static const QRegularExpression fence(QStringLiteral("^```.*?^```"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
        QString plain = answer;
        plain.replace(fence, QString());          // le JSON d'un modèle ChronoModel (bloc chronomodel) n'est pas une demande
        for (const JsonSpan& s : jsonSpans(plain))
            if (out.size() < 6 && isCalcRequest(s.object)) out << normalizeRequest(s.object);
    }
    return out;
}

} // namespace seldon
