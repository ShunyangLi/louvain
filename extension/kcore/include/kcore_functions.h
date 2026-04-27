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

#pragma once

#ifdef _OPENMP
#include <omp.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "neug/compiler/common/types/types.h"
#include "neug/compiler/function/function.h"
#include "neug/compiler/function/neug_call_function.h"
#include "neug/execution/common/context.h"
#include "neug/execution/common/columns/value_columns.h"
#include "neug/storages/graph/graph_interface.h"
#include "neug/storages/graph/property_graph.h"

namespace neug {
namespace function {

// ============================================================================
// k-core decomposition (Batagelj-Zaversnik O(V+E) peeling)
// ============================================================================

class KcoreComputer {
public:
    // Mode 1: plain graph (all labels, all edges).
    explicit KcoreComputer(const StorageReadInterface& graph)
        : graph_(graph) {}

    // Mode 2: country-level coauthor projection (parallels LOUVAIN_COAUTHOR).
    //   Publication —[hasAuthorInstitution]→ Organization
    //   → fold to country pairs, threshold by min_edge_weight, run k-core.
    struct CoauthorConfig {
        int64_t year_min = 0;
        int64_t year_max = 0;       // year_max < year_min means no year filter
        int max_coauthors = 30;     // skip pubs with > this many distinct author orgs
        int min_edge_weight = 1;    // min # shared cross-country pubs to count as edge
    };
    KcoreComputer(const StorageReadInterface& graph, CoauthorConfig cfg)
        : graph_(graph), coauthor_mode_(true), coauthor_cfg_(cfg) {}

    // Mode 3: organization-level project-participation projection (Brexit case).
    //   Organization —[isParticipant]→ Project
    //   → fold to org pairs that share an in-window Project, run k-core.
    //   Filter by Project.startDate year (parsed from "YYYY-MM-DD...").
    struct ProjectConfig {
        int64_t year_min = 0;
        int64_t year_max = 0;
        int min_org_projects = 1;   // drop orgs with < this many in-window projects
        int min_edge_weight = 1;    // min # shared in-window projects to count as edge
        int max_project_size = 200; // skip pseudo-projects with too many partners
    };
    KcoreComputer(const StorageReadInterface& graph, ProjectConfig cfg)
        : graph_(graph), project_mode_(true), project_cfg_(cfg) {}

    struct Result {
        int num_vertices = 0;
        int num_edges = 0;
        int max_coreness = 0;
        int num_in_max_core = 0;
        std::vector<int> coreness;                          // per global_id
        std::vector<std::pair<label_t, vid_t>> id_mapping;  // per global_id
        // Mode 2: country_code per super-vertex.
        // Mode 3: organization id per super-vertex.
        std::vector<std::string> aggregate_keys;
        // Mode 3 only: country_code of provider org (for Brexit-style country slicing).
        std::vector<std::string> aggregate_extra;
    };

    Result Compute() {
        auto t0 = std::chrono::steady_clock::now();
        Result result;
        if (coauthor_mode_) BuildCountryCoauthorProjection();
        else if (project_mode_) BuildOrgProjectProjection();
        else BuildGraph();

        result.num_vertices = num_vertex_;
        result.num_edges = num_edge_ / 2;  // undirected count
        result.id_mapping = global_to_local_;
        result.aggregate_keys = aggregate_keys_;
        result.aggregate_extra = aggregate_extra_;

        if (num_vertex_ == 0) {
            LOG(INFO) << "[Kcore] empty graph, nothing to do";
            return result;
        }

        result.coreness.assign(num_vertex_, 0);
        RunKCore(result.coreness);

        int mc = 0, n_in_max = 0;
        for (int v = 0; v < num_vertex_; ++v) {
            if (result.coreness[v] > mc) mc = result.coreness[v];
        }
        for (int v = 0; v < num_vertex_; ++v) {
            if (result.coreness[v] == mc) ++n_in_max;
        }
        result.max_coreness = mc;
        result.num_in_max_core = n_in_max;

        auto t1 = std::chrono::steady_clock::now();
        LOG(INFO) << "[Kcore] Done: V=" << num_vertex_
                  << " E=" << result.num_edges
                  << " max_core=" << mc << " #in_max=" << n_in_max
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                  << "ms";
        return result;
    }

private:
    // ---- Batagelj-Zaversnik O(V+E) k-core peeling ----
    void RunKCore(std::vector<int>& coreness) {
        const int n = num_vertex_;
        // adj_ may contain duplicate (v, w) entries from multi-edges; build a
        // clean dedup'd neighbor list. K-core is defined on simple graphs.
        std::vector<std::vector<int>> nbr(n);
        for (int u = 0; u < n; ++u) {
            std::unordered_set<int> seen;
            seen.reserve(adj_[u].size());
            nbr[u].reserve(adj_[u].size());
            for (auto& [v, w] : adj_[u]) {
                (void)w;
                if (v == u) continue;
                if (seen.insert(v).second) nbr[u].push_back(v);
            }
        }
        std::vector<int> deg(n);
        int md = 0;
        for (int v = 0; v < n; ++v) {
            deg[v] = static_cast<int>(nbr[v].size());
            if (deg[v] > md) md = deg[v];
        }
        // Bucket sort: bin[d] -> first slot in vert[] of vertices with degree d
        std::vector<int> bin(md + 2, 0);
        for (int v = 0; v < n; ++v) bin[deg[v]]++;
        int start = 0;
        for (int d = 0; d <= md; ++d) {
            int num = bin[d];
            bin[d] = start;
            start += num;
        }
        std::vector<int> vert(n);
        std::vector<int> pos(n);
        for (int v = 0; v < n; ++v) {
            pos[v] = bin[deg[v]];
            vert[pos[v]] = v;
            bin[deg[v]]++;
        }
        // Restore bin to start of each degree class.
        for (int d = md; d >= 1; --d) bin[d] = bin[d - 1];
        bin[0] = 0;

        // Peel: iterate vertices in ascending degree order.
        for (int i = 0; i < n; ++i) {
            int v = vert[i];
            coreness[v] = deg[v];
            for (int u : nbr[v]) {
                if (deg[u] > deg[v]) {
                    int du = deg[u];
                    int pu = pos[u];
                    int pw = bin[du];
                    int w = vert[pw];
                    if (u != w) {
                        pos[u] = pw;
                        vert[pw] = u;
                        pos[w] = pu;
                        vert[pu] = w;
                    }
                    bin[du]++;
                    deg[u]--;
                }
            }
        }
    }

