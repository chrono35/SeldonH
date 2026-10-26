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

// RemoteEngine : génération par un modèle distant (API Chat Completions compatible OpenAI, réponse en flux SSE).
// Mêmes signaux que LlmEngine (started, token, finished, failed) pour s'insérer dans MainWindow ; vit dans le thread
// de l'interface (le réseau est asynchrone, rien ne bloque).
#pragma once

#include <QElapsedTimer>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>

#include "RemoteApi.h"

namespace seldon {

class RemoteEngine : public QObject {
    Q_OBJECT
public:
    explicit RemoteEngine(QObject* parent = nullptr);

    // Un changement de serveur ou de modèle réinitialise le constat « ce modèle n'accepte pas les outils ».
    void setConfig(const RemoteConfig& cfg);
    const RemoteConfig& config() const { return cfg_; }
    bool busy() const { return reply_ != nullptr; }

    // Appel d'outils natif : `tools` = schémas (tools::schemaForApi()). Vide = désactivé (le modèle utilise alors le
    // bloc ```calcul). Le moteur le désactive de lui-même si le serveur le refuse (toolsUnavailable).
    void setTools(const QJsonArray& tools) { tools_ = tools; }
    bool toolsActive() const { return !tools_.isEmpty() && !toolsUnsupported_; }
    static constexpr int kMaxToolRounds = 4;      // allers-retours outil -> modèle pour une même question

public slots:
    void generate(const QVector<seldon::ChatMessage>& chat, const seldon::SamplingParams& sp);
    void stop();
    // Renvoie au modèle les résultats des outils demandés (signal toolCallsRequested) et relance la génération.
    void submitToolResults(const QVector<seldon::ToolResult>& results);

signals:
    void started(bool startsInThink, int promptTokens, int reusedTokens);
    void token(const QString& piece);
    void finished(const seldon::GenStats& stats);
    void failed(const QString& error);
    // Le modèle demande des outils au lieu de répondre : exécutez-les puis appelez submitToolResults().
    // `text` : ce que le modèle a écrit avant ses appels (souvent vide).
    void toolCallsRequested(const QVector<seldon::ToolCall>& calls, const QString& text, const seldon::GenStats& stats);
    void toolsUnavailable(const QString& reason);

private slots:
    void onReadyRead();
    void onReplyFinished();

private:
    void send();
    void handleEvents(const QVector<SseEvent>& events);
    void closeThinkIfOpen();
    void fail(const QString& msg);

    QNetworkAccessManager nam_;
    RemoteConfig          cfg_;
    QPointer<QNetworkReply> reply_;

    QVector<ChatMessage>  chat_;
    SamplingParams        sp_;
    remote::SseParser     sse_;
    QByteArray            errBody_;        // corps de la réponse en cas d'erreur HTTP
    QByteArray            fullBody_;       // corps entier quand le serveur ne diffuse pas
    QElapsedTimer         timer_;
    int                   promptTokens_ = 0;
    int                   completionTokens_ = 0;
    int                   pieces_ = 0;
    bool                  stopped_ = false;
    bool                  thinkOpen_ = false;
    bool                  gotDone_ = false;
    bool                  useMaxCompletionTokens_ = false;   // mémorisés d'une requête à l'autre
    bool                  omitTemperature_ = false;
    bool                  retried_ = false;
    QString               streamError_;
    QString               finishReason_;   // dernier finish_reason reçu (« length » = réponse coupée)

    // Appel d'outils
    QJsonArray            tools_;
    bool                  toolsUnsupported_ = false;
    bool                  toolsRetried_ = false;
    bool                  sentTools_ = false;    // la requête en cours déclare des outils
    QJsonArray            extra_;                // messages assistant(tool_calls) + tool de la question en cours
    int                   toolRounds_ = 0;
    remote::ToolCallAccumulator acc_;
    QString               roundText_, roundReasoning_;
    QVector<ToolCall>     pendingCalls_;
    QString               pendingText_, pendingReasoning_;
};

} // namespace seldon
