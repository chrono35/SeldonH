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

#include "CpuTopology.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace seldon {

namespace {

bool readInt(const std::string& path, int& v)
{
    std::ifstream f(path);
    return static_cast<bool>(f >> v);
}

// Liste de CPU au format du noyau : "0-3,8,10-11".
std::set<int> parseCpuList(const std::string& s)
{
    std::set<int> out;
    std::stringstream ss(s);
    for (std::string part; std::getline(ss, part, ',');) {
        part.erase(std::remove_if(part.begin(), part.end(), [](char c) { return c == ' ' || c == '\n' || c == '\r'; }), part.end());
        if (part.empty()) continue;
        const auto dash = part.find('-');
        try {
            if (dash == std::string::npos) out.insert(std::stoi(part));
            else for (int i = std::stoi(part.substr(0, dash)), e = std::stoi(part.substr(dash + 1)); i <= e; ++i) out.insert(i);
        } catch (...) {}
    }
    return out;
}

std::set<int> readCpuList(const std::string& path)
{
    std::ifstream f(path);
    std::string s;
    if (!std::getline(f, s)) return {};
    return parseCpuList(s);
}

#if defined(__APPLE__)
int sysctlInt(const char* name)
{
    int v = 0;
    size_t len = sizeof(v);
    return sysctlbyname(name, &v, &len, nullptr, 0) == 0 ? v : 0;
}
#endif

} // namespace

bool readLinuxCpuTopology(const std::string& root, CpuTopology& out)
{
    const std::string cpuDir = root + "/devices/system/cpu";
    std::set<int> cpus = readCpuList(cpuDir + "/online");
    if (cpus.empty()) return false;

    // Cœurs « performance » : Intel hybride (cpu_core / cpu_atom), sinon ARM (cpu_capacity maximale).
    const std::set<int> intelP = readCpuList(root + "/devices/cpu_core/cpus");
    std::vector<std::pair<int, int>> capacity;   // (cpu, capacité)
    int maxCap = -1;
    for (int c : cpus) {
        int cap;
        if (readInt(cpuDir + "/cpu" + std::to_string(c) + "/cpu_capacity", cap)) {
            capacity.emplace_back(c, cap);
            maxCap = std::max(maxCap, cap);
        }
    }

    std::set<std::pair<int, int>> cores, perfCores;   // (paquet, cœur)
    for (int c : cpus) {
        const std::string topo = cpuDir + "/cpu" + std::to_string(c) + "/topology/";
        int pkg = 0, core = c;
        readInt(topo + "physical_package_id", pkg);
        if (!readInt(topo + "core_id", core)) core = c;
        cores.insert({ pkg, core });

        bool perf = true;
        if (!intelP.empty()) perf = intelP.count(c) > 0;
        else if (capacity.size() == cpus.size())
            perf = std::find(capacity.begin(), capacity.end(), std::make_pair(c, maxCap)) != capacity.end();
        if (perf) perfCores.insert({ pkg, core });
    }
    if (cores.empty()) return false;
    out.logical     = static_cast<int>(cpus.size());
    out.physical    = static_cast<int>(cores.size());
    out.performance = perfCores.empty() ? out.physical : static_cast<int>(perfCores.size());
    out.source      = !intelP.empty() ? "sysfs (Intel hybride)" : (capacity.size() == cpus.size() ? "sysfs (cpu_capacity)" : "sysfs");
    return true;
}

static CpuTopology detect()
{
    CpuTopology t;
    t.logical = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    t.physical = t.performance = t.logical;
    t.source = "repli (threads matériels)";

#if defined(__APPLE__)
    const int phys = sysctlInt("hw.physicalcpu");
    if (phys > 0) {
        t.physical = phys;
        // Apple Silicon : perflevel0 = cœurs P. Mac Intel : clé absente, tous les cœurs sont équivalents.
        const int p0 = sysctlInt("hw.nperflevels") > 1 ? sysctlInt("hw.perflevel0.physicalcpu") : 0;
        t.performance = p0 > 0 ? p0 : phys;
        const int logical = sysctlInt("hw.logicalcpu");
        if (logical > 0) t.logical = logical;
        t.source = "sysctl";
    }
#elif defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len > 0) {
        std::vector<char> buf(len);
        auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) {
            std::vector<int> classes;   // EfficiencyClass de chaque cœur (plus grand = plus performant)
            for (DWORD off = 0; off < len;) {
                auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
                if (e->Relationship == RelationProcessorCore) classes.push_back(e->Processor.EfficiencyClass);
                if (e->Size == 0) break;
                off += e->Size;
            }
            if (!classes.empty()) {
                const int top = *std::max_element(classes.begin(), classes.end());
                t.physical    = static_cast<int>(classes.size());
                t.performance = static_cast<int>(std::count(classes.begin(), classes.end(), top));
                t.source = "Windows";
            }
        }
    }
#else
    CpuTopology s;
    if (readLinuxCpuTopology("/sys", s)) {
        s.logical = std::max(s.logical, t.logical);
        t = s;
    }
#endif
    t.physical    = std::clamp(t.physical, 1, t.logical);
    t.performance = std::clamp(t.performance, 1, t.physical);
    return t;
}

const CpuTopology& cpuTopology()
{
    static const CpuTopology t = detect();
    return t;
}

AutoThreads autoThreads(const CpuTopology& t)
{
    AutoThreads a;
    a.generation = std::max(1, t.performance);
    a.batch      = std::max(a.generation, t.physical);
    return a;
}

} // namespace seldon
