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

#include "LlmCore.h"

#include "llama.h"
#ifdef SELDON_GGML_DL
#include "ggml-backend.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace seldon {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void logCallback(ggml_log_level level, const char* /*text*/, void* /*user*/)
{
    // On ne garde que les erreurs, et on les ignore ici : LlmCore remonte ses propres messages.
    (void)level;
}

// Longueur du plus long préfixe de s composé de séquences UTF-8 complètes.
size_t utf8CompletePrefix(const std::string& s)
{
    const size_t n = s.size();
    size_t i = n;
    for (int back = 0; i > 0 && back < 4; ++back) {
        const unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0xC0) == 0x80) { --i; continue; }          // octet de continuation
        size_t need = 1;
        if      ((c >> 5) == 0x6)  need = 2;
        else if ((c >> 4) == 0xE)  need = 3;
        else if ((c >> 3) == 0x1E) need = 4;
        const size_t have = n - (i - 1);
        return have >= need ? n : i - 1;
    }
    return n;
}

} // namespace

#ifdef SELDON_GGML_DL
// llama.cpp compilé avec GGML_BACKEND_DL : les backends sont des bibliothèques séparées, chargées ici.
//   CPU    : une variante par jeu d'instructions (x64, sse42, haswell, skylakex, zen4…), la meilleure est choisie ;
//   Vulkan : chargé seulement si le pilote graphique fournit vulkan-1.dll / libvulkan.so.1, sinon ignoré.
// Recherche : dossier demandé, puis dossier llama.cpp de développement, puis dossier de l'exécutable.
static void loadBackends(const std::string& dir)
{
    if (!dir.empty()) ggml_backend_load_all_from_path(dir.c_str());
#ifdef SELDON_LLAMA_DIR
    if (ggml_backend_reg_count() == 0) ggml_backend_load_all_from_path(SELDON_LLAMA_DIR);
#endif
    if (ggml_backend_reg_count() == 0) ggml_backend_load_all();
}
#endif

LlmCore::LlmCore() = default;
LlmCore::~LlmCore() { unload(); }

bool LlmCore::load(const LlmParams& p, std::string& err, const std::function<bool(float)>& onProgress)
{
    unload();

    static std::once_flag once;
    std::call_once(once, [&p] {
        llama_log_set(logCallback, nullptr);
#ifdef SELDON_GGML_DL
        loadBackends(p.backendDir);
#endif
        llama_backend_init();
    });
#ifdef SELDON_GGML_DL
    if (ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU) == nullptr) {
        err = "Bibliothèques de calcul introuvables (ggml-cpu-*) : réinstaller SeldonH.";
        return false;
    }
#endif

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = p.nGpuLayers;
    nGpuLayers_ = p.nGpuLayers;

    struct Prog { const std::function<bool(float)>* cb; } prog{ &onProgress };
    if (onProgress) {
        mp.progress_callback = [](float v, void* ud) -> bool {
            auto* pr = static_cast<Prog*>(ud);
            return (*pr->cb)(v);
        };
        mp.progress_callback_user_data = &prog;
    }

    model_ = llama_model_load_from_file(p.modelPath.c_str(), mp);
    if (!model_) {
        err = "Impossible de charger le modèle : " + p.modelPath;
        return false;
    }
    vocab_ = llama_model_get_vocab(model_);

    const int trainCtx = llama_model_n_ctx_train(model_);
    nCtx_ = p.nCtx > 0 ? p.nCtx : 4096;
    if (trainCtx > 0) nCtx_ = std::min(nCtx_, trainCtx);
    nBatch_ = std::max(32, std::min(p.nBatch, nCtx_));

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx           = static_cast<uint32_t>(nCtx_);
    cp.n_batch         = static_cast<uint32_t>(nBatch_);
    cp.n_ubatch        = static_cast<uint32_t>(nBatch_);
    // "Automatique" : llama.cpp n'utilise que 4 threads par défaut. On prend les cœurs disponibles
    // moins deux (laisser de la place à l'interface et au système), au moins 1.
    int threads = p.nThreads;
    if (threads <= 0) {
        const int hw = static_cast<int>(std::thread::hardware_concurrency());
        threads = hw > 4 ? hw - 2 : std::max(1, hw);
    }
    cp.n_threads       = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = true;
    cp.op_offload = (p.nGpuLayers != 0);     // mode CPU : aucune opération déléguée au GPU, même pour le prompt

    ctx_ = llama_init_from_model(model_, cp);
    if (!ctx_) {
        err = "Impossible de créer le contexte (mémoire insuffisante ? essayez un contexte plus petit).";
        llama_model_free(model_);
        model_ = nullptr;
        vocab_ = nullptr;
        return false;
    }

    const char* tmpl = llama_model_chat_template(model_, nullptr);
    chatTemplate_ = tmpl ? tmpl : "";
    chatML_ = chatTemplate_.find("<|im_start|>") != std::string::npos;
    templateHasThinkSwitch_ = chatTemplate_.find("enable_thinking") != std::string::npos;
    cached_.clear();
    return true;
}

