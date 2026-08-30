// Contract tests for the web viewer JSON API.  The API is the stable
// interface for any external frontend (docs/web-api.md), so these
// tests pin its shape.

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "database/database.h"
#include "server/http.h"
#include "server/viewer.h"

namespace offcat {
namespace {

std::string temp_db_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

class ViewerApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = temp_db_path("offcat_test_viewer.db");
        std::filesystem::remove(path_);
        ASSERT_TRUE(is_ok(db_.create(path_)));
        ASSERT_TRUE(is_ok(db_.execute(
            "INSERT INTO source (id, name, type, source_path) VALUES"
            " (1, 'SRC', 'Directory', 'C:\\data\\');"
            "INSERT INTO entry (id, source_id, parent_id, name, type, size)"
            " VALUES"
            " (1, 1, NULL, 'root.txt', 1, 1024),"
            " (2, 1, 1, 'sub.txt', 1, 512);"
            "INSERT INTO scan (id, source_id, started_at, status)"
            " VALUES (1, 1, 0, 2);"
            "INSERT INTO entry_fts(rowid, name, path, source_name) VALUES"
            " (1, 'root.txt', 'root.txt', 'SRC'),"
            " (2, 'sub.txt', 'root.txt/sub.txt', 'SRC');")));
    }

    void TearDown() override {
        db_.close();
        std::filesystem::remove(path_);
        std::filesystem::remove(path_ + "-wal");
        std::filesystem::remove(path_ + "-shm");
    }

    std::string get(Viewer& v, const std::string& path,
                    const std::string& query = "") {
        HttpRequest req{path, query};
        return v.handle(req);
    }

    std::string path_;
    Database db_;
};

TEST_F(ViewerApiTest, Stats) {
    Viewer v(db_, "");
    EXPECT_EQ(get(v, "/api/stats"), "{\"sources\":1,\"entries\":2,\"containers\":0}");
}

TEST_F(ViewerApiTest, Sources) {
    Viewer v(db_, "");
    std::string j = get(v, "/api/sources");
    EXPECT_NE(j.find("\"id\":1"), std::string::npos);
    EXPECT_NE(j.find("\"name\":\"SRC\""), std::string::npos);
    EXPECT_NE(j.find("\"path\":\"C:\\\\data\\\\\""), std::string::npos);
    EXPECT_NE(j.find("\"entries\":2"), std::string::npos);
}

TEST_F(ViewerApiTest, TreeRoot) {
    Viewer v(db_, "");
    std::string j = get(v, "/api/tree", "parent_id=0&source_id=1");
    // Root entries sorted: root.txt (file) only at top level.
    EXPECT_NE(j.find("\"name\":\"root.txt\""), std::string::npos);
    EXPECT_NE(j.find("\"is_dir\":false"), std::string::npos);
}

TEST_F(ViewerApiTest, TreeChildren) {
    Viewer v(db_, "");
    std::string j = get(v, "/api/tree", "parent_id=1");
    EXPECT_NE(j.find("\"name\":\"sub.txt\""), std::string::npos);
}

TEST_F(ViewerApiTest, EntryDetail) {
    Viewer v(db_, "");
    std::string j = get(v, "/api/entry", "id=1");
    EXPECT_NE(j.find("\"path\":\"root.txt\""), std::string::npos);
    EXPECT_NE(j.find("\"size\":1024"), std::string::npos);
    EXPECT_NE(j.find("\"container\":null"), std::string::npos);
    EXPECT_NE(j.find("\"checksums\":[]"), std::string::npos);
}

TEST_F(ViewerApiTest, Search) {
    Viewer v(db_, "");
    std::string j = get(v, "/api/search", "q=sub&limit=10");
    EXPECT_NE(j.find("\"entry_name\":\"sub.txt\""), std::string::npos);
    EXPECT_NE(j.find("\"full_path\":\"root.txt/sub.txt\""), std::string::npos);
}

TEST_F(ViewerApiTest, NotFound) {
    Viewer v(db_, "");
    EXPECT_EQ(get(v, "/api/nope"), "");
    EXPECT_EQ(get(v, "/missing"), "");
}

TEST_F(ViewerApiTest, EmbeddedIndex) {
    Viewer v(db_, "");
    std::string j = get(v, "/");
    EXPECT_NE(j.find("Offcat Catalog Viewer"), std::string::npos);
    // The raw HTML is served as-is (not JSON-escaped).
    EXPECT_NE(j.find("<script>"), std::string::npos);
}

TEST_F(ViewerApiTest, ExternalWebRootWins) {
    std::string root = (std::filesystem::temp_directory_path() /
                        "offcat_test_webroot").string();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream f((std::filesystem::path(root) / "index.html"));
        f << "<html>external frontend</html>";
    }
    {
        std::ofstream f((std::filesystem::path(root) / "app.js"));
        f << "console.log('vvv');";
    }

    Viewer v(db_, root);
    std::string idx = get(v, "/");
    EXPECT_EQ(idx, "<html>external frontend</html>");
    std::string js = get(v, "/app.js");
    EXPECT_EQ(js, "console.log('vvv');");
    EXPECT_EQ(v.content_type_for("/app.js"), "text/javascript");

    // Path traversal must not escape the web root.
    EXPECT_EQ(get(v, "/../secret.txt"), "");
    EXPECT_EQ(get(v, "/a/../../secret.txt"), "");

    std::filesystem::remove_all(root);
}

TEST_F(ViewerApiTest, JsonEscapesUnicodeAndSpecials) {
    ASSERT_TRUE(is_ok(db_.execute(
        "INSERT INTO entry (id, source_id, parent_id, name, type) VALUES"
        " (10, 1, NULL, '\"quoted\" & <tag>', 1);")));
    Viewer v(db_, "");
    std::string j = get(v, "/api/tree", "parent_id=0&source_id=1");
    EXPECT_NE(j.find("\\\"quoted\\\" & <tag>"), std::string::npos);
}

}  // namespace
}  // namespace offcat
