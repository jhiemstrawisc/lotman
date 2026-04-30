/**
 * Database operations for LotMan using sqlite_orm
 *
 * This file implements all database CRUD operations using the ORM layer.
 * Complex queries that aren't easily expressed in the ORM can still use
 * raw SQL via storage.execute().
 */

#include "lotman_db.h"

#include "lotman.h"
#include "lotman_internal.h"

#include <nlohmann/json.hpp>
#include <pwd.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace sqlite_orm;

namespace lotman {
namespace db {

// Static member definitions
std::unique_ptr<Storage> StorageManager::m_storage = nullptr;
bool StorageManager::m_initialized = false;
bool StorageManager::m_wal_enabled = false;

// Connection pool static members
std::vector<sqlite3 *> ConnectionPool::m_pool;
std::mutex ConnectionPool::m_mutex;
size_t ConnectionPool::m_max_size = 5;

// Prepared statement cache static members
std::unordered_map<sqlite3 *, std::unordered_map<std::string, sqlite3_stmt *>> PreparedStatementCache::m_cache;
std::mutex PreparedStatementCache::m_mutex;

// Current target database schema version. Increment this when adding new migrations.
static constexpr int TARGET_DB_VERSION = 1;

/**
 * Helper function to create a Path record from JSON.
 * Handles normalization and default values for optional fields.
 *
 * @param lot_name The lot name to associate with the path
 * @param path_json JSON object containing path, recursive, and optionally exclude
 * @return A db::Path struct ready for database insertion
 */
static db::Path create_path_record(const std::string &lot_name, const nlohmann::json &path_json) {
	std::string normalized_path = ensure_trailing_slash(path_json["path"].get<std::string>());
	bool recursive = path_json["recursive"].get<bool>();
	// exclude defaults to false if not specified (schema validates it's boolean if present)
	bool exclude = path_json.contains("exclude") ? path_json["exclude"].get<bool>() : false;
	return db::Path{lot_name, normalized_path, static_cast<int>(recursive), static_cast<int>(exclude)};
}

/**
 * Perform explicit schema migrations between database versions.
 *
 * This function handles upgrading the database schema from one version to the next.
 * Each migration case should use explicit ALTER TABLE statements where possible,
 * as they are guaranteed to preserve existing data.
 *
 * IMPORTANT: Avoid using sync_schema() in migrations as it may drop and recreate
 * tables, causing data loss. Use raw SQL ALTER TABLE statements instead.
 *
 * @param storage Reference to the ORM storage
 * @param current_version The current database schema version
 * @param target_version The target database schema version
 * @throws std::runtime_error if migration fails or is not defined
 */
void migrate_db(Storage &storage, int current_version, int target_version) {
	for (int v = current_version + 1; v <= target_version; ++v) {
		switch (v) {
			case 1: {
				// Migration v0 -> v1:
				// 1. Ensure all paths in the database have trailing slashes.
				//    This prevents string matching bugs where paths like "/foo" and "/foobar"
				//    could be incorrectly matched.
				// 2. The 'exclude' column is added to the paths table by sync_schema().
				//    This supports path exclusions where a lot can track /foo recursively
				//    but exclude /foo/bar from that tracking.
				// 3. The paths table uses a composite primary key (lot_name, path) instead
				//    of unique(path), allowing different lots to share paths with
				//    non-overlapping time ranges.
				// 4. The parent_child_attributions table is created by sync_schema().
				//
				// v0 databases are those created before the schema_versions table existed.
				// They may have paths stored without trailing slashes.
				//
				// Note: The constraint change on paths (unique→composite PK) requires
				// recreating the table. Since this version is unreleased, sync_schema()
				// handles this. For pre-existing test databases, a manual DB reset may
				// be needed.
				using namespace sqlite_orm;

				// Normalize paths with trailing slashes
				auto all_paths = storage.get_all<Path>();
				for (auto &path_record : all_paths) {
					if (!path_record.path.empty() && path_record.path.back() != '/') {
						std::string old_path = path_record.path;
						path_record.path += '/';
						storage.update_all(
							set(c(&Path::path) = path_record.path),
							where(c(&Path::lot_name) == path_record.lot_name and c(&Path::path) == old_path));
					}
				}
				break;
			}
			default:
				throw std::runtime_error("No migration defined for version " + std::to_string(v));
		}
	}
}

std::pair<bool, std::string> StorageManager::get_db_path() {
	const char *lot_env_dir = getenv("LOT_HOME");

	auto bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	bufsize = (bufsize == -1) ? 16384 : bufsize;

	std::unique_ptr<char[]> buf(new char[bufsize]);

	std::string home_dir;
	struct passwd pwd, *result = NULL;
	getpwuid_r(geteuid(), &pwd, buf.get(), bufsize, &result);
	if (result && result->pw_dir) {
		home_dir = result->pw_dir;
	}

	std::string lot_home_dir;
	std::string configured_lot_home_dir = lotman::Context::get_lot_home();
	if (configured_lot_home_dir.length() > 0) {
		lot_home_dir = configured_lot_home_dir;
	} else {
		lot_home_dir = lot_env_dir ? lot_env_dir : home_dir.c_str();
	}

	if (lot_home_dir.size() == 0) {
		return std::make_pair(false, "Could not get Lot home");
	}

	int r = mkdir(lot_home_dir.c_str(), 0700);
	if ((r < 0) && errno != EEXIST) {
		return std::make_pair(false,
							  "Unable to create directory " + lot_home_dir + ": errno: " + std::to_string(errno));
	}

	std::string lot_db_dir = lot_home_dir + "/.lot";
	r = mkdir(lot_db_dir.c_str(), 0700);
	if ((r < 0) && errno != EEXIST) {
		return std::make_pair(false, "Unable to create directory " + lot_db_dir + ": errno: " + std::to_string(errno));
	}

	return std::make_pair(true, lot_db_dir + "/lotman_cpp.sqlite");
}

Storage &StorageManager::get_storage() {
	if (!m_initialized) {
		auto db_path_result = get_db_path();
		if (!db_path_result.first) {
			throw std::runtime_error("Failed to get database path: " + db_path_result.second);
		}

		m_storage = std::make_unique<Storage>(create_storage(db_path_result.second));

		// Enable WAL mode for better concurrency
		if (!m_wal_enabled) {
			m_storage->pragma.journal_mode(journal_mode::WAL);
			m_wal_enabled = true;
		}

		// Set busy timeout
		m_storage->busy_timeout(*lotman_db_timeout);

		// Check for existing database state before syncing schema
		bool schema_versions_exists = false;
		bool owners_exists = false;

		try {
			m_storage->count<SchemaVersion>();
			schema_versions_exists = true;
		} catch (const std::system_error &) {
			// Table does not exist - this is expected for fresh or legacy databases
			schema_versions_exists = false;
		}

		// If there's no schema_versions table, check for owners table to detect databases
		// that exist but predate schema_versions
		if (!schema_versions_exists) {
			try {
				m_storage->count<Owner>();
				owners_exists = true;
			} catch (const std::system_error &) {
				// Table does not exist - this is a fresh database
				owners_exists = false;
			}
		}

		// Determine whether this is a fresh database or existing one
		bool is_fresh_db = !schema_versions_exists && !owners_exists;

		if (is_fresh_db) {
			// Fresh database: safe to use sync_schema() to create all tables
			m_storage->sync_schema();
		} else {
			// Existing database: use sync_schema_simulate() to check what would happen
			// before making any changes. This protects against accidental data loss.
			auto simulation = m_storage->sync_schema_simulate(true); // true = preserve mode

			// Check if any table would be dropped and recreated (data loss!)
			for (const auto &table_result : simulation) {
				if (table_result.second == sqlite_orm::sync_schema_result::dropped_and_recreated) {
					throw std::runtime_error(
						"Database schema mismatch detected for table '" + table_result.first +
						"'. The required schema change would cause data loss. "
						"This may indicate database corruption or an incompatible schema change. "
						"Please backup your database and contact support, or delete the database to start fresh.");
				}
			}

			// Safe to proceed - use preserve mode to be extra careful
			m_storage->sync_schema(true);
		}

		// Initialize or migrate database version
		int current_version = 0;

		if (schema_versions_exists) {
			auto version_ptr = m_storage->get_pointer<SchemaVersion>(1);
			if (version_ptr) {
				current_version = version_ptr->version;
			} else {
				// Table existed but no row with id=1. Should not happen if we manage it correctly.
				// Need to check for owners table here since we skipped that check earlier
				// (owners_exists was only populated when schema_versions_exists was false)
				bool has_owners = false;
				try {
					m_storage->count<Owner>();
					has_owners = true;
				} catch (const std::system_error &) {
					has_owners = false;
				}

				if (has_owners)
					current_version = 1;
				else
					current_version = TARGET_DB_VERSION;

				m_storage->replace(SchemaVersion{1, current_version});
			}
		} else {
			// schema_versions table did not exist.
			if (owners_exists) {
				// Existing v0 database (pre-schema-versioning)
				// These databases need migration to add trailing slashes to paths
				current_version = 0;
				m_storage->replace(SchemaVersion{1, current_version});
			} else {
				// Fresh database
				current_version = TARGET_DB_VERSION;
				m_storage->replace(SchemaVersion{1, current_version});
			}
		}

		if (current_version > TARGET_DB_VERSION) {
			throw std::runtime_error("Database schema version (" + std::to_string(current_version) +
									 ") is newer than supported version (" + std::to_string(TARGET_DB_VERSION) +
									 "). Cannot downgrade. Please use a newer version of the application.");
		}

		if (current_version < TARGET_DB_VERSION) {
			migrate_db(*m_storage, current_version, TARGET_DB_VERSION);
			m_storage->replace(SchemaVersion{1, TARGET_DB_VERSION});
		}

		m_initialized = true;
	}

	return *m_storage;
}

void StorageManager::reset() {
	// First clear the connection pool and statement cache since they hold
	// connections/statements for the old database
	PreparedStatementCache::clear_all();
	ConnectionPool::clear();

	m_storage.reset();
	m_initialized = false;
	m_wal_enabled = false;
}

// ConnectionPool implementation

sqlite3 *ConnectionPool::acquire() {
	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_pool.empty()) {
		sqlite3 *conn = m_pool.back();
		m_pool.pop_back();
		return conn;
	}

