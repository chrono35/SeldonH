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

// LlmCore : enveloppe C++ pure (aucune dépendance Qt) autour de l'API C de llama.cpp.
//
// Responsabilités :
//   - charger un modèle GGUF (Metal sur macOS) et créer le contexte ;
//   - formater la conversation avec le modèle de chat du modèle (ChatML maison pour Qwen,
//     llama_chat_apply_template pour Gemma / Llama / Mistral ...) ;
//   - générer en streaming avec réutilisation du cache KV (préfixe commun) ;
//   - modèles récurrents ou hybrides (Qwen3.5, Mamba…) : points de reprise de l'état récurrent, sans lesquels
//     le cache ne peut pas être raccourci et tout le prompt serait réévalué à chaque question ;
//   - tronquer proprement l'historique si le contexte est plein.
//
// Cette classe n'est PAS thread-safe, sauf requestStop() qui peut être appelée de n'importe
// quel thread. Elle est conçue pour vivre dans un thread dédié (voir LlmEngine).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_vocab;
struct llama_sampler;
typedef int32_t llama_token;

namespace seldon {

struct ChatMessage {
    std::string role;     // "system" | "user" | "assistant"
    std::string content;
};

struct LlmParams {
    std::string modelPath;
    int  nCtx       = 8192;   // taille du contexte (tokens)
    int  nBatch     = 512;    // taille de lot pour l'évaluation du prompt
    int  nThreads   = 0;      // 0 = automatique
    int  nGpuLayers = 999;    // 999 = tout sur le GPU (Metal, Vulkan) ; 0 = CPU seul
    // Windows / Linux : dossier des backends llama.cpp chargés dynamiquement (ggml-cpu-*.dll, ggml-vulkan.dll /
    // libggml-*.so). Vide = dossier de l'exécutable. Sans effet avec llama.cpp statique (macOS).
    std::string backendDir;
};

struct SamplingParams {
    float    temperature     = 0.7f;   // <= 0 : décodage glouton (valeurs Qwen3.5 mode instruct)
    int      topK            = 20;
    float    topP            = 0.8f;
    float    minP            = 0.0f;
    float    repeatPenalty   = 1.05f;
    float    presencePenalty = 0.0f;
    int      penaltyLastN    = 256;
    uint32_t seed            = 0xFFFFFFFFu;   // aléatoire
    int      maxTokens       = 1024;
    bool     enableThinking  = false;         // modèles "thinking" (Qwen3.5...)
};

struct GenStats {
    int    promptTokens   = 0;   // taille totale du prompt
    int    reusedTokens   = 0;   // tokens retrouvés dans le cache KV
    bool   fromCheckpoint = false;  // reusedTokens obtenu en restaurant un point de reprise (modèle hybride)
    int    generated      = 0;
    int    droppedMessages = 0;  // messages supprimés pour tenir dans le contexte
    double promptMs       = 0;
    double genMs          = 0;
    bool   stoppedByUser  = false;
    bool   contextFull    = false;
    bool   startsInThink  = false;  // le prompt se termine par "<think>\n" (mode réflexion)

    // Réponse coupée avant sa fin naturelle (ni jeton de fin, ni arrêt demandé par l'utilisateur) :
    //   TokenLimit  : « Longueur max. d'une réponse » atteinte (local), ou finish_reason = "length" (distant) ;
    //   ContextFull : plus de place dans le contexte pour continuer (local) ;
    //   StreamCut   : flux distant interrompu (erreur en cours de réponse, ou fin sans [DONE] ni finish_reason).
    enum Truncation { NotTruncated = 0, TokenLimit, ContextFull, StreamCut };
    int         truncation = NotTruncated;
    int         tokenLimit = 0;          // limite de tokens appliquée à cette réponse
    std::string truncationDetail;        // message d'erreur du flux (StreamCut), sinon vide
};

// Caractéristiques du modèle chargé et du périphérique de calcul (pour « Optimiser pour ce modèle »).
struct ModelInfo {
    std::string description;            // ex. "qwen35 4B Q4_K_M"
    int         contextSize  = 0;       // contexte créé (tokens)
    int         trainContext = 0;       // contexte d'entraînement du modèle
    int         nLayer = 0, nEmbd = 0, nHead = 0, nHeadKv = 0;
    uint64_t    modelBytes = 0;         // taille des poids
    uint64_t    nParams    = 0;
    bool        onGpu      = false;     // couches confiées à un GPU (Metal, Vulkan…)
    std::string device;                 // nom du périphérique
    uint64_t    memTotal   = 0;         // mémoire de ce périphérique (GPU ; sinon RAM) ; 0 = inconnue
};

class LlmCore {
public:
    LlmCore();
    ~LlmCore();
    LlmCore(const LlmCore&) = delete;
    LlmCore& operator=(const LlmCore&) = delete;

