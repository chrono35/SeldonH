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

#include "AmMethod.h"

#include <algorithm>
#include <cmath>

#include "../common/CurveStore.h"
#include "../common/TextUtil.h"

namespace seldon {

namespace {

const long double kRad = 3.14159265358979323846264338327950288L / 180.0L;

struct Req {
    bool I = false, D = false, F = false;
};

Req needs(const QString& process)
{
    Req r;
    r.I = process.contains('I');
    r.D = process.contains('D');
    r.F = process.contains('F');
    return r;
}

QString defaultCurve(char kind, const QString& foldedQuestion)
{
    const bool bg = foldedQuestion.contains("bulgar");
    if (kind == 'I') return bg ? "bulgaria_i" : "gal2002sph2014_i";
    if (kind == 'D') return bg ? "bulgaria_d" : "gal2002sph2014_d";
    return bg ? "bulgaria_f" : "gwh2013uni_f";
}

// Courbes nommées dans la question (« courbe pour l'inclinaison gal2002sph2014_i, pour l'intensité eurw_2017_f »).
// 1) Un nom terminé par _i, _d ou _f donne sa composante, même si la courbe n'est pas installée (le calcul le dira).
// 2) Un nom de courbe connu sans ce suffixe : composante du mot le plus proche qui le précède (inclinaison,
//    déclinaison, intensité).
void namedCurves(const QString& q, QString& cI, QString& cD, QString& cF)
{
    static const QRegularExpression tok(R"((?<![a-z0-9_])([a-z][a-z0-9]*(?:[_-][a-z0-9]+)*_([idf]))(?![a-z0-9_]))");
    auto it = tok.globalMatch(q);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString n = m.captured(1);
        QString& dst = m.captured(2) == QLatin1String("i") ? cI : m.captured(2) == QLatin1String("d") ? cD : cF;
        if (dst.isEmpty()) dst = n;
    }
    static const QRegularExpression suffix(R"(_[idf]$)");
    static const QRegularExpression role(R"((inclin)|(declin)|(intensit|champ|field|paleointensit))");
    QString rest = q;
    for (int guard = 0; guard < 6; ++guard) {
        int pos = -1;
        const QString n = CurveStore::findNamed(QStringLiteral("am"), rest, &pos);
        if (n.isEmpty()) break;
        rest.replace(pos, n.size(), QString(n.size(), QLatin1Char(' ')));       // pour trouver la suivante
        if (suffix.match(n).hasMatch()) continue;                               // déjà traitée en 1)
        int best = -1; char kind = 0;
        auto rit = role.globalMatch(q.left(pos));
        while (rit.hasNext()) {
            const auto r = rit.next();
            if (r.capturedStart() > best) { best = int(r.capturedStart()); kind = !r.captured(1).isEmpty() ? 'I' : !r.captured(2).isEmpty() ? 'D' : 'F'; }
        }
        QString& dst = kind == 'I' ? cI : kind == 'D' ? cD : cF;
        if (kind && dst.isEmpty()) dst = n;
    }
}

} // namespace

QStringList AmMethod::curveNames() const { return CurveStore::names(id()); }

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression AmMethod::context() const
{
    static const QRegularExpression re(R"((archeomag|archaeomag|paleomag|palaeomag|inclinaison|declinaison|inclination|declination|alpha|α|\ba95\b))");
    return re;
}

