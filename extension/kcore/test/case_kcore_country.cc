/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Case: "k-core decomposition: who is at the unbreakable centre, and who fell out?"
//
// Two substrates:
//   1. KCORE_COAUTHOR(year_min, year_max)  — country-level coauthor graph,
//      4 windows. Trajectory of each country's coreness.
//   2. KCORE_PROJECT(year_min, year_max)   — organization-level project
//      participation graph, 2 windows: H2020 (2014-2020) vs Horizon Europe
//      (2021-2024). Brexit signal = UK orgs that lost coreness.
//
// Outputs:
//   country_coreness_pivot_<ts>.csv          — country × window coreness
//   country_max_core_members_<ts>.csv         — countries in deepest k-shell, per window
//   project_org_coreness_<ts>.csv             — org_id, country, coreness in H2020 + HE
//   project_brexit_diff_<ts>.csv              — UK orgs: coreness in H2020 vs HE
//   crosssub_country_coreness_compare_<ts>.csv — country: coauthor coreness vs
//                                                 country aggregated project coreness
//                                                 (max coreness over country's orgs)

#include <neug/main/neug_db.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

static constexpr const char* kOpenAireDbPath =
    "/mnt/shunyangl/louvain/tools/python_bind/openaire";

static std::string findBuildRoot() {
    auto exe = std::filesystem::read_symlink("/proc/self/exe");
    auto dir = exe.parent_path();
    const std::string target = "extension/kcore/libkcore.neug_extension";
    for (int i = 0; i < 8; i++) {
        if (std::filesystem::exists(dir / target)) return dir.string();
        dir = dir.parent_path();
    }
    return "";
}

bool loadExtension(neug::Connection* conn) {
    std::cout << "\n=== Loading kcore extension ===" << std::endl;
    std::string build_root = findBuildRoot();
    if (!build_root.empty()) {
        std::cout << "  Build root: " << build_root << std::endl;
        setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1);
    } else {
        std::cerr << "  WARNING: built extension library not found.\n";
    }
    auto load_res = conn->Query("LOAD kcore;");
    if (!load_res.has_value()) {
        std::cerr << "Failed to load kcore extension: "
                  << load_res.error().ToString() << std::endl;
        return false;
    }
    std::cout << "kcore extension loaded successfully" << std::endl;
    return true;
}

static int parseResultRow(const neug::QueryResult& rs,
                          const std::string& tag,
                          int64_t& num_vertices,
                          int64_t& max_coreness,
                          int64_t& num_in_max_core,
                          int64_t& num_edges,
                          std::string& result_file) {
    auto& resp = rs.response();
    if (resp.row_count() != 1) {
        std::cerr << "[" << tag << "] expected 1 row, got "
                  << resp.row_count() << std::endl;
        return 1;
    }
    std::string status = resp.arrays(0).string_array().values(0);
    if (status != "success") {
        std::cerr << "[" << tag << "] status=" << status << std::endl;
        return 1;
    }
    num_vertices    = resp.arrays(1).int64_array().values(0);
    max_coreness    = resp.arrays(2).int64_array().values(0);
    num_in_max_core = resp.arrays(3).int64_array().values(0);
    num_edges       = resp.arrays(4).int64_array().values(0);
    result_file     = resp.arrays(5).string_array().values(0);
    return 0;
}

// ---------- Coauthor side ----------

struct CountryCorenessRow { std::string country_code; int coreness = 0; };

static int loadCoauthorCSV(const std::string& path,
                           std::vector<CountryCorenessRow>& out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open: " << path << std::endl;
        return 1;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,country_code,coreness") {
        std::cerr << "Unexpected header: " << header << std::endl;
        return 1;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string gid, cc, cn;
        std::getline(ss, gid, ',');
        std::getline(ss, cc, ',');
        std::getline(ss, cn, ',');
        if (cc.empty()) continue;
        out.push_back({cc, std::stoi(cn)});
    }
    return 0;
}

struct CoauthorWindow {
    std::string label;
    int64_t num_vertices = 0;
    int64_t max_coreness = 0;
    int64_t num_in_max_core = 0;
    int64_t num_edges = 0;
    std::string result_file;
    std::vector<CountryCorenessRow> rows;
};

