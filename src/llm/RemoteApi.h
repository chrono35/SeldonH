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

// RemoteApi : outils purs (sans réseau) pour parler à un modèle distant via une API « Chat Completions »
// compatible OpenAI (OpenAI, Mistral, OpenRouter, Groq, Together, Ollama /v1, LM Studio, llama-server, vLLM…).
// Séparés du moteur réseau pour être testables sans connexion.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
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
    QString finishReason;         // « stop », « length » (réponse coupée : limite de tokens), « tool_calls »… ; vide si absent
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
QString textFromFullResponse(const QByteArray& body, int* promptTokens = nullptr, int* completionTokens = nullptr,
                             QString* finishReason = nullptr);

// Message d'erreur lisible à partir du corps JSON d'une réponse HTTP d'erreur.
QString errorMessage(const QByteArray& body, int httpStatus);

// Identifiants de modèles d'une réponse GET /models.
QStringList parseModelList(const QByteArray& body);
// Limites annoncées par la liste /models, quand le serveur les donne (OpenRouter : context_length et
// top_provider.max_completion_tokens ; Mistral : max_context_length ; vLLM : max_model_len…). 0 = inconnu.
struct ModelLimits { int context = 0; int maxOutput = 0; };
QHash<QString, ModelLimits> parseModelLimits(const QByteArray& body);

// Quota restant annoncé dans les en-têtes HTTP d'une réponse : x-ratelimit-remaining-tokens / -requests et
// x-ratelimit-limit-* (OpenAI, Groq, DeepSeek, Together…), anthropic-ratelimit-tokens-remaining… -1 = absent.
struct RateLimits { int tokensRemaining = -1, tokensLimit = -1, requestsRemaining = -1, requestsLimit = -1; };
RateLimits parseRateLimits(const QList<QPair<QByteArray, QByteArray>>& headers);

} // namespace remote
} // namespace seldon
