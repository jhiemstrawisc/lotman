#include "../src/lotman.h"
#include "../src/lotman_db.h"
#include "test_utils.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <sqlite3.h>

class MigrationTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	void SetUp() override {
		tmp_dir = create_temp_directory("lotman_mig_test");
		// Reset storage manager to ensure clean state
		lotman::db::StorageManager::reset();
	}

	void TearDown() override {
		lotman::db::StorageManager::reset();
		std::filesystem::remove_all(tmp_dir);
	}
};

TEST_F(MigrationTest, TestV0ToV1Migration) {
	// 1. Create a database that looks like a real pre-versioning LotMan database.
	// We'll use the ORM to create it (ensuring schema compatibility), then manually
	// remove the schema_versions table to simulate a v0 database.
	std::string db_dir = tmp_dir + "/.lot";
	std::filesystem::create_directories(db_dir);
	std::string db_path = db_dir + "/lotman_cpp.sqlite";

	// First, let StorageManager create a proper database with all tables
	{
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

		auto &storage = lotman::db::StorageManager::get_storage();

		// Insert test data
		storage.replace(lotman::db::Owner{"test_lot", "test_owner"});

		// Reset to close the connection
		lotman::db::StorageManager::reset();
	}

	// Now remove the schema_versions table to simulate a v0 database
	{
		auto db = open_sqlite3_db(db_path);

		char *errMsg = nullptr;
		int rc = sqlite3_exec(db.get(), "DROP TABLE schema_versions", nullptr, nullptr, &errMsg);
		if (rc != SQLITE_OK) {
			std::string err_str = errMsg ? errMsg : "unknown error";
			sqlite3_free(errMsg);
			FAIL() << "SQL error: " << err_str;
		}
	}

	// 2. Re-initialize StorageManager (should detect existing DB without version and set to v1)
	{
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

		auto &storage = lotman::db::StorageManager::get_storage();

		// 3. Verify schema_versions table was created and database is at latest version (1)
		try {
			auto versions = storage.get_all<lotman::db::SchemaVersion>();
			ASSERT_EQ(versions.size(), 1);
			ASSERT_EQ(versions[0].version, 1); // v0 database migrated to v1
		} catch (const std::exception &e) {
			FAIL() << "Failed to query schema_versions: " << e.what();
		}

		// 4. Verify existing data was preserved
		try {
			auto owners = storage.get_all<lotman::db::Owner>();
			ASSERT_EQ(owners.size(), 1);
			ASSERT_EQ(owners[0].lot_name, "test_lot");
			ASSERT_EQ(owners[0].owner, "test_owner");
		} catch (const std::exception &e) {
			FAIL() << "Failed to query owners (data not preserved?): " << e.what();
		}
	}
}

TEST_F(MigrationTest, TestFreshDBVersion) {
	// 1. Ensure no DB exists (SetUp handles this via unique tmp_dir)

	// 2. Initialize StorageManager
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

	auto &storage = lotman::db::StorageManager::get_storage();

	// 3. Verify schema_versions table exists and has current TARGET_DB_VERSION (1)
	try {
		auto versions = storage.get_all<lotman::db::SchemaVersion>();
		ASSERT_EQ(versions.size(), 1);
		ASSERT_EQ(versions[0].version, 1); // Fresh database starts at latest version
	} catch (const std::exception &e) {
		FAIL() << "Failed to query schema_versions: " << e.what();
	}
}

TEST_F(MigrationTest, TestEmptySchemaVersionsTable) {
	// This test verifies that an empty schema_versions table (edge case) is handled correctly.
	// This could happen if the table was created but no version row was inserted.
	std::string db_dir = tmp_dir + "/.lot";
	std::filesystem::create_directories(db_dir);
	std::string db_path = db_dir + "/lotman_cpp.sqlite";

	// First, let StorageManager create a proper database with all tables
	{
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

		auto &storage = lotman::db::StorageManager::get_storage();

		// Insert test data
		storage.replace(lotman::db::Owner{"test_lot_empty", "test_owner_empty"});

		// Reset to close the connection
		lotman::db::StorageManager::reset();
	}

	// Delete all rows from schema_versions table (leaving it empty)
	{
		auto db = open_sqlite3_db(db_path);

		char *errMsg = nullptr;
		int rc = sqlite3_exec(db.get(), "DELETE FROM schema_versions", nullptr, nullptr, &errMsg);
		if (rc != SQLITE_OK) {
			std::string err_str = errMsg ? errMsg : "unknown error";
			sqlite3_free(errMsg);
			FAIL() << "SQL error: " << err_str;
		}
	}

	// Re-initialize StorageManager - should handle empty table correctly
	{
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

		auto &storage = lotman::db::StorageManager::get_storage();

		// Verify schema_versions table was populated with current version
		try {
			auto versions = storage.get_all<lotman::db::SchemaVersion>();
			ASSERT_EQ(versions.size(), 1);
			ASSERT_EQ(versions[0].version, 1); // Migrated to latest version
		} catch (const std::exception &e) {
			FAIL() << "Failed to query schema_versions: " << e.what();
		}

		// Verify existing data was preserved
		try {
			auto owners = storage.get_all<lotman::db::Owner>();
			ASSERT_EQ(owners.size(), 1);
			ASSERT_EQ(owners[0].lot_name, "test_lot_empty");
			ASSERT_EQ(owners[0].owner, "test_owner_empty");
		} catch (const std::exception &e) {
			FAIL() << "Failed to query owners (data not preserved?): " << e.what();
		}
	}
}

