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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <random>
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
// Louvain Community Detection Algorithm Implementation
// ============================================================================

class LouvainComputer {
public:
    // Mode 1 (existing): run Louvain on the full underlying graph.
    LouvainComputer(const StorageReadInterface& graph,
                    int max_iterations, double resolution, int max_levels)
        : graph_(graph),
          max_iterations_(max_iterations),
          resolution_(resolution),
          max_levels_(max_levels) {}

    // Config for the bipartite co-author projection mode (Case A Level-1):
    //   Publication -[hasAuthorInstitution]-> Organization
    //   → fold two orgs that co-author a pub into an org-org edge
    //   → aggregate by Organization.country_code → country-country graph
    //   → run Louvain on that country graph
    struct CoauthorConfig {
        int64_t year_min = 0;           // inclusive
        int64_t year_max = 0;           // inclusive; year_max < year_min means no year filter
        int max_coauthors = 30;         // skip pubs with more than this many distinct author orgs;
                                        // 0 = no filter
    };
    // Mode 2 (new): co-author projection → country-level Louvain.
    LouvainComputer(const StorageReadInterface& graph, CoauthorConfig cfg,
                    int max_iterations, double resolution, int max_levels)
        : graph_(graph),
          max_iterations_(max_iterations),
          resolution_(resolution),
          max_levels_(max_levels),
          coauthor_mode_(true),
          coauthor_cfg_(cfg) {}

    // Config for the infrastructure-projection mode (new case):
    //   Datasource -[hosts]-> Publication -[hasAuthorInstitution]-> Organization
    //   → build bipartite (Datasource, country_code) weighted by host count
    //   → project to Country × Country:
    //       w(c1, c2) = Σ_d min(w_{d,c1}, w_{d,c2})
    //   → run Louvain on that country graph
    // Tests: "is the open-science infrastructure footprint of countries as
    // multipolar as their co-authorship footprint, or more centralized?"
    struct InfraConfig {
        int64_t year_min = 0;               // inclusive
        int64_t year_max = 0;               // inclusive; year_max < year_min means no year filter
        int min_hosts_per_datasource = 0;   // drop datasources hosting < this many
                                            // (distinct, counted) pubs in the window; 0 = no filter
        int max_author_orgs = 30;           // same mega-collab guard as coauthor mode;
                                            // 0 = no filter
    };
    // Mode 3 (new): infra projection → country-level Louvain.
    LouvainComputer(const StorageReadInterface& graph, InfraConfig cfg,
                    int max_iterations, double resolution, int max_levels)
        : graph_(graph),
          max_iterations_(max_iterations),
          resolution_(resolution),
          max_levels_(max_levels),
          infra_mode_(true),
          infra_cfg_(cfg) {}

    struct Result {
        int num_vertices = 0;
        int num_communities = 0;
        int levels_completed = 0;
        double modularity = 0.0;
        // Parallel arrays indexed by global_id
        std::vector<int> community;       // community assignment per vertex
        std::vector<std::pair<label_t, vid_t>> id_mapping;  // global_id -> (label, vid)
        // Populated only in coauthor projection mode: the aggregate key (country_code)
        // for each super-vertex. Empty in plain graph mode.
        std::vector<std::string> aggregate_keys;
    };

    Result Compute() {
        Result result;

#ifdef _OPENMP
        constexpr int kLouvainOmpThreads = 24;
        omp_set_num_threads(kLouvainOmpThreads);
        LOG(INFO) << "[Louvain] OpenMP enabled, threads="
                  << omp_get_max_threads();
#else
        LOG(INFO) << "[Louvain] OpenMP DISABLED (serial build)";
#endif

        if (coauthor_mode_) {
            BuildCountryCoauthorProjection();
        } else if (infra_mode_) {
            BuildInfraCountryProjection();
        } else {
            BuildGraph();
        }
        result.num_vertices = num_vertex_;
        result.id_mapping = global_to_local_;
        result.aggregate_keys = aggregate_keys_;

        if (num_vertex_ == 0) {
            LOG(INFO) << "[Louvain] Empty graph, nothing to compute.";
            return result;
        }

        LOG(INFO) << "[Louvain] Running on " << num_vertex_
                  << " vertices, " << num_edge_ << " directed edges"
                  << " (max_iter=" << max_iterations_
                  << ", resolution=" << resolution_
                  << ", max_levels=" << max_levels_ << ")";

        // Phase 0: initialize each vertex as its own community
        std::vector<int> comm(num_vertex_);
        std::iota(comm.begin(), comm.end(), 0);

        // Current-level graph representation (will be coarsened across levels)
        int n = num_vertex_;
        std::vector<std::vector<std::pair<int, double>>> adj = adj_;  // neighbor, weight
        std::vector<double> node_weight(n);
        double total_weight = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : total_weight) schedule(static)
#endif
        for (int u = 0; u < n; ++u) {
            double w = 0.0;
            for (auto& [_, ew] : adj[u]) w += ew;
            node_weight[u] = w;
            total_weight += w;
        }
        total_weight /= 2.0;  // each edge counted twice in undirected adjacency

        int level = 0;
        // node_to_orig[i] = list of original vertex ids that node i represents
        std::vector<std::vector<int>> node_to_orig(n);
        for (int i = 0; i < n; ++i) node_to_orig[i] = {i};

        for (; level < max_levels_; ++level) {
            auto tlv0 = std::chrono::steady_clock::now();
            LOG(INFO) << "[Louvain] Level " << level << " starting: n=" << n;
            bool improved = LocalMove(adj, node_weight, comm, n, total_weight);
            auto tlv1 = std::chrono::steady_clock::now();
            LOG(INFO) << "[Louvain] Level " << level
                      << " LocalMove done in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(tlv1 - tlv0).count()
                      << "ms, improved=" << improved;
            if (!improved && level > 0) break;

            // Renumber communities to be contiguous
            std::unordered_map<int, int> comm_remap;
            int next_id = 0;
            for (int u = 0; u < n; ++u) {
                if (comm_remap.find(comm[u]) == comm_remap.end()) {
                    comm_remap[comm[u]] = next_id++;
                }
                comm[u] = comm_remap[comm[u]];
            }
            int num_comms = next_id;

            if (num_comms == n) break;  // no further coarsening possible

            LOG(INFO) << "[Louvain] Level " << level
                      << " coarsening: " << n << " -> " << num_comms
                      << " super-nodes";

            // Phase 2: coarsen graph — aggregate communities into super-nodes
            std::vector<std::vector<int>> new_node_to_orig(num_comms);
            for (int u = 0; u < n; ++u) {
                for (int orig : node_to_orig[u]) {
                    new_node_to_orig[comm[u]].push_back(orig);
                }
            }

            // Build coarsened adjacency
            std::vector<std::unordered_map<int, double>> coarse_adj_map(num_comms);
            std::vector<double> new_node_weight(num_comms, 0.0);

            for (int u = 0; u < n; ++u) {
                int cu = comm[u];
                new_node_weight[cu] += node_weight[u];
                for (auto& [v, w] : adj[u]) {
                    int cv = comm[v];
                    if (cu <= cv) {  // avoid double counting
                        coarse_adj_map[cu][cv] += w;
                        if (cu != cv) coarse_adj_map[cv][cu] += w;
                    }
                }
            }

            std::vector<std::vector<std::pair<int, double>>> new_adj(num_comms);
#ifdef _OPENMP
#pragma omp parallel for schedule(guided, 32)
#endif
            for (int c = 0; c < num_comms; ++c) {
                new_adj[c].reserve(coarse_adj_map[c].size());
                for (auto& [nbr, w] : coarse_adj_map[c]) {
                    new_adj[c].push_back({nbr, w});
                }
            }

            // Prepare for next level
            adj = std::move(new_adj);
            node_weight = std::move(new_node_weight);
            node_to_orig = std::move(new_node_to_orig);
            n = num_comms;
            comm.resize(n);
            std::iota(comm.begin(), comm.end(), 0);
        }

        // Map final community ids back to original vertices.
        // node_to_orig partitions [0, num_vertex_), so writes by different c
        // target disjoint indices — safe to parallelize over c.
        result.community.resize(num_vertex_);
#ifdef _OPENMP
#pragma omp parallel for schedule(guided, 32)
#endif
        for (int c = 0; c < n; ++c) {
            for (int orig : node_to_orig[c]) {
                result.community[orig] = comm[c];
            }
        }

