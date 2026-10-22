// CurveStore : charge (une seule fois) les courbes de référence intégrées aux ressources Qt.
// Elles vivent dans le dossier de chaque méthode : :/methods/<id>/curves/<nom>.<ext>
#pragma once

#include <QString>
#include <QStringList>

#include "RefCurve.h"

namespace seldon {

enum class CurveFormat { Radiocarbon, Simple };

class CurveStore {
public:
    // Noms (sans extension, en minuscules) des courbes d'une méthode.
    static QStringList names(const QString& methodId);

    // Retourne nullptr si absente ou illisible. `asF14C` n'a de sens que pour le format Radiocarbon.
    static const RefCurve* get(const QString& methodId, const QString& name, CurveFormat fmt, bool asF14C = false);
};

} // namespace seldon
