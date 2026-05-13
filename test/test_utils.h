/**
 * Common test utilities for LotMan test suite
 *
 * This header provides shared RAII wrappers and helper functions
 * used across multiple test files.
 */

#ifndef LOTMAN_TEST_UTILS_H
#define LOTMAN_TEST_UTILS_H

#include "../src/lotman.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

// RAII wrappers for C-style memory management
struct CStringDeleter {
	void operator()(char *ptr) const {
		if (ptr)
			free(ptr);
	}
};
using UniqueCString = std::unique_ptr<char, CStringDeleter>;

struct StringListDeleter {
	void operator()(char **ptr) const {
		if (ptr)
			lotman_free_string_list(ptr);
	}
};
using UniqueStringList = std::unique_ptr<char *, StringListDeleter>;

// RAII wrapper for sqlite3 database connections
struct Sqlite3Deleter {
	void operator()(sqlite3 *db) const {
		if (db)
			sqlite3_close(db);
	}
};
using UniqueSqlite3 = std::unique_ptr<sqlite3, Sqlite3Deleter>;

/**
 * Helper to open a sqlite3 database with RAII management.
 * @param path The path to the database file
 * @return A unique_ptr managing the sqlite3 connection
 * @throws std::runtime_error if the database cannot be opened
 */
inline UniqueSqlite3 open_sqlite3_db(const std::string &path) {
	sqlite3 *db = nullptr;
	int rc = sqlite3_open(path.c_str(), &db);
	if (rc != SQLITE_OK) {
		if (db) {
			std::string err = sqlite3_errmsg(db);
			sqlite3_close(db);
			throw std::runtime_error("Failed to open database: " + err);
		}
		throw std::runtime_error("Failed to open database");
	}
	return UniqueSqlite3(db);
}

/**
 * Creates a unique temporary directory for test isolation.
 * @param prefix The prefix to use in the directory name template (e.g., "lotman_test")
 * @return The path to the created temporary directory
 */
inline std::string create_temp_directory(const std::string &prefix = "lotman_test") {
	std::string temp_dir_template = "/tmp/" + prefix + "_XXXXXX";

	char *mkdtemp_result = mkdtemp(temp_dir_template.data());
	if (mkdtemp_result == nullptr) {
		std::cerr << "Failed to create temporary directory\n";
		exit(1);
	}

	return temp_dir_template;
}

/**
 * Wall-clock "now" expressed in Unix-epoch milliseconds. Returned as a
 * signed 64-bit integer to match the int64_t timestamp parameters used
 * throughout the LotMan public API.
 */
inline int64_t test_now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

#endif // LOTMAN_TEST_UTILS_H
