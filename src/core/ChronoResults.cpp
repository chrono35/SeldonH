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

#include "ChronoResults.h"

#include "../skills/SkillManager.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace seldon {
namespace chronoresults {

namespace {

double num(QString s)
{
    s = s.trimmed();
    s.remove(QChar(0x00A0)).remove(QChar(0x202F)).remove(QLatin1Char(' '));
    s.replace(QLatin1Char(','), QLatin1Char('.'));
    return s.toDouble();
}

QString decodeEntities(QString s)
{
    static const QRegularExpression ent(QStringLiteral("&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);"));
    static const QHash<QString, QString> named = { { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
                                                   { "nbsp", " " }, { "middot", "·" }, { "plusmn", "±" }, { "hat", "^" } };
    QString out;
    int last = 0;
    auto it = ent.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        const QString e = m.captured(1);
        if (e.startsWith(QLatin1String("#x"))) out += QChar(char32_t(e.mid(2).toUInt(nullptr, 16)));
        else if (e.startsWith(QLatin1Char('#'))) {
            const uint c = e.mid(1).toUInt();
            out += c > 0xFFFF ? QString::fromUcs4(reinterpret_cast<const char32_t*>(&c), 1) : QString(QChar(char16_t(c)));
        } else out += named.value(e, m.captured(0));
        last = int(m.capturedEnd());
    }
    out += s.mid(last);
    return out;
}

} // namespace

QString htmlToText(const QString& html)
{
    QString t = html;
    t.remove(QRegularExpression(QStringLiteral("<(script|style)[^>]*>.*?</\\1>"),
                                QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption));
    t.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("\n"));
    t.replace(QRegularExpression(QStringLiteral("</(p|div|tr|h\\d|li|table|td)>"), QRegularExpression::CaseInsensitiveOption),
              QStringLiteral("\n"));
    t.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    t = decodeEntities(t);
    t.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    t.replace(QRegularExpression(QStringLiteral("\\n\\s*\\n+")), QStringLiteral("\n"));
    return t.trimmed();
}

bool looksLike(const QString& text)
{
    return text.contains(QLatin1String("ChronoModel")) && text.contains(QLatin1String("ESS"))
        && (text.contains(QLatin1String("Posterior Event Date")) || text.contains(QLatin1String("Posterior calib. date")));
}

namespace {

#define NUM "(-?[0-9][0-9.,]*)"

// Une ligne de statistiques de densité (bloc « Trace Stat. » puis densité, intervalle de crédibilité, HPD).
bool readStat(const QString& l, Density& d)
{
    static const QRegularExpression mapRe(QStringLiteral(R"(^MAP\s*=\s*)" NUM R"(\s*;\s*Mean\s*=\s*)" NUM R"(\s*;\s*Std\s*=\s*)" NUM));
    static const QRegularExpression qRe(QStringLiteral(R"(^Q1\s*=\s*)" NUM R"(\s*;\s*Q2\s*=\s*)" NUM R"(\s*;\s*Q3\s*=\s*)" NUM));
    static const QRegularExpression mmRe(QStringLiteral(R"(^min\s*=\s*)" NUM R"(\s*;\s*max\s*=\s*)" NUM));
    static const QRegularExpression ciRe(QStringLiteral(R"(^Credibility Interval\s*\(\s*)" NUM R"(\s*%\s*\)\s*:\s*\[\s*)" NUM R"(\s*;\s*)" NUM R"(\s*\]\s*(.*)$)"));
    static const QRegularExpression hpdRe(QStringLiteral(R"(^HPD Region\s*\(\s*)" NUM R"(\s*%\s*\)\s*:\s*(.*)$)"));
    static const QRegularExpression partRe(QStringLiteral(R"(\[\s*)" NUM R"(\s*;\s*)" NUM R"(\s*\]\s*\(\s*)" NUM R"(\s*%\s*\))"));
    if (const auto m = mapRe.match(l); m.hasMatch()) {
        if (!d.has) { d.map = num(m.captured(1)); d.mean = num(m.captured(2)); d.stdev = num(m.captured(3)); d.has = true; }
        return true;
    }
    if (const auto m = qRe.match(l); m.hasMatch()) {          // « Q2 (Median) » du Trace Stat. ne correspond pas
        if (d.has) { d.q1 = num(m.captured(1)); d.q2 = num(m.captured(2)); d.q3 = num(m.captured(3)); }
        return true;
    }
    if (const auto m = mmRe.match(l); m.hasMatch()) {
        if (!d.hasMinMax) { d.min = num(m.captured(1)); d.max = num(m.captured(2)); d.hasMinMax = true; }
        return true;
    }
    if (l.startsWith(QLatin1String("Warning: Sample is too sparse"))) { d.sparse = true; return true; }
    if (const auto m = ciRe.match(l); m.hasMatch()) {
        d.ciLevel = num(m.captured(1));
        d.ci = { num(m.captured(2)), num(m.captured(3)), d.ciLevel };
        if (d.unit.isEmpty()) d.unit = m.captured(4).trimmed();
        return true;
    }
    if (const auto m = hpdRe.match(l); m.hasMatch()) {
        d.hpdLevel = num(m.captured(1));
        d.hpd.clear();
        const QString rest = m.captured(2);
        int last = 0;
        auto it = partRe.globalMatch(rest);
        while (it.hasNext()) {
            const auto p = it.next();
            d.hpd << Interval{ num(p.captured(1)), num(p.captured(2)), num(p.captured(3)) };
            last = int(p.capturedEnd());
        }
        if (d.unit.isEmpty()) d.unit = rest.mid(last).trimmed();
        return true;
    }
    return false;
}

} // namespace