        // Count distinct communities
        std::unordered_set<int> distinct_comms(result.community.begin(),
                                                result.community.end());
        result.num_communities = static_cast<int>(distinct_comms.size());
        result.levels_completed = level;
        result.modularity = ComputeModularity(result.community, total_weight);

        LOG(INFO) << "[Louvain] Done: " << result.num_communities
                  << " communities, modularity=" << result.modularity
                  << ", levels=" << result.levels_completed;

        return result;
    }

private:
    // Local move phase: greedily move vertices to neighboring communities.
    // Parallel (Jacobi-style): each iteration, all vertices choose their best
    // community in parallel based on a *snapshot* of comm/comm_total, then
    // assignments are applied synchronously. This trades per-step greedy
    // optimality for thread-safety and scalability; convergence quality
    // remains comparable on most graphs.
    // Returns true if any vertex was moved.
    // Compute per-community weight sum: out[comm[u]] += node_weight[u].
    // Uses per-thread heap buffers + merge instead of OpenMP array reduction,
    // whose per-thread private copies can blow the thread stack on large n.
    static void AccumulateByComm(const std::vector<int>& comm,
                                 const std::vector<double>& node_weight,
                                 int n, std::vector<double>& out) {
        std::fill(out.begin(), out.end(), 0.0);
#ifdef _OPENMP
        int nth = omp_get_max_threads();
        std::vector<std::vector<double>> local(nth);
#pragma omp parallel
        {
            int tid = omp_get_thread_num();
            local[tid].assign(n, 0.0);
            double* buf = local[tid].data();
#pragma omp for schedule(static) nowait
            for (int u = 0; u < n; ++u) {
                buf[comm[u]] += node_weight[u];
            }
        }
        double* op = out.data();
#pragma omp parallel for schedule(static)
        for (int i = 0; i < n; ++i) {
            double s = 0.0;
            for (int t = 0; t < nth; ++t) s += local[t][i];
            op[i] = s;
        }
#else
        for (int u = 0; u < n; ++u) {
            out[comm[u]] += node_weight[u];
        }
#endif
    }

    bool LocalMove(std::vector<std::vector<std::pair<int, double>>>& adj,
                   std::vector<double>& node_weight,
                   std::vector<int>& comm,
                   int n, double total_weight) {
        std::vector<double> comm_total(n, 0.0);
        AccumulateByComm(comm, node_weight, n, comm_total);

        std::vector<int> new_comm(n);

        bool any_moved = false;
        for (int iter = 0; iter < max_iterations_; ++iter) {
            int moved_count = 0;

#ifdef _OPENMP
#pragma omp parallel for reduction(+ : moved_count) schedule(guided, 32)
#endif
            for (int u = 0; u < n; ++u) {
                int old_comm = comm[u];
                double ki = node_weight[u];

                // Weight from u to each neighboring community (thread-local)
                std::unordered_map<int, double> comm_edge_weight;
                comm_edge_weight.reserve(adj[u].size());
                for (auto& [v, w] : adj[u]) {
                    comm_edge_weight[comm[v]] += w;
                }

                double ki_over_2m = ki / (2.0 * total_weight);

                // Staying baseline: comm_total excludes u itself
                double old_w = comm_edge_weight.count(old_comm)
                                   ? comm_edge_weight[old_comm]
                                   : 0.0;
                double best_delta =
                    old_w - resolution_ * ki_over_2m * (comm_total[old_comm] - ki);
                int best_comm = old_comm;

                for (auto& [c, w_ic] : comm_edge_weight) {
                    if (c == old_comm) continue;
                    double delta = w_ic - resolution_ * ki_over_2m * comm_total[c];
                    if (delta > best_delta) {
                        best_delta = delta;
                        best_comm = c;
                    }
                }

                new_comm[u] = best_comm;
                if (best_comm != old_comm) moved_count++;
            }

            // Apply updates synchronously and recompute comm_total
            std::swap(comm, new_comm);
            AccumulateByComm(comm, node_weight, n, comm_total);

            if (moved_count > 0) any_moved = true;
            if (moved_count == 0) break;
        }

        return any_moved;
    }

    // Compute modularity of a given community assignment on the original graph
    double ComputeModularity(const std::vector<int>& comm, double total_weight) {
        if (total_weight <= 0.0) return 0.0;

        double q = 0.0;
        // Per-vertex degree (parallel), then dense comm_total via array reduction
        std::vector<double> ku_arr(num_vertex_, 0.0);
#ifdef _OPENMP
#pragma omp parallel for schedule(guided, 32)
#endif
        for (int u = 0; u < num_vertex_; ++u) {
            double ku = 0.0;
            for (auto& [_, w] : adj_[u]) ku += w;
            ku_arr[u] = ku;
        }

        int max_comm_id = 0;
        for (int u = 0; u < num_vertex_; ++u) {
            if (comm[u] > max_comm_id) max_comm_id = comm[u];
        }
        const int nc = max_comm_id + 1;
        std::vector<double> comm_total(nc, 0.0);
#ifdef _OPENMP
        {
            int nth = omp_get_max_threads();
            std::vector<std::vector<double>> local(nth);
#pragma omp parallel
            {
                int tid = omp_get_thread_num();
                local[tid].assign(nc, 0.0);
                double* buf = local[tid].data();
#pragma omp for schedule(static) nowait
                for (int u = 0; u < num_vertex_; ++u) {
                    buf[comm[u]] += ku_arr[u];
                }
            }
            double* op = comm_total.data();
#pragma omp parallel for schedule(static)
            for (int i = 0; i < nc; ++i) {
                double s = 0.0;
                for (int t = 0; t < nth; ++t) s += local[t][i];
                op[i] = s;
            }
        }
#else
        for (int u = 0; u < num_vertex_; ++u) {
            comm_total[comm[u]] += ku_arr[u];
        }
#endif

        // Sum of internal edge weights (reduction)
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : q) schedule(guided, 32)
#endif
        for (int u = 0; u < num_vertex_; ++u) {
            for (auto& [v, w] : adj_[u]) {
                if (comm[u] == comm[v]) {
                    q += w;
                }
            }
        }
        q /= (2.0 * total_weight);

        double penalty = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : penalty) schedule(static)
#endif
        for (int c = 0; c < nc; ++c) {
            double st = comm_total[c];
            penalty += (st / (2.0 * total_weight)) * (st / (2.0 * total_weight));
        }
        q -= resolution_ * penalty;

        return q;
    }

    // Build the undirected adjacency for level 0.
    //
    // Rewritten for speed: uses an offset-based (label, vid) -> global_id
    // mapping instead of a hashmap, and builds adj_ in three parallel passes
    // (count degrees, allocate, fill with atomic cursors). Multi-edges are
    // kept as duplicate entries in adj_ with weight 1.0 each — LocalMove /
    // coarsening aggregate them on-the-fly, so no extra collapse pass needed.
    void BuildGraph() {
        auto tstart = std::chrono::steady_clock::now();
        const auto& schema = graph_.schema();
        label_t num_vertex_labels = schema.vertex_label_num();

        // Step 1: offset-based id mapping.
        // global_id = label_offsets_[label] + vid  for any valid label.
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
        LOG(INFO) << "[Louvain] BuildGraph: " << num_vertex_
                  << " vertices across " << num_vertex_labels << " labels";

        if (num_vertex_ == 0) return;

        // Reverse mapping (dense, filled in parallel per label).
        global_to_local_.assign(num_vertex_, {});
        for (label_t L = 0; L < num_vertex_labels; ++L) {
            int base = label_offsets_[L];
            if (base < 0) continue;
            int sz = static_cast<int>(label_sizes[L]);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
            for (int v = 0; v < sz; ++v) {
                global_to_local_[base + v] = {L, static_cast<vid_t>(v)};
            }
        }

        // Step 2: degree count (parallel, atomic).
        // atomic<int> is not copy/moveable → allocate via unique_ptr<[]>.
        auto tdeg0 = std::chrono::steady_clock::now();
        std::unique_ptr<std::atomic<int>[]> deg(
            new std::atomic<int>[num_vertex_]);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (int i = 0; i < num_vertex_; ++i) {
            deg[i].store(0, std::memory_order_relaxed);
        }

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
            int64_t local_cnt = 0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : local_cnt) schedule(guided, 64)