    // ---- Mode 1: full graph (all labels, all edges) ----
    // Mirrors LouvainComputer::BuildGraph().
    void BuildGraph() {
        auto tstart = std::chrono::steady_clock::now();
        const auto& schema = graph_.schema();
        label_t num_vertex_labels = schema.vertex_label_num();

        label_offsets_.assign(num_vertex_labels, -1);
        std::vector<size_t> label_sizes(num_vertex_labels, 0);
        int offset = 0;
        for (label_t L = 0; L < num_vertex_labels; ++L) {
            if (!schema.vertex_label_valid(L)) continue;
            VertexSet vs = graph_.GetVertexSet(L);
            label_offsets_[L] = offset;
            label_sizes[L] = vs.size();
            offset += static_cast<int>(vs.size());
        }
        num_vertex_ = offset;
        if (num_vertex_ == 0) return;

        global_to_local_.assign(num_vertex_, {});
        for (label_t L = 0; L < num_vertex_labels; ++L) {
            int base = label_offsets_[L];
            if (base < 0) continue;
            int sz = static_cast<int>(label_sizes[L]);
            for (int v = 0; v < sz; ++v) {
                global_to_local_[base + v] = {L, static_cast<vid_t>(v)};
            }
        }

        adj_.assign(num_vertex_, {});
        int64_t total_directed = 0;
        for (const auto& [key, edge_schema] : schema.get_all_edge_schemas()) {
            auto [src_label, dst_label, e_label] = schema.parse_edge_label(key);
            int src_base = label_offsets_[src_label];
            int dst_base = label_offsets_[dst_label];
            if (src_base < 0 || dst_base < 0) continue;
            GenericView out_view = graph_.GetGenericOutgoingGraphView(
                src_label, dst_label, e_label);
            int src_sz = static_cast<int>(label_sizes[src_label]);
            size_t dst_sz = label_sizes[dst_label];
            for (int i = 0; i < src_sz; ++i) {
                vid_t src_vid = static_cast<vid_t>(i);
                if (!graph_.IsValidVertex(src_label, src_vid)) continue;
                int src_g = src_base + i;
                NbrList edges = out_view.get_edges(src_vid);
                for (auto it = edges.begin(); it != edges.end(); ++it) {
                    vid_t dst_vid = *it;
                    if (static_cast<size_t>(dst_vid) >= dst_sz) continue;
                    int dst_g = dst_base + static_cast<int>(dst_vid);
                    adj_[src_g].push_back({dst_g, 1.0});
                    adj_[dst_g].push_back({src_g, 1.0});
                    total_directed += 2;
                }
            }
        }
        num_edge_ = static_cast<int>(total_directed);

        auto tend = std::chrono::steady_clock::now();
        LOG(INFO) << "[Kcore-plain] Built full graph: V=" << num_vertex_
                  << " directed_edges=" << total_directed
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tend - tstart).count()
                  << "ms";
    }

