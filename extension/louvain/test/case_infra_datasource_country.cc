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

// Case: "The camps split, but the infrastructure didn't."
//
// For each year window we run two country-level Louvain variants and compare
// their partitions:
//
//   1. LOUVAIN_COAUTHOR       — countries clustered by who co-authors with whom
//                              (Publication → Organization → country_code).
//   2. LOUVAIN_INFRA_COUNTRY  — countries clustered by which Datasource hosts
//                              publications with their authors
//                              (Datasource → Publication → Organization →
//                              country_code, min-coupling projection).
//
// Hypothesis: scientific collaboration is multipolar (co-author Louvain yields
// several geopolitical blocs), but open-science infrastructure is single-pole
// (infra Louvain collapses most countries into one mega-community anchored by
// arXiv/Zenodo/etc.). The outputs below let the story be read off directly:
// modularity trajectories, community counts, and a per-window country table
// with (coauthor_comm, infra_comm) side by side.

#include <neug/main/neug_db.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

// NeuG data directory (contains graph.yaml, checkpoint/, etc.).
static constexpr const char* kOpenAireDbPath =
    "/mnt/lyk/openaire_18_26_neug/neug_db_1775109652105";

static std::string findBuildRoot() {
    auto exe = std::filesystem::read_symlink("/proc/self/exe");
    auto dir = exe.parent_path();
    const std::string target = "extension/louvain/liblouvain.neug_extension";
    for (int i = 0; i < 8; i++) {
        if (std::filesystem::exists(dir / target)) {
            return dir.string();
        }
        dir = dir.parent_path();
    }
    return "";
}

bool loadExtension(neug::Connection* conn) {
    std::cout << "\n=== Loading Louvain Extension ===" << std::endl;

    std::string build_root = findBuildRoot();
    if (!build_root.empty()) {
        std::cout << "  Build root: " << build_root << std::endl;
        setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1);
    } else {
        std::cerr << "  WARNING: Could not locate built extension library."
                  << std::endl;
        std::cerr << "  Set NEUG_EXTENSION_HOME_PYENV to the build directory."
                  << std::endl;
    }

    auto load_res = conn->Query("LOAD louvain;");
    if (!load_res.has_value()) {
        std::cerr << "Failed to load louvain extension: "
                  << load_res.error().ToString() << std::endl;
        return false;
    }
    std::cout << "Louvain extension loaded successfully" << std::endl;
    return true;
}

// Parallel to WindowResult in test_louvain_install.cc but carries both
// coauthor and infra runs for the same window.
struct WindowComparison {
    std::string label;

    // coauthor run
    int64_t co_num_vertices = 0;
    int64_t co_num_communities = 0;
    double  co_modularity = 0.0;
    int64_t co_levels = 0;
    std::string co_result_file;
    std::unordered_map<std::string, int> co_country_to_comm;

    // infra run
    int64_t in_num_vertices = 0;
    int64_t in_num_communities = 0;
    double  in_modularity = 0.0;
    int64_t in_levels = 0;
    std::string in_result_file;
    std::unordered_map<std::string, int> in_country_to_comm;
};

// Parse a 6-column result row (shape shared by LOUVAIN_COAUTHOR and
// LOUVAIN_INFRA_COUNTRY) into vertices / communities / modularity / levels /
// result_file. Returns 0 on success, 1 on failure.
static int parseResultRow(const neug::QueryResult& rs,
                          const std::string& tag,
                          int64_t& num_vertices,
                          int64_t& num_communities,
                          double&  modularity,
                          int64_t& levels,
                          std::string& result_file) {
    auto& resp = rs.response();
    if (resp.row_count() != 1) {
        std::cerr << "[" << tag << "] Expected 1 row, got "
                  << resp.row_count() << std::endl;
        return 1;
    }
    std::string status = resp.arrays(0).string_array().values(0);
    if (status != "success") {
        std::cerr << "[" << tag << "] status=" << status << std::endl;
        return 1;
    }
    num_vertices    = resp.arrays(1).int64_array().values(0);
    num_communities = resp.arrays(2).int64_array().values(0);
    modularity      = resp.arrays(3).double_array().values(0);
    levels          = resp.arrays(4).int64_array().values(0);
    result_file     = resp.arrays(5).string_array().values(0);
    return 0;
}

