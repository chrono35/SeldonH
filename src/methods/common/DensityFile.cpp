#include "DensityFile.h"

#include <QHash>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "TextUtil.h"

namespace seldon {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Nombre d'une clé : nombre JSON, ou texte (« 1 200 », « 0,5 », « −250 »). NaN si absente ou illisible.
double num(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        QString s = v.toString().trimmed();
        s.replace(QChar(0x2212), QLatin1Char('-'));
        s.remove(QLatin1Char(' ')).remove(QChar(0x00A0)).remove(QChar(0x202F));
        s.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double d = s.toDouble(&ok);
        if (ok) return d;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool flag(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0.0;
    const QString s = text::fold(v.toString()).trimmed();
    return s == QLatin1String("true") || s == QLatin1String("vrai") || s == QLatin1String("oui")
        || s == QLatin1String("yes") || s == QLatin1String("1");
}

QString nb(double v) { return QString::number(v, 'g', 10); }

QString frenchName(const QString& law)
{
    static const QHash<QString, QString> m = {
        { "normal", "normale" }, { "lognormal", "lognormale" }, { "uniform", "uniforme" }, { "triangular", "triangulaire" },
        { "trapezoidal", "trapezoidale" }, { "exponential", "exponentielle" }, { "gamma", "gamma" }, { "beta", "beta" } };
    return m.value(law, law);
}

} // namespace

QString densityLaw(const QString& name)
{
    QString s = text::fold(name).trimmed();
    s.replace(QRegularExpression(QStringLiteral("[-_]")), QStringLiteral(" "));
    s = s.simplified();
    s.remove(QRegularExpression(QStringLiteral("^(?:loi|distribution|law)\\s+(?:de\\s+)?")));
    static const QHash<QString, QString> m = {
        { "normal", "normal" }, { "normale", "normal" }, { "gauss", "normal" }, { "gaussienne", "normal" }, { "gaussian", "normal" },
        { "lognormal", "lognormal" }, { "log normal", "lognormal" }, { "lognormale", "lognormal" }, { "log normale", "lognormal" },
        { "uniform", "uniform" }, { "uniforme", "uniform" }, { "unif", "uniform" },
        { "triangular", "triangular" }, { "triangulaire", "triangular" }, { "triangle", "triangular" },
        { "trapezoidal", "trapezoidal" }, { "trapezoidale", "trapezoidal" }, { "trapeze", "trapezoidal" }, { "trapezoid", "trapezoidal" },
        { "exponential", "exponential" }, { "exponentielle", "exponential" }, { "exponentiel", "exponential" },
        { "gamma", "gamma" }, { "beta", "beta" } };
    return m.value(s);
}

DensityFileResult buildDensityFile(const QJsonObject& o)
{
    DensityFileResult R;
    auto fail = [&](const QString& why) { R.ok = false; R.error = why; return R; };

    const QString lawText = o.value("law").toString(o.value("loi").toString());
    R.law = densityLaw(lawText);
    if (R.law.isEmpty())
        return fail(QStringLiteral("loi « %1 » inconnue (normal, lognormal, uniform, triangular, trapezoidal, exponential, gamma, beta)")
                        .arg(lawText));

    // Densité de la variable x de la loi, et son étendue utile [xlo ; xhi].
    std::function<double(double)> pdf;
    double xlo = 0.0, xhi = 0.0;
    const QString& L = R.law;
    if (L == QLatin1String("normal")) {
        double m = num(o, "mean"), s = num(o, "sd");
        if (std::isnan(m)) m = num(o, "age");
        if (std::isnan(s)) s = num(o, "error");
        if (!std::isfinite(m) || !(s > 0)) return fail(QStringLiteral("loi normale : « mean » et « sd » > 0 requis"));
        pdf = [=](double x) { const double z = (x - m) / s; return std::exp(-0.5 * z * z) / (s * std::sqrt(2.0 * kPi)); };
        xlo = m - 5.0 * s; xhi = m + 5.0 * s;
        R.description = QStringLiteral("loi normale N(%1 ; %2)").arg(nb(m), nb(s));
    } else if (L == QLatin1String("lognormal")) {
        double mu = num(o, "meanlog"), sg = num(o, "sdlog");
        if (std::isnan(mu)) { const double med = num(o, "median"); if (med > 0) mu = std::log(med); }
        if (std::isnan(sg)) { const double f = num(o, "factor"); if (f > 1) sg = std::log(f); }
        if (!std::isfinite(mu) || !(sg > 0))
            return fail(QStringLiteral("loi log-normale : « meanlog » et « sdlog » > 0, ou « median » > 0 et « factor » > 1, requis"));
        pdf = [=](double x) {
            if (x <= 0) return 0.0;
            const double z = (std::log(x) - mu) / sg;
            return std::exp(-0.5 * z * z) / (x * sg * std::sqrt(2.0 * kPi));
        };
        xlo = 0.0; xhi = std::exp(mu + 4.0 * sg);
        R.description = QStringLiteral("loi log-normale (médiane %1, facteur %2)").arg(nb(std::exp(mu)), nb(std::exp(sg)));
    } else if (L == QLatin1String("uniform")) {
        const double a = num(o, "min"), b = num(o, "max");
        if (!(a < b)) return fail(QStringLiteral("loi uniforme : « min » < « max » requis"));
        pdf = [=](double x) { return (x >= a && x <= b) ? 1.0 / (b - a) : 0.0; };
        xlo = a; xhi = b;
        R.description = QStringLiteral("loi uniforme U[%1 ; %2]").arg(nb(a), nb(b));
    } else if (L == QLatin1String("triangular")) {
        const double a = num(o, "min"), c = num(o, "mode"), b = num(o, "max");
        if (!(a < b) || !(c >= a && c <= b)) return fail(QStringLiteral("loi triangulaire : min ≤ mode ≤ max et min < max requis"));
        pdf = [=](double x) {
            if (x < a || x > b) return 0.0;
            const double h = 2.0 / (b - a);
            if (x < c) return h * (x - a) / (c - a);
            if (x > c) return h * (b - x) / (b - c);
            return h;
        };
        xlo = a; xhi = b;
        R.description = QStringLiteral("loi triangulaire (min %1, mode %2, max %3)").arg(nb(a), nb(c), nb(b));
    } else if (L == QLatin1String("trapezoidal")) {
        const double a = num(o, "min"), b = num(o, "mode_min"), c = num(o, "mode_max"), d = num(o, "max");
        if (!(a <= b && b <= c && c <= d && a < d))
            return fail(QStringLiteral("loi trapézoïdale : min ≤ mode_min ≤ mode_max ≤ max et min < max requis"));
        const double h = 2.0 / ((d - a) + (c - b));
        pdf = [=](double x) {
            if (x < a || x > d) return 0.0;
            if (x < b) return h * (x - a) / (b - a);
            if (x > c) return h * (d - x) / (d - c);
            return h;
        };
        xlo = a; xhi = d;
        R.description = QStringLiteral("loi trapézoïdale (%1, plateau %2 à %3, %4)").arg(nb(a), nb(b), nb(c), nb(d));
    } else if (L == QLatin1String("exponential")) {
        double rate = num(o, "rate");
        if (!(rate > 0)) { const double m = num(o, "mean"); if (m > 0) rate = 1.0 / m; }
        if (!(rate > 0)) return fail(QStringLiteral("loi exponentielle : « mean » > 0 ou « rate » > 0 requis"));
        pdf = [=](double x) { return x < 0 ? 0.0 : rate * std::exp(-rate * x); };
        xlo = 0.0; xhi = std::log(1e4) / rate;
        R.description = QStringLiteral("loi exponentielle (moyenne %1)").arg(nb(1.0 / rate));
    } else if (L == QLatin1String("gamma")) {
        double k = num(o, "shape"), th = num(o, "scale");
        if (!(th > 0)) { const double r = num(o, "rate"); if (r > 0) th = 1.0 / r; }
        if (!(k > 0) || !(th > 0)) {
            const double m = num(o, "mean"), s = num(o, "sd");
            if (m > 0 && s > 0) { k = (m / s) * (m / s); th = s * s / m; }
        }
        if (!(k > 0) || !(th > 0)) return fail(QStringLiteral("loi gamma : « shape » et « scale » (ou « rate »), ou « mean » et « sd », > 0 requis"));
        const double lg = std::lgamma(k);
        pdf = [=](double x) {
            if (x < 0) return 0.0;
            if (x == 0) return k < 1 ? std::numeric_limits<double>::infinity() : (k == 1 ? 1.0 / th : 0.0);
            return std::exp((k - 1) * std::log(x) - x / th - lg - k * std::log(th));
        };
        xlo = 0.0; xhi = k * th + 10.0 * std::sqrt(k) * th;
        R.description = QStringLiteral("loi gamma (forme %1, échelle %2 ; moyenne %3)").arg(nb(k), nb(th), nb(k * th));
    } else {   // beta
        const double al = num(o, "alpha"), be = num(o, "beta");
        double a = num(o, "min"), b = num(o, "max");
        if (std::isnan(a)) a = 0.0;
        if (std::isnan(b)) b = 1.0;
        if (!(al > 0) || !(be > 0) || !(a < b)) return fail(QStringLiteral("loi bêta : « alpha » > 0, « beta » > 0 et min < max requis"));
        const double lb = std::lgamma(al) + std::lgamma(be) - std::lgamma(al + be);
        pdf = [=](double x) {
            const double u = (x - a) / (b - a);
            if (u < 0 || u > 1) return 0.0;
            return std::exp(-lb) * std::pow(u, al - 1) * std::pow(1 - u, be - 1) / (b - a);
        };
        xlo = a; xhi = b;
        R.description = QStringLiteral("loi bêta (α %1, β %2) sur [%3 ; %4]").arg(nb(al), nb(be), nb(a), nb(b));
    }

    // Placement sur l'axe du temps : t = offset + x, ou t = offset − x (durée comptée vers le passé).
    double offset = num(o, "offset");
    if (std::isnan(offset)) offset = 0.0;
    const bool reverse = flag(o, "reverse");
    const double sgn = reverse ? -1.0 : 1.0;
    if (reverse) R.placement = QStringLiteral("t = %1 − x (compté vers le passé)").arg(nb(offset));
    else if (offset != 0.0) R.placement = QStringLiteral("t = %1 + x").arg(nb(offset));

    // Bornes du fichier (années) : données, sinon étendue de la loi arrondie à l'année.
    const double tA = offset + sgn * xlo, tB = offset + sgn * xhi;
    double tmin = std::floor(std::min(tA, tB)), tmax = std::ceil(std::max(tA, tB));
    const bool userMin = !std::isnan(num(o, "tmin")), userMax = !std::isnan(num(o, "tmax"));
    if (userMin) tmin = num(o, "tmin");
    if (userMax) tmax = num(o, "tmax");
    if (!(tmax > tmin)) return fail(QStringLiteral("bornes du fichier invalides : tmin (%1) doit être inférieur à tmax (%2)").arg(nb(tmin), nb(tmax)));

    // Grille régulière : nombre de valeurs donné, ou pas donné (tmax ajusté pour un pas exact), sinon ≈ 1 an.
    int n = 0;
    const double points = num(o, "points"), stepIn = num(o, "step");
    if (points >= 2) {
        n = int(std::lround(points));
    } else if (stepIn > 0) {
        n = int(std::ceil((tmax - tmin) / stepIn - 1e-9)) + 1;
        tmax = tmin + (n - 1) * stepIn;
    } else {
        n = int(std::clamp<long>(std::lround(tmax - tmin) + 1, 101, 2001));
    }
    constexpr int kMaxPoints = 20001;
    if (n > kMaxPoints) {
        R.warnings << QStringLiteral("%1 valeurs demandées : limité à %2 (pas élargi).").arg(n).arg(kMaxPoints);
        n = kMaxPoints;
    }
    const double step = (tmax - tmin) / (n - 1);

    std::vector<double> t(n), v(n);
    QStringList infinite;
    for (int k = 0; k < n; ++k) {
        t[k] = (k == n - 1) ? tmax : tmin + k * step;
        const double x = sgn * (t[k] - offset);
        double y = pdf(x);
        if (!std::isfinite(y)) {          // densité infinie au bord (gamma ou bêta de forme < 1) : valeur un peu à l'intérieur
            const double y1 = pdf(x + 0.25 * step), y2 = pdf(x - 0.25 * step);
            y = std::max(std::isfinite(y1) ? y1 : 0.0, std::isfinite(y2) ? y2 : 0.0);
            infinite << nb(t[k]);
        }
        v[k] = std::max(0.0, y);
    }
    if (!infinite.isEmpty())
        R.warnings << QStringLiteral("densité infinie en t = %1 : remplacée par la valeur à un quart de pas.").arg(infinite.join(", "));

    double area = 0.0;
    for (int k = 1; k < n; ++k) area += 0.5 * (v[k] + v[k - 1]) * step;
    if (!(area > 0)) return fail(QStringLiteral("densité nulle sur [%1 ; %2] : vérifiez tmin, tmax et offset").arg(nb(tmin), nb(tmax)));
    if ((userMin || userMax) && area < 0.99)
        R.warnings << QStringLiteral("les bornes du fichier ne contiennent que %1 % de la loi (ChronoModel renormalise sur [tmin ; tmax]).")
                          .arg(QString::number(100.0 * area, 'f', 1));

    R.tmin = tmin; R.tmax = tmax; R.step = step; R.points = n;
    // Densité affichée : aire 1 au sens de densityArea (même règle des trapèzes que les statistiques et les HPD).
    R.density.t = t;
    R.density.v = v;
    R.density.step = step;
    const double shownArea = densityArea(R.density);
    for (double& y : R.density.v) y /= (shownArea > 0 ? shownArea : area);

    QString name = o.value("name").toString().trimmed();
    if (name.isEmpty()) name = QStringLiteral("%1_%2_%3").arg(frenchName(L), nb(tmin), nb(tmax)).toLower();
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    if (!name.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive)) name += QStringLiteral(".csv");
    R.fileName = name;

    QString csv = QStringLiteral("// SeldonH : %1%2 ; %3 valeurs de %4 à %5 (pas %6)\n")
                      .arg(R.description, R.placement.isEmpty() ? QString() : QStringLiteral(" ; ") + R.placement)
                      .arg(n).arg(nb(tmin), nb(tmax), nb(step));
    csv += nb(tmin) + QLatin1Char('\n') + nb(tmax) + QLatin1Char('\n');
    for (int k = 0; k < n; ++k) csv += QString::number(v[k], 'g', 8) + QLatin1Char('\n');
    R.csv = csv;
    R.ok = true;
    return R;
}

} // namespace seldon
