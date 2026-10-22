#include "MathText.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace seldon {
namespace math {

namespace {

const QHash<QString, QString>& symbols()
{
    static const QHash<QString, QString> m = {
        // lettres grecques
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ε"}, {"zeta", "ζ"},
        {"eta", "η"}, {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"},
        {"nu", "ν"}, {"xi", "ξ"}, {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"}, {"varrho", "ϱ"}, {"sigma", "σ"}, {"varsigma", "ς"},
        {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "φ"}, {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"},
        {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"},
        {"Upsilon", "Υ"}, {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
        // opérateurs
        {"times", "×"}, {"cdot", "·"}, {"pm", "±"}, {"mp", "∓"}, {"div", "÷"}, {"ast", "∗"}, {"star", "⋆"}, {"circ", "∘"},
        {"bullet", "•"}, {"oplus", "⊕"}, {"otimes", "⊗"}, {"cup", "∪"}, {"cap", "∩"}, {"wedge", "∧"}, {"vee", "∨"},
        {"sum", "∑"}, {"prod", "∏"}, {"int", "∫"}, {"iint", "∬"}, {"oint", "∮"},
        // relations
        {"leq", "≤"}, {"le", "≤"}, {"geq", "≥"}, {"ge", "≥"}, {"neq", "≠"}, {"ne", "≠"}, {"approx", "≈"}, {"sim", "∼"},
        {"simeq", "≃"}, {"cong", "≅"}, {"equiv", "≡"}, {"propto", "∝"}, {"ll", "≪"}, {"gg", "≫"}, {"in", "∈"}, {"notin", "∉"},
        {"subset", "⊂"}, {"subseteq", "⊆"}, {"supset", "⊃"}, {"supseteq", "⊇"}, {"mid", "∣"}, {"parallel", "∥"}, {"perp", "⊥"},
        {"rightarrow", "→"}, {"to", "→"}, {"leftarrow", "←"}, {"leftrightarrow", "↔"}, {"Rightarrow", "⇒"},
        {"Leftarrow", "⇐"}, {"Leftrightarrow", "⇔"}, {"implies", "⇒"}, {"iff", "⇔"}, {"mapsto", "↦"},
        // divers
        {"infty", "∞"}, {"partial", "∂"}, {"nabla", "∇"}, {"forall", "∀"}, {"exists", "∃"}, {"emptyset", "∅"},
        {"ldots", "…"}, {"dots", "…"}, {"cdots", "⋯"}, {"vdots", "⋮"}, {"ddots", "⋱"}, {"degree", "°"}, {"prime", "′"},
        {"hbar", "ℏ"}, {"ell", "ℓ"}, {"Re", "ℜ"}, {"Im", "ℑ"}, {"angle", "∠"}, {"langle", "⟨"}, {"rangle", "⟩"},
        {"lceil", "⌈"}, {"rceil", "⌉"}, {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"neg", "¬"}, {"top", "⊤"},
        // espaces
        {"quad", " "}, {"qquad", "  "}, {",", " "}, {";", " "}, {":", " "}, {" ", " "},
        {"!", ""}, {"{", "{"}, {"}", "}"}, {"%", "%"}, {"&", "&amp;"}, {"_", "_"}, {"#", "#"}, {"$", "$"}, {"|", "∥"},
        {"lbrace", "{"}, {"rbrace", "}"}, {"lbrack", "["}, {"rbrack", "]"}, {"vert", "|"}, {"Vert", "∥"},
    };
    return m;
}

const QSet<QString>& functions()
{
    static const QSet<QString> s = { "sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh",
                                     "tanh", "exp", "ln", "log", "lg", "lim", "max", "min", "sup", "inf", "det", "dim",
                                     "ker", "gcd", "mod", "arg", "Pr", "var", "cov", "erf" };
    return s;
}

const QSet<QString>& relations()
{
    static const QSet<QString> s = { "=", "<", ">", "≤", "≥", "≠", "≈", "∼", "≃", "≅", "≡", "∝", "≪", "≫", "∈", "∉", "⊂", "⊆",
                                     "⊃", "⊇", "→", "←", "↔", "⇒", "⇐", "⇔", "↦", "∣", "∥", "⊥" };
    return s;
}

const QSet<QString>& binaryOps()
{
    static const QSet<QString> s = { "+", "−", "×", "·", "±", "∓", "÷", "∗", "⋆", "∘", "•", "⊕", "⊗", "∪", "∩", "∧", "∨" };
    return s;
}

const QString kThin = QStringLiteral(" ");     // espace autour des opérateurs

QString esc(const QString& s)
{
    QString r;
    for (QChar c : s) {
        if (c == '<') r += "&lt;";
        else if (c == '>') r += "&gt;";
        else if (c == '&') r += "&amp;";
        else r += c;
    }
    return r;
}

class Parser {
public:
    explicit Parser(const QString& s) : s_(s) {}

    QString parseAll() { return parseSeq(false); }

private:
    const QString& s_;
    int i_ = 0;

    bool atEnd() const { return i_ >= s_.size(); }

    // Séquence jusqu'à « } » (consommé) si `inGroup`, sinon jusqu'à la fin.
    QString parseSeq(bool inGroup)
    {
        QString out;
        QString last;          // dernier « jeton » émis (texte brut), pour décider des espaces autour des opérateurs
        bool italicOpen = false;
        auto closeItalic = [&] { if (italicOpen) { out += "</i>"; italicOpen = false; } };

        while (!atEnd()) {
            const QChar c = s_[i_];
            if (c == '}') { ++i_; if (inGroup) break; else continue; }
            if (c.isSpace()) { ++i_; continue; }              // en LaTeX mathématique, les espaces sont ignorés

            QString atom;           // HTML de l'atome
            QString plain;          // représentation brute (opérateur ?)
            bool italicAtom = false;

            if (c == '{') {
                ++i_;
                atom = parseSeq(true);
            } else if (c == '\\') {
                parseCommand(atom, plain, italicAtom);
            } else if (c == '^' || c == '_') {
                // exposant/indice sans base : on le rattache quand même
                atom = QString();
            } else if (c.isLetter()) {
                ++i_;
                const bool latin = c.unicode() < 0x250;
                plain = QString(c);
                atom = esc(plain);
                italicAtom = latin || (c.unicode() >= 0x3b1 && c.unicode() <= 0x3c9);   // grec minuscule
            } else {
                ++i_;
                plain = QString(c);
                if (c == '-') plain = QStringLiteral("−");
                else if (c == '*') plain = QStringLiteral("∗");
                else if (c == '\'') plain = QStringLiteral("′");
                else if (c == '~') plain = QStringLiteral(" ");
                else if (c == '&') plain = QStringLiteral(" ");
                atom = esc(plain);
                // virgule décimale (« 2,448 » ou « 2{,}448 ») : pas d'espace ; sinon une fine espace après la virgule
                const bool decimal = c == ',' && !last.isEmpty() && last[0].isDigit() && !atEnd() && s_[i_].isDigit();
                if ((c == ',' && !decimal) || c == ';') atom += QStringLiteral("\u2009");
            }

            // exposants et indices
            QString sup, sub;
            for (;;) {
                skipSpaces();
                if (atEnd()) break;
                if (s_[i_] == '^')      { ++i_; sup += argHtml(); }
                else if (s_[i_] == '_') { ++i_; sub += argHtml(); }
                else if (s_[i_] == '\'' && sup.isEmpty() && sub.isEmpty()) { ++i_; atom += QStringLiteral("′"); }
                else break;
            }
            if (italicAtom && (!sup.isEmpty() || !sub.isEmpty())) atom = "<i>" + atom + "</i>";
            // indice avant exposant : Qt ne les empile pas, « σ_ref² » se lit mieux que « σ²ref »
            if (!sub.isEmpty()) atom += "<sub>" + sub + "</sub>";
            if (!sup.isEmpty()) atom += "<sup>" + sup + "</sup>";
            if (atom.isEmpty()) continue;

            // italique : on regroupe les lettres consécutives dans une seule balise
            if (italicAtom && sup.isEmpty() && sub.isEmpty()) {
                if (!italicOpen) { out += "<i>"; italicOpen = true; }
                out += atom;
                last = plain;
                continue;
            }
            closeItalic();

            const bool isRel = relations().contains(plain);
            const bool isBin = binaryOps().contains(plain);
            const bool binaryPosition = !last.isEmpty() && !binaryOps().contains(last) && !relations().contains(last)
                                        && last != "(" && last != "[" && last != "," && last != "{";
            if (isRel) out += kThin + atom + kThin;
            else if (isBin && binaryPosition) out += kThin + atom + kThin;
            else out += atom;
            last = plain.isEmpty() ? QStringLiteral("x") : plain;
        }
        closeItalic();
        return out;
    }

    void skipSpaces() { while (!atEnd() && s_[i_].isSpace()) ++i_; }

    // Argument d'une commande ou d'un exposant : {…} ou un seul jeton.
    QString argHtml()
    {
        skipSpaces();
        if (atEnd()) return {};
        if (s_[i_] == '{') { ++i_; return parseSeq(true); }
        if (s_[i_] == '\\') {
            QString atom, plain; bool it = false;
            parseCommand(atom, plain, it);
            return it ? "<i>" + atom + "</i>" : atom;
        }
        const QChar c = s_[i_++];
        if (c.isLetter() && (c.unicode() < 0x250 || (c.unicode() >= 0x3b1 && c.unicode() <= 0x3c9))) return "<i>" + esc(QString(c)) + "</i>";
        if (c == '-') return QStringLiteral("−");
        return esc(QString(c));
    }

    // Texte brut d'un argument {…} (pour \text, \mathrm…) : on garde les espaces.
    QString rawGroup()
    {
        skipSpaces();
        if (atEnd() || s_[i_] != '{') { return atEnd() ? QString() : esc(QString(s_[i_++])); }
        int depth = 0;
        QString r;
        while (!atEnd()) {
            const QChar c = s_[i_++];
            if (c == '{') { if (depth++ == 0) continue; }
            else if (c == '}') { if (--depth == 0) break; }
            r += c;
        }
        return r;
    }

    // Texte droit : on interprète quand même les commandes simples (\% \_ etc.) et on échappe le reste.
    static QString textHtml(const QString& raw)
    {
        QString r;
        for (int k = 0; k < raw.size(); ++k) {
            if (raw[k] == '\\' && k + 1 < raw.size() && !raw[k + 1].isLetter()) { r += esc(QString(raw[++k])); continue; }
            if (raw[k] == '~') { r += QStringLiteral(" "); continue; }
            r += esc(QString(raw[k]));
        }
        return r;
    }

    void parseCommand(QString& atom, QString& plain, bool& italicAtom)
    {
        ++i_;                                   // « \ »
        if (atEnd()) { atom = "\\"; return; }
        QString name;
        if (s_[i_].isLetter()) {
            while (!atEnd() && s_[i_].isLetter()) name += s_[i_++];
        } else {
            name = QString(s_[i_++]);
        }

        if (name == "\\") { atom = "<br>"; return; }
        if (name == "left" || name == "right" || name == "big" || name == "Big" || name == "bigg" || name == "Bigg"
            || name == "bigl" || name == "bigr" || name == "Bigl" || name == "Bigr") {
            skipSpaces();
            if (!atEnd()) {
                if (s_[i_] == '.') { ++i_; return; }                    // \left. : délimiteur invisible
                if (s_[i_] == '\\') { QString a, p; bool it = false; parseCommand(a, p, it); atom = a; plain = p; return; }
                plain = QString(s_[i_++]);
                atom = esc(plain);
            }
            return;
        }
        if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "cfrac") {
            const QString n = argHtml(), d = argHtml();
            atom = fraction(n, d);
            return;
        }
        if (name == "binom") {
            const QString n = argHtml(), d = argHtml();
            atom = "(" + n + QStringLiteral(" ; ") + d + ")";
            return;
        }
        if (name == "sqrt") {
            QString index;
            skipSpaces();
            if (!atEnd() && s_[i_] == '[') {
                ++i_;
                const int end = s_.indexOf(']', i_);
                if (end >= 0) { const QString idxSrc = s_.mid(i_, end - i_); Parser p(idxSrc); index = p.parseAll(); i_ = end + 1; }
            }
            const QString body = argHtml();
            atom = (index.isEmpty() ? QString() : "<sup>" + index + "</sup>") + "√<span style=\"text-decoration: overline;\">" + body + "</span>";
            return;
        }
        if (name == "text" || name == "mathrm" || name == "textrm" || name == "operatorname" || name == "mbox" || name == "textnormal") {
            atom = textHtml(rawGroup());
            return;
        }
        if (name == "textbf" || name == "mathbf" || name == "boldsymbol" || name == "bm") { atom = "<b>" + argHtml() + "</b>"; return; }
        if (name == "textit" || name == "mathit" || name == "emph") { atom = "<i>" + argHtml() + "</i>"; return; }
        if (name == "mathbb") {
            const QString r = rawGroup();
            static const QHash<QString, QString> bb = { {"R", "ℝ"}, {"N", "ℕ"}, {"Z", "ℤ"}, {"Q", "ℚ"}, {"C", "ℂ"}, {"P", "ℙ"}, {"E", "𝔼"} };
            atom = bb.value(r, esc(r));
            return;
        }
        if (name == "mathcal" || name == "mathscr" || name == "mathfrak" || name == "mathsf" || name == "mathtt" || name == "displaystyle"
            || name == "textstyle" || name == "scriptstyle" || name == "limits" || name == "nolimits") {
            if (name == "mathcal" || name == "mathscr" || name == "mathfrak" || name == "mathsf" || name == "mathtt") atom = argHtml();
            return;
        }
        if (name == "overline" || name == "bar" || name == "overbrace") { atom = "<span style=\"text-decoration: overline;\">" + argHtml() + "</span>"; return; }
        if (name == "underline" || name == "underbrace") { atom = "<u>" + argHtml() + "</u>"; return; }
        if (name == "hat" || name == "widehat") { atom = argHtml() + QStringLiteral("̂"); italicAtom = false; return; }
        if (name == "tilde" || name == "widetilde") { atom = argHtml() + QStringLiteral("̃"); return; }
        if (name == "vec")  { atom = argHtml() + QStringLiteral("⃗"); return; }
        if (name == "dot")  { atom = argHtml() + QStringLiteral("̇"); return; }
        if (name == "ddot") { atom = argHtml() + QStringLiteral("̈"); return; }
        if (name == "boxed") { atom = "[" + argHtml() + "]"; return; }
        if (name == "begin" || name == "end") {
            const QString env = rawGroup();
            if (name == "begin" && env == "cases") atom = "{ ";
            if (name == "begin" && (env.endsWith("matrix") || env == "array")) { if (env == "pmatrix") atom = "("; else if (env == "bmatrix") atom = "["; else if (env == "vmatrix") atom = "|"; }
            if (name == "end" && env == "pmatrix") atom = ")";
            if (name == "end" && env == "bmatrix") atom = "]";
            if (name == "end" && env == "vmatrix") atom = "|";
            if (env == "array" && name == "begin") rawGroup();                // spécification des colonnes
            return;
        }
        if (name == "label" || name == "tag" || name == "nonumber" || name == "notag") { if (name == "label" || name == "tag") rawGroup(); return; }
        if (name == "color" || name == "textcolor") { rawGroup(); if (name == "textcolor") atom = argHtml(); return; }

        if (functions().contains(name)) { atom = name + QStringLiteral(" "); plain = name; return; }
        const auto it = symbols().constFind(name);
        if (it != symbols().constEnd()) {
            plain = it.value();
            atom = plain;
            italicAtom = (plain.size() == 1 && plain[0].unicode() >= 0x3b1 && plain[0].unicode() <= 0x3c9);
            if (name == "sum" || name == "prod" || name == "int" || name == "iint" || name == "oint") atom = plain;
            return;
        }
        atom = esc(name);          // commande inconnue : on affiche son nom plutôt que « \nom »
    }

    static QString fraction(const QString& n, const QString& d)
    {
        auto visible = [](const QString& h) { QString t = h; t.remove(QRegularExpression("<[^>]*>")); return t.size(); };
        const bool simple = visible(n) <= 6 && visible(d) <= 6 && !n.contains("⁄") && !d.contains("⁄") && !n.contains("<br>");
        if (simple) return "<sup>" + n + "</sup>⁄<sub>" + d + "</sub>";
        auto wrap = [&](const QString& h) { return visible(h) <= 1 ? h : "(" + h + ")"; };
        return wrap(n) + QStringLiteral(" / ") + wrap(d);
    }
};

// Environnements d'une formule (aligned, cases, matrix…) : « & » devient un espace, « \\ » un retour à la ligne.
QString preprocess(const QString& latex) { QString s = latex.trimmed(); s.replace(QStringLiteral("{,}"), QStringLiteral(",")); return s; }

// ----- repérage des formules dans le Markdown

struct Span { int start = 0, end = 0; QString latex; bool display = false; };

QString marker(int idx, bool display)
{
    return QString(QChar(display ? 0xE002 : 0xE000)) + QString::number(idx) + QString(QChar(display ? 0xE003 : 0xE001));
}

QString renderOne(const QString& latex) { return Parser(preprocess(latex)).parseAll(); }

// Commandes LaTeX isolées (hors $…$) que les petits modèles écrivent parfois nues : \frac{a}{b}, \sqrt{x}, \sigma, \times…
QString protectBare(const QString& text, QStringList& rendered)
{
    static const QSet<QString> withArgs = { "frac", "dfrac", "sqrt", "text", "mathrm", "overline", "bar", "hat" };
    QString out;
    int i = 0;
    while (i < text.size()) {
        if (text[i] != '\\' || i + 1 >= text.size() || !text[i + 1].isLetter() || (i > 0 && text[i - 1] == '\\')) { out += text[i++]; continue; }
        int j = i + 1;
        while (j < text.size() && text[j].isLetter()) ++j;
        const QString name = text.mid(i + 1, j - i - 1);
        if (!symbols().contains(name) && !functions().contains(name) && !withArgs.contains(name)) { out += text[i++]; continue; }

        auto skipGroup = [&](int p) {           // saute un groupe {...} équilibré
            if (p >= text.size() || text[p] != '{') return p;
            int depth = 0;
            for (; p < text.size(); ++p) {
                if (text[p] == '{') ++depth;
                else if (text[p] == '}' && --depth == 0) return p + 1;
            }
            return p;
        };
        int end = j;
        if (name == "frac" || name == "dfrac") end = skipGroup(skipGroup(end));
        else if (name == "sqrt") { if (end < text.size() && text[end] == '[') { const int c = text.indexOf(']', end); if (c > 0) end = c + 1; } end = skipGroup(end); }
        else if (withArgs.contains(name)) end = skipGroup(end);
        // exposant / indice immédiat
        while (end < text.size() && (text[end] == '^' || text[end] == '_')) {
            ++end;
            if (end < text.size() && text[end] == '{') end = skipGroup(end);
            else if (end < text.size()) ++end;
        }
        rendered << renderOne(text.mid(i, end - i));
        out += marker(rendered.size() - 1, false);
        i = end;
    }
    return out;
}

QString protectText(const QString& text, QStringList& rendered)
{
    // $$…$$, \[…\], \(…\), $…$
    static const QRegularExpression re(
        R"(\$\$(.+?)\$\$|\\\[(.+?)\\\]|\\\((.+?)\\\)|(?<![\\$])\$(?![\s$])((?:[^$\n\\]|\\.){1,300}?)(?<![\s\\])\$(?![\d$]))",
        QRegularExpression::DotMatchesEverythingOption);
    QString out;
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        out += protectBare(text.mid(last, m.capturedStart() - last), rendered);
        bool display = false;
        QString body;
        if (m.capturedLength(1) > 0) { display = true; body = m.captured(1); }
        else if (m.capturedLength(2) > 0) { display = true; body = m.captured(2); }
        else if (m.capturedLength(3) > 0) body = m.captured(3);
        else body = m.captured(4);
        rendered << renderOne(body);
        const QString mk = marker(rendered.size() - 1, display);
        out += display ? "\n\n" + mk + "\n\n" : mk;
        last = m.capturedEnd();
    }
    out += protectBare(text.mid(last), rendered);
    return out;
}

} // namespace

QString latexToHtml(const QString& latex) { return renderOne(latex); }

QString protect(const QString& markdown, QStringList& rendered)
{
    // Hors blocs de code (``` … ```) et codes en ligne (`…`).
    static const QRegularExpression code(R"(```.*?```|~~~.*?~~~|`[^`\n]*`)", QRegularExpression::DotMatchesEverythingOption);
    QString out;
    int last = 0;
    auto it = code.globalMatch(markdown);
    while (it.hasNext()) {
        const auto m = it.next();
        out += protectText(markdown.mid(last, m.capturedStart() - last), rendered);
        out += m.captured(0);
        last = m.capturedEnd();
    }
    out += protectText(markdown.mid(last), rendered);
    return out;
}

QString restore(QString html, const QStringList& rendered)
{
    if (rendered.isEmpty()) return html;
    // Formules hors texte : le paragraphe entier devient un paragraphe centré.
    static const QRegularExpression disp(QStringLiteral("<p[^>]*>\\s*(\\d+)\\s*</p>"));
    QString out;
    int last = 0;
    auto it = disp.globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        const int idx = m.captured(1).toInt();
        out += html.mid(last, m.capturedStart() - last);
        out += QStringLiteral("<p align=\"center\" style=\"margin-top:8px; margin-bottom:8px; font-size:large;\">%1</p>")
                   .arg(idx >= 0 && idx < rendered.size() ? rendered[idx] : QString());
        last = m.capturedEnd();
    }
    out += html.mid(last);
    // Les autres marqueurs (en ligne, ou formule hors texte restée dans une phrase).
    static const QRegularExpression inl(QStringLiteral("[](\\d+)[]"));
    QString res;
    last = 0;
    auto it2 = inl.globalMatch(out);
    while (it2.hasNext()) {
        const auto m = it2.next();
        const int idx = m.captured(1).toInt();
        res += out.mid(last, m.capturedStart() - last);
        res += idx >= 0 && idx < rendered.size() ? rendered[idx] : QString();
        last = m.capturedEnd();
    }
    res += out.mid(last);
    return res;
}

} // namespace math
} // namespace seldon