#endif
            for (int i = 0; i < src_sz; ++i) {
                vid_t src_vid = static_cast<vid_t>(i);
                if (!graph_.IsValidVertex(src_label, src_vid)) continue;
                int src_g = src_base + i;
                NbrList edges = out_view.get_edges(src_vid);
                for (auto it = edges.begin(); it != edges.end(); ++it) {
                    vid_t dst_vid = *it;
                    if (static_cast<size_t>(dst_vid) >= dst_sz) continue;
                    int dst_g = dst_base + static_cast<int>(dst_vid);
                    // Undirected: each edge contributes to both endpoints.
                    deg[src_g].fetch_add(1, std::memory_order_relaxed);
                    deg[dst_g].fetch_add(1, std::memory_order_relaxed);
                    local_cnt++;
                }
            }
            total_directed += local_cnt;
        }
        num_edge_ = static_cast<int>(total_directed);
        auto tdeg1 = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain] BuildGraph: counted " << total_directed
                  << " directed edges in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         tdeg1 - tdeg0).count()
                  << "ms";

        // Step 3: allocate per-vertex adjacency (parallel).
        adj_.assign(num_vertex_, {});
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (int u = 0; u < num_vertex_; ++u) {
            adj_[u].resize(deg[u].load(std::memory_order_relaxed));
        }

        // Step 4: fill adjacency (parallel, atomic per-vertex cursor).
        // Reuse deg[] as cursor by resetting to 0.
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (int i = 0; i < num_vertex_; ++i) {
            deg[i].store(0, std::memory_order_relaxed);
        }

        auto tfill0 = std::chrono::steady_clock::now();
        for (const auto& [key, edge_schema] : schema.get_all_edge_schemas()) {
            auto [src_label, dst_label, e_label] = schema.parse_edge_label(key);
            int src_base = label_offsets_[src_label];
            int dst_base = label_offsets_[dst_label];
            if (src_base < 0 || dst_base < 0) continue;
            GenericView out_view = graph_.GetGenericOutgoingGraphView(
                src_label, dst_label, e_label);
            int src_sz = static_cast<int>(label_sizes[src_label]);
            size_t dst_sz = label_sizes[dst_label];
#ifdef _OPENMP
#pragma omp parallel for schedule(guided, 64)
#endif
            for (int i = 0; i < src_sz; ++i) {
                vid_t src_vid = static_cast<vid_t>(i);
                if (!graph_.IsValidVertex(src_label, src_vid)) continue;
                int src_g = src_base + i;
                NbrList edges = out_view.get_edges(src_vid);
                for (auto it = edges.begin(); it != edges.end(); ++it) {
                    vid_t dst_vid = *it;
                    if (static_cast<size_t>(dst_vid) >= dst_sz) continue;
                    int dst_g = dst_base + static_cast<int>(dst_vid);
                    int pu = deg[src_g].fetch_add(
                        1, std::memory_order_relaxed);
                    int pv = deg[dst_g].fetch_add(
                        1, std::memory_order_relaxed);
                    adj_[src_g][pu] = {dst_g, 1.0};
                    adj_[dst_g][pv] = {src_g, 1.0};
                }
            }
        }
        auto tfill1 = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain] BuildGraph: filled adjacency in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         tfill1 - tfill0).count()
                  << "ms; total BuildGraph="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         tfill1 - tstart).count()
                  << "ms";
    }

    // Build the co-author projection graph (Case A Level-1):
    //   vertices = distinct Organization.country_code values
    //   edges    = sum of co-authorships between countries within [year_min, year_max]
    // Only this BuildGraph variant is neug-aware; the Compute/LocalMove core is
    // reused as-is from the plain-graph path, so parallel behavior is unchanged.
    void BuildCountryCoauthorProjection() {
        auto tstart = std::chrono::steady_clock::now();
        const auto& schema = graph_.schema();

        // --- Resolve labels ---
        label_t pub_label;
        label_t org_label;
        try {
            pub_label = schema.get_vertex_label_id("Publication");
            org_label = schema.get_vertex_label_id("Organization");
        } catch (...) {
            LOG(ERROR) << "[Louvain-coauthor] Publication/Organization vertex label not found";
            return;
        }
        // Find the Publication→Organization edge label.
        label_t pub_org_edge_label = static_cast<label_t>(-1);
        for (const auto& [key, _] : schema.get_all_edge_schemas()) {
            auto [src, dst, e] = schema.parse_edge_label(key);
            if (src == pub_label && dst == org_label) {
                pub_org_edge_label = e;
                break;
            }
        }
        if (pub_org_edge_label == static_cast<label_t>(-1)) {
            LOG(ERROR) << "[Louvain-coauthor] No edge Publication→Organization found";
            return;
        }
        LOG(INFO) << "[Louvain-coauthor] pub_label=" << (int)pub_label
                  << " org_label=" << (int)org_label
                  << " edge_label=" << (int)pub_org_edge_label;

        // --- Typed column accessors ---
        auto year_col_base = graph_.GetVertexPropColumn(pub_label, "year");
        auto year_col = std::dynamic_pointer_cast<TypedRefColumn<int64_t>>(year_col_base);
        if (!year_col) {
            LOG(ERROR) << "[Louvain-coauthor] Publication.year is not INT64";
            return;
        }
        auto cc_col_base = graph_.GetVertexPropColumn(org_label, "country_code");
        auto cc_col = std::dynamic_pointer_cast<TypedRefColumn<std::string_view>>(cc_col_base);
        if (!cc_col) {
            LOG(ERROR) << "[Louvain-coauthor] Organization.country_code is not STRING";
            return;
        }

        VertexSet pub_set = graph_.GetVertexSet(pub_label);
        VertexSet org_set = graph_.GetVertexSet(org_label);
        const size_t num_pubs = pub_set.size();
        const size_t num_orgs = org_set.size();

        // --- Build org → country_idx mapping ---
        std::unordered_map<std::string, int> cc_to_idx;
        cc_to_idx.reserve(512);
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
        LOG(INFO) << "[Louvain-coauthor] Mapped " << num_orgs << " orgs → "
                  << num_countries << " distinct country codes";

        if (num_countries == 0) {
            num_vertex_ = 0;
            return;
        }

        // --- Projection: iterate publications, fold into country-pair weights ---
        GenericView pub_org_view = graph_.GetGenericOutgoingGraphView(
            pub_label, org_label, pub_org_edge_label);

        const bool year_filter_active =
            coauthor_cfg_.year_max >= coauthor_cfg_.year_min;
        const int max_coauthors = coauthor_cfg_.max_coauthors;

        // Sparse symmetric aggregator: we store only (a, b) with a <= b.
        // std::map avoids the unordered_map rehash cost at small N; country
        // graphs are ~200 nodes so map / unordered_map both fine.
        std::vector<std::unordered_map<int, double>> agg(num_countries);

        int64_t pubs_scanned = 0;
        int64_t pubs_accepted = 0;
        int64_t pubs_dropped_year = 0;
        int64_t pubs_dropped_size = 0;
        int64_t pubs_dropped_empty = 0;       // no orgs with a valid country
        int64_t pubs_dropped_single = 0;      // only one distinct country → nothing to pair

        for (size_t i = 0; i < num_pubs; ++i) {
            vid_t pub_vid = static_cast<vid_t>(i);
            if (!graph_.IsValidVertex(pub_label, pub_vid)) continue;
            ++pubs_scanned;

            if (year_filter_active) {
                int64_t y = year_col->get_view(i);
                if (y < coauthor_cfg_.year_min || y > coauthor_cfg_.year_max) {
                    ++pubs_dropped_year;
                    continue;
                }
            }

            NbrList edges = pub_org_view.get_edges(pub_vid);
            // Dedup author orgs at the edge-iteration level (same (pub,org) can
            // appear multiple times in some datasets), then collect the country
            // indices. The large-collab filter counts *distinct orgs*, not
            // distinct countries and not raw edges — that matches the spec
            // (CERN-style papers have 500+ author institutions).
            std::unordered_set<int> distinct_orgs;
            distinct_orgs.reserve(8);
            std::vector<int> cvec;
            cvec.reserve(8);
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                vid_t org_vid = *it;
                if (static_cast<size_t>(org_vid) >= num_orgs) continue;
                if (!distinct_orgs.insert(static_cast<int>(org_vid)).second) continue;
                int cidx = org_to_cidx[org_vid];
                if (cidx < 0) continue;
                cvec.push_back(cidx);
            }

            if (max_coauthors > 0 &&
                static_cast<int>(distinct_orgs.size()) > max_coauthors) {
                ++pubs_dropped_size;
                continue;
            }
            if (cvec.empty()) {
                ++pubs_dropped_empty;
                continue;
            }
            // Dedup country ids for this pub (multiple orgs may share country).
            std::sort(cvec.begin(), cvec.end());
            cvec.erase(std::unique(cvec.begin(), cvec.end()), cvec.end());

            // Single-country pubs are useless for cross-country clustering.
            if (cvec.size() < 2) {
                ++pubs_dropped_single;
                continue;
            }

            // Fold into pair weights. We DROP within-country pairs (self-loops):
            // inter-country community detection is about how countries cluster
            // with *other* countries, and within-country co-authorship swamps
            // the modularity objective (typically >80% of a country's total
            // co-authored pairs are internal), causing Louvain to collapse to
            // the trivial "every country is its own community" partition.
            for (size_t x = 0; x < cvec.size(); ++x) {
                for (size_t y = x + 1; y < cvec.size(); ++y) {
                    int a = cvec[x], b = cvec[y];  // a < b (cvec is sorted, dedup)
                    agg[a][b] += 1.0;
                }
            }
            ++pubs_accepted;
        }

        auto tproj = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain-coauthor] Projection done: scanned="
                  << pubs_scanned << " accepted=" << pubs_accepted
                  << " dropped_year=" << pubs_dropped_year
                  << " dropped_size=" << pubs_dropped_size
                  << " dropped_empty=" << pubs_dropped_empty
                  << " dropped_single_country=" << pubs_dropped_single
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tproj - tstart).count()
                  << "ms";

        // --- Materialize undirected adjacency ---
        // Convention: each undirected edge (a,b) contributes one entry to
        // adj_[a] and one to adj_[b]. Self-loops (a==b) go in twice to adj_[a],
        // matching BuildGraph()'s convention.
        num_vertex_ = num_countries;
        adj_.assign(num_vertex_, {});
        std::vector<size_t> counts(num_vertex_, 0);
        for (int a = 0; a < num_countries; ++a) {
            for (auto& [b, w] : agg[a]) {
                (void)w;
                counts[a]++;
                counts[b]++;  // if a == b this increments twice, as intended
            }
        }
        for (int i = 0; i < num_vertex_; ++i) adj_[i].reserve(counts[i]);
        int64_t total_entries = 0;
        for (int a = 0; a < num_countries; ++a) {
            for (auto& [b, w] : agg[a]) {
                adj_[a].push_back({b, w});
                adj_[b].push_back({a, w});  // when a==b this appends to same list
                total_entries += 2;
            }
        }
        num_edge_ = static_cast<int>(total_entries);

        // --- Mapping for output CSV ---
        global_to_local_.assign(num_vertex_, {});
        aggregate_keys_.resize(num_vertex_);
        for (int i = 0; i < num_vertex_; ++i) {
            // label/vid fields aren't meaningful in projection mode; we use
            // -1 sentinel for label and the country index for vid so the row
            // still has something interpretable.
            global_to_local_[i] = {static_cast<label_t>(-1),
                                   static_cast<vid_t>(i)};
            aggregate_keys_[i] = cc_list[i];
        }

        auto tend = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain-coauthor] Built country graph: "
                  << num_vertex_ << " vertices, " << total_entries
                  << " directed adjacency entries, total="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tend - tstart).count()
                  << "ms";
    }

    // Build the infrastructure-projection graph (new case):
    //   vertices = distinct Organization.country_code values
    //   edges    = for each datasource d, sum over country pairs (c1,c2) of
    //              min(host_count(d,c1), host_count(d,c2))
    //
    // Rationale: two countries share "infrastructure weight" through a given
    // datasource proportional to the smaller of their host-counts on it.
    // min-coupling is the standard weighted projection that does not amplify
    // asymmetric users of a platform (e.g. a country with 10 arXiv pubs and
    // one with 10^6 arXiv pubs contribute min=10, not 10^6).
    //
    // Two-pass construction:
    //   Pass 1: scan Publications. For each pub in [year_min, year_max] collect
    //           the set of country indices reached via its author institutions
    //           (with the same mega-collaboration guard as coauthor mode), and
    //           store it keyed by pub vid.
    //   Pass 2: scan Datasource→Publication edges. For each (d, p) lookup the
    //           country set collected in pass 1 and increment host_count[d][c]
    //           by 1 for every c in the set (so a single pub with authors from
    //           US and DE contributes +1 to (d,US) and +1 to (d,DE)).
    //   Pass 3: project to Country×Country via min-coupling per datasource.
    void BuildInfraCountryProjection() {
        auto tstart = std::chrono::steady_clock::now();
        const auto& schema = graph_.schema();

        // --- Resolve labels ---
        label_t pub_label, org_label, ds_label;
        try {
            pub_label = schema.get_vertex_label_id("Publication");
            org_label = schema.get_vertex_label_id("Organization");
            ds_label  = schema.get_vertex_label_id("Datasource");
        } catch (...) {
            LOG(ERROR) << "[Louvain-infra] Publication/Organization/Datasource vertex label missing";
            return;
        }
        // Publication → Organization edge label
        label_t pub_org_edge_label = static_cast<label_t>(-1);
        // Datasource → Publication edge label
        label_t ds_pub_edge_label = static_cast<label_t>(-1);
        for (const auto& [key, _] : schema.get_all_edge_schemas()) {
            auto [src, dst, e] = schema.parse_edge_label(key);
            if (src == pub_label && dst == org_label && pub_org_edge_label == static_cast<label_t>(-1)) {
                pub_org_edge_label = e;
            }
            if (src == ds_label && dst == pub_label && ds_pub_edge_label == static_cast<label_t>(-1)) {
                ds_pub_edge_label = e;
            }
        }
        if (pub_org_edge_label == static_cast<label_t>(-1)) {
            LOG(ERROR) << "[Louvain-infra] No edge Publication→Organization found";
            return;
        }
        if (ds_pub_edge_label == static_cast<label_t>(-1)) {
            LOG(ERROR) << "[Louvain-infra] No edge Datasource→Publication found";
            return;
        }
        LOG(INFO) << "[Louvain-infra] pub_label=" << (int)pub_label
                  << " org_label=" << (int)org_label
                  << " ds_label=" << (int)ds_label
                  << " pub_org_edge_label=" << (int)pub_org_edge_label
                  << " ds_pub_edge_label=" << (int)ds_pub_edge_label;

        // --- Typed column accessors ---
        auto year_col_base = graph_.GetVertexPropColumn(pub_label, "year");
        auto year_col = std::dynamic_pointer_cast<TypedRefColumn<int64_t>>(year_col_base);
        if (!year_col) {
            LOG(ERROR) << "[Louvain-infra] Publication.year is not INT64";
            return;
        }
        auto cc_col_base = graph_.GetVertexPropColumn(org_label, "country_code");
        auto cc_col = std::dynamic_pointer_cast<TypedRefColumn<std::string_view>>(cc_col_base);
        if (!cc_col) {
            LOG(ERROR) << "[Louvain-infra] Organization.country_code is not STRING";
            return;
        }

        VertexSet pub_set = graph_.GetVertexSet(pub_label);
        VertexSet org_set = graph_.GetVertexSet(org_label);
        VertexSet ds_set  = graph_.GetVertexSet(ds_label);
        const size_t num_pubs = pub_set.size();
        const size_t num_orgs = org_set.size();
        const size_t num_datasources = ds_set.size();

        // --- Build org → country_idx mapping ---
        std::unordered_map<std::string, int> cc_to_idx;
        cc_to_idx.reserve(512);
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
        LOG(INFO) << "[Louvain-infra] Mapped " << num_orgs << " orgs → "
                  << num_countries << " distinct country codes";

        if (num_countries == 0) {
            num_vertex_ = 0;
            return;
        }

        const bool year_filter_active =
            infra_cfg_.year_max >= infra_cfg_.year_min;
        const int max_author_orgs = infra_cfg_.max_author_orgs;

        // --- Pass 1: pub → sorted distinct country indices (within window) ---
        // pub_countries[pub_vid] may be empty if the pub is filtered out.
        GenericView pub_org_view = graph_.GetGenericOutgoingGraphView(
            pub_label, org_label, pub_org_edge_label);

        std::vector<std::vector<int>> pub_countries(num_pubs);
        int64_t pubs_scanned = 0;
        int64_t pubs_accepted = 0;
        int64_t pubs_dropped_year = 0;
        int64_t pubs_dropped_size = 0;
        int64_t pubs_dropped_empty = 0;

        for (size_t i = 0; i < num_pubs; ++i) {
            vid_t pub_vid = static_cast<vid_t>(i);
            if (!graph_.IsValidVertex(pub_label, pub_vid)) continue;
            ++pubs_scanned;

            if (year_filter_active) {
                int64_t y = year_col->get_view(i);
                if (y < infra_cfg_.year_min || y > infra_cfg_.year_max) {
                    ++pubs_dropped_year;
                    continue;
                }
            }

            NbrList edges = pub_org_view.get_edges(pub_vid);
            std::unordered_set<int> distinct_orgs;
            distinct_orgs.reserve(8);
            std::vector<int> cvec;
            cvec.reserve(8);
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                vid_t org_vid = *it;
                if (static_cast<size_t>(org_vid) >= num_orgs) continue;
                if (!distinct_orgs.insert(static_cast<int>(org_vid)).second) continue;
                int cidx = org_to_cidx[org_vid];
                if (cidx < 0) continue;
                cvec.push_back(cidx);
            }

            if (max_author_orgs > 0 &&
                static_cast<int>(distinct_orgs.size()) > max_author_orgs) {
                ++pubs_dropped_size;
                continue;
            }
            if (cvec.empty()) {
                ++pubs_dropped_empty;
                continue;
            }
            std::sort(cvec.begin(), cvec.end());
            cvec.erase(std::unique(cvec.begin(), cvec.end()), cvec.end());
            pub_countries[i] = std::move(cvec);
            ++pubs_accepted;
        }

        auto tpass1 = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain-infra] Pass 1 done: scanned=" << pubs_scanned
                  << " accepted=" << pubs_accepted
                  << " dropped_year=" << pubs_dropped_year
                  << " dropped_size=" << pubs_dropped_size
                  << " dropped_empty=" << pubs_dropped_empty
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tpass1 - tstart).count()
                  << "ms";

        // --- Pass 2: datasource → country host_count via shared pubs ---
        // ds_country_host[d] : country_idx -> host_count
        GenericView ds_pub_view = graph_.GetGenericOutgoingGraphView(
            ds_label, pub_label, ds_pub_edge_label);

        std::vector<std::unordered_map<int, int64_t>> ds_country_host(num_datasources);
        std::vector<int64_t> ds_total_host(num_datasources, 0);
        int64_t ds_edges_seen = 0;
        for (size_t i = 0; i < num_datasources; ++i) {
            vid_t ds_vid = static_cast<vid_t>(i);
            if (!graph_.IsValidVertex(ds_label, ds_vid)) continue;
            NbrList edges = ds_pub_view.get_edges(ds_vid);
            // Dedup target pubs in case an edge appears multiply.
            std::unordered_set<int> seen_pubs;
            seen_pubs.reserve(16);
            auto& bucket = ds_country_host[i];
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                vid_t pub_vid = *it;
                if (static_cast<size_t>(pub_vid) >= num_pubs) continue;
                if (!seen_pubs.insert(static_cast<int>(pub_vid)).second) continue;
                const auto& cvec = pub_countries[pub_vid];
                if (cvec.empty()) continue;
                ++ds_edges_seen;
                ++ds_total_host[i];
                for (int c : cvec) bucket[c]++;
            }
        }

        // Optionally drop long-tail datasources before projecting.
        const int min_hosts = infra_cfg_.min_hosts_per_datasource;
        int64_t ds_kept = 0, ds_dropped_small = 0, ds_dropped_empty = 0;
        for (size_t i = 0; i < num_datasources; ++i) {
            if (ds_country_host[i].empty()) {
                ++ds_dropped_empty;
                continue;
            }
            if (min_hosts > 0 && ds_total_host[i] < min_hosts) {
                ds_country_host[i].clear();
                ++ds_dropped_small;
                continue;
            }
            ++ds_kept;
        }

        auto tpass2 = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain-infra] Pass 2 done: datasources_kept=" << ds_kept
                  << " dropped_empty=" << ds_dropped_empty
                  << " dropped_small=" << ds_dropped_small
                  << " (min_hosts=" << min_hosts << ")"
                  << " in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tpass2 - tpass1).count()
                  << "ms";

        // --- Pass 3: project to Country × Country via min-coupling ---
        // agg[a] : b -> w, a < b. Self-pairs (a == b) dropped: like in coauthor
        // mode, Louvain on intra-country weight collapses to trivial partition.
        std::vector<std::unordered_map<int, double>> agg(num_countries);
        for (size_t i = 0; i < num_datasources; ++i) {
            const auto& bucket = ds_country_host[i];
            if (bucket.size() < 2) continue;
            // Copy to vector so we can enumerate pairs in sorted order.
            std::vector<std::pair<int, int64_t>> items(bucket.begin(), bucket.end());
            std::sort(items.begin(), items.end(),
                [](const auto& x, const auto& y){ return x.first < y.first; });
            for (size_t x = 0; x < items.size(); ++x) {
                int a = items[x].first;
                int64_t wa = items[x].second;
                for (size_t y = x + 1; y < items.size(); ++y) {
                    int b = items[y].first;
                    int64_t wb = items[y].second;
                    double coupling = static_cast<double>(std::min(wa, wb));
                    if (coupling <= 0.0) continue;
                    agg[a][b] += coupling;
                }
            }
        }

        // --- Materialize undirected adjacency (same convention as coauthor) ---
        num_vertex_ = num_countries;
        adj_.assign(num_vertex_, {});
        std::vector<size_t> counts(num_vertex_, 0);
        for (int a = 0; a < num_countries; ++a) {
            for (auto& [b, w] : agg[a]) {
                (void)w;
                counts[a]++;
                counts[b]++;
            }
        }
        for (int i = 0; i < num_vertex_; ++i) adj_[i].reserve(counts[i]);
        int64_t total_entries = 0;
        for (int a = 0; a < num_countries; ++a) {
            for (auto& [b, w] : agg[a]) {
                adj_[a].push_back({b, w});
                adj_[b].push_back({a, w});
                total_entries += 2;
            }
        }
        num_edge_ = static_cast<int>(total_entries);

        // --- Mapping for output CSV (reuse country_code schema) ---
        global_to_local_.assign(num_vertex_, {});
        aggregate_keys_.resize(num_vertex_);
        for (int i = 0; i < num_vertex_; ++i) {
            global_to_local_[i] = {static_cast<label_t>(-1),
                                   static_cast<vid_t>(i)};
            aggregate_keys_[i] = cc_list[i];
        }

        auto tend = std::chrono::steady_clock::now();
        LOG(INFO) << "[Louvain-infra] Built country graph: "
                  << num_vertex_ << " vertices, " << total_entries
                  << " directed adjacency entries, ds_edges_seen="
                  << ds_edges_seen << ", total="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(tend - tstart).count()
                  << "ms";
    }

    const StorageReadInterface& graph_;
    int max_iterations_;
    double resolution_;
    int max_levels_;

    // Coauthor-projection mode (Case A): off by default; set by the alternate ctor.
    bool coauthor_mode_ = false;
    CoauthorConfig coauthor_cfg_;

    // Infra-projection mode (infrastructure case): off by default; set by alternate ctor.
    bool infra_mode_ = false;
    InfraConfig infra_cfg_;

    int num_vertex_ = 0;
    int num_edge_ = 0;

    // Per-label starting global_id; -1 if label is not a valid vertex label.
    std::vector<int> label_offsets_;
    std::vector<std::pair<label_t, vid_t>> global_to_local_;
    std::vector<std::vector<std::pair<int, double>>> adj_;  // undirected weighted adjacency

    // Populated only in coauthor projection mode, parallel to global_to_local_.
    std::vector<std::string> aggregate_keys_;
};

