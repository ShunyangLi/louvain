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
#include <neug/compiler/common/file_system/virtual_file_system.h>
#include <neug/compiler/gopt/g_vfs_holder.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <sstream>
#include <unordered_set>
#include <unistd.h>

static std::string LDBC_DATA_PATH;

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

bool executeQuery(neug::Connection* conn, const std::string& query,
                  const std::string& description = "") {
    if (!description.empty()) {
        std::cout << "  " << description << std::endl;
    }
    auto res = conn->Query(query);
    if (!res.has_value()) {
        std::cerr << "Query failed: " << res.error().ToString() << std::endl;
        std::cerr << "Query was: " << query << std::endl;
        return false;
    }
    return true;
}

bool createSchema(neug::Connection* conn) {
    std::cout << "\n=== Creating Schema ===" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Person(
            id INT64 PRIMARY KEY, label STRING,
            firstName STRING, lastName STRING, gender STRING,
            birthday STRING, creationDate STRING, locationIP STRING,
            browserUsed STRING, language STRING, email STRING
        )
    )", "Creating Person node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Comment(
            id INT64 PRIMARY KEY, label STRING,
            creationDate STRING, locationIP STRING,
            browserUsed STRING, content STRING, length INT32
        )
    )", "Creating Comment node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Post(
            id INT64 PRIMARY KEY, label STRING,
            imageFile STRING, creationDate STRING, locationIP STRING,
            browserUsed STRING, language STRING, content STRING, length INT32
        )
    )", "Creating Post node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Forum(
            id INT64 PRIMARY KEY, label STRING,
            title STRING, creationDate STRING
        )
    )", "Creating Forum node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Tag(
            id INT64 PRIMARY KEY, label STRING, name STRING, url STRING
        )
    )", "Creating Tag node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE TagClass(
            id INT64 PRIMARY KEY, label STRING, name STRING, url STRING
        )
    )", "Creating TagClass node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Place(
            id INT64 PRIMARY KEY, label STRING,
            name STRING, url STRING, type STRING
        )
    )", "Creating Place node table")) return false;

    if (!executeQuery(conn, R"(
        CREATE NODE TABLE Organisation(
            id INT64 PRIMARY KEY, label STRING,
            type STRING, name STRING, url STRING
        )
    )", "Creating Organisation node table")) return false;

    if (!executeQuery(conn, R"(CREATE REL TABLE person_knows_person(FROM Person TO Person, creationDate STRING))", "Creating person_knows_person")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE comment_hasCreator_person(FROM Comment TO Person))", "Creating comment_hasCreator_person")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE post_hasCreator_person(FROM Post TO Person))", "Creating post_hasCreator_person")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE forum_hasMember_person(FROM Forum TO Person, joinDate STRING))", "Creating forum_hasMember_person")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE forum_hasModerator_person(FROM Forum TO Person))", "Creating forum_hasModerator_person")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE forum_containerOf_post(FROM Forum TO Post))", "Creating forum_containerOf_post")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE comment_replyOf_post(FROM Comment TO Post))", "Creating comment_replyOf_post")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE comment_replyOf_comment(FROM Comment TO Comment))", "Creating comment_replyOf_comment")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE person_likes_post(FROM Person TO Post, creationDate STRING))", "Creating person_likes_post")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE person_likes_comment(FROM Person TO Comment, creationDate STRING))", "Creating person_likes_comment")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE person_hasInterest_tag(FROM Person TO Tag))", "Creating person_hasInterest_tag")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE person_isLocatedIn_place(FROM Person TO Place))", "Creating person_isLocatedIn_place")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE person_studyAt_organisation(FROM Person TO Organisation, classYear INT32))", "Creating person_studyAt_organisation")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE person_workAt_organisation(FROM Person TO Organisation, workFrom INT32))", "Creating person_workAt_organisation")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE post_hasTag_tag(FROM Post TO Tag))", "Creating post_hasTag_tag")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE comment_hasTag_tag(FROM Comment TO Tag))", "Creating comment_hasTag_tag")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE forum_hasTag_tag(FROM Forum TO Tag))", "Creating forum_hasTag_tag")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE post_isLocatedIn_place(FROM Post TO Place))", "Creating post_isLocatedIn_place")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE comment_isLocatedIn_place(FROM Comment TO Place))", "Creating comment_isLocatedIn_place")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE organisation_isLocatedIn_place(FROM Organisation TO Place))", "Creating organisation_isLocatedIn_place")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE place_isPartOf_place(FROM Place TO Place))", "Creating place_isPartOf_place")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE tag_hasType_tagclass(FROM Tag TO TagClass))", "Creating tag_hasType_tagclass")) return false;
    if (!executeQuery(conn, R"(CREATE REL TABLE tagclass_isSubclassOf_tagclass(FROM TagClass TO TagClass))", "Creating tagclass_isSubclassOf_tagclass")) return false;

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Schema created successfully in " << ms << "ms!" << std::endl;
    return true;
}

