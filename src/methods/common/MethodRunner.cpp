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

#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "Combination.h"
#include "CurveStore.h"
#include "DensityFile.h"
#include "../c14/C14Method.h"
#include "EventMcmc.h"
#include "InverseCalibration.h"
#include "MethodRegistry.h"
#include "TextUtil.h"
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
        if (method.isEmpty() || method == "event" || method == "combine" || method == "traces" || method == "inverse"
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
            const int dec = x.group.contains(QLatin1String("F14C")) ? 4 : (x.unit == QLatin1String("°") || x.unit == QLatin1String("µT")) ? 2 : 0;
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
    C.ok = true;
    return C;
}

// ------------------------------------------------------------------ traces MCMC exportées par ChronoModel

struct TracesRun {
    bool ok = false;
    QString html, prompt;
    QJsonArray series;               // densités de dates (graphique des datations)
    QJsonObject durationPlot;        // densité d'une durée : graphique à part (axe en années, pas en dates)
    QString name;
    double level = MethodRunner::kDefaultLevel;
};

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


// Colonnes de traces (ou années fixes, pour une borne d'intervalle) alignées tirage par tirage.
struct TraceColumns {
    std::vector<std::vector<double>> constants;      // années fixes, répétées sur tous les tirages
    std::vector<const std::vector<double>*> cols;
    QStringList names;
    std::vector<bool> isColumn;
    const TraceSet* set = nullptr;
};

