#include "TlMethod.h"

#include <QDate>
#include <cmath>

#include "../common/TextUtil.h"

namespace seldon {

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression TlMethod::context() const
{
    static const QRegularExpression re(R"((\btl\b|\bosl\b|\birsl\b|thermoluminescen|luminescence|\bt\.l\.))");
    return re;
}

QVector<QJsonObject> TlMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);
    const QRegularExpression ctx = context();
    if (!ctx.match(q).hasMatch()) return out;

    static const QRegularExpression refRe(
        R"((?:annee\s+(?:de\s+)?(?:reference|mesure)|mesure[e]?\s+en|reference\s*[:=]?|refyear\s*[=:]?)\s*[:=]?\s*(\d{4}))");
    const auto rm = refRe.match(q);

    for (const text::ValueError& ve : text::findValueErrors(q)) {
        if (out.size() >= 6) break;
        double scale = 1.0;
        const QString after = q.mid(ve.start + ve.length, 12);
        if (QRegularExpression(R"(^\s*(ka|k\.a\.|milliers))").match(after).hasMatch()) scale = 1000.0;
        QJsonObject o;
        o["age"] = ve.value * scale;
        o["error"] = ve.error * scale;
        if (rm.hasMatch()) {
            o["refYear"] = rm.captured(1).toInt();
        } else {
            o["refYear"] = QDate::currentDate().year();
            o["refYearAssumed"] = true;
        }
        out << o;
    }
    return out;
}

MethodResult TlMethod::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble(), err = p.value("error").toDouble();
    const double ref = p.value("refYear").toDouble();
    if (!(err > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");

    const double centre = ref - age;
    const double tmin = centre - 5.0 * err, tmax = centre + 5.0 * err;
    const double step = std::abs(tmax - tmin) / 1001.0;
    const auto L = [&](double t) -> long double {
        const long double norm = (age - (ref - t)) / err;
        return std::exp(-0.5L * norm * norm) / err;
    };
    QStringList warn;
    if (p.value("refYearAssumed").toBool())
        warn << QStringLiteral("année de mesure non précisée : %1 supposée (à indiquer, par ex. « mesuré en 2015 »)").arg(int(ref));
    return MethodResult::fromDensity(buildDensity(tmin, tmax, step, L, true), warn, false);
}

QString TlMethod::describe(const QJsonObject& p) const
{
    return QStringLiteral("TL/OSL : %1 ± %2 ans avant %3")
        .arg(text::numAuto(p.value("age").toDouble(), 1), text::numAuto(p.value("error").toDouble(), 1),
             QString::number(p.value("refYear").toInt()));
}

} // namespace seldon
