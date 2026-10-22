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