static int runCoauthorWindow(neug::Connection* conn,
                             int64_t ymin, int64_t ymax,
                             CoauthorWindow& out) {
    std::ostringstream lbl;
    if (ymin == ymax) lbl << ymin;
    else lbl << ymin << "-" << ymax;
    out.label = lbl.str();
    std::cout << "\n=== Coauthor window " << out.label << " ===" << std::endl;

    std::ostringstream qs;
    qs << "CALL KCORE_COAUTHOR(" << ymin << ", " << ymax << ") RETURN *;";
    std::cout << "  " << qs.str() << std::endl;
    auto res = conn->Query(qs.str());
    if (!res.has_value()) {
        std::cerr << "  KCORE_COAUTHOR failed: "
                  << res.error().ToString() << std::endl;
        return 1;
    }
    if (parseResultRow(res.value(), "KCORE_COAUTHOR",
                       out.num_vertices, out.max_coreness,
                       out.num_in_max_core, out.num_edges,
                       out.result_file)) return 1;
    std::cout << "  V=" << out.num_vertices
              << " E=" << out.num_edges
              << " max_core=" << out.max_coreness
              << " #in_max=" << out.num_in_max_core
              << " csv=" << out.result_file << std::endl;
    return loadCoauthorCSV(out.result_file, out.rows);
}

// ---------- Project side ----------

struct OrgCorenessRow {
    std::string org_id;
    std::string country_code;
    int coreness = 0;
};

static int loadProjectCSV(const std::string& path,
                          std::vector<OrgCorenessRow>& out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open: " << path << std::endl;
        return 1;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,org_id,country_code,coreness") {
        std::cerr << "Unexpected header: " << header << std::endl;
        return 1;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        // gid,"org_id",cc,coreness
        auto open_q = line.find('"');
        auto close_q = (open_q == std::string::npos) ? std::string::npos
                                                     : line.find('"', open_q + 1);
        OrgCorenessRow r;
        if (open_q != std::string::npos && close_q != std::string::npos &&
            open_q < close_q) {
            r.org_id = line.substr(open_q + 1, close_q - open_q - 1);
            auto tail_start = line.find(',', close_q);
            if (tail_start == std::string::npos) continue;
            std::stringstream ss(line.substr(tail_start + 1));
            std::string cc, cn;
            std::getline(ss, cc, ',');
            std::getline(ss, cn, ',');
            r.country_code = cc;
            r.coreness = cn.empty() ? 0 : std::stoi(cn);
        } else {
            std::stringstream ss(line);
            std::string gid, oid, cc, cn;
            std::getline(ss, gid, ',');
            std::getline(ss, oid, ',');
            std::getline(ss, cc, ',');
            std::getline(ss, cn, ',');
            r.org_id = oid;
            r.country_code = cc;
            r.coreness = cn.empty() ? 0 : std::stoi(cn);
        }
        out.push_back(std::move(r));
    }
    return 0;
}

struct ProjectWindow {
    std::string label;
    int64_t num_vertices = 0;
    int64_t max_coreness = 0;
    int64_t num_in_max_core = 0;
    int64_t num_edges = 0;
    std::string result_file;
    std::vector<OrgCorenessRow> rows;
};

static int runProjectWindow(neug::Connection* conn,
                            int64_t ymin, int64_t ymax,
                            int64_t min_org_projects,
                            int64_t min_edge_weight,
                            ProjectWindow& out) {
    std::ostringstream lbl;
    lbl << ymin << "-" << ymax;
    out.label = lbl.str();
    std::cout << "\n=== Project window " << out.label << " ===" << std::endl;

    std::ostringstream qs;
    qs << "CALL KCORE_PROJECT(" << ymin << ", " << ymax << ", "
       << min_org_projects << ", " << min_edge_weight << ") RETURN *;";
    std::cout << "  " << qs.str() << std::endl;
    auto res = conn->Query(qs.str());
    if (!res.has_value()) {
        std::cerr << "  KCORE_PROJECT failed: "
                  << res.error().ToString() << std::endl;
        return 1;
    }
    if (parseResultRow(res.value(), "KCORE_PROJECT",
                       out.num_vertices, out.max_coreness,
                       out.num_in_max_core, out.num_edges,
                       out.result_file)) return 1;
    std::cout << "  V=" << out.num_vertices
              << " E=" << out.num_edges
              << " max_core=" << out.max_coreness
              << " #in_max=" << out.num_in_max_core
              << " csv=" << out.result_file << std::endl;
    return loadProjectCSV(out.result_file, out.rows);
}

// ---------- Outputs ----------