void LlmCore::unload()
{
    if (ctx_)   { llama_free(ctx_);          ctx_ = nullptr; }
    if (model_) { llama_model_free(model_);  model_ = nullptr; }
    vocab_ = nullptr;
    cached_.clear();
    chatTemplate_.clear();
}

void LlmCore::resetCache()
{
    cached_.clear();
    if (ctx_) llama_memory_clear(llama_get_memory(ctx_), true);
}

std::string LlmCore::description() const
{
    if (!model_) return {};
    char buf[256] = {0};
    llama_model_desc(model_, buf, sizeof(buf));
    return buf;
}

ModelInfo LlmCore::info() const
{
    ModelInfo m;
    if (!model_) return m;
    m.description  = description();
    m.contextSize  = nCtx_;
    m.trainContext = llama_model_n_ctx_train(model_);
    m.nLayer       = llama_model_n_layer(model_);
    m.nEmbd        = llama_model_n_embd(model_);
    m.nHead        = llama_model_n_head(model_);
    m.nHeadKv      = llama_model_n_head_kv(model_);
    m.modelBytes   = llama_model_size(model_);
    m.nParams      = llama_model_n_params(model_);

    // Périphérique : premier GPU (dédié ou intégré) si des couches y sont confiées, sinon le processeur (RAM).
    ggml_backend_dev_t chosen = nullptr;
    if (nGpuLayers_ != 0) {
        for (size_t i = 0; i < ggml_backend_dev_count() && !chosen; ++i) {
            ggml_backend_dev_t d = ggml_backend_dev_get(i);
            const auto t = ggml_backend_dev_type(d);
            if (t == GGML_BACKEND_DEVICE_TYPE_GPU || t == GGML_BACKEND_DEVICE_TYPE_IGPU) chosen = d;
        }
    }
    m.onGpu = chosen != nullptr;
    if (!chosen) chosen = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (chosen) {
        size_t freeMem = 0, totalMem = 0;
        ggml_backend_dev_memory(chosen, &freeMem, &totalMem);
        m.memTotal = totalMem;          // Metal : taille de travail recommandée (≈ 2/3 à 3/4 de la mémoire unifiée)
        const char* name = ggml_backend_dev_description(chosen);
        m.device = name ? name : "";
    }
    return m;
}

int LlmCore::trainContextSize() const
{
    return model_ ? llama_model_n_ctx_train(model_) : 0;
}

std::vector<llama_token> LlmCore::tokenize(const std::string& text) const
{
    // add_special = false : le modèle de chat contient déjà ses balises ; BOS ajouté à la main.
    int n = -llama_tokenize(vocab_, text.c_str(), static_cast<int32_t>(text.size()),
                            nullptr, 0, false, true);
    if (n <= 0) n = 0;
    std::vector<llama_token> tok(static_cast<size_t>(n));
    if (n > 0) {
        const int r = llama_tokenize(vocab_, text.c_str(), static_cast<int32_t>(text.size()),
                                     tok.data(), n, false, true);
        if (r < 0) tok.clear();
    }
    if (llama_vocab_get_add_bos(vocab_)) {
        const llama_token bos = llama_vocab_bos(vocab_);
        if (bos >= 0 && (tok.empty() || tok.front() != bos)) tok.insert(tok.begin(), bos);
    }
    return tok;
}

int LlmCore::countTokens(const std::string& text) const
{
    return isLoaded() ? static_cast<int>(tokenize(text).size()) : 0;
}

std::string LlmCore::pieceOf(llama_token t) const
{
    char buf[256];
    int n = llama_token_to_piece(vocab_, t, buf, sizeof(buf), 0, false);
    if (n < 0) {
        std::string big(static_cast<size_t>(-n), '\0');
        n = llama_token_to_piece(vocab_, t, big.data(), static_cast<int32_t>(big.size()), 0, false);
        if (n < 0) return {};
        big.resize(static_cast<size_t>(n));
        return big;
    }
    return std::string(buf, static_cast<size_t>(n));
}