    // ---- Mode 2: country coauthor projection ----
    void BuildCountryCoauthorProjection() {
        auto tstart = std::chrono::steady_clock::now();
        const auto& schema = graph_.schema();

        label_t pub_label, org_label;
        try {
            pub_label = schema.get_vertex_label_id("Publication");
            org_label = schema.get_vertex_label_id("Organization");
        } catch (...) {
            LOG(ERROR) << "[Kcore-coauthor] Publication/Organization label missing";
            return;
        }
        label_t pub_org_edge_label = static_cast<label_t>(-1);
        for (const auto& [key, _] : schema.get_all_edge_schemas()) {
            auto [src, dst, e] = schema.parse_edge_label(key);
            if (src == pub_label && dst == org_label) {
                pub_org_edge_label = e; break;
            }
        }
        if (pub_org_edge_label == static_cast<label_t>(-1)) {
            LOG(ERROR) << "[Kcore-coauthor] No edge Publication→Organization";
            return;
        }

        auto year_col_base = graph_.GetVertexPropColumn(pub_label, "year");
        auto year_col = std::dynamic_pointer_cast<TypedRefColumn<int64_t>>(year_col_base);
        if (!year_col) { LOG(ERROR) << "[Kcore-coauthor] Publication.year not INT64"; return; }
        auto cc_col_base = graph_.GetVertexPropColumn(org_label, "country_code");
        auto cc_col = std::dynamic_pointer_cast<TypedRefColumn<std::string_view>>(cc_col_base);
        if (!cc_col) { LOG(ERROR) << "[Kcore-coauthor] Organization.country_code not STRING"; return; }

        VertexSet pub_set = graph_.GetVertexSet(pub_label);
        VertexSet org_set = graph_.GetVertexSet(org_label);
        const size_t num_pubs = pub_set.size();
        const size_t num_orgs = org_set.size();

        std::unordered_map<std::string, int> cc_to_idx;
        std::vector<int> org_to_cidx(num_orgs, -1);
        std::vector<std::string> cc_list;
        for (size_t i = 0; i < num_orgs; ++i) {
            vid_t org_vid = static_cast<vid_t>(i);
            if (!graph_.IsValidVertex(org_label, org_vid)) continue;
            std::string_view cc = cc_col->get_view(i);
            if (cc.empty()) continue;
            std::string cc_str(cc);
            auto [it, inserted] = cc_to_idx.emplace(cc_str, static_cast<int>(cc_to_idx.size()));
            if (inserted) cc_list.push_back(cc_str);
            org_to_cidx[i] = it->second;
        }
        const int num_countries = static_cast<int>(cc_to_idx.size());
        if (num_countries == 0) { num_vertex_ = 0; return; }

        const bool year_filter = coauthor_cfg_.year_max >= coauthor_cfg_.year_min;
        const int max_co = coauthor_cfg_.max_coauthors;
        GenericView pub_org_view = graph_.GetGenericOutgoingGraphView(
            pub_label, org_label, pub_org_edge_label);

        std::vector<std::unordered_map<int, double>> agg(num_countries);
        int64_t pubs_accepted = 0;
        for (size_t i = 0; i < num_pubs; ++i) {
            vid_t pub_vid = static_cast<vid_t>(i);
            if (!graph_.IsValidVertex(pub_label, pub_vid)) continue;
            if (year_filter) {
                int64_t y = year_col->get_view(i);
                if (y < coauthor_cfg_.year_min || y > coauthor_cfg_.year_max) continue;
            }
            NbrList edges = pub_org_view.get_edges(pub_vid);
            std::unordered_set<int> distinct_orgs;
            std::vector<int> cvec;
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                vid_t org_vid = *it;
                if (static_cast<size_t>(org_vid) >= num_orgs) continue;
                if (!distinct_orgs.insert(static_cast<int>(org_vid)).second) continue;
                int cidx = org_to_cidx[org_vid];
                if (cidx < 0) continue;
                cvec.push_back(cidx);
            }
            if (max_co > 0 && static_cast<int>(distinct_orgs.size()) > max_co) continue;
            if (cvec.empty()) continue;
            std::sort(cvec.begin(), cvec.end());
            cvec.erase(std::unique(cvec.begin(), cvec.end()), cvec.end());
            if (cvec.size() < 2) continue;
            for (size_t x = 0; x < cvec.size(); ++x)
                for (size_t y = x + 1; y < cvec.size(); ++y)
                    agg[cvec[x]][cvec[y]] += 1.0;
            ++pubs_accepted;
        }

        const double thr = static_cast<double>(coauthor_cfg_.min_edge_weight);
        num_vertex_ = num_countries;
        adj_.assign(num_vertex_, {});
        int64_t total_entries = 0;
        for (int a = 0; a < num_countries; ++a) {
            for (auto& [b, w] : agg[a]) {
                if (w < thr) continue;
                adj_[a].push_back({b, w});
                adj_[b].push_back({a, w});
                total_entries += 2;
            }
        }
        num_edge_ = static_cast<int>(total_entries);

        global_to_local_.assign(num_vertex_, {});
        aggregate_keys_.resize(num_vertex_);
        for (int i = 0; i < num_vertex_; ++i) {
            global_to_local_[i] = {static_cast<label_t>(-1), static_cast<vid_t>(i)};
            aggregate_keys_[i] = cc_list[i];
        }

