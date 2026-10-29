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

#include "RemoteApi.h"

#include <algorithm>
#include <limits>

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <initializer_list>

namespace seldon {
namespace remote {

QString normalizeBase(const QString& baseUrl)
{
    QString b = baseUrl.trimmed();
    while (b.endsWith('/')) b.chop(1);
    for (const QString& suffix : { QStringLiteral("/chat/completions"), QStringLiteral("/completions"), QStringLiteral("/models") })
        if (b.endsWith(suffix)) { b.chop(suffix.size()); break; }
    while (b.endsWith('/')) b.chop(1);
    if (!b.isEmpty() && !b.contains("://")) b.prepend("https://");
    const QUrl u(b);
    if (u.isValid() && !u.host().isEmpty() && (u.path().isEmpty() || u.path() == "/")) b = b + "/v1";
    return b;
}

QUrl chatUrl(const QString& baseUrl) { return QUrl(normalizeBase(baseUrl) + "/chat/completions"); }
QUrl modelsUrl(const QString& baseUrl) { return QUrl(normalizeBase(baseUrl) + "/models"); }
QString hostOf(const QString& baseUrl) { return QUrl(normalizeBase(baseUrl)).host(); }

bool sendsKeyInClear(const QString& baseUrl, const QString& apiKey)
{
    if (apiKey.trimmed().isEmpty()) return false;
    const QUrl u(normalizeBase(baseUrl));
    if (u.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0) return false;
    const QString host = u.host().toLower();
    if (host.isEmpty() || host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost"))) return false;
    const QHostAddress a(host);
    return a.isNull() || !a.isLoopback();
}

QByteArray buildChatBody(const RemoteConfig& cfg, const QVector<ChatMessage>& chat, const SamplingParams& sp,
                         bool useMaxCompletionTokens, bool omitTemperature,
                         const QJsonArray& tools, const QJsonArray& extraMessages, bool toolChoiceNone)
{
    QJsonArray msgs;
    for (const ChatMessage& m : chat) {
        if (m.role == "system" && m.content.empty()) continue;
        msgs.append(QJsonObject{ { "role", QString::fromStdString(m.role) }, { "content", QString::fromStdString(m.content) } });
    }
    for (const QJsonValue& m : extraMessages) msgs.append(m);
    QJsonObject o;
    o["model"] = cfg.model.trimmed();
    o["messages"] = msgs;
    o["stream"] = true;
    o["stream_options"] = QJsonObject{ { "include_usage", true } };
    if (!omitTemperature) o["temperature"] = std::clamp(double(sp.temperature), 0.0, 2.0);
    if (sp.maxTokens > 0) o[useMaxCompletionTokens ? "max_completion_tokens" : "max_tokens"] = sp.maxTokens;
    if (!tools.isEmpty()) {
        o["tools"] = tools;
        o["tool_choice"] = toolChoiceNone ? QStringLiteral("none") : QStringLiteral("auto");
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

namespace {
// Les serveurs stricts relisent les arguments comme du JSON : on renvoie toujours un objet valide.
QString validArguments(const QString& a)
{
    const QJsonDocument d = QJsonDocument::fromJson(a.toUtf8());
    return d.isObject() ? a : QStringLiteral("{}");
}
} // namespace

QJsonObject assistantToolMessage(const QString& text, const QString& reasoning, const QVector<ToolCall>& calls)
{
    QJsonArray tc;
    for (const ToolCall& c : calls) {
        QJsonObject o = c.extra;
        o["id"] = c.id;
        o["type"] = QStringLiteral("function");
        o["function"] = QJsonObject{ { "name", c.name }, { "arguments", validArguments(c.arguments) } };
        tc.append(o);
    }
    QJsonObject m{ { "role", "assistant" }, { "tool_calls", tc } };
    m["content"] = text.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(text);
    if (!reasoning.isEmpty()) m["reasoning_content"] = reasoning;      // exigé par certains modèles « à réflexion »
    return m;
}

QJsonObject toolResultMessage(const ToolResult& r)
{
    return { { "role", "tool" }, { "tool_call_id", r.callId }, { "content", r.content } };
}

void ToolCallAccumulator::add(const ToolCallDelta& d)
{
    int slot = -1;
    if (d.index >= 0) {
        slot = indexes_.indexOf(d.index);
    } else if (!d.id.isEmpty()) {
        for (int i = 0; i < slots_.size(); ++i) if (slots_[i].id == d.id) { slot = i; break; }
    } else if (!slots_.isEmpty()) {
        slot = slots_.size() - 1;                    // suite du dernier appel
    }
    if (slot < 0) {
        slots_.append(ToolCall());
        indexes_.append(d.index >= 0 ? d.index : -1 - slots_.size());
        slot = slots_.size() - 1;
    }
    ToolCall& c = slots_[slot];
    if (!d.id.isEmpty()) c.id = d.id;
    if (!d.name.isEmpty()) c.name += d.name;       // le nom peut arriver en morceaux
    c.arguments += d.arguments;
    for (auto it = d.extra.begin(); it != d.extra.end(); ++it) c.extra[it.key()] = it.value();
}

QVector<ToolCall> ToolCallAccumulator::calls() const
{
    QVector<ToolCall> out;
    for (const ToolCall& c : slots_) {
        if (c.name.trimmed().isEmpty()) continue;
        ToolCall x = c;
        x.name = x.name.trimmed();
        if (x.id.isEmpty()) x.id = QStringLiteral("call_%1").arg(out.size() + 1);
        out.append(x);
    }
    return out;
}

namespace {
ToolCallDelta deltaFrom(const QJsonObject& t, int position)
{
    ToolCallDelta d;
    d.index = t.contains("index") ? t.value("index").toInt(position) : -1;
    d.id = t.value("id").toString();
    const QJsonObject f = t.value("function").toObject();
    d.name = f.value("name").toString();
    const QJsonValue a = f.value("arguments");
    d.arguments = a.isString() ? a.toString() : (a.isObject() ? QString::fromUtf8(QJsonDocument(a.toObject()).toJson(QJsonDocument::Compact)) : QString());
    if (t.contains("extra_content")) d.extra["extra_content"] = t.value("extra_content");
    return d;
}
} // namespace

QVector<ToolCall> toolCallsFromFullResponse(const QByteArray& body)
{
    ToolCallAccumulator acc;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    if (!d.isObject()) return {};
    const QJsonArray choices = d.object().value("choices").toArray();
    if (choices.isEmpty()) return {};
    const QJsonArray tcs = choices[0].toObject().value("message").toObject().value("tool_calls").toArray();
    for (int i = 0; i < tcs.size(); ++i) {
        ToolCallDelta dl = deltaFrom(tcs[i].toObject(), i);
        dl.index = i;                                  // réponse complète : un emplacement par appel
        acc.add(dl);
    }
    return acc.calls();
}

QVector<SseEvent> SseParser::feed(const QByteArray& chunk)
{
    buf_ += chunk;
    QVector<SseEvent> out;
    int nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        QByteArray line = buf_.left(nl);
        buf_.remove(0, nl + 1);
        if (line.endsWith('\r')) line.chop(1);
        out += parseLine(line);
    }
    return out;
}

QVector<SseEvent> SseParser::finish()
{
    QVector<SseEvent> out;
    if (!buf_.trimmed().isEmpty()) out += parseLine(buf_.trimmed());
    buf_.clear();
    return out;
}

QVector<SseEvent> SseParser::parseLine(const QByteArray& line)
{
    QVector<SseEvent> out;
    if (!line.startsWith("data:")) return out;           // commentaires (« : keep-alive »), « event: », lignes vides
    saw_ = true;
    const QByteArray payload = line.mid(5).trimmed();
    if (payload.isEmpty()) return out;
    SseEvent ev;
    if (payload == "[DONE]") { ev.done = true; out << ev; return out; }

    const QJsonDocument d = QJsonDocument::fromJson(payload);
    if (!d.isObject()) return out;
    const QJsonObject o = d.object();
    if (o.contains("error")) {
        const QJsonValue e = o.value("error");
        ev.error = e.isObject() ? e.toObject().value("message").toString() : e.toString();
        if (ev.error.isEmpty()) ev.error = QStringLiteral("erreur renvoyée par le serveur");
        out << ev;
        return out;
    }
    const QJsonArray choices = o.value("choices").toArray();
    if (!choices.isEmpty()) {
        const QJsonObject delta = choices[0].toObject().value("delta").toObject();
        ev.finishReason = choices[0].toObject().value("finish_reason").toString();   // null -> chaîne vide
        ev.text = delta.value("content").toString();                       // null -> chaîne vide
        ev.reasoning = delta.value("reasoning_content").toString();
        if (ev.reasoning.isEmpty()) ev.reasoning = delta.value("reasoning").toString();
        const QJsonArray tcs = delta.value("tool_calls").toArray();
        for (int i = 0; i < tcs.size(); ++i) ev.toolCalls.append(deltaFrom(tcs[i].toObject(), i));
    }
    const QJsonObject usage = o.value("usage").toObject();
    if (!usage.isEmpty()) {
        ev.promptTokens = usage.value("prompt_tokens").toInt(-1);
        ev.completionTokens = usage.value("completion_tokens").toInt(-1);
    }
    if (!ev.text.isEmpty() || !ev.reasoning.isEmpty() || !ev.toolCalls.isEmpty() || ev.promptTokens >= 0 || ev.completionTokens >= 0
        || !ev.finishReason.isEmpty())
        out << ev;
    return out;
}

QHash<QString, ModelLimits> parseModelLimits(const QByteArray& body)
{
    QHash<QString, ModelLimits> out;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    const QJsonArray arr = d.isObject() ? d.object().value("data").toArray() : d.array();
    auto firstInt = [](const QJsonObject& o, std::initializer_list<const char*> keys) {
        for (const char* k : keys) {
            const QJsonValue v = o.value(QLatin1String(k));
            const int n = v.isDouble() ? int(v.toDouble()) : v.toString().toInt();
            if (n > 0) return n;
        }
        return 0;
    };
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QString id = o.value("id").toString();
        if (id.isEmpty()) continue;
        const QJsonObject top = o.value("top_provider").toObject();
        ModelLimits m;
        m.context = firstInt(o, { "context_length", "max_context_length", "context_window", "max_model_len" });
        if (m.context == 0) m.context = firstInt(top, { "context_length" });
        m.maxOutput = firstInt(top, { "max_completion_tokens" });
        if (m.maxOutput == 0) m.maxOutput = firstInt(o, { "max_output_tokens", "max_completion_tokens" });
        if (m.context > 0 || m.maxOutput > 0) out.insert(id, m);
    }
    return out;
}

RateLimits parseRateLimits(const QList<QPair<QByteArray, QByteArray>>& headers)
{
    RateLimits r;
    for (const auto& h : headers) {
        const QByteArray k = h.first.toLower();
        bool ok = false;
        const long long v = h.second.trimmed().toLongLong(&ok);
        if (!ok || v < 0) continue;
        const int n = int(std::min<long long>(v, std::numeric_limits<int>::max()));
        if (k == "x-ratelimit-remaining-tokens" || k == "anthropic-ratelimit-tokens-remaining") r.tokensRemaining = n;
        else if (k == "x-ratelimit-limit-tokens" || k == "anthropic-ratelimit-tokens-limit") r.tokensLimit = n;
        else if (k == "x-ratelimit-remaining-requests" || k == "anthropic-ratelimit-requests-remaining") r.requestsRemaining = n;
        else if (k == "x-ratelimit-limit-requests" || k == "anthropic-ratelimit-requests-limit") r.requestsLimit = n;
    }
    return r;
}

QString textFromFullResponse(const QByteArray& body, int* promptTokens, int* completionTokens, QString* finishReason)
{
    const QJsonDocument d = QJsonDocument::fromJson(body);
    if (!d.isObject()) return {};
    const QJsonObject o = d.object();
    const QJsonObject usage = o.value("usage").toObject();
    if (promptTokens) *promptTokens = usage.value("prompt_tokens").toInt(-1);
    if (completionTokens) *completionTokens = usage.value("completion_tokens").toInt(-1);
    const QJsonArray choices = o.value("choices").toArray();
    if (choices.isEmpty()) return {};
    if (finishReason) *finishReason = choices[0].toObject().value("finish_reason").toString();
    return choices[0].toObject().value("message").toObject().value("content").toString();
}

QString errorMessage(const QByteArray& body, int httpStatus)
{
    QString detail;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    if (d.isObject()) {
        const QJsonValue e = d.object().value("error");
        if (e.isObject()) detail = e.toObject().value("message").toString();
        else if (e.isString()) detail = e.toString();
        else detail = d.object().value("message").toString();
    }
    if (detail.isEmpty()) detail = QString::fromUtf8(body.left(300)).simplified();

    QString hint;
    switch (httpStatus) {
    case 401: case 403: hint = QStringLiteral("clé API refusée ou absente : vérifiez-la dans les Préférences"); break;
    case 404: hint = QStringLiteral("adresse ou modèle introuvable : vérifiez la Base URL (souvent terminée par /v1) et le nom du modèle"); break;
    case 429: hint = QStringLiteral("trop de requêtes ou quota épuisé"); break;
    default: break;
    }
    QString msg = httpStatus > 0 ? QStringLiteral("HTTP %1").arg(httpStatus) : QStringLiteral("Erreur");
    if (!hint.isEmpty()) msg += " — " + hint;
    if (!detail.isEmpty()) msg += QStringLiteral(" (%1)").arg(detail);
    return msg;
}

QStringList parseModelList(const QByteArray& body)
{
    QStringList ids;
    const QJsonDocument d = QJsonDocument::fromJson(body);
    QJsonArray arr;
    if (d.isObject()) arr = d.object().value("data").toArray();
    else if (d.isArray()) arr = d.array();
    for (const QJsonValue& v : arr) {
        const QString id = v.isObject() ? v.toObject().value("id").toString() : v.toString();
        if (!id.isEmpty()) ids << id;
    }
    ids.sort(Qt::CaseInsensitive);
    ids.removeDuplicates();
    return ids;
}

} // namespace remote
} // namespace seldon