// ============================================================================
// Helper: generate unique output file path
// ============================================================================

inline std::string GenerateLouvainOutputPath(const std::string& prefix) {
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::filesystem::create_directories("/tmp/p/neug_louvain");
    return "/tmp/p/neug_louvain/" + prefix + "_" + std::to_string(ts) + ".csv";
}

// ============================================================================
// Input struct: carries bind-time parameters to exec-time
// ============================================================================

struct LouvainInput : public CallFuncInputBase {
    int64_t max_iter;
    double resolution;
    int64_t max_levels;

    LouvainInput(int64_t m = 20, double r = 1.0, int64_t l = 10)
        : max_iter(m), resolution(r), max_levels(l) {}
    ~LouvainInput() override = default;
};

// ============================================================================
// LouvainFunction: CALL LOUVAIN([max_iterations, resolution, max_levels])
//
// Returns community detection results.
// All parameters are optional with sensible defaults.
// ============================================================================

struct LouvainFunction {
    static constexpr const char* name = "LOUVAIN";

    static function_set getFunctionSet() {
        function_set functionSet;

        call_output_columns outputCols{
            {"status", common::LogicalTypeID::STRING},
            {"num_vertices", common::LogicalTypeID::INT64},
            {"num_communities", common::LogicalTypeID::INT64},
            {"modularity", common::LogicalTypeID::DOUBLE},
            {"levels", common::LogicalTypeID::INT64},
            {"result_file", common::LogicalTypeID::STRING},
            {"props_dir", common::LogicalTypeID::STRING}
        };

        // ---- Overload 1: CALL LOUVAIN()  (all defaults) ----
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{},
                call_output_columns(outputCols));

            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan&, int)
                -> std::unique_ptr<CallFuncInputBase> {
                LOG(INFO) << "[LOUVAIN] Bind: no parameters (defaults)";
                return std::make_unique<LouvainInput>();
            };

            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph)
                -> execution::Context {
                return ExecuteLouvain(input, graph);
            };

            functionSet.push_back(std::move(func));
        }

        // ---- Overload 2: CALL LOUVAIN(max_iterations, resolution, max_levels) ----
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,
                    common::LogicalTypeID::DOUBLE,
                    common::LogicalTypeID::INT64},
                call_output_columns(outputCols));

            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t max_iter = 20;
                double resolution = 1.0;
                int64_t max_levels = 10;

                if (proc.query().arguments_size() >= 1 &&
                    proc.query().arguments(0).has_const_())
                    max_iter = proc.query().arguments(0).const_().i64();
                if (proc.query().arguments_size() >= 2 &&
                    proc.query().arguments(1).has_const_())
                    resolution = proc.query().arguments(1).const_().f64();
                if (proc.query().arguments_size() >= 3 &&
                    proc.query().arguments(2).has_const_())
                    max_levels = proc.query().arguments(2).const_().i64();

                LOG(INFO) << "[LOUVAIN] Bind: max_iter=" << max_iter
                          << " resolution=" << resolution
                          << " max_levels=" << max_levels;
                return std::make_unique<LouvainInput>(max_iter, resolution, max_levels);
            };

            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph)
                -> execution::Context {
                return ExecuteLouvain(input, graph);
            };

            functionSet.push_back(std::move(func));
        }

        return functionSet;
    }

