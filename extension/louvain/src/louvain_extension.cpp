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

#include "neug/compiler/extension/extension_api.h"
#include "neug/utils/exception/exception.h"

#include "louvain_functions.h"

extern "C" {

void Init() {
    std::cout << "[louvain extension] init called" << std::endl;

    try {
        neug::extension::ExtensionAPI::registerFunction<
            neug::function::LouvainFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerFunction<
            neug::function::LouvainCoauthorFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerFunction<
            neug::function::LouvainInfraCountryFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerFunction<
            neug::function::LouvainDatasourceFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerExtension(
            neug::extension::ExtensionInfo{
                "louvain",
                "Provides Louvain community detection algorithm. "
                "Functions: "
                "CALL LOUVAIN() - run with defaults (max_iter=20, resolution=1.0, max_levels=10), "
                "CALL LOUVAIN(max_iterations, resolution, max_levels) - full control. "
                "CALL LOUVAIN_COAUTHOR(year_min, year_max) - country-level Louvain on "
                "Publication→Organization co-author projection within a year range. "
                "CALL LOUVAIN_COAUTHOR(year_min, year_max, max_coauthors, max_iter, "
                "resolution, max_levels) - full control. "
                "CALL LOUVAIN_INFRA_COUNTRY(year_min, year_max) - country-level Louvain on "
                "Datasource→Publication→Organization infrastructure projection "
                "(min-coupling over shared datasources) within a year range. "
                "CALL LOUVAIN_INFRA_COUNTRY(year_min, year_max, min_hosts_per_datasource, "
                "max_author_orgs, max_iter, resolution, max_levels) - full control. "
                "CALL LOUVAIN_DATASOURCE(year_min, year_max) - datasource-level Louvain on "
                "the Datasource×Datasource projection (edge weight = # in-window pubs "
                "co-hosted), with each datasource annotated by provider org + country. "
                "CALL LOUVAIN_DATASOURCE(year_min, year_max, min_pubs, max_iter, "
                "resolution, max_levels) - full control. "
                "Returns status, num_vertices, num_communities, modularity, levels, "
                "and result_file."
            });

        std::cout << "[louvain extension] functions registered successfully"
                  << std::endl;
    } catch (const std::exception& e) {
        THROW_EXCEPTION_WITH_FILE_LINE(
            "[louvain extension] registration failed: " + std::string(e.what()));
    } catch (...) {
        THROW_EXCEPTION_WITH_FILE_LINE(
            "[louvain extension] registration failed: unknown exception");
    }
}

const char* Name() { return "LOUVAIN"; }

}  // extern "C"