        auto tend = std::chrono::steady_clock::now();
        LOG(INFO) << "[Kcore-coauthor] year=[" << coauthor_cfg_.year_min
                  << "," << coauthor_cfg_.year_max
                  << "] pubs_accepted=" << pubs_accepted
                  << " V=" << num_vertex_ << " 2E=" << total_entries
                  << " min_w=" << thr
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tend - tstart).count()
                  << "ms";
    }

    // ---- Mode 3: organization × organization via shared Project participation ----
    // Project.startDate is STRING in the form "YYYY-MM-DD..."; parse the leading 4 chars.
    static int parse_year_prefix(std::string_view sv) {
        if (sv.size() < 4) return -1;
        int y = 0;
        for (int i = 0; i < 4; ++i) {
            char c = sv[i];
            if (c < '0' || c > '9') return -1;
            y = y * 10 + (c - '0');
        }
        return y;
    }

    void BuildOrgProjectProjection() {
        auto tstart = std::chrono::steady_clock::now();
        const auto& schema = graph_.schema();

        label_t org_label, proj_label;
        try {
            org_label = schema.get_vertex_label_id("Organization");
            proj_label = schema.get_vertex_label_id("Project");
        } catch (...) {
            LOG(ERROR) << "[Kcore-project] Organization/Project label missing";
            return;
        }
        label_t org_proj_edge_label = static_cast<label_t>(-1);
        for (const auto& [key, _] : schema.get_all_edge_schemas()) {
            auto [src, dst, e] = schema.parse_edge_label(key);
            if (src == org_label && dst == proj_label) {
                org_proj_edge_label = e; break;
            }
        }
        if (org_proj_edge_label == static_cast<label_t>(-1)) {
            LOG(ERROR) << "[Kcore-project] No edge Organization→Project";
            return;
        }
        LOG(INFO) << "[Kcore-project] org_label=" << (int)org_label
                  << " proj_label=" << (int)proj_label
                  << " org_proj_edge_label=" << (int)org_proj_edge_label;

        auto sd_col_base = graph_.GetVertexPropColumn(proj_label, "startDate");
        auto sd_col = std::dynamic_pointer_cast<TypedRefColumn<std::string_view>>(sd_col_base);
        if (!sd_col) { LOG(ERROR) << "[Kcore-project] Project.startDate not STRING"; return; }
        auto cc_col_base = graph_.GetVertexPropColumn(org_label, "country_code");
        auto cc_col = std::dynamic_pointer_cast<TypedRefColumn<std::string_view>>(cc_col_base);
        auto org_id_col_base = graph_.GetVertexPropColumn(org_label, "id");
        auto org_id_col = std::dynamic_pointer_cast<TypedRefColumn<std::string_view>>(org_id_col_base);

        VertexSet org_set = graph_.GetVertexSet(org_label);
        VertexSet proj_set = graph_.GetVertexSet(proj_label);
        const size_t num_orgs = org_set.size();
        const size_t num_projs = proj_set.size();

        // Pass 1: mark in-window projects.
        const int64_t ymin = project_cfg_.year_min;
        const int64_t ymax = project_cfg_.year_max;
        const bool year_filter = ymax >= ymin;
        std::vector<uint8_t> proj_in_window(num_projs, 0);
        int64_t in_win = 0;
        int64_t proj_no_year = 0;
        for (size_t p = 0; p < num_projs; ++p) {
            vid_t pv = static_cast<vid_t>(p);
            if (!graph_.IsValidVertex(proj_label, pv)) continue;
            if (year_filter) {
                std::string_view sv = sd_col->get_view(p);
                int y = parse_year_prefix(sv);
                if (y < 0) { ++proj_no_year; continue; }
                if (y < ymin || y > ymax) continue;
            }
            proj_in_window[p] = 1; ++in_win;
        }

        // Pass 2: scan Org → Project edges, build inverted index proj → orgs.
        GenericView op_view = graph_.GetGenericOutgoingGraphView(
            org_label, proj_label, org_proj_edge_label);
        std::vector<std::vector<int>> proj_orgs(num_projs);
        std::vector<int> org_proj_count(num_orgs, 0);
        int64_t edges_seen = 0;
        for (size_t i = 0; i < num_orgs; ++i) {
            vid_t ov = static_cast<vid_t>(i);
            if (!graph_.IsValidVertex(org_label, ov)) continue;
            NbrList edges = op_view.get_edges(ov);
            std::unordered_set<int> seen;
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                vid_t pv = *it;
                if (static_cast<size_t>(pv) >= num_projs) continue;
                if (!proj_in_window[pv]) continue;
                if (!seen.insert(static_cast<int>(pv)).second) continue;
                proj_orgs[pv].push_back(static_cast<int>(i));
                ++org_proj_count[i];
                ++edges_seen;
            }
        }

        // Filter orgs to keep.
        const int min_proj = project_cfg_.min_org_projects;
        std::vector<int> org_keep;
        std::vector<int> org_to_super(num_orgs, -1);
        for (size_t i = 0; i < num_orgs; ++i) {
            if (org_proj_count[i] >= min_proj && org_proj_count[i] > 0) {
                org_to_super[i] = static_cast<int>(org_keep.size());
                org_keep.push_back(static_cast<int>(i));
            }
        }
        const int num_kept = static_cast<int>(org_keep.size());
        LOG(INFO) << "[Kcore-project] in_win_projects=" << in_win
                  << " proj_no_year=" << proj_no_year
                  << " edges_seen=" << edges_seen
                  << " orgs_kept=" << num_kept
                  << " (min_org_projects=" << min_proj << ")";

        if (num_kept == 0) { num_vertex_ = 0; return; }

        // Pass 3: project to org-pairs. Skip mega-projects to avoid N² blowup.
        const int max_psize = project_cfg_.max_project_size;
        std::vector<std::unordered_map<int, double>> agg(num_kept);
        int64_t pair_emits = 0;
        int64_t big_proj_skipped = 0;
        for (size_t p = 0; p < num_projs; ++p) {
            if (!proj_in_window[p]) continue;
            const auto& orgs = proj_orgs[p];
            if (orgs.size() < 2) continue;
            if (max_psize > 0 && static_cast<int>(orgs.size()) > max_psize) {
                ++big_proj_skipped; continue;
            }
            std::vector<int> svec;
            svec.reserve(orgs.size());
            for (int o : orgs) {
                int s = org_to_super[o];
                if (s >= 0) svec.push_back(s);
            }
            std::sort(svec.begin(), svec.end());
            svec.erase(std::unique(svec.begin(), svec.end()), svec.end());
            for (size_t x = 0; x < svec.size(); ++x) {
                int a = svec[x];
                for (size_t y = x + 1; y < svec.size(); ++y) {
                    int b = svec[y];
                    agg[a][b] += 1.0;
                    ++pair_emits;
                }
            }
        }

        const double thr = static_cast<double>(project_cfg_.min_edge_weight);
        num_vertex_ = num_kept;
        adj_.assign(num_vertex_, {});
        int64_t total_entries = 0;
        for (int a = 0; a < num_kept; ++a) {
            for (auto& [b, w] : agg[a]) {
                if (w < thr) continue;
                adj_[a].push_back({b, w});
                adj_[b].push_back({a, w});
                total_entries += 2;
            }
        }
        num_edge_ = static_cast<int>(total_entries);

        // Annotate vertices with org_id and country_code.
        global_to_local_.assign(num_vertex_, {});
        aggregate_keys_.resize(num_vertex_);
        aggregate_extra_.resize(num_vertex_);
        for (int s = 0; s < num_kept; ++s) {
            vid_t ov = static_cast<vid_t>(org_keep[s]);
            global_to_local_[s] = {org_label, ov};
            if (org_id_col) {
                aggregate_keys_[s] = std::string(org_id_col->get_view(org_keep[s]));
            } else {
                aggregate_keys_[s] = std::to_string(org_keep[s]);
            }
            if (cc_col) {
                aggregate_extra_[s] = std::string(cc_col->get_view(org_keep[s]));
            }
        }

        auto tend = std::chrono::steady_clock::now();
        LOG(INFO) << "[Kcore-project] year=[" << ymin << "," << ymax
                  << "] pair_emits=" << pair_emits
                  << " big_proj_skipped(>" << max_psize << ")=" << big_proj_skipped
                  << " V=" << num_vertex_ << " 2E=" << total_entries
                  << " min_w=" << thr
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tend - tstart).count()
                  << "ms";
    }

    const StorageReadInterface& graph_;

    bool coauthor_mode_ = false;
    CoauthorConfig coauthor_cfg_;

    bool project_mode_ = false;
    ProjectConfig project_cfg_;

    int num_vertex_ = 0;
    int num_edge_ = 0;

    std::vector<int> label_offsets_;
    std::vector<std::pair<label_t, vid_t>> global_to_local_;
    std::vector<std::vector<std::pair<int, double>>> adj_;

    std::vector<std::string> aggregate_keys_;
    std::vector<std::string> aggregate_extra_;
};