	// No available connections, create a new one
	auto db_path = StorageManager::get_db_path();
	if (!db_path.first) {
		return nullptr;
	}

	// Ensure storage is initialized (creates tables if needed)
	StorageManager::get_storage();

	sqlite3 *conn = nullptr;
	int rc = sqlite3_open(db_path.second.c_str(), &conn);
	if (rc != SQLITE_OK) {
		if (conn) {
			sqlite3_close(conn);
		}
		return nullptr;
	}

	// Enable WAL mode for better concurrency across processes
	sqlite3_exec(conn, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
	sqlite3_busy_timeout(conn, *lotman_db_timeout);
	return conn;
}

void ConnectionPool::release(sqlite3 *conn) {
	if (!conn)
		return;

	bool should_close = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (m_pool.size() >= m_max_size) {
			should_close = true;
		} else {
			m_pool.push_back(conn);
		}
	}

	if (should_close) {
		// Clear cached statements outside of ConnectionPool mutex to avoid deadlock
		PreparedStatementCache::clear_for_connection(conn);
		sqlite3_close(conn);
	}
}

void ConnectionPool::clear() {
	std::lock_guard<std::mutex> lock(m_mutex);

	for (auto *conn : m_pool) {
		PreparedStatementCache::clear_for_connection(conn);
		sqlite3_close(conn);
	}
	m_pool.clear();
}

void ConnectionPool::set_max_size(size_t size) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_max_size = size;

