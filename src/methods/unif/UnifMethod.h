// Méthode « Unif » : date uniforme entre deux années calendaires (plugin_uniform de ChronoModel) :
//   L(t) = 1/(max - min) si min ≤ t ≤ max, sinon 0 ; pas de calcul = (max - min)/51.
#pragma once

#include "../common/Method.h"

namespace seldon {

class UnifMethod : public Method {
public:
    QString id() const override { return QStringLiteral("unif"); }
    QString title() const override { return QStringLiteral("Uniforme"); }
    QVector<QJsonObject> parse(const QString& question) const override;
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
};

} // namespace seldon
