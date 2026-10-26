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

#include "RemoteEngine.h"

#include <QMetaType>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>

namespace seldon {

RemoteEngine::RemoteEngine(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<seldon::ToolCall>("seldon::ToolCall");
    qRegisterMetaType<QVector<seldon::ToolCall>>("QVector<seldon::ToolCall>");
    qRegisterMetaType<seldon::ToolResult>("seldon::ToolResult");
    qRegisterMetaType<QVector<seldon::ToolResult>>("QVector<seldon::ToolResult>");
    qRegisterMetaType<seldon::GenStats>("seldon::GenStats");
}

void RemoteEngine::setConfig(const RemoteConfig& cfg)
{
    if (cfg.baseUrl != cfg_.baseUrl || cfg.model != cfg_.model) toolsUnsupported_ = false;
    cfg_ = cfg;
}

void RemoteEngine::generate(const QVector<ChatMessage>& chat, const SamplingParams& sp)
{
    if (reply_) return;
    if (!cfg_.isValid()) { emit failed(tr("Modèle distant non configuré : renseignez la Base URL et le nom du modèle dans les Préférences.")); return; }
    chat_ = chat;
    sp_ = sp;
    retried_ = false;
    toolsRetried_ = false;
    extra_ = QJsonArray();
    toolRounds_ = 0;
    pendingCalls_.clear();
    send();
}

void RemoteEngine::submitToolResults(const QVector<ToolResult>& results)
{
    if (reply_ || pendingCalls_.isEmpty()) return;
    extra_.append(remote::assistantToolMessage(pendingText_, pendingReasoning_, pendingCalls_));
    for (const ToolCall& c : pendingCalls_) {
        ToolResult r{ c.id, c.name, QStringLiteral("Aucun résultat.") };
        for (const ToolResult& x : results) if (x.callId == c.id) r = x;
        extra_.append(remote::toolResultMessage(r));       // un message « tool » pour chaque appel, dans l'ordre
    }
    pendingCalls_.clear();
    ++toolRounds_;
    retried_ = false;
    send();
}

void RemoteEngine::send()
{
    sse_ = remote::SseParser();
    errBody_.clear();
    fullBody_.clear();
    streamError_.clear();
    finishReason_.clear();
    promptTokens_ = completionTokens_ = pieces_ = 0;
    stopped_ = thinkOpen_ = gotDone_ = false;
    acc_.clear();
    roundText_.clear();
    roundReasoning_.clear();

    QNetworkRequest rq(remote::chatUrl(cfg_.baseUrl));
    rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    rq.setRawHeader("Accept", "text/event-stream");
    QString key = cfg_.apiKey.trimmed();
    if (key.isEmpty()) key = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SELDONH_API_KEY")).trimmed();
    if (!key.isEmpty()) rq.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    rq.setTransferTimeout(120000);                    // 2 min sans aucun octet reçu
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // HTTP/1.1 : la pile HTTP/2 de Qt 6 lit le socket TLS après sa fermeture par le serveur (fin du flux SSE,
    // connexion inactive) et affiche « QIODevice::read (QSslSocket): device not open ». Gain d'HTTP/2 nul ici.
    rq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    timer_.start();
    const bool withTools = toolsActive();
    sentTools_ = withTools;
    reply_ = nam_.post(rq, remote::buildChatBody(cfg_, chat_, sp_, useMaxCompletionTokens_, omitTemperature_,
                                                 withTools ? tools_ : QJsonArray(), extra_, toolRounds_ >= kMaxToolRounds));
    connect(reply_, &QNetworkReply::readyRead, this, &RemoteEngine::onReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &RemoteEngine::onReplyFinished);
    emit started(false, 0, 0);
}

void RemoteEngine::stop()
{
    if (!reply_) return;
    stopped_ = true;
    reply_->abort();                                  // déclenche finished()
}

void RemoteEngine::closeThinkIfOpen()
{
    if (thinkOpen_) { thinkOpen_ = false; emit token(QStringLiteral("</think>\n")); }
}

void RemoteEngine::handleEvents(const QVector<SseEvent>& events)
{
    for (const SseEvent& ev : events) {
        if (!ev.error.isEmpty()) { streamError_ = ev.error; continue; }
        if (ev.done) { gotDone_ = true; continue; }
        if (!ev.finishReason.isEmpty()) finishReason_ = ev.finishReason;
        if (ev.promptTokens >= 0) promptTokens_ = ev.promptTokens;
        if (ev.completionTokens >= 0) completionTokens_ = ev.completionTokens;
        for (const ToolCallDelta& d : ev.toolCalls) acc_.add(d);
        if (!ev.reasoning.isEmpty()) {
            roundReasoning_ += ev.reasoning;
            if (!thinkOpen_) { thinkOpen_ = true; emit token(QStringLiteral("<think>\n")); }
            emit token(ev.reasoning);
            ++pieces_;
        }
        if (!ev.text.isEmpty()) {
            closeThinkIfOpen();
            roundText_ += ev.text;
            emit token(ev.text);
            ++pieces_;
        }
    }
}

void RemoteEngine::onReadyRead()
{
    if (!reply_) return;
    if (!reply_->isOpen()) return;                    // réponse déjà fermée (arrêt, erreur) : rien à lire
    const QByteArray chunk = reply_->readAll();
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) { errBody_ += chunk; return; }
    fullBody_ += chunk;                               // conservé au cas où le serveur ne diffuse pas
    handleEvents(sse_.feed(chunk));
}