// ============================================================================
// Helper: generate unique output file path
// ============================================================================

inline std::string GenerateKcoreOutputPath(const std::string& prefix) {
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::filesystem::create_directories("/tmp/p/neug_kcore");
    return "/tmp/p/neug_kcore/" + prefix + "_" + std::to_string(ts) + ".csv";
}

// ============================================================================
// CALL KCORE() — plain k-core on the full graph
// ============================================================================

struct KcoreInput : public CallFuncInputBase {
    KcoreInput() = default;
    ~KcoreInput() override = default;
};

struct KcoreFunction {
    static constexpr const char* name = "KCORE";

    static function_set getFunctionSet() {
        function_set functionSet;
        call_output_columns outputCols{
            {"status", common::LogicalTypeID::STRING},
            {"num_vertices", common::LogicalTypeID::INT64},
            {"max_coreness", common::LogicalTypeID::INT64},
            {"num_in_max_core", common::LogicalTypeID::INT64},
            {"num_edges", common::LogicalTypeID::INT64},
            {"result_file", common::LogicalTypeID::STRING}
        };

        auto func = std::make_unique<NeugCallFunction>(
            name,
            std::vector<common::LogicalTypeID>{},
            call_output_columns(outputCols));

        func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                            const ::physical::PhysicalPlan&, int)
            -> std::unique_ptr<CallFuncInputBase> {
            LOG(INFO) << "[KCORE] Bind: no params";
            return std::make_unique<KcoreInput>();
        };
        func->execFunc = [](const CallFuncInputBase&,
                            IStorageInterface& graph) -> execution::Context {
            return ExecutePlain(graph);
        };
        functionSet.push_back(std::move(func));
        return functionSet;
    }

