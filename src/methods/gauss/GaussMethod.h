// Méthode « Gauss » : mesure gaussienne N(age, error), sans calibration (mode "none") ou avec une équation
// de calibration g(t) = a t² + b t + c (mode "eq").  Formule de ChronoModel (plugin_gauss) :
//   L(t) = exp( -½ (age - g(t))² / error² ) / error      (g(t) = t sans calibration)
// Domaine : age ± 5 σ (mode none) ou intervalle où g(t) ∈ age ± 5 σ (mode eq).
#pragma once

#include "../common/Method.h"

namespace seldon {

class GaussMethod : public Method {
public:
    QString id() const override { return QStringLiteral("gauss"); }
    QString title() const override { return QStringLiteral("Gauss"); }
    QVector<QJsonObject> parse(const QString& question) const override;
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
};

} // namespace seldon
