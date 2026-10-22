#include "SkillManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace seldon {

QString SkillManager::normalize(const QString& s)
{
    const QString d = s.toLower().normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d)
        if (c.category() != QChar::Mark_NonSpacing) out.append(c);
    return out.normalized(QString::NormalizationForm_C);
}

bool SkillManager::parse(const QString& id, const QString& text, const QString& origin, Skill& out)
{
    QString t = text;
    t.replace("\r\n", "\n");
    out = Skill();
    out.id = id;
    out.origin = origin;
    out.name = id;

    if (t.startsWith("---\n")) {
        const int end = t.indexOf("\n---", 4);
        if (end > 0) {
            const QString head = t.mid(4, end - 4);
            for (const QString& line : head.split('\n')) {
                const int c = line.indexOf(':');
                if (c <= 0) continue;
                const QString key = line.left(c).trimmed().toLower();
                QString val = line.mid(c + 1);
                const int hash = val.indexOf(" #");           // commentaire de fin de ligne
                if (hash >= 0) val = val.left(hash);
                val = val.trimmed();
                if (key == "name")             out.name = val;
                else if (key == "description") out.description = val;
                else if (key == "always")      out.always = (val.toLower() == "true" || val == "1");
                else if (key == "priority")    out.priority = val.toInt();
                else if (key == "keywords") {
                    for (const QString& k : val.split(',', Qt::SkipEmptyParts)) {
                        const QString n = normalize(k.trimmed());
                        if (!n.isEmpty()) out.keywords << n;
                    }
                }
            }
            t = t.mid(end + 4);
        }
    }
    out.body = t.trimmed();
    return !out.body.isEmpty();
}

static void loadDir(const QString& dirPath, const QString& origin, QHash<QString, Skill>& into)
{
    const QDir dir(dirPath);
    const QFileInfoList files = dir.entryInfoList({ "*.md" }, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        Skill s;
        if (SkillManager::parse(fi.completeBaseName(), QString::fromUtf8(f.readAll()), origin, s))
            into.insert(s.id, s);       // le dossier utilisateur, lu en second, remplace l'intégré
    }
}

int SkillManager::reload(const QString& userDir)
{
    userDir_ = userDir;
    QHash<QString, Skill> map;
    loadDir(QStringLiteral(":/skills"), QStringLiteral("intégrée"), map);
    if (!userDir.isEmpty()) {
        QDir().mkpath(userDir);
        loadDir(userDir, QStringLiteral("utilisateur"), map);
    }
    skills_ = map.values();
    std::sort(skills_.begin(), skills_.end(), [](const Skill& a, const Skill& b) { return a.id < b.id; });
    return skills_.size();
}

QList<SkillMatch> SkillManager::select(const QString& question, int maxChars, const QStringList& exclude,
                                       const QStringList& pinned) const
{
    const QString q = normalize(question);
    const QSet<QString> qWords = [&] {
        QSet<QString> w;
        for (const QString& x : q.split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts))
            if (x.size() > 3) w.insert(x);
        return w;
    }();

    QList<SkillMatch> scored;
    for (const Skill& s : skills_) {
        if (s.always || pinned.contains(s.id) || exclude.contains(s.id)) continue;

        int score = 0;
        for (const QString& k : s.keywords) {
            if (k.contains(' ')) { if (q.contains(k)) score += 4; }
            else if (qWords.contains(k)) score += 3;
            else if (k.size() > 4 && q.contains(k)) score += 2;     // préfixe / pluriel
        }
        // Un peu de recouvrement avec le titre et la description.
        const QString nd = normalize(s.name + ' ' + s.description);
        for (const QString& w : qWords)
            if (nd.contains(w)) score += 1;
        if (score > 0) scored.append({ &s, score * 10 + s.priority });
    }
    std::sort(scored.begin(), scored.end(), [](const SkillMatch& a, const SkillMatch& b) { return a.score > b.score; });

    QList<SkillMatch> out;
    int used = 0;
    for (const QString& id : pinned) {
        for (const Skill& s : skills_) {
            if (s.id != id || s.always) continue;
            out.append({ &s, 1000000 });
            used += s.body.size();
        }
    }
    for (const SkillMatch& m : std::as_const(scored)) {
        if (used + m.skill->body.size() > maxChars) continue;     // on essaie les suivantes, plus courtes
        out.append(m);
        used += m.skill->body.size();
    }
    return out;
}

QString SkillManager::alwaysText() const
{
    QString s;
    for (const Skill& k : skills_)
        if (k.always) s += k.body + "\n\n";
    return s.trimmed();
}

QString SkillManager::format(const QList<SkillMatch>& m)
{
    QString s;
    for (const SkillMatch& x : m) {
        s += QStringLiteral("### %1\n%2\n\n").arg(x.skill->name, x.skill->body);
    }
    return s.trimmed();
}

} // namespace seldon
