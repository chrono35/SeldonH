#include "MethodRegistry.h"

#include "../am/AmMethod.h"
#include "../c14/C14Method.h"
#include "../f14c/F14cMethod.h"
#include "../gauss/GaussMethod.h"
#include "../tl/TlMethod.h"
#include "../unif/UnifMethod.h"

namespace seldon {

MethodRegistry::MethodRegistry()
{
    // Ordre = priorité de reconnaissance (du plus spécifique au plus général).
    methods_ << new F14cMethod << new TlMethod << new AmMethod << new UnifMethod << new GaussMethod << new C14Method;
}

const MethodRegistry& MethodRegistry::instance()
{
    static const MethodRegistry r;
    return r;
}

const Method* MethodRegistry::byId(const QString& id) const
{
    for (const Method* m : methods_)
        if (m->id() == id) return m;
    return nullptr;
}

QVector<QJsonObject> MethodRegistry::parse(const QString& question) const
{
    for (const Method* m : methods_) {
        QVector<QJsonObject> v = m->parse(question);
        if (!v.isEmpty()) {
            for (QJsonObject& o : v) o["method"] = m->id();
            return v;
        }
    }
    return {};
}

MethodResult MethodRegistry::compute(const QJsonObject& request) const
{
    const Method* m = byId(request.value("method").toString());
    if (!m) return MethodResult::failure(QStringLiteral("méthode inconnue : « %1 »").arg(request.value("method").toString()));
    return m->compute(request);
}

QString MethodRegistry::describe(const QJsonObject& request) const
{
    const Method* m = byId(request.value("method").toString());
    return m ? m->describe(request) : QString();
}

} // namespace seldon
