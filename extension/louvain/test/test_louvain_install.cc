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

#include <neug/main/neug_db.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <unistd.h>

// NeuG data directory (contains graph.yaml, checkpoint/, etc.).
static constexpr const char* kOpenAireDbPath =
    "/mnt/lyk/openaire_18_26_neug/neug_db_1775109652105";

static constexpr int64_t kLouvainMaxIterations = 20;
static constexpr double  kLouvainResolution    = 1.0;
static constexpr int64_t kLouvainMaxLevels     = 10;

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

bool verifyExtensionLoaded(neug::Connection* conn) {
    auto show_res = conn->Query("CALL show_loaded_extensions() RETURN *;");
    if (!show_res.has_value()) {
        std::cerr << "Failed to query loaded extensions" << std::endl;
        return false;
    }
    std::cout << "\nLoaded extensions:" << std::endl;
    auto& resp = show_res.value().response();
    if (resp.arrays_size() < 2) {
        std::cerr << "Unexpected response format" << std::endl;
        return false;
    }

    const auto& name_arr = resp.arrays(0).string_array();
    const auto& desc_arr = resp.arrays(1).string_array();
    bool found = false;
    for (int i = 0; i < name_arr.values_size(); i++) {
        std::cout << "  - " << name_arr.values(i) << ": "
                  << desc_arr.values(i) << std::endl;
        if (name_arr.values(i) == "louvain") found = true;
    }
    if (!found) {
        std::cerr << "ERROR: louvain extension not found in loaded extensions!"
                  << std::endl;
    }
    return found;
}

bool validateResultCSV(const std::string& filepath,
                       int64_t expected_vertices,
                       int64_t expected_communities) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open result CSV: " << filepath << std::endl;
        return false;
    }

    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,vertex_label,original_vid,community_id") {
        std::cerr << "Unexpected CSV header: " << header << std::endl;
        return false;
    }

    int64_t row_count = 0;
    std::unordered_set<int64_t> communities;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        row_count++;

        std::stringstream ss(line);
        std::string gid_str, label_str, vid_str, comm_str;
        std::getline(ss, gid_str, ',');
        std::getline(ss, label_str, ',');
        std::getline(ss, vid_str, ',');
        std::getline(ss, comm_str, ',');

        communities.insert(std::stoll(comm_str));
    }

    std::cout << "  CSV validation: " << row_count << " rows, "
              << communities.size() << " distinct communities" << std::endl;

    if (expected_vertices > 0 && row_count != expected_vertices) {
        std::cerr << "  Expected " << expected_vertices << " rows, got "
                  << row_count << std::endl;
        return false;
    }
    if (expected_communities > 0 &&
        static_cast<int64_t>(communities.size()) != expected_communities) {
        std::cerr << "  Expected " << expected_communities
                  << " communities, got " << communities.size() << std::endl;
        return false;
    }
    return row_count > 0 && !communities.empty();
}

// Louvain output columns:
//   0: status          (STRING)
//   1: num_vertices    (INT64)
//   2: num_communities (INT64)
//   3: modularity      (DOUBLE)
//   4: levels          (INT64)
//   5: result_file     (STRING)

// Validate a LOUVAIN_COAUTHOR result CSV.
// Expected header: "global_id,country_code,community_id".
// Every row must have a non-empty country_code and a valid community_id.
bool validateCoauthorCSV(const std::string& filepath,
                         int64_t expected_vertices,
                         int64_t expected_communities) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open coauthor CSV: " << filepath << std::endl;
        return false;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,country_code,community_id") {
        std::cerr << "Unexpected CSV header: " << header << std::endl;
        return false;
    }
    int64_t row_count = 0;
    std::unordered_set<int64_t> communities;
    std::unordered_set<std::string> countries;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        row_count++;
        std::stringstream ss(line);
        std::string gid_str, cc_str, comm_str;
        std::getline(ss, gid_str, ',');
        std::getline(ss, cc_str, ',');
        std::getline(ss, comm_str, ',');
        if (cc_str.empty()) {
            std::cerr << "Empty country_code on row " << row_count << std::endl;
            return false;
        }
        communities.insert(std::stoll(comm_str));
        countries.insert(cc_str);
    }
    std::cout << "  Coauthor CSV: " << row_count << " rows, "
              << countries.size() << " distinct countries, "
              << communities.size() << " distinct communities" << std::endl;
    if (expected_vertices > 0 && row_count != expected_vertices) {
        std::cerr << "  Expected " << expected_vertices << " rows, got "
                  << row_count << std::endl;
        return false;
    }
    if (expected_communities > 0 &&
        static_cast<int64_t>(communities.size()) != expected_communities) {
        std::cerr << "  Expected " << expected_communities
                  << " communities, got " << communities.size() << std::endl;
        return false;
    }
    // Every vertex should correspond to a distinct country in projection mode.
    if (countries.size() != static_cast<size_t>(row_count)) {
        std::cerr << "  Duplicate country_code in rows (expected distinct)"
                  << std::endl;
        return false;
    }
    return row_count > 0;
}

