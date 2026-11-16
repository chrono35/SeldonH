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

// Arbre de décision (12/11/2026) : au lieu de laisser le modèle de langage choisir librement un outil, SeldonH lui pose des
// questions FERMÉES, une par nœud (« Que demande l'utilisateur ? », « Le modèle décrit-il une courbe ? »…). Chaque choix
// mène à un autre nœud ou à une feuille : outils, fiches de compétences, action de SeldonH. L'arbre est un fichier de
// données intégré (resources/decision/arbre.json), lu ici et vérifié : structure, outils et fiches existants, et chaque
// outil de calcul branché à au moins une feuille. En local, la réponse est forcée par une grammaire GBNF ; en distant, elle
// est lue dans le texte (`parseAnswer`). QtCore seul, sans modèle de langage : testable.

#ifndef SELDON_DECISIONTREE_H
#define SELDON_DECISIONTREE_H

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {
namespace decision {

struct Leaf {
    QStringList tools;       // outils de calcul (noms de tools::all())
    QStringList skills;      // fiches de compétences (identifiants)
    QString     action;      // action de SeldonH : modele, journal, aide, libre… (vide : outils seuls)
    QJsonObject params;      // autres clés de la feuille (« courbe », « strati »…)
};

struct Choice {
    QString id;              // minuscules ASCII : ce que le modèle répond
    QString label;           // ce que le modèle et l'utilisateur lisent
    QString next;            // nœud suivant, ou vide pour une feuille
    Leaf    leaf;
    Leaf    adds;            // « ajoute » : fiches et paramètres apportés en chemin (type de courbe avant « stratigraphie ? »)
    bool isLeaf() const { return next.isEmpty(); }
};

struct Node {
    QString id;
    QString title;           // titre court montré à l'utilisateur (« courbe », « stratigraphie »)
    QString question;
    QVector<Choice> choices;
    const Choice* choice(const QString& id) const;
};

class Tree {
public:
    // Lit le JSON ; faux (et `error`) si le fichier est illisible ou mal formé (la cohérence est vérifiée par validate()).
    bool load(const QByteArray& json, QString* error = nullptr);
    // Arbre intégré à l'application (:/decision/arbre.json), lu une fois.
    static const Tree& builtin();

    QString root() const { return root_; }
    const Node* node(const QString& id) const;
    QStringList nodeIds() const { return order_; }

    // Problèmes de cohérence (vide = arbre valide) : racine, suites inexistantes, nœuds inaccessibles, cycles, identifiants
    // de choix invalides ou en double, choix sans suite ni feuille, outils ou fiches inconnus (si les listes sont données).
    QStringList validate(const QStringList& knownTools = {}, const QStringList& knownSkills = {}) const;
    // Outils de `tools` qui ne sont dans aucune feuille.
    QStringList uncoveredTools(const QStringList& tools) const;
    // Chemins (suite d'identifiants « nœud=choix ») qui mènent à une feuille contenant l'outil ; vide s'il n'y en a pas.
    QStringList pathsTo(const QString& tool) const;

private:
    QString root_;
    QHash<QString, Node> nodes_;
    QStringList order_;
};

// Ce qu'apporte un chemin (couples nœud, choix, de la racine à la feuille) : fiches, outils et paramètres des « ajoute »
// rencontrés, puis ceux de la feuille (une clé de la feuille l'emporte) ; action de la feuille. Fiches sans doublon.
Leaf accumulate(const Tree& t, const QVector<QPair<QString, QString>>& path);

// Le texte parle d'une courbe (mode courbe de ChronoModel) : courbe, inclinaison, déclinaison, intensité, profondeur,
// carotte, spline, variation séculaire, uni-/bi-/trivariée… Sert à ne PAS répondre « courbe : non » à la place du modèle.
bool mentionsCurve(const QString& text);
// Demande explicite d'un modèle en mode courbe (« mode courbe », « courbe vectorielle », « courbe de variation séculaire »…) :
// SeldonH répond « courbe : oui » lui-même.
bool requestsCurveModel(const QString& text);
// Type de courbe lu dans le texte (choix du nœud « modele_type_courbe ») : grandeurs archéomagnétiques citées (« I = 39 »,
// « (I, D, F) », inclinaison, déclinaison, intensité, « courbe vectorielle ») → inc_dec_int, inc_dec, inc_int,
// inclinaison, declinaison, intensite ; sinon « univariée / bivariée / trivariée ». Vide si rien de sûr.
QString curveTypeFromText(const QString& text);
// Le texte dit qu'il n'y a pas d'ordre stratigraphique (« ni phase ni ordre stratigraphique », « sans stratigraphie »).
bool deniesStratigraphy(const QString& text);

// Grammaire GBNF d'un nœud : la réponse est exactement l'un des identifiants de choix.
QString grammarFor(const Node& n);
// Message pour le modèle de langage : la question de l'utilisateur, puis la question du nœud et ses choix (identifiant :
// libellé), avec la consigne de répondre par un seul identifiant.
QString promptFor(const Node& n, const QString& userQuestion);
// Identifiant de choix lu dans une réponse libre (modèle distant, ou local sans grammaire) : réponse exacte (guillemets,
// gras, point final tolérés), sinon un seul identifiant présent comme mot ; vide si rien ou plusieurs. Le raisonnement
// (<think>…</think>) est ignoré ; non refermé (réponse coupée), la réponse est vide.
QString parseAnswer(const Node& n, const QString& answer);

} // namespace decision
} // namespace seldon

#endif