TEST_F(MigrationTest, TestCorruptedDBRejected) {
	// This test verifies that corrupted or incompatible databases are rejected
	// with a clear error message, rather than silently losing data.
	std::string db_dir = tmp_dir + "/.lot";
	std::filesystem::create_directories(db_dir);
	std::string db_path = db_dir + "/lotman_cpp.sqlite";

	// Create a database with an incompatible schema (missing columns, wrong types, etc.)
	{
		auto db = open_sqlite3_db(db_path);

		// Create an owners table with a different schema (simulating corruption or incompatibility)
		const char *sql = "CREATE TABLE owners (lot_name TEXT PRIMARY KEY, owner TEXT, extra_column INTEGER)";
		char *errMsg = nullptr;
		int rc = sqlite3_exec(db.get(), sql, nullptr, nullptr, &errMsg);
		if (rc != SQLITE_OK) {
			std::string err_str = errMsg ? errMsg : "unknown error";
			sqlite3_free(errMsg);
			FAIL() << "SQL error: " << err_str;
		}
	}

	// Try to initialize StorageManager - should throw an error
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

	// This should throw because the schema is incompatible
	EXPECT_THROW(
		{
			try {
				lotman::db::StorageManager::get_storage();
			} catch (const std::runtime_error &e) {
				// Verify the error message mentions data loss protection
				std::string msg = e.what();
				EXPECT_TRUE(msg.find("data loss") != std::string::npos ||
							msg.find("schema mismatch") != std::string::npos)
					<< "Error message should mention data loss or schema mismatch, got: " << msg;
				throw;
			}
		},
		std::runtime_error);
}

TEST_F(MigrationTest, TestV0ToV1TrailingSlashMigration) {
	// This test verifies that the v0 -> v1 migration correctly adds trailing slashes
	// to all existing paths in the database.
	std::string db_dir = tmp_dir + "/.lot";
	std::filesystem::create_directories(db_dir);
	std::string db_path = db_dir + "/lotman_cpp.sqlite";

	// Step 1: Let the ORM create a proper database structure first
	{
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

		// Initialize storage to create tables
		auto &storage = lotman::db::StorageManager::get_storage();
		static_cast<void>(storage);

		// Reset storage manager to close connections
		lotman::db::StorageManager::reset();
	}

	// Step 2: Manually modify the database to simulate a v0 state
	{
		auto db = open_sqlite3_db(db_path);
		char *errMsg = nullptr;

		// Downgrade version to 0
		int rc =
			sqlite3_exec(db.get(), "UPDATE schema_versions SET version = 0 WHERE id = 1;", nullptr, nullptr, &errMsg);
		if (rc != SQLITE_OK) {
			std::string err_str = errMsg ? errMsg : "unknown error";
			sqlite3_free(errMsg);
			FAIL() << "SQL error updating version: " << err_str;
		}

		// Insert test paths WITHOUT trailing slashes (simulating v0 data)
		const char *insert_sql = "INSERT INTO paths (lot_name, path, recursive) VALUES "
								 "('test_lot', '/foo', 1),"
								 "('test_lot', '/bar/baz', 0),"
								 "('test_lot', '/already/has/slash/', 1);";
		rc = sqlite3_exec(db.get(), insert_sql, nullptr, nullptr, &errMsg);
		if (rc != SQLITE_OK) {
			std::string err_str = errMsg ? errMsg : "unknown error";
			sqlite3_free(errMsg);
			FAIL() << "SQL error inserting paths: " << err_str;
		}
	}

	// Step 3: Re-initialize StorageManager - this should trigger the v0 -> v1 migration
	{
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

		auto &storage = lotman::db::StorageManager::get_storage();

		// Verify schema version was updated to 1
		auto versions = storage.get_all<lotman::db::SchemaVersion>();
		ASSERT_EQ(versions.size(), 1);
		ASSERT_EQ(versions[0].version, 1) << "Expected schema version 1 after migration";

		// Verify all paths now have trailing slashes
		auto paths = storage.get_all<lotman::db::Path>();
		ASSERT_EQ(paths.size(), 3);

		for (const auto &path : paths) {
			EXPECT_TRUE(path.path.back() == '/') << "Path '" << path.path << "' should end with trailing slash";
		}

		// Verify specific paths were migrated correctly
		std::vector<std::string> expected_paths = {"/foo/", "/bar/baz/", "/already/has/slash/"};
		for (const auto &expected : expected_paths) {
			bool found = false;
			for (const auto &path : paths) {
				if (path.path == expected) {
					found = true;
					break;
				}
			}
			EXPECT_TRUE(found) << "Expected path '" << expected << "' not found after migration";
		}
	}
}