Report parse(const QString& text)
{
    Report R;
    const QStringList lines = text.split(QLatin1Char('\n'));
    static const QRegularExpression eventRe(QStringLiteral(R"(^\s*Event\s*:\s*(.*?)\s*$)"));
    static const QRegularExpression dataRe(QStringLiteral(R"(^\s*Data\s*:\s*(.*?)\s*$)"));
    static const QRegularExpression postRe(QStringLiteral(R"(^\s*Posterior\s+(Event Date|Shrinkage param\.?|calib\. date|Std ti)\s*$)"));
    static const QRegularExpression rhatRe(QStringLiteral(R"(R̂\s*=\s*([0-9.,]+))"));
    static const QRegularExpression bulkRe(QStringLiteral(R"(ESS bulk\s*=\s*([0-9]+))"));
    static const QRegularExpression tailRe(QStringLiteral(R"(ESS tail\s*=\s*([0-9]+))"));
    static const QRegularExpression thrRe(QStringLiteral(R"(\(<\s*([0-9]+)\))"));
    static const QRegularExpression headerRe(QStringLiteral(
        R"(ChronoModel\s+\d|Project filename|Elapsed|onverged|onvergence\s*\(|^\s*R̂\s*:|^\s*ESS\s*:)"));
    static const QRegularExpression phaseRe(QStringLiteral(R"(^Phase\s*:\s*(.*?)\s*$)"));
    static const QRegularExpression nEventsRe(QStringLiteral(R"(^Number of Events\s*:\s*([0-9]+))"));
    static const QRegularExpression partRe(QStringLiteral(R"(^(Begin|End|Duration)\s*\(posterior distrib\.\)\s*$)"));
    static const QRegularExpression rangeRe(QStringLiteral(
        R"(^(Phase Time Range|Transition Range|Gap Range)\s*\(\s*)" NUM R"(\s*%\s*\)\s*:\s*\[\s*)" NUM R"(\s*;\s*)" NUM R"(\s*\]\s*(.*)$)"));
    static const QRegularExpression succRe(QStringLiteral(R"(^Succession\s*:\s*from\s+(.*)$)"));
    static const QRegularExpression hiatusRe(QStringLiteral(R"(^Min Hiatus\s*(.*)$)"));

    QString event, data;
    int curVar = -1, curPhase = -1, curSucc = -1;
    Density* dens = nullptr;           // densité en cours de lecture (phase ou variable)
    bool inBody = false;
    for (const QString& raw : lines) {
        const QString l = raw.trimmed();
        if (l.isEmpty()) continue;
        if (!inBody && R.header.size() < 12 && headerRe.match(raw).hasMatch()) { R.header << l; continue; }

        // Phases et successions (avant les Events).
        if (!inBody) {
            if (const auto m = phaseRe.match(l); m.hasMatch()) {
                R.phases << Phase{}; R.phases.last().name = m.captured(1);
                curPhase = int(R.phases.size()) - 1; curSucc = -1; dens = nullptr;
                continue;
            }
            if (const auto m = succRe.match(l); m.hasMatch()) {
                // « from A to B » : les noms peuvent contenir « to » ; on préfère la coupure entre deux phases connues.
                const QString both = m.captured(1).trimmed();
                Succession sc;
                QVector<int> cuts;
                for (int i = int(both.indexOf(QLatin1String(" to "))); i >= 0; i = int(both.indexOf(QLatin1String(" to "), i + 1))) cuts << i;
                int cut = cuts.isEmpty() ? -1 : cuts.first();
                for (int c : cuts) {
                    const QString a = both.left(c).trimmed(), b = both.mid(c + 4).trimmed();
                    bool ka = false, kb = false;
                    for (const Phase& p : std::as_const(R.phases)) { ka = ka || p.name.trimmed() == a; kb = kb || p.name.trimmed() == b; }
                    if (ka && kb) { cut = c; break; }
                }
                sc.from = cut < 0 ? both : both.left(cut).trimmed();
                sc.to = cut < 0 ? QString() : both.mid(cut + 4).trimmed();
                R.successions << sc;
                curSucc = int(R.successions.size()) - 1; curPhase = -1; dens = nullptr;
                continue;
            }
            if (curPhase >= 0) {
                Phase& ph = R.phases[curPhase];
                if (const auto m = nEventsRe.match(l); m.hasMatch()) { ph.events = m.captured(1).toInt(); continue; }
                if (const auto m = partRe.match(l); m.hasMatch()) {
                    const QString k = m.captured(1);
                    dens = k == QLatin1String("Begin") ? &ph.begin : k == QLatin1String("End") ? &ph.end : &ph.duration;
                    continue;
                }
                if (const auto m = rangeRe.match(l); m.hasMatch() && m.captured(1) == QLatin1String("Phase Time Range")) {
                    ph.rangeLevel = num(m.captured(2));
                    ph.range = { num(m.captured(3)), num(m.captured(4)), ph.rangeLevel };
                    ph.hasRange = true;
                    dens = nullptr;
                    continue;
                }
                if (dens && readStat(l, *dens)) continue;
            }
            if (curSucc >= 0) {
                Succession& sc = R.successions[curSucc];
                if (const auto m = hiatusRe.match(l); m.hasMatch()) { sc.hiatus = m.captured(1).trimmed(); continue; }
                if (l == QLatin1String("No Gap")) { sc.gap = false; continue; }
                if (const auto m = rangeRe.match(l); m.hasMatch()) {
                    const Interval iv{ num(m.captured(3)), num(m.captured(4)), num(m.captured(2)) };
                    if (sc.unit.isEmpty()) sc.unit = m.captured(5).trimmed();
                    if (m.captured(1) == QLatin1String("Transition Range")) { sc.transition = iv; sc.level = iv.p; sc.hasTransition = true; }
                    else if (m.captured(1) == QLatin1String("Gap Range")) { sc.gapRange = iv; sc.gap = true; }
                    continue;
                }
            }
        }

        if (const auto m = eventRe.match(raw); m.hasMatch()) { inBody = true; event = m.captured(1); data.clear(); curVar = -1; dens = nullptr; ++R.events; continue; }
        if (const auto m = dataRe.match(raw); m.hasMatch()) { inBody = true; data = m.captured(1); curVar = -1; dens = nullptr; ++R.data; continue; }
        if (const auto m = postRe.match(raw); m.hasMatch() && inBody) {
            Variable v;
            const QString k = m.captured(1);
            v.kind = k.startsWith(QLatin1String("Event")) ? QStringLiteral("date de l'événement")
                   : k.startsWith(QLatin1String("Shrinkage")) ? QStringLiteral("σ de l'événement (shrinkage)")
                   : k.startsWith(QLatin1String("calib")) ? QStringLiteral("date calibrée")
                   : QStringLiteral("σ de la date (Std ti)");
            v.event = event;
            if (!k.startsWith(QLatin1String("Event")) && !k.startsWith(QLatin1String("Shrinkage"))) v.data = data;
            R.vars << v;
            curVar = int(R.vars.size()) - 1;
            dens = &R.vars[curVar].d;
            continue;
        }
        if (curVar < 0) continue;
        Variable* cur = &R.vars[curVar];
        const bool warn = l.contains(QStringLiteral("⚠")) || l.contains(QStringLiteral("❌"));
        if (const auto m = thrRe.match(l); m.hasMatch()) R.essThreshold = m.captured(1).toInt();
        if (const auto m = rhatRe.match(l); m.hasMatch() && !l.startsWith(QStringLiteral("R̂ :"))) {
            cur->rhat = num(m.captured(1));
            const bool oneLine = l.contains(QLatin1String("ESS bulk"));
            if (!oneLine) cur->rhatOk = !warn;
            else if (warn) cur->rhatOk = false;      // une ligne avec ⚠ : critère non précisé, on le marque sur R̂ s'il dépasse 1.01
        }
        if (const auto m = bulkRe.match(l); m.hasMatch()) {
            cur->essBulk = m.captured(1).toInt();
            if (!l.contains(QStringLiteral("R̂"))) cur->bulkOk = !warn;
        }
        if (const auto m = tailRe.match(l); m.hasMatch()) {
            cur->essTail = m.captured(1).toInt();
            if (!l.contains(QStringLiteral("R̂")) && !l.contains(QLatin1String("bulk"))) cur->tailOk = !warn;
        }
        if (dens) readStat(l, *dens);
    }
    for (Variable& v : R.vars) {
        if (v.d.has) { v.map = v.d.map; v.stdev = v.d.stdev; v.hasStats = true; }
        // Une ligne unique « R̂ · ESS bulk · ESS tail ⚠ » : on attribue l'échec d'après les seuils.
        if (!v.rhatOk && v.bulkOk && v.tailOk && v.rhat > 0 && v.rhat < 1.01 && (v.essBulk < R.essThreshold || v.essTail < R.essThreshold)) {
            v.rhatOk = true;
            v.bulkOk = v.essBulk >= R.essThreshold;
            v.tailOk = v.essTail >= R.essThreshold;
        }
    }
    return R;
}

