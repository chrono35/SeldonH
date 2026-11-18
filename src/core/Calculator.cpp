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

#include "Calculator.h"

#include <QRegularExpression>
#include <cmath>
#include <limits>

#include "../methods/common/DateFormat.h"
#include "../methods/common/FuzzyWords.h"
#include "../methods/common/RefCurve.h"

namespace seldon {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr double kEarthR = 6.3712e6;          // rayon terrestre de référence (m)

// Angle écrit en degrés, minutes, secondes (16/11/2026, prompt réel « convertie de degres decimal W 00° 05' 52'' »,
// parti au modèle de langage sans outil) : « 48° 06′ 51″ N », « W 00° 05' 52'' », « 1°30' E », « 12,5° S » → dms(d;m;s),
// avec un signe -1 pour le sud et l'ouest (S, W, O) en 4e argument : « W 00° 05′ » garde son signe, ce que dms(-0;5)
// ne pourrait pas faire. Un « - » écrit devant l'angle reste l'opérateur moins.
QString angleNotation(const QString& text)
{
    if (!text.contains(QChar(0x00B0))) return text;
    static const QString N = QStringLiteral(R"((\d+(?:[.,]\d+)?))");
    static const QRegularExpression re(
        QStringLiteral(R"((?:(?<![A-Za-z0-9_])([NSEWO])\s*)?(?<![0-9_.,]))") + N
            + QStringLiteral(R"(\s*°\s*(?:)") + N + QStringLiteral(R"(\s*(?:′|'(?!')|’(?!’)|min\b)\s*)?(?:)") + N
            + QStringLiteral(R"(\s*(?:″|''|’’|"|sec\b)\s*)?(?:([NSEWO])(?![A-Za-z0-9_]))?)"),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        auto n = [&](int k) { QString v = m.captured(k); v.replace(QLatin1Char(','), QLatin1Char('.')); return v.isEmpty() ? QStringLiteral("0") : v; };
        const QString h = (m.captured(1) + m.captured(5)).toUpper();
        const bool neg = h.contains(QLatin1Char('S')) || h.contains(QLatin1Char('W')) || h.contains(QLatin1Char('O'));
        // Espace final rendu : « 45° 30' + 1 » garde son opérateur.
        const QString tailSpace = m.captured(0).endsWith(QLatin1Char(' ')) ? QStringLiteral(" ") : QString();
        out += text.mid(last, m.capturedStart() - last)
               + QStringLiteral("dms(%1;%2;%3;%4)").arg(n(2), n(3), n(4), neg ? QStringLiteral("-1") : QStringLiteral("1")) + tailSpace;
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

// Longitude ramenée dans ]-180 ; 180].
double wrap180(double a)
{
    a = std::fmod(a, 360.0);
    if (a <= -180.0) a += 360.0;
    if (a > 180.0) a -= 360.0;
    return a;
}

// Moment dipolaire (10²² A·m²) d'un champ F (µT) à la latitude (géographique pour le VADM, magnétique pour le VDM) :
// m = 4π R³ F / (µ0 √(1 + 3 sin² λ)).
double dipoleMoment(double fMicroT, double latDeg)
{
    const double s = std::sin(latDeg * kDeg);
    return 4.0 * M_PI * std::pow(kEarthR, 3) * fMicroT * 1e-6 / (4.0 * M_PI * 1e-7 * std::sqrt(1.0 + 3.0 * s * s)) / 1e22;
}

double roundTo(double v, int decimals) { const double k = std::pow(10.0, decimals); return std::round(v * k) / k; }

// Fonction gamma incomplète régularisée supérieure Q(a, x) (Numerical Recipes : série ou fraction continue).
double gammaQ(double a, double x)
{
    if (x <= 0) return 1.0;
    const double gln = std::lgamma(a);
    if (x < a + 1) {
        double ap = a, sum = 1.0 / a, del = sum;
        for (int n = 0; n < 500; ++n) { ap += 1; del *= x / ap; sum += del; if (std::abs(del) < std::abs(sum) * 1e-15) break; }
        return 1.0 - sum * std::exp(-x + a * std::log(x) - gln);
    }
    double b = x + 1 - a, c = 1e300, d = 1.0 / b, h = d;
    for (int i = 1; i < 500; ++i) {
        const double an = -i * (i - a);
        b += 2;
        d = an * d + b; if (std::abs(d) < 1e-300) d = 1e-300;
        c = b + an / c; if (std::abs(c) < 1e-300) c = 1e-300;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) < 1e-15) break;
    }
    return std::exp(-x + a * std::log(x) - gln) * h;
}

