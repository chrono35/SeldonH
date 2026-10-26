/* ---------------------------------------------------------------------

Copyright © CNRS	2014 - 2026

Authors :
	Philippe LANOS
	Helori LANOS
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This file contains dating methods derived from ChronoModel
(https://chronomodel.com), distributed under the same license.

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

#include "RefCurve.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <sstream>

namespace seldon {

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

bool isComment(const std::string& line)
{
    const std::string s = trim(line);
    return s.rfind("//", 0) == 0 || s.rfind("#", 0) == 0 || s.rfind("/*", 0) == 0 || s.rfind("!", 0) == 0
           || s.rfind("\"", 0) == 0;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> splitWhitespace(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

bool toDouble(const std::string& s, double& v)
{
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end != t.c_str() && *end == '\0';
}

// Découpe une ligne de données : « , » si présente, sinon tabulation, sinon espaces.
std::vector<std::string> splitData(const std::string& line, bool allowSemicolon)
{
    if (line.find(',') != std::string::npos) return split(line, ',');
    if (allowSemicolon && line.find(';') != std::string::npos) return split(line, ';');
    if (line.find('\t') != std::string::npos) return split(line, '\t');
    return splitWhitespace(line);
}

} // namespace

// ---------------------------------------------------------------------------------------------

double craToF14C(double cra) { return std::exp(-cra / 8033.0); }
double errCraToErrF14C(double errCra, double cra) { return std::exp(-cra / 8033.0) - std::exp(-(cra + errCra) / 8033.0); }
double f14cToCra(double f14c) { return -8033.0 * std::log(f14c); }
double errF14CToErrCra(double errF14c, double f14c) { return -8033.0 * std::log(f14c - errF14c) + 8033.0 * std::log(f14c); }

void RefCurve::finalize()
{
    minStep = std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < t.size(); ++i) {
        const double d = t[i] - t[i - 1];
        if (d > 0.0) minStep = std::min(minStep, d);
    }
    if (!std::isfinite(minStep)) minStep = 1.0;
    supMax = -std::numeric_limits<double>::infinity();
    infMin = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < t.size(); ++i) {
        supMax = std::max(supMax, mean[i] + 1.96 * err[i]);
        infMin = std::min(infMin, mean[i] - 1.96 * err[i]);
    }
}

void RefCurve::at(double year, double& m, double& e) const
{
    if (year < tmin() || year > tmax()) {
        m = (supMax + infMin) * 0.5;
        e = 1.0e6 * (supMax - infMin);
        return;
    }
    const auto it = std::lower_bound(t.begin(), t.end(), year);
    const size_t k = static_cast<size_t>(it - t.begin());
    if (k == 0) { m = mean.front(); e = err.front(); return; }
    if (k >= t.size()) { m = mean.back(); e = err.back(); return; }
    const double u = (year - t[k - 1]) / (t[k] - t[k - 1]);
    m = mean[k - 1] + u * (mean[k] - mean[k - 1]);
    e = err[k - 1] + u * (err[k] - err[k - 1]);
}

// ---------------------------------------------------------------------------------------------

namespace {

struct Row { double t, g, e; };

bool finishCurve(std::vector<Row>& rows, RefCurve& out, std::string* err)
{
    if (rows.size() < 2) {
        if (err) *err = "courbe vide ou illisible";
        return false;
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t < b.t; });
    out.t.clear(); out.mean.clear(); out.err.clear();
    for (const Row& r : rows) {
        if (!out.t.empty() && r.t == out.t.back()) continue;      // doublons
        out.t.push_back(r.t);
        out.mean.push_back(r.g);
        out.err.push_back(r.e);
    }
    out.finalize();
    return out.valid();
}

// « # CAL BP, 14C age, Sigma... » : renvoie vrai si la ligne est un en-tête de colonnes.
bool looksLikeHeader(const std::string& commentLine, const std::vector<std::string>& fields)
{
    if (fields.size() < 3) return false;
    std::string first = lower(trim(fields[0]));
    while (!first.empty() && (first[0] == '#' || first[0] == '/' || first[0] == '!' || first[0] == ' ' || first[0] == '\t'))
        first.erase(first.begin());
    const bool timeLike = first.rfind("cal", 0) == 0 || first.rfind("year", 0) == 0 || first.rfind("bp", 0) == 0
                          || first.rfind("age", 0) == 0 || first.rfind("date", 0) == 0;
    const std::string l = lower(commentLine);
    return timeLike && (contains(l.c_str(), "14c") || contains(l.c_str(), "f14") || contains(l.c_str(), "age")
                        || contains(l.c_str(), "sigma") || contains(l.c_str(), "error"));
}

} // namespace

bool parseRefCurve14C(const std::string& text, bool asF14C, RefCurve& out, std::string* err)
{
    bool timeInBP = true, dataInF14C = false;
    int ageCol = 1, f14Col = 0;
    std::vector<Row> rows;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;

        if (isComment(line)) {
            const char sep = line.find(',') != std::string::npos ? ',' : '\t';
            const std::vector<std::string> f = split(line, sep);
            if (!looksLikeHeader(line, f)) continue;

            const std::string v0 = lower(f[0]);
            if (contains(v0.c_str(), "bp")) timeInBP = true;
            else if (contains(v0.c_str(), "bc") || contains(v0.c_str(), "year") || contains(v0.c_str(), "ad")) timeInBP = false;

            bool foundF = false, foundAge = false;
            for (size_t i = 0; i < f.size(); ++i) {
                const std::string fi = lower(f[i]);
                if (!foundF && contains(fi.c_str(), "f14")) { f14Col = int(i); foundF = true; }
                else if (!foundAge && contains(fi.c_str(), "age")) { ageCol = int(i); foundAge = true; }
            }
            if (foundF) dataInF14C = true;
            else if (foundAge) dataInF14C = false;
            continue;
        }

        const std::vector<std::string> v = splitData(line, false);
        const int col = dataInF14C ? f14Col : ageCol;
        if (v.size() < 3 || int(v.size()) < col + 2) continue;
        double t, g, e;
        if (!toDouble(v[0], t) || !toDouble(v[size_t(col)], g) || !toDouble(v[size_t(col) + 1], e)) continue;
        if (timeInBP) t = 1950.0 - t;

        if (dataInF14C && !asF14C) {
            if (!(g > 0.0) || !(g - e > 0.0)) continue;
            e = errF14CToErrCra(e, g);
            g = f14cToCra(g);
        } else if (!dataInF14C && asF14C) {
            e = errCraToErrF14C(e, g);
            g = craToF14C(g);
        }
        rows.push_back({ t, g, e });
    }
    return finishCurve(rows, out, err);
}

bool parseRefCurveSimple(const std::string& text, RefCurve& out, std::string* err)
{
    std::vector<Row> rows;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty() || isComment(line)) continue;
        const std::vector<std::string> v = splitData(line, true);
        double t, g, e;
        if (v.size() < 3 || !toDouble(v[0], t) || !toDouble(v[1], g) || !toDouble(v[2], e)) continue;
        rows.push_back({ t, g, e });
    }
    return finishCurve(rows, out, err);
}

} // namespace seldon