std::string LlmCore::formatPrompt(const std::vector<ChatMessage>& chat, bool enableThinking,
                                  bool* endsInThink) const
{
    if (endsInThink) *endsInThink = false;

    auto chatml = [&]() {
        std::string s;
        for (const auto& m : chat)
            s += "<|im_start|>" + m.role + "\n" + m.content + "<|im_end|>\n";
        s += "<|im_start|>assistant\n";
        if (templateHasThinkSwitch_) {
            if (enableThinking) {
                s += "<think>\n";
                if (endsInThink) *endsInThink = true;
            } else {
                s += "<think>\n\n</think>\n\n";   // même résultat que enable_thinking=false
            }
        }
        return s;
    };

    if (chatML_ || chatTemplate_.empty()) return chatml();

    std::vector<llama_chat_message> msgs;
    msgs.reserve(chat.size());
    size_t total = 0;
    for (const auto& m : chat) {
        msgs.push_back({ m.role.c_str(), m.content.c_str() });
        total += m.content.size() + m.role.size() + 16;
    }
    std::vector<char> buf(std::max<size_t>(total * 2, 1024));
    int n = llama_chat_apply_template(chatTemplate_.c_str(), msgs.data(), msgs.size(), true,
                                      buf.data(), static_cast<int32_t>(buf.size()));
    if (n > static_cast<int>(buf.size())) {
        buf.resize(static_cast<size_t>(n));
        n = llama_chat_apply_template(chatTemplate_.c_str(), msgs.data(), msgs.size(), true,
                                      buf.data(), static_cast<int32_t>(buf.size()));
    }
    if (n < 0) return chatml();     // gabarit non reconnu : repli sur ChatML
    return std::string(buf.data(), static_cast<size_t>(n));
}

