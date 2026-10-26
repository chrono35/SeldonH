// RemoteApi : outils purs (sans réseau) pour parler à un modèle distant via une API « Chat Completions »
// compatible OpenAI (OpenAI, Mistral, OpenRouter, Groq, Together, Ollama /v1, LM Studio, llama-server, vLLM…).
// Séparés du moteur réseau pour être testables sans connexion.
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "LlmCore.h"      // ChatMessage, SamplingParams

namespace seldon {

struct RemoteConfig {
    QString baseUrl;      // ex. https://api.openai.com/v1
    QString apiKey;       // peut être vide (serveurs locaux)
    QString model;        // ex. gpt-4o-mini, mistral-small-latest, qwen3:8b
    bool    isValid() const { return !baseUrl.trimmed().isEmpty() && !model.trimmed().isEmpty(); }
};

// Appel d'outil demandé par le modèle (API « tools »). `extra` : champs propres à certains fournisseurs
// (par ex. extra_content) à renvoyer tels quels avec le message.
struct ToolCall {
    QString     id;
    QString     name;
    QString     arguments;     // chaîne JSON, telle que produite par le modèle
    QJsonObject extra;
};

// Résultat d'un outil, renvoyé au modèle.
struct ToolResult {
    QString callId;
    QString name;
    QString content;
};

// Fragment d'appel d'outil dans le flux (le nom et les arguments arrivent en morceaux).
struct ToolCallDelta {
    int         index = -1;
    QString     id;
    QString     name;
    QString     arguments;
    QJsonObject extra;
};

// Morceau décodé du flux SSE.
struct SseEvent {
    QString text;                 // fragment de réponse
    QString reasoning;            // fragment de raisonnement (reasoning_content), s'il existe
    int     promptTokens = -1;    // si le serveur fournit « usage »
    int     completionTokens = -1;
    bool    done = false;         // « data: [DONE] »
    QString error;                // message d'erreur renvoyé dans le flux
    QVector<ToolCallDelta> toolCalls;   // fragments d'appels d'outils
};

namespace remote {

// « https://api.openai.com/v1/ », « …/v1/chat/completions » ou « http://localhost:11434 » -> base sans barre finale
// (« /v1 » ajouté quand l'adresse n'a pas de chemin).
QString normalizeBase(const QString& baseUrl);
QUrl    chatUrl(const QString& baseUrl);
QUrl    modelsUrl(const QString& baseUrl);
QString hostOf(const QString& baseUrl);              // « api.openai.com » (pour l'affichage et l'avertissement)
// Vrai si une clé non vide partirait en clair : adresse en http:// vers une autre machine que celle-ci
// (localhost, 127.x.x.x et ::1 ne quittent pas l'ordinateur ; un serveur du réseau local, si).
bool    sendsKeyInClear(const QString& baseUrl, const QString& apiKey);

// Corps JSON d'une requête de génération en flux. Certains modèles refusent `max_tokens` (→ `max_completion_tokens`)
// ou `temperature` : le moteur réessaie une fois avec ces variantes.
// `tools` : schémas des outils (voir tools::schemaForApi), vide = pas d'appel d'outils. `toolChoiceNone` : les outils
// restent déclarés mais le modèle ne peut plus en appeler (dernier tour). `extraMessages` : messages bruts ajoutés après
// la conversation (appel d'outil de l'assistant + résultats, rôle « tool »).
QByteArray buildChatBody(const RemoteConfig& cfg, const QVector<ChatMessage>& chat, const SamplingParams& sp,
                         bool useMaxCompletionTokens = false, bool omitTemperature = false,
                         const QJsonArray& tools = QJsonArray(), const QJsonArray& extraMessages = QJsonArray(),
                         bool toolChoiceNone = false);

// Message « assistant » qui porte les appels d'outils, et message « tool » de résultat.
QJsonObject assistantToolMessage(const QString& text, const QString& reasoning, const QVector<ToolCall>& calls);
QJsonObject toolResultMessage(const ToolResult& r);

// Assemble les fragments d'appels d'outils d'un flux. Tolère l'absence d'index ou d'id.
class ToolCallAccumulator {
public:
    void add(const ToolCallDelta& d);
    bool isEmpty() const { return slots_.isEmpty(); }
    QVector<ToolCall> calls() const;      // seuls les appels qui ont un nom
    void clear() { slots_.clear(); indexes_.clear(); }
private:
    QVector<ToolCall> slots_;
    QVector<int>      indexes_;
};

// Décodeur incrémental du flux « text/event-stream ».
class SseParser {
public:
    QVector<SseEvent> feed(const QByteArray& chunk);     // peut être appelé avec des morceaux coupés n'importe où
    QVector<SseEvent> finish();                           // dernier morceau sans saut de ligne final
    bool sawEvent() const { return saw_; }
private:
    QVector<SseEvent> parseLine(const QByteArray& line);
    QByteArray buf_;
    bool saw_ = false;
};

// Appels d'outils d'une réponse complète non diffusée.
QVector<ToolCall> toolCallsFromFullResponse(const QByteArray& body);

// Réponse complète non diffusée (serveur qui ignore stream:true) -> texte ; vide si absent.
QString textFromFullResponse(const QByteArray& body, int* promptTokens = nullptr, int* completionTokens = nullptr);

// Message d'erreur lisible à partir du corps JSON d'une réponse HTTP d'erreur.
QString errorMessage(const QByteArray& body, int httpStatus);

// Identifiants de modèles d'une réponse GET /models.
QStringList parseModelList(const QByteArray& body);

} // namespace remote
} // namespace seldon
