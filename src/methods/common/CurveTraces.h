/* ---------------------------------------------------------------------

Copyright © CNRS	2014 - 2026

Authors :
	Philippe LANOS
	Helori LANOS
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This file contains dating methods derived from ChronoModel
(https://chronomodel.com), distributed under the same license.

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

// CurveTraces : courbes (splines) estimées par ChronoModel en mode « courbe » (1, 2 ou 3 composantes en fonction du
// temps : X ; X, Y ; X, Y, Z ou inclinaison, déclinaison, intensité), lues dans les exports de ChronoModel.
//
// 1. Trace des splines (ModelCurve::saveSplinesToFile, « Curve_X_Splines.csv ») : une ligne par tirage d'acquisition,
//    un bloc de colonnes par nœud k : Theta_k (date réelle, BC/AD), Theta_reduced_k (temps réduit), Gx_k, Gamma_x_k
//    (valeur et dérivée seconde de la spline au nœud, en temps réduit), puis Gy_k, Gamma_y_k, Gz_k, Gamma_z_k s'il y a
//    d'autres composantes. Les nœuds sont triés par date dans chaque tirage. Temps réduit = (t − tmin) / (tmax − tmin)
//    sur la période d'étude, retrouvée à partir des deux colonnes Theta et Theta_reduced.
//    Évaluation entre les nœuds et prolongement linéaire au-delà : port de valeurs_G_GP_GS (ChronoModel,
//    src/curve/CurveUtilities.cpp, version 4) ; G′ et G″ ramenés en unités par an. Calcul exact, tirage par tirage.
//
// 2. Carte de densité (ModelCurve::saveMapToFile, « Curve_X_Map.csv » pour G, « Curve_X_GP_Map.csv » pour G′) :
//    en-tête « Y / Date » (ou « Y / Age ») puis les dates de la grille ; une ligne par valeur de Y, de la plus grande à
//    la plus petite ; chaque case = nombre de tirages dont la spline passe dans cette case (case la plus proche, les
//    valeurs hors de la fenêtre de la carte ne sont pas comptées). Les étiquettes Y sont écrites ARRONDIES par
//    ChronoModel 4.0 (stringForCSV sans pleine précision) : la grille est reconstruite entre la première et la dernière
//    étiquette ; si elles sont égales (carte de G′ : toutes à 0), la carte n'est pas exploitable.
//    Une carte ne donne que la loi de G(t) à un instant donné (une colonne) : pas les liens entre instants (vitesse
//    moyenne, date d'atteinte d'une valeur), qui demandent la trace des splines.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

namespace seldon {

struct SplineTrace {
    QString file;
    QString csvFormat;                        // séparateur et décimale repérés
    int dims = 0;                             // composantes : 1 (X), 2 (X, Y) ou 3 (X, Y, Z)
    int knots = 0;                            // nœuds par tirage (= nombre d'Events de la courbe)
    size_t rows = 0;                          // tirages
    double tmin = 0.0, span = 1.0;            // période d'étude : temps réduit = (t − tmin) / span
    std::vector<double> theta;                // rows × knots, dates réelles des nœuds (BC/AD)
    std::vector<double> tr;                   // rows × knots, temps réduit
    std::vector<double> g[3], gamma[3];       // rows × knots, par composante
    // Mode vecteur de l'archéomagnétisme (ChronoModel : eProcess_Vector / eProcess_Spherical) : les trois splines sont
    // les composantes cartésiennes x = F cos I cos D, y = F cos I sin D, z = F sin I ; I, D (degrés) et F en sont tirés
    // comme dans ChronoModel (convertToIDF, computeDerivatives). Reconnu au nom du fichier (« Curve_Inc_… ») avec 3
    // composantes. Sinon les composantes sont directes (X, Y, Z ; ou I, F pour une déclinaison inconnue).
    bool vector = false;
    QStringList names;                        // noms des composantes : « X », « Y », « Z » ou « I », « D », « F »
    double tmax() const { return tmin + span; }
    // Composante désignée par un nom (X/Y/Z, I/D/F, inclinaison, déclinaison, intensité, 1/2/3) ; -1 si inconnue.
    int dimOf(const QString& name) const;
    QString unitOf(int dim) const;            // « ° » pour I et D, vide sinon
    // Valeur G et dérivée G′ (unités par an) de la composante `dim` au tirage `row`, à la date t (BC/AD).
    void eval(size_t row, int dim, double t, double& G, double& GP) const;
    // Spline brute `comp` (composante enregistrée, cartésienne en mode vecteur).
    void evalRaw(size_t row, int comp, double t, double& G, double& GP) const;
    // Avec la dérivée seconde G″ (accélération, unités par an²) : nulle hors des nœuds (prolongement linéaire).
    void eval2(size_t row, int dim, double t, double& G, double& GP, double& GS) const;
    void evalRaw2(size_t row, int comp, double t, double& G, double& GP, double& GS) const;
    double value(size_t row, int dim, double t) const { double G, GP; eval(row, dim, t, G, GP); return G; }
    // Grandeur d'ordre `order` : 0 = G (valeur), 1 = G′ (vitesse, par an), 2 = G″ (accélération, par an²).
    double quantity(size_t row, int dim, double t, int order) const;
};

struct CurveMap {
    QString file;
    QString component;                        // « X », « Y », « Z » (d'après le nom du fichier), vide si inconnu
    bool derivative = false;                  // carte de G′ (« _GP_ ») : vitesse, en unités par an
    QString dateFormat;                       // ligne « # Date Format : … »
    std::vector<double> t;                    // dates de la grille (BC/AD), croissantes
    std::vector<double> y;                    // valeurs de la grille, croissantes
    std::vector<double> counts;               // counts[c * y.size() + r] : tirages dans la case (t[c], y[r])
    bool usable = true;                       // faux si la grille des valeurs n'a pas pu être reconstruite
    QStringList notes;                        // avertissements de lecture
    double at(size_t c, size_t r) const { return counts[c * y.size() + r]; }
};

namespace curves {

// Nom de la composante : 0 = X (ou inclinaison), 1 = Y (déclinaison), 2 = Z (intensité) ; -1 si inconnu.
int dimIndex(const QString& name);
QString dimName(int dim);                     // « X », « Y », « Z »

bool looksLikeSplines(const QString& text);
// En cas d'échec, `error` reçoit la raison ; en cas de succès, les remarques de lecture (vide s'il n'y en a pas).
bool parseSplines(const QString& text, const QString& fileName, SplineTrace& out, QString* error = nullptr);
bool looksLikeMap(const QString& text);
bool parseMap(const QString& text, const QString& fileName, CurveMap& out, QString* error = nullptr);

// Toutes les dates t de [a ; b] où la spline (tirage `row`, composante `dim`) vaut y, de la plus ancienne à la plus
// récente : recherche sur une grille de `steps` pas, puis bissection.
// `order` : 0 = la courbe G, 1 = sa vitesse G′ (par an), 2 = son accélération G″ (par an²).
std::vector<double> crossings(const SplineTrace& s, size_t row, int dim, double y, double a, double b, int steps = 2000, int order = 0);

// Description pour le modèle de langage et la fiche.
QString summary(const SplineTrace& s);
QString summary(const CurveMap& m);

// Courbes chargées dans la session (pièces jointes) : un fichier remplace celui de même nom.
void add(const SplineTrace& s);
void add(const CurveMap& m);
void clear();
const QVector<SplineTrace>& splines();
const QVector<CurveMap>& maps();
bool anyLoaded();

} // namespace curves
} // namespace seldon