	// Trim pool if it exceeds new max size
	while (m_pool.size() > m_max_size) {
		sqlite3 *conn = m_pool.back();
		m_pool.pop_back();
		PreparedStatementCache::clear_for_connection(conn);
		sqlite3_close(conn);
	}
}

// PreparedStatementCache implementation

std::pair<sqlite3_stmt *, std::string> PreparedStatementCache::get_or_prepare(sqlite3 *db, const std::string &query) {
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		auto conn_it = m_cache.find(db);
		if (conn_it != m_cache.end()) {
			auto stmt_it = conn_it->second.find(query);
			if (stmt_it != conn_it->second.end()) {
				// Found cached statement, remove from cache (will be returned later)
				sqlite3_stmt *stmt = stmt_it->second;
				conn_it->second.erase(stmt_it);
				return std::make_pair(stmt, "");
			}
		}
	}

	// Not in cache, prepare a new statement
	sqlite3_stmt *stmt = nullptr;
	int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
	if (rc != SQLITE_OK) {
		return std::make_pair(nullptr, "Call to sqlite3_prepare_v2 failed: sqlite errno: " + std::to_string(rc) +
										   " - " + std::string(sqlite3_errmsg(db)));
	}

	return std::make_pair(stmt, "");
}

void PreparedStatementCache::return_statement(sqlite3 *db, const std::string &query, sqlite3_stmt *stmt) {
	if (!stmt)
		return;

	std::lock_guard<std::mutex> lock(m_mutex);

	// Check if there's already a statement cached for this query
	auto &conn_cache = m_cache[db];
	if (conn_cache.find(query) != conn_cache.end()) {
		// Already have one cached, just finalize this one
		sqlite3_finalize(stmt);
	} else {
		conn_cache[query] = stmt;
	}
}

