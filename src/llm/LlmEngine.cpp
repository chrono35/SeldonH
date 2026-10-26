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

#include "LlmEngine.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace seldon {

LlmEngine::LlmEngine(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<seldon::ChatMessage>("seldon::ChatMessage");
    qRegisterMetaType<QVector<seldon::ChatMessage>>("QVector<seldon::ChatMessage>");
    qRegisterMetaType<seldon::SamplingParams>("seldon::SamplingParams");
    qRegisterMetaType<seldon::GenStats>("seldon::GenStats");
    qRegisterMetaType<seldon::ModelInfo>("seldon::ModelInfo");
}

LlmEngine::~LlmEngine() = default;

void LlmEngine::loadModel(const QString& path, int nCtx, int nThreads, int nGpuLayers)
{
    if (!QFileInfo::exists(path)) {
        emit modelFailed(tr("Fichier modèle introuvable : %1").arg(path));
        return;
    }
    LlmParams p;
    p.modelPath  = path.toStdString();
    p.nCtx       = nCtx;
    p.nThreads   = nThreads;
    p.nGpuLayers = nGpuLayers;
#if defined(Q_OS_LINUX)
    p.backendDir = (QCoreApplication::applicationDirPath() + "/lib").toUtf8().toStdString();   // installé : lib/
#else
    p.backendDir = QCoreApplication::applicationDirPath().toUtf8().toStdString();              // Windows : à côté du .exe
#endif

    std::string err;
    const bool ok = core_.load(p, err, [this](float v) {
        emit loadProgress(static_cast<double>(v));
        return true;
    });
    if (!ok) {
        emit modelFailed(QString::fromStdString(err));
        return;
    }
    emit modelInfoReady(core_.info());
    emit modelLoaded(QString::fromStdString(core_.description()), core_.contextSize());
}

void LlmEngine::unloadModel() { core_.unload(); }

void LlmEngine::resetConversation() { core_.resetCache(); }

void LlmEngine::generate(const QVector<ChatMessage>& chat, const SamplingParams& sp)
{
    std::vector<ChatMessage> msgs(chat.begin(), chat.end());
    std::string err;
    GenStats st;
    const bool ok = core_.generate(
        msgs, sp,
        [this](const std::string& piece) {
            emit token(QString::fromUtf8(piece.data(), static_cast<int>(piece.size())));
            return true;
        },
        err, &st,
        [this](const GenStats& s) { emit started(s.startsInThink, s.promptTokens, s.reusedTokens); });
    if (!ok) {
        emit failed(QString::fromStdString(err));
        return;
    }
    emit finished(st);
}

} // namespace seldon