// P(χ²_df > T) et quantile du χ² (bissection).
double chi2Upper(double T, int df) { return gammaQ(df / 2.0, T / 2.0); }
double chi2Quantile(double prob, int df)
{
    double lo = 0.0, hi = std::max(10.0, 10.0 * df);
    while (chi2Upper(hi, df) > 1 - prob) hi *= 2;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (1.0 - chi2Upper(mid, df) < prob) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

double gadInclination(double latDeg) { return std::atan(2.0 * std::tan(latDeg * kDeg)) / kDeg; }

// Direction (D, I) en un site à partir d'un pôle (lat, lon), en degrés.
void poleToDirection(double latP, double lonP, double latS, double lonS, double& D, double& I)
{
    const double lp = latP * kDeg, ls = latS * kDeg, dl = (lonP - lonS) * kDeg;
    const double cp = std::sin(ls) * std::sin(lp) + std::cos(ls) * std::cos(lp) * std::cos(dl);
    const double p = std::acos(std::clamp(cp, -1.0, 1.0));
    I = std::atan2(2.0 * std::cos(p), std::sin(p)) / kDeg;
    const double sd = std::cos(lp) * std::sin(dl) / std::sin(p);
    const double cd = (std::sin(lp) - std::sin(ls) * std::cos(p)) / (std::cos(ls) * std::sin(p));
    D = wrap180(std::atan2(sd, cd) / kDeg);
}

// Pôle géomagnétique virtuel d'une direction (D, I) mesurée au site (lat, lon) ; p = distance polaire (degrés).
void directionToPole(double latS, double lonS, double D, double I, double& latP, double& lonP, double& p)
{
    const double ls = latS * kDeg, d = D * kDeg, i = I * kDeg;
    const double pr = std::atan2(2.0, std::tan(i));                       // cot p = tan I / 2
    const double lp = std::asin(std::clamp(std::sin(ls) * std::cos(pr) + std::cos(ls) * std::sin(pr) * std::cos(d), -1.0, 1.0));
    const double beta = std::asin(std::clamp(std::sin(pr) * std::sin(d) / std::cos(lp), -1.0, 1.0)) / kDeg;
    lonP = std::cos(pr) >= std::sin(ls) * std::sin(lp) ? lonS + beta : lonS + 180.0 - beta;
    lonP = wrap180(lonP);
    latP = lp / kDeg;
    p = pr / kDeg;
}

bool isIdentStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
bool isIdentChar(QChar c)  { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

const QStringList& reservedNames()
{
    static const QStringList r = Calculator::functionNames() + QStringList{ "pi", "e", "ans" };
    return r;
}

} // namespace

QStringList Calculator::functionNames()
{
    return { "sqrt", "racine", "exp", "ln", "log", "log10", "abs", "round", "arrondi", "floor", "ceil", "min", "max",
             "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinr", "cosr", "tanr", "rad", "deg",
             "adtobp", "bptoad", "adtob2k", "b2ktoad", "bptob2k", "b2ktobp", "adtoka", "katoad", "adtoma", "matoad",
             "adtobc", "bctoad", "c14tof14c", "f14ctoc14", "vgp", "vgptodi", "reducvgp", "igad", "reducgad",
             "vadm", "vdm", "vadmtof", "reducvadm", "val", "err", "somme", "sum", "moyenne", "mean", "ecart_type", "écart_type",
             "sd", "mediane", "médiane", "median", "moy_ponderee", "moy_pondérée", "wmean", "dms", "ddtodms" };
}

// Analyseur à descente récursive. Une erreur de syntaxe (ou un nom inconnu) dit que le texte n'est pas un calcul ;
// une erreur mathématique (division par zéro, racine d'un négatif) est un calcul dont le résultat est une erreur.
struct Calculator::Eval {
    QString s;
    int pos = 0;
    // Dans les parenthèses d'un appel de fonction, la virgule sépare les arguments… sauf si ceux-ci sont séparés par
    // « ; » : elle est alors décimale (moyenne(15,6; 12) = moyenne de 15,6 et 12 ; max(1,5) = max de 1 et 5).
    bool commaSeparates = false;

    // Les arguments de l'appel qui commence en `from` (juste après « ( ») contiennent-ils un « ; » à leur niveau ?
    bool semicolonArgs(int from) const
    {
        int depth = 0;
        for (int k = from; k < s.size(); ++k) {
            const QChar c = s[k];
            if (c == QLatin1Char('(')) ++depth;
            else if (c == QLatin1Char(')')) { if (depth == 0) return false; --depth; }
            else if (c == QLatin1Char(';') && depth == 0) return true;
        }
        return false;
    }
    const QHash<QString, Value>& vars;
    Value ans;
    bool hasAns = false;
    QString err;
    bool syntax = false;
    QVector<QPair<QString, double>> parts;        // fonction à plusieurs résultats
    QString details;

    Eval(const QString& text, const QHash<QString, Value>& v, Value a, bool ha) : s(text), vars(v), ans(a), hasAns(ha) {}

    void skip() { while (pos < s.size() && s[pos].isSpace()) ++pos; }
    QChar peek() { skip(); return pos < s.size() ? s[pos] : QChar(); }
    QChar at(int i) const { return i < s.size() ? s[i] : QChar(); }
    bool eat(QChar c) { if (peek() == c) { ++pos; return true; } return false; }
    double fail(const QString& m, bool syn)
    {
        if (err.isEmpty()) { err = m; syntax = syn; }
        return std::numeric_limits<double>::quiet_NaN();
    }
    Value failV(const QString& m, bool syn) { return Value{ fail(m, syn), 0.0 }; }

    // « ± », « +- » ou « +/- » à la position courante (espaces permis) : longueur du signe, 0 sinon.
    int pmLength()
    {
        skip();
        if (at(pos) == QChar(0x00B1)) return 1;
        if (at(pos) != QLatin1Char('+')) return 0;
        int k = pos + 1;
        while (k < s.size() && s[k].isSpace()) ++k;
        if (at(k) == QLatin1Char('-')) return k + 1 - pos;
        if (at(k) == QLatin1Char('/')) {
            ++k;
            while (k < s.size() && s[k].isSpace()) ++k;
            if (at(k) == QLatin1Char('-')) return k + 1 - pos;
        }
        return 0;
    }

    // Propagation au premier ordre, erreurs supposées indépendantes : σ² = Σ (∂f/∂xᵢ · σᵢ)².
    Value expr()
    {
        Value v = term();
        for (;;) {
            if (!err.isEmpty()) return v;
            if (const int n = pmLength()) {                       // valeur ± erreur
                pos += n;
                const Value e = term();
                v = Value{ v.v, std::hypot(v.e, std::abs(e.v)) };
            } else if (eat(QLatin1Char('+'))) {
                const Value b = term();
                v = Value{ v.v + b.v, std::hypot(v.e, b.e) };
            } else if (eat(QLatin1Char('-')) || eat(QChar(0x2212))) {
                const Value b = term();
                v = Value{ v.v - b.v, std::hypot(v.e, b.e) };
            } else return v;
        }
    }

    Value term()
    {
        Value v = unary();
        for (;;) {
            if (!err.isEmpty()) return v;
            const QChar c = peek();
            if ((c == QLatin1Char('*') && at(pos + 1) != QLatin1Char('*')) || c == QChar(0x00D7) || c == QChar(0x00B7)) {
                ++pos;
                const Value b = unary();
                v = Value{ v.v * b.v, std::hypot(b.v * v.e, v.v * b.e) };
            } else if (c == QLatin1Char('/') || c == QChar(0x00F7)) {
                ++pos;
                const Value d = unary();
                if (d.v == 0.0 && err.isEmpty()) return failV(QStringLiteral("division par zéro"), false);
                v = Value{ v.v / d.v, std::hypot(v.e / d.v, v.v * d.e / (d.v * d.v)) };
            } else return v;
        }
    }

    Value unary()
    {
        if (eat(QLatin1Char('-')) || eat(QChar(0x2212))) { const Value v = unary(); return Value{ -v.v, v.e }; }
        if (eat(QLatin1Char('+'))) return unary();
        return power();
    }

    Value power()
    {
        Value b = primary();
        if (!err.isEmpty()) return b;
        if (eat(QChar(0x00B2))) b = Value{ b.v * b.v, std::abs(2.0 * b.v) * b.e };          // x²
        if (peek() == QLatin1Char('^') || (peek() == QLatin1Char('*') && at(pos + 1) == QLatin1Char('*'))) {
            pos += s[pos] == QLatin1Char('^') ? 1 : 2;
            const Value e = unary();                                        // associatif à droite : 2^3^2 = 2^9
            if (!err.isEmpty()) return e;
            const double r = std::pow(b.v, e.v);
            if (std::isnan(r)) return failV(QStringLiteral("puissance non définie (%1 ^ %2)").arg(format(b.v), format(e.v)), false);
            double sb = b.e > 0 ? e.v * std::pow(b.v, e.v - 1.0) * b.e : 0.0;
            double se = (e.e > 0 && b.v > 0) ? r * std::log(b.v) * e.e : 0.0;
            return Value{ r, std::hypot(sb, se) };
        }
        return b;
    }

    double number()
    {
        const int start = pos;
        while (pos < s.size() && s[pos].isDigit()) ++pos;
        const QChar c = at(pos);
        if ((c == QLatin1Char('.') || (c == QLatin1Char(',') && !commaSeparates)) && at(pos + 1).isDigit()) {
            ++pos;
            while (pos < s.size() && s[pos].isDigit()) ++pos;
        }
        if ((at(pos) == QLatin1Char('e') || at(pos) == QLatin1Char('E'))
            && (at(pos + 1).isDigit() || ((at(pos + 1) == QLatin1Char('+') || at(pos + 1) == QLatin1Char('-')) && at(pos + 2).isDigit()))) {
            pos += 2;
            while (pos < s.size() && s[pos].isDigit()) ++pos;
        }
        QString t = s.mid(start, pos - start);
        t.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double v = t.toDouble(&ok);
        return ok ? v : fail(QStringLiteral("nombre illisible « %1 »").arg(t), true);
    }

    Value primary()
    {
        const QChar c = peek();
        if (c.isNull()) return failV(QStringLiteral("expression incomplète"), true);
        if (eat(QLatin1Char('('))) {
            const bool saved = commaSeparates;
            commaSeparates = false;                                          // (1,5) : virgule décimale
            const Value v = expr();
            commaSeparates = saved;
            if (!err.isEmpty()) return v;
            if (!eat(QLatin1Char(')'))) return failV(QStringLiteral("parenthèse fermante manquante"), true);
            return v;
        }
        if (c.isDigit() || (c == QLatin1Char('.') && at(pos + 1).isDigit())) return Value{ number(), 0.0 };
        if (c == QChar(0x03C0)) { ++pos; return Value{ M_PI, 0.0 }; }       // π
        if (!isIdentStart(c)) return failV(QStringLiteral("caractère inattendu « %1 »").arg(c), true);
        const int start = pos;
        while (pos < s.size() && isIdentChar(s[pos])) ++pos;
        const QString name = s.mid(start, pos - start);
        const QString low = name.toLower();
        if (peek() == QLatin1Char('(')) {
            ++pos;
            const bool saved = commaSeparates;
            // Fonction à un seul argument : « racine(12,5) » = racine de 12,5 (virgule décimale, prompt réel du 16/11/2026).
            static const QStringList oneArg{ "sqrt", "racine", "exp", "ln", "log", "log10", "abs", "floor", "ceil", "sin", "cos", "tan",
                                             "asin", "acos", "atan", "sinr", "cosr", "tanr", "rad", "deg", "adtobp", "bptoad", "adtob2k",
                                             "b2ktoad", "bptob2k", "b2ktobp", "adtoka", "katoad", "adtoma", "matoad", "adtobc", "bctoad",
                                             "igad", "val", "err", "ddtodms" };
            commaSeparates = !semicolonArgs(pos) && !oneArg.contains(low);
            QVector<Value> args;
            if (!eat(QLatin1Char(')'))) {
                for (;;) {
                    args << expr();
                    if (!err.isEmpty()) { commaSeparates = saved; return args.last(); }
                    if (eat(QLatin1Char(';')) || eat(QLatin1Char(','))) continue;     // « 15, 6 » (espace) : séparateur
                    if (eat(QLatin1Char(')'))) break;
                    commaSeparates = saved;
                    return failV(QStringLiteral("« ) » attendue après les arguments de %1").arg(name), true);
                }
            }
            commaSeparates = saved;
            return callV(low, name, args);
        }
        if (low == QLatin1String("pi")) return Value{ M_PI, 0.0 };
        if (low == QLatin1String("e")) return Value{ M_E, 0.0 };
        if (low == QLatin1String("ans")) return hasAns ? ans : failV(QStringLiteral("aucun résultat précédent (ans)"), false);
        if (vars.contains(low)) return vars.value(low);
        return failV(QStringLiteral("nom inconnu « %1 »").arg(name), true);
    }

    // Fonctions sur des listes de valeurs avec incertitude (statistiques), puis fonctions ordinaires : la valeur est
    // calculée par scalar(), l'incertitude propagée au premier ordre (dérivées numériques, erreurs indépendantes).
    Value callV(const QString& f, const QString& shown, const QVector<Value>& a)
    {
        QVector<double> x;
        for (const Value& v : a) x << v.v;
        const bool anyErr = std::any_of(a.begin(), a.end(), [](const Value& v) { return v.e > 0; });
        auto needAtLeast = [&](int n) {
            if (a.size() >= n) return true;
            fail(QStringLiteral("%1 attend au moins %2 valeur%3").arg(shown).arg(n).arg(n > 1 ? "s" : ""), true);
            return false;
        };
        if (f == "val") { if (a.size() != 1) return failV(QStringLiteral("val attend 1 argument"), true); return Value{ a[0].v, 0.0 }; }
        if (f == "err") { if (a.size() != 1) return failV(QStringLiteral("err attend 1 argument"), true); return Value{ a[0].e, 0.0 }; }
        if (f == "min" || f == "max") {
            if (!needAtLeast(1)) return Value{};
            Value r = a[0];
            for (const Value& v : a) if (f == "min" ? v.v < r.v : v.v > r.v) r = v;
            return r;
        }
        if (f == "floor" || f == "ceil" || f == "round" || f == "arrondi") {          // arrondi : l'incertitude est gardée
            const double r = scalar(f, shown, x);
            return Value{ r, a.isEmpty() ? 0.0 : a[0].e };
        }
        if (f == "somme" || f == "sum") {
            if (!needAtLeast(1)) return Value{};
            Value r{ 0.0, 0.0 };
            for (const Value& v : a) r = Value{ r.v + v.v, std::hypot(r.e, v.e) };
            return r;
        }
        if (f == "moyenne" || f == "mean") {
            if (!needAtLeast(1)) return Value{};
            double sm = 0, se = 0;
            for (const Value& v : a) { sm += v.v; se += v.e * v.e; }
            const int n = int(a.size());
            double sd = 0;
            for (const Value& v : a) sd += (v.v - sm / n) * (v.v - sm / n);
            sd = n > 1 ? std::sqrt(sd / (n - 1)) : 0.0;
            parts = { { "moyenne", sm / n }, { "n", double(n) }, { "sd", sd } };
            details = QStringLiteral("n = %1 ; écart-type des valeurs = %2 ; erreur de la moyenne (σ/√n) = %3")
                          .arg(n).arg(format(roundTo(sd, 6)), formatErr(n > 1 ? sd / std::sqrt(double(n)) : 0.0));
            if (anyErr) details += QStringLiteral(" ; ± propagé des erreurs = %1").arg(formatErr(std::sqrt(se) / n));
            return Value{ sm / n, std::sqrt(se) / n };
        }
        if (f == "ecart_type" || f == "écart_type" || f == "sd") {
            if (!needAtLeast(2)) return Value{};
            double m = 0;
            for (double v : x) m += v;
            m /= x.size();
            double s2 = 0;
            for (double v : x) s2 += (v - m) * (v - m);
            return Value{ std::sqrt(s2 / (x.size() - 1)), 0.0 };
        }
        if (f == "mediane" || f == "médiane" || f == "median") {
            if (!needAtLeast(1)) return Value{};
            QVector<double> y = x;
            std::sort(y.begin(), y.end());
            const int n = int(y.size());
            return Value{ n % 2 ? y[n / 2] : 0.5 * (y[n / 2 - 1] + y[n / 2]), 0.0 };
        }
        if (f == "moy_ponderee" || f == "moy_pondérée" || f == "wmean") {             // Ward et Wilson (1978)
            if (!needAtLeast(2)) return Value{};
            for (const Value& v : a)
                if (!(v.e > 0)) return failV(QStringLiteral("%1 : chaque valeur doit avoir une erreur > 0 (ex. 2500 ± 30)").arg(shown), false);
            double sw = 0, swx = 0;
            for (const Value& v : a) { const double w = 1.0 / (v.e * v.e); sw += w; swx += w * v.v; }
            const double m = swx / sw, sm = 1.0 / std::sqrt(sw);
            double T = 0;
            for (const Value& v : a) T += (v.v - m) * (v.v - m) / (v.e * v.e);
            const int df = int(a.size()) - 1;
            const double p = chi2Upper(T, df), crit = chi2Quantile(0.95, df);
            parts = { { "moyenne", m }, { "err", sm }, { "t", T }, { "ddl", double(df) }, { "p", p }, { "seuil", crit } };
            details = QStringLiteral("T = %1 ; χ² à %2 ddl, seuil à 5 % : %3 ; p %4 → %5")
                          .arg(format(roundTo(T, 3)), QString::number(df), format(roundTo(crit, 3)),
                               p < 1e-4 ? QStringLiteral("< 0,0001") : QStringLiteral("= ") + format(roundTo(p, 4)),
                               T < crit ? QStringLiteral("valeurs compatibles (test de Ward et Wilson)")
                                        : QStringLiteral("valeurs NON compatibles au seuil de 5 % : la moyenne pondérée n'a pas de sens"));
            return Value{ m, sm };
        }
        // Conversions qui ont un argument d'erreur : « C14toF14C(2500 ± 30) » = « C14toF14C(2500; 30) ».
        QVector<double> xs = x;
        if ((f == "c14tof14c" || f == "f14ctoc14") && a.size() == 1 && a[0].e > 0) xs = { a[0].v, a[0].e };
        else if ((f == "vadm" || f == "vdm") && a.size() == 2 && a[0].e > 0) xs = { a[0].v, a[0].e, a[1].v };
        const bool errInArgs = xs.size() != x.size();
        const auto outerParts = parts;                  // résultats multiples d'un appel imbriqué : gardés si celui-ci n'en a pas
        const QString outerDetails = details;
        parts.clear();
        details.clear();
        const double r = scalar(f, shown, xs);
        if (!err.isEmpty()) return Value{ r, 0.0 };
        const bool own = !parts.isEmpty();
        for (const auto& pr : std::as_const(parts))
            if (pr.first == QLatin1String("err")) return Value{ r, pr.second };   // erreur calculée par la fonction elle-même
        if (!own) { parts = outerParts; details = outerDetails; }
        if (!anyErr || errInArgs) return Value{ r, 0.0 };
        // Dérivées numériques (différences centrées) ; les détails de l'appel principal sont gardés.
        const auto savedParts = parts;
        const QString savedDetails = details;
        double var = 0.0;
        for (int i = 0; i < a.size(); ++i) {
            if (!(a[i].e > 0)) continue;
            const double h = std::max(std::abs(x[i]) * 1e-6, 1e-6);
            QVector<double> xp = x, xm = x;
            xp[i] += h; xm[i] -= h;
            double fp = scalar(f, shown, xp), fm = scalar(f, shown, xm), d;
            if (!err.isEmpty() || !std::isfinite(fp) || !std::isfinite(fm)) {    // bord du domaine : différence d'un côté
                err.clear(); syntax = false;
                fp = scalar(f, shown, xp);
                if (err.isEmpty() && std::isfinite(fp)) d = (fp - r) / h;
                else { err.clear(); syntax = false; fm = scalar(f, shown, xm); d = (r - fm) / h; }
                if (!err.isEmpty()) { err.clear(); syntax = false; d = 0.0; }
            } else d = (fp - fm) / (2 * h);
            var += d * d * a[i].e * a[i].e;
        }
        parts = savedParts;
        details = savedDetails;
        return Value{ r, std::sqrt(var) };
    }

    // Fonction sur des valeurs exactes (l'incertitude est propagée par callV).
    double scalar(const QString& f, const QString& shown, const QVector<double>& a)
    {
        auto need = [&](int n) {
            if (a.size() == n) return true;
            fail(QStringLiteral("%1 attend %2 argument%3").arg(shown).arg(n).arg(n > 1 ? "s" : ""), true);
            return false;
        };
        auto conv = [&](double v) { return v; };
        if (f == "min" || f == "max") {
            if (a.isEmpty()) return fail(QStringLiteral("%1 attend au moins un argument").arg(shown), true);
            double r = a[0];
            for (double x : a) r = f == "min" ? std::min(r, x) : std::max(r, x);
            return r;
        }
        if (f == "round" || f == "arrondi") {
            if (a.size() != 1 && a.size() != 2) return fail(QStringLiteral("%1 attend 1 ou 2 arguments").arg(shown), true);
            const double k = a.size() == 2 ? std::pow(10.0, std::round(a[1])) : 1.0;
            return std::round(a[0] * k) / k;
        }
        if (f == "atan2") { if (!need(2)) return 0; return std::atan2(a[0], a[1]) / kDeg; }
        auto deg = [](double v) { return format(v) + QStringLiteral("°"); };
        auto range = [&](int lo, int hi) {
            if (a.size() >= lo && a.size() <= hi) return true;
            fail(QStringLiteral("%1 attend %2 à %3 arguments").arg(shown).arg(lo).arg(hi), true);
            return false;
        };
        auto latOk = [&](double l, const char* what) {
            if (l >= -90 && l <= 90) return true;
            fail(QStringLiteral("%1 : %2 hors de [-90 ; 90]").arg(shown, QString::fromLatin1(what)), false);
            return false;
        };
        // ---- angles : degrés, minutes, secondes <-> degrés décimaux (signe porté par les degrés ; sud, ouest négatifs)
        auto dmsText = [](double x) {
            const double t = std::round(std::abs(x) * 3600.0 * 100.0) / 100.0;   // secondes au centième
            const double d = std::floor(t / 3600.0), mn = std::floor((t - d * 3600.0) / 60.0), sc = t - d * 3600.0 - mn * 60.0;
            QString s = format(std::round(sc * 100.0) / 100.0);
            if (sc < 10) s.prepend(QLatin1Char('0'));
            return QStringLiteral("%1%2° %3′ %4″").arg(x < 0 && t > 0 ? QStringLiteral("−") : QString(), format(d),
                                                     QStringLiteral("%1").arg(int(mn), 2, 10, QLatin1Char('0')), s);
        };
        if (f == "dms") {
            if (!range(1, 4)) return 0;
            const double mn = a.size() > 1 ? a[1] : 0.0, sc = a.size() > 2 ? a[2] : 0.0;
            // 4e argument : signe (-1 = sud ou ouest), mis par la notation écrite (« W 00° 05′ » : -0 n'existe pas).
            const double sg = a.size() > 3 ? (a[3] < 0 ? -1.0 : 1.0) : (a[0] < 0 ? -1.0 : 1.0);
            if (mn < 0 || mn >= 60 || sc < 0 || sc >= 60)
                return fail(QStringLiteral("%1 : minutes et secondes doivent être dans [0 ; 60[").arg(shown), false);
            const double r = sg * (std::abs(a[0]) + mn / 60.0 + sc / 3600.0);
            parts = { { "dd", r } };                   // garde les détails (voir callV)
            details = QStringLiteral("%1 = %2 en degrés décimaux").arg(dmsText(r), deg(r));
            return r;
        }
        if (f == "ddtodms") {
            if (!need(1)) return 0;
            const double t = std::round(std::abs(a[0]) * 3600.0 * 100.0) / 100.0;
            const double d = std::floor(t / 3600.0), mn = std::floor((t - d * 3600.0) / 60.0);
            parts = { { "dd", a[0] }, { "d", a[0] < 0 ? -d : d }, { "m", mn }, { "s", t - d * 3600.0 - mn * 60.0 } };
            details = QStringLiteral("%1 = %2").arg(deg(a[0]), dmsText(a[0]));
            return a[0];
        }
        // ---- radiocarbone : âge conventionnel <-> F14C (formules de ChronoModel, erreur dissymétrique ramenée au côté ancien)
        if (f == "c14tof14c") {
            if (!range(1, 2)) return 0;
            const double F = craToF14C(a[0]);
            parts = { { "f14c", F } };
            details = QStringLiteral("F14C = %1").arg(format(F));
            if (a.size() == 2) {
                const double e = errCraToErrF14C(a[1], a[0]);
                parts << qMakePair(QStringLiteral("err"), e);
                details += QStringLiteral(" ± %1").arg(format(e));
            }
            return F;
        }
        if (f == "f14ctoc14") {
            if (!range(1, 2)) return 0;
            if (a[0] <= 0) return fail(QStringLiteral("F14C doit être > 0"), false);
            const double age = f14cToCra(a[0]);
            parts = { { "age", age } };
            details = QStringLiteral("âge 14C = %1 BP").arg(format(age));
            if (a.size() == 2) {
                if (a[1] <= 0 || a[1] >= a[0]) return fail(QStringLiteral("erreur F14C : 0 < erreur < F14C"), false);
                const double e = errF14CToErrCra(a[1], a[0]);
                parts << qMakePair(QStringLiteral("err"), e);
                details = QStringLiteral("âge 14C = %1 ± %2 BP").arg(format(age), format(e));
            }
            return age;
        }
        // ---- archéomagnétisme
        if (f == "igad") { if (!need(1) || !latOk(a[0], "latitude")) return 0; return gadInclination(a[0]); }
        if (f == "reducgad") {                                      // I_ref = I + I_GAD(ref) − I_GAD(site)
            if (!need(3) || !latOk(a[1], "latitude du site") || !latOk(a[2], "latitude de référence")) return 0;
            const double r = a[0] + gadInclination(a[2]) - gadInclination(a[1]);
            details = QStringLiteral("I réduite = %1 (I GAD : %2 au site, %3 à la référence)").arg(deg(r), deg(gadInclination(a[1])), deg(gadInclination(a[2])));
            parts = { { "i", r } };
            return r;
        }
        if (f == "vgp") {
            if (!range(4, 5) || !latOk(a[0], "latitude du site")) return 0;
            double lat = 0, lon = 0, p = 0;
            directionToPole(a[0], a[1], a[2], a[3], lat, lon, p);
            parts = { { "lat", lat }, { "lon", lon } };
            details = QStringLiteral("pôle : latitude %1, longitude %2").arg(deg(lat), deg(lon));
            if (a.size() == 5) {
                const double dp = 0.5 * a[4] * (1.0 + 3.0 * std::pow(std::cos(p * kDeg), 2));
                const double dm = a[4] * std::sin(p * kDeg) / std::cos(a[3] * kDeg);
                parts << qMakePair(QStringLiteral("dp"), dp) << qMakePair(QStringLiteral("dm"), dm);
                details += QStringLiteral(" ; dp = %1, dm = %2").arg(deg(dp), deg(dm));
            }
            return lat;
        }
        if (f == "vgptodi") {
            if (!need(4) || !latOk(a[0], "latitude du pôle") || !latOk(a[2], "latitude du site")) return 0;
            double D = 0, I = 0;
            poleToDirection(a[0], a[1], a[2], a[3], D, I);
            parts = { { "i", I }, { "d", D } };
            details = QStringLiteral("D = %1, I = %2").arg(deg(D), deg(I));
            return I;
        }
        if (f == "reducvgp") {                                      // site -> VGP -> site de référence
            if (!need(6) || !latOk(a[0], "latitude du site") || !latOk(a[4], "latitude de référence")) return 0;
            double lat = 0, lon = 0, p = 0, D = 0, I = 0;
            directionToPole(a[0], a[1], a[2], a[3], lat, lon, p);
            poleToDirection(lat, lon, a[4], a[5], D, I);
            parts = { { "i", I }, { "d", D } };
            details = QStringLiteral("D réduite = %1, I réduite = %2 (VGP : %3, %4)").arg(deg(D), deg(I), deg(lat), deg(lon));
            return I;
        }
        if (f == "vadm" || f == "vdm") {                             // (F; λ) ou (F; σF; λ) ; VDM : λ magnétique d'après I
            if (!range(2, 3)) return 0;
            const double F = a[0], x = a.last();
            if (f == "vadm" && !latOk(x, "latitude")) return 0;
            const double lat = f == "vadm" ? x : std::atan(std::tan(x * kDeg) / 2.0) / kDeg;
            const double m = dipoleMoment(F, lat);
            parts = { { "m", m } };
            details = QStringLiteral("%1 = %2 × 10²² A·m²").arg(f.toUpper(), format(m));
            if (a.size() == 3) {
                const double e = m * a[1] / F;
                parts << qMakePair(QStringLiteral("err"), e);
                details = QStringLiteral("%1 = %2 ± %3 × 10²² A·m²").arg(f.toUpper(), format(m), format(e));
            }
            if (f == "vdm") details += QStringLiteral(" (latitude magnétique %1)").arg(deg(lat));
            return m;
        }
        if (f == "vadmtof") {
            if (!need(2) || !latOk(a[1], "latitude")) return 0;
            return a[0] / dipoleMoment(1.0, a[1]);                  // F (µT) donnant ce VADM à cette latitude
        }
        if (f == "reducvadm") {                                     // F_ref = F √(1+3 sin² λref) / √(1+3 sin² λ)
            if (!need(3) || !latOk(a[1], "latitude du site") || !latOk(a[2], "latitude de référence")) return 0;
            const double r = a[0] * dipoleMoment(1.0, a[1]) / dipoleMoment(1.0, a[2]);
            details = QStringLiteral("F réduite = %1 µT (même VADM : %2 × 10²² A·m²)").arg(format(r), format(dipoleMoment(a[0], a[1])));
            parts = { { "f", r } };
            return r;
        }
        if (!functionNames().contains(f)) return fail(QStringLiteral("fonction inconnue « %1 »").arg(shown), true);
        if (!need(1)) return 0;
        const double x = a[0];
        if (f == "sqrt" || f == "racine") return x < 0 ? fail(QStringLiteral("racine d'un nombre négatif"), false) : std::sqrt(x);
        if (f == "exp")   return std::exp(x);
        if (f == "ln")    return x <= 0 ? fail(QStringLiteral("logarithme d'un nombre ≤ 0"), false) : std::log(x);
        if (f == "log" || f == "log10") return x <= 0 ? fail(QStringLiteral("logarithme d'un nombre ≤ 0"), false) : std::log10(x);
        if (f == "abs")   return std::abs(x);
        if (f == "floor") return std::floor(x);
        if (f == "ceil")  return std::ceil(x);
        if (f == "sin")   return std::sin(x * kDeg);
        if (f == "cos")   return std::cos(x * kDeg);
        if (f == "tan")   return std::tan(x * kDeg);
        if (f == "asin" || f == "acos") {
            if (x < -1 || x > 1) return fail(QStringLiteral("%1 : argument hors de [-1 ; 1]").arg(shown), false);
            return (f == "asin" ? std::asin(x) : std::acos(x)) / kDeg;
        }
        if (f == "atan")  return std::atan(x) / kDeg;
        if (f == "sinr")  return std::sin(x);
        if (f == "cosr")  return std::cos(x);
        if (f == "tanr")  return std::tan(x);
        if (f == "rad")   { parts = { { "rad", x * kDeg } }; details = QStringLiteral("%1° = %2 rad").arg(format(x), format(x * kDeg)); return x * kDeg; }
        if (f == "deg")   { parts = { { "deg", x / kDeg } }; details = QStringLiteral("%1 rad = %2°").arg(format(x), format(x / kDeg)); return x / kDeg; }
        // Dates : AD = année calendaire signée (BC/AD de ChronoModel) ; mêmes formules que les formats d'affichage.
        if (f == "adtobp")  return conv(dateformat::convert(x, DateFormat::CalBP));
        if (f == "bptoad")  return dateformat::toCalendar(x, DateFormat::CalBP);
        if (f == "adtob2k") return dateformat::convert(x, DateFormat::CalB2K);
        if (f == "b2ktoad") return dateformat::toCalendar(x, DateFormat::CalB2K);
        if (f == "bptob2k") return x + 50.0;
        if (f == "b2ktobp") return x - 50.0;
        if (f == "adtoka")  return dateformat::convert(x, DateFormat::Ka);
        if (f == "katoad")  return dateformat::toCalendar(x, DateFormat::Ka);
        if (f == "adtoma")  return dateformat::convert(x, DateFormat::Ma);
        if (f == "matoad")  return dateformat::toCalendar(x, DateFormat::Ma);
        if (f == "adtobc")  return x >= 1 ? fail(QStringLiteral("ADtoBC : %1 est une année après J.-C.").arg(format(x)), false) : 1.0 - x;
        if (f == "bctoad")  return x < 1 ? fail(QStringLiteral("BCtoAD : l'année av. J.-C. doit être ≥ 1"), false) : 1.0 - x;
        return fail(QStringLiteral("fonction inconnue « %1 »").arg(shown), true);
    }
};

QString Calculator::format(double v)
{
    if (std::isnan(v)) return QStringLiteral("indéfini");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("∞") : QStringLiteral("−∞");
    if (v == 0.0) v = 0.0;                                     // pas de « -0 »
    QString t = QString::number(v, 'g', 12);
    t.replace(QLatin1Char('.'), QLatin1Char(','));
    return t;
}

QString Calculator::formatErr(double e)
{
    if (!(e > 0) || !std::isfinite(e)) return format(e);
    const int dec = std::max(0, 1 - int(std::floor(std::log10(e))));
    return format(roundTo(e, dec));
}

QString Calculator::formatValue(double v, double e)
{
    if (!(e > 0) || !std::isfinite(e)) return format(v);
    const int dec = std::max(0, 1 - int(std::floor(std::log10(e))));
    double rv = roundTo(v, dec);
    if (rv == 0.0) rv = 0.0;                                   // pas de « -0 »
    QString t = QString::number(rv, 'f', dec);
    t.replace(QLatin1Char('.'), QLatin1Char(','));
    QString te = QString::number(roundTo(e, dec), 'f', dec);
    te.replace(QLatin1Char('.'), QLatin1Char(','));
    return t + QStringLiteral(" ± ") + te;
}

QString Calculator::stripPrompt(const QString& text)
{
    QString t = text.trimmed();
    static const QRegularExpression prefix(QStringLiteral(
        R"(^(?:calcule[rz]?|calcul|combien\s+(?:font|fait|vaut)|que\s+vaut|quel\s+est\s+le\s+r[ée]sultat\s+de|[ée]value[rz]?|compute|calculate)\s*:?\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    t.remove(prefix);
    static const QRegularExpression suffix(QStringLiteral(R"(\s*[=?]+\s*$)"));
    t.remove(suffix);
    return t.trimmed();
}

QStringList Calculator::splitStatements(const QString& text)
{
    QStringList out;
    QString cur;
    int depth = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char('(')) ++depth;
        else if (c == QLatin1Char(')')) depth = std::max(0, depth - 1);
        if (c == QLatin1Char('\n') || (c == QLatin1Char(';') && depth == 0)) {
            if (!cur.trimmed().isEmpty()) out << cur.trimmed();
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.trimmed().isEmpty()) out << cur.trimmed();
    return out;
}

QVector<Calculator::Line> Calculator::evaluate(const QString& text)
{
    QVector<Line> out;
    static const QRegularExpression assign(QStringLiteral(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=)\s*)"));
    for (const QString& st : splitStatements(stripPrompt(text))) {
        Line l;
        l.expr = st;
        QString body = st;
        const auto m = assign.match(st);
        if (m.hasMatch()) {
            l.variable = m.captured(1).toLower();
            body = st.mid(m.capturedEnd());
            if (reservedNames().contains(l.variable)) {
                l.error = QStringLiteral("« %1 » est un nom réservé").arg(m.captured(1));
                l.syntax = true;
                out << l;
                continue;
            }
        }
        // Instruction qui commence par un opérateur (« +20 », « /3 », « * 2 », « - 5 ») : elle continue le résultat précédent,
        // comme une calculatrice de poche (prompt réel du 16/11/2026 : « +20 /2 » calculé 10 au lieu de ans + 20/2).
        // « -5 » collé reste un nombre négatif ; « - 5 » (espace) est une soustraction.
        static const QRegularExpression continues(QStringLiteral(R"(^\s*(?:[+*/^×÷]|[-−]\s))"));
        bool fromAns = false;
        if (hasAns_ && l.variable.isEmpty() && continues.match(body).hasMatch()) {
            body = QStringLiteral("ans ") + body.trimmed();
            l.expr = body;
            fromAns = true;
        }
        body = angleNotation(body);                  // 48° 06′ 51″ N → (dms(48;06;51))
        Eval e(body, vars_, ans_, hasAns_);
        const Value val = e.expr();
        const double v = val.v;
        if (e.err.isEmpty() && e.pos < body.size()) {
            e.skip();
            if (e.pos < body.size()) e.fail(QStringLiteral("caractère inattendu « %1 »").arg(body.mid(e.pos, 1)), true);
        }
        if (!e.err.isEmpty()) {
            l.error = e.err;
            l.syntax = e.syntax;
        } else if (!std::isfinite(v)) {
            l.error = QStringLiteral("résultat non fini");
        } else {
            l.ok = true;
            l.value = v;
            l.uncertainty = std::isfinite(val.e) ? val.e : 0.0;
            l.parts = e.parts;
            l.details = e.details;
            if (fromAns && l.details.isEmpty()) l.details = QStringLiteral("ans = %1").arg(formatValue(ans_.v, ans_.e));
            if (!l.variable.isEmpty()) {
                vars_[l.variable] = Value{ v, l.uncertainty };
                for (int k = 1; k < e.parts.size(); ++k)
                    vars_[l.variable + QLatin1Char('_') + e.parts[k].first] = Value{ e.parts[k].second, 0.0 };
            }
            ans_ = Value{ v, l.uncertainty };
            hasAns_ = true;
        }
        out << l;
    }
    return out;
}

QString Calculator::fromWords(const QString& text)
{
    QString t = text.trimmed().toLower().normalized(QString::NormalizationForm_D);
    t.remove(QRegularExpression(QStringLiteral("[\\x{0300}-\\x{036f}]")));
    t.replace(QChar(0x2019), QLatin1Char('\''));
    t.replace(QChar(0x2212), QLatin1Char('-'));
    if (t.isEmpty() || t.contains(QLatin1Char('\n'))) return QString();
    static const QRegularExpression polite(QStringLiteral(R"(\b(?:s'il (?:te|vous) pla[iî]t|stp|svp|merci|please)\b)"));
    t.remove(polite);
    static const QRegularExpression tail(QStringLiteral(R"([\s?!.=,;:]+$)"));
    t.remove(tail);
    t = t.simplified();

    // Mot de commande en tête, faute de frappe d'une lettre tolérée (« calcuk ») : retiré.
    {
        static const QStringList cmds{ "calcul", "calcule", "calculer", "calculez", "evalue", "evaluer", "compute", "calculate" };
        const qsizetype sp = t.indexOf(QLatin1Char(' '));
        const QString w = sp < 0 ? t : t.left(sp);
        if (w.size() >= 5)
            for (const QString& c : cmds)
                if (fuzzy::distance(w, c, 1) <= 1) { t = sp < 0 ? QString() : t.mid(sp + 1); break; }
    }
    static const QRegularExpression lead(QStringLiteral(
        R"(^(?:(?:combien\s+(?:font|fait|vaut)|que\s+vaut|quel(?:le)?\s+est|donne[sz]?(?:[\s-]moi)?|what\s+is)\s+)?(?:la\s+|le\s+|l'|the\s+)?)"));
    t.remove(lead);
    t = t.simplified();
    if (t.isEmpty()) return QString();

    // Degrés <-> radians (16/11/2026) : « convertis 45 degrés en radians », « 45° en radians », « 1,2 rad en degrés »,
    // « convertis en degrés π/4 radians », « 0,5 rad en degrés minutes secondes ». Verbe facultatif.
    {
        static const QRegularExpression convR(QStringLiteral(R"(^(?:conver\w*|transform\w*|passe[rz]?|exprime[rz]?|convert)\s+)"));
        static const QString radW = QStringLiteral(R"((?:radians?|rad))");
        static const QString degW = QStringLiteral(R"((?:degres?|degrees?|deg))");
        static const QString dmsW = QStringLiteral(R"((\s+minutes?[\s,-]+(?:et\s+|and\s+)?(?:secondes?|seconds?))?)");
        static const QString to = QStringLiteral(R"(\s+(?:en|to|into|in)\s+)");
        static const QRegularExpression toRad(QStringLiteral(R"(^(.+?)(?:\s*)") + degW + QStringLiteral(")?") + to + radW + QStringLiteral("$"));
        static const QRegularExpression toRadHead(QStringLiteral(R"(^(?:en|to|into|in)\s+)") + radW + QStringLiteral(R"(\s+(?:de\s+|d'|of\s+)?(.+?)(?:\s*)") + degW + QStringLiteral(")?$"));
        static const QRegularExpression toDeg(QStringLiteral(R"(^(.+?)\s*)") + radW + to + degW + QStringLiteral(R"((?:\s+decima\w*)?)") + dmsW + QStringLiteral("$"));
        static const QRegularExpression toDegHead(QStringLiteral(R"(^(?:en|to|into|in)\s+)") + degW + QStringLiteral(R"((?:\s+decima\w*)?)") + dmsW
                                                  + QStringLiteral(R"(\s+(?:de\s+|d'|of\s+)?(.+?)\s*)") + radW + QStringLiteral("$"));
        QString u = t;
        u.remove(convR);
        auto arg = [](QString v) {
            v.replace(QRegularExpression(QStringLiteral(R"((\d),(\d))")), QStringLiteral("\\1.\\2"));
            return v.trimmed();
        };
        QRegularExpressionMatch m;
        if ((m = toRad.match(u)).hasMatch() && !arg(m.captured(1)).isEmpty())
            return QStringLiteral("rad(%1)").arg(arg(m.captured(1)));
        if ((m = toRadHead.match(u)).hasMatch() && !arg(m.captured(1)).isEmpty())
            return QStringLiteral("rad(%1)").arg(arg(m.captured(1)));
        const bool head = !(m = toDeg.match(u)).hasMatch() && (m = toDegHead.match(u)).hasMatch();
        if (m.hasMatch()) {
            const QString x = arg(m.captured(head ? 2 : 1)), dms = m.captured(head ? 1 : 2);
            if (!x.isEmpty()) return dms.isEmpty() ? QStringLiteral("deg(%1)").arg(x) : QStringLiteral("ddtodms(deg(%1))").arg(x);
        }
    }

    // Conversion d'angle (16/11/2026) : « convertis en degrés décimaux W 00° 05' 52'' », « 48° 06′ 51″ N en degrés
    // décimaux », « convertis -0,0978 en degrés minutes secondes ». La notation d'angle est traduite par evaluate.
    bool angle = false;
    {
        static const QString conv = QStringLiteral(R"((?:conver\w*|transform\w*|passe[rz]?|exprime[rz]?|donne[sz]?|ecri[st]?|convert)\s+)");
        static const QString into = QStringLiteral(R"((?:(?:en|de|des|vers|to|into|in)\s+)?)");
        static const QString dd = QStringLiteral(R"((?:degres?\s+decima\w*|decimal\s+degrees?|dd))");
        static const QString dms = QStringLiteral(R"((?:degres?[\s,-]+minutes?[\s,-]+(?:et\s+)?secondes?|degrees?[\s,-]+minutes?[\s,-]+(?:and\s+)?seconds?|dms|sexagesima\w*))");
        const QRegularExpression ddHead(QStringLiteral("^") + conv + into + dd + QStringLiteral(R"(\s*(?:de\s+|d'|du\s+|:)?\s*)"));
        const QRegularExpression ddTail(QStringLiteral(R"(\s+(?:en|to|into|in)\s+)") + dd + QStringLiteral("$"));
        const QRegularExpression dmsHead(QStringLiteral("^") + conv + into + dms + QStringLiteral(R"(\s*(?:de\s+|d'|du\s+|:)?\s*)"));
        const QRegularExpression dmsTail(QStringLiteral(R"(\s+(?:en|to|into|in)\s+)") + dms + QStringLiteral("$"));
        const QRegularExpression convHead(QStringLiteral("^") + conv);
        if (t.contains(QRegularExpression(dmsHead.pattern())) || t.contains(dmsTail)) {
            t.remove(dmsHead); t.remove(dmsTail); t.remove(convHead);
            t.replace(QRegularExpression(QStringLiteral(R"((\d),(\d))")), QStringLiteral("\\1.\\2"));
            t = t.simplified();
            return t.isEmpty() ? QString() : QStringLiteral("ddtodms(%1)").arg(t);
        }
        if (t.contains(ddHead) || t.contains(ddTail)) {
            t.remove(ddHead); t.remove(ddTail); t.remove(convHead);
            t = t.simplified();
            angle = !t.isEmpty();
        } else if (t.contains(QChar(0x00B0)) && t.contains(convHead)) {     // « convertis 48° 06′ 51″ N »
            t.remove(convHead);
            t = t.simplified();
            angle = !t.isEmpty();
        }
    }

    t.replace(QRegularExpression(QStringLiteral(R"((\d),(\d))")), QStringLiteral("\\1.\\2"));   // 2,5 → 2.5 (argument de fonction)

    // Opérande : nombre, appel de fonction déjà traduit (un niveau de parenthèses) ou parenthèse.
    const QString X = QStringLiteral(R"((-?\d+(?:\.\d+)?|[a-z0-9_]+\([^()]*\)|\([^()]*\)))");
    const QString de = QStringLiteral(R"((?:\s+(?:de\s+|d'|of\s+)|\s+|(?<=')))");
    struct Rule { QString re, out; };
    const QVector<Rule> rules{
        { R"(\bracines?\s+cubiques?)" + de + X, "(\\1)^(1/3)" },
        { R"(\bcube\s+root)" + de + X, "(\\1)^(1/3)" },
        { R"(\bracines?(?:\s+carree?s?)?)" + de + X, "sqrt(\\1)" },      // « carré » : faute courante
        { R"(\bsquare\s+root)" + de + X, "sqrt(\\1)" },
        { R"(\b(?:logarithmes?|log)\s+(?:neperiens?|naturels?|natural))" + de + X, "ln(\\1)" },
        { R"(\bln)" + de + X, "ln(\\1)" },
        { R"(\b(?:logarithmes?(?:\s+decimale?s?)?|log(?:\s+decimale?s?)?))" + de + X, "log10(\\1)" },
        { R"(\b(?:exponentielles?|exponential|exp))" + de + X, "exp(\\1)" },
        { R"(\b(?:valeurs?\s+absolues?|absolute\s+value))" + de + X, "abs(\\1)" },
        { R"(\b(?:sinus|sine|sin))" + de + X, "sin(\\1)" },
        { R"(\b(?:cosinus|cosine|cos))" + de + X, "cos(\\1)" },
        { R"(\b(?:tangentes?|tangent|tan))" + de + X, "tan(\\1)" },
        { R"(\b(?:carres?|square))" + de + X, "(\\1)^2" },
        { R"(\bcube)" + de + X, "(\\1)^3" },
        { X + R"(\s+(?:au\s+carre|squared))", "(\\1)^2" },
        { X + R"(\s+(?:au\s+cube|cubed))", "(\\1)^3" },
        { X + R"(\s+(?:(?:a\s+la\s+|to\s+the\s+)?puissance|(?:to\s+the\s+)?power(?:\s+of)?|exposant)\s+)" + X, "(\\1)^(\\2)" },
        { X + R"(\s+(?:fois|multiplie\s+par|times|multiplied\s+by)\s+)" + X, "\\1*\\2" },
        { X + R"(\s+(?:divise\s+par|divided\s+by)\s+)" + X, "\\1/\\2" },
        { X + R"(\s+(?:plus)\s+)" + X, "\\1+\\2" },
        { X + R"(\s+(?:moins|minus)\s+)" + X, "\\1-\\2" },
    };
    bool changed = false;
    for (int guard = 0; guard < 12; ++guard) {
        bool any = false;
        for (const Rule& r : rules) {
            const QRegularExpression re(r.re);
            const QString before = t;
            t.replace(re, r.out);
            if (t != before) { any = true; break; }        // une règle à la fois, puis on recommence (imbrication)
        }
        if (!any) break;
        changed = true;
    }
    return changed || angle ? t.simplified() : QString();
}

bool Calculator::isCalculation(const QString& text) const
{
    const QString t = stripPrompt(text);
    if (t.isEmpty()) return false;
    // Il faut une opération : opérateur (hors signe initial), fonction, affectation ou carré. Un « ± » ne compte pas :
    // « 3560 +- 20 » seul est une mesure (datation), pas un calcul ; une unité ou un nom de méthode (« BP », « C14 »)
    // donne de toute façon un nom inconnu.
    static const QRegularExpression pm(QStringLiteral(R"(\+\s*/\s*-|\+\s*-|±)"));
    static const QRegularExpression op(QStringLiteral(R"([-+*/^×÷²=(−°])"));
    QString rest = t;
    rest.replace(pm, QStringLiteral(" "));
    rest.remove(QRegularExpression(QStringLiteral(R"(^[\s\-−+]+)")));
    // « +20 », « /3 », « - 5 » après un résultat : suite du calcul précédent (voir evaluate).
    static const QRegularExpression continues(QStringLiteral(R"(^\s*(?:[+*/^×÷]|[-−]\s))"));
    const bool fromAns = hasAns_ && continues.match(t).hasMatch();
    if (!fromAns && !op.match(rest).hasMatch()) return false;
    Calculator dry = *this;                        // essai à blanc : les variables du texte sont connues ligne après ligne
    const QVector<Line> lines = dry.evaluate(text);
    if (lines.isEmpty()) return false;
    for (const Line& l : lines) if (l.syntax) return false;
    return true;
}

} // namespace seldon