#undef NUM

QString summarize(const Report& r, const QString& fileName, int maxListed)
{
    QString s = QStringLiteral("[Résultats ChronoModel — résumé exact extrait par SeldonH du fichier « %1 » ; le fichier complet "
                               "n'est pas transmis]\n").arg(fileName);
    for (const QString& h : r.header) s += h + QLatin1Char('\n');
    s += QStringLiteral("%1 Events, %2 datations, %3 variables suivies. Seuils : R̂ < 1,01 ; ESS bulk et ESS tail ≥ %4.\n")
             .arg(r.events).arg(r.data).arg(r.vars.size()).arg(r.essThreshold);

    // Comptes par type de variable : le modèle ne doit pas généraliser (« la plupart des Events… ») au-delà de ces nombres.
    static const QStringList kinds = { QStringLiteral("date de l'événement"), QStringLiteral("σ de l'événement (shrinkage)"),
                                       QStringLiteral("date calibrée"), QStringLiteral("σ de la date (Std ti)") };
    {
        QStringList parts;
        for (const QString& k : kinds) {
            int n = 0, nb = 0, nr = 0;
            for (const Variable& v : r.vars) if (v.kind == k) { ++n; if (!v.ok()) ++nb; if (!v.rhatOk) ++nr; }
            if (n == 0) continue;
            parts << QStringLiteral("%1 : %2/%3 hors critères (dont R̂ : %4)").arg(k).arg(nb).arg(n).arg(nr);
        }
        if (!parts.isEmpty())
            s += QStringLiteral("Par type de variable (comptes exacts, ne pas généraliser au-delà) : %1.\n").arg(parts.join(QStringLiteral(" ; ")));
    }

    auto label = [](const Variable& v) {
        return v.data.isEmpty() ? QStringLiteral("Event « %1 » — %2").arg(v.event, v.kind)
                                : QStringLiteral("Datation « %1 » (Event « %2 ») — %3").arg(v.data, v.event, v.kind);
    };
    // Variables hors critères, de la pire à la meilleure : R̂ en échec d'abord (le plus grand en tête), puis ESS le plus faible.
    QVector<Variable> bad;
    for (const Variable& v : r.vars) if (!v.ok()) bad << v;
    std::sort(bad.begin(), bad.end(), [](const Variable& a, const Variable& b) {
        if (a.rhatOk != b.rhatOk) return !a.rhatOk;
        if (!a.rhatOk && a.rhat != b.rhat) return a.rhat > b.rhat;
        return std::min(a.essBulk, a.essTail) < std::min(b.essBulk, b.essTail);
    });
    int rhatBad = 0, essBad = 0, minEss = 0;
    for (const Variable& v : bad) {
        if (!v.rhatOk) ++rhatBad;
        if (!v.bulkOk || !v.tailOk) {
            ++essBad;
            const int e = std::min(v.bulkOk ? v.essTail : v.essBulk, v.tailOk ? v.essBulk : v.essTail);
            if (minEss == 0 || e < minEss) minEss = e;
        }
    }
    // Verdict global calibré sur les proportions (et non sur la seule mention « Not converged »).
    {
        const int n = std::max(1, int(r.vars.size()));
        double maxRhat = 0.0;
        for (const Variable& v : r.vars) maxRhat = std::max(maxRhat, v.rhat);
        const double fr = 100.0 * rhatBad / n, fb = 100.0 * bad.size() / n;
        const QString pr = QString::number(fr, 'f', 1), pb = QString::number(fb, 'f', 1);
        QString verdict;
        if (bad.isEmpty())
            verdict = QStringLiteral("bonne : tous les critères sont remplis, les résultats peuvent être interprétés.");
        else if (rhatBad == 0)
            verdict = QStringLiteral("R̂ bon pour toutes les variables, ESS insuffisant pour %1 % des variables : les chaînes ont convergé "
                                     "mais l'échantillon est trop petit ; augmenter le nombre d'itérations d'acquisition.").arg(pb);
        else if (fr < 1.0 && maxRhat < 1.02)
            verdict = QStringLiteral("presque atteinte (R̂ en échec pour %1 % des variables, R̂ max = %2) : résultats utilisables avec prudence ; "
                                     "relancer avec les réglages suggérés pour confirmer.").arg(pr, QString::number(maxRhat, 'f', 4));
        else if (fr <= 5.0 && maxRhat < 1.05)
            verdict = QStringLiteral("partielle (R̂ en échec pour %1 % des variables, R̂ max = %2) : les variables listées ne sont pas fiables, "
                                     "le reste peut être lu avec prudence ; relancer avant toute conclusion définitive.").arg(pr, QString::number(maxRhat, 'f', 4));
        else
            verdict = QStringLiteral("insuffisante (R̂ en échec pour %1 % des variables, R̂ max = %2) : les résultats ne sont pas fiables en l'état ; "
                                     "relancer le calcul.").arg(pr, QString::number(maxRhat, 'f', 4));
        s += QStringLiteral("Verdict de convergence (calculé par SeldonH, à reprendre tel quel) : %1\n").arg(verdict);
    }
    if (bad.isEmpty()) {
        s += QStringLiteral("Toutes les variables remplissent les critères de convergence.\n");
    } else {
        s += QStringLiteral("Variables hors critères : %1 (R̂ : %2 ; ESS : %3), de la pire à la meilleure :\n").arg(bad.size()).arg(rhatBad).arg(essBad);
        for (int i = 0; i < bad.size() && i < maxListed; ++i) {
            const Variable& v = bad[i];
            QStringList c;
            c << QStringLiteral("R̂ = %1%2").arg(QString::number(v.rhat, 'f', 4), v.rhatOk ? QString() : QStringLiteral(" ⚠"));
            c << QStringLiteral("ESS bulk = %1%2").arg(v.essBulk).arg(v.bulkOk ? QString() : QStringLiteral(" ⚠"));
            c << QStringLiteral("ESS tail = %1%2").arg(v.essTail).arg(v.tailOk ? QString() : QStringLiteral(" ⚠"));
            s += QStringLiteral("%1. %2 : %3\n").arg(i + 1).arg(label(v), c.join(QStringLiteral(" ; ")));
        }
        if (bad.size() > maxListed) s += QStringLiteral("… et %1 autres variables hors critères (moins mauvaises).\n").arg(bad.size() - maxListed);
        if (minEss > 0)
            s += QStringLiteral("ESS le plus faible : %1 pour un seuil de %2 -> facteur d'itérations d'acquisition suggéré : ×%3 "
                                "(si R̂ est bon).\n").arg(minEss).arg(r.essThreshold)
                     .arg(QString::number(std::ceil(10.0 * r.essThreshold / minEss) / 10.0, 'f', 1));
    }

    // σ des dates (MAP de « Posterior Std ti ») comparé à l'écart-type de la date calibrée.
    QHash<QString, double> dateStd;
    QHash<QString, bool> dataOk;      // convergence de la date calibrée et de son σ
    for (const Variable& v : r.vars) {
        if (v.data.isEmpty()) continue;
        const QString key = v.event + QChar(0x1f) + v.data;
        dataOk[key] = dataOk.value(key, true) && v.ok();
        if (v.kind == QStringLiteral("date calibrée") && v.hasStats) dateStd[key] = v.stdev;
    }
    int bigConverged = 0;
    QHash<QString, int> nDates, nBig;
    QStringList big;
    for (const Variable& v : r.vars) {
        if (v.kind != QStringLiteral("σ de la date (Std ti)") || !v.hasStats) continue;
        const QString key = v.event + QChar(0x1f) + v.data;
        ++nDates[v.event];
        const double sd = dateStd.value(key, -1.0);
        if (sd > 0 && v.map > sd) {
            ++nBig[v.event];
            const bool conv = dataOk.value(key, true);
            if (conv) ++bigConverged;
            big << QStringLiteral("« %1 » (Event « %2 ») : σ MAP = %3 > écart-type de la date = %4 (rapport %5) ; %6")
                       .arg(v.data, v.event, QString::number(v.map, 'f', 1), QString::number(sd, 'f', 1),
                            QString::number(v.map / sd, 'f', 1),
                            conv ? QStringLiteral("convergence bonne (σ grand réel, pas un défaut de convergence)")
                                 : QStringLiteral("convergence aussi hors critères"));
        }
    }
    if (big.isEmpty()) {
        s += QStringLiteral("Aucune datation n'a un σ (MAP) supérieur à son écart-type.\n");
    } else {
        s += QStringLiteral("Datations dont le σ (MAP de Posterior Std ti) dépasse l'écart-type de la date calibrée (outliers possibles) : %1, "
                            "dont %2 avec une convergence bonne (liste distincte de celle des variables hors critères) :\n")
                 .arg(big.size()).arg(bigConverged);
        for (int i = 0; i < big.size() && i < maxListed; ++i) s += QStringLiteral("- %1\n").arg(big[i]);
        if (big.size() > maxListed) s += QStringLiteral("- … et %1 autres.\n").arg(big.size() - maxListed);
        QStringList inv, single;
        for (auto it = nBig.constBegin(); it != nBig.constEnd(); ++it) {
            if (it.value() != nDates.value(it.key())) continue;
            if (it.value() > 1) inv << QStringLiteral("« %1 » (%2 datations)").arg(it.key()).arg(it.value());
            else single << QStringLiteral("« %1 »").arg(it.key());
        }
        inv.sort();
        single.sort();
        if (!inv.isEmpty())
            s += QStringLiteral("Events dont TOUTES les datations (plusieurs) ont un σ grand : inversion stratigraphique possible : %1 "
                                "(à mentionner obligatoirement)\n").arg(inv.join(", "));
        if (!single.isEmpty())
            s += QStringLiteral("Events à une seule datation, au σ grand : date aberrante (outlier) ou inversion stratigraphique possible : %1 "
                                "(mentionner les deux possibilités)\n").arg(single.join(", "));
    }
    s += QStringLiteral("Consignes : ne jamais proposer de retirer (même « après vérification »), déplacer, décaler (wiggle) ou "
                        "rééchantillonner une date ou un Event, ni d'ajouter des contraintes ; seulement inviter à bien contrôler le "
                        "modèle chronologique (contraintes existantes, attribution des dates aux Events, données). N'affirmer que ce "
                        "que ces nombres établissent.\n");
    return s;
}

