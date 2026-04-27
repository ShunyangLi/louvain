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

// Case: "Datasource-level Louvain — does the platform layer split with politics?"
//
// For each year window we run two Louvain variants:
//
//   1. LOUVAIN_COAUTHOR     — country-level (Pub→Org→country).  Already known
//                              to track geopolitics: RU bloc shifts post-2022,
//                              CN/US decoupling, etc.
//
//   2. LOUVAIN_DATASOURCE   — datasource-level (Datasource×Datasource via
//                              shared in-window publications). Each datasource
//                              is annotated with its provider organization +
//                              that organization's country_code.
//
// The cross-section comes from the datasource ANNOTATIONS, not from the
// Louvain projection itself: after Louvain assigns each ds to a community,
// we summarize each ds-community by the country distribution of its provider
// organizations, and then for each country compute "the dominant ds-community
// it provides datasources to". That dominant ds-community can be compared
// directly against the country's coauthor community for the same window.
//
// Outputs:
//   ds_pivot_<ts>.csv             — datasource_id × window → community_id
//   ds_country_dist_<window>_<ts>.csv
//                                 — per (community, country) count of dsplaces
//   country_compare_<window>_<ts>.csv
//                                 — country, coauthor_comm, dominant_ds_comm,
//                                   ds_count_in_dominant, ds_total_for_country

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
    const std::string target = "extension/louvain/liblouvain.neug_extension";
    for (int i = 0; i < 8; i++) {
        if (std::filesystem::exists(dir / target)) return dir.string();
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
        std::cerr << "  WARNING: built extension library not found.\n";
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

// Generic 6-column response parser (status, num_vertices, num_communities,
// modularity, levels, result_file).
static int parseResultRow(const neug::QueryResult& rs,
                          const std::string& tag,
                          int64_t& num_vertices,
                          int64_t& num_communities,
                          double&  modularity,
                          int64_t& levels,
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
    num_communities = resp.arrays(2).int64_array().values(0);
    modularity      = resp.arrays(3).double_array().values(0);
    levels          = resp.arrays(4).int64_array().values(0);
    result_file     = resp.arrays(5).string_array().values(0);
    return 0;
}

// ---- coauthor CSV: global_id,country_code,community_id ----
static int loadCoauthorCSV(const std::string& path, int64_t expected_rows,
                           std::unordered_map<std::string, int>& out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open coauthor CSV: " << path << std::endl;
        return 1;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,country_code,community_id") {
        std::cerr << "Unexpected coauthor header: " << header << std::endl;
        return 1;
    }
    int64_t rows = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        ++rows;
        std::stringstream ss(line);
        std::string gid, cc, cid;
        std::getline(ss, gid, ',');
        std::getline(ss, cc, ',');
        std::getline(ss, cid, ',');
        if (cc.empty()) continue;
        out[cc] = std::stoi(cid);
    }
    if (expected_rows > 0 && rows != expected_rows) {
        std::cerr << "Coauthor row mismatch: csv=" << rows
                  << " reported=" << expected_rows << std::endl;
        return 1;
    }
    return 0;
}

// ---- datasource row ----
struct DsRow {
    std::string datasource_id;
    std::string provider_org_id;
    std::string provider_cc;
    int community_id = 0;
};

