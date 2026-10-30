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

// McpServer : les outils de calcul de SeldonH exposés par le Model Context Protocol (MCP), pour d'autres assistants
// (Claude Desktop, Jan, Gemini CLI…). Transport stdio : l'assistant lance « seldonh-mcp » et échange avec lui des
// messages JSON-RPC 2.0, un par ligne, sur l'entrée et la sortie standard. Pas de réseau, pas de port ouvert : les
// calculs restent faits sur l'ordinateur de l'utilisateur, avec les mêmes méthodes que l'application (Tools,
// MethodRunner, méthodes de ChronoModel).
//
// Messages traités : initialize, notifications/initialized, ping, tools/list, tools/call, notifications/cancelled.
// Progression d'un calcul long (MCMC) : notifications/progress si la requête porte _meta.progressToken.
// Sans dépendance à Qt Widgets ni au réseau (QtCore seulement) : testable dans tests/tst_core.
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <mutex>

#include "DateFormat.h"

namespace seldon {

class McpServer {
public:
    // `notify` écrit un message (une ligne JSON, sans saut de ligne final) sur la sortie : notifications de progression.
    // Peut être appelé depuis un autre thread (calcul MCMC) : l'appelant doit protéger la sortie.
    using Writer = std::function<void(const QByteArray&)>;
    explicit McpServer(Writer notify = {});

    // Versions du protocole acceptées, de la plus récente à la plus ancienne.
    static QStringList protocolVersions();

    // Traite un message (objet JSON-RPC ou lot) ; renvoie la réponse à écrire, vide pour une notification.
    QByteArray handleLine(const QByteArray& line);
    QJsonValue handle(const QJsonValue& message);         // objet -> réponse (objet) ou null ; tableau -> tableau

    // notifications/cancelled reçue pendant un calcul (appelée par le thread de lecture) : le calcul en cours s'arrête
    // au prochain point de progression si c'est la requête visée.
    void cancel(const QJsonValue& requestId);

    // Outils annoncés (tools/list) : ceux de l'application, avec les ajouts propres au MCP (fichier de traces à lire,
    // dossier où écrire un fichier de densité).
    static QJsonArray toolList();
    // Exécute un outil : résultat MCP {"content":[{"type":"text",…}], "isError":…, "structuredContent":{…}}.
    QJsonObject callTool(const QString& name, const QJsonObject& arguments, const QJsonValue& progressToken = QJsonValue());

    // Réglages : format des dates (comme les Préférences de SeldonH, « bcad » par défaut).
    void setDateFormat(DateFormat f) { format_ = f; }
    // Texte de résultat de SeldonH adapté à un assistant tiers (pas de panneau de graphiques).
    static QString adaptText(const QString& promptBlock);

private:
    QJsonObject response(const QJsonValue& id, const QJsonObject& result) const;
    QJsonObject error(const QJsonValue& id, int code, const QString& message) const;

    Writer notify_;
    DateFormat format_ = DateFormat::BcAd;
    bool initialized_ = false;
    std::mutex cancelMutex_;
    QJsonValue running_;                 // id de la requête tools/call en cours
    std::atomic<bool> cancelled_{ false };
};

} // namespace seldon