// ------------------------------------------------------------ phases, successions, Events

namespace {

QString fmt(double v)
{
    const double r = std::abs(v) < 10.0 && std::abs(v - std::round(v)) > 1e-9 ? std::round(v * 10.0) / 10.0 : std::round(v);
    return QString::number(r == 0.0 ? 0.0 : r, 'f', std::abs(r - std::round(r)) > 1e-9 ? 1 : 0);
}

QString interval(const Interval& iv) { return QStringLiteral("[%1 ; %2]").arg(fmt(iv.a), fmt(iv.b)); }

QString level(double l) { return QString::number(l, 'f', std::abs(l - std::round(l)) < 0.05 ? 0 : 1); }

// HPD : morceaux d'au moins 1 % (les miettes de l'estimation de densité sont seulement signalées), du plus probable
// au moins probable ; un seul morceau -> « [a ; b] ».
QVector<Interval> hpdParts(const Density& d)
{
    QVector<Interval> parts;
    for (const Interval& iv : d.hpd) if (iv.p >= 1.0 || d.hpd.size() == 1) parts << iv;
    std::sort(parts.begin(), parts.end(), [](const Interval& a, const Interval& b) { return a.p > b.p; });
    return parts;
}

QString hpdText(const Density& d)
{
    if (d.hpd.isEmpty()) return QStringLiteral("—");
    const QVector<Interval> parts = hpdParts(d);
    QStringList out;
    for (const Interval& iv : parts)
        out << (parts.size() == 1 && d.hpd.size() == 1 ? interval(iv) : QStringLiteral("%1 (%2 %)").arg(interval(iv), QString::number(iv.p, 'f', 1)));
    QString s = out.join(QStringLiteral(" ∪ "));
    if (parts.size() < d.hpd.size()) s += QStringLiteral(" + morceaux < 1 %");
    return s;
}

// Échelle d'âges (BP, b2k, ka) : les valeurs grandes sont les plus anciennes.
bool isAgeScale(const QString& unit)
{
    return unit.contains(QLatin1String("BP")) || unit.contains(QLatin1String("b2k")) || unit.contains(QLatin1String("ka"), Qt::CaseInsensitive)
        || unit.contains(QLatin1String("Age"), Qt::CaseInsensitive);
}

QString dateUnit(const Report& r)
{
    for (const Phase& p : r.phases) if (!p.begin.unit.isEmpty()) return p.begin.unit;
    for (const Variable& v : r.vars) if (!v.d.unit.isEmpty() && v.kind == QStringLiteral("date de l'événement")) return v.d.unit;
    return QString();
}

// Une borne de l'intervalle de crédibilité égale à l'extrême de l'échantillon : la densité s'appuie sur une limite
// (en pratique la période d'étude), la valeur n'est pas contrainte par les données de ce côté.
bool atBound(const Density& d, double* where = nullptr)
{
    if (!d.hasMinMax || d.ciLevel <= 0) return false;
    const double tol = std::max(0.5, 1e-6 * std::abs(d.max));
    if (std::abs(d.ci.b - d.max) < tol) { if (where) *where = d.max; return true; }
    if (std::abs(d.ci.a - d.min) < tol) { if (where) *where = d.min; return true; }
    return false;
}

QVector<const Phase*> chronological(const Report& r)
{
    QVector<const Phase*> ph;
    for (const Phase& p : r.phases) ph << &p;
    const bool age = isAgeScale(dateUnit(r));
    auto key = [](const Phase* p) { return p->begin.q2 != 0.0 ? p->begin.q2 : p->begin.map; };
    std::stable_sort(ph.begin(), ph.end(), [&](const Phase* a, const Phase* b) { return age ? key(a) > key(b) : key(a) < key(b); });
    return ph;
}

QString statCell(const Density& d)
{
    if (!d.has) return QStringLiteral("—");
    return QStringLiteral("MAP %1<br>moy. %2 ± %3<br>Q1–Q3 %4–%5<br>HPD %6 % %7")
        .arg(fmt(d.map), fmt(d.mean), fmt(d.stdev), fmt(d.q1), fmt(d.q3), level(d.hpdLevel), hpdText(d).toHtmlEscaped());
}

QString statLine(const Density& d)
{
    if (!d.has) return QStringLiteral("—");
    return QStringLiteral("MAP %1, moy. %2 ± %3, Q1–Q3 %4–%5, HPD %6 % %7")
        .arg(fmt(d.map), fmt(d.mean), fmt(d.stdev), fmt(d.q1), fmt(d.q3), level(d.hpdLevel), hpdText(d));
}

QString phaseLine(const Phase& p)
{
    QString s = QStringLiteral("- « %1 » (%2 Event%3) : début %4 ; fin %5 ; ")
                    .arg(p.name.trimmed()).arg(p.events).arg(p.events > 1 ? "s" : "").arg(statLine(p.begin), statLine(p.end));
    s += p.events == 1 ? QStringLiteral("durée nulle (un seul Event)") : QStringLiteral("durée %1").arg(statLine(p.duration));
    if (p.hasRange) s += QStringLiteral(" ; Time Range %1 % %2").arg(level(p.rangeLevel), interval(p.range));
    return s + QLatin1Char('\n');
}

QString successionLine(const Succession& s)
{
    QString l = QStringLiteral("- « %1 » → « %2 » : ").arg(s.from, s.to);
    if (s.hasTransition) l += QStringLiteral("Transition Range %1 % %2 ; ").arg(level(s.level), interval(s.transition));
    l += s.gap ? QStringLiteral("Gap Range %1 % %2").arg(level(s.gapRange.p), interval(s.gapRange)) : QStringLiteral("pas de gap (No Gap)");
    l += QStringLiteral(" ; hiatus minimal a priori : %1").arg(s.hiatus.isEmpty() || s.hiatus == QLatin1String("unknown")
                                                                   ? QStringLiteral("non fixé") : s.hiatus);
    return l + QLatin1Char('\n');
}

QVector<const Variable*> eventDates(const Report& r)
{
    QVector<const Variable*> ev;
    for (const Variable& v : r.vars) if (v.kind == QStringLiteral("date de l'événement")) ev << &v;
    return ev;
}

QString eventLine(const Variable& v)
{
    return QStringLiteral("- « %1 » : %2%3\n").arg(v.event, statLine(v.d), v.ok() ? QString() : QStringLiteral(" (convergence hors critères ⚠)"));
}

bool mentioned(const QString& normQuestion, const QString& name)
{
    const QString n = SkillManager::normalize(name);
    return n.size() >= 3 && normQuestion.contains(n);
}

} // namespace