QVector<QJsonObject> AmMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);
    const QRegularExpression ctx = context();
    if (!ctx.match(q).hasMatch()) return out;

    auto grab = [&](const QString& pattern, double& v) {
        const QRegularExpression re(pattern + QStringLiteral("\\s*(?:de|=|:|a|egale|of|is)?\\s*(-?\\d+(?:[.,]\\d+)?)"));
        const auto m = re.match(q);
        if (!m.hasMatch()) return false;
        v = text::toNumber(m.captured(1));
        return true;
    };

    double inc = 0, dec = 0, a95 = 0, field = 0, errF = 0;
    const bool hasI = grab(R"((?:inclinaison|inclination|\binc\b|\bi\b(?=\s*[=:])))", inc);
    const bool hasD = grab(R"((?:declinaison|declination|\bdec\b|\bd\b(?=\s*[=:])))", dec);
    const bool hasA = grab(R"((?:(?:alpha|α)(?:\s*_?95)?|\ba95\b))", a95);
    const bool hasF = grab(R"((?:intensite|intensity|field\s+strength|\bf\b(?=\s*[=:])))", field);
    bool hasE = grab(R"((?:erreur\s*(?:sur\s*)?(?:f|l'intensite|intensite)|error\s*(?:on\s*)?(?:the\s*)?(?:f|intensity)|sigma\s*f|err(?:eur)?\s*f))", errF);
    // « intensité = 56,89 erreur = 0,74 » : une erreur sans précision écrite APRÈS l'intensité est celle de F
    // (l'incertitude des directions est α95).
    if (hasF && !hasE) {
        static const QRegularExpression fRe(R"((?:intensite|intensity|field\s+strength|\bf\b(?=\s*[=:])))");
        static const QRegularExpression eRe(
            R"((?:\berreur\b|\berror\b|\berr\b|\bincertitude\b|\buncertainty\b|\bsigma\b|σ)\s*(?:de|=|:|of|is)?\s*(\d+(?:[.,]\d+)?))");
        const auto fm = fRe.match(q);
        if (fm.hasMatch()) {
            const auto em = eRe.match(q, fm.capturedEnd());
            if (em.hasMatch()) { errF = text::toNumber(em.captured(1)); hasE = true; }
        }
    }
    // « intensité 52 ± 3 µT » : erreur donnée par ±
    if (hasF && !hasE) {
        for (const text::ValueError& ve : text::findValueErrors(q)) {
            if (std::abs(ve.value - field) < 1e-9) { errF = ve.error; hasE = true; break; }
        }
    }

    QString process;
    if (hasI) process += 'I';
    if (hasD) process += 'D';
    if (hasF) process += 'F';
    if (process.isEmpty() || (hasD && !hasI)) return out;        // D seul n'est pas calculable (σ_D dépend de I)

    QJsonObject o;
    o["process"] = process;
    if (hasI) o["inc"] = inc;
    if (hasD) o["dec"] = dec;
    if (hasA) o["alpha95"] = a95;
    if (hasF) { o["field"] = field; o["error_f"] = hasE ? errF : 0.0; }
    o["integration_steps"] = 500;
    QString nI, nD, nF;
    namedCurves(q, nI, nD, nF);
    // Courbes non nommées : défaut régional (« Bulgarie »), sans compter les noms de courbes (« bulgaria_f » ne
    // désigne que la courbe d'intensité).
    QString region = q;
    region.remove(QRegularExpression(QStringLiteral(R"((?<![a-z0-9_])[a-z][a-z0-9]*(?:[_-][a-z0-9]+)*_[idf](?![a-z0-9_]))")));
    if (hasI) o["curve_i"] = nI.isEmpty() ? defaultCurve('I', region) : nI;
    if (hasD) o["curve_d"] = nD.isEmpty() ? defaultCurve('D', region) : nD;
    if (hasF) o["curve_f"] = nF.isEmpty() ? defaultCurve('F', region) : nF;
    out << o;
    return out;
}

