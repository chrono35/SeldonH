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

#include "CurveTraces.h"

#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>
#include <numeric>

#include "DateFormat.h"
#include "TextUtil.h"

namespace seldon {

// ------------------------------------------------------------------ évaluation (port de ChronoModel)

void SplineTrace::eval(size_t row, int dim, double t, double& G, double& GP) const
{
    if (!vector) { evalRaw(row, dim, t, G, GP); return; }
    // convertToIDF et computeDerivatives de ChronoModel (CurveUtilities.cpp)
    double x, xp, y, yp, z, zp;
    evalRaw(row, 0, t, x, xp);
    evalRaw(row, 1, t, y, yp);
    evalRaw(row, 2, t, z, zp);
    constexpr double deg = 180.0 / M_PI;
    const double F = std::sqrt(x * x + y * y + z * z);
    if (F == 0.0) { G = 0.0; GP = 0.0; return; }
    const double dF = (x * xp + y * yp + z * zp) / F;
    if (dim == 0) {
        G = std::asin(z / F) * deg;
        const double den = std::sqrt(F * F - z * z);
        GP = den == 0.0 ? 0.0 : (zp - (z / F) * dF) / den * deg;
    } else if (dim == 1) {
        G = std::atan2(y, x) * deg;
        const double den = x * x + y * y;
        GP = den == 0.0 ? 0.0 : (x * yp - y * xp) / den * deg;
    } else {
        G = F;
        GP = dF;
    }
}

int SplineTrace::dimOf(const QString& name) const
{
    const QString n = text::fold(name).trimmed();
    if (n.isEmpty()) return -1;
    auto named = [&](const QString& want) { return int(names.indexOf(want)); };
    int k = -1;
    if (n == "i" || n.startsWith("inc")) k = named("I");
    else if (n == "d" || n.startsWith("dec")) k = named("D");
    else if (n == "f" || n.startsWith("int") || n.startsWith("champ") || n.startsWith("field")) k = named("F");
    if (k >= 0) return k;
    const int pos = curves::dimIndex(n);             // X, Y, Z ou 1, 2, 3 : position
    return pos >= 0 && pos < dims ? pos : -1;
}

QString SplineTrace::unitOf(int dim) const
{
    const QString n = names.value(dim);
    return n == "I" || n == "D" ? QStringLiteral("°") : QString();
}

void SplineTrace::evalRaw(size_t row, int dim, double t, double& G, double& GP) const
{
    // valeurs_G_GP_GS (ChronoModel 4, CurveUtilities.cpp) : spline cubique naturelle en temps réduit, écrite avec les
    // valeurs g_i et les dérivées secondes γ_i aux nœuds (forme de Green et Silverman) ; prolongement linéaire hors des
    // nœuds (dérivée première constante, dérivée seconde nulle). Dérivée ramenée en unités par an (÷ (tmax − tmin)).
    constexpr double one_sixth = 1.0 / 6.0;
    const size_t n = size_t(knots);
    const double* T = tr.data() + row * n;
    const double* g = this->g[dim].data() + row * n;
    const double* c = gamma[dim].data() + row * n;
    const double inv = 1.0 / span;
    const double x = (t - tmin) * inv;
    if (n < 2) { G = n ? g[0] : std::nan(""); GP = 0.0; return; }
    if (x <= T[0]) {
        const double h = T[1] - T[0];
        GP = (g[1] - g[0]) / h - h * c[1] * one_sixth;
        G = g[0] - (T[0] - x) * GP;
    } else if (x >= T[n - 1]) {
        const double h = T[n - 1] - T[n - 2];
        GP = (g[n - 1] - g[n - 2]) / h + h * c[n - 2] * one_sixth;
        G = g[n - 1] + (x - T[n - 1]) * GP;
    } else {
        const size_t i = size_t(std::upper_bound(T, T + n, x) - T) - 1;      // T[i] <= x < T[i+1]
        const double h = T[i + 1] - T[i];
        const double u = x - T[i], v = h - u;
        G = (u * g[i + 1] + v * g[i]) / h - one_sixth * (u * v) * ((1.0 + u / h) * c[i + 1] + (1.0 + v / h) * c[i]);
        const double ur = u / h, vr = v / h;
        GP = (g[i + 1] - g[i]) / h + (h * one_sixth) * ((3.0 * ur * ur - 1.0) * c[i + 1] - (3.0 * vr * vr - 1.0) * c[i]);
    }
    GP *= inv;
}

void SplineTrace::evalRaw2(size_t row, int dim, double t, double& G, double& GP, double& GS) const
{
    evalRaw(row, dim, t, G, GP);
    // Dérivée seconde de la spline cubique naturelle : interpolation linéaire des γ_i entre les nœuds (Green et
    // Silverman), nulle hors des nœuds (prolongement linéaire), ramenée en unités par an² (÷ (tmax − tmin)²).
    const size_t n = size_t(knots);
    GS = 0.0;
    if (n < 2) return;
    const double* T = tr.data() + row * n;
    const double* c = gamma[dim].data() + row * n;
    const double x = (t - tmin) / span;
    if (x <= T[0] || x >= T[n - 1]) return;
    const size_t i = size_t(std::upper_bound(T, T + n, x) - T) - 1;
    const double h = T[i + 1] - T[i];
    const double u = x - T[i], v = h - u;
    GS = (u * c[i + 1] + v * c[i]) / h / (span * span);
}

void SplineTrace::eval2(size_t row, int dim, double t, double& G, double& GP, double& GS) const
{
    if (!vector) { evalRaw2(row, dim, t, G, GP, GS); return; }
    // Mode vecteur : dérivées première et seconde de I, D, F tirées de celles de x, y, z (règle de dérivation en chaîne).
    double x, xp, xs, y, yp, ys, z, zp, zs;
    evalRaw2(row, 0, t, x, xp, xs);
    evalRaw2(row, 1, t, y, yp, ys);
    evalRaw2(row, 2, t, z, zp, zs);
    constexpr double deg = 180.0 / M_PI;
    const double F = std::sqrt(x * x + y * y + z * z);
    if (F == 0.0) { G = GP = GS = 0.0; return; }
    const double dF = (x * xp + y * yp + z * zp) / F;
    const double d2F = (xp * xp + yp * yp + zp * zp + x * xs + y * ys + z * zs - dF * dF) / F;
    if (dim == 0) {                          // I = asin(w), w = z / F
        const double w = z / F;
        const double wp = (zp - w * dF) / F;
        const double ws = (zs - 2.0 * wp * dF - w * d2F) / F;
        const double c2 = 1.0 - w * w;
        G = std::asin(std::clamp(w, -1.0, 1.0)) * deg;
        if (c2 <= 0.0) { GP = GS = 0.0; return; }
        const double c1 = std::sqrt(c2);
        GP = wp / c1 * deg;
        GS = (ws / c1 + w * wp * wp / (c2 * c1)) * deg;
    } else if (dim == 1) {                   // D = atan2(y, x)
        const double r2 = x * x + y * y;
        G = std::atan2(y, x) * deg;
        if (r2 == 0.0) { GP = GS = 0.0; return; }
        const double num = x * yp - y * xp;
        GP = num / r2 * deg;
        GS = ((x * ys - y * xs) * r2 - num * 2.0 * (x * xp + y * yp)) / (r2 * r2) * deg;
    } else {
        G = F;
        GP = dF;
        GS = d2F;
    }
}

double SplineTrace::quantity(size_t row, int dim, double t, int order) const
{
    double G, GP, GS;
    if (order <= 0) { eval(row, dim, t, G, GP); return G; }
    if (order == 1) { eval(row, dim, t, G, GP); return GP; }
    eval2(row, dim, t, G, GP, GS);
    return GS;
}

namespace curves {

namespace {

QVector<SplineTrace>& splineStore() { static QVector<SplineTrace> s; return s; }
QVector<CurveMap>& mapStore() { static QVector<CurveMap> s; return s; }

QStringList splitLines(const QString& text)
{
    static const QRegularExpression eol(QStringLiteral("\r\n|\r|\n"));
    return text.split(eol, Qt::SkipEmptyParts);
}

// Lignes sans les commentaires « # … » ; `comments` reçoit ceux-ci.
QStringList dataLines(const QString& text, QStringList* comments = nullptr)
{
    QStringList out;
    for (QString l : splitLines(text)) {
        if (l.startsWith(QChar(0xFEFF))) l.remove(0, 1);
        if (l.trimmed().isEmpty()) continue;
        if (l.trimmed().startsWith('#')) { if (comments) *comments << l.trimmed().mid(1).trimmed(); continue; }
        out << l;
    }
    return out;
}

QString unquote(QString s)
{
    s = s.trimmed();
    if (s.size() >= 2 && s.startsWith('"') && s.endsWith('"')) s = s.mid(1, s.size() - 2);
    return s.trimmed();
}

QStringList cellsOf(const QString& line, QChar sep)
{
    QStringList c = line.split(sep);
    while (!c.isEmpty() && c.last().trimmed().isEmpty()) c.removeLast();       // séparateur final (cartes)
    return c;
}

double number(QString s, bool decimalComma, bool& ok)
{
    s = unquote(s);
    if (s.isEmpty() || s.compare(QLatin1String("nan"), Qt::CaseInsensitive) == 0) { ok = false; return std::nan(""); }
    s.replace(QChar(0x2212), '-');
    if (decimalComma) s.replace(',', '.');
    return s.toDouble(&ok);
}

const QRegularExpression& splineHeaderRe()
{
    static const QRegularExpression re(QStringLiteral("^(Theta|Theta_reduced|Gx|Gamma_x|Gy|Gamma_y|Gz|Gamma_z)_(\\d+)$"));
    return re;
}

// Séparateur dont l'en-tête commence par « Theta_1 ».
QChar splineSeparator(const QString& header)
{
    for (const QChar sep : { QChar(';'), QChar('\t'), QChar(',') }) {
        const QStringList h = cellsOf(header, sep);
        if (h.size() >= 4 && unquote(h.first()) == QLatin1String("Theta_1") && splineHeaderRe().match(unquote(h.at(1))).hasMatch())
            return sep;
    }
    return QChar();
}

bool mapHeader(const QString& header, QChar& sep, bool& age)
{
    for (const QChar s : { QChar(';'), QChar('\t'), QChar(',') }) {
        const QStringList h = cellsOf(header, s);
        if (h.size() < 3) continue;
        const QString first = text::fold(unquote(h.first())).remove(' ');
        if (first == QLatin1String("y/date") || first == QLatin1String("y/age")) {
            sep = s;
            age = first == QLatin1String("y/age");
            return true;
        }
    }
    return false;
}

// Format de date de ChronoModel (DateUtils::dateFormatToString) : faux si inconnu (format personnalisé).
bool dateFormatOf(const QString& label, DateFormat& f)
{
    const QString l = text::fold(label).simplified();
    static const QList<QPair<QString, DateFormat>> known = {
        { "bc/ad", DateFormat::BcAd }, { "bce/ce", DateFormat::BceCe }, { "age cal. bp", DateFormat::CalBP },
        { "age cal. b2k", DateFormat::CalB2K }, { "date cal. bp", DateFormat::DatBP }, { "date cal. b2k", DateFormat::DatB2K },
        { "age ka", DateFormat::Ka }, { "age ma", DateFormat::Ma } };
    for (const auto& k : known) if (l == k.first) { f = k.second; return true; }
    return false;
}

QString fileComponent(const QString& fileName, bool* derivative)
{
    // ChronoModel écrit « Curve_Inc_Map.csv » mais « CurveDec_Map.csv », « CurveF_GP_Map.csv » : « _ » facultatif.
    static const QRegularExpression re(QStringLiteral("Curve_?([A-Za-z]+?)(_GP)?_(?:Map|Splines)"), QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(QFileInfo(fileName).fileName());
    if (derivative) *derivative = m.hasMatch() && !m.captured(2).isEmpty();
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

int dimIndex(const QString& name)
{
    const QString n = text::fold(name).trimmed();
    if (n.isEmpty()) return -1;
    if (n == "x" || n == "gx" || n == "1" || n.startsWith("inc") || n == "i") return 0;
    if (n == "y" || n == "gy" || n == "2" || n.startsWith("dec") || n == "d") return 1;
    if (n == "z" || n == "gz" || n == "3" || n == "f" || n.startsWith("int") || n.startsWith("champ") || n.startsWith("field")) return 2;
    return -1;
}

QString dimName(int dim)
{
    return dim == 0 ? QStringLiteral("X") : dim == 1 ? QStringLiteral("Y") : dim == 2 ? QStringLiteral("Z") : QString();
}

bool looksLikeSplines(const QString& text)
{
    const QStringList l = dataLines(text.left(20000));
    return !l.isEmpty() && !splineSeparator(l.first()).isNull();
}

bool parseSplines(const QString& text, const QString& fileName, SplineTrace& out, QString* error)
{
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    const QStringList lines = dataLines(text);
    if (lines.size() < 2) return fail(QStringLiteral("aucun tirage après l'en-tête"));
    const QChar sep = splineSeparator(lines.first());
    if (sep.isNull()) return fail(QStringLiteral("en-tête « Theta_1… » introuvable : ce n'est pas un export des splines de ChronoModel"));
    QStringList head = cellsOf(lines.first(), sep);
    for (QString& h : head) h = unquote(h);
    // Colonnes de chaque nœud : Theta, Theta_reduced, Gx, Gamma_x (et Gy, Gamma_y, Gz, Gamma_z).
    struct Col { int theta = -1, tr = -1, g[3] = { -1, -1, -1 }, gamma[3] = { -1, -1, -1 }; };
    std::vector<Col> knots;
    for (int i = 0; i < head.size(); ++i) {
        const auto m = splineHeaderRe().match(head[i]);
        if (!m.hasMatch()) return fail(QStringLiteral("colonne « %1 » inattendue dans l'en-tête des splines").arg(head[i]));
        const int k = m.captured(2).toInt() - 1;
        if (k < 0 || k > 100000) return fail(QStringLiteral("numéro de nœud invalide : « %1 »").arg(head[i]));
        if (size_t(k) >= knots.size()) knots.resize(size_t(k) + 1);
        const QString w = m.captured(1);
        Col& c = knots[size_t(k)];
        if (w == "Theta") c.theta = i; else if (w == "Theta_reduced") c.tr = i;
        else if (w == "Gx") c.g[0] = i; else if (w == "Gamma_x") c.gamma[0] = i;
        else if (w == "Gy") c.g[1] = i; else if (w == "Gamma_y") c.gamma[1] = i;
        else if (w == "Gz") c.g[2] = i; else c.gamma[2] = i;
    }
    if (knots.size() < 2) return fail(QStringLiteral("il faut au moins deux nœuds (deux Events) pour une spline"));
    int dims = 0;
    for (int d = 0; d < 3; ++d) {
        bool all = true;
        for (const Col& c : knots) all = all && c.g[d] >= 0 && c.gamma[d] >= 0;
        if (all) dims = d + 1; else break;
    }
    for (const Col& c : knots)
        if (c.theta < 0 || c.tr < 0) return fail(QStringLiteral("colonnes Theta / Theta_reduced incomplètes"));
    if (dims == 0) return fail(QStringLiteral("colonnes Gx / Gamma_x absentes"));

    // Décimale : « , » si un nombre de la première ligne en contient une et que le séparateur n'est pas « , ».
    const QStringList first = cellsOf(lines.at(1), sep);
    bool commaPairs = false, decimalComma = false;
    if (first.size() == 2 * head.size() && sep == ',') { commaPairs = true; decimalComma = true; }
    else if (first.size() != head.size())
        return fail(QStringLiteral("la première ligne a %1 cellules pour %2 colonnes").arg(first.size()).arg(head.size()));
    else decimalComma = sep != ',' && first.join(QString()).contains(',');

    SplineTrace S;
    S.file = QFileInfo(fileName).fileName();
    S.dims = dims;
    S.knots = int(knots.size());
    S.csvFormat = QStringLiteral("séparateur « %1 », décimale « %2 »").arg(sep == '\t' ? QStringLiteral("tabulation") : QString(sep),
                                                                          decimalComma ? QStringLiteral(",") : QStringLiteral("."));
    const size_t n = knots.size();
    size_t skipped = 0;
    bool resorted = false;
    std::vector<double> rowV(size_t(head.size()));
    for (int li = 1; li < lines.size(); ++li) {
        QStringList cells = cellsOf(lines[li], sep);
        if (commaPairs) {
            if (cells.size() != 2 * head.size()) { ++skipped; continue; }
            QStringList joined;
            for (int k = 0; k + 1 < cells.size(); k += 2) joined << cells[k] + ',' + cells[k + 1];
            cells = joined;
        }
        if (cells.size() < head.size()) { ++skipped; continue; }
        bool okRow = true;
        for (int i = 0; i < head.size() && okRow; ++i) { bool ok = false; rowV[size_t(i)] = number(cells[i], decimalComma, ok); okRow = ok; }
        if (!okRow) { ++skipped; continue; }
        // Nœuds triés par temps réduit (ChronoModel les trie ; on le vérifie).
        std::vector<size_t> ord(n);
        std::iota(ord.begin(), ord.end(), size_t(0));
        auto trOf = [&](size_t k) { return rowV[size_t(knots[k].tr)]; };
        if (!std::is_sorted(ord.begin(), ord.end(), [&](size_t a, size_t b) { return trOf(a) < trOf(b); })) {
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return trOf(a) < trOf(b); });
            resorted = true;
        }
        bool distinct = true;
        for (size_t k = 1; k < n; ++k) distinct = distinct && trOf(ord[k]) > trOf(ord[k - 1]);
        if (!distinct) { ++skipped; continue; }
        for (size_t k : ord) {
            const Col& c = knots[k];
            S.theta.push_back(rowV[size_t(c.theta)]);
            S.tr.push_back(rowV[size_t(c.tr)]);
            for (int d = 0; d < dims; ++d) { S.g[d].push_back(rowV[size_t(c.g[d])]); S.gamma[d].push_back(rowV[size_t(c.gamma[d])]); }
        }
        ++S.rows;
    }
    if (S.rows < 2) return fail(QStringLiteral("moins de deux tirages lisibles"));
    // Noms des composantes : « Curve_Inc_Splines.csv » = archéomagnétisme. 3 composantes : mode vecteur (x, y, z
    // cartésiens -> I, D, F) ; 2 : déclinaison inconnue (I et F directs) ; 1 : la composante du nom.
    {
        bool gp = false;
        const QString comp = text::fold(fileComponent(fileName, &gp));
        if (comp.startsWith(QLatin1String("inc")) && dims == 3) { S.vector = true; S.names = { "I", "D", "F" }; }
        else if (comp.startsWith(QLatin1String("inc")) && dims == 2) S.names = { "I", "F" };
        else if (dims == 1 && comp.startsWith(QLatin1String("inc"))) S.names = { "I" };
        else if (dims == 1 && comp.startsWith(QLatin1String("dec"))) S.names = { "D" };
        else if (dims == 1 && comp == QLatin1String("f")) S.names = { "F" };
        else S.names = QStringList{ "X", "Y", "Z" }.mid(0, dims);
    }
    // Période d'étude : Theta = tmin + span × Theta_reduced (moindres carrés sur les 200 premiers tirages).
    const size_t m = std::min<size_t>(S.rows, 200) * n;
    double mx = 0, my = 0;
    for (size_t i = 0; i < m; ++i) { mx += S.tr[i]; my += S.theta[i]; }
    mx /= double(m); my /= double(m);
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < m; ++i) { sxx += (S.tr[i] - mx) * (S.tr[i] - mx); sxy += (S.tr[i] - mx) * (S.theta[i] - my); }
    if (!(sxx > 0)) return fail(QStringLiteral("temps réduits constants : période d'étude impossible à retrouver"));
    S.span = sxy / sxx;
    S.tmin = my - S.span * mx;
    if (!(S.span > 0)) return fail(QStringLiteral("période d'étude incohérente (Theta et Theta_reduced)"));
    // Arrondi à l'année quand c'est le cas (période d'étude entière dans ChronoModel).
    if (std::abs(S.span - std::round(S.span)) < 1e-4 * std::max(1.0, S.span)) S.span = std::round(S.span);
    if (std::abs(S.tmin - std::round(S.tmin)) < 1e-4 * std::max(1.0, S.span)) S.tmin = std::round(S.tmin);
    if (error) {
        QStringList notes;
        if (skipped) notes << QStringLiteral("%1 ligne(s) illisible(s) ignorée(s)").arg(skipped);
        if (resorted) notes << QStringLiteral("nœuds remis dans l'ordre des dates");
        *error = notes.join(QStringLiteral(" ; "));
    }
    out = std::move(S);
    return true;
}

bool looksLikeMap(const QString& text)
{
    const QStringList l = dataLines(text.left(20000));
    QChar sep;
    bool age = false;
    return !l.isEmpty() && mapHeader(l.first(), sep, age);
}

bool parseMap(const QString& text, const QString& fileName, CurveMap& out, QString* error)
{
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    QStringList comments;
    const QStringList lines = dataLines(text, &comments);
    if (lines.size() < 3) return fail(QStringLiteral("carte trop courte"));
    QChar sep;
    bool age = false;
    if (!mapHeader(lines.first(), sep, age)) return fail(QStringLiteral("en-tête « Y / Date » introuvable : ce n'est pas une carte de courbe de ChronoModel"));
    CurveMap M;
    M.file = QFileInfo(fileName).fileName();
    M.component = fileComponent(fileName, &M.derivative);
    for (const QString& c : comments) {
        static const QRegularExpression df(QStringLiteral("^Date Format\\s*:\\s*(.*)$"), QRegularExpression::CaseInsensitiveOption);
        if (const auto m = df.match(c); m.hasMatch()) M.dateFormat = m.captured(1).trimmed();
    }
    DateFormat fmt = DateFormat::BcAd;
    if (!M.dateFormat.isEmpty() && !dateFormatOf(M.dateFormat, fmt))
        M.notes << QStringLiteral("format de date personnalisé « %1 » : ChronoModel y écrit les années BC/AD").arg(M.dateFormat);
    const QStringList head = cellsOf(lines.first(), sep);
    const bool decimalComma = sep != ',' && (head.join(QString()).contains(',') || lines.at(1).contains(','));
    if (sep == ',' && head.size() > 3 && lines.at(1).count(',') > 2 * head.size())
        return fail(QStringLiteral("séparateur et décimale sont tous deux « , » : réexporter la carte avec « ; » comme séparateur"));
    std::vector<double> tcol;
    for (int i = 1; i < head.size(); ++i) {
        bool ok = false;
        const double v = number(head[i], decimalComma, ok);
        if (!ok) return fail(QStringLiteral("date illisible dans l'en-tête : « %1 »").arg(head[i]));
        tcol.push_back(dateformat::toCalendar(v, fmt));
    }
    const size_t nc = tcol.size();
    std::vector<double> labels;
    std::vector<std::vector<double>> rowsV;
    for (int li = 1; li < lines.size(); ++li) {
        const QStringList cells = cellsOf(lines[li], sep);
        if (size_t(cells.size()) != nc + 1) return fail(QStringLiteral("la ligne %1 a %2 cellules pour %3 dates").arg(li + 1).arg(cells.size() - 1).arg(nc));
        bool ok = false;
        labels.push_back(number(cells[0], decimalComma, ok));
        if (!ok) return fail(QStringLiteral("valeur Y illisible : « %1 »").arg(cells[0]));
        std::vector<double> r(nc);
        for (size_t c = 0; c < nc; ++c) {
            r[c] = number(cells[int(c) + 1], decimalComma, ok);
            if (!ok) r[c] = 0.0;
        }
        rowsV.push_back(std::move(r));
    }
    const size_t nr = labels.size();
    // Grille des valeurs (ChronoModel écrit les lignes de la plus grande valeur à la plus petite).
    const double top = labels.front(), bottom = labels.back();
    std::vector<double> y(nr);
    bool strictly = true, integers = true;
    for (size_t r = 1; r < nr; ++r) strictly = strictly && labels[r] < labels[r - 1];
    for (double l : labels) integers = integers && std::abs(l - std::round(l)) < 1e-12;
    for (size_t r = 0; r < nr; ++r) y[r] = bottom + (top - bottom) * double(nr - 1 - r) / double(nr - 1);
    if (!(top > bottom)) {
        M.usable = false;
        M.notes << QStringLiteral("les valeurs Y de la carte sont toutes écrites « %1 » (arrondies à l'unité par ChronoModel) : sa grille "
                                  "ne peut pas être reconstruite. Joindre plutôt la trace des splines (Curve_…_Splines.csv)")
                       .arg(QString::number(top));
    } else if (!strictly || integers) {
        double dev = 0.0;
        for (size_t r = 0; r < nr; ++r) dev = std::max(dev, std::abs(labels[r] - y[r]));
        if (dev > 0.5 + 1e-9 && strictly) {
            for (size_t r = 0; r < nr; ++r) y[r] = labels[r];          // grille irrégulière mais lisible : telle quelle
        } else if (!strictly || dev > 1e-9) {
            M.notes << QStringLiteral("valeurs Y écrites arrondies par ChronoModel : grille reconstruite, régulière, de %1 à %2 (%3 valeurs, "
                                      "pas %4) ; ses bornes sont exactes à ± 0,5 près")
                           .arg(text::numAuto(bottom, 3), text::numAuto(top, 3)).arg(nr)
                           .arg(text::numAuto((top - bottom) / double(nr - 1), 4));
        }
    } else {
        for (size_t r = 0; r < nr; ++r) y[r] = labels[r];
    }
    // Rangement : dates croissantes, valeurs croissantes.
    std::vector<size_t> cord(nc);
    std::iota(cord.begin(), cord.end(), size_t(0));
    std::sort(cord.begin(), cord.end(), [&](size_t a, size_t b) { return tcol[a] < tcol[b]; });
    std::vector<size_t> rord(nr);
    std::iota(rord.begin(), rord.end(), size_t(0));
    std::sort(rord.begin(), rord.end(), [&](size_t a, size_t b) { return y[a] < y[b]; });
    M.t.resize(nc);
    M.y.resize(nr);
    M.counts.assign(nc * nr, 0.0);
    for (size_t c = 0; c < nc; ++c) M.t[c] = tcol[cord[c]];
    for (size_t r = 0; r < nr; ++r) M.y[r] = y[rord[r]];
    for (size_t c = 0; c < nc; ++c)
        for (size_t r = 0; r < nr; ++r) M.counts[c * nr + r] = rowsV[rord[r]][cord[c]];
    if (error) *error = M.notes.join(QStringLiteral(" ; "));
    out = std::move(M);
    return true;
}

std::vector<double> crossings(const SplineTrace& s, size_t row, int dim, double y, double a, double b, int steps, int order)
{
    std::vector<double> out;
    if (!(b > a) || steps < 2) return out;
    const double h = (b - a) / steps;
    auto f = [&](double t) { return s.quantity(row, dim, t, order); };
    double t0 = a, f0 = f(a) - y;
    if (f0 == 0.0) out.push_back(a);
    for (int k = 1; k <= steps; ++k) {
        const double t1 = k == steps ? b : a + k * h;
        const double f1 = f(t1) - y;
        if (f1 == 0.0) out.push_back(t1);
        else if (f0 != 0.0 && (f0 < 0) != (f1 < 0)) {
            double lo = t0, hi = t1, flo = f0;
            for (int it = 0; it < 40 && hi - lo > 1e-6; ++it) {
                const double mid = 0.5 * (lo + hi), fm = f(mid) - y;
                if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
            }
            out.push_back(0.5 * (lo + hi));
        }
        t0 = t1;
        f0 = f1;
    }
    return out;
}

QString summary(const SplineTrace& s)
{
    const QLocale fr(QLocale::French);
    QStringList comps = s.names;
    if (s.vector) comps = { QStringLiteral("I (inclinaison)"), QStringLiteral("D (déclinaison)"), QStringLiteral("F (intensité)") };
    return QStringLiteral("[Trace des splines de ChronoModel « %1 » lue par SeldonH : %2 tirages, %3 nœuds (Events) par spline, "
                          "composante%4 %5%8, période d'étude %6 → %7 (BC/AD). SeldonH évalue chaque spline comme ChronoModel "
                          "(valeur G, vitesse G′ et accélération G″ à toute date) : valeur de la courbe à une date, vitesse ou "
                          "accélération instantanée, vitesse moyenne entre deux dates, date où la courbe (ou sa vitesse, son "
                          "accélération) atteint une valeur, date d'une mesure sur la courbe, sa vitesse ou son accélération, "
                          "vitesse entre deux valeurs, tracé des courbes de valeur, de vitesse et d'accélération (op « plot »). Demande ces "
                          "calculs avec la méthode « curve » ; n'écris pas les valeurs toi-même.]")
        .arg(s.file, fr.toString(qlonglong(s.rows))).arg(s.knots).arg(s.dims > 1 ? QStringLiteral("s") : QString(), comps.join(QStringLiteral(", ")),
             text::numAuto(s.tmin, 3), text::numAuto(s.tmax(), 3),
             s.vector ? QStringLiteral(" (mode vecteur : splines sur x, y, z cartésiens, I, D, F calculés comme ChronoModel)") : QString());
}

QString summary(const CurveMap& m)
{
    QString t = QStringLiteral("[Carte de densité de ChronoModel « %1 » lue par SeldonH : %2 de la composante %3, %4 dates de %5 à %6, "
                               "%7 valeurs de %8 à %9. Elle donne la loi de la %2 à une date donnée (pas la vitesse moyenne ni la date "
                               "d'atteinte d'une valeur, qui demandent Curve_…_Splines.csv). Méthode « curve ».")
                    .arg(m.file, m.derivative ? QStringLiteral("vitesse G′") : QStringLiteral("valeur G"),
                         m.component.isEmpty() ? QStringLiteral("?") : m.component)
                    .arg(m.t.size()).arg(text::numAuto(m.t.front(), 2), text::numAuto(m.t.back(), 2))
                    .arg(m.y.size()).arg(text::numAuto(m.y.front(), 4), text::numAuto(m.y.back(), 4));
    if (!m.notes.isEmpty()) t += QStringLiteral(" Lecture : ") + m.notes.join(QStringLiteral(" ; ")) + '.';
    return t + ']';
}

void add(const SplineTrace& s)
{
    QVector<SplineTrace>& v = splineStore();
    for (SplineTrace& o : v) if (o.file == s.file) { o = s; return; }
    v << s;
}

void add(const CurveMap& m)
{
    QVector<CurveMap>& v = mapStore();
    for (CurveMap& o : v) if (o.file == m.file) { o = m; return; }
    v << m;
}

void clear() { splineStore().clear(); mapStore().clear(); }
const QVector<SplineTrace>& splines() { return splineStore(); }
const QVector<CurveMap>& maps() { return mapStore(); }
bool anyLoaded() { return !splineStore().isEmpty() || !mapStore().isEmpty(); }

} // namespace curves
} // namespace seldon