// Load a result CSV into country_code -> community_id.
// Expected header: "global_id,country_code,community_id".
static int loadCountryCommunityCSV(
        const std::string& filepath,
        int64_t expected_rows,
        std::unordered_map<std::string, int>& out) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open CSV: " << filepath << std::endl;
        return 1;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,country_code,community_id") {
        std::cerr << "Unexpected header: " << header << std::endl;
        return 1;
    }
    std::string line;
    int64_t row_count = 0;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        ++row_count;
        std::stringstream ss(line);
        std::string gid, cc, cid;
        std::getline(ss, gid, ',');
        std::getline(ss, cc, ',');
        std::getline(ss, cid, ',');
        if (cc.empty()) {
            std::cerr << "Empty country_code on row " << row_count << std::endl;
            return 1;
        }
        out[cc] = std::stoi(cid);
    }
    if (expected_rows > 0 && row_count != expected_rows) {
        std::cerr << "Row count mismatch: CSV=" << row_count
                  << " reported=" << expected_rows << std::endl;
        return 1;
    }
    return 0;
}

// Run a single year window: call both CALL LOUVAIN_COAUTHOR and
// CALL LOUVAIN_INFRA_COUNTRY, parse their CSVs, populate `out`.
// Returns 0 on success, 1 if any step failed.
static int runWindow(neug::Connection* conn, int64_t ymin, int64_t ymax,
                     WindowComparison& out) {
    std::ostringstream label_ss;
    if (ymin == ymax) label_ss << ymin;
    else label_ss << ymin << "-" << ymax;
    out.label = label_ss.str();

    std::cout << "\n=== Window " << out.label << " ===" << std::endl;

    // --- LOUVAIN_COAUTHOR ---
    {
        std::ostringstream qs;
        qs << "CALL LOUVAIN_COAUTHOR(" << ymin << ", " << ymax
           << ") RETURN *;";
        std::cout << "  " << qs.str() << std::endl;
        auto res = conn->Query(qs.str());
        if (!res.has_value()) {
            std::cerr << "  LOUVAIN_COAUTHOR failed: "
                      << res.error().ToString() << std::endl;
            return 1;
        }
        if (parseResultRow(res.value(), "LOUVAIN_COAUTHOR",
                           out.co_num_vertices, out.co_num_communities,
                           out.co_modularity, out.co_levels,
                           out.co_result_file) != 0) {
            return 1;
        }
        std::cout << "  [coauthor] vertices=" << out.co_num_vertices
                  << " communities=" << out.co_num_communities
                  << " modularity=" << out.co_modularity
                  << " levels=" << out.co_levels << std::endl;
        std::cout << "  [coauthor] CSV: " << out.co_result_file << std::endl;
        if (loadCountryCommunityCSV(out.co_result_file,
                                    out.co_num_vertices,
                                    out.co_country_to_comm) != 0) {
            return 1;
        }
    }

    // --- LOUVAIN_INFRA_COUNTRY ---
    {
        std::ostringstream qs;
        qs << "CALL LOUVAIN_INFRA_COUNTRY(" << ymin << ", " << ymax
           << ") RETURN *;";
        std::cout << "  " << qs.str() << std::endl;
        auto res = conn->Query(qs.str());
        if (!res.has_value()) {
            std::cerr << "  LOUVAIN_INFRA_COUNTRY failed: "
                      << res.error().ToString() << std::endl;
            return 1;
        }
        if (parseResultRow(res.value(), "LOUVAIN_INFRA_COUNTRY",
                           out.in_num_vertices, out.in_num_communities,
                           out.in_modularity, out.in_levels,
                           out.in_result_file) != 0) {
            return 1;
        }
        std::cout << "  [infra]    vertices=" << out.in_num_vertices
                  << " communities=" << out.in_num_communities
                  << " modularity=" << out.in_modularity
                  << " levels=" << out.in_levels << std::endl;
        std::cout << "  [infra]    CSV: " << out.in_result_file << std::endl;
        if (loadCountryCommunityCSV(out.in_result_file,
                                    out.in_num_vertices,
                                    out.in_country_to_comm) != 0) {
            return 1;
        }
    }

    // Sanity: infra should see at least as many countries as coauthor
    // (any country that co-authors has at least one hosted pub). If not,
    // print a warning but don't fail — edge cases exist.
    if (out.in_num_vertices < out.co_num_vertices) {
        std::cerr << "  NOTE: infra vertex count (" << out.in_num_vertices
                  << ") < coauthor vertex count (" << out.co_num_vertices
                  << ") — possible filter drift" << std::endl;
    }

    return 0;
}

