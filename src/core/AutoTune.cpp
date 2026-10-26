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

#include "AutoTune.h"

#include "AppSettings.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>

namespace seldon {
namespace {

QString tr(const char* s) { return QCoreApplication::translate("AutoTune", s); }

constexpr int    kContexts[] = { 4096, 8192, 16384, 32768 };   // valeurs proposées dans les Préférences
constexpr double kCharsPerToken = 3.5;                          // texte français, tokeniseurs récents
constexpr double kSkillShare = 0.20;                            // part du contexte donnée aux compétences
constexpr uint64_t kMiB = 1024ull * 1024ull;

int roundTo(double v, int step) { return int(std::lround(v / step)) * step; }

QString gib(uint64_t bytes) { return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1); }

int skillBudget(int contextTokens, int maxChars)
{
    return std::clamp(roundTo(contextTokens * kSkillShare * kCharsPerToken, 500), 3000, maxChars);
}

} // namespace

uint64_t kvBytesPerToken(int nLayer, int nEmbd, int nHead, int nHeadKv)
{
    if (nLayer <= 0 || nEmbd <= 0 || nHead <= 0 || nHeadKv <= 0) return 0;
    const uint64_t headDim = uint64_t(nEmbd / nHead);
    return 2ull * uint64_t(nLayer) * uint64_t(nHeadKv) * headDim * 2ull;     // K + V, f16
}

TuneResult autoTune(const TuneInput& in)
{
    TuneResult r;

    if (in.remote) {
        const int ctx = in.remoteContext > 0 ? in.remoteContext : 32768;
        if (in.remoteContext > 0) r.notes << tr("Contexte du modèle distant : %1 tokens.").arg(in.remoteContext);
        else r.notes << tr("Contexte du modèle distant non indiqué par le serveur : 32 768 tokens supposés.");
        int maxTok = std::clamp(ctx / 4, 1024, 4096);
        if (in.remoteMaxOutput > 0) {
            maxTok = std::clamp(std::min(maxTok, in.remoteMaxOutput), 256, 4096);
            r.notes << tr("Réponse limitée par le serveur à %1 tokens.").arg(in.remoteMaxOutput);
        }
        r.maxTokens = maxTok;
        r.skillChars = skillBudget(ctx, 12000);       // plafond plus bas : chaque caractère envoyé est facturé et transmis
        return r;
    }

    // --- Modèle local : la plus grande taille de contexte qui tient en mémoire et que le modèle connaît.
    int cap = 32768;
    if (in.trainContext > 0) {
        cap = std::min(cap, in.trainContext);
        r.notes << tr("Contexte d'entraînement du modèle : %1 tokens.").arg(in.trainContext);
    }
    uint64_t kv = kvBytesPerToken(in.nLayer, in.nEmbd, in.nHead, in.nHeadKv);
    if (kv == 0) kv = 128 * 1024;                     // dimensions inconnues : valeur typique d'un modèle de 4 à 8 B
    if (in.memTotal > 0) {
        // GPU : on garde 15 % de marge ; mémoire système : 40 % pour le système et les autres applications.
        const double usable = double(in.memTotal) * (in.onGpu ? 0.85 : 0.60);
        const double avail = usable - double(in.modelBytes) - double(512 * kMiB);   // poids + tampons de calcul
        const int byMem = avail > 0 ? int(std::min(avail / double(kv), 1e9)) : 0;
        cap = std::min(cap, byMem);
        r.notes << tr("Mémoire %1 : %2 Go ; modèle : %3 Go ; cache : %4 Ko par token.")
                       .arg(in.onGpu ? tr("GPU") : tr("système"), gib(in.memTotal), gib(in.modelBytes))
                       .arg(kv / 1024);
    } else {
        cap = std::min(cap, 8192);
        r.notes << tr("Mémoire disponible inconnue : contexte limité à 8 192 tokens par prudence.");
    }
    if (in.tokPerSec > 0 && in.tokPerSec < 8.0) {
        cap = std::min(cap, 8192);
        r.notes << tr("Génération lente (%1 tokens/s) : contexte limité à 8 192 tokens pour garder des réponses rapides.")
                       .arg(in.tokPerSec, 0, 'f', 1);
    }
    r.nCtx = kContexts[0];
    for (int c : kContexts) if (c <= cap) r.nCtx = c;
    if (cap < kContexts[0]) r.notes << tr("Mémoire juste : contexte minimal proposé (4 096 tokens).");

    int maxTok = std::clamp(r.nCtx / 4, 1024, 4096);
    if (in.tokPerSec > 0) {
        const int byTime = std::max(1024, roundTo(in.tokPerSec * 120.0, 100));   // ≈ 2 minutes de génération au plus
        if (byTime < maxTok) {
            maxTok = byTime;
            r.notes << tr("Vitesse mesurée : %1 tokens/s ; réponse limitée à environ 2 minutes.").arg(in.tokPerSec, 0, 'f', 1);
        }
    }
    if (in.thinking)                                  // réflexion : réserve ajoutée, le tout ≤ moitié du contexte
        maxTok = std::max(1024, std::min(maxTok, r.nCtx / 2 - AppSettings::kThinkingReserve));
    r.maxTokens = maxTok;
    r.skillChars = skillBudget(r.nCtx, 20000);
    return r;
}

} // namespace seldon