private:
    static execution::Context ExecutePlain(IStorageInterface& graph) {
        auto* readInterface = dynamic_cast<StorageReadInterface*>(&graph);
        if (!readInterface) {
            LOG(ERROR) << "[KCORE] graph not StorageReadInterface";
            return execution::Context();
        }
        KcoreComputer computer(*readInterface);
        auto result = computer.Compute();

        std::string outputFile = GenerateKcoreOutputPath("kcore");
        {
            std::ofstream ofs(outputFile);
            ofs << "global_id,vertex_label,original_vid,coreness\n";
            for (int i = 0; i < result.num_vertices; ++i) {
                auto [vlabel, vid] = result.id_mapping[i];
                ofs << i << "," << (int)vlabel << "," << vid << ","
                    << result.coreness[i] << "\n";
            }
        }
        LOG(INFO) << "[KCORE] Results written to: " << outputFile;

        execution::Context ctx;
        execution::ValueColumnBuilder<std::string> sB; sB.push_back_opt(std::string("success"));
        ctx.set(0, sB.finish());
        execution::ValueColumnBuilder<int64_t> vB; vB.push_back_opt((int64_t)result.num_vertices);
        ctx.set(1, vB.finish());
        execution::ValueColumnBuilder<int64_t> mB; mB.push_back_opt((int64_t)result.max_coreness);
        ctx.set(2, mB.finish());
        execution::ValueColumnBuilder<int64_t> nmB; nmB.push_back_opt((int64_t)result.num_in_max_core);
        ctx.set(3, nmB.finish());
        execution::ValueColumnBuilder<int64_t> eB; eB.push_back_opt((int64_t)result.num_edges);
        ctx.set(4, eB.finish());
        execution::ValueColumnBuilder<std::string> fB; fB.push_back_opt(outputFile);
        ctx.set(5, fB.finish());
        ctx.tag_ids = {0, 1, 2, 3, 4, 5};
        return ctx;
    }
};

// ============================================================================
// CALL KCORE_COAUTHOR(year_min, year_max [, max_coauthors, min_edge_weight])
// ============================================================================

struct KcoreCoauthorInput : public CallFuncInputBase {
    int64_t year_min;
    int64_t year_max;
    int64_t max_coauthors;
    int64_t min_edge_weight;
    KcoreCoauthorInput(int64_t ymin, int64_t ymax,
                       int64_t mc = 30, int64_t mw = 1)
        : year_min(ymin), year_max(ymax),
          max_coauthors(mc), min_edge_weight(mw) {}
    ~KcoreCoauthorInput() override = default;
};

struct KcoreCoauthorFunction {
    static constexpr const char* name = "KCORE_COAUTHOR";