llama_sampler* LlmCore::makeSampler(const SamplingParams& sp) const
{
    llama_sampler* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (sp.temperature <= 0.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
        return chain;
    }
    if (sp.repeatPenalty != 1.0f || sp.presencePenalty != 0.0f) {
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), sp.penaltyLastN, sp.repeatPenalty, 0.0f, sp.presencePenalty));
    }
    if (sp.topK > 0)       llama_sampler_chain_add(chain, llama_sampler_init_top_k(sp.topK));
    if (sp.topP < 1.0f)    llama_sampler_chain_add(chain, llama_sampler_init_top_p(sp.topP, 1));
    if (sp.minP > 0.0f)    llama_sampler_chain_add(chain, llama_sampler_init_min_p(sp.minP, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(sp.temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(sp.seed));
    return chain;
}

bool LlmCore::decodeTokens(const llama_token* tok, int n, std::string& err)
{
    int done = 0;
    while (done < n) {
        if (stop_.load()) return true;                       // arrêt demandé : état cohérent
        const int chunk = std::min(nBatch_, n - done);
        llama_batch b = llama_batch_get_one(const_cast<llama_token*>(tok + done), chunk);
        const int rc = llama_decode(ctx_, b);
        if (rc != 0) {
            err = rc == 1 ? "Contexte saturé pendant l'évaluation du prompt."
                          : "Erreur d'évaluation llama_decode (code " + std::to_string(rc) + ").";
            llama_memory_clear(llama_get_memory(ctx_), true);
            cached_.clear();
            return false;
        }
        cached_.insert(cached_.end(), tok + done, tok + done + chunk);
        done += chunk;
    }
    return true;
}

bool LlmCore::generate(const std::vector<ChatMessage>& chat, const SamplingParams& sp,
                       const std::function<bool(const std::string&)>& onPiece,
                       std::string& err, GenStats* stats,
                       const std::function<void(const GenStats&)>& onStart)
{
    GenStats st;
    if (!isLoaded()) { err = "Aucun modèle chargé."; return false; }
    if (chat.empty() || chat.back().role != "user") { err = "La conversation doit se terminer par un message utilisateur."; return false; }
    stop_.store(false);

    // --- 1. Prompt, avec troncature des plus anciens échanges si nécessaire ---
    std::vector<ChatMessage> msgs = chat;
    const int maxNew = std::max(16, std::min(sp.maxTokens, nCtx_ / 2));
    const int budget = nCtx_ - maxNew;
    bool endsInThink = false;
    std::vector<llama_token> prompt;
    for (;;) {
        prompt = tokenize(formatPrompt(msgs, sp.enableThinking, &endsInThink));
        if (static_cast<int>(prompt.size()) <= budget) break;
        const size_t first = (!msgs.empty() && msgs[0].role == "system") ? 1 : 0;
        if (msgs.size() - first <= 1) break;                  // il ne reste que la question
        msgs.erase(msgs.begin() + static_cast<long>(first));  // on retire le plus ancien message...
        ++st.droppedMessages;
        while (msgs.size() - first > 1 && msgs[first].role != "user") {   // ...et on réaligne sur un "user"
            msgs.erase(msgs.begin() + static_cast<long>(first));
            ++st.droppedMessages;
        }
    }
    if (prompt.empty()) { err = "Prompt vide après tokenisation."; return false; }
    if (static_cast<int>(prompt.size()) >= nCtx_ - 16) {
        err = "La question (avec ses connaissances associées) dépasse la taille du contexte ("
              + std::to_string(nCtx_) + " tokens). Augmentez le contexte dans les réglages.";
        return false;
    }
    st.promptTokens = static_cast<int>(prompt.size());
    st.startsInThink = endsInThink;

    // --- 2. Réutilisation du cache KV (préfixe commun) ---
    size_t common = 0;
    const size_t lim = std::min(cached_.size(), prompt.size() - 1);   // on garde ≥ 1 token à évaluer
    while (common < lim && cached_[common] == prompt[common]) ++common;
    llama_memory_t mem = llama_get_memory(ctx_);
    if (common < cached_.size()) {
        if (!llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1)) {
            llama_memory_clear(mem, true);                    // modèles récurrents : retrait partiel impossible
            common = 0;
        }
        cached_.resize(common);
    }
    st.reusedTokens = static_cast<int>(common);

    // --- 3. Évaluation du prompt ---
    auto t0 = Clock::now();
    if (!decodeTokens(prompt.data() + common, static_cast<int>(prompt.size() - common), err)) return false;
    st.promptMs = msSince(t0);
    if (stop_.load()) {
        st.stoppedByUser = true;
        if (stats) *stats = st;
        return true;
    }

    // --- 4. Génération ---
    if (onStart) onStart(st);
    llama_sampler* smpl = makeSampler(sp);
    std::string pending;
    t0 = Clock::now();
    bool ok = true;
    int limit = std::min(maxNew, nCtx_ - static_cast<int>(prompt.size()) - 1);
    bool eog = false;

    for (int i = 0; i < limit; ++i) {
        if (stop_.load()) { st.stoppedByUser = true; break; }

        llama_token tok = llama_sampler_sample(smpl, ctx_, -1);
        if (llama_vocab_is_eog(vocab_, tok)) { eog = true; break; }

        pending += pieceOf(tok);
        const size_t cut = utf8CompletePrefix(pending);
        if (cut > 0) {
            const std::string out = pending.substr(0, cut);
            pending.erase(0, cut);
            if (onPiece && !onPiece(out)) { st.stoppedByUser = true; ++st.generated; break; }
        }

        llama_batch b = llama_batch_get_one(&tok, 1);
        const int rc = llama_decode(ctx_, b);
        if (rc != 0) {
            err = rc == 1 ? "Contexte saturé." : "Erreur llama_decode (code " + std::to_string(rc) + ").";
            llama_memory_clear(mem, true);
            cached_.clear();
            ok = false;
            break;
        }
        cached_.push_back(tok);
        ++st.generated;
        if (i == limit - 1 && static_cast<int>(cached_.size()) >= nCtx_ - 1) st.contextFull = true;
    }
    if (ok && !pending.empty() && onPiece) onPiece(pending);   // octets résiduels
    // Boucle terminée sans jeton de fin ni arrêt : la réponse est coupée. Limite demandée atteinte, ou limite réduite
    // par la place restante dans le contexte (ou par la moitié du contexte, plafond de maxNew).
    if (ok && !eog && !st.stoppedByUser && st.generated >= limit) {
        st.tokenLimit = std::max(0, limit);
        st.truncation = (limit >= sp.maxTokens) ? GenStats::TokenLimit : GenStats::ContextFull;
    }
    st.genMs = msSince(t0);
    llama_sampler_free(smpl);

    if (stats) *stats = st;
    return ok;
}

} // namespace seldon