static void writeCoauthorPivot(const std::vector<CoauthorWindow>& windows,
                               const std::string& path) {
    std::set<std::string> all_cc;
    std::vector<std::unordered_map<std::string, int>> per(windows.size());
    for (size_t i = 0; i < windows.size(); ++i) {
        for (const auto& r : windows[i].rows) {
            per[i][r.country_code] = r.coreness;
            all_cc.insert(r.country_code);
        }
    }
    std::ofstream ofs(path);
    if (!ofs.is_open()) { std::cerr << "Cannot open: " << path << std::endl; return; }
    ofs << "country_code";
    for (const auto& w : windows) ofs << ",w_" << w.label;
    ofs << "\n";
    for (const auto& cc : all_cc) {
        ofs << cc;
        for (size_t i = 0; i < windows.size(); ++i) {
            ofs << ",";
            auto it = per[i].find(cc);
            if (it != per[i].end()) ofs << it->second;
        }
        ofs << "\n";
    }
    std::cout << "  wrote " << path << " (" << all_cc.size()
              << " countries × " << windows.size() << " windows)" << std::endl;
}

static void writeMaxCoreMembers(const std::vector<CoauthorWindow>& windows,
                                const std::string& path) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) { std::cerr << "Cannot open: " << path << std::endl; return; }
    ofs << "window,max_coreness,country_code\n";
    for (const auto& w : windows) {
        for (const auto& r : w.rows) {
            if (r.coreness == w.max_coreness) {
                ofs << w.label << "," << w.max_coreness << "," << r.country_code << "\n";
            }
        }
    }
    std::cout << "  wrote " << path << std::endl;
}

// Brexit-style diff: per UK org, coreness in H2020 vs in Horizon Europe.
// Generalized to per-country-code: any (cc) seen in either window contributes.
static void writeBrexitDiff(const ProjectWindow& a, const ProjectWindow& b,
                            const std::string& path) {
    // Build org -> coreness for each window.
    std::unordered_map<std::string, OrgCorenessRow> ma, mb;
    for (const auto& r : a.rows) ma[r.org_id] = r;
    for (const auto& r : b.rows) mb[r.org_id] = r;

    std::set<std::string> all_org;
    for (const auto& kv : ma) all_org.insert(kv.first);
    for (const auto& kv : mb) all_org.insert(kv.first);

    std::ofstream ofs(path);
    if (!ofs.is_open()) { std::cerr << "Cannot open: " << path << std::endl; return; }
    ofs << "org_id,country_code," << "core_" << a.label << "," << "core_" << b.label
        << ",delta\n";
    for (const auto& oid : all_org) {
        std::string cc;
        int ca = -1, cb = -1;
        if (auto it = ma.find(oid); it != ma.end()) { ca = it->second.coreness; cc = it->second.country_code; }
        if (auto it = mb.find(oid); it != mb.end()) { cb = it->second.coreness; if (cc.empty()) cc = it->second.country_code; }
        int delta = (ca >= 0 && cb >= 0) ? (cb - ca) : 0;
        ofs << "\"" << oid << "\"," << cc << ",";
        if (ca >= 0) { ofs << ca; }
        ofs << ",";
        if (cb >= 0) { ofs << cb; }
        ofs << ",";
        if (ca >= 0 && cb >= 0) { ofs << delta; }
        ofs << "\n";
    }
    std::cout << "  wrote " << path << " (" << all_org.size() << " orgs total)" << std::endl;
}

// Per-country aggregate: max coreness among orgs of that country in window.
// This lets us cross-compare with coauthor-level coreness.
static std::unordered_map<std::string, int>
countryMaxOrgCoreness(const ProjectWindow& w) {
    std::unordered_map<std::string, int> out;
    for (const auto& r : w.rows) {
        if (r.country_code.empty()) continue;
        auto it = out.find(r.country_code);
        if (it == out.end() || r.coreness > it->second) {
            out[r.country_code] = r.coreness;
        }
    }
    return out;
}

