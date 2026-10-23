// CurveStore : charge (une seule fois par courbe) les courbes de référence.
// Ordre de recherche, le premier trouvé l'emporte :
//   1. dossiers ajoutés par setExtraRoots() — dossier des courbes de l'utilisateur, puis dossier « Calib » de
//      ChronoModel — avec un sous-dossier par méthode, comme ChronoModel : 14C, F14C, AM ;
//   2. courbes intégrées aux ressources Qt : :/methods/<id>/curves/<nom>.<ext>.
// Extensions : .14c pour c14 et f14c, .ref pour am (casse indifférente). Nom d'une courbe = nom du fichier sans
// extension, en minuscules.
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

    // Dossiers racines supplémentaires, par priorité décroissante (chacun contient 14C/, F14C/, AM/). Vide le cache :
    // une courbe modifiée ou ajoutée est relue au prochain calcul.
    static void setExtraRoots(const QStringList& roots);
    static QStringList extraRoots();
    // Sous-dossier d'une méthode dans un dossier racine (« 14C », « F14C », « AM »), comme dans ChronoModel.
    static QString subdir(const QString& methodId);
    // Fichier d'où vient une courbe (chemin, ou « :/… » pour une courbe intégrée) ; vide si inconnue.
    static QString sourceOf(const QString& methodId, const QString& name);
    // Nom d'une courbe connue de la méthode écrit dans un texte (minuscules, sans accents), en mot entier ; le plus
    // long l'emporte (« intcal98_fix » plutôt que « intcal98 »). `pos` reçoit sa position. Vide si aucun.
    static QString findNamed(const QString& methodId, const QString& foldedText, int* pos = nullptr);
};

} // namespace seldon