struct WindowResult {
    std::string label;                 // e.g. "2018-2019" or "2024"
    int64_t num_vertices = 0;
    int64_t num_communities = 0;
    double  modularity = 0.0;
    int64_t levels = 0;
    std::string result_file;
    // country_code -> community_id for this window
    std::unordered_map<std::string, int> country_to_comm;
};

// Run one CALL LOUVAIN_COAUTHOR(year_min, year_max), validate basic invariants,
// and parse its result CSV into out.country_to_comm.
// Returns 0 on success, 1 on failure.
int runCoauthorWindow(neug::Connection* conn,
                      int64_t year_min, int64_t year_max,
                      WindowResult& out) {
    std::ostringstream label_ss;
    if (year_min == year_max) label_ss << year_min;
    else label_ss << year_min << "-" << year_max;
    out.label = label_ss.str();

    std::cout << "\n=== CALL LOUVAIN_COAUTHOR(" << year_min << ", "
              << year_max << ")  [window=" << out.label << "] ===" << std::endl;

    std::ostringstream qs;
    qs << "CALL LOUVAIN_COAUTHOR(" << year_min << ", " << year_max
       << ") RETURN *;";
    auto res = conn->Query(qs.str());
    if (!res.has_value()) {
        std::cerr << "LOUVAIN_COAUTHOR failed: "
                  << res.error().ToString() << std::endl;
        return 1;
    }
    auto& resp = res.value().response();
    if (resp.row_count() != 1) {
        std::cerr << "Expected 1 row, got " << resp.row_count() << std::endl;
        return 1;
    }
    std::string status = resp.arrays(0).string_array().values(0);
    if (status != "success") {
        std::cerr << "status=" << status << std::endl;
        return 1;
    }
    out.num_vertices    = resp.arrays(1).int64_array().values(0);
    out.num_communities = resp.arrays(2).int64_array().values(0);
    out.modularity      = resp.arrays(3).double_array().values(0);
    out.levels          = resp.arrays(4).int64_array().values(0);
    out.result_file     = resp.arrays(5).string_array().values(0);

    std::cout << "  vertices=" << out.num_vertices
              << " communities=" << out.num_communities
              << " modularity=" << out.modularity
              << " levels=" << out.levels << std::endl;
    std::cout << "  Result file: " << out.result_file << std::endl;

    if (out.num_vertices < 5) {
        std::cerr << "  Suspiciously few countries; window may be empty"
                  << std::endl;
        return 1;
    }
    if (out.num_communities < 1 || out.num_communities > out.num_vertices) {
        std::cerr << "  Invalid community count" << std::endl;
        return 1;
    }
    if (out.modularity < -0.5 || out.modularity > 1.0) {
        std::cerr << "  Modularity out of range" << std::endl;
        return 1;
    }

    // Parse CSV into country_to_comm (doubles as content validation).
    std::ifstream ifs(out.result_file);
    if (!ifs.is_open()) {
        std::cerr << "  Cannot open result CSV" << std::endl;
        return 1;
    }
    std::string header;
    std::getline(ifs, header);
    if (header != "global_id,country_code,community_id") {
        std::cerr << "  Unexpected header: " << header << std::endl;
        return 1;
    }
    std::string line;
    int64_t row_count = 0;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        row_count++;
        std::stringstream ss(line);
        std::string gid_str, cc_str, comm_str;
        std::getline(ss, gid_str, ',');
        std::getline(ss, cc_str, ',');
        std::getline(ss, comm_str, ',');
        if (cc_str.empty()) {
            std::cerr << "  Empty country_code on row " << row_count
                      << std::endl;
            return 1;
        }
        out.country_to_comm[cc_str] = std::stoi(comm_str);
    }
    if (row_count != out.num_vertices) {
        std::cerr << "  Row count mismatch: CSV=" << row_count
                  << " reported=" << out.num_vertices << std::endl;
        return 1;
    }
    std::cout << "  Loaded " << out.country_to_comm.size()
              << " countries into pivot" << std::endl;
    std::cout << "  PASSED" << std::endl;
    return 0;
}

// Emit a pivot CSV with header `country_code,w_<label1>,w_<label2>,...`
// where each cell holds the community_id for that country in that window
// (blank if the country did not appear in the window).
// Community ids are NOT comparable across windows (Louvain renumbers each run),
// so this table is useful for spotting structural changes, not absolute labels.
void writePivotCSV(const std::vector<WindowResult>& windows,
                   const std::string& out_path) {
    std::set<std::string> all_countries;
    for (const auto& w : windows) {
        for (const auto& kv : w.country_to_comm) {
            all_countries.insert(kv.first);
        }
    }

    std::ofstream ofs(out_path);
    if (!ofs.is_open()) {
        std::cerr << "Cannot open pivot output: " << out_path << std::endl;
        return;
    }
    ofs << "country_code";
    for (const auto& w : windows) ofs << ",w_" << w.label;
    ofs << "\n";
    for (const auto& cc : all_countries) {
        ofs << cc;
        for (const auto& w : windows) {
            auto it = w.country_to_comm.find(cc);
            ofs << ",";
            if (it != w.country_to_comm.end()) ofs << it->second;
        }
        ofs << "\n";
    }
    ofs.close();

    std::cout << "\nPivot written to: " << out_path << std::endl;
    std::cout << "  " << all_countries.size() << " countries × "
              << windows.size() << " windows" << std::endl;
    std::cout << "\n  Modularity trajectory:" << std::endl;
    for (const auto& w : windows) {
        std::cout << "    " << w.label
                  << ": modularity=" << w.modularity
                  << " communities=" << w.num_communities
                  << " vertices=" << w.num_vertices << std::endl;
    }
}