    static function_set getFunctionSet() {
        function_set functionSet;
        call_output_columns outputCols{
            {"status", common::LogicalTypeID::STRING},
            {"num_vertices", common::LogicalTypeID::INT64},
            {"max_coreness", common::LogicalTypeID::INT64},
            {"num_in_max_core", common::LogicalTypeID::INT64},
            {"num_edges", common::LogicalTypeID::INT64},
            {"result_file", common::LogicalTypeID::STRING}
        };

        // Overload A: (year_min, year_max)
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64},
                call_output_columns(outputCols));
            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t ymin = 0, ymax = 0;
                if (proc.query().arguments_size() >= 1 && proc.query().arguments(0).has_const_())
                    ymin = proc.query().arguments(0).const_().i64();
                if (proc.query().arguments_size() >= 2 && proc.query().arguments(1).has_const_())
                    ymax = proc.query().arguments(1).const_().i64();
                LOG(INFO) << "[KCORE_COAUTHOR] Bind (short): year=[" << ymin << "," << ymax << "]";
                return std::make_unique<KcoreCoauthorInput>(ymin, ymax);
            };
            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph) -> execution::Context {
                return ExecuteCoauthor(input, graph);
            };
            functionSet.push_back(std::move(func));
        }

        // Overload B: (year_min, year_max, max_coauthors, min_edge_weight)
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64},
                call_output_columns(outputCols));
            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t ymin = 0, ymax = 0, mc = 30, mw = 1;
                auto& args = proc.query();
                if (args.arguments_size() >= 1 && args.arguments(0).has_const_())
                    ymin = args.arguments(0).const_().i64();
                if (args.arguments_size() >= 2 && args.arguments(1).has_const_())
                    ymax = args.arguments(1).const_().i64();
                if (args.arguments_size() >= 3 && args.arguments(2).has_const_())
                    mc = args.arguments(2).const_().i64();
                if (args.arguments_size() >= 4 && args.arguments(3).has_const_())
                    mw = args.arguments(3).const_().i64();
                LOG(INFO) << "[KCORE_COAUTHOR] Bind (full): year=[" << ymin << "," << ymax
                          << "] max_co=" << mc << " min_w=" << mw;
                return std::make_unique<KcoreCoauthorInput>(ymin, ymax, mc, mw);
            };
            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph) -> execution::Context {
                return ExecuteCoauthor(input, graph);
            };
            functionSet.push_back(std::move(func));
        }

        return functionSet;
    }

private:
    static execution::Context ExecuteCoauthor(
            const CallFuncInputBase& input, IStorageInterface& graph) {
        auto& cIn = static_cast<const KcoreCoauthorInput&>(input);
        auto* readInterface = dynamic_cast<StorageReadInterface*>(&graph);
        if (!readInterface) {
            LOG(ERROR) << "[KCORE_COAUTHOR] graph not StorageReadInterface";
            return execution::Context();
        }
        KcoreComputer::CoauthorConfig cfg;
        cfg.year_min = cIn.year_min;
        cfg.year_max = cIn.year_max;
        cfg.max_coauthors = static_cast<int>(cIn.max_coauthors);
        cfg.min_edge_weight = static_cast<int>(cIn.min_edge_weight);

        KcoreComputer computer(*readInterface, cfg);
        auto result = computer.Compute();

        std::string outputFile = GenerateKcoreOutputPath("kcore_coauthor");
        {
            std::ofstream ofs(outputFile);
            ofs << "global_id,country_code,coreness\n";
            for (int i = 0; i < result.num_vertices; ++i) {
                const std::string& cc =
                    (i < (int)result.aggregate_keys.size()) ? result.aggregate_keys[i] : std::string{};
                ofs << i << "," << cc << "," << result.coreness[i] << "\n";
            }
        }
        LOG(INFO) << "[KCORE_COAUTHOR] Results written to: " << outputFile;

        execution::Context ctx;
        execution::ValueColumnBuilder<std::string> sB; sB.push_back_opt(std::string("success"));
        ctx.set(0, sB.finish());
        execution::ValueColumnBuilder<int64_t> vB; vB.push_back_opt((int64_t)result.num_vertices);
        ctx.set(1, vB.finish());
        execution::ValueColumnBuilder<int64_t> mB; mB.push_back_opt((int64_t)result.max_coreness);
        ctx.set(2, mB.finish());
        execution::ValueColumnBuilder<int64_t> nmB; nmB.push_back_opt((int64_t)result.num_in_max_core);
        ctx.set(3, nmB.finish());
        execution::ValueColumnBuilder<int64_t> eB; eB.push_back_opt((int64_t)result.num_edges);
        ctx.set(4, eB.finish());
        execution::ValueColumnBuilder<std::string> fB; fB.push_back_opt(outputFile);
        ctx.set(5, fB.finish());
        ctx.tag_ids = {0, 1, 2, 3, 4, 5};
        return ctx;
    }
};

// ============================================================================
// CALL KCORE_PROJECT(year_min, year_max
//                    [, min_org_projects, min_edge_weight])
// ============================================================================

struct KcoreProjectInput : public CallFuncInputBase {
    int64_t year_min;
    int64_t year_max;
    int64_t min_org_projects;
    int64_t min_edge_weight;
    KcoreProjectInput(int64_t ymin, int64_t ymax,
                      int64_t mp = 1, int64_t mw = 1)
        : year_min(ymin), year_max(ymax),
          min_org_projects(mp), min_edge_weight(mw) {}
    ~KcoreProjectInput() override = default;
};

struct KcoreProjectFunction {
    static constexpr const char* name = "KCORE_PROJECT";

