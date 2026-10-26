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

// LlmEngine : adaptateur Qt autour de LlmCore. À déplacer dans un QThread dédié :
//
//     auto* thread = new QThread(this);
//     engine->moveToThread(thread);  thread->start();
//
// Les slots (loadModel, generate, ...) sont appelés par signaux/slots en file d'attente depuis
// l'interface ; seul stop() peut être appelé directement depuis le thread de l'interface.
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "LlmCore.h"

namespace seldon {

class LlmEngine : public QObject {
    Q_OBJECT
public:
    explicit LlmEngine(QObject* parent = nullptr);
    ~LlmEngine() override;

    // Thread-safe : interrompt la génération (ou le prompt) en cours.
    void stop() { core_.requestStop(); }

public slots:
    void loadModel(const QString& path, int nCtx, int nThreads, int nGpuLayers);
    void unloadModel();
    void resetConversation();
    void generate(const QVector<seldon::ChatMessage>& chat, const seldon::SamplingParams& sp);

signals:
    void loadProgress(double fraction);
    void modelInfoReady(const seldon::ModelInfo& info);     // juste avant modelLoaded
    void modelLoaded(const QString& description, int contextSize);
    void modelFailed(const QString& error);

    void started(bool startsInThink, int promptTokens, int reusedTokens);
    void token(const QString& piece);                       // fragment de texte (UTF-8 complet)
    void finished(const seldon::GenStats& stats);
    void failed(const QString& error);

private:
    LlmCore core_;
};

} // namespace seldon

Q_DECLARE_METATYPE(seldon::ChatMessage)
Q_DECLARE_METATYPE(QVector<seldon::ChatMessage>)
Q_DECLARE_METATYPE(seldon::SamplingParams)
Q_DECLARE_METATYPE(seldon::GenStats)
Q_DECLARE_METATYPE(seldon::ModelInfo)
