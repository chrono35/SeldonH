// MethodRegistry : liste des méthodes de datation disponibles (équivalent du PluginManager de ChronoModel).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "Method.h"

namespace seldon {

class MethodRegistry {
public:
    static const MethodRegistry& instance();

    const QVector<const Method*>& methods() const { return methods_; }
    const Method* byId(const QString& id) const;

    // Demandes reconnues dans une question, dans l'ordre où elles y apparaissent. Si plusieurs méthodes sont citées
    // (« C14 2500 ± 30 BP et TL 488 ± 19 ans mesuré en 2015 »), chaque paire « valeur ± erreur » va à la méthode dont
    // le mot-clé la précède de plus près (ou la suit immédiatement, comme l'unité « BP »).
    QVector<QJsonObject> parse(const QString& question) const;

    // Calcule une demande JSON (clé "method").
    MethodResult compute(const QJsonObject& request) const;
    QString      describe(const QJsonObject& request) const;

private:
    MethodRegistry();
    QVector<const Method*> methods_;
};

} // namespace seldon