TEST_F(MigrationTest, TestPathNormalizationOnInsert) {
	// This test verifies that paths are normalized (trailing slash added) when inserted
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

	rv = lotman_set_context_str("caller", "test_owner", &raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set caller: " << (err2.get() ? err2.get() : "unknown error");

	// Create the default lot first
	const char *default_lot_json = R"({
		"lot_name": "default",
		"owner": "test_owner",
		"parents": ["default"],
		"paths": [{"path": "/default/path", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1700000000,
			"expiration_time": 1800000000,
			"deletion_time": 1900000000
		}
	})";

	rv = lotman_add_lot(default_lot_json, &raw_err);
	UniqueCString err_default(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add default lot: " << (err_default.get() ? err_default.get() : "unknown error");

	// Create a lot with paths that don't have trailing slashes
	const char *lot_json = R"({
		"lot_name": "test_lot",
		"owner": "test_owner",
		"parents": ["test_lot"],
		"paths": [
			{"path": "/no/trailing/slash", "recursive": true},
			{"path": "/has/trailing/slash/", "recursive": false}
		],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1700000000,
			"expiration_time": 1800000000,
			"deletion_time": 1900000000
		}
	})";

	rv = lotman_add_lot(lot_json, &raw_err);
	UniqueCString err3(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add lot: " << (err3.get() ? err3.get() : "unknown error");

	// Verify the paths were stored with trailing slashes (excluding default lot's path)
	auto &storage = lotman::db::StorageManager::get_storage();
	auto paths = storage.get_all<lotman::db::Path>();
	ASSERT_EQ(paths.size(), 3); // 1 from default + 2 from test_lot

	for (const auto &path : paths) {
		EXPECT_TRUE(path.path.back() == '/') << "Path '" << path.path << "' should end with trailing slash";
	}

	// Verify specific normalized paths
	bool found_no_trailing = false;
	bool found_has_trailing = false;
	for (const auto &path : paths) {
		if (path.path == "/no/trailing/slash/") {
			found_no_trailing = true;
		}
		if (path.path == "/has/trailing/slash/") {
			found_has_trailing = true;
		}
	}
	EXPECT_TRUE(found_no_trailing) << "Path '/no/trailing/slash/' not found after normalization";
	EXPECT_TRUE(found_has_trailing) << "Path '/has/trailing/slash/' not found";
}

TEST_F(MigrationTest, TestPathLookupWithoutTrailingSlash) {
	// This test verifies that looking up paths works correctly whether or not
	// the input has a trailing slash
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set lot_home: " << (err.get() ? err.get() : "unknown error");

	rv = lotman_set_context_str("caller", "default_owner", &raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set caller: " << (err2.get() ? err2.get() : "unknown error");

	// Create the default lot first
	const char *default_lot_json = R"({
		"lot_name": "default",
		"owner": "default_owner",
		"parents": ["default"],
		"paths": [{"path": "/default/path", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1700000000,
			"expiration_time": 1800000000,
			"deletion_time": 1900000000
		}
	})";

	rv = lotman_add_lot(default_lot_json, &raw_err);
	UniqueCString err3(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add default lot: " << (err3.get() ? err3.get() : "unknown error");

	// Create a test lot
	const char *test_lot_json = R"({
		"lot_name": "test_lot",
		"owner": "default_owner",
		"parents": ["test_lot"],
		"paths": [{"path": "/test/path", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1700000000,
			"expiration_time": 1800000000,
			"deletion_time": 1900000000
		}
	})";

	rv = lotman_add_lot(test_lot_json, &raw_err);
	UniqueCString err4(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add test lot: " << (err4.get() ? err4.get() : "unknown error");

	// Test lookup WITHOUT trailing slash
	char **output = nullptr;
	rv = lotman_get_lots_from_dir("/test/path", false, 1750000000, &output, &raw_err);
	UniqueCString err5(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to get lots from dir: " << (err5.get() ? err5.get() : "unknown error");
	ASSERT_NE(output, nullptr);
	EXPECT_STREQ(output[0], "test_lot") << "Expected test_lot for path /test/path";
	lotman_free_string_list(output);

	// Test lookup WITH trailing slash
	output = nullptr;
	rv = lotman_get_lots_from_dir("/test/path/", false, 1750000000, &output, &raw_err);
	UniqueCString err6(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to get lots from dir: " << (err6.get() ? err6.get() : "unknown error");
	ASSERT_NE(output, nullptr);
	EXPECT_STREQ(output[0], "test_lot") << "Expected test_lot for path /test/path/";
	lotman_free_string_list(output);

	// Test subdirectory lookup (recursive path)
	output = nullptr;
	rv = lotman_get_lots_from_dir("/test/path/subdir", false, 1750000000, &output, &raw_err);
	UniqueCString err7(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to get lots from dir: " << (err7.get() ? err7.get() : "unknown error");
	ASSERT_NE(output, nullptr);
	EXPECT_STREQ(output[0], "test_lot") << "Expected test_lot for subdirectory /test/path/subdir";
	lotman_free_string_list(output);
}
