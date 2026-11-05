#pragma once
// Correction approchée des mots déclencheurs des détecteurs de SeldonH (05/11/2026).
// Une question mal orthographiée (« date le nivau 50 », « variaton la plus forte ») n'était reconnue par aucun détecteur et
// partait au modèle de langage. Avant les détecteurs, SeldonH corrige sur une COPIE de la question les mots à une lettre
// (deux pour les mots longs) d'un mot déclencheur : distance d'édition de Damerau-Levenshtein (substitution, insertion,
// suppression, inversion de deux lettres voisines). Prudent : jamais un mot qui existe (dictionnaires français et anglais),
// jamais un mot de moins de 5 lettres ni vers un mot de moins de 6, jamais un nombre, un nom d'Event, de colonne ou de
// fichier, et rien en cas d'ambiguïté. Les nombres ne sont jamais touchés : une mauvaise correction ne peut donner qu'un
// mauvais choix de calcul, signalé dans la fiche (« ↻ Compris : « nivau » → « niveau » »).
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon::fuzzy {

struct Fix { QString from, to; };      // mot écrit, mot compris (forme affichée, accentuée)

// Distance de Damerau-Levenshtein restreinte (OSA) ; maxd + 1 dès qu'elle dépasse maxd.
int distance(const QString& a, const QString& b, int maxd);

// Question corrigée (mêmes mots ailleurs, ponctuation et nombres inchangés) ; `fixes` reçoit les corrections faites.
// `keep` : mots à ne jamais corriger (noms d'Events, de colonnes, de fichiers), comparés sans casse ni accents.
QString correct(const QString& question, QVector<Fix>* fixes = nullptr, const QStringList& keep = {});

// « « nivau » → « niveau », « variaton » → « variation » »
QString describe(const QVector<Fix>& fixes);

} // namespace seldon::fuzzy
