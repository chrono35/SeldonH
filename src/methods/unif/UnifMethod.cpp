#include "UnifMethod.h"

#include <cmath>

#include "../common/TextUtil.h"

namespace seldon {

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression UnifMethod::context() const
{
    static const QRegularExpression re(R"((uniforme|\bunif\s*\())");
    return re;
}

QVector<QJsonObject> UnifMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);
    if (!q.contains("uniforme") && !QRegularExpression(R"(\bunif\s*\()").match(q).hasMatch()) return out;

    const QString num = QStringLiteral("(-?\\d+(?:[.,]\\d+)?)");
    const QRegularExpression forms[] = {
        QRegularExpression(QStringLiteral("(?:unif\\w*|\\bu)\\s*\\(\\s*") + num + QStringLiteral("\\s*[,;]\\s*") + num + QStringLiteral("\\s*\\)")),
        QRegularExpression(QStringLiteral("entre\\s+") + num + QStringLiteral("\\s+et\\s+") + num),
        QRegularExpression(QStringLiteral("de\\s+") + num + QStringLiteral("\\s+(?:a|jusqu'a)\\s+") + num),
        QRegularExpression(QStringLiteral("min\\w*\\s*[=:]?\\s*") + num + QStringLiteral(".{0,20}?max\\w*\\s*[=:]?\\s*") + num),
    };
    for (const QRegularExpression& re : forms) {
        auto it = re.globalMatch(q);
        while (it.hasNext() && out.size() < 6) {
            const auto m = it.next();
            double a = text::toNumber(m.captured(1)), b = text::toNumber(m.captured(2));
            if (a > b) std::swap(a, b);
            QJsonObject o;
            o["min"] = a;
            o["max"] = b;
            out << o;
        }
        if (!out.isEmpty()) break;
    }
    return out;
}

MethodResult UnifMethod::compute(const QJsonObject& p) const
{
    const double mn = p.value("min").toDouble(), mx = p.value("max").toDouble();
    if (!(mx > mn)) return MethodResult::failure("il faut min < max");
    const double w = 1.0 / (mx - mn);
    const auto L = [&](double t) -> long double { return (t >= mn && t <= mx) ? w : 0.0; };
    // La grille couvre exactement [min, max] ; pas non tronqué (densité constante).
    return MethodResult::fromDensity(buildDensity(mn, mx, (mx - mn) / 51.0, L, false), {}, false);
}

QString UnifMethod::describe(const QJsonObject& p) const
{
    return QStringLiteral("Uniforme : de %1 à %2 (années calendaires signées)")
        .arg(text::numAuto(p.value("min").toDouble(), 3), text::numAuto(p.value("max").toDouble(), 3));
}

} // namespace seldon