int focusOf(const QString& question)
{
    const QString q = SkillManager::normalize(question);
    int f = 0;
    static const QRegularExpression phaseRe(QStringLiteral(R"(\b(phases?|debut|fin|durees?|begin|end|duration|time range|start)\b)"));
    static const QRegularExpression succRe(QStringLiteral(R"(\b(successions?|hiatus|gaps?|transitions?|transition range|lacunes?|interruptions?)\b)"));
    static const QRegularExpression eventRe(QStringLiteral(R"(\b(events?|evenements?)\b)"));
    static const QRegularExpression tableRe(QStringLiteral(R"(\b(tableaux?|tables?|synthe\w*|liste\w*|lists?|recapitul\w*|resume\w*|summar\w*|toutes? les|all)\b)"));
    if (phaseRe.match(q).hasMatch()) f |= FocusPhases;
    if (succRe.match(q).hasMatch()) f |= FocusSuccessions;
    if (eventRe.match(q).hasMatch()) f |= FocusEvents;
    if (f && tableRe.match(q).hasMatch()) f |= FocusTable;
    return f;
}

QString tablesHtml(const Report& r, int focus)
{
    QString html;
    const QString unit = dateUnit(r).toHtmlEscaped();
    if ((focus & FocusPhases) && !r.phases.isEmpty()) {
        QString rows;
        for (const Phase* p : chronological(r)) {
            QString dur = p->events == 1 ? QStringLiteral("0 (un seul Event)") : statCell(p->duration);
            rows += QStringLiteral("<tr><td valign='top'><b>%1</b></td><td valign='top' align='right'>%2</td><td valign='top'>%3</td>"
                                   "<td valign='top'>%4</td><td valign='top'>%5</td><td valign='top'>%6</td></tr>")
                        .arg(p->name.trimmed().toHtmlEscaped()).arg(p->events).arg(statCell(p->begin), statCell(p->end), dur,
                             p->hasRange ? QStringLiteral("%1 %<br>%2").arg(level(p->rangeLevel), interval(p->range)) : QStringLiteral("—"));
        }
        html += QStringLiteral("<p><b>Phases (%1), du plus ancien au plus récent — valeurs exactes du fichier%2</b></p>"
                               "<table cellspacing='0' cellpadding='3'><tr><th align='left'>Phase</th><th>Events</th>"
                               "<th align='left'>Début</th><th align='left'>Fin</th><th align='left'>Durée (années)</th>"
                               "<th align='left'>Time Range</th></tr>%3</table>")
                    .arg(r.phases.size()).arg(unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(unit), rows);
    }
    if ((focus & (FocusPhases | FocusSuccessions)) && !r.successions.isEmpty()) {
        QString rows;
        for (const Succession& s : r.successions)
            rows += QStringLiteral("<tr><td valign='top'>%1 → %2</td><td valign='top'>%3</td><td valign='top'>%4</td><td valign='top'>%5</td></tr>")
                        .arg(s.from.toHtmlEscaped(), s.to.toHtmlEscaped(),
                             s.hasTransition ? QStringLiteral("%1 % %2").arg(level(s.level), interval(s.transition)) : QStringLiteral("—"),
                             s.gap ? QStringLiteral("%1 % %2").arg(level(s.gapRange.p), interval(s.gapRange)) : QStringLiteral("aucun"),
                             s.hiatus.isEmpty() || s.hiatus == QLatin1String("unknown") ? QStringLiteral("non fixé") : s.hiatus.toHtmlEscaped());
        html += QStringLiteral("<p><b>Successions de phases (%1)</b></p><table cellspacing='0' cellpadding='3'><tr><th align='left'>Succession</th>"
                               "<th align='left'>Transition Range</th><th align='left'>Gap</th><th align='left'>Hiatus min. a priori</th></tr>%2</table>")
                    .arg(r.successions.size()).arg(rows);
    }
    if (focus & FocusEvents) {
        const QVector<const Variable*> ev = eventDates(r);
        if (!ev.isEmpty()) {
            QString rows;
            for (const Variable* v : ev)
                rows += QStringLiteral("<tr><td valign='top'>%1</td><td align='right'>%2</td><td align='right'>%3 ± %4</td><td align='right'>%5–%6</td>"
                                       "<td>%7</td><td>%8</td></tr>")
                            .arg(v->event.toHtmlEscaped(), fmt(v->d.map), fmt(v->d.mean), fmt(v->d.stdev), fmt(v->d.q1), fmt(v->d.q3),
                                 hpdText(v->d).toHtmlEscaped(), v->ok() ? QStringLiteral("✅") : QStringLiteral("⚠️"));
            html += QStringLiteral("<p><b>Events (%1) — date a posteriori%2</b></p><table cellspacing='0' cellpadding='3'><tr><th align='left'>Event</th>"
                                   "<th>MAP</th><th>Moyenne ± écart-type</th><th>Q1–Q3</th><th align='left'>HPD %3 %</th><th>Conv.</th></tr>%4</table>")
                        .arg(ev.size()).arg(unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(unit),
                             level(ev.first()->d.hpdLevel), rows);
        }
    }
    return html;
}