MethodResult AmMethod::compute(const QJsonObject& p) const
{
    const QString process = p.value("process").toString();
    const Req need = needs(process);
    if (process != "I" && process != "D" && process != "F" && process != "ID" && process != "IF" && process != "IDF")
        return MethodResult::failure(QStringLiteral("combinaison « %1 » non prise en charge (I, D, F, ID, IF ou IDF)").arg(process));

    const long double incl = p.value("inc").toDouble(), decl = p.value("dec").toDouble();
    const long double a95 = p.value("alpha95").toDouble();
    const long double field = p.value("field").toDouble(), errF = p.value("error_f").toDouble();
    const int steps = std::max(1, p.value("integration_steps").toInt(500));

    if ((need.I || need.D) && !(a95 > 0.0L)) return MethodResult::failure("α95 (strictement positif) est nécessaire");
    if (need.F && !(errF > 0.0L)) return MethodResult::failure("l'erreur sur l'intensité (strictement positive) est nécessaire");

    const QString qnone;       // pas de texte de question ici : les défauts viennent de l'appelant (JSON)
    const QString nI = p.value("curve_i").toString(defaultCurve('I', qnone)).toLower();
    const QString nD = p.value("curve_d").toString(defaultCurve('D', qnone)).toLower();
    const QString nF = p.value("curve_f").toString(defaultCurve('F', qnone)).toLower();

    const RefCurve *cI = nullptr, *cD = nullptr, *cF = nullptr;
    auto load = [&](const QString& n, const RefCurve*& dst, const char* what) -> QString {
        dst = CurveStore::get(id(), n, CurveFormat::Simple);
        return dst ? QString() : QStringLiteral("courbe %1 inconnue : « %2 » (disponibles : %3)").arg(what, n, curveNames().join(", "));
    };
    if (need.I) { const QString e = load(nI, cI, "d'inclinaison"); if (!e.isEmpty()) return MethodResult::failure(e); }
    if (need.D) { const QString e = load(nD, cD, "de déclinaison"); if (!e.isEmpty()) return MethodResult::failure(e); }
    if (need.F) { const QString e = load(nF, cF, "d'intensité"); if (!e.isEmpty()) return MethodResult::failure(e); }

    double tmin = -1e300, tmax = 1e300;
    for (const RefCurve* c : { cI, cD, cF }) {
        if (!c) continue;
        tmin = std::max(tmin, c->tmin());
        tmax = std::min(tmax, c->tmax());
    }
    if (!(tmax > tmin)) return MethodResult::failure("les courbes choisies n'ont pas de période commune");

    const long double errI = a95 / 2.448L;
    const long double errD = a95 / (2.448L * cosl(incl * kRad));
    const long double rrD = powl(cosl(incl * kRad), 2.0L);

    const auto L = [&](double t) -> long double {
        double mI = 0, eI = 0, mD = 0, eD = 0, mF = 0, eF = 0;
        if (cI) cI->at(t, mI, eI);
        if (cD) cD->at(t, mD, eD);
        if (cF) cF->at(t, mF, eF);

        long double I = 0, vI = 1, D = 0, vD = 1, F = 0, vF = 1;
        if (need.I) { vI = errI * errI + static_cast<long double>(eI) * eI; I = -0.5L * powl(incl - mI, 2.0L); }
        if (need.D) { vD = errD * errD + static_cast<long double>(eD) * eD; D = -0.5L * powl(decl - mD, 2.0L); }
        if (need.F) { vF = errF * errF + static_cast<long double>(eF) * eF; F = -0.5L * powl(field - mF, 2.0L); }

        if (process == "I") return expl(I / vI) / sqrtl(vI);
        if (process == "D") return expl(D / vD) / sqrtl(vD);
        if (process == "F") return expl(F / vF) / sqrtl(vF);

        long double sum = 0.0L, w = 0.0L;
        if (process == "ID") {
            for (int i = 0; i < steps; ++i) {
                w += 1.0L / steps;
                const long double si = errI * errI * ((1.0L - w) / w);
                sum += expl(I / (vI + si) + D / (vD + si / rrD)) / sqrtl((vI + si) * (vD + si / rrD));
            }
        } else if (process == "IF") {
            const long double si0 = 0.5L * (errF * errF + powl(errI * field * kRad, 2.0L));
            for (int i = 0; i < steps; ++i) {
                w += 1.0L / steps;
                const long double si = si0 * ((1.0L - w) / w);
                sum += expl(I / (vI + si / (field * field)) + F / (vF + si)) / sqrtl((vI + si / (field * field)) * (vF + si));
            }
        } else {                                  // IDF
            const long double rr = powl(field * cosl(incl * kRad), 2.0L);
            const long double si0 = (errF * errF + 2.0L * powl(errI * field * kRad, 2.0L)) / 3.0L;
            for (int i = 0; i < steps; ++i) {
                w += 1.0L / steps;
                const long double si = si0 * ((1.0L - w) / w);
                sum += expl(I / (vI + si / (field * field)) + D / (vD + si / rr) + F / (vF + si))
                       / sqrtl((vI + si / (field * field)) * (vF + si) * (vD + si / rr));
            }
        }
        return sum;
    };

    const Density d = buildDensity(tmin, tmax, 1.0, L, !p.value("untruncated").toBool());   // « untruncated » : combinaison
    return MethodResult::fromDensity(d, {}, true);
}

QStringList AmMethod::referenceCurves(const QJsonObject& p) const
{
    const QString proc = p.value("process").toString().toUpper();
    const QString qnone;
    QStringList out;
    if (proc.contains('I')) out << QStringLiteral("I|am|") + p.value("curve_i").toString(defaultCurve('I', qnone)).toLower();
    if (proc.contains('D')) out << QStringLiteral("D|am|") + p.value("curve_d").toString(defaultCurve('D', qnone)).toLower();
    if (proc.contains('F')) out << QStringLiteral("F|am|") + p.value("curve_f").toString(defaultCurve('F', qnone)).toLower();
    return out;
}

QString AmMethod::describe(const QJsonObject& p) const
{
    auto n = [](double v) { return text::numAuto(v, 2); };
    QStringList parts;
    const QString proc = p.value("process").toString();
    if (proc.contains('I')) parts << QStringLiteral("I = %1°").arg(n(p.value("inc").toDouble()));
    if (proc.contains('D')) parts << QStringLiteral("D = %1°").arg(n(p.value("dec").toDouble()));
    if (proc.contains('F')) parts << QStringLiteral("F = %1 ± %2 µT").arg(n(p.value("field").toDouble()), n(p.value("error_f").toDouble()));
    if (proc.contains('I') || proc.contains('D')) parts << QStringLiteral("α95 = %1°").arg(n(p.value("alpha95").toDouble()));
    return QStringLiteral("Archéomagnétisme (%1) : %2").arg(proc, parts.join(", "));
}

} // namespace seldon
