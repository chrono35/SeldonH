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

#include "Tools.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include "CurveStore.h"
#include "MethodRegistry.h"
#include "TextUtil.h"

namespace seldon {
namespace tools {

namespace {

QJsonObject num(const QString& desc) { return { { "type", "number" }, { "description", desc } }; }

QJsonObject str(const QString& desc, const QStringList& allowed = {})
{
    QJsonObject o{ { "type", "string" }, { "description", desc } };
    if (!allowed.isEmpty()) o["enum"] = QJsonArray::fromStringList(allowed);
    return o;
}

QJsonObject schema(const QJsonObject& props, const QStringList& required)
{
    return { { "type", "object" }, { "properties", props }, { "required", QJsonArray::fromStringList(required) },
             { "additionalProperties", false } };
}

QString names(const QString& methodId) { return CurveStore::names(methodId).join(", "); }

QVector<ToolDef> build()
{
    QVector<ToolDef> v;

    v.append({ "calibrer_radiocarbone",
               QStringLiteral("Calibre une date radiocarbone (14C) avec les courbes de ChronoModel et renvoie la densité calendaire, "
                              "les intervalles HPD à 95 % et 68 %, le MAP, la moyenne et les quartiles. À utiliser pour TOUTE date 14C "
                              "(âge conventionnel en BP avec son erreur), même écrite brièvement : « date 14c 3548 +- 25 », "
                              "« C14 3548 ± 25 BP », « 3548±25 BP », « âge radiocarbone de 3548 ans, erreur 25 », « date-moi 3548 ± 25 BP ». "
                              "Ne calibre jamais de tête et n'estime jamais une date calendaire sans cet outil : un âge BP n'est pas une date calendaire."),
               schema({ { "age", num(QStringLiteral("Âge radiocarbone conventionnel, en années BP (ex. 2500).")) },
                        { "error", num(QStringLiteral("Erreur (écart-type) de la mesure, en années, strictement positive (ex. 30).")) },
                        { "curve", str(QStringLiteral("Courbe de calibration. Défaut : intcal20 (atmosphère, hémisphère nord) ; shcal20 pour le sud ; "
                                                      "marine20 pour un échantillon marin. Disponibles : %1.").arg(names("c14"))) },
                        { "delta_r", num(QStringLiteral("Effet réservoir régional ΔR, en années (échantillon marin seulement).")) },
                        { "delta_r_error", num(QStringLiteral("Erreur sur ΔR, en années (avec delta_r).")) } },
                      { "age", "error" }),
               "c14" });

    v.append({ "calibrer_f14c",
               QStringLiteral("Calibre une fraction moderne F14C (échantillon post-bombe, après 1950) avec les courbes de la bombe atomique "
                              "et renvoie la densité calendaire et les intervalles HPD à 95 % et 68 %."),
               schema({ { "f14c", num(QStringLiteral("Fraction moderne F14C (ex. 1.2345).")) },
                        { "error", num(QStringLiteral("Erreur sur F14C, strictement positive (ex. 0.0030).")) },
                        { "curve", str(QStringLiteral("Courbe. Défaut : bomb21nhz1 (zone 1 de l'hémisphère nord) ; bomb21shz1 pour le sud ; "
                                                      "intcal20 pour une date avant la bombe. Disponibles : %1.")
                                           .arg(names("f14c") + ", " + names("c14"))) } },
                      { "f14c", "error" }),
               "f14c" });

    v.append({ "dater_luminescence",
               QStringLiteral("Densité calendaire et intervalles HPD pour une datation par luminescence (TL/OSL) : un âge en années "
                              "avant l'année de mesure, avec son erreur (« TL 800 ± 60 ans, mesuré en 2015 », « OSL 12,3 ± 0,8 ka »). L'année de mesure est obligatoire : demande-la à l'utilisateur si elle manque."),
               schema({ { "age", num(QStringLiteral("Âge de luminescence, en années avant l'année de mesure.")) },
                        { "error", num(QStringLiteral("Erreur (écart-type) de l'âge, en années, strictement positive.")) },
                        { "ref_year", num(QStringLiteral("Année de la mesure (ex. 2015).")) },
                        { "unit", str(QStringLiteral("Unité de l'âge et de l'erreur telle que l'utilisateur l'écrit : ans (défaut), ka (milliers "
                                                     "d'années), Ma (millions d'années). Recopie les valeurs sans les convertir : SeldonH convertit."),
                                      { "ans", "ka", "Ma" }) } },
                      { "age", "error", "ref_year" }),
               "tl" });

    v.append({ "densite_gauss",
               QStringLiteral("Densité calendaire et intervalles HPD pour une mesure gaussienne (valeur, écart-type) : sans calibration, "
                              "la valeur est une année calendaire (années astronomiques, 0 = 1 av. J.-C.) ; avec « curve », la valeur est "
                              "une mesure datée sur une courbe de référence du plugin Gauss de ChronoModel (fichier du dossier Gauss, "
                              "« t, valeur, erreur », ex. un export Curve_X_Gauss_ref.csv), comme une calibration."),
               schema({ { "age", num(QStringLiteral("Moyenne : année calendaire sans courbe (%1), ou valeur mesurée avec « curve ».").arg(QStringLiteral("Année astronomique (l'année 0 existe : 0 = 1 av. J.-C., -499 = 500 av. J.-C.) ; pour une année av. J.-C., écris-la plutôt en texte, « 500 av. J.-C. » : SeldonH convertit."))) },
                        { "error", num(QStringLiteral("Écart-type (années, ou unité de la mesure avec « curve »), strictement positif.")) },
                        { "curve", str(QStringLiteral("Courbe de référence du plugin Gauss (nom du fichier du dossier Gauss, voir courbes_disponibles). "
                                                      "Facultatif : seulement si l'utilisateur la nomme.")) },
                        { "unit", str(QStringLiteral("Âge des géologues : unité telle que l'utilisateur l'écrit, ka ou Ma (« 12 ± 1 ka ») ; "
                                                     "recopie les valeurs sans les convertir, SeldonH les ramène en années calendaires. "
                                                     "Sans courbe seulement. Facultatif."), { "ans", "ka", "Ma" }) },
                        { "origin", str(QStringLiteral("Origine de l'âge si l'utilisateur la donne : BP (1950, défaut) ou b2k (2000). Facultatif."),
                                        { "BP", "b2k" }) } },
                      { "age", "error" }),
               "gauss" });

    v.append({ "densite_uniforme",
               QStringLiteral("Densité calendaire uniforme entre deux années (terminus post quem / terminus ante quem) et ses intervalles "
                              "HPD. Années astronomiques (0 = 1 av. J.-C.)."),
               schema({ { "min", num(QStringLiteral("Borne inférieure, en année calendaire. %1").arg(QStringLiteral("Année astronomique (l'année 0 existe : 0 = 1 av. J.-C., -499 = 500 av. J.-C.) ; pour une année av. J.-C., écris-la plutôt en texte, « 500 av. J.-C. » : SeldonH convertit."))) },
                        { "max", num(QStringLiteral("Borne supérieure, en année calendaire (même convention).")) } },
                      { "min", "max" }),
               "unif" });

    v.append({ "densite_tabulee",
               QStringLiteral("Datation par une densité tabulée du plugin Density de ChronoModel (fichier .csv du dossier Density : "
                              "tmin, tmax, puis les valeurs) : HPD, médiane, graphique. Voir courbes_disponibles pour les fichiers."),
               schema({ { "curve", str(QStringLiteral("Nom du fichier de densité (dossier Density), tel que l'utilisateur le donne.")) } },
                      { "curve" }),
               "density" });

    v.append({ "dater_archeomagnetisme",
               QStringLiteral("Datation archéomagnétique par comparaison à une courbe de référence de la variation séculaire : "
                              "inclinaison (I), déclinaison (D), intensité (F) ou une combinaison. Renvoie la densité calendaire et les "
                              "intervalles HPD. Pour I ou D, α95 est obligatoire ; pour F, l'erreur sur l'intensité est obligatoire. "
                              "Demande à l'utilisateur toute valeur manquante."),
               schema({ { "process", str(QStringLiteral("Grandeurs mesurées : I (inclinaison), D (déclinaison), F (intensité), "
                                                         "ou ID, IF, IDF."), { "I", "D", "F", "ID", "IF", "IDF" }) },
                        { "inc", num(QStringLiteral("Inclinaison, en degrés (si process contient I).")) },
                        { "dec", num(QStringLiteral("Déclinaison, en degrés (si process contient D).")) },
                        { "alpha95", num(QStringLiteral("Cône de confiance α95, en degrés, strictement positif (si I ou D).")) },
                        { "field", num(QStringLiteral("Intensité du champ, en µT (si process contient F).")) },
                        { "error_f", num(QStringLiteral("Erreur sur l'intensité, en µT, strictement positive (si F).")) },
                        { "curve_i", str(QStringLiteral("Courbe d'inclinaison (défaut gal2002sph2014_i ; bulgaria_i pour la Bulgarie). Disponibles : %1.").arg(names("am"))) },
                        { "curve_d", str(QStringLiteral("Courbe de déclinaison (défaut gal2002sph2014_d ; bulgaria_d).")) },
                        { "curve_f", str(QStringLiteral("Courbe d'intensité (défaut gwh2013uni_f ; bulgaria_f).")) } },
                      { "process" }),
               "am" });

    v.append({ "calculer_evenement",
               QStringLiteral("Estime la date θ d'un ÉVÉNEMENT (Event de ChronoModel) à partir de plusieurs datations du même "
                              "événement, éventuellement de méthodes différentes (14C, F14C, TL/OSL, archéomagnétisme, Gauss, uniforme), "
                              "par MCMC avec le modèle d'événement de ChronoModel (dispersion individuelle σ_i de chaque date). "
                              "Paramètres MCMC par défaut de ChronoModel si l'utilisateur n'en précise pas. Renvoie la densité a posteriori "
                              "de θ, ses HPD, les valeurs a posteriori de chaque date et les critères de convergence (R̂, ESS). Ne sert PAS aux traces "
                              "MCMC jointes (durée ou date entre des Events d'un calcul ChronoModel) : utiliser analyser_traces."),
               schema({ { "name", str(QStringLiteral("Nom de l'événement (ex. « US 12 »).")) },
                        { "dates", QJsonObject{ { "type", "array" },
                                                { "description", QStringLiteral("Datations : objets avec « method » (c14, f14c, tl, gauss, unif, am) et les "
                                                                                "paramètres de cette méthode (mêmes noms que les autres outils, ex. "
                                                                                "{\"method\":\"c14\",\"age\":2500,\"error\":30}), et facultativement "
                                                                                "« name » (nom de la date, ex. « Ly-1234 », « Gor-12 » d'un .chr ; jamais le nom "
                                                                                "de l'événement) et « wiggle » ({\"type\":\"fixed\",\"value\":20}, "
                                                                                "{\"type\":\"range\",\"min\":10,\"max\":30} ou "
                                                                                "{\"type\":\"gaussian\",\"mean\":20,\"error\":5}) pour un décalage δ entre "
                                                                                "l'échantillon et l'événement.") },
                                                { "items", QJsonObject{ { "type", "object" } } } } },
                        { "tmin", num(QStringLiteral("Début de la période d'étude (année astronomique, ou en texte « 500 av. J.-C. »). Facultatif : calculé d'après les datations.")) },
                        { "tmax", num(QStringLiteral("Fin de la période d'étude (année astronomique, ou en texte « 500 av. J.-C. »). Facultatif.")) },
                        { "mcmc", QJsonObject{ { "type", "object" },
                                               { "description", QStringLiteral("Facultatif, seulement si l'utilisateur le demande : num_iter (100000), num_burn (1000), "
                                                                               "num_proc = nombre de chaînes (4), max_batches (200), iter_per_batch (100), "
                                                                               "thinning_interval (10), mixing_level (0.99), anneal_temp (10), "
                                                                               "anneal_recurrence (1000), anneal_dwell (1000), seeds (liste d'entiers).") } } },
                        { "traces", QJsonObject{ { "type", "boolean" },
                                                 { "description", QStringLiteral("Vrai seulement si l'utilisateur demande explicitement les traces des "
                                                                                 "chaînes MCMC (graphique long à tracer). Par défaut : pas de traces.") } } } },
                      { "dates" }),
               "event" });

    v.append({ "combiner_dates",
               QStringLiteral("Combine plusieurs datations d'une MÊME date (bouton « Combine » de ChronoModel) : produit de leurs "
                              "vraisemblances, même si les méthodes diffèrent (14C, F14C, TL/OSL, archéomagnétisme, Gauss, uniforme), "
                              "chaque datation pouvant avoir un décalage δ (wiggle). Renvoie la densité combinée, ses HPD, chaque "
                              "datation seule et, à titre d'information seulement, le test de Ward et Wilson. À distinguer de "
                              "calculer_evenement (MCMC avec une dispersion propre à chaque date) : utiliser celui-ci quand "
                              "l'utilisateur dit « combine » / « combinaison », l'autre quand il parle d'événement."),
               schema({ { "name", str(QStringLiteral("Nom de la combinaison (ex. « Four 3 »). Facultatif.")) },
                        { "dates", QJsonObject{ { "type", "array" },
                                                { "description", QStringLiteral("Au moins deux datations : objets avec « method » (c14, f14c, tl, gauss, unif, am) et "
                                                                                "les paramètres de cette méthode (mêmes noms que les autres outils), et "
                                                                                "facultativement « name » et « wiggle » ({\"type\":\"fixed\",\"value\":20}, "
                                                                                "{\"type\":\"range\",\"min\":10,\"max\":30} ou "
                                                                                "{\"type\":\"gaussian\",\"mean\":20,\"error\":5}) : la datation date alors t + δ.") },
                                                { "items", QJsonObject{ { "type", "object" } } } } } },
                      { "dates" }),
               "combine" });

    {
        // Deux outils pour le même calcul « relation » : « comparer » (datations) et « traces » (colonnes d'un export de
        // ChronoModel). Chacun accepte les deux sortes de termes, et des années fixes : un petit modèle choisit l'outil
        // d'après les mots de la question.
        const QString dating = QStringLiteral("objet avec « method » (c14, f14c, tl, gauss, unif, am, density) et les paramètres de cette "
                                              "méthode (mêmes noms que les autres outils, ex. {\"method\":\"c14\",\"age\":2500,\"error\":30,"
                                              "\"name\":\"Ly-1234\"}), facultativement « wiggle » ; ou un événement {\"method\":\"event\","
                                              "\"name\":…,\"dates\":[…]} ou une combinaison {\"method\":\"combine\",\"dates\":[…]}");
        const QString column = QStringLiteral("nom d'une colonne des traces MCMC jointes (ex. « US 12 », « Phase A Begin », « Phase A End »)");
        const QJsonObject terms{ { "type", "array" },
                                 { "description", QStringLiteral("Pour « correlation » seulement : liste de 2 à 8 termes (colonnes des traces ou "
                                                                 "datations, comme A et B) dont on veut la matrice de corrélation.") },
                                 { "items", QJsonObject{ { "anyOf", QJsonArray{ QJsonObject{ { "type", "object" } }, QJsonObject{ { "type", "string" } } } } } } };
        auto term = [&](const QString& what) {
            return QJsonObject{ { "anyOf", QJsonArray{ QJsonObject{ { "type", "object" } }, QJsonObject{ { "type", "string" } } } },
                                { "description", QStringLiteral("%1 : soit une datation (%2), soit le %3, soit une année fixe (« -500 », "
                                                                "« 500 av. J.-C. »).").arg(what, dating, column) } };
        };
        const QString ops = QStringLiteral("Opérations : « order » (défaut) : P(A antérieur à B) et l'inverse, avec l'écart B − A ; « duration » : "
                                           "durée B − A en années, ses HPD et P(B postérieur à A) ; « between » : date d'un niveau compris "
                                           "entre A et B (uniforme entre les deux) ; « inside » : P(C dans [A ; B]) ; « value » : densité de C ; « correlation » : "
                                           "corrélation tirage par tirage (r de Pearson, ρ de Spearman, matrice et nuage de points) de 2 à 8 "
                                           "termes donnés dans « terms » (ou A et B).");
        v.append({ "comparer_dates",
                   QStringLiteral("Relation entre DEUX datations (ou trois pour « inside ») : laquelle est la plus ancienne, l'écart ou la "
                                  "durée entre elles, la date d'un niveau situé entre les deux, ou si une datation tombe dans un intervalle. "
                                  "Datations simples supposées indépendantes : calcul exact sur leurs densités calibrées. %1 Un terme peut aussi "
                                  "être une colonne des traces MCMC jointes (Event ou borne de phase d'un modèle ChronoModel) : le calcul se fait "
                                  "alors tirage par tirage. Si les deux termes sont des colonnes de traces, préférer analyser_traces.").arg(ops),
                   schema({ { "op", str(QStringLiteral("Opération : order (défaut), duration, between, inside, value ou correlation."),
                                        { "order", "duration", "between", "inside", "value", "correlation" }) },
                            { "a", term(QStringLiteral("Premier terme (A), le plus ancien supposé ; borne ancienne pour « inside »")) },
                            { "b", term(QStringLiteral("Second terme (B) ; borne récente pour « inside »")) },
                            { "of", term(QStringLiteral("Terme testé (C) pour « inside », ou terme analysé pour « value »")) },
                            { "terms", terms },
                            { "name", str(QStringLiteral("Nom du résultat, repris dans le titre du graphique. Facultatif.")) } },
                          { "a", "b" }),
                   "relation" });

        v.append({ "analyser_traces",
                   QStringLiteral("Calcule a posteriori, sur les traces MCMC exportées par ChronoModel et jointes par l'utilisateur "
                                  "(Chain_all_Events.csv, Chain_all_Phases.csv…), tirage par tirage : la durée entre deux colonnes "
                                  "(op « duration » : to − from, en années, avec la probabilité que « to » soit postérieur à « from »), "
                                  "une date comprise entre deux colonnes (op « between » : uniforme entre les deux à chaque tirage), ou la "
                                  "densité d'une colonne (op « value », ex. début ou fin d'une phase), l'antéro-postérité de deux colonnes "
                                  "(op « order » : P(from antérieur à to) et l'inverse), ou l'appartenance de « of » à l'intervalle "
                                  "[from ; to] (op « inside » : deux Events, début et fin d'une phase, ou années fixes). Un terme peut aussi être "
                                  "une datation simple hors du modèle (ex. durée entre la fin d'une phase et une date 14C) : elle est tirée à "
                                  "chaque tirage, supposée indépendante du modèle. Renvoie la densité, ses HPD et la convergence, ou les "
                                  "probabilités avec leur erreur Monte-Carlo. Les noms de colonnes sont ceux du fichier joint (ex. « US 12 », "
                                  "« Phase A Begin », « Phase A End ») ; durée d'une phase : from = « X Begin », to = « X End ». "
                                  "Op « correlation » : corrélation entre 2 à 8 colonnes ou datations (« terms »), avec la matrice "
                                  "(r de Pearson, ρ de Spearman) et le nuage de points de chaque paire."),
                   schema({ { "op", str(QStringLiteral("Opération : duration, between, value, order, inside ou correlation."),
                                        { "duration", "between", "value", "order", "inside", "correlation" }) },
                            { "from", term(QStringLiteral("Terme A (duration, between, order) ou borne ancienne de l'intervalle (inside)")) },
                            { "to", term(QStringLiteral("Terme B (duration, between, order) ou borne récente de l'intervalle (inside)")) },
                            { "of", term(QStringLiteral("Terme analysé (value) ou testé (inside)")) },
                            { "terms", terms },
                            { "name", str(QStringLiteral("Nom du résultat, repris dans le titre du graphique. Facultatif.")) } },
                          { "op" }),
                   "relation" });

        v.append({ "courbe_chronomodel",
                   QStringLiteral("Calcule a posteriori sur une COURBE estimée par ChronoModel (mode courbe : 1 à 3 composantes X, Y, Z en "
                                  "fonction du temps, par exemple inclinaison, déclinaison, intensité), à partir de la trace des splines "
                                  "jointe par l'utilisateur (Curve_X_Splines.csv), tirage par tirage, exactement comme ChronoModel évalue "
                                  "ses splines. Opérations : « value » : loi de la valeur de la courbe à la date t (avec y : P(courbe < y) "
                                  "et P(courbe > y) ; avec ymin et ymax : P(ymin ≤ courbe ≤ ymax)) ; « derivative » : vitesse instantanée "
                                  "à la date t ; « rate » : vitesse moyenne entre les dates from et to ; « crossing » : date où la courbe "
                                  "atteint la valeur y (which : first, last ou all) ; « calibrate » : densité de la date d'une mesure y ± error "
                                  "(comme une calibration, sur la courbe a posteriori ; « probabilité de date pour X = 0 ± 1 ») ; « rate_values » : temps mis et vitesse moyenne pour "
                                  "aller de la valeur y1 à la valeur y2 ; « acceleration » : accélération (dérivée seconde) à la date t ; "
                                  "« plot » : trace les courbes de la valeur, de la vitesse et de l'accélération (médiane et intervalle de crédibilité à 95 %, quantiles 2,5 – 97,5 %, "
                                  "liste « quantities ») ; « extremum » : date où la grandeur est extrême dans chaque courbe tirée "
                                  "(« où / quand la variation est la plus forte », « vitesse maximale », « maximum de la courbe » ; "
                                  "quantity derivative par défaut, sense abs, max, min ou absmin ; aucune date à donner). « quantity » choisit la courbe sur laquelle portent crossing, calibrate et derivative : "
                                  "value (défaut), derivative (vitesse ; taux d'accumulation d'une courbe âge-profondeur) ou acceleration "
                                  "(ex. date d'une vitesse mesurée, date où la vitesse s'annule). Une date peut être une année, une colonne des "
                                  "traces jointes (date d'un Event, ex. « US 12 ») ou une datation. Vitesses en unités de la composante par an, "
                                  "accélérations par an² (per : 100 pour par siècle, par siècle²). Une carte de densité (Curve_X_Map.csv) suffit "
                                  "pour « value » à une année fixe."),
                   schema({ { "op", str(QStringLiteral("Opération : value, derivative, acceleration, rate, crossing, calibrate, rate_values, extremum ou plot. "
                                                   "Date de la variation la plus forte : extremum (sans t), jamais derivative sans t."),
                                        { "value", "derivative", "acceleration", "rate", "crossing", "calibrate", "rate_values", "extremum", "plot" }) },
                            { "quantity", str(QStringLiteral("Courbe visée par crossing, calibrate, value : value (la courbe, défaut), derivative "
                                                             "(sa vitesse) ou acceleration (son accélération). La valeur y est alors une vitesse "
                                                             "(ou une accélération), dans l'unité donnée par per. Facultatif."),
                                              { "value", "derivative", "acceleration" }) },
                            { "quantities", QJsonObject{ { "type", "array" }, { "items", QJsonObject{ { "type", "string" }, { "enum", QJsonArray{ "value", "derivative", "acceleration" } } } },
                                                         { "description", QStringLiteral("plot : courbes à tracer (défaut : les trois).") } } },
                            { "dim", str(QStringLiteral("Composante : X (défaut), Y, Z ; pour une courbe archéomagnétique I (inclinaison), D (déclinaison), "
                                                        "F (intensité). Défaut : la première composante."),
                                         { "X", "Y", "Z", "I", "D", "F" }) },
                            { "t", term(QStringLiteral("Date (value, derivative)")) },
                            { "from", term(QStringLiteral("Première date (rate)")) },
                            { "to", term(QStringLiteral("Seconde date (rate)")) },
                            { "y", num(QStringLiteral("Valeur de la composante : seuil (value) ou valeur à atteindre (crossing).")) },
                            { "ymin", num(QStringLiteral("Borne basse d'un intervalle de valeurs (value). Facultatif.")) },
                            { "ymax", num(QStringLiteral("Borne haute d'un intervalle de valeurs (value). Facultatif.")) },
                            { "error", num(QStringLiteral("calibrate : erreur (1σ) de la valeur mesurée y. Seulement si l'utilisateur la donne : sans elle, la valeur est prise exacte.")) },
                            { "y1", num(QStringLiteral("Valeur de départ (rate_values).")) },
                            { "y2", num(QStringLiteral("Valeur d'arrivée (rate_values).")) },
                            { "sense", str(QStringLiteral("extremum : abs (la plus forte en valeur absolue, défaut pour une vitesse), max "
                                                          "(défaut pour la courbe elle-même), min, absmin (la plus faible, courbe la plus plate)."),
                                           { "abs", "max", "min", "absmin" }) },
                            { "which", str(QStringLiteral("crossing : première (first, défaut), dernière (last) ou toutes (all) les atteintes."),
                                           { "first", "last", "all" }) },
                            { "per", num(QStringLiteral("Vitesse pour combien d'années : 1 (défaut, par an), 100 (par siècle). Facultatif.")) },
                            { "speed", num(QStringLiteral("derivative, rate : seuil de vitesse (même unité que la vitesse, donc par siècle si per = 100) ; "
                                                          "donne P(vitesse > seuil) et P(|vitesse| > |seuil|). Facultatif.")) },
                            { "tmin", num(QStringLiteral("crossing, calibrate, rate_values, extremum, plot : début de la fenêtre (défaut : période d'étude). Facultatif.")) },
                            { "tmax", num(QStringLiteral("crossing, calibrate, rate_values, plot : fin de la fenêtre. Facultatif.")) },
                            { "name", str(QStringLiteral("Nom du résultat, repris dans le titre du graphique. Facultatif.")) } },
                          { "op" }),
                   "curve" });
    }

    v.append({ "calibration_inverse",
               QStringLiteral("Calibration inverse : mesure attendue (âge 14C en BP, F14C, inclinaison, déclinaison ou intensité) pour un "
                              "échantillon d'une date calendaire donnée, lue sur une courbe de référence. La date peut être fixe (t), "
                              "gaussienne (t ± t_error) ou uniforme (t_min, t_max) ; lab_error ajoute l'erreur de mesure prévue. "
                              "Renvoie la valeur de la courbe, la loi de la mesure attendue et ses HPD."),
               schema({ { "type", str(QStringLiteral("Courbe : c14 (âge radiocarbone), f14c ou am (archéomagnétisme)."), { "c14", "f14c", "am" }) },
                        { "t", num(QStringLiteral("Date calendaire. %1").arg(QStringLiteral("Année astronomique (l'année 0 existe : 0 = 1 av. J.-C., -499 = 500 av. J.-C.) ; pour une année av. J.-C., écris-la plutôt en texte, « 500 av. J.-C. » : SeldonH convertit."))) },
                        { "t_error", num(QStringLiteral("Écart-type de la date, en années, SEULEMENT si l'utilisateur en donne un. Facultatif : "
                                                        "ne le demande pas et ne l'invente pas (sans lui, la date est prise exacte).")) },
                        { "t_min", num(QStringLiteral("Début d'une période (année astronomique, ou en texte « 500 av. J.-C. »), avec t_max, à la place de t. Facultatif.")) },
                        { "t_max", num(QStringLiteral("Fin de la période (année astronomique, ou en texte « 500 av. J.-C. »). Facultatif.")) },
                        { "curve", str(QStringLiteral("Courbe de référence (défaut : intcal20 pour c14, bomb21nhz1 pour f14c, "
                                                      "gal2002sph2014_i / _d ou gwh2013uni_f pour am).")) },
                        { "component", str(QStringLiteral("Composante archéomagnétique (type am) : I, D ou F."), { "I", "D", "F" }) },
                        { "lab_error", num(QStringLiteral("Erreur de mesure prévue (même unité que la mesure : années BP, F14C, degrés, µT). Facultatif.")) } },
                      { "type" }),
               "inverse" });

    v.append({ "creer_densite",
               QStringLiteral("Crée un fichier de densité tabulée (CSV) pour le plugin Density de ChronoModel à partir d'une loi classique, "
                              "placée sur l'axe du temps : t = offset + x, ou t = offset − x avec reverse (durée comptée vers le passé). "
                              "Renvoie la description, les HPD et le nom du fichier ; l'utilisateur l'enregistre avec un lien. "
                              "N'invente aucun paramètre : demande-les."),
               schema({ { "law", str(QStringLiteral("Loi : normal (mean, sd), lognormal (meanlog et sdlog, ou median et factor), uniform (min, max), "
                                                    "triangular (min, mode, max), trapezoidal (min, mode_min, mode_max, max), exponential (mean ou "
                                                    "rate), gamma (shape et scale, ou mean et sd), beta (alpha, beta, sur [min ; max])."),
                                     { "normal", "lognormal", "uniform", "triangular", "trapezoidal", "exponential", "gamma", "beta" }) },
                        { "mean", num(QStringLiteral("Moyenne (normal, exponential, gamma).")) },
                        { "sd", num(QStringLiteral("Écart-type (normal, gamma).")) },
                        { "meanlog", num(QStringLiteral("Moyenne de ln x (lognormal).")) },
                        { "sdlog", num(QStringLiteral("Écart-type de ln x (lognormal).")) },
                        { "median", num(QStringLiteral("Médiane (lognormal), à la place de meanlog.")) },
                        { "factor", num(QStringLiteral("Facteur multiplicatif > 1 = exp(sdlog) (lognormal), à la place de sdlog.")) },
                        { "min", num(QStringLiteral("Borne inférieure (uniform, triangular, trapezoidal, beta).")) },
                        { "max", num(QStringLiteral("Borne supérieure (uniform, triangular, trapezoidal, beta).")) },
                        { "mode", num(QStringLiteral("Mode (triangular).")) },
                        { "mode_min", num(QStringLiteral("Début du plateau (trapezoidal).")) },
                        { "mode_max", num(QStringLiteral("Fin du plateau (trapezoidal).")) },
                        { "rate", num(QStringLiteral("Taux (exponential, gamma), à la place de mean ou scale.")) },
                        { "shape", num(QStringLiteral("Paramètre de forme (gamma).")) },
                        { "scale", num(QStringLiteral("Paramètre d'échelle (gamma).")) },
                        { "alpha", num(QStringLiteral("Premier paramètre de forme (beta).")) },
                        { "beta", num(QStringLiteral("Second paramètre de forme (beta).")) },
                        { "offset", num(QStringLiteral("Décalage sur l'axe du temps (année) : t = offset + x. 0 par défaut.")) },
                        { "reverse", QJsonObject{ { "type", "boolean" },
                                                  { "description", QStringLiteral("Vrai : t = offset − x (une durée comptée vers le passé depuis offset).") } } },
                        { "tmin", num(QStringLiteral("Début du fichier (année). Facultatif : calculé d'après la loi.")) },
                        { "tmax", num(QStringLiteral("Fin du fichier (année). Facultatif.")) },
                        { "step", num(QStringLiteral("Pas de la grille en années. Facultatif (≈ 1 an).")) },
                        { "points", num(QStringLiteral("Nombre de valeurs, à la place de step. Facultatif.")) },
                        { "name", str(QStringLiteral("Nom du fichier (.csv ajouté). Facultatif.")) } },
                      { "law" }),
               "density_file" });

    {
        const QJsonObject strArray{ { "type", "array" }, { "items", QJsonObject{ { "type", "string" } } } };
        const QJsonObject intArray{ { "type", "array" }, { "items", QJsonObject{ { "type", "integer" } } } };
        auto withDesc = [](QJsonObject o, const QString& d) { o["description"] = d; return o; };
        v.append({ "tableau",
                   QStringLiteral("Agit sur le TABLEAU DE DATATIONS joint ou collé par l'utilisateur, que SeldonH a lu et garde (son résumé, "
                                  "avec les colonnes et les lignes numérotées #1, #2…, est dans la conversation). Sélectionne des lignes "
                                  "(méthode, numéros, noms, valeur d'une colonne) puis : « extract » affiche le sous-tableau (export CSV), "
                                  "« date » calibre chaque ligne (récapitulatif HPD et médiane), « event » estime un événement (MCMC) avec "
                                  "les lignes choisies, « combine » les combine, « model » prépare un modèle ChronoModel (.chr), un événement "
                                  "par ligne ou par valeur de la colonne group_by. Les valeurs sont prises dans le tableau : n'écris "
                                  "jamais toi-même les âges ou les erreurs. Sans critère : toutes les lignes."),
                   schema({ { "action", str(QStringLiteral("extract (défaut), date, event, combine ou model."),
                                            { "extract", "date", "event", "combine", "model" }) },
                            { "methods", withDesc(strArray, QStringLiteral("Méthodes à garder : c14, f14c, tl, am, gauss, unif. Facultatif.")) },
                            { "rows", withDesc(intArray, QStringLiteral("Numéros de ligne à garder (#1 = première ligne de données). Facultatif.")) },
                            { "exclude_rows", withDesc(intArray, QStringLiteral("Numéros de ligne à écarter. Facultatif.")) },
                            { "names", withDesc(strArray, QStringLiteral("Noms (colonne de nom ou code de laboratoire) à garder. Facultatif.")) },
                            { "exclude_names", withDesc(strArray, QStringLiteral("Noms à écarter. Facultatif.")) },
                            { "where", QJsonObject{ { "type", "array" },
                                                    { "description", QStringLiteral("Conditions sur des colonnes, toutes vraies à la fois : "
                                                                                    "{\"column\":\"US\",\"op\":\"=\",\"value\":\"12\"} ; op : =, !=, "
                                                                                    "contains, not_contains, <, >, <=, >=, in (value = liste). "
                                                                                    "Colonnes : en-têtes du tableau (ou méthode, nom, âge, erreur).") },
                                                    { "items", QJsonObject{ { "type", "object" } } } } },
                            { "group_by", str(QStringLiteral("model : colonne qui regroupe les lignes en événements (ex. « US »). Sans elle : un événement par ligne.")) },
                            { "name", str(QStringLiteral("Nom de l'événement, de la combinaison ou du modèle. Facultatif.")) },
                            { "mcmc", QJsonObject{ { "type", "object" },
                                                   { "description", QStringLiteral("event : réglages MCMC, seulement si l'utilisateur les donne (mêmes clés que calculer_evenement).") } } } },
                          {}),
                   "table" });
    }

    v.append({ "courbes_disponibles",
               QStringLiteral("Liste les courbes de référence connues de SeldonH (calibration 14C, F14C bombe, archéomagnétisme, plugin "
                              "Gauss) et les densités du plugin Density. À appeler avant un calcul quand l'utilisateur nomme une courbe "
                              "dont tu n'es pas sûr."),
               schema({ { "method", str(QStringLiteral("Méthode : c14, f14c, am, gauss ou density. Vide = toutes."), { "c14", "f14c", "am", "gauss", "density" }) } }, {}),
               QString() });

    // Niveau de confiance facultatif, commun à toutes les datations.
    for (ToolDef& d : v) {
        if (d.methodId.isEmpty() || d.methodId == QLatin1String("density_file")) continue;
        QJsonObject props = d.parameters.value("properties").toObject();
        // Calculs qui ne sont pas des datations : seulement le niveau de confiance.
        const bool notDating = d.name == QLatin1String("analyser_traces") || d.methodId == QLatin1String("inverse")
                               || d.methodId == QLatin1String("curve")
                               || d.methodId == QLatin1String("table");
        props["level"] = num(QStringLiteral("Niveau de confiance des intervalles HPD, en % (défaut 95). À donner seulement si "
                                            "l'utilisateur demande un autre niveau (ex. 99 ou 90) ; les HPD à 95 % et 68 % sont toujours fournies."));
        if (notDating) { d.parameters["properties"] = props; continue; }
        props["color"] = str(QStringLiteral("Couleur de la courbe (\"#rrggbb\" ou nom de couleur), seulement si l'utilisateur "
                                            "en donne une (ou si la date / l'événement du .chr en a une). Facultatif."));
        props["layout"] = str(QStringLiteral("« overlay » seulement si l'utilisateur demande de superposer les densités sur le même "
                                             "graphe ; sinon (défaut) chaque densité est tracée sur sa propre ligne."), { "rows", "overlay" });
        if (d.methodId != "event") {
            props["tmin"] = num(QStringLiteral("Début de la période d'étude (année astronomique, ou en texte « 500 av. J.-C. »), seulement si l'utilisateur en donne "
                                               "une : la densité est calculée sur [tmin ; tmax] puis renormalisée. Défaut : toute la courbe."));
            props["tmax"] = num(QStringLiteral("Fin de la période d'étude (année astronomique, ou en texte « 500 av. J.-C. »). Facultatif."));
        }
        if (d.methodId != "event" && d.methodId != "combine" && d.methodId != "relation")
            props["name"] = str(QStringLiteral("Nom de la datation (ex. « Ly-1234 », ou le « name » de la date dans un fichier .chr), "
                                               "repris dans le titre du graphique. Facultatif."));
        if (d.methodId != "event" && d.methodId != "combine" && d.methodId != "relation")
            props["wiggle"] = QJsonObject{ { "type", "object" },
                                           { "description", QStringLiteral("Facultatif, seulement si l'utilisateur donne un décalage δ (wiggle-matching) entre "
                                                                           "l'échantillon et ce qu'on veut dater : {\"type\":\"fixed\",\"value\":20}, "
                                                                           "{\"type\":\"range\",\"min\":10,\"max\":30} ou "
                                                                           "{\"type\":\"gaussian\",\"mean\":20,\"error\":5}. Le résultat est la densité de t + δ.") } };
        d.parameters["properties"] = props;
    }
    return v;
}

// Convertit en nombre les valeurs données comme chaînes ; retourne un message d'erreur si impossible.
QString coerce(const ToolDef& def, QJsonObject& args)
{
    const QJsonObject props = def.parameters.value("properties").toObject();
    QJsonObject out;
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (!args.contains(it.key())) continue;
        QJsonValue v = args.value(it.key());
        if (v.isNull() || v.isUndefined()) continue;
        const QString type = it.value().toObject().value("type").toString();
        if (type == "number") {
            if (v.isString()) {
                QString s = v.toString().trimmed();
                s.replace(QChar(0x00A0), QString()).replace(' ', QString()).replace(',', '.');
                if (s.isEmpty()) continue;
                bool ok = false;
                double d = s.toDouble(&ok);
                if (!ok) {                                   // année écrite avec son ère : « 500 av. J.-C. » -> -499
                    d = text::yearFromText(v.toString());
                    ok = std::isfinite(d);
                }
                if (!ok) return QStringLiteral("la valeur de « %1 » n'est pas un nombre : « %2 »").arg(it.key(), v.toString());
                v = d;
            } else if (!v.isDouble()) {
                return QStringLiteral("la valeur de « %1 » doit être un nombre").arg(it.key());
            }
        } else if (type == "string") {
            if (!v.isString()) v = QString::number(v.toDouble());
            const QString t = v.toString().trimmed();
            if (t.isEmpty()) continue;
            v = t;
        }
        out[it.key()] = v;
    }
    args = out;
    return {};
}

QStringList missing(const ToolDef& def, const QJsonObject& a)
{
    QStringList m;
    for (const QJsonValue& r : def.parameters.value("required").toArray())
        if (!a.contains(r.toString())) m << r.toString();
    if (def.methodId == "relation") {
        // Termes nécessaires selon l'opération (« comparer_dates » : a, b ; « analyser_traces » : from, to ; « of » pour C).
        const bool cmp = def.name == QLatin1String("comparer_dates");
        const QString A = cmp ? QStringLiteral("a") : QStringLiteral("from"), B = cmp ? QStringLiteral("b") : QStringLiteral("to");
        const QString op = a.value("op").toString();
        m.removeAll(A);
        m.removeAll(B);
        if ((op == QLatin1String("value") || op == QLatin1String("inside")) && !a.contains("of")) m << "of";
        const bool listed = op == QLatin1String("correlation") && a.value("terms").toArray().size() >= 2;
        if (op != QLatin1String("value") && !listed) { if (!a.contains(A)) m << A; if (!a.contains(B)) m << B; }
    }
    if (def.methodId == "curve") {
        const QString op = a.value("op").toString();
        // derivative / acceleration sans t : SeldonH donne la date de la vitesse (accélération) la plus forte (op extremum).
        if (op == QLatin1String("value") && !a.contains("t")) m << "t";
        if (op == QLatin1String("rate")) { if (!a.contains("from")) m << "from"; if (!a.contains("to")) m << "to"; }
        if ((op == QLatin1String("crossing") || op == QLatin1String("calibrate")) && !a.contains("y")) m << "y";
        if (op == QLatin1String("rate_values")) { if (!a.contains("y1")) m << "y1"; if (!a.contains("y2")) m << "y2"; }
    }
    if (def.methodId == "inverse" && !a.contains("t") && !(a.contains("t_min") && a.contains("t_max"))) m << "t";
    if (def.methodId == "inverse" && a.value("type").toString() == QLatin1String("am") && !a.contains("component")) m << "component";
    if (def.methodId == "am") {
        const QString p = a.value("process").toString();
        if ((p.contains('I') || p.contains('D')) && !a.contains("alpha95")) m << "alpha95";
        if (p.contains('I') && !a.contains("inc")) m << "inc";
        if (p.contains('D') && !a.contains("dec")) m << "dec";
        if (p.contains('F')) {
            if (!a.contains("field")) m << "field";
            if (!a.contains("error_f")) m << "error_f";
        }
    }
    return m;
}

QString curvesText(const QString& which)
{
    struct Row { const char* id; const char* title; const char* def; };
    const Row rows[] = { { "c14", "Calibration radiocarbone (calibrer_radiocarbone)", "intcal20" },
                         { "f14c", "F14C post-bombe (calibrer_f14c ; accepte aussi les courbes 14C)", "bomb21nhz1" },
                         { "am", "Archéomagnétisme (dater_archeomagnetisme)", "gal2002sph2014_i / _d, gwh2013uni_f" },
                         { "gauss", "Plugin Gauss : courbes de référence (densite_gauss avec curve)", "aucune" },
                         { "density", "Plugin Density : densités tabulées (densite_tabulee)", "aucune" } };
    QString t;
    for (const Row& r : rows) {
        if (!which.isEmpty() && which != r.id) continue;
        QStringList n = CurveStore::names(r.id);
        if (QString(r.id) == "f14c") n += CurveStore::names("c14");
        t += QStringLiteral("%1 — défaut : %2\n  %3\n").arg(QString::fromUtf8(r.title), QString::fromUtf8(r.def),
                                                              n.isEmpty() ? QStringLiteral("(aucune)") : n.join(", "));
    }
    return t.isEmpty() ? QStringLiteral("Méthode inconnue : « %1 » (c14, f14c, am, gauss ou density).").arg(which) : t.trimmed();
}

} // namespace

const QVector<ToolDef>& all()
{
    static const QVector<ToolDef> defs = build();
    return defs;
}

const ToolDef* find(const QString& name)
{
    for (const ToolDef& d : all())
        if (d.name == name) return &d;
    return nullptr;
}

QJsonArray schemaForApi()
{
    QJsonArray a;
    for (const ToolDef& d : all())
        a.append(QJsonObject{ { "type", "function" },
                              { "function", QJsonObject{ { "name", d.name }, { "description", d.description }, { "parameters", d.parameters } } } });
    return a;
}

ToolOutcome execute(const QString& name, const QString& argumentsJson, DateFormat format)
{
    ToolOutcome r;
    const ToolDef* def = find(name.trimmed());
    if (!def) {
        QStringList ns;
        for (const ToolDef& d : all()) ns << d.name;
        r.content = QStringLiteral("Outil inconnu : « %1 ». Outils disponibles : %2.").arg(name, ns.join(", "));
        return r;
    }
    r.known = true;

    QJsonObject args;
    const QString raw = argumentsJson.trimmed();
    if (!raw.isEmpty()) {
        QJsonParseError pe;
        const QJsonDocument d = QJsonDocument::fromJson(raw.toUtf8(), &pe);
        if (!d.isObject()) {
            r.content = QStringLiteral("Arguments invalides pour %1 : un objet JSON est attendu%2.")
                            .arg(def->name, pe.error != QJsonParseError::NoError ? QStringLiteral(" (%1)").arg(pe.errorString()) : QString());
            return r;
        }
        args = d.object();
    }

    if (def->methodId.isEmpty()) {              // outil d'information
        const QString bad = coerce(*def, args);
        if (!bad.isEmpty()) { r.content = QStringLiteral("Arguments invalides pour %1 : %2.").arg(def->name, bad); return r; }
        r.content = curvesText(args.value("method").toString().toLower());
        return r;
    }
    QString why;
    const QJsonObject req = requestFor(def->name, args, &why);
    if (req.isEmpty()) { r.content = why; return r; }
    r.run = MethodRunner::run({ req }, format);
    r.executed = r.run.found;
    r.request = req;
    r.content = r.run.promptBlock;
    return r;
}

QJsonObject requestFor(const QString& name, QJsonObject args, QString* error)
{
    const ToolDef* def = find(name);
    auto failWith = [&](const QString& why) { if (error) *error = why; return QJsonObject(); };
    if (!def) return failWith(QStringLiteral("Outil inconnu : %1.").arg(name));
    if (def->methodId.isEmpty()) return failWith(QStringLiteral("%1 est un outil d'information, pas un calcul.").arg(def->name));
    const QString bad = coerce(*def, args);
    if (!bad.isEmpty()) return failWith(QStringLiteral("Arguments invalides pour %1 : %2.").arg(def->name, bad));

    if (def->methodId == "event" || def->methodId == "combine") {
        // Datations normalisées (alias de méthode, format .chr, nombres en texte) avant la vérification.
        QJsonArray ds;
        for (const QJsonValue& v : args.value("dates").toArray())
            ds.append(v.isObject() ? QJsonValue(MethodRunner::normalizeRequest(v.toObject())) : v);
        args["dates"] = ds;
        if (ds.isEmpty()) {
            return failWith(QStringLiteral("Calcul non lancé : « dates » doit contenir au moins une datation (objets avec « method »)."));
        }
        for (int i = 0; i < ds.size(); ++i)
            if (!ds[i].isObject() || ds[i].toObject().value("method").toString().isEmpty()) {
                return failWith(QStringLiteral("Calcul non lancé : la datation %1 n'a pas de « method » (c14, f14c, tl, gauss, unif ou am).").arg(i + 1));
            }
    }
    if (def->methodId == "relation") {
        // Terme : datation (objet, ou objet JSON écrit en texte), colonne des traces ou année fixe (texte).
        for (const char* k : { "a", "b", "from", "to", "of" }) {
            const QJsonValue v = args.value(k);
            if (v.isString() && v.toString().trimmed().startsWith('{')) {
                const QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8());
                if (d.isObject()) args[k] = d.object();
            }
            if (args.value(k).isObject()) {
                args[k] = MethodRunner::normalizeRequest(args.value(k).toObject());
                if (args.value(k).toObject().value("method").toString().isEmpty()) {
                    return failWith(QStringLiteral("Calcul non lancé : « %1 » doit être une datation avec « method » (c14, f14c, tl, gauss, unif, am, "
                                               "density, event ou combine) et ses valeurs, une colonne des traces jointes ou une année.")
                                    .arg(QString::fromLatin1(k)));
                }
            }
        }
        if (args.value("terms").isArray()) {                        // « correlation » : termes en liste
            QJsonArray t;
            for (QJsonValue x : args.value("terms").toArray()) {
                if (x.isString() && x.toString().trimmed().startsWith('{')) {
                    const QJsonDocument d = QJsonDocument::fromJson(x.toString().toUtf8());
                    if (d.isObject()) x = d.object();
                }
                t.append(x.isObject() ? QJsonValue(MethodRunner::normalizeRequest(x.toObject())) : x);
            }
            args["terms"] = t;
        }
        if (def->name == QLatin1String("comparer_dates") && args.value("op").toString().trimmed().isEmpty()) args["op"] = QStringLiteral("order");
    }
    if (def->methodId == "am" && args.contains("process")) args["process"] = args.value("process").toString().toUpper();
    if (args.contains("curve")) args["curve"] = args.value("curve").toString().toLower();