bool loadData(neug::Connection* conn) {
    std::cout << "\n=== Loading Data from CSV Files ===" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    auto loadCSV = [&](const std::string& table, const std::string& file,
                       const std::string& desc) -> bool {
        return executeQuery(conn,
            "COPY " + table + " FROM '" + LDBC_DATA_PATH + file +
            "' (HEADER=true, DELIMITER=',')", desc);
    };

    std::cout << "\n  Loading vertex data..." << std::endl;
    if (!loadCSV("Person", "person.csv", "Loading Person nodes")) return false;
    if (!loadCSV("Comment", "comment.csv", "Loading Comment nodes")) return false;
    if (!loadCSV("Post", "post.csv", "Loading Post nodes")) return false;
    if (!loadCSV("Forum", "forum.csv", "Loading Forum nodes")) return false;
    if (!loadCSV("Tag", "tag.csv", "Loading Tag nodes")) return false;
    if (!loadCSV("TagClass", "tagclass.csv", "Loading TagClass nodes")) return false;
    if (!loadCSV("Place", "place.csv", "Loading Place nodes")) return false;
    if (!loadCSV("Organisation", "organisation.csv", "Loading Organisation nodes")) return false;

    std::cout << "\n  Loading edge data..." << std::endl;
    if (!loadCSV("person_knows_person", "person_knows_person.csv", "Loading person_knows_person")) return false;
    if (!loadCSV("comment_hasCreator_person", "comment_hasCreator_person.csv", "Loading comment_hasCreator_person")) return false;
    if (!loadCSV("post_hasCreator_person", "post_hasCreator_person.csv", "Loading post_hasCreator_person")) return false;
    if (!loadCSV("forum_hasMember_person", "forum_hasMember_person.csv", "Loading forum_hasMember_person")) return false;
    if (!loadCSV("forum_hasModerator_person", "forum_hasModerator_person.csv", "Loading forum_hasModerator_person")) return false;
    if (!loadCSV("forum_containerOf_post", "forum_containerOf_post.csv", "Loading forum_containerOf_post")) return false;
    if (!loadCSV("comment_replyOf_post", "comment_replyOf_post.csv", "Loading comment_replyOf_post")) return false;
    if (!loadCSV("comment_replyOf_comment", "comment_replyOf_comment.csv", "Loading comment_replyOf_comment")) return false;
    if (!loadCSV("person_likes_post", "person_likes_post.csv", "Loading person_likes_post")) return false;
    if (!loadCSV("person_likes_comment", "person_likes_comment.csv", "Loading person_likes_comment")) return false;
    if (!loadCSV("person_hasInterest_tag", "person_hasInterest_tag.csv", "Loading person_hasInterest_tag")) return false;
    if (!loadCSV("person_isLocatedIn_place", "person_isLocatedIn_place.csv", "Loading person_isLocatedIn_place")) return false;
    if (!loadCSV("person_studyAt_organisation", "person_studyAt_organisation.csv", "Loading person_studyAt_organisation")) return false;
    if (!loadCSV("person_workAt_organisation", "person_workAt_organisation.csv", "Loading person_workAt_organisation")) return false;
    if (!loadCSV("post_hasTag_tag", "post_hasTag_tag.csv", "Loading post_hasTag_tag")) return false;
    if (!loadCSV("comment_hasTag_tag", "comment_hasTag_tag.csv", "Loading comment_hasTag_tag")) return false;
    if (!loadCSV("forum_hasTag_tag", "forum_hasTag_tag.csv", "Loading forum_hasTag_tag")) return false;
    if (!loadCSV("post_isLocatedIn_place", "post_isLocatedIn_place.csv", "Loading post_isLocatedIn_place")) return false;
    if (!loadCSV("comment_isLocatedIn_place", "comment_isLocatedIn_place.csv", "Loading comment_isLocatedIn_place")) return false;
    if (!loadCSV("organisation_isLocatedIn_place", "organisation_isLocatedIn_place.csv", "Loading organisation_isLocatedIn_place")) return false;
    if (!loadCSV("place_isPartOf_place", "place_isPartOf_place.csv", "Loading place_isPartOf_place")) return false;
    if (!loadCSV("tag_hasType_tagclass", "tag_hasType_tagclass.csv", "Loading tag_hasType_tagclass")) return false;
    if (!loadCSV("tagclass_isSubclassOf_tagclass", "tagclass_isSubclassOf_tagclass.csv", "Loading tagclass_isSubclassOf_tagclass")) return false;

    auto end = std::chrono::high_resolution_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
    std::cout << "\nData loaded successfully in " << secs << "s!" << std::endl;
    return true;
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

    auto res = conn->Query("LOAD louvain;");
    if (!res.has_value()) {
        std::cerr << "Failed to load louvain extension: "
                  << res.error().ToString() << std::endl;
        return false;
    }
    std::cout << "Louvain extension loaded successfully" << std::endl;
    return true;
}

