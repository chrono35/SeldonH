#include "CurveStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <memory>

namespace seldon {

namespace {

QMutex gMutex;

QString dirOf(const QString& methodId) { return QStringLiteral(":/methods/%1/curves").arg(methodId); }

QString findFile(const QString& methodId, const QString& name)
{
    const QDir d(dirOf(methodId));
    const QString want = name.toLower();
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files)) {
        if (fi.completeBaseName().toLower() == want || fi.fileName().toLower() == want) return fi.filePath();
    }
    return QString();
}

} // namespace

QStringList CurveStore::names(const QString& methodId)
{
    QStringList out;
    const QDir d(dirOf(methodId));
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files, QDir::Name)) out << fi.completeBaseName().toLower();
    return out;
}

const RefCurve* CurveStore::get(const QString& methodId, const QString& name, CurveFormat fmt, bool asF14C)
{
    static QHash<QString, std::shared_ptr<RefCurve>> cache;
    const QString key = methodId + '/' + name.toLower() + (asF14C ? "#f" : "#a");

    QMutexLocker lock(&gMutex);
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return it.value().get();

    const QString path = findFile(methodId, name);
    std::shared_ptr<RefCurve> curve;
    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray raw = f.readAll();
            const std::string text = raw.toStdString();      // octets bruts : seuls les commentaires sont non ASCII
            auto c = std::make_shared<RefCurve>();
            c->name = QFileInfo(path).completeBaseName().toLower().toStdString();
            const bool ok = (fmt == CurveFormat::Radiocarbon) ? parseRefCurve14C(text, asF14C, *c)
                                                              : parseRefCurveSimple(text, *c);
            if (ok) curve = c;
        }
    }
    cache.insert(key, curve);       // met aussi en cache l'échec (nullptr)
    return curve.get();
}

} // namespace seldon