    const QStringList miss = missing(*def, args);
    if (!miss.isEmpty()) {
        return failWith(QStringLiteral("Calcul non lancé : il manque %1. Demande ces valeurs à l'utilisateur ; n'en invente aucune.")
                            .arg(miss.join(", ")));
    }

    QJsonObject req = args;
    req["method"] = def->methodId;
    if (def->methodId == "c14" && req.contains("delta_r")) req["has_delta_r"] = true;
    if (def->methodId == "gauss" && !req.value("curve").toString().trimmed().isEmpty()) req["mode"] = QStringLiteral("curve");
    if (def->methodId == "am") req["integration_steps"] = 500;

    return req;
}

bool callFromText(const QString& text, QString& name, QJsonObject& args)
{
    // Nom d'un outil en mot entier, avec un « _ » (« tableau » seul est un mot ordinaire) ; le premier trouvé.
    static const QRegularExpression word(QStringLiteral(R"(\b([a-z][a-z0-9]*(?:_[a-z0-9]+)+)\b)"));
    qsizetype after = -1;
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        if (const ToolDef* d = find(m.captured(1)); d && !d->methodId.isEmpty()) { name = d->name; after = m.capturedEnd(); break; }
    }
    if (after < 0) return false;
    args = QJsonObject();
    const QString rest = text.mid(after);
    // Objet JSON après le nom : du premier « { » à l'accolade fermante correspondante.
    if (const qsizetype b = rest.indexOf('{'); b >= 0) {
        int depth = 0;
        bool inStr = false;
        for (qsizetype i = b; i < rest.size(); ++i) {
            const QChar c = rest[i];
            if (c == '"' && (i == 0 || rest[i - 1] != '\\')) inStr = !inStr;
            if (inStr) continue;
            if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                const QJsonDocument d = QJsonDocument::fromJson(rest.mid(b, i - b + 1).toUtf8());
                if (d.isObject()) { args = d.object(); return true; }
                break;
            }
        }
    }
    // Paires clé = valeur (ou clé : valeur) : nombre, texte entre guillemets ou mot.
    static const QRegularExpression kv(QStringLiteral(
        R"RX(\b([a-z_][a-z0-9_]*)\s*[=:]\s*(?:"([^"]*)"|'([^']*)'|(-?\d+(?:[.,]\d+)?)(?![\w.,]*\w)|([A-Za-z_][\w.\-]*)))RX"));
    for (auto it = kv.globalMatch(rest); it.hasNext();) {
        const auto m = it.next();
        const QString k = m.captured(1);
        if (!m.captured(4).isEmpty()) { QString n = m.captured(4); n.replace(',', '.'); args[k] = n.toDouble(); }
        else if (!m.captured(2).isEmpty() || !m.captured(3).isEmpty()) args[k] = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
        else {
            const QString v = m.captured(5);
            if (v == QLatin1String("true") || v == QLatin1String("false")) args[k] = v == QLatin1String("true");
            else args[k] = v;
        }
    }
    return true;
}

QJsonObject mergePlots(const QVector<QJsonObject>& plots)
{
    QJsonObject merged;
    QJsonArray series;
    QStringList names;
    QStringList dateNames;
    bool dating = true;              // tous des graphiques de datation (clé « dates ») : titre refait avec tous les noms
    bool sameLevel = true;
    for (const QJsonObject& p : plots) {
        if (p.isEmpty()) continue;
        if (!p.contains("dates")) dating = false;
        for (const QJsonValue& n : p.value("dates").toArray())
            if (!dateNames.contains(n.toString())) dateNames << n.toString();
        if (merged.isEmpty()) merged = p;
        else if (p.value("hpd").toDouble(95) != merged.value("hpd").toDouble(95)) sameLevel = false;
        for (const QJsonValue& s : p.value("series").toArray()) {
            const QString n = s.toObject().value("name").toString();
            if (!n.isEmpty() && names.contains(n)) continue;          // même datation déjà tracée
            names << n;
            series.append(s);
        }
    }
    if (!merged.isEmpty()) {
        merged["series"] = series;
        if (!sameLevel) merged["hpd"] = 95;                            // niveaux différents : 95 % pour toutes
        if (dating) {
            merged["dates"] = QJsonArray::fromStringList(dateNames);
            merged["title"] = datingPlotTitle(dateNames, int(series.size()));
        }
    }
    return merged;
}

QString datingPlotTitle(const QStringList& names, int seriesCount)
{
    if (names.isEmpty()) return QStringLiteral("Densités calendaires (calcul SeldonH)");
    constexpr int kMax = 4;                                            // titre lisible : au plus 4 noms
    QString list = QStringList(names.mid(0, kMax)).join(", ");
    if (names.size() > kMax) list += QStringLiteral(", … (+%1)").arg(names.size() - kMax);
    return (seriesCount <= 1 ? QStringLiteral("Densité calendaire — %1") : QStringLiteral("Densités calendaires — %1")).arg(list);
}

} // namespace tools
} // namespace seldon
