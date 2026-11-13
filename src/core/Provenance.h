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

// Provenance des nombres : le modèle de langage est un INTERMÉDIAIRE entre l'utilisateur et les outils de SeldonH. Il ne
// doit ni inventer une mesure qu'il passe à un outil, ni écrire un résultat qu'aucun outil n'a produit. Ces fonctions le
// vérifient sur le texte (sans modèle de langage, déterministe, testable seul) :
//   - checkRequest   : valeurs d'une demande de calcul écrite par le modèle (bloc ```calcul ou appel d'outil) qui ne
//                      figurent pas dans le texte de l'utilisateur (questions, pièces jointes, tableau) ;
//   - unsourcedNumbers : nombres de la réponse du modèle qui ne figurent dans rien de ce qu'il a reçu (messages de
//                      l'utilisateur, résultats de SeldonH, fiches), donc calculés de tête ou inventés.
// Formes dérivées acceptées : ka / Ma (× 10³, × 10⁶ et inverse), années av. J.-C. (1 − N), cal BP ↔ année (1950 − N),
// b2k (2000 − N), signe de l'année. Une recopie à l'identique d'une valeur reçue passe toujours.

#ifndef SELDON_PROVENANCE_H
#define SELDON_PROVENANCE_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {
namespace provenance {

// Ensemble des nombres d'un texte (et de leurs formes dérivées), pour savoir si une valeur « vient » de ce texte.
class NumberPool
{
public:
    explicit NumberPool(const QString& text = QString(), bool derived = true);
    void add(const QString& text);
    bool contains(double v) const;
    bool isEmpty() const { return values_.isEmpty(); }
private:
    void insert(double v);
    QVector<double> values_;       // trié à la fin de chaque add()
    bool derived_ = true;
};

struct RequestCheck {
    // Mesures (âge, erreur, F14C, inc, dec, α95, intensité, année de mesure, ΔR, année visée d'une calibration inverse…)
    // absentes du texte de l'utilisateur : le calcul NE DOIT PAS être lancé (résultat sans fondement).
    QStringList measures;
    // Bornes et paramètres (min / max d'une uniforme, période d'étude, paramètres d'une loi, wiggle) absents du texte de
    // l'utilisateur : le calcul est lancé, avec un avertissement (le modèle les a peut-être tirés d'un autre résultat).
    QStringList bounds;
    bool ok() const { return measures.isEmpty(); }
};

// Valeurs d'une demande de calcul (objet JSON, imbrications comprises : dates d'un Event, termes d'une relation…)
// comparées aux nombres du texte de l'utilisateur. Une datation sans valeur (nom seul, repris d'un .chr, ligne du
// tableau, colonne de traces) ne pose pas de problème.
RequestCheck checkRequest(const QJsonObject& request, const NumberPool& userNumbers);

// Nombres de la réponse (hors blocs de code, déjà vérifiés ailleurs) absents de `received`. Seuls les nombres qui
// peuvent porter un résultat sont examinés : années et âges (|v| ≥ 100), nombres décimaux, pourcentages ; pas les
// petits entiers (numéros, « 2 dates », « 3 chaînes »), ni les références (« Ly-8812 », « IntCal20 », « Q1 »).
// Renvoie chaque nombre une fois, tel qu'écrit dans la réponse.
QStringList unsourcedNumbers(const QString& answer, const NumberPool& received);

// Phrases d'avertissement (affichage dans le dialogue, consigne au modèle).
QString blockedMessage(const QStringList& measures);
QString boundsMessage(const QStringList& bounds);

} // namespace provenance
} // namespace seldon

#endif // SELDON_PROVENANCE_H
