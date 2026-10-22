#include "MethodRunner.h"

#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "EventMcmc.h"
#include "MethodRegistry.h"
#include "TextUtil.h"
#include "Tools.h"

namespace seldon {

namespace {

QString intervalsText(const std::vector<Interval>& v, DateFormat fmt)
{
    QStringList items;
    for (const Interval& iv : v) {
        if (iv.prob < 0.005) continue;          // miettes de bruit numérique
        const bool point = std::lround(iv.from) == std::lround(iv.to);
        items << (point ? dateformat::format(iv.from, fmt)
                        : QStringLiteral("%1 → %2").arg(dateformat::format(iv.from, fmt), dateformat::format(iv.to, fmt)))
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
    double level = MethodRunner::kDefaultLevel;
};

EventRun runEvent(const QJsonObject& rq, DateFormat fmt)
{
    EventRun E;
    const MethodRegistry& reg = MethodRegistry::instance();
    const QString name = rq.value("name").toString().trimmed().isEmpty() ? QStringLiteral("Événement") : rq.value("name").toString().trimmed();
    const QJsonArray dates = rq.value("dates").toArray();
    const bool named = !rq.value("name").toString().trimmed().isEmpty();
    const QString head = QStringLiteral("%1 — combinaison bayésienne de %2 datation%3 (modèle d'événement de ChronoModel, MCMC)")
                             .arg(named ? QStringLiteral("Événement « %1 »").arg(name) : name).arg(dates.size()).arg(dates.size() > 1 ? "s" : "");
    E.html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
    E.prompt += QStringLiteral("\n%1\n").arg(head);
    auto fail = [&](const QString& why) {
        E.html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(why.toHtmlEscaped());
        E.prompt += QStringLiteral("Calcul impossible : %1.\n").arg(why);
        return E;
    };
    if (dates.isEmpty()) return fail(QStringLiteral("l'événement ne contient aucune datation (« dates »)"));

    // 1. Chaque datation est calibrée par sa méthode.
    std::vector<EventDateInput> inputs;
    std::vector<MethodResult> calibs;
    QStringList names;
    double lo = 1e300, hi = -1e300;
    for (int k = 0; k < dates.size(); ++k) {
        QJsonObject d = MethodRunner::normalizeRequest(dates[k].toObject());
        const QString method = d.value("method").toString().toLower();
        if (method.isEmpty() || method == "event") return fail(QStringLiteral("la datation %1 n'a pas de « method » valide").arg(k + 1));
        d["method"] = method;
        if (method == "am") { d["process"] = d.value("process").toString("I").toUpper(); if (!d.contains("steps")) d["steps"] = 500; }
        if (d.contains("curve")) d["curve"] = d.value("curve").toString().toLower();
        if (method == "c14" && d.contains("deltaR")) d["hasDeltaR"] = true;
        QString label = d.value("name").toString().trimmed();
        const QString desc = reg.describe(d);
        if (label.isEmpty()) label = desc.isEmpty() ? QStringLiteral("Date %1").arg(k + 1) : desc;
        const MethodResult r = reg.compute(d);
        if (!r.ok) return fail(QStringLiteral("datation %1 (%2) : %3").arg(k + 1).arg(label, r.error));
        Wiggle w;
        QString werr;
        if (!wiggleFromJson(d.value("wiggle"), w, werr)) return fail(QStringLiteral("datation %1 (%2) : %3").arg(k + 1).arg(label, werr));
        double dlo = 0, dhi = 0;
        if (w.type == Wiggle::Fixed) dlo = dhi = w.fixed;
        else if (w.type == Wiggle::Range) { dlo = w.min; dhi = w.max; }
        else if (w.type == Wiggle::Gaussian) { dlo = w.mean - 3 * w.error; dhi = w.mean + 3 * w.error; }
        lo = std::min(lo, r.density.t.front() + dlo);
        hi = std::max(hi, r.density.t.back() + dhi);
        inputs.push_back({ label.toStdString(), r.density, w });
        // Affichage : densité de t + δ (calibration « wiggle » de ChronoModel) ; le MCMC garde la vraisemblance sans décalage.
        MethodResult shown = r;
        if (w.type != Wiggle::None) {
            shown.density = wiggleDensity(r.density, w);
            shown.hpd95 = hpdIntervals(shown.density, 95.0);
        }
        calibs.push_back(shown);
        names << label;
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
        const QString c95 = intervalsText(calibs[k].hpd95, fmt), p95 = intervalsText(hpdIntervals(R.ti[k], 95.0), fmt);
        const QString y95 = wig ? intervalsText(hpdIntervals(R.yi[k], 95.0), fmt) : QString();
        const QString wt = wiggleText(inputs[k].wiggle);
        const QString sg = std::isfinite(R.sigmaMedian[k]) ? dateformat::spread(R.sigmaMedian[k], fmt) : QStringLiteral("—");
        QString post = QString(p95).replace('\n', "<br>");
        if (wig) post = QStringLiteral("t : %1<br>t + δ : %2").arg(post, QString(y95).replace('\n', "<br>"));
        rows += QStringLiteral("<tr><td valign='top'>%1%2</td><td valign='top'>%3</td><td valign='top'>%4</td><td valign='top'>%5</td></tr>")
                    .arg(names[int(k)].toHtmlEscaped(), wt.isEmpty() ? QString() : QStringLiteral("<br><i>%1</i>").arg(wt.toHtmlEscaped()),
                         QString(c95).replace('\n', "<br>"), post, sg);
        E.prompt += QStringLiteral("- %1%2 : calibration%6 HPD 95 % = %3 ; t a posteriori HPD 95 % = %4%7 ; σ médian = %5\n")
                        .arg(names[int(k)], wt.isEmpty() ? QString() : " (" + wt + ")", QString(c95).replace('\n', " ; "),
                             QString(p95).replace('\n', " ; "), sg, wig ? QStringLiteral(" de t + δ") : QString(),
                             wig ? QStringLiteral(" ; t + δ a posteriori HPD 95 % = %1").arg(QString(y95).replace('\n', " ; ")) : QString());
    }
    E.html += QStringLiteral("<table cellspacing='0' cellpadding='3'><tr><th align='left'>Datation</th><th align='left'>Calibration HPD 95 %</th>"
                             "<th align='left'>a posteriori HPD 95 %</th><th align='left'>σ médian</th></tr>%1</table>").arg(rows);

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
    E.html += QStringLiteral("<p><i>Seuils de ChronoModel : R̂ &lt; 1,01 et ESS ≥ %1 (500 par chaîne) : satisfaisant ; R̂ &lt; 1,05 et ESS ≥ %2 "
                             "(100 par chaîne) : à surveiller ; au-delà : non convergé. Acceptation : marche aléatoire du bloc (t, σ) "
                             "et décalage rigide de θ (cible ≈ 30–46 %).</i></p>")
                  .arg(text::num(500.0 * double(M), 0), text::num(100.0 * double(M), 0));
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
    const QString mcmcLine = QStringLiteral("%1 chaînes ; burn-in %2 ; adaptation ≤ %3 lots de %4 ; acquisition %5 itérations (thinning %6, "
                                            "%7 tirages par chaîne) ; mixing %8 ; recuit T max = 2^%9 tous les %10 itérations (dwell %11)")
                                 .arg(M).arg(used.numBurn).arg(used.maxBatches).arg(used.iterPerBatch).arg(used.numIter).arg(used.thinning)
                                 .arg(R.thetaTraces.empty() ? 0 : int(R.thetaTraces.front().size()))
                                 .arg(text::numAuto(used.mixingLevel, 4)).arg(used.annealTemp).arg(used.annealRecurrence).arg(used.annealDwell);
    const QString period = QStringLiteral("%1 → %2%3").arg(dateformat::format(tmin, fmt), dateformat::format(tmax, fmt),
                                                        autoPeriod ? QStringLiteral(" (calculée d'après les datations)") : QString());
    E.html += QStringLiteral("<p><i>MCMC : %1. Période d'étude (a priori uniforme de θ) : %2. Graines : %3 (à réutiliser pour "
                             "reproduire le calcul). Adaptation : %4. Durée : %5 s.</i></p>")
                  .arg(mcmcLine.toHtmlEscaped(), period.toHtmlEscaped(), seeds.join(";"),
                       adapt.isEmpty() ? QStringLiteral("toutes les chaînes adaptées") : adapt.join(", ").toHtmlEscaped(),
                       text::num(R.elapsedMs / 1000.0, 1));
    E.prompt += QStringLiteral("MCMC : %1. Période d'étude : %2. Graines : %3. %4.\n")
                    .arg(mcmcLine, period, seeds.join(";"), adapt.isEmpty() ? QStringLiteral("Toutes les chaînes sont adaptées") : adapt.join(", "));

    // 8. Figures : θ (rempli) + calibrations (traits) ; traces de θ par chaîne.
    QJsonObject ts0 = plotSeries(QStringLiteral("%1 : θ a posteriori").arg(name), R.theta, fmt);
    E.series.append(ts0);
    for (size_t k = 0; k < inputs.size(); ++k) {
        QJsonObject s = plotSeries((inputs[k].wiggle.type != Wiggle::None ? QStringLiteral("%1 (calibration de t + δ)") : QStringLiteral("%1 (calibration)")).arg(names[int(k)]), calibs[k].density, fmt);
        s["fill"] = false;
        s["width"] = 1.2;
        E.series.append(s);
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

} // namespace

void MethodRunner::setProgressHandler(std::function<bool(const QString&, double)> handler)
{
    progressHandler() = std::move(handler);
}

bool MethodRunner::asksEvent(const QString& question)
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(event|events|evenement|combine|combiner|combinaison|combinees?|combines|regroupe\w*|meme evenement)\b)"));
    return re.match(text::fold(question)).hasMatch();
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
    static const QRegularExpression chains(QStringLiteral(R"((\d{1,2})\s*chaines?\b)"));
    if (const auto mm = chains.match(q); mm.hasMatch()) m["num_proc"] = mm.captured(1).toInt();
    static const QRegularExpression thin(QStringLiteral(R"((?:thinning|intervalle d'echantillonnage)\s*(?:de\s*|=\s*|:\s*)?(\d+))"));
    if (const auto mm = thin.match(q); mm.hasMatch()) m["thinning_interval"] = mm.captured(1).toInt();
    static const QRegularExpression seeds(QStringLiteral(R"((?:graines?|seeds?)\s*(?:=|:)?\s*(\d+(?:\s*[;,]\s*\d+)*))"));
    if (const auto mm = seeds.match(q); mm.hasMatch()) m["seeds"] = mm.captured(1);
    static const QRegularExpression period(QStringLiteral(
        R"(periode d'etude\s*(?:de|:|=)?\s*(-?\d+)\s*(?:a|-|–|→|et)\s*(-?\d+))"));
    if (const auto mm = period.match(q); mm.hasMatch() && tmin && tmax) {
        *tmin = mm.captured(1).toDouble(); *tmax = mm.captured(2).toDouble();
    }
    return m;
}

double MethodRunner::requestedLevel(const QString& question)
{
    QString q = text::fold(question);
    // α95 (cône de confiance archéomagnétique) n'est pas un niveau de confiance d'intervalle : on l'efface d'abord.
    static const QRegularExpression alpha(QStringLiteral(R"(cone de confiance|(?:alpha|α|\ba)\s*-?\s*95\b)"));
    q.remove(alpha);
    // 1) « hpd 99 », « confiance de 90 % », « crédibilité 99,7 » : « % » facultatif
    static const QRegularExpression kw(QStringLiteral(
        R"(\b(?:hpd|confiance|credibilite)\b[^\d\n%]{0,30}?(\d{1,2}(?:[.,]\d{1,2})?)\s*(?:%|pour ?cent|\b))"));
    // 2) « niveau / seuil / probabilité / intervalle / région / à N % » : « % » obligatoire
    static const QRegularExpression pct(QStringLiteral(
        R"((?:\b(?:niveau|seuil|probabilite|intervalles?|regions?|a)\b)[^\d\n%]{0,20}?(\d{1,2}(?:[.,]\d{1,2})?)\s*(?:%|pour ?cent))"));
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
    QString html = QStringLiteral("<b>🧮 Calcul fait par SeldonH</b> <i>(méthodes de ChronoModel — valeurs exactes, non générées par le modèle)</i>");
    QString prompt = QStringLiteral("[Résultat de calcul de SeldonH — valeurs exactes, à reprendre telles quelles sans les recalculer ni les contredire]\n");
    QJsonArray series;
    QVector<double> levelsUsed;
    if (fmt != DateFormat::BcAd)
        prompt += QStringLiteral("Format des dates : %1. Toutes les dates ci-dessous sont déjà exprimées dans ce format.\n").arg(dateformat::title(fmt));

    bool anyEvent = false;
    for (const QJsonObject& rq0 : requests) {
        const QJsonObject rq = normalizeRequest(rq0);
        if (rq.value("method").toString() == "event") {
            const EventRun E = runEvent(rq, fmt);
            html += E.html;
            prompt += E.prompt;
            if (E.ok) {
                anyEvent = true;
                levelsUsed << E.level;
                for (const QJsonValue& s : E.series) series.append(s);
                if (!E.trace.isEmpty()) out.extraPlots << E.trace;
            }
            continue;
        }
        QString head = reg.describe(rq);
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

        html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
        prompt += QStringLiteral("\n%1\n").arg(head);

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

        series.append(plotSeries(head, r.density, fmt));
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
                             "dans le panneau de droite (courbe bleue, région HPD à %2 % remplie, sans échelle verticale) : n'écris aucun "
                             "bloc « plot » pour ces densités et ne décris aucun autre élément graphique (couleurs, légende, intervalles "
                             "en rouge…). %1 %3 : jamais de « 1σ » ou « 2σ » pour une densité calibrée. N'invente aucune autre valeur.\n")
                  .arg(fmt == DateFormat::BcAd ? QStringLiteral("Les années sont signées dans les graphiques (négatif = avant J.-C.).")
                                               : QStringLiteral("Les dates sont exprimées en %1.").arg(dateformat::title(fmt)),
                       levelText(plotLevel), summary);
    if (anyEvent)
        prompt += QStringLiteral("Pour l'événement : la densité a posteriori de θ est la courbe remplie, les calibrations individuelles des "
                                 "datations sont tracées en traits, et les traces des chaînes MCMC sont dans un autre onglet. Explique ce que "
                                 "signifie θ (date commune de l'événement), commente l'accord entre les datations (σ_i grand = date "
                                 "discordante, peu prise en compte) et la convergence (R̂, ESS) ; si la convergence n'est pas satisfaisante, "
                                 "conseille d'augmenter le nombre d'itérations ou de chaînes.\n");

    if (!series.isEmpty()) {
        out.plot["title"] = QStringLiteral("Densités calendaires (calcul SeldonH)");
        out.plot["xlabel"] = dateformat::axisLabel(fmt);
        if (dateformat::isAge(fmt)) out.plot["xreverse"] = true;      // ordre chronologique de gauche à droite
        out.plot["ylabel"] = QStringLiteral("Densité de probabilité");
        out.plot["fill"] = true;
        out.plot["yaxis"] = false;       // pas d'échelle verticale pour une densité
        out.plot["hpd"] = plotLevel;     // seule la région HPD (95 % ou niveau demandé) est remplie
        out.plot["series"] = series;
    }
    out.noticeHtml = html;
    out.promptBlock = prompt;
    return out;
}

bool MethodRunner::isModelRequest(const QString& question)
{
    const QString q = text::fold(question);
    static const QRegularExpression model(QStringLiteral(R"(\b(modele|modeles|modeliser|modelisation)\b|\.chr\b|projet chronomodel)"));
    static const QRegularExpression context(QStringLiteral(
        R"(\b(chronomodel|evenement|evenements|phase|phases|contrainte|contraintes|stratigraph\w*|fais|faire|cree|creer|construi\w*|genere|generer|prepare|exporte\w*)\b)"));
    return model.match(q).hasMatch() && context.match(q).hasMatch();
}

RunOutcome MethodRunner::analyzeQuestion(const QString& question, DateFormat format)
{
    if (isModelRequest(question)) return RunOutcome();
    QVector<QJsonObject> rq = MethodRegistry::instance().parse(question);
    // Fragment JSON collé dans la question (bloc chronomodel, .chr) : ses valeurs sont exactes, elles priment.
    if (const QVector<QJsonObject> js = jsonRequests(question); !js.isEmpty()) rq = js;
    // Décalage écrit dans la question (« avec un wiggle entre 100 et 300 ») : appliqué s'il n'y a qu'une date ;
    // avec plusieurs dates on ne sait pas laquelle est concernée, le modèle le demandera.
    if (rq.size() == 1 && !rq[0].contains("wiggle") && rq[0].value("method").toString() != "event")
        if (const QJsonObject w = wiggleFromQuestion(question); !w.isEmpty()) rq[0]["wiggle"] = w;
    const double level = requestedLevel(question);
    // « Calcule l'événement à partir de ces datations » : une seule demande « event » qui les combine (MCMC).
    if (rq.size() >= 2 && asksEvent(question)) {
        QJsonObject ev{ { "method", "event" } };
        static const QRegularExpression nameRe(QStringLiteral(
            R"RX((?:event|[ée]v[éè]nement)\s*(?:«\s*([^»]{1,40}?)\s*»|"([^"]{1,40})"|(?:nomm[ée]|appel[ée])\s+([^\s,;:.]{1,40})))RX"),
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
        rq = { ev };
    }
    if (level > 0)
        for (QJsonObject& o : rq) if (!o.contains("level")) o["level"] = level;
    return run(rq, dateformat::detect(question, format));
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
    static const char* const amKeys[][2] = { { "error_f", "errorF" }, { "refI_curve", "curveI" }, { "refD_curve", "curveD" },
                                              { "refF_curve", "curveF" }, { "curve_i", "curveI" }, { "curve_d", "curveD" },
                                              { "curve_f", "curveF" }, { "integration_steps", "steps" } };
    for (const auto& r : amKeys)
        if (o.contains(r[0]) && !o.contains(r[1])) {
            QJsonValue v = o.value(r[0]);
            if (v.isString()) v = v.toString().trimmed().toLower().remove(QRegularExpression(QStringLiteral("\\.ref$")));
            o[r[1]] = v;
            o.remove(r[0]);
        }
    static const char* const renames[][2] = { { "ref_curve", "curve" }, { "delta_r", "deltaR" }, { "delta_r_error", "deltaRerr" },
                                              { "ref_year", "refYear" }, { "err", "error" }, { "sigma", "error" } };
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
        { "evenement", "event" }, { "events", "event" } };
    if (alias.contains(m)) m = alias.value(m);
    if (!m.isEmpty()) o["method"] = m;

    // Nombres écrits en texte.
    static const char* const numeric[] = { "age", "error", "deltaR", "deltaRerr", "refYear", "min", "max", "inc", "dec", "alpha95",
                                           "field", "errorF", "f14c", "level", "tmin", "tmax" };
    for (const char* k : numeric)
        if (o.value(k).isString()) {
            const double d = numberFromText(o.value(k).toString());
            if (std::isfinite(d)) o[k] = d;
        }

    if (m == "event" && o.value("dates").isArray()) {
        QJsonArray ds;
        for (const QJsonValue& v : o.value("dates").toArray()) ds.append(v.isObject() ? QJsonValue(normalizeRequest(v.toObject())) : v);
        o["dates"] = ds;
    }
    return o;
}

QJsonObject MethodRunner::wiggleFromQuestion(const QString& question)
{
    const QString q = text::fold(question);
    static const QRegularExpression kw(QStringLiteral(R"(\b(wiggle|wiggle[- ]matching|decalage|delta(?!\s*r\b)|δ(?!\s*r\b))\b)"));
    const auto k = kw.match(q);
    if (!k.hasMatch()) return {};
    const QString tail = q.mid(k.capturedEnd(), 80);
    const QString num = QStringLiteral(R"((-?\d+(?:[.,]\d+)?))");
    // « entre 100 et 300 », « de 100 à 300 », « [100 ; 300] »
    static const QRegularExpression range(QStringLiteral(R"(^\D{0,25}?(?:entre\s+|de\s+|\[\s*))") + num
                                          + QStringLiteral(R"(\s*(?:et|a|à|;|,|-)\s*)") + num);
    if (const auto m = range.match(tail); m.hasMatch()) {
        double a = text::toNumber(m.captured(1)), b = text::toNumber(m.captured(2));
        if (a > b) std::swap(a, b);
        return QJsonObject{ { "type", "range" }, { "min", a }, { "max", b } };
    }
    // « 20 ± 5 » : gaussien
    static const QRegularExpression gauss(QStringLiteral(R"(^\D{0,25}?)") + num + QStringLiteral(R"(\s*)") + text::plusMinus()
                                          + QStringLiteral(R"(\s*(\d+(?:[.,]\d+)?))"));
    if (const auto m = gauss.match(tail); m.hasMatch())
        return QJsonObject{ { "type", "gaussian" }, { "mean", text::toNumber(m.captured(1)) }, { "error", text::toNumber(m.captured(2)) } };
    // « wiggle de 20 ans » : fixe
    static const QRegularExpression fixed(QStringLiteral(R"(^\D{0,15}?)") + num);
    if (const auto m = fixed.match(tail); m.hasMatch())
        return QJsonObject{ { "type", "fixed" }, { "value", text::toNumber(m.captured(1)) } };
    return {};
}

QVector<QJsonObject> MethodRunner::jsonRequests(const QString& text)
{
    QVector<QJsonObject> out;
    for (const QJsonObject& raw : topLevelObjects(text)) {
        const QJsonObject o = normalizeRequest(raw);
        const QString m = o.value("method").toString();
        if (!m.isEmpty() && (m == "event" || MethodRegistry::instance().byId(m)) && out.size() < 12) out << o;
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
            if (!calcFence && m != "event" && !MethodRegistry::instance().byId(m)) continue;
            out << o;
        }
    }
    return out;
}

} // namespace seldon
