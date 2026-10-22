#include "LlmEngine.h"

#include <QFileInfo>

namespace seldon {

LlmEngine::LlmEngine(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<seldon::ChatMessage>("seldon::ChatMessage");
    qRegisterMetaType<QVector<seldon::ChatMessage>>("QVector<seldon::ChatMessage>");
    qRegisterMetaType<seldon::SamplingParams>("seldon::SamplingParams");
    qRegisterMetaType<seldon::GenStats>("seldon::GenStats");
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

    std::string err;
    const bool ok = core_.load(p, err, [this](float v) {
        emit loadProgress(static_cast<double>(v));
        return true;
    });
    if (!ok) {
        emit modelFailed(QString::fromStdString(err));
        return;
    }
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