    // Charge le modèle. onProgress reçoit [0,1] ; retourner false annule le chargement.
    bool load(const LlmParams& p, std::string& err,
              const std::function<bool(float)>& onProgress = {});
    void unload();
    bool isLoaded() const { return model_ && ctx_; }

    // Génère la réponse à la dernière entrée de `chat` (rôle "user").
    // onPiece reçoit des fragments UTF-8 valides ; retourner false interrompt la génération.
    // Retourne false en cas d'erreur (err renseigné). Un arrêt demandé n'est pas une erreur.
    // onStart (optionnel) est appelé une fois le prompt évalué, juste avant le premier token :
    // stats->startsInThink indique si la sortie commence à l'intérieur d'un bloc de réflexion.
    bool generate(const std::vector<ChatMessage>& chat, const SamplingParams& sp,
                  const std::function<bool(const std::string&)>& onPiece,
                  std::string& err, GenStats* stats = nullptr,
                  const std::function<void(const GenStats&)>& onStart = {});

    void requestStop() { stop_.store(true); }

    // --- Informations / utilitaires ---
    std::string description() const;     // ex. "qwen35 4B Q4_K_M"
    ModelInfo   info() const;            // vide si aucun modèle
    int  contextSize() const { return nCtx_; }
    int  trainContextSize() const;
    int  countTokens(const std::string& text) const;
    void resetCache();                   // oublie le cache KV (nouvelle conversation)
    bool usesCheckpoints() const { return needCheckpoints_; }   // modèle récurrent / hybride
    size_t checkpointCount() const { return checkpoints_.size(); }
    size_t checkpointBytes() const;      // mémoire occupée par les points de reprise

    // Construit le prompt texte (exposé pour les tests).
    std::string formatPrompt(const std::vector<ChatMessage>& chat, bool enableThinking,
                             bool* endsInThink = nullptr) const;

private:
    std::vector<llama_token> tokenize(const std::string& text) const;
    std::string pieceOf(llama_token t) const;
    bool decodeTokens(const llama_token* tok, int n, std::string& err);
    llama_sampler* makeSampler(const SamplingParams& sp) const;
    void forgetCache();                  // vide le cache KV, cached_ et les points de reprise

    // --- Points de reprise (modèles récurrents / hybrides) ---
    // L'état d'une couche récurrente résume tous les tokens lus : on ne peut pas en retirer la fin
    // (llama_memory_seq_rm échoue). On sauvegarde donc l'état récurrent seul (LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY,
    // quelques dizaines de Mo) à des positions qui resteront communes au tour suivant ; les couches d'attention,
    // elles, se raccourcissent normalement. Même mécanisme que les « context checkpoints » de llama-server.
    struct Checkpoint { size_t nTokens = 0; std::vector<uint8_t> state; };
    std::string formatPrefix(const std::vector<ChatMessage>& chat, size_t n) const;   // n premiers messages
    std::vector<size_t> checkpointPositions(const std::vector<ChatMessage>& msgs,
                                            const std::vector<llama_token>& prompt) const;
    void saveCheckpoint(size_t nTokens);
    bool restoreCheckpoint(size_t maxTokens, size_t& restored);

    llama_model*         model_ = nullptr;
    llama_context*       ctx_   = nullptr;
    const llama_vocab*   vocab_ = nullptr;
    int                  nCtx_  = 0;
    int                  nBatch_ = 512;
    int                  nGpuLayers_ = 0;
    std::string          chatTemplate_;       // modèle Jinja embarqué dans le GGUF
    bool                 chatML_ = false;     // format ChatML géré en interne
    bool                 templateHasThinkSwitch_ = false;
    std::vector<llama_token> cached_;         // tokens présents dans le cache KV (séquence 0)
    bool                 needCheckpoints_ = false;   // modèle récurrent ou hybride
    std::vector<Checkpoint> checkpoints_;     // ordre de création ; nTokens <= cached_.size()
    static constexpr size_t kMaxCheckpoints = 3;   // ≈ 50 Mo chacun pour Qwen3.5-4B
    std::atomic<bool>    stop_{false};
};

} // namespace seldon