void PreparedStatementCache::clear_for_connection(sqlite3 *db) {
	std::lock_guard<std::mutex> lock(m_mutex);

	auto it = m_cache.find(db);
	if (it != m_cache.end()) {
		for (auto &[query, stmt] : it->second) {
			sqlite3_finalize(stmt);
		}
		m_cache.erase(it);
	}
}

void PreparedStatementCache::clear_all() {
	std::lock_guard<std::mutex> lock(m_mutex);

	for (auto &[db, conn_cache] : m_cache) {
		for (auto &[query, stmt] : conn_cache) {
			sqlite3_finalize(stmt);
		}
	}
	m_cache.clear();
}

// PooledConnection implementation

PooledConnection::PooledConnection(TransactionType txn_type) {
	m_db = ConnectionPool::acquire();
	if (!m_db) {
		m_error = "Failed to acquire connection from pool";
		return;
	}

	// Begin transaction if requested
	if (txn_type != TransactionType::None) {
		const char *txn_cmd = nullptr;
		switch (txn_type) {
			case TransactionType::Deferred:
				txn_cmd = "BEGIN DEFERRED";
				break;
			case TransactionType::Immediate:
				txn_cmd = "BEGIN IMMEDIATE";
				break;
			case TransactionType::Exclusive:
				txn_cmd = "BEGIN EXCLUSIVE";
				break;
			default:
				break;
		}

		if (txn_cmd) {
			int rc = sqlite3_exec(m_db, txn_cmd, nullptr, nullptr, nullptr);
			if (rc != SQLITE_OK) {
				m_error = "Failed to begin transaction: sqlite errno: " + std::to_string(rc);
				ConnectionPool::release(m_db);
				m_db = nullptr;
				return;
			}
			m_in_transaction = true;
		}
	}
}

PooledConnection::PooledConnection(PooledConnection &&other) noexcept
	: m_db(other.m_db),
	  m_in_transaction(other.m_in_transaction),
	  m_committed(other.m_committed),
	  m_error(std::move(other.m_error)) {
	other.m_db = nullptr;
	other.m_in_transaction = false;
}

PooledConnection &PooledConnection::operator=(PooledConnection &&other) noexcept {
	if (this != &other) {
		// Clean up current state
		if (m_db) {
			if (m_in_transaction && !m_committed) {
				sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
			}
			ConnectionPool::release(m_db);
		}

		m_db = other.m_db;
		m_in_transaction = other.m_in_transaction;
		m_committed = other.m_committed;
		m_error = std::move(other.m_error);

		other.m_db = nullptr;
		other.m_in_transaction = false;
	}
	return *this;
}

PooledConnection::~PooledConnection() {
	if (m_db) {
		if (m_in_transaction && !m_committed) {
			sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
		}
		ConnectionPool::release(m_db);
	}
}

bool PooledConnection::commit() {
	if (!m_db || !m_in_transaction || m_committed) {
		return false;
	}

	int rc = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
	if (rc != SQLITE_OK) {
		m_error = "Failed to commit: sqlite errno: " + std::to_string(rc);
		return false;
	}

	m_committed = true;
	return true;
}

void PooledConnection::rollback() {
	if (m_db && m_in_transaction && !m_committed) {
		sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
		m_committed = true; // Prevent double-rollback in destructor
	}
}