private:
    static execution::Context ExecuteLouvain(const CallFuncInputBase& input,
                                             IStorageInterface& graph) {
        auto& lvInput = static_cast<const LouvainInput&>(input);

        auto* readInterface = dynamic_cast<StorageReadInterface*>(&graph);
        if (!readInterface) {
            LOG(ERROR) << "[LOUVAIN] graph is not a StorageReadInterface!";
            return execution::Context();
        }

        LOG(INFO) << "[LOUVAIN] Computing Louvain community detection...";

        LouvainComputer computer(
            *readInterface,
            static_cast<int>(lvInput.max_iter),
            lvInput.resolution,
            static_cast<int>(lvInput.max_levels));
        auto result = computer.Compute();

        // Write all vertex -> community assignments to CSV
        std::string outputFile = GenerateLouvainOutputPath("louvain");
        {
            std::ofstream ofs(outputFile);
            if (!ofs.is_open()) {
                LOG(ERROR) << "[LOUVAIN] Failed to open: " << outputFile;
                return execution::Context();
            }
            ofs << "global_id,vertex_label,original_vid,community_id\n";
            for (int i = 0; i < result.num_vertices; ++i) {
                auto [vlabel, vid] = result.id_mapping[i];
                ofs << i << "," << (int)vlabel << "," << vid
                    << "," << result.community[i] << "\n";
            }
            ofs.close();
        }

        LOG(INFO) << "[LOUVAIN] Results written to: " << outputFile;

        // Write per-label property CSV files (with community assignment)
        auto now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string propsDir = "/tmp/p/neug_louvain/props_" + std::to_string(now_ts);
        std::filesystem::create_directories(propsDir);
        {
            const auto& schema = readInterface->schema();

            struct LabelInfo {
                std::string label_name;
                std::vector<std::string> prop_names;
            };
            std::unordered_map<label_t, LabelInfo> label_info_map;

            // Group vertices by label
            std::unordered_map<label_t, std::vector<int>> label_to_vertices;
            for (int i = 0; i < result.num_vertices; ++i) {
                auto [vlabel, vid] = result.id_mapping[i];
                label_to_vertices[vlabel].push_back(i);
                if (label_info_map.find(vlabel) == label_info_map.end()) {
                    LabelInfo li;
                    li.label_name = schema.get_vertex_label_name(vlabel);
                    li.prop_names = schema.get_vertex_property_names(vlabel);
                    label_info_map[vlabel] = std::move(li);
                }
            }

            auto escape_csv = [](const std::string& val_str) -> std::string {
                if (val_str.find(',') != std::string::npos ||
                    val_str.find('"') != std::string::npos ||
                    val_str.find('\n') != std::string::npos) {
                    std::string escaped;
                    for (char c : val_str) {
                        if (c == '"') escaped += "\"\"";
                        else escaped += c;
                    }
                    return "\"" + escaped + "\"";
                }
                return val_str;
            };

            for (auto& [vlabel, vertex_ids] : label_to_vertices) {
                const auto& li = label_info_map[vlabel];
                std::string filepath = propsDir + "/" + li.label_name + ".csv";

                std::ofstream pofs(filepath);
                if (!pofs.is_open()) {
                    LOG(WARNING) << "[LOUVAIN] Failed to open: " << filepath;
                    continue;
                }

                pofs << "global_id,original_vid,community_id";
                for (const auto& pname : li.prop_names) {
                    pofs << "," << pname;
                }
                pofs << "\n";

                for (int gid : vertex_ids) {
                    auto [_, vid] = result.id_mapping[gid];
                    pofs << gid << "," << vid << "," << result.community[gid];

                    for (int pi = 0; pi < (int)li.prop_names.size(); ++pi) {
                        pofs << ",";
                        try {
                            Property prop = readInterface->GetVertexProperty(vlabel, vid, pi);
                            execution::Value val = execution::property_to_value(prop);
                            pofs << escape_csv(val.to_string());
                        } catch (...) {}
                    }
                    pofs << "\n";
                }
                pofs.close();
                LOG(INFO) << "[LOUVAIN] Properties for label '"
                          << li.label_name << "' written to: " << filepath;
            }
        }

        // Build output context (single row)
        execution::Context ctx;

        execution::ValueColumnBuilder<std::string> statusBuilder;
        statusBuilder.push_back_opt(std::string("success"));
        ctx.set(0, statusBuilder.finish());

        execution::ValueColumnBuilder<int64_t> verticesBuilder;
        verticesBuilder.push_back_opt(static_cast<int64_t>(result.num_vertices));
        ctx.set(1, verticesBuilder.finish());

        execution::ValueColumnBuilder<int64_t> commBuilder;
        commBuilder.push_back_opt(static_cast<int64_t>(result.num_communities));
        ctx.set(2, commBuilder.finish());

        execution::ValueColumnBuilder<double> modBuilder;
        modBuilder.push_back_opt(result.modularity);
        ctx.set(3, modBuilder.finish());

        execution::ValueColumnBuilder<int64_t> levelBuilder;
        levelBuilder.push_back_opt(static_cast<int64_t>(result.levels_completed));
        ctx.set(4, levelBuilder.finish());

        execution::ValueColumnBuilder<std::string> fileBuilder;
        fileBuilder.push_back_opt(outputFile);
        ctx.set(5, fileBuilder.finish());

        execution::ValueColumnBuilder<std::string> propsDirBuilder;
        propsDirBuilder.push_back_opt(propsDir);
        ctx.set(6, propsDirBuilder.finish());

        ctx.tag_ids = {0, 1, 2, 3, 4, 5, 6};
        return ctx;
    }
};

