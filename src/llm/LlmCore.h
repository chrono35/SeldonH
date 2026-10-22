// LlmCore : enveloppe C++ pure (aucune dépendance Qt) autour de l'API C de llama.cpp.
//
// Responsabilités :
//   - charger un modèle GGUF (Metal sur macOS) et créer le contexte ;
//   - formater la conversation avec le modèle de chat du modèle (ChatML maison pour Qwen,
//     llama_chat_apply_template pour Gemma / Llama / Mistral ...) ;
//   - générer en streaming avec réutilisation du cache KV (préfixe commun) ;
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
    int    generated      = 0;
    int    droppedMessages = 0;  // messages supprimés pour tenir dans le contexte
    double promptMs       = 0;
    double genMs          = 0;
    bool   stoppedByUser  = false;
    bool   contextFull    = false;
    bool   startsInThink  = false;  // le prompt se termine par "<think>\n" (mode réflexion)
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
    int  contextSize() const { return nCtx_; }
    int  trainContextSize() const;
    int  countTokens(const std::string& text) const;
    void resetCache();                   // oublie le cache KV (nouvelle conversation)

    // Construit le prompt texte (exposé pour les tests).
    std::string formatPrompt(const std::vector<ChatMessage>& chat, bool enableThinking,
                             bool* endsInThink = nullptr) const;

private:
    std::vector<llama_token> tokenize(const std::string& text) const;
    std::string pieceOf(llama_token t) const;
    bool decodeTokens(const llama_token* tok, int n, std::string& err);
    llama_sampler* makeSampler(const SamplingParams& sp) const;

    llama_model*         model_ = nullptr;
    llama_context*       ctx_   = nullptr;
    const llama_vocab*   vocab_ = nullptr;
    int                  nCtx_  = 0;
    int                  nBatch_ = 512;
    std::string          chatTemplate_;       // modèle Jinja embarqué dans le GGUF
    bool                 chatML_ = false;     // format ChatML géré en interne
    bool                 templateHasThinkSwitch_ = false;
    std::vector<llama_token> cached_;         // tokens présents dans le cache KV (séquence 0)
    std::atomic<bool>    stop_{false};
};

} // namespace seldon
