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

#include "McpServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QHash>
#include <QStandardPaths>

#include <algorithm>

#include "DensityFile.h"
#include "MethodRunner.h"
#include "Tools.h"
#include "Traces.h"

namespace seldon {

namespace {

// Titres lisibles des outils (champ « title » de MCP).
QString toolTitle(const QString& name)
{
    static const QHash<QString, QString> t = {
        { "calibrer_radiocarbone", "Calibrer une date radiocarbone (14C)" },
        { "calibrer_f14c", "Calibrer une fraction moderne F14C (post-bombe)" },
        { "dater_luminescence", "Datation par luminescence (TL/OSL)" },
        { "densite_gauss", "Date gaussienne (années calendaires)" },
        { "densite_uniforme", "Date uniforme (TPQ / TAQ)" },
        { "dater_archeomagnetisme", "Datation archéomagnétique" },
        { "calculer_evenement", "Événement ChronoModel (MCMC)" },
        { "combiner_dates", "Combiner des datations d'une même date" },
        { "comparer_dates", "Ordre de deux datations (laquelle est la plus ancienne)" },
        { "analyser_traces", "Calculs sur des traces MCMC de ChronoModel" },
        { "calibration_inverse", "Calibration inverse (mesure attendue pour une date)" },
        { "creer_densite", "Fichier de densité pour le plugin Density de ChronoModel" },
        { "courbes_disponibles", "Courbes de référence disponibles" } };
    return t.value(name, name);
}

QString densityFolderHint()
{
    const QStringList l = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    return l.isEmpty() ? QString() : QDir::toNativeSeparators(l.first() + QStringLiteral("/CNRS/ChronoModel/Calib/Density"));
}

QJsonObject textContent(const QString& t) { return QJsonObject{ { "type", "text" }, { "text", t } }; }

} // namespace

McpServer::McpServer(Writer notify) : notify_(std::move(notify)) {}

QStringList McpServer::protocolVersions()
{
    return { QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05") };
}

QJsonArray McpServer::toolList()
{
    QJsonArray out;
    for (const tools::ToolDef& d : tools::all()) {
        QJsonObject schema = d.parameters;
        QJsonObject props = schema.value("properties").toObject();
        QString desc = d.description;
        bool readOnly = true;
        if (d.name == QLatin1String("analyser_traces")) {
            props["file"] = QJsonObject{ { "type", "string" },
                                         { "description", QStringLiteral("Chemin complet du fichier de traces exporté par ChronoModel "
                                                                         "(Chain_all_Events.csv, Chain_all_Phases.csv…). Obligatoire au "
                                                                         "premier appel ; le fichier reste chargé pour les appels suivants.") } };
            desc += QStringLiteral(" Avec ce serveur, indique le chemin du fichier de traces dans « file ».");
        }
        if (d.name == QLatin1String("creer_densite")) {
            const QString hint = densityFolderHint();
            props["dossier"] = QJsonObject{ { "type", "string" },
                                            { "description", QStringLiteral("Dossier où écrire le fichier CSV, seulement si l'utilisateur le "
                                                                            "demande%1. Sans dossier, le fichier n'est pas écrit.")
                                                                 .arg(hint.isEmpty() ? QString() : QStringLiteral(" (ChronoModel le lit dans %1)").arg(hint)) } };
            desc.replace(QStringLiteral("l'utilisateur l'enregistre avec un lien"),
                         QStringLiteral("le fichier est écrit dans « dossier » si l'utilisateur en donne un"));
            readOnly = false;
        }
        schema["properties"] = props;
        out.append(QJsonObject{
            { "name", d.name }, { "title", toolTitle(d.name) }, { "description", desc }, { "inputSchema", schema },
            { "annotations", QJsonObject{ { "title", toolTitle(d.name) }, { "readOnlyHint", readOnly },
                                          { "destructiveHint", false }, { "idempotentHint", true }, { "openWorldHint", false } } } });
    }
    return out;
}

QString McpServer::adaptText(const QString& promptBlock)
{
    QString t = promptBlock;
    // L'application de SeldonH trace les courbes elle-même ; ici, aucun panneau de graphiques.
    static const QRegularExpression drawn(QStringLiteral("Le graphique est DÉJÀ tracé par l'application.*?en rouge…\\)\\. "),
                                          QRegularExpression::DotMatchesEverythingOption);
    t.replace(drawn, QStringLiteral("Aucun graphique n'est affiché par ce serveur : les densités (abscisses et ordonnées) sont "
                                    "dans le résultat structuré « plot », si tu dois en tracer une. "));
    t.replace(QStringLiteral("le graphique (à part) est sur l'axe de la mesure"), QStringLiteral("sa densité est sur l'axe de la mesure"));
    t.replace(QStringLiteral("L'utilisateur l'enregistre avec le lien « Enregistrer le fichier… » ; il doit être placé"),
              QStringLiteral("Il doit être placé"));
    return t;
}

QJsonObject McpServer::callTool(const QString& name, const QJsonObject& arguments, const QJsonValue& progressToken)
{
    QJsonObject args = arguments;
    auto failure = [](const QString& msg) {
        return QJsonObject{ { "content", QJsonArray{ textContent(msg) } }, { "isError", true } };
    };
    const tools::ToolDef* def = tools::find(name);
    if (!def) return failure(QStringLiteral("Outil inconnu : « %1 ».").arg(name));

    // Traces : le fichier est lu sur cet ordinateur (pas de pièce jointe comme dans l'application).
    if (name == QLatin1String("analyser_traces")) {
        const QString file = args.take("file").toString().trimmed();
        if (!file.isEmpty()) {
            QFile f(QDir::fromNativeSeparators(file));
            if (!f.open(QIODevice::ReadOnly))
                return failure(QStringLiteral("Impossible de lire le fichier de traces « %1 » : %2.").arg(file, f.errorString()));
            TraceSet set;
            QString err;
            if (!traces::parse(QString::fromUtf8(f.readAll()), QFileInfo(file).fileName(), set, &err))
                return failure(QStringLiteral("« %1 » n'est pas un fichier de traces de ChronoModel : %2.").arg(file, err));
            traces::add(set);
        }
        if (traces::loaded().isEmpty())
            return failure(QStringLiteral("Aucune trace chargée : donne dans « file » le chemin du fichier exporté par ChronoModel "
                                          "(Résultats > Export, Chain_all_Events.csv ou Chain_all_Phases.csv)."));
    }
    QString folder;
    if (name == QLatin1String("creer_densite")) folder = args.take("dossier").toString().trimmed();

    // Progression des calculs longs (MCMC) et annulation.
    // Le calcul MCMC peut signaler sa progression depuis plusieurs threads (une chaîne par thread).
    double lastSent = -1.0;
    std::mutex progressMutex;
    if (!progressToken.isNull() && !progressToken.isUndefined() && notify_) {
        MethodRunner::setProgressHandler([this, progressToken, &lastSent, &progressMutex](const QString& what, double f) {
            std::lock_guard<std::mutex> lock(progressMutex);
            if (f >= 1.0 || f - lastSent >= 0.02) {
                lastSent = f;
                const QJsonObject n{ { "jsonrpc", "2.0" }, { "method", "notifications/progress" },
                                     { "params", QJsonObject{ { "progressToken", progressToken }, { "progress", std::clamp(f, 0.0, 1.0) },
                                                              { "total", 1.0 }, { "message", what } } } };
                notify_(QJsonDocument(n).toJson(QJsonDocument::Compact));
            }
            return !cancelled_.load();
        });
    } else {
        MethodRunner::setProgressHandler([this](const QString&, double) { return !cancelled_.load(); });
    }
    const tools::ToolOutcome r = tools::execute(name, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), format_);
    MethodRunner::setProgressHandler({});

    QString text = r.executed ? adaptText(r.content) : r.content;
    if (cancelled_.load()) text.prepend(QStringLiteral("Calcul interrompu à la demande.\n"));
    if (r.executed && !folder.isEmpty()) {
        const DensityFileResult F = buildDensityFile(r.request);
        const QDir dir(QDir::fromNativeSeparators(folder));
        if (!F.ok) {
            text += QStringLiteral("\nFichier non écrit : %1.").arg(F.error);
        } else if (!dir.exists()) {
            text += QStringLiteral("\nFichier non écrit : le dossier « %1 » n'existe pas.").arg(folder);
        } else {
            QFile f(dir.filePath(F.fileName));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) && f.write(F.csv.toUtf8()) >= 0)
                text += QStringLiteral("\nFichier écrit : %1 (ChronoModel le lit dans son dossier Calib/Density ; relancer ChronoModel).")
                            .arg(QDir::toNativeSeparators(f.fileName()));
            else
                text += QStringLiteral("\nFichier non écrit : %1.").arg(f.errorString());
        }
    } else if (r.executed && name == QLatin1String("creer_densite")) {
        text += QStringLiteral("\nLe fichier n'a pas été écrit : pour l'enregistrer, rappelle l'outil avec « dossier » (demande-le à l'utilisateur).");
    }

    QJsonObject result{ { "content", QJsonArray{ textContent(text.trimmed()) } },
                        { "isError", !r.executed && !def->methodId.isEmpty() } };
    if (r.executed && !r.run.plot.isEmpty()) {
        QJsonObject sc{ { "plot", r.run.plot } };
        if (!r.run.extraPlots.isEmpty()) {
            QJsonArray extra;
            for (const QJsonObject& p : r.run.extraPlots) extra.append(p);
            sc["extra_plots"] = extra;
        }
        result["structuredContent"] = sc;
    }
    return result;
}

