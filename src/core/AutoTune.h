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

// AutoTune : réglages proposés par « Optimiser pour ce modèle » (Préférences), calculés à partir de données
// mesurables (contexte du modèle, mémoire, coût du cache KV, vitesse mesurée), pas demandés au modèle de langage.
// Fonction pure, sans interface, testée dans tst_core.
#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>

namespace seldon {

struct TuneInput {
    bool     remote = false;
    bool     thinking = false;      // mode réflexion actif (modèle local)
    // Modèle local (ModelInfo)
    int      trainContext = 0;      // 0 = inconnu
    int      nLayer = 0, nEmbd = 0, nHead = 0, nHeadKv = 0;
    uint64_t modelBytes = 0;
    uint64_t memTotal = 0;          // mémoire du périphérique de calcul (GPU, sinon RAM) ; 0 = inconnue
    bool     onGpu = false;
    double   tokPerSec = 0;         // vitesse de génération mesurée ; 0 = inconnue
    // Modèle distant (liste /models du serveur)
    int      remoteContext = 0;     // 0 = inconnu
    int      remoteMaxOutput = 0;   // 0 = inconnu
};

struct TuneResult {
    int         nCtx = 0;           // taille du contexte (modèle local) ; 0 = sans objet (distant)
    int         maxTokens = 0;      // longueur max. d'une réponse
    int         skillChars = 0;     // budget des compétences (caractères)
    QStringList notes;              // explications affichées sous le bouton
};

// Octets de cache KV par token de contexte (K et V en f16, toutes les couches) ; 0 si les dimensions sont inconnues.
// Estimation prudente : les modèles hybrides (couches d'attention linéaire) en utilisent moins.
uint64_t kvBytesPerToken(int nLayer, int nEmbd, int nHead, int nHeadKv);

TuneResult autoTune(const TuneInput& in);

} // namespace seldon