bool traceColumns(const QStringList& wanted, bool allowYears, DateFormat fmt, TraceColumns& C, QString& error)
{
    std::vector<double> years(size_t(wanted.size()), std::nan(""));
    C.cols.assign(size_t(wanted.size()), nullptr);
    C.names = wanted;
    C.isColumn.assign(size_t(wanted.size()), false);
    // Un même fichier qui contient toutes les colonnes est préféré (noms présents dans plusieurs exports).
    QStringList columnNames;
    for (const QString& w : wanted) { int s = -1, c = -1; if (traces::resolve(w, s, c)) columnNames << w; }
    const int preferred = traces::setWithAll(columnNames);
    for (int k = 0; k < wanted.size(); ++k) {
        int s = -1, c = -1;
        QString err;
        if (traces::resolve(wanted[k], s, c, &err, preferred)) {
            const TraceSet& ts = traces::loaded()[s];
            if (C.set && (ts.rows() != C.set->rows() || ts.chainStarts != C.set->chainStarts)) {
                error = QStringLiteral("« %1 » et « %2 » viennent d'exports différents (nombre de tirages ou chaînes différents) : "
                                       "exporter les deux depuis le même calcul").arg(C.names.value(0), ts.columns[c]);
                return false;
            }
            if (!C.set) C.set = &ts;
            C.cols[size_t(k)] = &ts.values[size_t(c)];
            C.names[k] = ts.columns[c];
            C.isColumn[size_t(k)] = true;
        } else if (allowYears && std::isfinite(text::yearFromText(wanted[k]))) {
            years[size_t(k)] = text::yearFromText(wanted[k]);           // borne fixe : « -500 », « 500 av. J.-C. »
            C.names[k] = dateformat::format(years[size_t(k)], fmt);
        } else {
            error = err;
            return false;
        }
    }
    if (!C.set) { error = QStringLiteral("aucune colonne des traces chargées n'est citée"); return false; }
    C.constants.reserve(size_t(wanted.size()));
    for (size_t k = 0; k < size_t(wanted.size()); ++k)
        if (!C.cols[k]) { C.constants.emplace_back(C.set->rows(), years[k]); C.cols[k] = &C.constants.back(); }
    return true;
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

// Tests sur les traces : antéro-postérité de A et B (« order »), appartenance de C à l'intervalle [A ; B] (« inside »).
TracesRun runTracesProbability(const QJsonObject& rq, DateFormat fmt, const QString& op, const QString& from, const QString& to, const QString& of)
{
    TracesRun T;
    auto fail = [&](const QString& why) {
        T.html += QStringLiteral("<p><b>Traces MCMC</b></p><p><i>Calcul impossible : %1.</i></p>").arg(why.toHtmlEscaped());
        T.prompt += QStringLiteral("\nTraces MCMC — calcul impossible : %1.\n").arg(why);
        return T;
    };
    const bool inside = op == QLatin1String("inside");
    if (from.isEmpty() || to.isEmpty() || (inside && of.isEmpty()))
        return fail(inside ? QStringLiteral("il faut la colonne testée (« of ») et les deux bornes de l'intervalle (« from », « to »)")
                           : QStringLiteral("il faut deux colonnes : « from » (A) et « to » (B)"));
    TraceColumns C;
    QString err;
    if (!traceColumns(inside ? QStringList{ of, from, to } : QStringList{ from, to }, inside, fmt, C, err)) return fail(err);
    if (inside && !C.isColumn[0]) return fail(QStringLiteral("« %1 » n'est pas une colonne des traces chargées").arg(of));
    const TraceSet& S = *C.set;
    const size_t N = S.rows();

    // Indicatrices tirage par tirage
    std::vector<std::vector<double>> byChain(S.chainStarts.size());
    size_t chain = 0, kept = 0, n1 = 0, n2 = 0, n3 = 0;       // order : A<B, B<A, égaux ; inside : avant, dedans, après
    for (size_t i = 0; i < N; ++i) {
        while (chain + 1 < S.chainStarts.size() && i >= S.chainStarts[chain + 1]) ++chain;
        bool finite = true;
        for (const auto* c : C.cols) finite = finite && std::isfinite((*c)[i]);
        if (!finite) continue;
        ++kept;
        double ind = 0.0;
        if (!inside) {
            const double a = (*C.cols[0])[i], b = (*C.cols[1])[i];
            if (a < b) { ++n1; ind = 1.0; } else if (b < a) ++n2; else ++n3;
        } else {
            const double c = (*C.cols[0])[i];
            const double lo = std::min((*C.cols[1])[i], (*C.cols[2])[i]), hi = std::max((*C.cols[1])[i], (*C.cols[2])[i]);
            if (c < lo) ++n1; else if (c > hi) ++n3; else { ++n2; ind = 1.0; }
        }
        byChain[chain].push_back(ind);
    }
    if (kept < 10) return fail(QStringLiteral("moins de 10 tirages exploitables (valeurs vides dans les colonnes)"));
    const double ess = indicatorEss(byChain, kept);
    const QLocale fr(QLocale::French);

    QString rows;
    if (!inside) {
        const QString& A = C.names[0]; const QString& B = C.names[1];
        T.name = QStringLiteral("%1 / %2").arg(A, B);
        const QString head = QStringLiteral("Traces MCMC « %1 » — antéro-postérité de « %2 » et « %3 »").arg(S.file, A, B);
        T.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
        T.prompt += QStringLiteral("\n%1\n").arg(head);
        const double pAB = double(n1) / double(kept), pBA = double(n2) / double(kept);
        rows += QStringLiteral("<tr><td><b>P(« %1 » antérieur à « %2 »)</b></td><td>%3</td></tr>").arg(A.toHtmlEscaped(), B.toHtmlEscaped(), probabilityText(pAB, ess));
        rows += QStringLiteral("<tr><td><b>P(« %1 » antérieur à « %2 »)</b></td><td>%3</td></tr>").arg(B.toHtmlEscaped(), A.toHtmlEscaped(), probabilityText(pBA, ess));
        if (n3 > 0) rows += QStringLiteral("<tr><td>Tirages égaux</td><td>%1</td></tr>").arg(fr.toString(qlonglong(n3)));
        T.prompt += QStringLiteral("P(« %1 » antérieur à « %2 ») = %3 ; P(« %2 » antérieur à « %1 ») = %4 (proportion des tirages, erreur "
                                   "Monte-Carlo entre parenthèses).%5\n")
                        .arg(A, B, probabilityText(pAB, ess), probabilityText(pBA, ess),
                             n3 > 0 ? QStringLiteral(" %1 tirages égaux.").arg(n3) : QString());
    } else {
        const QString& Cn = C.names[0];
        const QString iv = QStringLiteral("[%1 ; %2]").arg(C.names[1], C.names[2]);
        T.name = QStringLiteral("%1 dans %2").arg(Cn, iv);
        const QString head = QStringLiteral("Traces MCMC « %1 » — appartenance de « %2 » à l'intervalle %3").arg(S.file, Cn, iv);
        T.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
        T.prompt += QStringLiteral("\n%1\n").arg(head);
        const double pb = double(n1) / double(kept), pi = double(n2) / double(kept), pa = double(n3) / double(kept);
        rows += QStringLiteral("<tr><td><b>P(« %1 » dans %2)</b></td><td>%3</td></tr>").arg(Cn.toHtmlEscaped(), iv.toHtmlEscaped(), probabilityText(pi, ess));
        rows += QStringLiteral("<tr><td>P(« %1 » avant l'intervalle)</td><td>%2</td></tr>").arg(Cn.toHtmlEscaped(), probabilityText(pb, ess));
        rows += QStringLiteral("<tr><td>P(« %1 » après l'intervalle)</td><td>%2</td></tr>").arg(Cn.toHtmlEscaped(), probabilityText(pa, ess));
        T.prompt += QStringLiteral("P(« %1 » dans %2) = %3 ; P(avant) = %4 ; P(après) = %5 (proportions des tirages, bornes prises à chaque "
                                   "tirage, la plus ancienne en premier ; erreur Monte-Carlo entre parenthèses).\n")
                        .arg(Cn, iv, probabilityText(pi, ess), probabilityText(pb, ess), probabilityText(pa, ess));
    }
    T.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(rows);
    const QString basis = QStringLiteral("%1 tirages a posteriori (%2 chaîne%3, ESS ≈ %4)")
                              .arg(fr.toString(qlonglong(kept))).arg(S.chainStarts.size()).arg(S.chainStarts.size() > 1 ? "s" : "")
                              .arg(fr.toString(qlonglong(std::llround(ess))));
    T.html += QStringLiteral("<p><small>Proportion des tirages, calculée tirage par tirage sur %1 : probabilité a posteriori dans le "
                             "modèle ChronoModel (contraintes et phases comprises), pas un test de signification.</small></p>")
                  .arg(basis.toHtmlEscaped());
    T.prompt += QStringLiteral("Calcul sur %1 (dépendance entre colonnes respectée).\n").arg(basis);

    // Figure : densités des colonnes en jeu, une par ligne.
    const double infv = std::numeric_limits<double>::infinity();
    for (size_t k = 0; k < C.cols.size(); ++k) {
        if (!C.isColumn[k]) continue;
        std::vector<double> v;
        for (double y : *C.cols[k]) if (std::isfinite(y)) v.push_back(y);
        const Density dc = kernelDensity(v, -infv, infv, Bandwidth::SheatherJones);
        if (dc.valid()) T.series.append(plotSeries(C.names[int(k)], dc, fmt));
    }
    QString lw;
    T.level = readLevel(rq, lw);
    T.ok = true;
    return T;
}

TracesRun runTraces(const QJsonObject& rq, DateFormat fmt)
{
    TracesRun T;
    auto fail = [&](const QString& why) {
        T.html += QStringLiteral("<p><b>Traces MCMC</b></p><p><i>Calcul impossible : %1.</i></p>").arg(why.toHtmlEscaped());
        T.prompt += QStringLiteral("\nTraces MCMC — calcul impossible : %1.\n").arg(why);
        return T;
    };
    auto str = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys) if (rq.value(k).isString() && !rq.value(k).toString().trimmed().isEmpty()) return rq.value(k).toString().trimmed();
        return QString();
    };
    const QString from = str({ "from", "a", "start" }), to = str({ "to", "b", "end" }), of = str({ "of", "column", "variable" });
    QString op = text::fold(rq.value("op").toString()).trimmed();
    static const QHash<QString, QString> ops = {
        { "duration", "duration" }, { "duree", "duration" }, { "ecart", "duration" }, { "difference", "duration" }, { "interval", "duration" },
        { "between", "between" }, { "entre", "between" }, { "date entre", "between" }, { "date_between", "between" },
        { "value", "value" }, { "valeur", "value" }, { "date", "value" }, { "densite", "value" }, { "density", "value" },
        { "column", "value" }, { "colonne", "value" },
        { "order", "order" }, { "anteriorite", "order" }, { "posteriorite", "order" }, { "antero-posteriorite", "order" },
        { "before", "order" }, { "avant", "order" }, { "precedence", "order" },
        { "inside", "inside" }, { "within", "inside" }, { "appartenance", "inside" }, { "dans", "inside" }, { "membership", "inside" } };
    if (ops.contains(op)) op = ops.value(op);
    else if (op.isEmpty()) op = !of.isEmpty() ? QStringLiteral("value") : QStringLiteral("duration");
    else return fail(QStringLiteral("opération « %1 » inconnue (duration, between, value, order ou inside)").arg(rq.value("op").toString()));
    if (op == QLatin1String("order") || op == QLatin1String("inside")) return runTracesProbability(rq, fmt, op, from, to, of);

    QString levelWarning;
    T.level = readLevel(rq, levelWarning);

    // Colonnes désignées
    const QStringList wanted = op == "value" ? QStringList{ of.isEmpty() ? from : of } : QStringList{ from, to };
    if (wanted.contains(QString()))
        return fail(op == "value" ? QStringLiteral("colonne à analyser absente (« of »)")
                                  : QStringLiteral("il faut deux colonnes : « from » (A) et « to » (B)"));
    std::vector<const std::vector<double>*> cols;
    QStringList colNames;
    const TraceSet* first = nullptr;
    const int preferred = traces::setWithAll(wanted);     // un même fichier contenant toutes les colonnes, si possible
    for (const QString& w : wanted) {
        int s = -1, c = -1;
        QString err;
        if (!traces::resolve(w, s, c, &err, preferred)) return fail(err);
        const TraceSet& ts = traces::loaded()[s];
        if (first && (ts.rows() != first->rows() || ts.chainStarts != first->chainStarts))
            return fail(QStringLiteral("« %1 » et « %2 » viennent d'exports différents (nombre de tirages ou chaînes différents) : "
                                       "exporter les deux depuis le même calcul").arg(colNames.value(0), ts.columns[c]));
        if (!first) first = &ts;
        cols.push_back(&ts.values[size_t(c)]);
        colNames << ts.columns[c];
    }
    const size_t N = first->rows();

    // Quantité dérivée, tirage par tirage (tirages où une colonne est vide ignorés).
    std::vector<double> x;
    std::vector<std::vector<double>> byChain(first->chainStarts.size());
    std::mt19937 rng(12345);                     // reproductible
    std::uniform_real_distribution<double> unif(0.0, 1.0);
    const int draws = op == "between" ? int(std::clamp<size_t>(200000 / std::max<size_t>(N, 1), 1, 10)) : 1;
    size_t chain = 0, kept = 0, positive = 0;
    for (size_t i = 0; i < N; ++i) {
        while (chain + 1 < first->chainStarts.size() && i >= first->chainStarts[chain + 1]) ++chain;
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
    const double inf = std::numeric_limits<double>::infinity();
    const Density d = kernelDensity(x, -inf, inf, Bandwidth::SheatherJones, &h, &rule);
    if (!d.valid()) return fail(QStringLiteral("densité impossible : tous les tirages ont la même valeur (colonne fixée ?)"));
    const DensityStats st = densityStats(d);

    const QString opText = op == "value" ? QStringLiteral("densité de « %1 »").arg(colNames[0])
                         : op == "duration" ? QStringLiteral("durée de « %1 » à « %2 » (%2 − %1)").arg(colNames[0], colNames[1])
                         : QStringLiteral("date comprise entre « %1 » et « %2 » (uniforme entre les deux, tirage par tirage)").arg(colNames[0], colNames[1]);
    const QString name = rq.value("name").toString().trimmed();
    T.name = name.isEmpty() ? (op == "value" ? colNames[0] : op == "duration" ? QStringLiteral("%1 → %2").arg(colNames[0], colNames[1])
                                                                            : QStringLiteral("entre %1 et %2").arg(colNames[0], colNames[1]))
                            : name;
    const QString head = QStringLiteral("Traces MCMC « %1 » — %2").arg(first->file, opText);
    T.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
    T.prompt += QStringLiteral("\n%1\n").arg(head);
    const QLocale fr(QLocale::French);
    const QString basis = QStringLiteral("%1 tirages a posteriori (%2 chaîne%3)%4")
                              .arg(fr.toString(qlonglong(kept))).arg(first->chainStarts.size()).arg(first->chainStarts.size() > 1 ? "s" : "")
                              .arg(op == "between" ? QStringLiteral(", %1 dates tirées par tirage").arg(draws) : QString());
    T.html += QStringLiteral("<p><i>Calcul tirage par tirage sur %1 : la dépendance entre les colonnes (contraintes, phases) est respectée.</i></p>")
                  .arg(basis.toHtmlEscaped());
    T.prompt += QStringLiteral("Calcul tirage par tirage sur %1 (dépendance entre colonnes respectée).\n").arg(basis);

    if (op == "duration") {
        // Durée : en années, pas une date.
        const dateformat::PrecisionScope precision(st.sd);
        QString rows, promptHpd;
        for (double l : displayLevels(T.level)) {
            const QString t = durationIntervalsText(hpdIntervals(d, l), fmt), lt = levelText(l);
            const bool asked = std::abs(l - T.level) < 1e-9 && std::abs(T.level - MethodRunner::kDefaultLevel) > 1e-9;
            rows += QStringLiteral("<tr><td valign='top'><b>HPD %1 %</b>%2</td><td>%3</td></tr>")
                        .arg(lt, asked ? QStringLiteral(" <i>(demandé)</i>") : QString(), QString(t).replace('\n', "<br>"));
            promptHpd += QStringLiteral("Intervalles HPD de la durée à %1 %%2 :\n- %3\n")
                             .arg(lt, asked ? QStringLiteral(" (niveau demandé par l'utilisateur)") : QString(), QString(t).replace("\n", "\n- "));
        }
        const double pPos = double(positive) / double(kept);
        T.html += QStringLiteral("<table cellspacing='0' cellpadding='3'>%1"
                                 "<tr><td><b>MAP (mode)</b></td><td>%2</td></tr>"
                                 "<tr><td><b>Moyenne ; écart-type</b></td><td>%3 ; %4</td></tr>"
                                 "<tr><td><b>Q1 ; Q2 (médiane) ; Q3</b></td><td>%5 ; %6 ; %7</td></tr>"
                                 "<tr><td><b>P(« %8 » postérieur à « %9 »)</b></td><td>%10 %</td></tr></table>")
                      .arg(rows, dateformat::spread(st.mode, fmt), dateformat::spread(st.mean, fmt), dateformat::spread(st.sd, fmt),
                           dateformat::spread(st.q1, fmt), dateformat::spread(st.median, fmt), dateformat::spread(st.q3, fmt),
                           colNames[1].toHtmlEscaped(), colNames[0].toHtmlEscaped(), text::num(100.0 * pPos, 1));
        T.prompt += promptHpd;
        T.prompt += QStringLiteral("Durée : MAP %1, moyenne %2, écart-type %3, Q1 %4, médiane %5, Q3 %6. Une durée négative signifie que "
                                   "« %7 » est antérieur à « %8 ». Probabilité que « %7 » soit postérieur à « %8 » : %9 %.\n")
                        .arg(dateformat::spread(st.mode, fmt), dateformat::spread(st.mean, fmt), dateformat::spread(st.sd, fmt),
                             dateformat::spread(st.q1, fmt), dateformat::spread(st.median, fmt), dateformat::spread(st.q3, fmt),
                             colNames[1], colNames[0], text::num(100.0 * pPos, 1));
        QJsonArray xs, ys;
        const size_t step = std::max<size_t>(1, d.t.size() / 400);
        for (size_t k = 0; k < d.t.size(); k += step) { xs.append(d.t[k] / dateformat::unitYears(fmt)); ys.append(d.v[k]); }
        T.durationPlot = QJsonObject{ { "title", QStringLiteral("Durée — %1").arg(T.name) },
                                      { "xlabel", fmt == DateFormat::Ka ? QStringLiteral("Durée (ka)") : fmt == DateFormat::Ma ? QStringLiteral("Durée (Ma)")
                                                                                                     : QStringLiteral("Durée (années)") },
                                      { "ylabel", QStringLiteral("Densité de probabilité") }, { "fill", true }, { "yaxis", false },
                                      { "hpd", T.level }, { "layout", QStringLiteral("rows") },
                                      { "series", QJsonArray{ QJsonObject{ { "name", T.name }, { "x", xs }, { "y", ys } } } } };
    } else {
        appendSummary(T.html, T.prompt, d, hpdIntervals(d, 95.0), hpdIntervals(d, 68.0), st, T.level, fmt);
        T.series.append(plotSeries(op == "between" ? QStringLiteral("Date entre %1 et %2").arg(colNames[0], colNames[1]) : colNames[0], d, fmt));
        if (op == "between")
            for (size_t c = 0; c < cols.size(); ++c) {
                std::vector<double> v;
                for (double y : *cols[c]) if (std::isfinite(y)) v.push_back(y);
                const Density dc = kernelDensity(v, -inf, inf, Bandwidth::SheatherJones);
                if (dc.valid()) T.series.append(plotSeries(colNames[int(c)], dc, fmt));
            }
    }
    if (!levelWarning.isEmpty()) {
        T.html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(levelWarning.toHtmlEscaped());
        T.prompt += QStringLiteral("Avertissement : %1.\n").arg(levelWarning);
    }

    // Lissage et convergence de la quantité calculée
    const QString hRule = rule == Bandwidth::SheatherJones ? QStringLiteral("Sheather-Jones") : QStringLiteral("Silverman (repli)");
    QString conv;
    size_t len = std::numeric_limits<size_t>::max();
    for (const std::vector<double>& c : byChain) len = std::min(len, c.size());
    if (op != "between" && byChain.size() >= 2 && len >= 4) {
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
    T.html += QStringLiteral("<p><small>Densité lissée par noyau gaussien, h = %1 (règle de %2)%3.</small></p>")
                  .arg(dateformat::spread(h, fmt).toHtmlEscaped(), hRule,
                       conv.isEmpty() ? QString() : QStringLiteral(" ; convergence de cette quantité : %1").arg(conv.toHtmlEscaped()));
    T.prompt += QStringLiteral("Densité lissée par noyau gaussien : h = %1 (règle de %2).%3\n")
                    .arg(dateformat::spread(h, fmt), hRule, conv.isEmpty() ? QString() : QStringLiteral(" Convergence de cette quantité : %1.").arg(conv));
    T.ok = true;
    return T;
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
                                  R.quantity == QLatin1String("Inclinaison") || R.quantity == QLatin1String("Déclinaison")
                                      || R.quantity == QLatin1String("Intensité") ? QStringLiteral("e") : QString(), when);
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

    const MethodRegistry& reg = MethodRegistry::instance();
    QString html = QStringLiteral("<b>🧮 Calcul fait par SeldonH</b> <i>(méthodes de ChronoModel)</i>");
    QString prompt = QStringLiteral("[Résultat de calcul de SeldonH — valeurs exactes, à reprendre telles quelles sans les recalculer ni les contredire]\n");
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
    bool anyInverse = false;         // calibration inverse : mesure attendue pour une date       // calcul sur des traces MCMC exportées par ChronoModel
    bool anyTraces = false;
    bool anyCurve = false;           // au moins une courbe de référence : le modèle doit la rappeler
    bool overlay = false;            // « layout »: « overlay » demandé : densités superposées ; sinon une par ligne
    bool anyColor = false;
    // Un modèle de langage met parfois un calcul « traces » ou « inverse » dans un événement ou une combinaison
    // ({"method":"event","dates":[{"method":"traces",…}]}) : ce ne sont pas des datations, ils sont exécutés seuls.
    QVector<QJsonObject> todo;
    for (const QJsonObject& r : requests) {
        const QJsonObject n = normalizeRequest(r);
        const QString m = n.value("method").toString();
        if ((m == QLatin1String("event") || m == QLatin1String("combine")) && n.value("dates").isArray() && !n.value("dates").toArray().isEmpty()) {
            QVector<QJsonObject> inner;
            for (const QJsonValue& d : n.value("dates").toArray()) {
                const QString dm = d.toObject().value("method").toString();
                if (dm == QLatin1String("traces") || dm == QLatin1String("inverse")) inner << d.toObject();
            }
            if (inner.size() == n.value("dates").toArray().size()) {
                for (QJsonObject d : inner) { if (n.contains("level") && !d.contains("level")) d["level"] = n.value("level"); todo << d; }
                continue;
            }
        }
        todo << r;
    }
    out.count = todo.size();
    for (const QJsonObject& rq0 : std::as_const(todo)) {
        if (rq0.value("layout").toString() == QLatin1String("overlay")) overlay = true;
        if (!colorOf(rq0).isEmpty()) anyColor = true;
        const QJsonObject rq = normalizeRequest(rq0);
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
        if (rq.value("method").toString() == "traces") {
            const TracesRun T = runTraces(rq, fmt);
            html += T.html;
            prompt += T.prompt;
            if (T.ok) {
                anyPosterior = true;
                levelsUsed << T.level;
                for (const QJsonValue& s : T.series) series.append(s);
                if (!T.series.isEmpty() && !plotNames.contains(T.name)) plotNames << T.name;
                if (!T.durationPlot.isEmpty()) out.extraPlots << T.durationPlot;
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
        QString head = reg.describe(rq);
        // Nom de la datation (champ « name », notamment d'un fichier .chr) : en tête de la fiche et dans le titre du graphique.
        const QString dateName = rq.value("name").toString().trimmed();
        if (!dateName.isEmpty()) head = head.isEmpty() ? dateName : QStringLiteral("%1 — %2").arg(dateName, head);
        MethodResult r = reg.compute(rq);

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
                    if (!(cutOld && w.contains(QLatin1String("limite ancienne"))) && !(cutNew && w.contains(QLatin1String("limite récente"))))
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
        }

        if (!r.ok) {
            html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(r.error.toHtmlEscaped());
            prompt += QStringLiteral("Calcul impossible : %1.\n").arg(r.error);
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
        for (const QString& w : r.warnings) html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());
        for (const QString& w : r.warnings) prompt += QStringLiteral("Avertissement : %1.\n").arg(w);

        QJsonObject ser = plotSeries(head, r.density, fmt);
        if (const QString col = colorOf(rq); !col.isEmpty()) ser["color"] = col;
        series.append(ser);
        if (!dateName.isEmpty() && !plotNames.contains(dateName)) plotNames << dateName;
    }

    // Remplissage du graphique : niveau demandé s'il est le même pour toutes les dates, sinon 95 %.
    double plotLevel = levelsUsed.isEmpty() ? kDefaultLevel : levelsUsed.first();
    for (double l : levelsUsed) if (std::abs(l - plotLevel) > 1e-9) plotLevel = kDefaultLevel;
    const bool custom = std::any_of(levelsUsed.begin(), levelsUsed.end(), [](double l) { return std::abs(l - kDefaultLevel) > 1e-9; });
    const QString summary = custom
        ? QStringLiteral("Résume d'abord par la HPD au niveau demandé par l'utilisateur, puis par les HPD à 95 % et 68 % pour comparaison")
        : QStringLiteral("Résume par les HPD à 95 % et 68 %");
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
    out.noticeHtml = html;
    out.promptBlock = prompt;
    return out;
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

RunOutcome MethodRunner::analyzeQuestion(const QString& question, DateFormat format)
{
    QVector<QJsonObject> rq = requestsFromQuestion(question);
    if (rq.isEmpty()) return RunOutcome();
    const DateFormat fmt = dateformat::detect(question, format);
    double a = 0, b = 0;
    if (periodFromQuestion(question, a, b)) {
        const double c1 = dateformat::toCalendar(a, fmt), c2 = dateformat::toCalendar(b, fmt);
        for (QJsonObject& o : rq) { o["tmin"] = std::min(c1, c2); o["tmax"] = std::max(c1, c2); }
    }
    return run(rq, fmt);
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

QVector<QJsonObject> MethodRunner::tracesFromQuestion(const QString& question)
{
    if (traces::loaded().isEmpty()) return {};
    const QVector<traces::Mention> ms = traces::mentions(question);
    if (ms.isEmpty()) return {};
    const QString q = text::fold(question);
    static const QRegularExpression duration(QStringLiteral(
        R"(\b(?:durees?|ecarts?|ecoule\w*|combien d'annees|combien de temps|temps (?:entre|ecoule|separant|qui separe)|separ\w*|)"
        R"(intervalle de temps|difference|duration|how long|time between|elapsed|time span)\b)"));
    static const QRegularExpression between(QStringLiteral(
        R"(\b(?:date|niveau|couche|evenement|event|episode|moment|occupation|sol|structure)\b.{0,60}\b(?:entre|between)\b|\bintermediaire\b|\bsitue\w* entre\b|\bin between\b)"));
    static const QRegularExpression value(QStringLiteral(
        R"(\b(?:debut|fin|commencement|begin\w*|end|start|hpd|densite|date|quand|when|intervalle)\b)"));
    // Appartenance à un intervalle (deux colonnes, une phase, ou deux années) ; antéro-postérité de deux colonnes.
    static const QRegularExpression inside(QStringLiteral(
        R"(\b(?:appartien\w*|appartenance|dans l'intervalle|inclu\w*|compris\w* (?:entre|dans)|contenu\w*|a l'interieur|pendant|durant|)"
        R"(au cours de|fait partie|dans la phase|within|inside|during|belong\w*|est(?:-(?:il|elle)| (?:il|elle))?\s+(?:bien\s+)?entre|)"
        R"(se (?:situe|trouve)\w*(?:-t-(?:il|elle))? (?:entre|dans)|lies? between|is .{0,30} between)\b)"));
    static const QRegularExpression order(QStringLiteral(
        R"(\b(?:anterieur\w*|posterieur\w*|anteriorite|posteriorite|antero-?posteriorite|precede\w*|avant|apres|plus anciens?|)"
        R"(plus ancienne|plus recents?|plus recente|older|younger|earlier|later|before|after)\b)"));
    QJsonObject o{ { "method", "traces" } };
    const QStringList phases = traces::phasesIn(question);
    if (ms.size() >= 2 && duration.match(q).hasMatch()) {
        o["op"] = QStringLiteral("duration");
        o["from"] = ms[0].column;
        o["to"] = ms[1].column;
    } else if (inside.match(q).hasMatch() && (ms.size() >= 3 || (ms.size() >= 1 && !phases.isEmpty()))) {
        // « N(300) appartient-il à la phase A ? », « US 5 est-il entre US 2 et US 8 ? »
        o["op"] = QStringLiteral("inside");
        o["of"] = ms[0].column;
        if (ms.size() >= 3) { o["from"] = ms[1].column; o["to"] = ms[2].column; }
        else { o["from"] = phases.first() + QStringLiteral(" Begin"); o["to"] = phases.first() + QStringLiteral(" End"); }
    } else if (inside.match(q).hasMatch() && ms.size() == 1) {
        // « N(300) est-il compris entre -500 et -400 ? » : bornes fixes
        static const QRegularExpression years(QStringLiteral(R"(\b(?:entre|between|de|from)\s+(-?\d+(?:[.,]\d+)?)\s*(?:et|a|and|to|-)\s*(-?\d+(?:[.,]\d+)?))"));
        const auto m = years.match(text::fold(text::erasToAstronomical(question)));
        if (!m.hasMatch()) return {};
        o["op"] = QStringLiteral("inside");
        o["of"] = ms[0].column;
        o["from"] = m.captured(1);
        o["to"] = m.captured(2);
    } else if (ms.size() >= 2 && order.match(q).hasMatch() && !between.match(q).hasMatch()) {
        // « N(500) est-il antérieur à N(200) ? »
        o["op"] = QStringLiteral("order");
        o["from"] = ms[0].column;
        o["to"] = ms[1].column;
    } else if (ms.size() >= 2 && between.match(q).hasMatch()) {
        o["op"] = QStringLiteral("between");
        o["from"] = ms[0].column;
        o["to"] = ms[1].column;
    } else if (ms.size() == 1 && value.match(q).hasMatch()) {
        o["op"] = QStringLiteral("value");
        o["of"] = ms[0].column;
    } else {
        return {};
    }
    return { o };
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

QVector<QJsonObject> MethodRunner::requestsFromQuestion(const QString& question)
{
    if (isModelRequest(question)) return {};
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
        static const QStringList known = { "c14", "14c", "f14c", "tl", "tl/osl", "osl", "gauss", "unif", "uniforme", "am", "radiocarbone" };
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
    if (const tools::ToolDef* def = tools::find(m); def && !def->methodId.isEmpty()) m = def->methodId;
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
        { "trace", "traces" }, { "chaines", "traces" }, { "chains", "traces" },
        { "calibration inverse", "inverse" }, { "inverse calibration", "inverse" }, { "inverse_calibration", "inverse" } };
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

    if ((m == "event" || m == "combine") && o.value("dates").isArray()) {
        QJsonArray ds;
        for (const QJsonValue& v : o.value("dates").toArray()) ds.append(v.isObject() ? QJsonValue(normalizeRequest(v.toObject())) : v);
        o["dates"] = ds;
    }
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
        static const QStringList extra = { "event", "combine", "traces", "inverse", "density_file" };
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
    static const QStringList extra = { "event", "combine", "traces", "inverse", "density_file" };
    return !m.isEmpty() && (extra.contains(m) || MethodRegistry::instance().byId(m));
}

} // namespace

QString MethodRunner::hideCalcBlocks(const QString& md, bool showJson)
{
    if (showJson) return md;
    const QString note = QStringLiteral("*🧮 Calcul confié à SeldonH.*");
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
    if (const auto mo = open.match(rest); mo.hasMatch()) rest = rest.left(mo.capturedStart()) + QStringLiteral("*🧮 Préparation du calcul…*");
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
            static const QStringList extra = { "event", "combine", "traces", "inverse", "density_file" };
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
