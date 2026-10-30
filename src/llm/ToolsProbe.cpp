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

#include "ToolsProbe.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>
#include <QVariantMap>

namespace seldon {
namespace toolprobe {

QByteArray requestBody(const QString& model, bool useMaxCompletionTokens, bool omitTemperature)
{
    const QJsonObject tool{
        { "type", "function" },
        { "function", QJsonObject{
              { "name", QString::fromLatin1(kToolName) },
              { "description", QStringLiteral("Outil de test de SeldonH : à appeler quand l'utilisateur le demande.") },
              { "parameters", QJsonObject{
                    { "type", "object" },
                    { "properties", QJsonObject{ { "valeur", QJsonObject{ { "type", "number" },
                                                                         { "description", QStringLiteral("Nombre à transmettre.") } } } } },
                    { "required", QJsonArray{ "valeur" } } } } } } };
    QJsonObject o;
    o["model"] = model.trimmed();
    o["messages"] = QJsonArray{
        QJsonObject{ { "role", "system" }, { "content", QStringLiteral("Tu es un assistant qui utilise les outils disponibles.") } },
        QJsonObject{ { "role", "user" }, { "content", QStringLiteral("Test automatique : appelle l'outil %1 avec valeur = 42. "
                                                                     "N'écris rien d'autre.").arg(QString::fromLatin1(kToolName)) } } };
    o["stream"] = false;
    if (!omitTemperature) o["temperature"] = 0.0;
    o[useMaxCompletionTokens ? "max_completion_tokens" : "max_tokens"] = 400;   // un modèle qui réfléchit d'abord en a besoin
    o["tools"] = QJsonArray{ tool };
    o["tool_choice"] = QStringLiteral("auto");
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

namespace {

QString shortText(QString s)
{
    s = s.simplified();
    return s.size() > 160 ? s.left(157) + QStringLiteral("…") : s;
}

// Appels et texte d'une réponse complète, ou d'un flux SSE si le serveur diffuse malgré « stream: false ».
void readAnswer(const QByteArray& body, QVector<ToolCall>& calls, QString& text)
{
    if (body.trimmed().startsWith("data:")) {
        remote::SseParser p;
        remote::ToolCallAccumulator acc;
        QVector<SseEvent> evs = p.feed(body);
        evs += p.finish();
        for (const SseEvent& e : evs) {
            text += e.text;
            for (const ToolCallDelta& d : e.toolCalls) acc.add(d);
        }
        calls = acc.calls();
        return;
    }
    calls = remote::toolCallsFromFullResponse(body);
    text = remote::textFromFullResponse(body);
}

bool mentionsTools(const QString& lowerDetail)
{
    return lowerDetail.contains(QLatin1String("tool")) || lowerDetail.contains(QLatin1String("function"));
}

} // namespace

ToolProbeResult classify(int httpStatus, const QByteArray& body, const QString& networkError)
{
    ToolProbeResult r;
    r.when = QDateTime::currentDateTimeUtc();
    if (httpStatus == 0) {
        r.support = ToolSupport::Unknown;
        r.detail = networkError.isEmpty() ? QStringLiteral("pas de réponse du serveur") : networkError;
        return r;
    }
    if (httpStatus >= 400) {
        const QString msg = remote::errorMessage(body, httpStatus);
        const QString low = QString::fromUtf8(body).toLower();
        // 401/403/404/429 : clé, adresse, modèle ou quota ; rien à conclure sur les outils.
        const bool toolsRelated = (httpStatus == 400 || httpStatus == 422 || httpStatus == 500 || httpStatus == 501) && mentionsTools(low);
        r.support = toolsRelated ? ToolSupport::Refused : ToolSupport::Unknown;
        r.detail = msg;
        return r;
    }
    QVector<ToolCall> calls;
    QString text;
    readAnswer(body, calls, text);
    for (const ToolCall& c : calls)
        if (c.name.trimmed() == QLatin1String(kToolName)) {
            r.support = ToolSupport::Native;
            r.detail = QStringLiteral("%1(%2)").arg(c.name, shortText(c.arguments));
            return r;
        }
    if (!calls.isEmpty()) {                       // un appel, mais d'un autre outil : le mécanisme marche
        r.support = ToolSupport::Native;
        r.detail = QStringLiteral("%1(%2)").arg(calls.first().name, shortText(calls.first().arguments));
        return r;
    }
    if (text.contains(QLatin1String(kToolName)) || text.contains(QLatin1String("\"valeur\"")) || text.contains(QLatin1String("<tool_call>"))) {
        r.support = ToolSupport::TextOnly;
        r.detail = shortText(text);
        return r;
    }
    r.support = ToolSupport::Ignored;
    r.detail = text.trimmed().isEmpty() ? QStringLiteral("réponse vide") : shortText(text);
    return r;
}

bool wantsRetry(int httpStatus, const QByteArray& body, bool& useMaxCompletionTokens, bool& omitTemperature)
{
    if (httpStatus != 400 && httpStatus != 422) return false;
    const QString low = QString::fromUtf8(body).toLower();
    if (!useMaxCompletionTokens && low.contains(QLatin1String("max_tokens"))) { useMaxCompletionTokens = true; return true; }
    if (!omitTemperature && low.contains(QLatin1String("temperature"))) { omitTemperature = true; return true; }
    return false;
}

namespace {
QString settingsKey(const QString& baseUrl, const QString& model)
{
    const QByteArray id = (remote::normalizeBase(baseUrl) + QLatin1Char('|') + model.trimmed()).toUtf8();
    return QStringLiteral("remote/toolsProbe/") + QString::fromLatin1(id.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
} // namespace

QString supportId(ToolSupport s)
{
    switch (s) {
    case ToolSupport::Native:   return QStringLiteral("native");
    case ToolSupport::TextOnly: return QStringLiteral("text");
    case ToolSupport::Ignored:  return QStringLiteral("ignored");
    case ToolSupport::Refused:  return QStringLiteral("refused");
    default:                    return QStringLiteral("unknown");
    }
}

ToolSupport supportFromId(const QString& id)
{
    if (id == QLatin1String("native")) return ToolSupport::Native;
    if (id == QLatin1String("text")) return ToolSupport::TextOnly;
    if (id == QLatin1String("ignored")) return ToolSupport::Ignored;
    if (id == QLatin1String("refused")) return ToolSupport::Refused;
    return ToolSupport::Unknown;
}

bool cached(const QString& baseUrl, const QString& model, ToolProbeResult& out)
{
    if (baseUrl.trimmed().isEmpty() || model.trimmed().isEmpty()) return false;
    const QVariantMap m = QSettings().value(settingsKey(baseUrl, model)).toMap();
    if (m.isEmpty()) return false;
    out.support = supportFromId(m.value("support").toString());
    out.detail = m.value("detail").toString();
    out.when = QDateTime::fromString(m.value("when").toString(), Qt::ISODate);
    return out.conclusive();
}

void store(const QString& baseUrl, const QString& model, const ToolProbeResult& r)
{
    if (!r.conclusive() || baseUrl.trimmed().isEmpty() || model.trimmed().isEmpty()) return;    // un échec réseau n'efface rien
    QVariantMap m;
    m["support"] = supportId(r.support);
    m["detail"] = r.detail;
    m["when"] = (r.when.isValid() ? r.when : QDateTime::currentDateTimeUtc()).toString(Qt::ISODate);
    QSettings().setValue(settingsKey(baseUrl, model), m);
}

bool isFresh(const ToolProbeResult& r, int days)
{
    return r.when.isValid() && r.when.daysTo(QDateTime::currentDateTimeUtc()) <= days;
}

QString describe(const ToolProbeResult& r, bool html)
{
    QString t, color;
    switch (r.support) {
    case ToolSupport::Native:
        t = QStringLiteral("✅ Appel d'outils natif accepté : le modèle demande lui-même les calculs de SeldonH.");
        color = QStringLiteral("#1b7f3b");
        break;
    case ToolSupport::TextOnly:
        t = QStringLiteral("⚠ Le modèle écrit l'appel dans sa réponse au lieu d'utiliser « tool_calls » (serveur sans analyseur d'outils ?) : "
                           "SeldonH passe par le bloc de calcul.");
        color = QStringLiteral("#b35c00");
        break;
    case ToolSupport::Ignored:
        t = QStringLiteral("⚠ Le serveur accepte les outils mais le modèle n'en appelle aucun : SeldonH passe par le bloc de calcul.");
        color = QStringLiteral("#b35c00");
        break;
    case ToolSupport::Refused:
        t = QStringLiteral("⚠ Le serveur refuse l'appel d'outils : SeldonH passe par le bloc de calcul.");
        color = QStringLiteral("#b35c00");
        break;
    default:
        t = QStringLiteral("Test impossible");
        color = QStringLiteral("#b00020");
        break;
    }
    QString extra;
    if (!r.detail.isEmpty()) extra = QStringLiteral(" (%1)").arg(r.detail);
    if (r.when.isValid() && r.conclusive())
        extra += QStringLiteral(" — testé le %1").arg(r.when.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm")));
    if (!html) return t + extra;
    return QStringLiteral("<span style='color:%1'>%2</span><small>%3</small>").arg(color, t.toHtmlEscaped(), extra.toHtmlEscaped());
}

bool wantTools(const QString& mode, const ToolProbeResult* c)
{
    if (mode == QLatin1String("off")) return false;
    if (mode == QLatin1String("on")) return true;
    return !(c && c->conclusive()) || c->usesNativeTools();
}

} // namespace toolprobe

ToolsProbe::ToolsProbe(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<seldon::ToolProbeResult>("seldon::ToolProbeResult");
}

void ToolsProbe::start(const RemoteConfig& cfg)
{
    if (reply_) { reply_->disconnect(this); reply_->abort(); reply_->deleteLater(); }
    cfg_ = cfg;
    maxCompletion_ = omitTemperature_ = retried_ = false;
    if (!cfg_.isValid()) {
        ToolProbeResult r;
        r.detail = QStringLiteral("Base URL ou nom du modèle manquant");
        emit finished(r);
        return;
    }
    send();
}

void ToolsProbe::send()
{
    QNetworkRequest rq(remote::chatUrl(cfg_.baseUrl));
    rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QString key = cfg_.apiKey.trimmed();
    if (key.isEmpty()) key = QProcessEnvironment::systemEnvironment().value(QStringLiteral("SELDONH_API_KEY")).trimmed();
    if (!key.isEmpty()) rq.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    rq.setTransferTimeout(timeoutMs_);
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);      // voir RemoteEngine::send
    QNetworkReply* r = nam_.post(rq, toolprobe::requestBody(cfg_.model, maxCompletion_, omitTemperature_));
    reply_ = r;
    connect(r, &QNetworkReply::finished, this, [this, r] {
        if (reply_ != r) { r->deleteLater(); return; }
        reply_ = nullptr;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        const QString netErr = r->error() != QNetworkReply::NoError ? r->errorString() : QString();
        r->deleteLater();
        if (!retried_ && toolprobe::wantsRetry(status, body, maxCompletion_, omitTemperature_)) {
            retried_ = true;
            send();
            return;
        }
        const ToolProbeResult res = toolprobe::classify(status, body, netErr);
        toolprobe::store(cfg_.baseUrl, cfg_.model, res);
        emit finished(res);
    });
}

} // namespace seldon