// ============================================================================
// LOUVAIN_COAUTHOR — Case A Level 1 / Level 3
// ============================================================================
//
// Builds the country-level co-author graph from
//   Publication —[hasAuthorInstitution]→ Organization
// within a given year range, then runs the existing parallel Louvain core.
//
// Grammar (two overloads):
//   CALL LOUVAIN_COAUTHOR(year_min, year_max)
//   CALL LOUVAIN_COAUTHOR(year_min, year_max,
//                         max_coauthors, max_iter, resolution, max_levels)
//
// Output columns match LOUVAIN except the result CSV schema differs:
//   global_id, country_code, community_id
// ============================================================================

struct LouvainCoauthorInput : public CallFuncInputBase {
    int64_t year_min;
    int64_t year_max;
    int64_t max_coauthors;
    int64_t max_iter;
    double  resolution;
    int64_t max_levels;

    LouvainCoauthorInput(int64_t ymin, int64_t ymax,
                         int64_t mc = 30, int64_t mi = 20,
                         double r = 1.0, int64_t ml = 10)
        : year_min(ymin), year_max(ymax),
          max_coauthors(mc), max_iter(mi),
          resolution(r), max_levels(ml) {}
    ~LouvainCoauthorInput() override = default;
};

struct LouvainCoauthorFunction {
    static constexpr const char* name = "LOUVAIN_COAUTHOR";

