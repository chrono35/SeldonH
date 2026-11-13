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

// Ordre des couches écrit par l'utilisateur (12/11/2026). Le modèle de langage traduit « au-dessus », « au fond »,
// « sous », « recoupe »… en contraintes d'Events ; un petit modèle se trompe de sens. SeldonH lit lui-même l'ordre dans
// le texte de l'utilisateur, sans modèle de langage, et le compare aux contraintes du bloc ```chronomodel :
//   - relationsFromText : couples (plus ancien, plus récent) d'unités nommées (US 1, couche 3, niveau 2, layer 4) ;
//     positions absolues (au-dessus / en haut / au sommet, au milieu, au fond / en bas / à la base) et relations entre
//     deux unités (A sous B, A sur B, A recoupe B, A est scellée par B, below, above, cuts…) ;
//   - checkSpec : retire les unités nommées sans date (elles se datent avec les traces), corrige ou complète les
//     contraintes qui contredisent le texte ou y manquent, avec une note pour l'utilisateur.
// Seule la stratigraphie (ordre des Events) est contrôlée : une inversion des DATATIONS est traitée par ChronoModel.

#ifndef SELDON_STRATIGRAPHY_H
#define SELDON_STRATIGRAPHY_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {
namespace strat {

struct Relation {
    QString older;       // clé d'unité (« us3 », « couche2 »)
    QString younger;
    QString evidence;    // mots du texte (« us3 : au fond ; us1 : au dessus »)
};

// Clé d'une unité stratigraphique écrite dans un nom ou un texte (« US 1 », « us1 », « U.S. 1 » -> « us1 » ; « Couche 3 »,
// « layer 3 » -> « couche3 » ; « niveau 2 », « level 2 » -> « niveau2 ») ; vide si ce n'en est pas une.
QString unitKey(const QString& name);

// Clé d'un nom d'Event : celle de son US numérotée s'il en a une, sinon « n:<nom passé par fold> » (« n:fond de fosse »).
QString eventKey(const QString& name);

// Couples (plus ancien, plus récent) lus dans un texte ; vide s'il n'y a pas au moins deux unités ordonnées.
// `eventNames` : noms d'Events du modèle, cherchés tels quels dans le texte (« Surface », « Fond de fosse »), en plus des
// US numérotées ; un nom pris dans une position (« en surface », « au fond ») reste une position. Noms de moins de trois
// lettres ou trop courants (« date », « phase ») ignorés.
QVector<Relation> relationsFromText(const QString& text, const QStringList& eventNames = {});
// Plusieurs messages, du plus ancien au plus récent : un couple d'un message plus récent remplace le couple inverse.
QVector<Relation> relationsFromTexts(const QStringList& texts, const QStringList& eventNames = {});
// Fermeture transitive des couples (A < B et B < C donnent A < C), avec la chaîne en preuve.
QVector<Relation> closure(const QVector<Relation>& rels);
// Unités nommées dans les textes (clés), avec ou sans position.
QStringList unitsIn(const QStringList& texts, const QStringList& eventNames = {});

// Unité à dater qui n'est pas un Event du modèle (US sans date, retirée ou absente) et ses voisines dans le modèle, d'après
// l'ordre du texte : la plus récente des plus anciennes (en dessous) et la plus ancienne des plus récentes (au-dessus).
struct Undated {
    QString name;            // nom de l'Event retiré, ou unité écrite proprement (« US 2 »)
    QString older;           // Event du modèle juste en dessous (vide si aucun)
    QString younger;         // Event du modèle juste au-dessus (vide si aucun)
};

struct SpecCheck {
    QJsonObject spec;
    QStringList notes;       // pour l'utilisateur
    bool changed = false;
    QVector<Undated> undated;
};

// « us2 » -> « US 2 », « couche3 » -> « couche 3 », « niveau2 » -> « niveau 2 » ; nom d'Event (« n:… ») inchangé sans « n: ».
QString unitLabel(const QString& key);
// Spécification du bloc ```chronomodel contrôlée contre l'ordre du texte. `textUnits` : unités nommées par l'utilisateur ;
// un Event sans date (ni borne) dont le nom est l'une d'elles est retiré, ainsi que ses contraintes (une séquence garde
// les autres noms). Puis chaque couple de `rels` entre deux Events du modèle : contredit -> contraintes de ces Events
// remplacées par celles du texte ; absent -> ajouté. Les autres contraintes sont gardées.
// `source` (texte de l'utilisateur, facultatif) : une BORNE d'unité nommée dont l'année n'y figure pas (N, 1 − N, −N,
// 1950 − N) est inventée par le modèle d'IA et retirée comme un Event sans date.
SpecCheck checkSpec(const QJsonObject& spec, const QVector<Relation>& rels, const QStringList& textUnits,
                    const QString& source = QString());
// Même contrôle, l'ordre étant lu dans `texts` (questions de l'utilisateur) avec les noms d'Events de `spec`. Une courbe
// âge-profondeur passe d'abord par checkAgeDepth, et les réglages écrits par l'utilisateur (période d'étude, pas, recuit…)
// oubliés par le modèle d'IA sont repris (studySettingsFromTexts).
// `attached` : textes des fichiers joints (tableau CSV de ChronoModel joint avec 📎), lus seulement par checkAgeDepth.
SpecCheck checkSpec(const QJsonObject& spec, const QStringList& texts, const QStringList& attached = {});
// Appartenance aux phases écrite par l'utilisateur (« US3 et US5 sont dans la même phase "habitation" », « la phase A
// regroupe l'US 2 et l'US 4 ») : nom de phase (tel qu'écrit dans `phaseNames`) -> clés des unités citées. Seulement les
// phases de `phaseNames` ; une phrase qui cite deux phases n'est pas lue (prompt réel du 13/11/2026).
QHash<QString, QStringList> phaseMembersFromTexts(const QStringList& texts, const QStringList& eventNames, const QStringList& phaseNames);

// Ordre de superposition écrit pour une suite de niveaux sans nom d'unité (courbe âge-profondeur) : « chacun plus ancien
// que celui du dessus », « du plus récent au plus ancien », « en ordre stratigraphique », « le plus profond est le plus
// ancien », « each older than the one above »… Faux si le texte le nie (« pas en ordre stratigraphique », « remanié »).
bool superpositionStated(const QString& text);

// Courbe âge-profondeur (Philippe, 12/11/2026 ; prompt réel « Teilleul 2 », 15 h 45) :
//   - stratigraphie : par nature, les niveaux d'une carotte sont superposés. Ordre écrit (superpositionStated) : les
//     contraintes sont la séquence des profondeurs de ChronoModel (ChrBuilder::chronoDepth), de la plus basse (l'Event
//     le plus ancien, valeur négative permise) à la plus haute, à la place de celles du modèle d'IA si elles ne la
//     donnent pas (sens inversé, chaîne incomplète) ;
//   - surface : le niveau de profondeur la plus haute, nommé « surface », « sommet », « actuel »… (ou dit « niveau actuel » dans le
//     texte), est le présent de l'étude : une BORNE à la fin de la période d'étude (tmax des réglages, sinon la fin de
//     l'intervalle uniforme écrit), jamais un intervalle uniforme sur toute la période.
//   - profondeurs : convention de ChronoModel, la plus PETITE est l'Event le plus ancien (Philippe, 12/11/2026). Tableau CSV
//     de ChronoModel dans les textes : les profondeurs du tableau sont recopiées telles quelles, avec leur signe, sur les
//     Events (retrouvés par leurs valeurs de date, sinon par l'ordre des lignes), et « depth_sign »: "as_given" est posé ;
//     les lignes peuvent être dans le désordre. Texte qui mesure depuis la base (« hauteur », « depuis la base », « 0 au
//     fond », « la profondeur augmente avec le temps ») : « as_given » aussi.
SpecCheck checkAgeDepth(const QJsonObject& spec, const QStringList& texts);

// Lignes du tableau CSV de ChronoModel (« method;dating name/code;param a;param b;param c;Depth;Err depth ») trouvées dans
// un texte, dans l'ordre du tableau ; vide sans en-tête.
struct CsvDepthRow {
    QString method, name;
    double a = 0, b = 0;
    double depth = 0, depthError = 0;
    bool hasDepth = false;
};
QVector<CsvDepthRow> chronoModelCsvRows(const QString& text);

// Réglages écrits dans le texte de l'utilisateur, au format du bloc ```chronomodel : « settings » (tmin, tmax : « période
// de -1500 à 2000 » ; step : « au pas d'un an ») et « mcmc » (« pas de recuit », itérations, burn-in, chaînes, thinning).
// Le message le plus récent qui en donne l'emporte. Objet vide si rien n'est écrit.
QJsonObject studySettingsFromTexts(const QStringList& texts);

// Remplace le JSON du premier bloc ```chronomodel (ou ```json avec « events ») de `markdown` par `spec`.
QString replaceModelBlock(const QString& markdown, const QJsonObject& spec);

} // namespace strat
} // namespace seldon

#endif