// ---- datasource CSV: global_id,"datasource_id",provider_org_id,provider_country_code,community_id ----
// datasource_id is double-quoted because it may legitimately contain commas.
// Other fields do not contain commas.
static int loadDatasourceCSV(const std::string& path, int64_t expected_rows,
                             std::vector<DsRow>& out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open ds CSV: " << path << std::endl;
        return 1;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,datasource_id,provider_org_id,provider_country_code,community_id") {
        std::cerr << "Unexpected ds header: " << header << std::endl;
        return 1;
    }
    int64_t rows = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        ++rows;
        // Expected layout: gid,"<ds_id>",org,cc,comm
        // Find the closing quote of the ds_id field, then comma-split the rest.
        auto open_q = line.find('"');
        auto close_q = (open_q == std::string::npos)
                       ? std::string::npos
                       : line.find('"', open_q + 1);
        DsRow r;
        if (open_q != std::string::npos && close_q != std::string::npos &&
            open_q < close_q) {
            r.datasource_id = line.substr(open_q + 1, close_q - open_q - 1);
            // tail starts after `,` following close_q
            auto tail_start = line.find(',', close_q);
            if (tail_start == std::string::npos) continue;
            std::stringstream ss(line.substr(tail_start + 1));
            std::string org, cc, cid;
            std::getline(ss, org, ',');
            std::getline(ss, cc, ',');
            std::getline(ss, cid, ',');
            r.provider_org_id = org;
            r.provider_cc = cc;
            r.community_id = cid.empty() ? 0 : std::stoi(cid);
        } else {
            // Fall back: assume no quoting, simple split
            std::stringstream ss(line);
            std::string gid, ds, org, cc, cid;
            std::getline(ss, gid, ',');
            std::getline(ss, ds, ',');
            std::getline(ss, org, ',');
            std::getline(ss, cc, ',');
            std::getline(ss, cid, ',');
            r.datasource_id = ds;
            r.provider_org_id = org;
            r.provider_cc = cc;
            r.community_id = cid.empty() ? 0 : std::stoi(cid);
        }
        out.push_back(std::move(r));
    }
    if (expected_rows > 0 && rows != expected_rows) {
        std::cerr << "DS row mismatch: csv=" << rows
                  << " reported=" << expected_rows << std::endl;
        return 1;
    }
    return 0;
}

struct WindowResult {
    std::string label;

    // coauthor
    int64_t co_num_vertices = 0;
    int64_t co_num_communities = 0;
    double  co_modularity = 0.0;
    int64_t co_levels = 0;
    std::string co_result_file;
    std::unordered_map<std::string, int> co_country_to_comm;

    // datasource
    int64_t ds_num_vertices = 0;
    int64_t ds_num_communities = 0;
    double  ds_modularity = 0.0;
    int64_t ds_levels = 0;
    std::string ds_result_file;
    std::vector<DsRow> ds_rows;
};

static int runWindow(neug::Connection* conn, int64_t ymin, int64_t ymax,
                     int64_t min_pubs, WindowResult& out) {
    std::ostringstream lbl;
    if (ymin == ymax) lbl << ymin;
    else lbl << ymin << "-" << ymax;
    out.label = lbl.str();
    std::cout << "\n=== Window " << out.label << " ===" << std::endl;

    // --- LOUVAIN_COAUTHOR ---
    {
        std::ostringstream qs;
        qs << "CALL LOUVAIN_COAUTHOR(" << ymin << ", " << ymax << ") RETURN *;";
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
                           out.co_result_file)) return 1;
        std::cout << "  [coauthor] verts=" << out.co_num_vertices
                  << " comm=" << out.co_num_communities
                  << " mod=" << out.co_modularity
                  << " csv=" << out.co_result_file << std::endl;
        if (loadCoauthorCSV(out.co_result_file, out.co_num_vertices,
                            out.co_country_to_comm)) return 1;
    }

    // --- LOUVAIN_DATASOURCE ---
    {
        std::ostringstream qs;
        qs << "CALL LOUVAIN_DATASOURCE(" << ymin << ", " << ymax << ", "
           << min_pubs << ", 20, 1.0, 10) RETURN *;";
        std::cout << "  " << qs.str() << std::endl;
        auto res = conn->Query(qs.str());
        if (!res.has_value()) {
            std::cerr << "  LOUVAIN_DATASOURCE failed: "
                      << res.error().ToString() << std::endl;
            return 1;
        }
        if (parseResultRow(res.value(), "LOUVAIN_DATASOURCE",
                           out.ds_num_vertices, out.ds_num_communities,
                           out.ds_modularity, out.ds_levels,
                           out.ds_result_file)) return 1;
        std::cout << "  [ds]       verts=" << out.ds_num_vertices
                  << " comm=" << out.ds_num_communities
                  << " mod=" << out.ds_modularity
                  << " csv=" << out.ds_result_file << std::endl;
        if (loadDatasourceCSV(out.ds_result_file, out.ds_num_vertices,
                              out.ds_rows)) return 1;
    }

    return 0;
}

