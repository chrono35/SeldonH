/* ---------------------------------------------------------------------

Copyright © CNRS	2026

Authors :
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later
--------------------------------------------------------------------- */

// Calculator : petite calculatrice locale de SeldonH. Une question qui n'est qu'un calcul (« 2+3 », « sqrt(30^2+40^2) »,
// « a = 3560 » puis « ADtoBP(a) ») est calculée ici, sans le modèle de langage.
//
//   nombres      12  1.5  1,5  1e3  2,5E-2 ; dans une fonction, « , » sépare les arguments sauf s'ils sont séparés par
//                « ; » : moyenne(15,6; 12) a deux arguments, 15,6 et 12 ; max(1,5) en a deux, 1 et 5
//   opérateurs   + - * / ^ (puissance, aussi **), × ÷, parenthèses ; moins unaire : -2^2 = -4
//   fonctions    sqrt racine exp ln log log10 abs round(x[;n]) arrondi floor ceil min max (plusieurs arguments)
//                sin cos tan asin acos atan atan2(y;x) en degrés ; rad(x) deg(x) ; sinr cosr tanr en radians
//   constantes   pi (π), e ; ans = dernier résultat
//   variables    nom = expression (gardées dans la conversation)
//   dates        AD = année calendaire signée (BC/AD de ChronoModel, 0 = 1 av. J.-C.) :
//                ADtoBP BPtoAD (âge cal BP = 1950 − AD), ADtoB2K B2KtoAD (2000 − AD), BPtoB2K B2KtoBP,
//                ADtoKa KatoAD (−AD / 1000), ADtoMa MatoAD, ADtoBC BCtoAD (an av. J.-C. = 1 − AD)
//   radiocarbone C14toF14C(âge [; erreur]) F14CtoC14(F14C [; erreur]) : 8033 = 5568/ln 2, erreur comme ChronoModel
//   archéomag    VGP(lat; lon; D; I [; α95]) pôle géomagnétique virtuel (lat, lon, dp, dm) ; VGPtoDI(latP; lonP; lat; lon)
//                ReducVGP(lat; lon; D; I; latRef; lonRef) réduction de D et I par le VGP ; IGAD(lat) inclinaison du
//                dipôle axial centré ; ReducGAD(I; lat; latRef) réduction de I par le GAD ; VADM(F [; σF]; lat),
//                VDM(F [; σF]; I) en 10²² A·m² ; VADMtoF(m; lat) ; ReducVADM(F; lat; latRef) réduction de F.
//                Latitudes nord et longitudes est positives, angles en degrés, F en µT.
//   plusieurs résultats : la valeur principale sert dans le calcul ; les autres sont affichées et, si le résultat est
//                affecté (« p = VGP(…) »), gardées dans des variables p_lon, p_dp… (voir Line::parts).
//   incertitudes 2500 ± 30, 2500 +- 30 ou 2500 +/- 30 : l'erreur suit les calculs au premier ordre, ERREURS SUPPOSÉES
//                INDÉPENDANTES (σ² = Σ (∂f/∂xᵢ σᵢ)² ; a − a donne 0 ± σ√2). « ± » a la priorité de + et − : 2*310 ± 50 = 620 ± 50,
//                2*(310 ± 50) = 620 ± 100. val(x), err(x) : valeur et erreur.
//   statistiques moyenne mediane ecart_type somme (mean median sd sum) sur une liste ; moy_ponderee (wmean) :
//                moyenne pondérée par 1/σ² et test du χ² de Ward et Wilson (1978) au seuil de 5 %.
//   séparateurs  une instruction par ligne ou séparées par « ; » ; arguments d'une fonction séparés par « ; » ou « , »
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {

class Calculator {
public:
    struct Value { double v = 0.0; double e = 0.0; };   // valeur et incertitude (1σ, 0 = exacte)

    struct Line {
        QString expr;          // instruction telle qu'écrite
        bool    ok = false;
        double  value = 0.0;
        double  uncertainty = 0.0;   // 1σ propagée (0 = valeur exacte)
        QString variable;      // nom affecté (« a = … »), vide sinon
        QString error;         // en français si !ok
        bool    syntax = false; // erreur de syntaxe ou nom inconnu (le texte n'est pas un calcul)
        // Fonction à plusieurs résultats (dernier appel de l'instruction) : (nom, valeur), le premier est la valeur
        // principale ; `details` les présente (« F14C = 0,73255 ± 0,00273 »).
        QVector<QPair<QString, double>> parts;
        QString details;
    };

    // Le texte entier est-il un calcul ? (éventuellement précédé de « calcule », « combien font », « que vaut »… et suivi
    // de « = » ou « ? »). Faux pour une datation (« 3560 +- 20 BP »), une phrase, un nombre seul.
    bool isCalculation(const QString& text) const;

    // Évalue chaque instruction (lignes ou « ; »). Les variables et `ans` sont mis à jour.
    QVector<Line> evaluate(const QString& text);

    void reset() { vars_.clear(); ans_ = Value{}; hasAns_ = false; }
    const QHash<QString, Value>& variables() const { return vars_; }

    // Nombre affiché : 12 chiffres significatifs au plus, virgule décimale, sans zéros inutiles.
    static QString format(double v);
    // Incertitude à 2 chiffres significatifs ; valeur ± incertitude arrondies à la même décimale (« 620 ± 100 »).
    static QString formatErr(double e);
    static QString formatValue(double v, double e);
    // Noms des fonctions reconnues (minuscules).
    static QStringList functionNames();

private:
    struct Eval;                                   // analyseur (descente récursive)
    static QStringList splitStatements(const QString& text);
    static QString stripPrompt(const QString& text);

    QHash<QString, Value> vars_;
    Value  ans_;
    bool   hasAns_ = false;
};

} // namespace seldon
