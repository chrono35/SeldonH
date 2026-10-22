// Méthode « TL/OSL » : luminescence (plugin_tl de ChronoModel).  Âge en années avant l'année de mesure :
//   L(t) = exp( -½ ((age - (refYear - t)) / σ)² ) / σ ,   domaine : refYear - age ± 5σ
#pragma once

#include "../common/Method.h"

namespace seldon {

class TlMethod : public Method {
public:
    QString id() const override { return QStringLiteral("tl"); }
    QString title() const override { return QStringLiteral("TL / OSL"); }
    QVector<QJsonObject> parse(const QString& question) const override;
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
};

} // namespace seldon