// Per-window: write (ds_community, country) count distribution.
static void writeDsCountryDistribution(const WindowResult& w,
                                       const std::string& path) {
    // (community, cc) -> count
    std::map<std::pair<int, std::string>, int> cell;
    std::map<int, int> comm_size;
    for (const auto& r : w.ds_rows) {
        comm_size[r.community_id]++;
        if (r.provider_cc.empty()) continue;
        cell[{r.community_id, r.provider_cc}]++;
    }
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        std::cerr << "Cannot open: " << path << std::endl;
        return;
    }
    ofs << "ds_community,community_size,country_code,n_datasources\n";
    for (const auto& [k, n] : cell) {
        const auto& [cid, cc] = k;
        ofs << cid << "," << comm_size[cid] << "," << cc << "," << n << "\n";
    }
    ofs.close();
    std::cout << "  wrote " << path << " (" << cell.size() << " cells, "
              << comm_size.size() << " ds-communities)" << std::endl;
}

// Per-window: country, coauthor_comm, dominant_ds_comm,
// ds_count_in_dominant, ds_total_for_country.
// "Dominant" = ds-community where the country provides the most ds.
static void writeCountryCompare(const WindowResult& w,
                                const std::string& path) {
    // country -> ds_community -> count
    std::map<std::string, std::map<int, int>> by_country;
    for (const auto& r : w.ds_rows) {
        if (r.provider_cc.empty()) continue;
        by_country[r.provider_cc][r.community_id]++;
    }
    std::set<std::string> all_cc;
    for (const auto& kv : w.co_country_to_comm) all_cc.insert(kv.first);
    for (const auto& kv : by_country)            all_cc.insert(kv.first);

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        std::cerr << "Cannot open: " << path << std::endl;
        return;
    }
    ofs << "country_code,coauthor_comm,dominant_ds_comm,"
           "ds_count_in_dominant,ds_total_for_country\n";
    for (const auto& cc : all_cc) {
        ofs << cc << ",";
        auto cit = w.co_country_to_comm.find(cc);
        if (cit != w.co_country_to_comm.end()) ofs << cit->second;
        ofs << ",";
        auto dit = by_country.find(cc);
        if (dit == by_country.end() || dit->second.empty()) {
            ofs << ",,0\n";
            continue;
        }
        int dom_comm = -1, dom_count = -1, total = 0;
        for (const auto& [c, n] : dit->second) {
            total += n;
            if (n > dom_count) { dom_count = n; dom_comm = c; }
        }
        ofs << dom_comm << "," << dom_count << "," << total << "\n";
    }
    ofs.close();
    std::cout << "  wrote " << path << " (" << all_cc.size() << " countries)"
              << std::endl;
}

// Cross-window: ds_id -> per-window community.
// Using ds_id (string) as the row key keeps the table interpretable across
// windows; community ids are NOT stable across windows (Louvain renumbers).
static void writeDsPivot(const std::vector<WindowResult>& windows,
                         const std::string& path) {
    std::set<std::string> all_ds;
    for (const auto& w : windows)
        for (const auto& r : w.ds_rows) all_ds.insert(r.datasource_id);

    std::vector<std::unordered_map<std::string, int>> per_window_map(
        windows.size());
    for (size_t i = 0; i < windows.size(); ++i) {
        for (const auto& r : windows[i].ds_rows) {
            per_window_map[i][r.datasource_id] = r.community_id;
        }
    }

    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        std::cerr << "Cannot open: " << path << std::endl;
        return;
    }
    ofs << "datasource_id";
    for (const auto& w : windows) ofs << ",w_" << w.label;
    ofs << "\n";
    for (const auto& ds : all_ds) {
        // datasource_id may contain commas; quote it.
        ofs << "\"" << ds << "\"";
        for (size_t i = 0; i < windows.size(); ++i) {
            ofs << ",";
            auto it = per_window_map[i].find(ds);
            if (it != per_window_map[i].end()) ofs << it->second;
        }
        ofs << "\n";
    }
    ofs.close();
    std::cout << "  wrote " << path << " (" << all_ds.size()
              << " datasources × " << windows.size() << " windows)"
              << std::endl;
}