    static function_set getFunctionSet() {
        function_set functionSet;
        call_output_columns outputCols{
            {"status", common::LogicalTypeID::STRING},
            {"num_vertices", common::LogicalTypeID::INT64},
            {"max_coreness", common::LogicalTypeID::INT64},
            {"num_in_max_core", common::LogicalTypeID::INT64},
            {"num_edges", common::LogicalTypeID::INT64},
            {"result_file", common::LogicalTypeID::STRING}
        };

        // Overload A: (year_min, year_max)
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64},
                call_output_columns(outputCols));
            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t ymin = 0, ymax = 0;
                if (proc.query().arguments_size() >= 1 && proc.query().arguments(0).has_const_())
                    ymin = proc.query().arguments(0).const_().i64();
                if (proc.query().arguments_size() >= 2 && proc.query().arguments(1).has_const_())
                    ymax = proc.query().arguments(1).const_().i64();
                LOG(INFO) << "[KCORE_PROJECT] Bind (short): year=[" << ymin << "," << ymax << "]";
                return std::make_unique<KcoreProjectInput>(ymin, ymax);
            };
            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph) -> execution::Context {
                return ExecuteProject(input, graph);
            };
            functionSet.push_back(std::move(func));
        }

        // Overload B: (year_min, year_max, min_org_projects, min_edge_weight)
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::INT64},
                call_output_columns(outputCols));
            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t ymin = 0, ymax = 0, mp = 1, mw = 1;
                auto& args = proc.query();
                if (args.arguments_size() >= 1 && args.arguments(0).has_const_())
                    ymin = args.arguments(0).const_().i64();
                if (args.arguments_size() >= 2 && args.arguments(1).has_const_())
                    ymax = args.arguments(1).const_().i64();
                if (args.arguments_size() >= 3 && args.arguments(2).has_const_())
                    mp = args.arguments(2).const_().i64();
                if (args.arguments_size() >= 4 && args.arguments(3).has_const_())
                    mw = args.arguments(3).const_().i64();
                LOG(INFO) << "[KCORE_PROJECT] Bind (full): year=[" << ymin << "," << ymax
                          << "] min_proj=" << mp << " min_w=" << mw;
                return std::make_unique<KcoreProjectInput>(ymin, ymax, mp, mw);
            };
            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph) -> execution::Context {
                return ExecuteProject(input, graph);
            };
            functionSet.push_back(std::move(func));
        }

        return functionSet;
    }

private:
    static execution::Context ExecuteProject(
            const CallFuncInputBase& input, IStorageInterface& graph) {
        auto& cIn = static_cast<const KcoreProjectInput&>(input);
        auto* readInterface = dynamic_cast<StorageReadInterface*>(&graph);
        if (!readInterface) {
            LOG(ERROR) << "[KCORE_PROJECT] graph not StorageReadInterface";
            return execution::Context();
        }
        KcoreComputer::ProjectConfig cfg;
        cfg.year_min = cIn.year_min;
        cfg.year_max = cIn.year_max;
        cfg.min_org_projects = static_cast<int>(cIn.min_org_projects);
        cfg.min_edge_weight = static_cast<int>(cIn.min_edge_weight);

        KcoreComputer computer(*readInterface, cfg);
        auto result = computer.Compute();

        std::string outputFile = GenerateKcoreOutputPath("kcore_project");
        {
            std::ofstream ofs(outputFile);
            ofs << "global_id,org_id,country_code,coreness\n";
            for (int i = 0; i < result.num_vertices; ++i) {
                const std::string& oid =
                    (i < (int)result.aggregate_keys.size()) ? result.aggregate_keys[i] : std::string{};
                const std::string& cc =
                    (i < (int)result.aggregate_extra.size()) ? result.aggregate_extra[i] : std::string{};
                // org_id may legitimately contain commas; quote it.
                ofs << i << ",\"" << oid << "\"," << cc << "," << result.coreness[i] << "\n";
            }
        }
        LOG(INFO) << "[KCORE_PROJECT] Results written to: " << outputFile;

        execution::Context ctx;
        execution::ValueColumnBuilder<std::string> sB; sB.push_back_opt(std::string("success"));
        ctx.set(0, sB.finish());
        execution::ValueColumnBuilder<int64_t> vB; vB.push_back_opt((int64_t)result.num_vertices);
        ctx.set(1, vB.finish());
        execution::ValueColumnBuilder<int64_t> mB; mB.push_back_opt((int64_t)result.max_coreness);
        ctx.set(2, mB.finish());
        execution::ValueColumnBuilder<int64_t> nmB; nmB.push_back_opt((int64_t)result.num_in_max_core);
        ctx.set(3, nmB.finish());
        execution::ValueColumnBuilder<int64_t> eB; eB.push_back_opt((int64_t)result.num_edges);
        ctx.set(4, eB.finish());
        execution::ValueColumnBuilder<std::string> fB; fB.push_back_opt(outputFile);
        ctx.set(5, fB.finish());
        ctx.tag_ids = {0, 1, 2, 3, 4, 5};
        return ctx;
    }
};

}  // namespace function
}  // namespace neug