QString digest(const Report& r, int focus, const QString& question, int maxChars, bool tableShown)
{
    const QString q = SkillManager::normalize(question);
    const QString unit = dateUnit(r);
    QString s = QStringLiteral("[Phases, successions et Events — valeurs exactes extraites par SeldonH du fichier de résultats%1. "
                               "Dates en %2. HPD : niveau indiqué par ChronoModel (le fichier ne donne pas de HPD à 68 %). "
                               "Ne recalcule rien, n'invente aucune valeur absente.]\n")
                    .arg(tableShown ? QStringLiteral(" ; le tableau complet est DÉJÀ affiché par SeldonH : ne le recopie pas, commente-le")
                                    : QString(),
                         unit.isEmpty() ? QStringLiteral("unité du fichier") : unit);
    if (isAgeScale(unit))
        s += QStringLiteral("Échelle d'âges : la plus grande valeur est la plus ancienne (le début d'une phase a une valeur plus grande que sa fin).\n");

    if ((focus & FocusPhases) && !r.phases.isEmpty()) {
        // Remarques calculées : à reprendre, ce sont les points d'interprétation importants.
        QStringList single, bound, multi, zero;
        for (const Phase& p : r.phases) {
            const QString n = QStringLiteral("« %1 »").arg(p.name.trimmed());
            if (p.events == 1) single << n;
            double w = 0;
            if (atBound(p.begin, &w)) bound << QStringLiteral("%1 (début, %2)").arg(n, fmt(w));
            if (atBound(p.end, &w)) bound << QStringLiteral("%1 (fin, %2)").arg(n, fmt(w));
            if (hpdParts(p.begin).size() > 1 || hpdParts(p.end).size() > 1) multi << n;
            if (p.events > 1 && !p.duration.hpd.isEmpty() && p.duration.hpd.first().a <= 0.0) zero << n;
        }
        s += QStringLiteral("%1 phases. Remarques (calculées par SeldonH) :\n").arg(r.phases.size());
        if (!single.isEmpty())
            s += QStringLiteral("- %1 phase(s) à un seul Event : début = fin = date de l'Event, durée nulle par construction (ce n'est pas une "
                                "phase « brève » mesurée) : %2\n").arg(single.size()).arg(single.join(", "));
        if (!bound.isEmpty())
            s += QStringLiteral("- Début ou fin collé à une limite (probablement la période d'étude) : non contraint par les données de ce côté, "
                                "la valeur dépend de la période d'étude choisie : %1\n").arg(bound.join(", "));
        if (!multi.isEmpty())
            s += QStringLiteral("- Début ou fin dont la HPD est en plusieurs morceaux (multimodale) : %1\n").arg(multi.join(", "));
        if (!zero.isEmpty())
            s += QStringLiteral("- Durée dont la HPD part de 0 (phase possiblement très courte, durée mal contrainte vers le bas) : %1\n")
                     .arg(zero.join(", "));
    }
    if ((focus & (FocusPhases | FocusSuccessions)) && !r.successions.isEmpty()) {
        QStringList gaps;
        for (const Succession& sc : r.successions) if (sc.gap) gaps << QStringLiteral("« %1 » → « %2 » %3").arg(sc.from, sc.to, interval(sc.gapRange));
        s += QStringLiteral("%1 successions ; gap (hiatus démontré au niveau indiqué) pour %2 d'entre elles%3.\n")
                 .arg(r.successions.size()).arg(gaps.size()).arg(gaps.isEmpty() ? QString() : QStringLiteral(" : ") + gaps.join(", "));
    }

    // Lignes détaillées : d'abord ce que la question nomme, puis le reste dans l'ordre chronologique, dans le budget.
    QStringList first, rest;
    if (focus & FocusPhases)
        for (const Phase* p : chronological(r)) (mentioned(q, p->name) ? first : rest) << phaseLine(*p);
    if (focus & (FocusPhases | FocusSuccessions))
        for (const Succession& sc : r.successions) ((mentioned(q, sc.from) || mentioned(q, sc.to)) ? first : rest) << successionLine(sc);
    if (focus & FocusEvents)
        for (const Variable* v : eventDates(r)) (mentioned(q, v->event) ? first : rest) << eventLine(*v);
    const int total = int(first.size() + rest.size());
    int shown = 0;
    s += QStringLiteral("Détail :\n");
    for (const QString& l : first + rest) {
        if (s.size() + l.size() > maxChars && shown > 0) break;
        s += l;
        ++shown;
    }
    if (shown < total)
        s += QStringLiteral("… %1 ligne(s) sur %2 non transmises faute de place%3 : ne les invente pas ; propose de demander une phase ou un "
                            "Event par son nom.\n").arg(total - shown).arg(total)
                 .arg(tableShown ? QStringLiteral(" (elles sont dans le tableau affiché)") : QString());
    return s;
}

} // namespace chronoresults
} // namespace seldon
