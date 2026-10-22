#include "GaussMethod.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../common/TextUtil.h"

namespace seldon {

namespace {

constexpr double kSigma = 5.0;

// y = a x² + b x + c  ->  racines triées (ChronoModel : solve_quadratic)
std::pair<double, double> solveQuadratic(double y, double a, double b, double c)
{
    const double inf = std::numeric_limits<double>::infinity();
    if (a == 0.0) {
        if (b == 0.0) return { inf, inf };
        const double r = (y - c) / b;
        return { r, r };
    }
    const double delta = b * b - 4.0 * a * (c - y);
    if (delta > 0.0) {
        const double s1 = (-b - std::sqrt(delta)) / (2.0 * a), s2 = (-b + std::sqrt(delta)) / (2.0 * a);
        return { std::min(s1, s2), std::max(s1, s2) };
    }
    return { 0.0, 0.0 };
}

} // namespace

QVector<QJsonObject> GaussMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);

    static const QRegularExpression ctx(R"((gauss|loi normale|distribution normale|\bn\s*\(|normale))");
    if (!ctx.match(q).hasMatch()) return out;

    // N(1200, 50) ou N(1200; 50)
    static const QRegularExpression nform(R"(\bn\s*\(\s*(-?\d+(?:[.,]\d+)?)\s*[,;]\s*(\d+(?:[.,]\d+)?)\s*\))");
    auto it = nform.globalMatch(q);
    while (it.hasNext() && out.size() < 6) {
        const auto m = it.next();
        QJsonObject o;
        o["age"] = text::toNumber(m.captured(1));
        o["error"] = text::toNumber(m.captured(2));
        o["mode"] = "none";
        if (o["error"].toDouble() > 0) out << o;
    }
    if (!out.isEmpty()) return out;

    for (const text::ValueError& ve : text::findValueErrors(q)) {
        if (out.size() >= 6) break;
        QJsonObject o;
        o["age"] = ve.value;
        o["error"] = ve.error;
        o["mode"] = "none";
        out << o;
    }
    return out;
}

MethodResult GaussMethod::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble();
    const double error = p.value("error").toDouble();
    const QString mode = p.value("mode").toString("none");
    if (!(error > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");

    double tmin, tmax, step;
    std::function<double(double)> g;

    if (mode == "eq") {
        const double a = p.value("a").toDouble(), b = p.value("b").toDouble(), c = p.value("c").toDouble();
        g = [=](double t) { return a * std::abs(t) * std::abs(t) + b * t + c; };

        const auto s1 = solveQuadratic(age - kSigma * error, a, b, c);
        const auto s2 = solveQuadratic(age + kSigma * error, a, b, c);
        if (std::isinf(s1.first) || std::isinf(s2.first))
            return MethodResult::failure("équation de calibration constante : domaine indéterminé");
        tmin = std::min(s1.first, s2.first);
        tmax = std::max(s1.second, s2.second);
        const double sp = std::min(std::abs(s2.first - s1.first), std::abs(s2.second - s1.second));
        step = (sp > 0.0 ? sp : (tmax - tmin)) / 1001.0;
        if (!(tmax > tmin)) return MethodResult::failure("l'équation de calibration n'a pas de solution réelle dans la plage de la mesure");
    } else {
        g = [](double t) { return t; };
        tmin = age - kSigma * error;
        tmax = age + kSigma * error;
        step = 2.0 * error / 1001.0;
    }

    const double var = error * error;
    const auto L = [&](double t) -> long double {
        const long double d = age - g(t);
        return expl(-0.5L * d * d / var) / sqrtl(var);
    };
    return MethodResult::fromDensity(buildDensity(tmin, tmax, step, L, true), {}, false);
}

QString GaussMethod::describe(const QJsonObject& p) const
{
    auto n = [](double v) { return text::numAuto(v, 3); };
    QString s = QStringLiteral("Gauss : N(%1 ; %2)").arg(n(p.value("age").toDouble()), n(p.value("error").toDouble()));
    if (p.value("mode").toString() == "eq")
        s += QStringLiteral(" — calibration g(t) = %1·t² + %2·t + %3")
                 .arg(n(p.value("a").toDouble()), n(p.value("b").toDouble()), n(p.value("c").toDouble()));
    else
        s += QStringLiteral(" — sans calibration (année calendaire signée)");
    return s;
}

} // namespace seldon