// ScopedConnection implementation

ScopedConnection::ScopedConnection(TransactionType txn_type) {
	auto db_path = StorageManager::get_db_path();
	if (!db_path.first) {
		m_error = "Could not get lot_file: " + db_path.second;
		return;
	}

	// Ensure storage is initialized (creates tables if needed)
	StorageManager::get_storage();

	int rc = sqlite3_open(db_path.second.c_str(), &m_db);
	if (rc != SQLITE_OK) {
		m_error = "Unable to open lotdb: sqlite errno: " + std::to_string(rc);
		if (m_db) {
			sqlite3_close(m_db);
			m_db = nullptr;
		}
		return;
	}

	// Enable WAL mode for better concurrency across processes
	sqlite3_exec(m_db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
	sqlite3_busy_timeout(m_db, *lotman_db_timeout);

	// Begin transaction if requested
	if (txn_type != TransactionType::None) {
		const char *txn_cmd = nullptr;
		switch (txn_type) {
			case TransactionType::Deferred:
				txn_cmd = "BEGIN DEFERRED";
				break;
			case TransactionType::Immediate:
				txn_cmd = "BEGIN IMMEDIATE";
				break;
			case TransactionType::Exclusive:
				txn_cmd = "BEGIN EXCLUSIVE";
				break;
			default:
				break;
		}

		if (txn_cmd) {
			rc = sqlite3_exec(m_db, txn_cmd, nullptr, nullptr, nullptr);
			if (rc != SQLITE_OK) {
				m_error = "Failed to begin transaction: sqlite errno: " + std::to_string(rc);
				sqlite3_close(m_db);
				m_db = nullptr;
				return;
			}
			m_in_transaction = true;
		}
	}
}

ScopedConnection::ScopedConnection(ScopedConnection &&other) noexcept
	: m_db(other.m_db),
	  m_in_transaction(other.m_in_transaction),
	  m_committed(other.m_committed),
	  m_error(std::move(other.m_error)) {
	other.m_db = nullptr;
	other.m_in_transaction = false;
}

ScopedConnection &ScopedConnection::operator=(ScopedConnection &&other) noexcept {
	if (this != &other) {
		// Clean up current state
		if (m_db) {
			if (m_in_transaction && !m_committed) {
				sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
			}
			sqlite3_close(m_db);
		}

		m_db = other.m_db;
		m_in_transaction = other.m_in_transaction;
		m_committed = other.m_committed;
		m_error = std::move(other.m_error);

		other.m_db = nullptr;
		other.m_in_transaction = false;
	}
	return *this;
}

ScopedConnection::~ScopedConnection() {
	if (m_db) {
		if (m_in_transaction && !m_committed) {
			sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
		}
		sqlite3_close(m_db);
	}
}

bool ScopedConnection::commit() {
	if (!m_db || !m_in_transaction || m_committed) {
		return false;
	}

	int rc = sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
	if (rc != SQLITE_OK) {
		m_error = "Failed to commit: sqlite errno: " + std::to_string(rc);
		return false;
	}

	m_committed = true;
	return true;
}

void ScopedConnection::rollback() {
	if (m_db && m_in_transaction && !m_committed) {
		sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
		m_committed = true; // Prevent double-rollback in destructor
	}
}

// Raw functions for complex SQL queries

std::pair<std::vector<std::string>, std::string> SQL_get_matches(const std::string &dynamic_query,
																 const std::map<std::string, std::vector<int>> &str_map,
																 const std::map<int64_t, std::vector<int>> &int_map,
																 const std::map<double, std::vector<int>> &double_map) {
	std::vector<std::string> return_vec;

	try {
		// Use pooled connection with deferred transaction for read consistency
		PooledConnection conn(PooledConnection::TransactionType::Deferred);
		if (!conn.valid()) {
			return std::make_pair(return_vec, conn.error());
		}

		// Get or prepare the statement (uses cache)
		auto [stmt, prep_error] = PreparedStatementCache::get_or_prepare(conn.get(), dynamic_query);
		if (!stmt) {
			return std::make_pair(return_vec, prep_error);
		}

		// RAII guard that returns statement to cache on scope exit
		CachedStmtGuard stmt_guard(conn.get(), dynamic_query, stmt);

		// Bind parameters
		int rc;
		for (const auto &[value, positions] : str_map) {
			for (int pos : positions) {
				rc = sqlite3_bind_text(stmt, pos, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
				if (rc != SQLITE_OK) {
					stmt_guard.discard(); // Don't cache a statement that failed
					return std::make_pair(return_vec,
										  "Call to sqlite3_bind_text failed: sqlite3 errno: " + std::to_string(rc));
				}
			}
		}

		for (const auto &[value, positions] : int_map) {
			for (int pos : positions) {
				rc = sqlite3_bind_int64(stmt, pos, value);
				if (rc != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(return_vec,
										  "Call to sqlite3_bind_int64 failed: sqlite3 errno: " + std::to_string(rc));
				}
			}
		}

		for (const auto &[value, positions] : double_map) {
			for (int pos : positions) {
				rc = sqlite3_bind_double(stmt, pos, value);
				if (rc != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(return_vec,
										  "Call to sqlite3_bind_double failed: sqlite3 errno: " + std::to_string(rc));
				}
			}
		}

		// Execute and collect results
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			const unsigned char *data = sqlite3_column_text(stmt, 0);
			if (data) {
				return_vec.push_back(reinterpret_cast<const char *>(data));
			}
		}

		if (rc != SQLITE_DONE) {
			stmt_guard.discard();
			return std::make_pair(return_vec, "Error stepping through results: sqlite3 errno: " + std::to_string(rc));
		}

		// Commit the transaction
		conn.commit();

		return std::make_pair(return_vec, "");
	} catch (const std::exception &e) {
		return std::make_pair(return_vec, std::string("Query failed: ") + e.what());
	}
}

std::pair<std::vector<std::vector<std::string>>, std::string> SQL_get_matches_multi_col(
	const std::string &dynamic_query, int num_returns, const std::map<std::string, std::vector<int>> &str_map,
	const std::map<int64_t, std::vector<int>> &int_map, const std::map<double, std::vector<int>> &double_map) {
	std::vector<std::vector<std::string>> return_vec;

	try {
		// Use pooled connection with deferred transaction for read consistency
		PooledConnection conn(PooledConnection::TransactionType::Deferred);
		if (!conn.valid()) {
			return std::make_pair(return_vec, conn.error());
		}

		// Get or prepare the statement (uses cache)
		auto [stmt, prep_error] = PreparedStatementCache::get_or_prepare(conn.get(), dynamic_query);
		if (!stmt) {
			return std::make_pair(return_vec, prep_error);
		}

		// RAII guard that returns statement to cache on scope exit
		CachedStmtGuard stmt_guard(conn.get(), dynamic_query, stmt);

		// Bind parameters
		int rc;
		for (const auto &[value, positions] : str_map) {
			for (int pos : positions) {
				rc = sqlite3_bind_text(stmt, pos, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
				if (rc != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(return_vec,
										  "Call to sqlite3_bind_text failed: sqlite3 errno: " + std::to_string(rc));
				}
			}
		}

		for (const auto &[value, positions] : int_map) {
			for (int pos : positions) {
				rc = sqlite3_bind_int64(stmt, pos, value);
				if (rc != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(return_vec,
										  "Call to sqlite3_bind_int64 failed: sqlite3 errno: " + std::to_string(rc));
				}
			}
		}

		for (const auto &[value, positions] : double_map) {
			for (int pos : positions) {
				rc = sqlite3_bind_double(stmt, pos, value);
				if (rc != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(return_vec,
										  "Call to sqlite3_bind_double failed: sqlite3 errno: " + std::to_string(rc));
				}
			}
		}

		// Execute and collect results
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			std::vector<std::string> row;
			row.reserve(num_returns);
			for (int i = 0; i < num_returns; ++i) {
				const unsigned char *data = sqlite3_column_text(stmt, i);
				row.push_back(data ? reinterpret_cast<const char *>(data) : "");
			}
			return_vec.push_back(std::move(row));
		}

		if (rc != SQLITE_DONE) {
			stmt_guard.discard();
			return std::make_pair(return_vec, "Error stepping through results: sqlite3 errno: " + std::to_string(rc));
		}

		// Commit the transaction
		conn.commit();

		return std::make_pair(return_vec, "");
	} catch (const std::exception &e) {
		return std::make_pair(return_vec, std::string("Query failed: ") + e.what());
	}
}

} // namespace db