    static function_set getFunctionSet() {
        function_set functionSet;

        call_output_columns outputCols{
            {"status", common::LogicalTypeID::STRING},
            {"num_vertices", common::LogicalTypeID::INT64},
            {"num_communities", common::LogicalTypeID::INT64},
            {"modularity", common::LogicalTypeID::DOUBLE},
            {"levels", common::LogicalTypeID::INT64},
            {"result_file", common::LogicalTypeID::STRING}
        };

        // ---- Overload A: (year_min, year_max) ----
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
                if (proc.query().arguments_size() >= 1 &&
                    proc.query().arguments(0).has_const_())
                    ymin = proc.query().arguments(0).const_().i64();
                if (proc.query().arguments_size() >= 2 &&
                    proc.query().arguments(1).has_const_())
                    ymax = proc.query().arguments(1).const_().i64();
                LOG(INFO) << "[LOUVAIN_COAUTHOR] Bind (short): year_min="
                          << ymin << " year_max=" << ymax;
                return std::make_unique<LouvainCoauthorInput>(ymin, ymax);
            };

            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph)
                -> execution::Context {
                return ExecuteLouvainCoauthor(input, graph);
            };

            functionSet.push_back(std::move(func));
        }

        // ---- Overload B: full params ----
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,  // year_min
                    common::LogicalTypeID::INT64,  // year_max
                    common::LogicalTypeID::INT64,  // max_coauthors
                    common::LogicalTypeID::INT64,  // max_iter
                    common::LogicalTypeID::DOUBLE, // resolution
                    common::LogicalTypeID::INT64}, // max_levels
                call_output_columns(outputCols));

            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t ymin = 0, ymax = 0, mc = 30, mi = 20, ml = 10;
                double  r = 1.0;
                auto& args = proc.query();
                if (args.arguments_size() >= 1 && args.arguments(0).has_const_())
                    ymin = args.arguments(0).const_().i64();
                if (args.arguments_size() >= 2 && args.arguments(1).has_const_())
                    ymax = args.arguments(1).const_().i64();
                if (args.arguments_size() >= 3 && args.arguments(2).has_const_())
                    mc = args.arguments(2).const_().i64();
                if (args.arguments_size() >= 4 && args.arguments(3).has_const_())
                    mi = args.arguments(3).const_().i64();
                if (args.arguments_size() >= 5 && args.arguments(4).has_const_())
                    r  = args.arguments(4).const_().f64();
                if (args.arguments_size() >= 6 && args.arguments(5).has_const_())
                    ml = args.arguments(5).const_().i64();
                LOG(INFO) << "[LOUVAIN_COAUTHOR] Bind (full): year=[" << ymin
                          << "," << ymax << "] max_coauthors=" << mc
                          << " max_iter=" << mi << " resolution=" << r
                          << " max_levels=" << ml;
                return std::make_unique<LouvainCoauthorInput>(ymin, ymax, mc, mi, r, ml);
            };

            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph)
                -> execution::Context {
                return ExecuteLouvainCoauthor(input, graph);
            };

            functionSet.push_back(std::move(func));
        }

        return functionSet;
    }

private:
    static execution::Context ExecuteLouvainCoauthor(
            const CallFuncInputBase& input, IStorageInterface& graph) {
        auto& cInput = static_cast<const LouvainCoauthorInput&>(input);

        auto* readInterface = dynamic_cast<StorageReadInterface*>(&graph);
        if (!readInterface) {
            LOG(ERROR) << "[LOUVAIN_COAUTHOR] graph is not a StorageReadInterface!";
            return execution::Context();
        }

        LOG(INFO) << "[LOUVAIN_COAUTHOR] Running with year=["
                  << cInput.year_min << "," << cInput.year_max
                  << "] max_coauthors=" << cInput.max_coauthors;

        LouvainComputer::CoauthorConfig cfg;
        cfg.year_min = cInput.year_min;
        cfg.year_max = cInput.year_max;
        cfg.max_coauthors = static_cast<int>(cInput.max_coauthors);

        LouvainComputer computer(
            *readInterface, cfg,
            static_cast<int>(cInput.max_iter),
            cInput.resolution,
            static_cast<int>(cInput.max_levels));
        auto result = computer.Compute();

        // Write country-level community CSV.
        std::string outputFile = GenerateLouvainOutputPath("louvain_coauthor");
        {
            std::ofstream ofs(outputFile);
            if (!ofs.is_open()) {
                LOG(ERROR) << "[LOUVAIN_COAUTHOR] Failed to open: " << outputFile;
                return execution::Context();
            }
            ofs << "global_id,country_code,community_id\n";
            for (int i = 0; i < result.num_vertices; ++i) {
                const std::string& cc =
                    (i < (int)result.aggregate_keys.size())
                        ? result.aggregate_keys[i]
                        : std::string{};
                ofs << i << "," << cc << "," << result.community[i] << "\n";
            }
            ofs.close();
        }
        LOG(INFO) << "[LOUVAIN_COAUTHOR] Results written to: " << outputFile;

        // Build output context (single row, 6 columns).
        execution::Context ctx;

        execution::ValueColumnBuilder<std::string> statusBuilder;
        statusBuilder.push_back_opt(std::string("success"));
        ctx.set(0, statusBuilder.finish());

        execution::ValueColumnBuilder<int64_t> verticesBuilder;
        verticesBuilder.push_back_opt(static_cast<int64_t>(result.num_vertices));
        ctx.set(1, verticesBuilder.finish());

        execution::ValueColumnBuilder<int64_t> commBuilder;
        commBuilder.push_back_opt(static_cast<int64_t>(result.num_communities));
        ctx.set(2, commBuilder.finish());

        execution::ValueColumnBuilder<double> modBuilder;
        modBuilder.push_back_opt(result.modularity);
        ctx.set(3, modBuilder.finish());

        execution::ValueColumnBuilder<int64_t> levelBuilder;
        levelBuilder.push_back_opt(static_cast<int64_t>(result.levels_completed));
        ctx.set(4, levelBuilder.finish());

        execution::ValueColumnBuilder<std::string> fileBuilder;
        fileBuilder.push_back_opt(outputFile);
        ctx.set(5, fileBuilder.finish());

        ctx.tag_ids = {0, 1, 2, 3, 4, 5};
        return ctx;
    }
};

// ============================================================================
// LOUVAIN_INFRA_COUNTRY — Open-science infrastructure case
// ============================================================================
//
// Builds the country-level infrastructure graph from
//   Datasource —[hosts]→ Publication —[hasAuthorInstitution]→ Organization
// by (1) tagging each Publication within [year_min, year_max] with the set
// of its distinct author-institution country codes, (2) for each Datasource d
// aggregating host_count[d][c] = # pubs hosted by d that have an author from
// country c, and (3) projecting to Country×Country via min-coupling:
//   w(c1, c2) = Σ_d min(host_count[d][c1], host_count[d][c2])
//
// Story: if open-science infrastructure (arXiv, Zenodo, HAL, CNKI, elibrary.ru,
// etc.) is multipolar the Louvain partition should mirror LOUVAIN_COAUTHOR;
// if infrastructure is single-pole / Western-centric the partition should
// collapse most countries into one mega-community.
//
// Grammar:
//   CALL LOUVAIN_INFRA_COUNTRY(year_min, year_max)
//   CALL LOUVAIN_INFRA_COUNTRY(year_min, year_max,
//                              min_hosts_per_datasource, max_author_orgs,
//                              max_iter, resolution, max_levels)
//
// Output columns match LOUVAIN_COAUTHOR; result CSV schema is identical:
//   global_id, country_code, community_id
// ============================================================================

