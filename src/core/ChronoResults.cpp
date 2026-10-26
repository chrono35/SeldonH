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
    static const QRegularExpression mapRe(QStringLiteral(R"(^\s*MAP\s*=\s*([-0-9.,]+)\s*;\s*Mean\s*=\s*[-0-9.,]+\s*;\s*Std\s*=\s*([0-9.,]+))"));
    static const QRegularExpression thrRe(QStringLiteral(R"(\(<\s*([0-9]+)\))"));
    static const QRegularExpression headerRe(QStringLiteral(
        R"(ChronoModel\s+\d|Project filename|Elapsed|onverged|^\s*R̂\s*:|^\s*ESS\s*:)"));

    QString event, data;
    Variable* cur = nullptr;
    bool inBody = false;
    for (const QString& raw : lines) {
        const QString l = raw.trimmed();
        if (l.isEmpty()) continue;
        if (!inBody && R.header.size() < 12 && headerRe.match(raw).hasMatch()) { R.header << l; continue; }
        if (const auto m = eventRe.match(raw); m.hasMatch()) { inBody = true; event = m.captured(1); data.clear(); cur = nullptr; ++R.events; continue; }
        if (const auto m = dataRe.match(raw); m.hasMatch()) { inBody = true; data = m.captured(1); cur = nullptr; ++R.data; continue; }
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
            cur = &R.vars.last();
            continue;
        }
        if (!cur) continue;
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
        if (const auto m = mapRe.match(l); m.hasMatch() && !cur->hasStats) {
            cur->map = num(m.captured(1));
            cur->stdev = num(m.captured(2));
            cur->hasStats = true;
        }
    }
    // Une ligne unique « R̂ · ESS bulk · ESS tail ⚠ » : on attribue l'échec d'après les seuils.
    for (Variable& v : R.vars) {
        if (!v.rhatOk && v.bulkOk && v.tailOk && v.rhat > 0 && v.rhat < 1.01 && (v.essBulk < R.essThreshold || v.essTail < R.essThreshold)) {
            v.rhatOk = true;
            v.bulkOk = v.essBulk >= R.essThreshold;
            v.tailOk = v.essTail >= R.essThreshold;
        }
    }
    return R;
}

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

} // namespace chronoresults
} // namespace seldon
