#include "C14Method.h"

#include <cmath>

#include "../common/CurveStore.h"
#include "../common/TextUtil.h"

namespace seldon {

QStringList C14Method::curveNames() const { return CurveStore::names(id()); }

QString C14Method::curveTitle(const QString& name)
{
    QString n = name;
    if (n.startsWith("intcal")) return "IntCal" + n.mid(6);
    if (n.startsWith("shcal")) return "SHCal" + n.mid(5);
    if (n.startsWith("marine")) return "Marine" + n.mid(6);
    if (n == "uwsy98") return "UWSY98";
    return name;
}

QString C14Method::defaultCurve(const QString& q)
{
    static const QRegularExpression explicitName(R"((intcal|shcal|marine)\s*-?\s*(20|13|09|04))");
    const auto m = explicitName.match(q);
    if (m.hasMatch()) return m.captured(1) + m.captured(2);

    QString family = "intcal";
    if (q.contains("marine") || q.contains("marin ") || q.contains("marins") || q.contains("coquille")
        || q.contains("poisson") || q.contains("mammifere marin") || q.contains("reservoir"))
        family = "marine";
    else if (q.contains("shcal") || q.contains("hemisphere sud") || q.contains("hemisphere austral")
             || q.contains("australe") || q.contains("sud de l'equateur"))
        family = "shcal";
    return family + "20";
}

QVector<QJsonObject> C14Method::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);

    // Contexte radiocarbone obligatoire : évite de prendre n'importe quel « 120 ± 5 » pour une date 14C.
    static const QRegularExpression ctx(
        R"((c\s?-?14|14\s?-?c|carbone\s*14|radiocarbon|\bbp\b|calibr|intcal|shcal|marine\s?(?:20|13|09|04)))");
    if (!ctx.match(q).hasMatch()) return out;

    const QString curve = defaultCurve(q);

    // ΔR (réservoir)
    static const QRegularExpression dr(
        QStringLiteral("(?:delta\\s*r|\\bdr\\b|δr)\\s*[=:]?\\s*(-?\\d+(?:[.,]\\d+)?)(?:\\s*") + text::plusMinus()
        + QStringLiteral("\\s*(\\d+(?:[.,]\\d+)?))?"));
    const auto dm = dr.match(q);
    QString scan = q;
    if (dm.hasMatch()) scan.remove(dm.capturedStart(), dm.capturedLength());

    for (const text::ValueError& ve : text::findValueErrors(scan)) {
        if (ve.value < 0 || out.size() >= 6) continue;
        QJsonObject o;
        o["age"] = ve.value;
        o["error"] = ve.error;
        o["curve"] = curve;
        if (dm.hasMatch()) {
            o["deltaR"] = text::toNumber(dm.captured(1));
            o["deltaRerr"] = dm.captured(2).isEmpty() ? 0.0 : text::toNumber(dm.captured(2));
            o["hasDeltaR"] = true;
        }
        out << o;
    }
    return out;
}

MethodResult C14Method::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble();
    const double error = p.value("error").toDouble();
    const double dR = p.value("deltaR").toDouble();
    const double dRerr = p.value("deltaRerr").toDouble();
    const QString name = p.value("curve").toString("intcal20").toLower();

    if (!(error > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");
    const RefCurve* c = CurveStore::get(id(), name, CurveFormat::Radiocarbon, false);
    if (!c) return MethodResult::failure(QStringLiteral("courbe de calibration inconnue : « %1 » (disponibles : %2)")
                                             .arg(name, curveNames().join(", ")));

    const double a = age - dR;
    const double meas2 = error * error + dRerr * dRerr;

    const auto L = [&](double t) -> long double {
        double mu, sg;
        c->at(t, mu, sg);
        const long double v = static_cast<long double>(sg) * sg + meas2;
        const long double d = a - mu;
        return expl(-0.5L * d * d / v) / sqrtl(v);
    };

    const Density d = buildDensity(c->tmin(), c->tmax(), c->minStep / 3.0, L, true);
    QStringList warn;
    if (name.startsWith("marine") && !p.value("hasDeltaR").toBool())
        warn << "courbe marine sans ΔR : la correction réservoir régionale n'est pas précisée (ΔR = 0 par défaut)";
    if (age < 0) warn << "âge conventionnel négatif : échantillon post-bombe, utiliser plutôt la méthode F14C";
    return MethodResult::fromDensity(d, warn, true);
}

QString C14Method::describe(const QJsonObject& p) const
{
    auto n = [](double v) { return text::numAuto(v, 1); };
    QString s = QStringLiteral("14C : %1 ± %2 BP — %3")
                    .arg(n(p.value("age").toDouble()), n(p.value("error").toDouble()),
                         curveTitle(p.value("curve").toString("intcal20")));
    if (p.value("hasDeltaR").toBool())
        s += QStringLiteral(", ΔR = %1 ± %2").arg(n(p.value("deltaR").toDouble()), n(p.value("deltaRerr").toDouble()));
    return s;
}

} // namespace seldon