static void writeCrossSubstrate(
        const std::vector<CoauthorWindow>& co_windows,
        const ProjectWindow& proj_h2020,
        const ProjectWindow& proj_he,
        const std::string& path) {
    auto h2020_country = countryMaxOrgCoreness(proj_h2020);
    auto he_country = countryMaxOrgCoreness(proj_he);

    // Pick the coauthor windows that align to H2020 (2018-2019 + 2020-2021 avg)
    // and HE (2022-2023 + 2024 avg). Simple max across the pair for robustness.
    auto pickCoauthorMax = [&](const std::set<std::string>& wlabels) {
        std::unordered_map<std::string, int> out;
        for (const auto& w : co_windows) {
            if (!wlabels.count(w.label)) continue;
            for (const auto& r : w.rows) {
                auto it = out.find(r.country_code);
                if (it == out.end() || r.coreness > it->second) {
                    out[r.country_code] = r.coreness;
                }
            }
        }
        return out;
    };
    auto co_h2020 = pickCoauthorMax({"2018-2019", "2020-2021"});
    auto co_he    = pickCoauthorMax({"2022-2023", "2024"});

    std::set<std::string> all_cc;
    for (const auto& kv : co_h2020) all_cc.insert(kv.first);
    for (const auto& kv : co_he)    all_cc.insert(kv.first);
    for (const auto& kv : h2020_country) all_cc.insert(kv.first);
    for (const auto& kv : he_country)    all_cc.insert(kv.first);

    std::ofstream ofs(path);
    if (!ofs.is_open()) { std::cerr << "Cannot open: " << path << std::endl; return; }
    ofs << "country_code,coauth_h2020_max,coauth_he_max,"
           "proj_h2020_max,proj_he_max,"
           "coauth_delta,proj_delta\n";
    for (const auto& cc : all_cc) {
        int a = -1, b = -1, c = -1, d = -1;
        if (auto it = co_h2020.find(cc); it != co_h2020.end()) a = it->second;
        if (auto it = co_he.find(cc);    it != co_he.end())    b = it->second;
        if (auto it = h2020_country.find(cc); it != h2020_country.end()) c = it->second;
        if (auto it = he_country.find(cc);    it != he_country.end())    d = it->second;
        ofs << cc << ",";
        if (a >= 0) { ofs << a; }
        ofs << ",";
        if (b >= 0) { ofs << b; }
        ofs << ",";
        if (c >= 0) { ofs << c; }
        ofs << ",";
        if (d >= 0) { ofs << d; }
        ofs << ",";
        if (a >= 0 && b >= 0) { ofs << (b - a); }
        ofs << ",";
        if (c >= 0 && d >= 0) { ofs << (d - c); }
        ofs << "\n";
    }
    std::cout << "  wrote " << path << " (" << all_cc.size() << " countries)" << std::endl;
}

// Console punch-line: for each coauthor window, list countries by coreness
// (sorted descending), printing the top 30 and any country with coreness ==
// max_core (the "innermost ring").
static void printCoauthorTopByWindow(const CoauthorWindow& w) {
    std::vector<CountryCorenessRow> rows = w.rows;
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b){ return a.coreness > b.coreness; });
    std::cout << "\n  [top by coreness w=" << w.label << "] max_core="
              << w.max_coreness << std::endl;
    int shown = 0;
    for (const auto& r : rows) {
        if (shown >= 30 && r.coreness < w.max_coreness) break;
        std::cout << "    " << r.country_code << "=" << r.coreness;
        ++shown;
        if (shown % 10 == 0) std::cout << "\n   ";
        else std::cout << "  ";
    }
    std::cout << std::endl;
}

// Print Brexit punch line: top 20 UK orgs by coreness loss (positive delta = loss).
static void printBrexitPunchline(const ProjectWindow& a, const ProjectWindow& b) {
    std::unordered_map<std::string, OrgCorenessRow> ma, mb;
    for (const auto& r : a.rows) ma[r.org_id] = r;
    for (const auto& r : b.rows) mb[r.org_id] = r;

    struct Item { std::string oid, cc; int ca, cb, delta; };
    std::vector<Item> items;
    for (const auto& [oid, ra] : ma) {
        auto it = mb.find(oid);
        int ca = ra.coreness;
        int cb = (it != mb.end()) ? it->second.coreness : 0;
        items.push_back({oid, ra.country_code, ca, cb, ca - cb});
    }
    std::sort(items.begin(), items.end(),
              [](const auto& x, const auto& y){ return x.delta > y.delta; });
    auto print = [&](const std::string& cc_filter, int top_n, const std::string& tag) {
        std::cout << "\n  [" << tag << "] top-" << top_n << " coreness LOSS ("
                  << a.label << " → " << b.label << ")" << std::endl;
        int shown = 0;
        for (const auto& it : items) {
            if (!cc_filter.empty() && it.cc != cc_filter) continue;
            std::cout << "    " << it.cc << "  ca=" << it.ca << " → cb=" << it.cb
                      << "  Δ=" << it.delta << "  org=" << it.oid << std::endl;
            if (++shown >= top_n) break;
        }
    };
    print("GB", 15, "Brexit-UK");
    print("",   20, "All-orgs largest losses");
}

