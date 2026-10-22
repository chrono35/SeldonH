// Outil en ligne de commande pour tester le moteur SANS Qt ni interface.
//
//   llm_cli modele.gguf                 conversation interactive (/reset, /quit)
//   llm_cli modele.gguf --selftest      vérifications automatiques (gabarit, cache KV, arrêt, troncature)
//   options : --ctx N  --max N  --think  --cpu
//
// Compilation (après scripts/build_llama.sh) :
//   macOS : c++ -std=c++17 -O2 -I third_party/llama-install/include tests/llm_cli.cpp src/llm/LlmCore.cpp
//             -L third_party/llama-install/lib -lllama -lggml -lggml-metal -lggml-blas -lggml-cpu -lggml-base
//             -framework Accelerate -framework Metal -framework MetalKit -framework Foundation -o llm_cli
//   Linux : c++ -std=c++17 -O2 -I third_party/llama-install/include tests/llm_cli.cpp src/llm/LlmCore.cpp
//             -L third_party/llama-install/lib -lllama -lggml -lggml-cpu -lggml-base -lgomp -ldl -lpthread -o llm_cli
//   (une seule ligne de commande : les retours à la ligne ci-dessus sont là pour la lisibilité)
#include "../src/core/ChatText.h"
#include "../src/llm/LlmCore.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace seldon;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { ++g_fail; std::printf("  [ECHEC] %s\n", msg); } else std::printf("  [ok]    %s\n", msg); } while (0)

static std::string runTurn(LlmCore& core, std::vector<ChatMessage>& chat, const std::string& user,
                           SamplingParams sp, GenStats& st, bool stream)
{
    chat.push_back({ "user", user });
    std::string out, err;
    const bool ok = core.generate(chat, sp, [&](const std::string& p) {
        out += p;
        if (stream) { std::fputs(p.c_str(), stdout); std::fflush(stdout); }
        return true;
    }, err, &st);
    if (!ok) { std::printf("\n[erreur] %s\n", err.c_str()); chat.pop_back(); return {}; }
    chat.push_back({ "assistant", seldon::splitThinking(out, st.startsInThink).answer });
    return out;
}