// Two-column pivot per run type: rows=country, cols=window, cell=community id.
// One file per view; callers pass view="coauthor" or "infra" and pick the
// matching map off of each WindowComparison.
static void writePivotCSV(const std::vector<WindowComparison>& windows,
                          const std::string& view,
                          const std::string& out_path) {
    auto pick = [&](const WindowComparison& w)
        -> const std::unordered_map<std::string, int>& {
        return view == "coauthor" ? w.co_country_to_comm
                                  : w.in_country_to_comm;
    };

    std::set<std::string> all_countries;
    for (const auto& w : windows) {
        for (const auto& kv : pick(w)) all_countries.insert(kv.first);
    }
    std::ofstream ofs(out_path);
    if (!ofs.is_open()) {
        std::cerr << "Cannot open pivot file: " << out_path << std::endl;
        return;
    }
    ofs << "country_code";
    for (const auto& w : windows) ofs << ",w_" << w.label;
    ofs << "\n";
    for (const auto& cc : all_countries) {
        ofs << cc;
        for (const auto& w : windows) {
            const auto& m = pick(w);
            auto it = m.find(cc);
            ofs << ",";
            if (it != m.end()) ofs << it->second;
        }
        ofs << "\n";
    }
    ofs.close();
    std::cout << "  Pivot (" << view << ") → " << out_path
              << " (" << all_countries.size() << " countries × "
              << windows.size() << " windows)" << std::endl;
}

// Per-window side-by-side: rows=country, cols=(coauthor_comm, infra_comm).
// Easiest table for eyeballing "do the two partitions agree for country X?".
static void writeComparisonCSV(const WindowComparison& w,
                               const std::string& out_path) {
    std::set<std::string> all_countries;
    for (const auto& kv : w.co_country_to_comm) all_countries.insert(kv.first);
    for (const auto& kv : w.in_country_to_comm) all_countries.insert(kv.first);

    std::ofstream ofs(out_path);
    if (!ofs.is_open()) {
        std::cerr << "Cannot open comparison file: " << out_path << std::endl;
        return;
    }
    ofs << "country_code,coauthor_comm,infra_comm\n";
    for (const auto& cc : all_countries) {
        ofs << cc << ",";
        auto c = w.co_country_to_comm.find(cc);
        if (c != w.co_country_to_comm.end()) ofs << c->second;
        ofs << ",";
        auto i = w.in_country_to_comm.find(cc);
        if (i != w.in_country_to_comm.end()) ofs << i->second;
        ofs << "\n";
    }
    ofs.close();
    std::cout << "  Compare w=" << w.label << " → " << out_path
              << " (" << all_countries.size() << " countries)" << std::endl;
}

// Partition-contingency summary: for a single window, for each
// (coauthor_comm, infra_comm) pair count the number of countries that fall in
// that cell. Useful for spotting "infra collapses coauthor blocs A, B, C all
// into community 0" without drilling into the CSV.
static void printContingency(const WindowComparison& w) {
    std::map<std::pair<int, int>, int> cell;
    for (const auto& [cc, co_c] : w.co_country_to_comm) {
        auto it = w.in_country_to_comm.find(cc);
        if (it == w.in_country_to_comm.end()) continue;
        cell[{co_c, it->second}]++;
    }
    if (cell.empty()) {
        std::cout << "  [contingency w=" << w.label
                  << "] no overlapping countries" << std::endl;
        return;
    }
    std::cout << "  [contingency w=" << w.label
              << "] coauthor_comm × infra_comm → #countries" << std::endl;
    for (const auto& [k, n] : cell) {
        std::cout << "      (" << k.first << ", " << k.second << ") -> "
                  << n << std::endl;
    }
}