int main(int argc, char* argv[]) {
    int64_t coauthor_min_w = 1;
    int64_t proj_min_org_projects = 2;
    int64_t proj_min_edge_weight = 1;
    if (argc > 1) coauthor_min_w = std::stoll(argv[1]);
    if (argc > 2) proj_min_org_projects = std::stoll(argv[2]);
    if (argc > 3) proj_min_edge_weight = std::stoll(argv[3]);
    (void)coauthor_min_w;  // currently using default overload, threshold left default

    std::cout << "============================================" << std::endl;
    std::cout << "  NeuG k-core — country coauthor + org project" << std::endl;
    std::cout << "  proj min_org_projects=" << proj_min_org_projects
              << ", proj min_edge_weight=" << proj_min_edge_weight << std::endl;
    std::cout << "============================================" << std::endl;

    const std::string db_path = kOpenAireDbPath;
    if (!std::filesystem::exists(db_path)) {
        std::cerr << "DB not found: " << db_path << std::endl;
        return 1;
    }
    neug::NeugDB db;
    if (!db.Open(db_path, /*max_num_threads=*/0, neug::DBMode::READ_WRITE)) {
        std::cerr << "Failed to open DB" << std::endl;
        return 1;
    }
    auto conn = db.Connect();
    if (!conn) { db.Close(); return 1; }
    if (!loadExtension(conn.get())) { conn.reset(); db.Close(); return 1; }

    // ---- Coauthor: 4 windows ----
    const std::vector<std::pair<int64_t, int64_t>> kCoWindows = {
        {2018, 2019}, {2020, 2021}, {2022, 2023}, {2024, 2024},
    };
    std::vector<CoauthorWindow> co_windows;
    co_windows.reserve(kCoWindows.size());
    int failures = 0;
    for (const auto& [ymin, ymax] : kCoWindows) {
        CoauthorWindow w;
        failures += runCoauthorWindow(conn.get(), ymin, ymax, w);
        co_windows.push_back(std::move(w));
    }

    // ---- Project: H2020 vs Horizon Europe ----
    ProjectWindow h2020, he;
    failures += runProjectWindow(conn.get(), 2014, 2020,
                                 proj_min_org_projects, proj_min_edge_weight, h2020);
    failures += runProjectWindow(conn.get(), 2021, 2024,
                                 proj_min_org_projects, proj_min_edge_weight, he);

    // ---- Outputs ----
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string out_dir = "/tmp/p/neug_kcore";
    std::filesystem::create_directories(out_dir);

    std::cout << "\n=== Outputs ===" << std::endl;
    writeCoauthorPivot(co_windows,
        out_dir + "/country_coreness_pivot_" + std::to_string(ts) + ".csv");
    writeMaxCoreMembers(co_windows,
        out_dir + "/country_max_core_members_" + std::to_string(ts) + ".csv");
    writeBrexitDiff(h2020, he,
        out_dir + "/project_brexit_diff_" + std::to_string(ts) + ".csv");
    writeCrossSubstrate(co_windows, h2020, he,
        out_dir + "/crosssub_country_coreness_compare_" + std::to_string(ts) + ".csv");

    std::cout << "\n=== Coauthor max-core members per window ===" << std::endl;
    for (const auto& w : co_windows) {
        std::cout << "  " << w.label << " — V=" << w.num_vertices
                  << " max_core=" << w.max_coreness
                  << " #in_max=" << w.num_in_max_core << "  members: ";
        for (const auto& r : w.rows) {
            if (r.coreness == w.max_coreness) std::cout << r.country_code << " ";
        }
        std::cout << std::endl;
    }

    std::cout << "\n=== Coauthor coreness ranking per window (top 30) ===";
    for (const auto& w : co_windows) printCoauthorTopByWindow(w);

    std::cout << "\n=== Project k-core summary ===" << std::endl;
    std::cout << "  H2020   (" << h2020.label << "): V=" << h2020.num_vertices
              << " max_core=" << h2020.max_coreness
              << " #in_max=" << h2020.num_in_max_core << std::endl;
    std::cout << "  HE      (" << he.label   << "): V=" << he.num_vertices
              << " max_core=" << he.max_coreness
              << " #in_max=" << he.num_in_max_core << std::endl;

    printBrexitPunchline(h2020, he);

    std::cout << "\n============================================" << std::endl;
    if (failures == 0) std::cout << "  k-core case PASSED!" << std::endl;
    else std::cout << "  k-core case FAILED (" << failures << " step errors)"
                   << std::endl;
    std::cout << "============================================" << std::endl;

    conn.reset();
    db.Close();
    return failures;
}