static int selftest(const std::string& model, int ctx)
{
    std::printf("== Auto-test du moteur ==\n");
    LlmCore core;
    std::string err;
    LlmParams lp; lp.modelPath = model; lp.nCtx = ctx; lp.nGpuLayers = 0;
    CHECK(core.load(lp, err), "chargement du modèle");
    if (!core.isLoaded()) { std::printf("%s\n", err.c_str()); return 1; }
    std::printf("  modèle : %s, contexte %d\n", core.description().c_str(), core.contextSize());

    std::vector<ChatMessage> chat = { { "system", "Tu es un assistant." } };

    // 1. Gabarit
    chat.push_back({ "user", "Bonjour" });
    bool think = false;
    std::string p0 = core.formatPrompt(chat, false, &think);
    CHECK(p0.find("<|im_start|>assistant\n<think>\n\n</think>\n\n") != std::string::npos && !think, "gabarit : réflexion désactivée");
    std::string p1 = core.formatPrompt(chat, true, &think);
    CHECK(p1.size() >= 8 && p1.compare(p1.size() - 8, 8, "<think>\n") == 0 && think, "gabarit : réflexion activée (finit par <think>)");
    chat.pop_back();
    CHECK(core.countTokens("Bonjour à tous, voici un été très chaud.") > 3, "tokenisation UTF-8");

    // 2. Génération + réutilisation du cache KV
    SamplingParams sp; sp.maxTokens = 24; sp.temperature = 0.8f;
    GenStats s1, s2;
    std::string a1 = runTurn(core, chat, "Quelle est la date de la fondation ?", sp, s1, false);
    CHECK(s1.generated > 0, "tour 1 : des tokens générés");
    CHECK(s1.reusedTokens == 0, "tour 1 : cache vide au départ");
    std::string a2 = runTurn(core, chat, "Et celle de l'abandon ?", sp, s2, false);
    CHECK(s2.generated > 0, "tour 2 : des tokens générés");
    CHECK(s2.reusedTokens > 5, "tour 2 : le préfixe du cache KV est réutilisé");
    std::printf("  tour 2 : prompt=%d, réutilisés=%d\n", s2.promptTokens, s2.reusedTokens);

    // 3. Arrêt demandé par le consommateur
    GenStats s3; int n = 0; std::string e3;
    chat.push_back({ "user", "Encore une question." });
    sp.maxTokens = 200;
    bool ok = core.generate(chat, sp, [&](const std::string&) { return ++n < 4; }, e3, &s3);
    chat.pop_back();
    CHECK(ok && s3.stoppedByUser && s3.generated <= 5, "arrêt par le consommateur (callback false)");

    // 4. requestStop() avant génération est ignoré (réarmé à chaque appel), puis fonctionne en cours
    GenStats s4; int m = 0; std::string e4;
    chat.push_back({ "user", "Une autre." });
    ok = core.generate(chat, sp, [&](const std::string&) { if (++m == 3) core.requestStop(); return true; }, e4, &s4);
    chat.pop_back();
    CHECK(ok && s4.stoppedByUser && s4.generated < 10, "requestStop() en cours de génération");

    // 5. Troncature de l'historique quand le contexte est plein
    LlmCore small;
    lp.nCtx = 512;
    CHECK(small.load(lp, err), "rechargement avec contexte 512");
    std::vector<ChatMessage> long_chat = { { "system", "Assistant." } };
    GenStats last; bool allOk = true;
    sp.maxTokens = 16;
    for (int i = 0; i < 14; ++i) {
        std::string q = "Question numéro " + std::to_string(i) + " : ";
        for (int k = 0; k < 20; ++k) q += "datation stratigraphie phase événement ";
        runTurn(small, long_chat, q, sp, last, false);
        if (last.generated == 0 && last.promptTokens == 0) allOk = false;
    }
    CHECK(allOk, "14 tours dans un contexte de 512 sans erreur");
    CHECK(last.droppedMessages > 0 && last.promptTokens <= 512, "anciens messages abandonnés, prompt ≤ contexte");
    std::printf("  dernier tour : prompt=%d tokens, messages abandonnés=%d\n", last.promptTokens, last.droppedMessages);

    std::printf("== %s ==\n", g_fail ? "ECHEC" : "TOUT EST OK");
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s modele.gguf [--selftest] [--ctx N] [--max N] [--think] [--cpu]\n", argv[0]); return 2; }
    std::string model = argv[1];
    int ctx = 4096, maxTok = 512;
    bool self = false, think = false, cpu = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--selftest")) self = true;
        else if (!std::strcmp(argv[i], "--think")) think = true;
        else if (!std::strcmp(argv[i], "--cpu")) cpu = true;
        else if (!std::strcmp(argv[i], "--ctx") && i + 1 < argc) ctx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) maxTok = std::atoi(argv[++i]);
    }
    if (self) return selftest(model, ctx);

    LlmCore core;
    std::string err;
    LlmParams lp; lp.modelPath = model; lp.nCtx = ctx; lp.nGpuLayers = cpu ? 0 : 999;
    if (!core.load(lp, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("Modèle : %s (contexte %d). /reset pour recommencer, /quit pour sortir.\n", core.description().c_str(), core.contextSize());

    std::vector<ChatMessage> chat = { { "system", "Tu es un assistant d'aide à la datation en archéologie. Réponds en français." } };
    SamplingParams sp; sp.maxTokens = maxTok; sp.enableThinking = think;
    for (std::string line; std::printf("\n> "), std::fflush(stdout), std::getline(std::cin, line);) {
        if (line == "/quit") break;
        if (line == "/reset") { chat.resize(1); core.resetCache(); continue; }
        if (line.empty()) continue;
        GenStats st;
        runTurn(core, chat, line, sp, st, true);
        const double tps = st.genMs > 0 ? st.generated * 1000.0 / st.genMs : 0;
        std::printf("\n[prompt %d (cache %d), %d tokens, %.1f tok/s]\n", st.promptTokens, st.reusedTokens, st.generated, tps);
    }
    return 0;
}