int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "  NeuG Louvain — Infrastructure case (OpenAire)" << std::endl;
    std::cout << "============================================" << std::endl;

    const std::string db_path = kOpenAireDbPath;
    if (!std::filesystem::exists(db_path)) {
        std::cerr << "Database directory does not exist: " << db_path
                  << std::endl;
        return 1;
    }

    std::cout << "\n=== Opening Database ===" << std::endl;
    std::cout << "Database path: " << db_path << std::endl;

    neug::NeugDB db;
    if (!db.Open(db_path, /*max_num_threads=*/0, neug::DBMode::READ_WRITE)) {
        std::cerr << "Failed to open database: " << db_path << std::endl;
        return 1;
    }
    std::cout << "Database opened successfully" << std::endl;

    auto conn = db.Connect();
    if (!conn) {
        std::cerr << "Failed to connect to database" << std::endl;
        db.Close();
        return 1;
    }
    std::cout << "Connected to database" << std::endl;

    if (!loadExtension(conn.get())) {
        conn.reset();
        db.Close();
        return 1;
    }

    // Same windows used in test_louvain_install.cc so the infra trajectory
    // can be overlaid on the existing coauthor pivot.
    const std::vector<std::pair<int64_t, int64_t>> kWindows = {
        {2018, 2019},
        {2020, 2021},
        {2022, 2023},
        {2024, 2024},
    };

    std::vector<WindowComparison> windows;
    windows.reserve(kWindows.size());
    int failures = 0;
    for (const auto& [ymin, ymax] : kWindows) {
        WindowComparison w;
        failures += runWindow(conn.get(), ymin, ymax, w);
        windows.push_back(std::move(w));
    }

    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string out_dir = "/tmp/p/neug_louvain";
    std::filesystem::create_directories(out_dir);

    const std::string coauth_pivot =
        out_dir + "/infra_case_pivot_coauthor_" + std::to_string(ts) + ".csv";
    const std::string infra_pivot =
        out_dir + "/infra_case_pivot_infra_"    + std::to_string(ts) + ".csv";
    std::cout << "\n=== Writing pivots ===" << std::endl;
    writePivotCSV(windows, "coauthor", coauth_pivot);
    writePivotCSV(windows, "infra",    infra_pivot);

    std::cout << "\n=== Per-window comparison files ===" << std::endl;
    for (const auto& w : windows) {
        const std::string cmp_path =
            out_dir + "/infra_case_compare_" + w.label + "_" +
            std::to_string(ts) + ".csv";
        writeComparisonCSV(w, cmp_path);
    }

    std::cout << "\n=== Modularity / community-count trajectory ===" << std::endl;
    std::cout << "  (coauthor vs infra, per window)" << std::endl;
    for (const auto& w : windows) {
        std::cout << "    " << w.label
                  << "  coauthor: mod=" << w.co_modularity
                  << " comm=" << w.co_num_communities
                  << " verts=" << w.co_num_vertices
                  << "  ||  infra: mod=" << w.in_modularity
                  << " comm=" << w.in_num_communities
                  << " verts=" << w.in_num_vertices
                  << std::endl;
    }

    std::cout << "\n=== Contingency tables ===" << std::endl;
    for (const auto& w : windows) {
        printContingency(w);
    }

    std::cout << "\n============================================" << std::endl;
    if (failures == 0) {
        std::cout << "  Infra case PASSED!" << std::endl;
    } else {
        std::cout << "  Infra case FAILED (" << failures << " window errors)"
                  << std::endl;
    }
    std::cout << "============================================" << std::endl;

    conn.reset();
    db.Close();
    return failures;
}
