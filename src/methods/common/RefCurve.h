// RefCurve : courbe de référence (calibration, archéomagnétisme...), sans dépendance Qt.
//
// Port simplifié de ChronoModel (CNRS, GPL-3.0-or-later) : src/plugins/RefCurve.*, PluginAbstract.h et
// les fonctions loadRefFile() des plugins 14C, F14C et AM. Le temps est toujours en ANNÉES CALENDAIRES
// signées (année = 1950 - BP), croissant.
#pragma once

#include <string>
#include <vector>

namespace seldon {

struct RefCurve {
    std::string         name;
    std::vector<double> t;        // années calendaires, croissantes
    std::vector<double> mean;
    std::vector<double> err;      // écart-type (1 sigma)
    double minStep = 0.0;         // plus petit pas entre deux nœuds
    double supMax = 0.0;          // max de mean + 1,96 err
    double infMin = 0.0;          // min de mean - 1,96 err

    bool   valid() const { return t.size() >= 2 && mean.size() == t.size() && err.size() == t.size(); }
    double tmin() const { return t.empty() ? 0.0 : t.front(); }
    double tmax() const { return t.empty() ? 0.0 : t.back(); }

    // Valeur et erreur interpolées linéairement. Hors domaine : mêmes conventions que ChronoModel
    // (valeur moyenne de l'enveloppe, erreur énorme -> vraisemblance plate).
    void at(double year, double& m, double& e) const;

    // Calcule minStep, supMax, infMin à partir de t/mean/err.
    void finalize();
};

// Conversions âge conventionnel <-> fraction F14C (demi-vie de Libby : 8033 = 5568/ln2).
double craToF14C(double cra);
double errCraToErrF14C(double errCra, double cra);
double f14cToCra(double f14c);
double errF14CToErrCra(double errF14c, double f14c);

// Fichiers de type IntCal/Marine/SHCal/bomb (*.14c) : en-tête libre commençant par #, //, ! ou ".
// La colonne de temps est en BP ou en années (AD/BC) selon l'en-tête ; les données sont des âges 14C
// ou des F14C. `asF14C` choisit l'espace de sortie (conversion si nécessaire).
bool parseRefCurve14C(const std::string& text, bool asF14C, RefCurve& out, std::string* err = nullptr);

// Fichiers simples « année, valeur, erreur » (courbes archéomagnétiques .ref, CSV de référence).
// Séparateur , ; ou tabulation ; lignes commençant par //, # ou ! ignorées ; colonnes supplémentaires ignorées.
bool parseRefCurveSimple(const std::string& text, RefCurve& out, std::string* err = nullptr);

} // namespace seldon