// Implementation of Lot and Checks database methods

std::pair<bool, std::string> Lot::write_new() {
	// Note: caller (store_lot) must wrap this in a transaction.
	try {
		auto &storage = db::StorageManager::get_storage();

		db::Owner owner_record{lot_name, owner};
		storage.replace(owner_record);

		for (const auto &parent : parents) {
			db::Parent parent_record{lot_name, parent};
			storage.replace(parent_record);
		}

		// Temporal overlap check: ensure no other lot claims the same path
		// during an overlapping time period before inserting paths.
		for (const auto &path_json : paths) {
			std::string path_str = path_json.at("path").get<std::string>();
			std::string normalized = ensure_trailing_slash(path_str);
			bool exclude = path_json.contains("exclude") ? path_json["exclude"].get<bool>() : false;

			if (!exclude) {
				auto overlap = check_path_temporal_overlap(lot_name, normalized, man_policy_attr.creation_time,
														   man_policy_attr.expiration_time);
				if (!overlap.first) {
					return overlap;
				}
			}
		}

		for (const auto &path : paths) {
			storage.replace(db::create_path_record(lot_name, path));
		}

		db::ManagementPolicyAttributes mpa{lot_name,
										   man_policy_attr.dedicated_GB,
										   man_policy_attr.opportunistic_GB,
										   man_policy_attr.max_num_objects,
										   man_policy_attr.creation_time,
										   man_policy_attr.expiration_time,
										   man_policy_attr.deletion_time};
		storage.replace(mpa);

		db::LotUsage usage_record{lot_name,
								  usage.self_GB,
								  usage.children_GB,
								  usage.self_objects,
								  usage.children_objects,
								  usage.self_GB_being_written,
								  usage.children_GB_being_written,
								  usage.self_objects_being_written,
								  usage.children_objects_being_written};
		storage.replace(usage_record);

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to write new lot: ") + e.what());
	}
}

