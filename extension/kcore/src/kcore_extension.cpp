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

#include "kcore_functions.h"

extern "C" {

void Init() {
    std::cout << "[kcore extension] init called" << std::endl;

    try {
        neug::extension::ExtensionAPI::registerFunction<
            neug::function::KcoreFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerFunction<
            neug::function::KcoreCoauthorFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerFunction<
            neug::function::KcoreProjectFunction>(
            neug::catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY);

        neug::extension::ExtensionAPI::registerExtension(
            neug::extension::ExtensionInfo{
                "kcore",
                "Provides k-core decomposition. "
                "Functions: "
                "CALL KCORE() - run on the full underlying graph (all labels). "
                "CALL KCORE_COAUTHOR(year_min, year_max) - country-level k-core on "
                "Publication->Organization co-author projection within a year range. "
                "CALL KCORE_COAUTHOR(year_min, year_max, max_coauthors, min_edge_weight) - "
                "full control. "
                "CALL KCORE_PROJECT(year_min, year_max) - organization-level k-core on "
                "Project participation projection within a startDate year range. "
                "CALL KCORE_PROJECT(year_min, year_max, min_org_projects, min_edge_weight) - "
                "full control. "
                "Returns status, num_vertices, max_coreness, num_in_max_core, "
                "and result_file (CSV with per-vertex coreness)."
            });

        std::cout << "[kcore extension] functions registered successfully"
                  << std::endl;
    } catch (const std::exception& e) {
        THROW_EXCEPTION_WITH_FILE_LINE(
            "[kcore extension] registration failed: " + std::string(e.what()));
    } catch (...) {
        THROW_EXCEPTION_WITH_FILE_LINE(
            "[kcore extension] registration failed: unknown exception");
    }
}

const char* Name() { return "KCORE"; }

}  // extern "C"