bool validateResultCSV(const std::string& filepath) {
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

    int row_count = 0;
    std::unordered_set<int> communities;
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
        communities.insert(std::stoi(comm_str));
    }

    std::cout << "  CSV validation passed: " << row_count << " rows, "
              << communities.size() << " communities" << std::endl;
    return row_count > 0;
}

int main(int argc, char* argv[]) {
    std::cout << "============================================" << std::endl;
    std::cout << "  NeuG Louvain Extension Test (LDBC SF01)" << std::endl;
    std::cout << "============================================" << std::endl;

    if (argc > 1 && std::string(argv[1]) != "--no-reload") {
        LDBC_DATA_PATH = argv[1];
    } else {
        const char* env_path = std::getenv("LDBC_DATA_PATH");
        if (env_path) {
            LDBC_DATA_PATH = env_path;
        } else {
            std::cerr << "Usage: " << argv[0]
                      << " <ldbc_data_path> [--no-reload]" << std::endl;
            std::cerr << "  Or set LDBC_DATA_PATH environment variable."
                      << std::endl;
            return 1;
        }
    }
    if (LDBC_DATA_PATH.back() != '/') LDBC_DATA_PATH += '/';

    bool reload_data = true;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--no-reload") {
            reload_data = false;
            std::cout << "Using existing database (--no-reload)" << std::endl;
        }
    }

    std::cout << "LDBC data path: " << LDBC_DATA_PATH << std::endl;

    auto vfs = std::make_unique<neug::common::VirtualFileSystem>();
    neug::common::VFSHolder::setVFS(vfs.get());

    std::string db_path = "/tmp/neug_louvain_ldbc_db";

    if (reload_data && std::filesystem::exists(db_path)) {
        std::cout << "Removing existing database: " << db_path << std::endl;
        std::filesystem::remove_all(db_path);
    }

    std::cout << "\n=== Opening Database ===" << std::endl;
    neug::NeugDB db;
    if (!db.Open(db_path)) {
        std::cerr << "Failed to open database: " << db_path << std::endl;
        return 1;
    }
    auto conn = db.Connect();
    if (!conn) {
        std::cerr << "Failed to connect to database" << std::endl;
        return 1;
    }
    std::cout << "Database opened and connected" << std::endl;

    if (reload_data) {
        if (!createSchema(conn.get())) { conn.reset(); db.Close(); return 1; }
        if (!loadData(conn.get())) { conn.reset(); db.Close(); return 1; }
    }

    if (!loadExtension(conn.get())) { conn.reset(); db.Close(); return 1; }

    {
        auto show_res = conn->Query("CALL show_loaded_extensions() RETURN *;");
        if (show_res.has_value()) {
            std::cout << "\nLoaded extensions:" << std::endl;
            auto& resp = show_res.value().response();
            if (resp.arrays_size() >= 2) {
                const auto& name_arr = resp.arrays(0).string_array();
                const auto& desc_arr = resp.arrays(1).string_array();
                for (int i = 0; i < name_arr.values_size(); i++) {
                    std::cout << "  - " << name_arr.values(i) << ": "
                              << desc_arr.values(i) << std::endl;
                }
            }
        }
    }

    int failures = 0;

    // Test 1: CALL LOUVAIN() with defaults
    {
        std::cout << "\n=== Test 1: CALL LOUVAIN() (defaults) ===" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();

        auto res = conn->Query("CALL LOUVAIN() RETURN *;");
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        if (!res.has_value()) {
            std::cerr << "LOUVAIN() failed: " << res.error().ToString() << std::endl;
            failures++;
        } else {
            auto& rs = res.value();
            std::cout << rs.ToString() << std::endl;
            std::cout << "  Execution time: " << ms << "ms" << std::endl;

            auto& resp = rs.response();
            if (resp.row_count() == 1) {
                std::string status = resp.arrays(0).string_array().values(0);
                int64_t num_v = resp.arrays(1).int64_array().values(0);
                int64_t num_c = resp.arrays(2).int64_array().values(0);
                double modularity = resp.arrays(3).double_array().values(0);
                std::string result_file = resp.arrays(5).string_array().values(0);

                if (status != "success") {
                    std::cerr << "  FAILED: status=" << status << std::endl;
                    failures++;
                } else if (num_v <= 0) {
                    std::cerr << "  FAILED: num_vertices=" << num_v << std::endl;
                    failures++;
                } else if (!validateResultCSV(result_file)) {
                    failures++;
                } else {
                    std::cout << "  PASSED (vertices=" << num_v
                              << ", communities=" << num_c
                              << ", modularity=" << modularity << ")"
                              << std::endl;
                }
            } else {
                std::cerr << "  FAILED: expected 1 result row" << std::endl;
                failures++;
            }
        }
    }

    // Test 2: CALL LOUVAIN(30, 1.0, 15) - custom params
    {
        std::cout << "\n=== Test 2: CALL LOUVAIN(30, 1.0, 15) ===" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();

        auto res = conn->Query("CALL LOUVAIN(30, 1.0, 15) RETURN *;");
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        if (!res.has_value()) {
            std::cerr << "LOUVAIN(30,1.0,15) failed: " << res.error().ToString() << std::endl;
            failures++;
        } else {
            auto& rs = res.value();
            std::cout << rs.ToString() << std::endl;
            std::cout << "  Execution time: " << ms << "ms" << std::endl;

            auto& resp = rs.response();
            if (resp.row_count() == 1) {
                std::string result_file = resp.arrays(5).string_array().values(0);
                if (!validateResultCSV(result_file)) {
                    failures++;
                } else {
                    std::cout << "  PASSED" << std::endl;
                }
            } else {
                std::cerr << "  FAILED: expected 1 result row" << std::endl;
                failures++;
            }
        }
    }

    // Test 3: CALL LOUVAIN(20, 2.0, 10) - high resolution (more communities)
    {
        std::cout << "\n=== Test 3: CALL LOUVAIN(20, 2.0, 10) (high resolution) ===" << std::endl;
        auto start = std::chrono::high_resolution_clock::now();

        auto res = conn->Query("CALL LOUVAIN(20, 2.0, 10) RETURN *;");
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        if (!res.has_value()) {
            std::cerr << "LOUVAIN(20,2.0,10) failed: " << res.error().ToString() << std::endl;
            failures++;
        } else {
            auto& rs = res.value();
            std::cout << rs.ToString() << std::endl;
            std::cout << "  Execution time: " << ms << "ms" << std::endl;

            auto& resp = rs.response();
            if (resp.row_count() == 1) {
                int64_t num_c = resp.arrays(2).int64_array().values(0);
                std::string result_file = resp.arrays(5).string_array().values(0);
                if (!validateResultCSV(result_file)) {
                    failures++;
                } else {
                    std::cout << "  PASSED (communities=" << num_c << ")"
                              << std::endl;
                }
            } else {
                std::cerr << "  FAILED: expected 1 result row" << std::endl;
                failures++;
            }
        }
    }

    std::cout << "\n============================================" << std::endl;
    if (failures == 0) {
        std::cout << "  All LDBC Louvain tests PASSED!" << std::endl;
    } else {
        std::cout << "  " << failures << " test(s) FAILED!" << std::endl;
    }
    std::cout << "============================================" << std::endl;

    conn.reset();
    db.Close();
    return failures;
}