int testLouvain(neug::Connection* conn,
                int64_t max_iterations,
                double resolution,
                int64_t max_levels) {
    std::cout << "\n=== CALL LOUVAIN(" << max_iterations << ", "
              << resolution << ", " << max_levels << ") ===" << std::endl;

    std::ostringstream qs;
    qs << "CALL LOUVAIN(" << max_iterations << ", " << resolution << ", "
       << max_levels << ") RETURN *;";
    std::string query = qs.str();
    std::cout << "Query: " << query << std::endl;

    auto res = conn->Query(query);
    if (!res.has_value()) {
        std::cerr << "LOUVAIN failed: " << res.error().ToString() << std::endl;
        return 1;
    }

    auto& result_rs = res.value();
    std::cout << result_rs.ToString() << std::endl;

    auto& resp = result_rs.response();
    if (resp.row_count() != 1) {
        std::cerr << "Expected exactly 1 result row, got: "
                  << resp.row_count() << std::endl;
        return 1;
    }

    std::string status = resp.arrays(0).string_array().values(0);
    if (status != "success") {
        std::cerr << "Expected status=success, got: " << status << std::endl;
        return 1;
    }

    int64_t num_vertices    = resp.arrays(1).int64_array().values(0);
    int64_t num_communities = resp.arrays(2).int64_array().values(0);
    double  modularity      = resp.arrays(3).double_array().values(0);
    int64_t levels          = resp.arrays(4).int64_array().values(0);
    std::string result_file = resp.arrays(5).string_array().values(0);

    std::cout << "  Graph vertices:  " << num_vertices << std::endl;
    std::cout << "  Communities:     " << num_communities << std::endl;
    std::cout << "  Modularity:      " << modularity << std::endl;
    std::cout << "  Levels:          " << levels << std::endl;
    std::cout << "  Result file:     " << result_file << std::endl;

    if (num_vertices < 1) {
        std::cerr << "Expected at least 1 vertex, got: " << num_vertices
                  << std::endl;
        return 1;
    }
    if (num_communities < 1) {
        std::cerr << "Expected at least 1 community, got: "
                  << num_communities << std::endl;
        return 1;
    }
    if (levels < 1 || levels > max_levels) {
        std::cerr << "Invalid levels: " << levels
                  << " (expected 1.." << max_levels << ")" << std::endl;
        return 1;
    }

    if (!validateResultCSV(result_file, num_vertices, num_communities)) {
        return 1;
    }

    std::cout << "  PASSED" << std::endl;
    return 0;
}

int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "  NeuG Louvain — OpenAire DB" << std::endl;
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
    if (!db.Open(db_path, /*max_num_threads=*/0,
                 neug::DBMode::READ_WRITE)) {
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

    if (!verifyExtensionLoaded(conn.get())) {
        conn.reset();
        db.Close();
        return 1;
    }

    int failures = testLouvain(conn.get(),
                               kLouvainMaxIterations,
                               kLouvainResolution,
                               kLouvainMaxLevels);

    // Case A 4-window sweep: pre-war deep baseline, COVID-era baseline,
    // war-onset acute window, war-steady-state. The pivot CSV emitted at
    // the end lets us diff country → community membership across windows
    // to spot RU/UA/BY migrations, CN/US drift, and IR trajectory.
    const std::vector<std::pair<int64_t, int64_t>> kWindows = {
        {2018, 2019},
        {2020, 2021},
        {2022, 2023},
        {2024, 2024},
    };
    std::vector<WindowResult> window_results;
    window_results.reserve(kWindows.size());
    for (const auto& [ymin, ymax] : kWindows) {
        WindowResult wr;
        failures += runCoauthorWindow(conn.get(), ymin, ymax, wr);
        window_results.push_back(std::move(wr));
    }

    const auto pivot_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string pivot_path =
        "/tmp/p/neug_louvain/pivot_country_community_" +
        std::to_string(pivot_ts) + ".csv";
    writePivotCSV(window_results, pivot_path);

    std::cout << "\n============================================" << std::endl;
    if (failures == 0) {
        std::cout << "  Louvain test PASSED!" << std::endl;
    } else {
        std::cout << "  Louvain test FAILED!" << std::endl;
    }
    std::cout << "============================================" << std::endl;

    conn.reset();
    db.Close();

    return failures;
}