std::pair<bool, std::string> Lot::delete_lot_from_db() {
	// Note: caller (destroy_lot, destroy_lot_recursive) must wrap this in a transaction.
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Delete from all tables where lot_name matches
		storage.remove_all<db::Owner>(where(c(&db::Owner::lot_name) == lot_name));
		storage.remove_all<db::Parent>(where(c(&db::Parent::lot_name) == lot_name));
		storage.remove_all<db::Path>(where(c(&db::Path::lot_name) == lot_name));
		storage.remove_all<db::ManagementPolicyAttributes>(
			where(c(&db::ManagementPolicyAttributes::lot_name) == lot_name));
		storage.remove_all<db::LotUsage>(where(c(&db::LotUsage::lot_name) == lot_name));

		// Remove attributions where this lot is child or parent
		storage.remove_all<db::ParentChildAttribution>(
			where(c(&db::ParentChildAttribution::child_lot_name) == lot_name or
				  c(&db::ParentChildAttribution::parent_lot_name) == lot_name));

		// Remove dangling parent references from other lots that had this lot as a parent
		storage.remove_all<db::Parent>(where(c(&db::Parent::parent) == lot_name));

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to delete lot: ") + e.what());
	}
}

std::pair<bool, std::string> Lot::store_updates(const std::string &update_stmt,
												const std::map<std::string, std::vector<int>> &update_str_map,
												const std::map<int64_t, std::vector<int>> &update_int_map,
												const std::map<double, std::vector<int>> &update_dbl_map) {
	// For complex/dynamic updates, we use raw SQL with pooled connection
	try {
		// Use immediate transaction for write operations (acquires write lock immediately)
		db::PooledConnection conn(db::PooledConnection::TransactionType::Immediate);
		if (!conn.valid()) {
			return std::make_pair(false, conn.error());
		}

		// Get or prepare the statement (uses cache)
		auto [stmt, prep_error] = db::PreparedStatementCache::get_or_prepare(conn.get(), update_stmt);
		if (!stmt) {
			return std::make_pair(false, prep_error);
		}

		// RAII guard that returns statement to cache on scope exit
		db::CachedStmtGuard stmt_guard(conn.get(), update_stmt, stmt);

		// Bind string parameters
		for (const auto &[value, positions] : update_str_map) {
			for (int pos : positions) {
				if (sqlite3_bind_text(stmt, pos, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
					SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(false, "Failed to bind string parameter");
				}
			}
		}

		// Bind int parameters
		for (const auto &[value, positions] : update_int_map) {
			for (int pos : positions) {
				if (sqlite3_bind_int64(stmt, pos, value) != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(false, "Failed to bind int parameter");
				}
			}
		}

		// Bind double parameters
		for (const auto &[value, positions] : update_dbl_map) {
			for (int pos : positions) {
				if (sqlite3_bind_double(stmt, pos, value) != SQLITE_OK) {
					stmt_guard.discard();
					return std::make_pair(false, "Failed to bind double parameter");
				}
			}
		}

		int rc = sqlite3_step(stmt);

		if (rc != SQLITE_DONE) {
			stmt_guard.discard();
			return std::make_pair(false, "Failed to execute update: sqlite errno: " + std::to_string(rc));
		}

		// Commit the transaction
		if (!conn.commit()) {
			return std::make_pair(false, conn.error());
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to store updates: ") + e.what());
	}
}

std::pair<bool, std::string> Lot::check_path_temporal_overlap(const std::string &lot_name,
															  const std::string &normalized_path, int64_t creation_time,
															  int64_t expiration_time) {
	// Single JOIN query replaces the previous N+1 pattern (one SELECT for
	// candidate lots, plus one get_pointer per candidate).
	std::string query = "SELECT p.lot_name, mpa.creation_time, mpa.expiration_time "
						"FROM paths p "
						"JOIN management_policy_attributes mpa ON p.lot_name = mpa.lot_name "
						"WHERE p.path = ?1 AND p.lot_name != ?2 AND p.exclude = 0 "
						"AND ?3 < mpa.expiration_time AND mpa.creation_time < ?4 "
						"LIMIT 1;";
	std::map<std::string, std::vector<int>> str_map{{normalized_path, {1}}, {lot_name, {2}}};
	std::map<int64_t, std::vector<int>> int_map{{creation_time, {3}}, {expiration_time, {4}}};
	auto rp = db::SQL_get_matches_multi_col(query, 3, str_map, int_map);

	if (!rp.second.empty()) {
		return std::make_pair(false, "Temporal overlap check failed: " + rp.second);
	}

	if (!rp.first.empty()) {
		const auto &row = rp.first[0];
		return std::make_pair(false, "Path '" + normalized_path + "' has a temporal overlap with lot '" + row[0] +
										 "'. Time ranges (treated as half-open intervals [start, end)) overlap: [" +
										 std::to_string(creation_time) + ", " + std::to_string(expiration_time) +
										 ") vs [" + row[1] + ", " + row[2] + ").");
	}

	return std::make_pair(true, "");
}

std::pair<bool, std::string> Lot::store_new_parents(const std::vector<Lot> &new_parents) {
	// Note: caller (add_parents) must wrap this in a transaction.
	try {
		auto &storage = db::StorageManager::get_storage();

		for (const auto &parent : new_parents) {
			db::Parent parent_record{lot_name, parent.lot_name};
			storage.replace(parent_record);
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to store new parents: ") + e.what());
	}
}

std::pair<bool, std::string> Lot::remove_parents_from_db(const std::vector<std::string> &parents) {
	// Note: caller (remove_parents) must wrap this in a transaction.
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		for (const auto &parent : parents) {
			storage.remove_all<db::Parent>(
				where(c(&db::Parent::lot_name) == lot_name and c(&db::Parent::parent) == parent));
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to remove parents: ") + e.what());
	}
}

std::pair<bool, std::string> Lot::remove_paths_from_db(const std::vector<std::string> &paths) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Use transaction for batch delete atomicity
		storage.transaction([&] {
			for (const auto &path : paths) {
				// Normalize path with trailing slash to match stored format
				std::string normalized_path = ensure_trailing_slash(path);
				// Paths are keyed by composite (lot_name, path)
				storage.remove_all<db::Path>(
					where(c(&db::Path::lot_name) == lot_name and c(&db::Path::path) == normalized_path));
			}
			return true; // Commit
		});

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to remove paths: ") + e.what());
	}
}

} // namespace lotman