struct LouvainInfraCountryInput : public CallFuncInputBase {
    int64_t year_min;
    int64_t year_max;
    int64_t min_hosts_per_datasource;
    int64_t max_author_orgs;
    int64_t max_iter;
    double  resolution;
    int64_t max_levels;

    LouvainInfraCountryInput(int64_t ymin, int64_t ymax,
                             int64_t mh = 0, int64_t mo = 30,
                             int64_t mi = 20, double r = 1.0,
                             int64_t ml = 10)
        : year_min(ymin), year_max(ymax),
          min_hosts_per_datasource(mh), max_author_orgs(mo),
          max_iter(mi), resolution(r), max_levels(ml) {}
    ~LouvainInfraCountryInput() override = default;
};

struct LouvainInfraCountryFunction {
    static constexpr const char* name = "LOUVAIN_INFRA_COUNTRY";

    static function_set getFunctionSet() {
        function_set functionSet;

        call_output_columns outputCols{
            {"status", common::LogicalTypeID::STRING},
            {"num_vertices", common::LogicalTypeID::INT64},
            {"num_communities", common::LogicalTypeID::INT64},
            {"modularity", common::LogicalTypeID::DOUBLE},
            {"levels", common::LogicalTypeID::INT64},
            {"result_file", common::LogicalTypeID::STRING}
        };

        // ---- Overload A: (year_min, year_max) ----
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
                if (proc.query().arguments_size() >= 1 &&
                    proc.query().arguments(0).has_const_())
                    ymin = proc.query().arguments(0).const_().i64();
                if (proc.query().arguments_size() >= 2 &&
                    proc.query().arguments(1).has_const_())
                    ymax = proc.query().arguments(1).const_().i64();
                LOG(INFO) << "[LOUVAIN_INFRA_COUNTRY] Bind (short): year_min="
                          << ymin << " year_max=" << ymax;
                return std::make_unique<LouvainInfraCountryInput>(ymin, ymax);
            };

            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph)
                -> execution::Context {
                return ExecuteLouvainInfraCountry(input, graph);
            };

            functionSet.push_back(std::move(func));
        }

        // ---- Overload B: full params ----
        {
            auto func = std::make_unique<NeugCallFunction>(
                name,
                std::vector<common::LogicalTypeID>{
                    common::LogicalTypeID::INT64,  // year_min
                    common::LogicalTypeID::INT64,  // year_max
                    common::LogicalTypeID::INT64,  // min_hosts_per_datasource
                    common::LogicalTypeID::INT64,  // max_author_orgs
                    common::LogicalTypeID::INT64,  // max_iter
                    common::LogicalTypeID::DOUBLE, // resolution
                    common::LogicalTypeID::INT64}, // max_levels
                call_output_columns(outputCols));

            func->bindFunc = [](const Schema&, const execution::ContextMeta&,
                                const ::physical::PhysicalPlan& plan, int op_idx)
                -> std::unique_ptr<CallFuncInputBase> {
                auto& proc = plan.plan(op_idx).opr().procedure_call();
                int64_t ymin = 0, ymax = 0, mh = 0, mo = 30, mi = 20, ml = 10;
                double  r = 1.0;
                auto& args = proc.query();
                if (args.arguments_size() >= 1 && args.arguments(0).has_const_())
                    ymin = args.arguments(0).const_().i64();
                if (args.arguments_size() >= 2 && args.arguments(1).has_const_())
                    ymax = args.arguments(1).const_().i64();
                if (args.arguments_size() >= 3 && args.arguments(2).has_const_())
                    mh = args.arguments(2).const_().i64();
                if (args.arguments_size() >= 4 && args.arguments(3).has_const_())
                    mo = args.arguments(3).const_().i64();
                if (args.arguments_size() >= 5 && args.arguments(4).has_const_())
                    mi = args.arguments(4).const_().i64();
                if (args.arguments_size() >= 6 && args.arguments(5).has_const_())
                    r  = args.arguments(5).const_().f64();
                if (args.arguments_size() >= 7 && args.arguments(6).has_const_())
                    ml = args.arguments(6).const_().i64();
                LOG(INFO) << "[LOUVAIN_INFRA_COUNTRY] Bind (full): year=[" << ymin
                          << "," << ymax << "] min_hosts=" << mh
                          << " max_author_orgs=" << mo
                          << " max_iter=" << mi << " resolution=" << r
                          << " max_levels=" << ml;
                return std::make_unique<LouvainInfraCountryInput>(
                    ymin, ymax, mh, mo, mi, r, ml);
            };

            func->execFunc = [](const CallFuncInputBase& input,
                                IStorageInterface& graph)
                -> execution::Context {
                return ExecuteLouvainInfraCountry(input, graph);
            };

            functionSet.push_back(std::move(func));
        }

        return functionSet;
    }

private:
    static execution::Context ExecuteLouvainInfraCountry(
            const CallFuncInputBase& input, IStorageInterface& graph) {
        auto& cInput = static_cast<const LouvainInfraCountryInput&>(input);

        auto* readInterface = dynamic_cast<StorageReadInterface*>(&graph);
        if (!readInterface) {
            LOG(ERROR) << "[LOUVAIN_INFRA_COUNTRY] graph is not a StorageReadInterface!";
            return execution::Context();
        }

        LOG(INFO) << "[LOUVAIN_INFRA_COUNTRY] Running with year=["
                  << cInput.year_min << "," << cInput.year_max
                  << "] min_hosts=" << cInput.min_hosts_per_datasource
                  << " max_author_orgs=" << cInput.max_author_orgs;

        LouvainComputer::InfraConfig cfg;
        cfg.year_min = cInput.year_min;
        cfg.year_max = cInput.year_max;
        cfg.min_hosts_per_datasource =
            static_cast<int>(cInput.min_hosts_per_datasource);
        cfg.max_author_orgs = static_cast<int>(cInput.max_author_orgs);

        LouvainComputer computer(
            *readInterface, cfg,
            static_cast<int>(cInput.max_iter),
            cInput.resolution,
            static_cast<int>(cInput.max_levels));
        auto result = computer.Compute();

        std::string outputFile =
            GenerateLouvainOutputPath("louvain_infra_country");
        {
            std::ofstream ofs(outputFile);
            if (!ofs.is_open()) {
                LOG(ERROR) << "[LOUVAIN_INFRA_COUNTRY] Failed to open: "
                           << outputFile;
                return execution::Context();
            }
            ofs << "global_id,country_code,community_id\n";
            for (int i = 0; i < result.num_vertices; ++i) {
                const std::string& cc =
                    (i < (int)result.aggregate_keys.size())
                        ? result.aggregate_keys[i]
                        : std::string{};
                ofs << i << "," << cc << "," << result.community[i] << "\n";
            }
            ofs.close();
        }
        LOG(INFO) << "[LOUVAIN_INFRA_COUNTRY] Results written to: " << outputFile;

        execution::Context ctx;

        execution::ValueColumnBuilder<std::string> statusBuilder;
        statusBuilder.push_back_opt(std::string("success"));
        ctx.set(0, statusBuilder.finish());

        execution::ValueColumnBuilder<int64_t> verticesBuilder;
        verticesBuilder.push_back_opt(static_cast<int64_t>(result.num_vertices));
        ctx.set(1, verticesBuilder.finish());

        execution::ValueColumnBuilder<int64_t> commBuilder;
        commBuilder.push_back_opt(static_cast<int64_t>(result.num_communities));
        ctx.set(2, commBuilder.finish());

        execution::ValueColumnBuilder<double> modBuilder;
        modBuilder.push_back_opt(result.modularity);
        ctx.set(3, modBuilder.finish());

        execution::ValueColumnBuilder<int64_t> levelBuilder;
        levelBuilder.push_back_opt(static_cast<int64_t>(result.levels_completed));
        ctx.set(4, levelBuilder.finish());

        execution::ValueColumnBuilder<std::string> fileBuilder;
        fileBuilder.push_back_opt(outputFile);
        ctx.set(5, fileBuilder.finish());

        ctx.tag_ids = {0, 1, 2, 3, 4, 5};
        return ctx;
    }
};

}  // namespace function
}  // namespace neug