void McpServer::cancel(const QJsonValue& requestId)
{
    std::lock_guard<std::mutex> lock(cancelMutex_);
    if (!running_.isNull() && running_ == requestId) cancelled_ = true;
}

QJsonObject McpServer::response(const QJsonValue& id, const QJsonObject& result) const
{
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
}

QJsonObject McpServer::error(const QJsonValue& id, int code, const QString& message) const
{
    return QJsonObject{ { "jsonrpc", "2.0" }, { "id", id }, { "error", QJsonObject{ { "code", code }, { "message", message } } } };
}

QJsonValue McpServer::handle(const QJsonValue& message)
{
    if (message.isArray()) {                       // lot (versions 2025-03-26 et antérieures)
        QJsonArray out;
        for (const QJsonValue& m : message.toArray()) {
            const QJsonValue r = handle(m);
            if (!r.isNull()) out.append(r);
        }
        return out.isEmpty() ? QJsonValue() : QJsonValue(out);
    }
    if (!message.isObject()) return error(QJsonValue(), -32600, QStringLiteral("Invalid Request"));
    const QJsonObject m = message.toObject();
    const QString method = m.value("method").toString();
    const bool isRequest = m.contains("id") && !m.value("id").isNull();
    const QJsonObject params = m.value("params").toObject();
    if (m.value("jsonrpc").toString() != QLatin1String("2.0"))
        return isRequest ? QJsonValue(error(m.value("id"), -32600, QStringLiteral("Invalid Request: jsonrpc must be \"2.0\""))) : QJsonValue();
    if (method.isEmpty()) return QJsonValue();     // réponse du client à une requête du serveur : rien à faire

    if (!isRequest) {                              // notification
        if (method == QLatin1String("notifications/initialized")) initialized_ = true;
        else if (method == QLatin1String("notifications/cancelled")) cancel(params.value("requestId"));
        return QJsonValue();
    }
    const QJsonValue id = m.value("id");
    if (method == QLatin1String("initialize")) {
        const QString asked = params.value("protocolVersion").toString();
        const QStringList ok = protocolVersions();
        const QString version = ok.contains(asked) ? asked : ok.first();
        return response(id, QJsonObject{
            { "protocolVersion", version },
            { "capabilities", QJsonObject{ { "tools", QJsonObject{ { "listChanged", false } } } } },
            { "serverInfo", QJsonObject{ { "name", "seldonh" }, { "title", "SeldonH" },
                                         { "version", QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("1.0.0")
                                                                                                      : QCoreApplication::applicationVersion() } } },
            { "instructions", QStringLiteral(
                  "Outils de datation de SeldonH (méthodes de ChronoModel, CNRS), exécutés sur l'ordinateur de l'utilisateur : "
                  "calibration 14C et F14C, TL/OSL, archéomagnétisme, dates gaussiennes et uniformes, Event (MCMC), combinaison, "
                  "ordre de deux dates, traces MCMC de ChronoModel, calibration inverse, fichiers de densité. Ne calcule jamais "
                  "toi-même une date calibrée, une HPD ou une probabilité : appelle l'outil et reprends ses valeurs telles quelles. "
                  "Années astronomiques signées (0 = 1 av. J.-C.). Si une valeur manque, demande-la à l'utilisateur.") } });
    }
    if (method == QLatin1String("ping")) return response(id, QJsonObject());
    if (method == QLatin1String("tools/list")) return response(id, QJsonObject{ { "tools", toolList() } });
    if (method == QLatin1String("tools/call")) {
        const QString name = params.value("name").toString();
        if (!tools::find(name)) return error(id, -32602, QStringLiteral("Unknown tool: %1").arg(name));
        if (params.contains("arguments") && !params.value("arguments").isObject())
            return error(id, -32602, QStringLiteral("Invalid params: arguments must be an object"));
        {
            std::lock_guard<std::mutex> lock(cancelMutex_);
            running_ = id;
            cancelled_ = false;
        }
        const QJsonObject res = callTool(name, params.value("arguments").toObject(), params.value("_meta").toObject().value("progressToken"));
        {
            std::lock_guard<std::mutex> lock(cancelMutex_);
            running_ = QJsonValue();
        }
        return response(id, res);
    }
    return error(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

QByteArray McpServer::handleLine(const QByteArray& line)
{
    const QByteArray t = line.trimmed();
    if (t.isEmpty()) return {};
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(t, &pe);
    QJsonValue out;
    if (pe.error != QJsonParseError::NoError) out = error(QJsonValue(), -32700, QStringLiteral("Parse error: %1").arg(pe.errorString()));
    else out = handle(d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object()));
    if (out.isNull() || out.isUndefined()) return {};
    return out.isArray() ? QJsonDocument(out.toArray()).toJson(QJsonDocument::Compact)
                         : QJsonDocument(out.toObject()).toJson(QJsonDocument::Compact);
}

} // namespace seldon