// Top-K country composition per ds-community for one window — printed to
// stdout. Reads the storyline directly: which countries are heavily
// represented in each ds-community.
static void printTopCountriesPerDsComm(const WindowResult& w, int top_k = 5) {
    std::map<int, std::map<std::string, int>> per_comm_cc;
    std::map<int, int> comm_size;
    for (const auto& r : w.ds_rows) {
        comm_size[r.community_id]++;
        if (r.provider_cc.empty()) continue;
        per_comm_cc[r.community_id][r.provider_cc]++;
    }
    std::cout << "\n  [country composition w=" << w.label
              << "] top-" << top_k << " provider countries per ds-community" << std::endl;
    std::vector<std::pair<int, int>> by_size(comm_size.begin(),
                                              comm_size.end());
    std::sort(by_size.begin(), by_size.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });
    for (size_t i = 0; i < std::min<size_t>(8, by_size.size()); ++i) {
        int cid = by_size[i].first;
        int sz  = by_size[i].second;
        std::vector<std::pair<std::string, int>> cc_list(
            per_comm_cc[cid].begin(), per_comm_cc[cid].end());
        std::sort(cc_list.begin(), cc_list.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });
        std::cout << "    comm=" << cid << " (size=" << sz << "): ";
        for (size_t k = 0; k < std::min<size_t>(top_k, cc_list.size()); ++k) {
            std::cout << cc_list[k].first << "=" << cc_list[k].second;
            if (k + 1 < std::min<size_t>(top_k, cc_list.size())) std::cout << ", ";
        }
        std::cout << std::endl;
    }
}

int main(int argc, char* argv[]) {
    int64_t min_pubs = 100;
    if (argc > 1) min_pubs = std::stoll(argv[1]);

    std::cout << "============================================" << std::endl;
    std::cout << "  NeuG Louvain — Datasource case (OpenAire)" << std::endl;
    std::cout << "  min_pubs_per_datasource = " << min_pubs << std::endl;
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

    const std::vector<std::pair<int64_t, int64_t>> kWindows = {
        {2018, 2019},
        {2020, 2021},
        {2022, 2023},
        {2024, 2024},
    };

    std::vector<WindowResult> windows;
    windows.reserve(kWindows.size());
    int failures = 0;
    for (const auto& [ymin, ymax] : kWindows) {
        WindowResult w;
        failures += runWindow(conn.get(), ymin, ymax, min_pubs, w);
        windows.push_back(std::move(w));
    }

    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string out_dir = "/tmp/p/neug_louvain";
    std::filesystem::create_directories(out_dir);

    std::cout << "\n=== Per-window outputs ===" << std::endl;
    for (const auto& w : windows) {
        const std::string dist_path = out_dir + "/ds_country_dist_" +
            w.label + "_" + std::to_string(ts) + ".csv";
        const std::string cmp_path  = out_dir + "/country_compare_" +
            w.label + "_" + std::to_string(ts) + ".csv";
        writeDsCountryDistribution(w, dist_path);
        writeCountryCompare(w, cmp_path);
        printTopCountriesPerDsComm(w);
    }

    const std::string pivot_path = out_dir + "/ds_pivot_" +
        std::to_string(ts) + ".csv";
    std::cout << "\n=== Cross-window pivot ===" << std::endl;
    writeDsPivot(windows, pivot_path);

    std::cout << "\n=== Modularity / community-count trajectory ===" << std::endl;
    for (const auto& w : windows) {
        std::cout << "    " << w.label
                  << "  coauthor: mod=" << w.co_modularity
                  << " comm=" << w.co_num_communities
                  << " verts=" << w.co_num_vertices
                  << "  ||  ds: mod=" << w.ds_modularity
                  << " comm=" << w.ds_num_communities
                  << " verts=" << w.ds_num_vertices
                  << std::endl;
    }

    std::cout << "\n============================================" << std::endl;
    if (failures == 0) std::cout << "  Datasource case PASSED!" << std::endl;
    else std::cout << "  Datasource case FAILED (" << failures << " errors)"
                   << std::endl;
    std::cout << "============================================" << std::endl;

    conn.reset();
    db.Close();
    return failures;
}
