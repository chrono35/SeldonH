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

// ToolsProbe : le serveur distant et son modèle savent-ils faire de l'appel d'outils natif ?
//
// Deux conditions sont nécessaires et l'une peut manquer sans erreur visible : le modèle doit avoir été entraîné à
// l'appel d'outils, et le serveur doit transmettre le champ « tools » (vLLM sans --enable-auto-tool-choice,
// llama-server sans --jinja, Ollama avec un gabarit sans outils…). Le test envoie une requête minimale qui déclare un
// outil factice (« verifier_outil ») et demande de l'appeler :
//   - Native   : la réponse contient un appel « tool_calls » de cet outil -> appel d'outils natif ;
//   - TextOnly : le modèle a écrit l'appel dans le texte -> le bloc ```calcul est plus sûr ;
//   - Ignored  : le serveur accepte « tools » mais le modèle n'appelle rien -> bloc ```calcul ;
//   - Refused  : le serveur refuse le champ « tools » (erreur 400…) -> bloc ```calcul ;
//   - Unknown  : test impossible (réseau, clé refusée, quota…) : rien n'est conclu.
// Le résultat est mémorisé par serveur et par modèle (QSettings, groupe « remote/toolsProbe »).
#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>

#include "RemoteApi.h"



namespace seldon {

enum class ToolSupport { Unknown, Native, TextOnly, Ignored, Refused };

struct ToolProbeResult {
    ToolSupport support = ToolSupport::Unknown;
    QString     detail;          // message du serveur ou extrait de la réponse
    QDateTime   when;            // date du test (UTC)
    bool usesNativeTools() const { return support == ToolSupport::Native; }
    bool conclusive() const { return support != ToolSupport::Unknown; }
};

namespace toolprobe {

constexpr const char* kToolName = "verifier_outil";

// Corps de la requête de test (non diffusée, température 0, outil factice, tool_choice auto).
QByteArray requestBody(const QString& model, bool useMaxCompletionTokens = false, bool omitTemperature = false);
// Classe la réponse du serveur (statut HTTP, corps ; status 0 = pas de réponse, `networkError` en dit la raison).
ToolProbeResult classify(int httpStatus, const QByteArray& body, const QString& networkError = QString());
// Le serveur demande une autre forme de requête (max_completion_tokens, sans temperature) : vrai si l'on peut réessayer.
bool wantsRetry(int httpStatus, const QByteArray& body, bool& useMaxCompletionTokens, bool& omitTemperature);

// Mémoire des tests, par serveur (Base URL normalisée) et modèle.
bool cached(const QString& baseUrl, const QString& model, ToolProbeResult& out);
void store(const QString& baseUrl, const QString& model, const ToolProbeResult& r);
// Un test plus vieux que `days` jours est refait en mode automatique.
bool isFresh(const ToolProbeResult& r, int days = 30);

// Texte pour l'utilisateur (« ✅ appel d'outils natif accepté », « ⚠ le modèle écrit l'appel dans le texte »…).
QString describe(const ToolProbeResult& r, bool html = true);
QString supportId(ToolSupport s);                 // native, text, ignored, refused, unknown (stockage)
ToolSupport supportFromId(const QString& id);

// Mode des Préférences (« auto », « on », « off ») et résultat mémorisé -> faut-il déclarer les outils ?
// auto : oui si le test a conclu « natif », ou s'il n'y a pas encore de test concluant (le repli du moteur couvre un refus).
bool wantTools(const QString& mode, const ToolProbeResult* cachedResult);

} // namespace toolprobe

// Lance le test (un seul à la fois) ; le résultat est mémorisé avant le signal.
class ToolsProbe : public QObject {
    Q_OBJECT
public:
    explicit ToolsProbe(QObject* parent = nullptr);
    void start(const RemoteConfig& cfg);
    bool running() const { return reply_ != nullptr; }
    void setTimeoutMs(int ms) { timeoutMs_ = ms; }

signals:
    void finished(const seldon::ToolProbeResult& result);

private:
    void send();
    QNetworkAccessManager nam_;
    QPointer<QNetworkReply> reply_;
    RemoteConfig cfg_;
    bool maxCompletion_ = false, omitTemperature_ = false, retried_ = false;
    int timeoutMs_ = 45000;
};

} // namespace seldon

Q_DECLARE_METATYPE(seldon::ToolProbeResult)