void RemoteEngine::fail(const QString& msg)
{
    emit failed(msg);
}

void RemoteEngine::onReplyFinished()
{
    QNetworkReply* r = reply_;
    if (!r) return;
    reply_.clear();
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netErr = r->error();
    const QString netErrString = r->errorString();
    const QByteArray rest = r->isOpen() ? r->readAll() : QByteArray();      // fermée après un arrêt ou une erreur de connexion
    r->deleteLater();

    if (status >= 400) errBody_ += rest; else { fullBody_ += rest; handleEvents(sse_.feed(rest)); }

    if (stopped_) {
        closeThinkIfOpen();
        GenStats st;
        st.generated = completionTokens_ > 0 ? completionTokens_ : pieces_;
        st.promptTokens = promptTokens_;
        st.genMs = timer_.elapsed();
        st.stoppedByUser = true;
        emit finished(st);
        return;
    }

    if (status >= 400) {
        // Variantes de paramètres refusées par certains modèles : une seule nouvelle tentative.
        const QString detail = QString::fromUtf8(errBody_).toLower();
        // Serveur ou modèle sans appel d'outils : on réessaie sans « tools » (le bloc ```calcul prend le relais).
        if (sentTools_ && !toolsRetried_ && (status == 400 || status == 404 || status == 422 || status == 500) &&
            (detail.contains("tool") || detail.contains("function"))) {
            toolsRetried_ = true;
            toolsUnsupported_ = true;
            extra_ = QJsonArray();
            emit toolsUnavailable(remote::errorMessage(errBody_, status));
            send();
            return;
        }
        if (status == 400 && !retried_) {
            bool changed = false;
            if (!useMaxCompletionTokens_ && detail.contains("max_completion_tokens")) { useMaxCompletionTokens_ = true; changed = true; }
            if (!omitTemperature_ && detail.contains("temperature")) { omitTemperature_ = true; changed = true; }
            if (changed) { retried_ = true; send(); return; }
        }
        fail(remote::errorMessage(errBody_, status));
        return;
    }
    if (netErr != QNetworkReply::NoError) {
        closeThinkIfOpen();
        fail(tr("Connexion au modèle distant impossible : %1").arg(netErrString));
        return;
    }

    handleEvents(sse_.finish());
    if (!streamError_.isEmpty() && pieces_ == 0) { fail(streamError_); return; }

    if (!sse_.sawEvent() && pieces_ == 0) {           // réponse non diffusée
        int pt = -1, ct = -1;
        const QString text = remote::textFromFullResponse(fullBody_, &pt, &ct, &finishReason_);
        const QVector<ToolCall> fullCalls = remote::toolCallsFromFullResponse(fullBody_);
        for (const ToolCall& c : fullCalls) acc_.add(ToolCallDelta{ -1, c.id, c.name, c.arguments, c.extra });
        if (text.isEmpty() && fullCalls.isEmpty()) { fail(tr("Réponse vide ou illisible du serveur : %1").arg(QString::fromUtf8(fullBody_.left(300)).simplified())); return; }
        if (pt >= 0) promptTokens_ = pt;
        if (ct >= 0) completionTokens_ = ct;
        if (!text.isEmpty()) { roundText_ += text; emit token(text); ++pieces_; }
    }
    closeThinkIfOpen();

    GenStats st;
    st.generated = completionTokens_ > 0 ? completionTokens_ : pieces_;
    st.promptTokens = promptTokens_;
    st.genMs = timer_.elapsed();
    // Réponse coupée : limite de tokens du serveur, erreur au milieu du flux, ou flux terminé sans [DONE] ni finish_reason.
    if (finishReason_ == QLatin1String("length")) {
        st.truncation = GenStats::TokenLimit;
        st.tokenLimit = sp_.maxTokens;
    } else if (pieces_ > 0 && !streamError_.isEmpty()) {
        st.truncation = GenStats::StreamCut;
        st.truncationDetail = streamError_.toStdString();
    } else if (pieces_ > 0 && sse_.sawEvent() && !gotDone_ && finishReason_.isEmpty()) {
        st.truncation = GenStats::StreamCut;
    }

    const QVector<ToolCall> calls = acc_.calls();
    if (!calls.isEmpty() && sentTools_ && toolRounds_ < kMaxToolRounds) {
        pendingCalls_ = calls;
        pendingText_ = roundText_;
        pendingReasoning_ = roundReasoning_;
        emit toolCallsRequested(calls, roundText_, st);
        return;
    }
    emit finished(st);
}

} // namespace seldon
