/**
 * Database layer for LotMan using sqlite_orm
 *
 * This header centralizes all SQLite/ORM logic to keep database operations
 * separate from business logic.
 */

#ifndef LOTMAN_DB_H
#define LOTMAN_DB_H

#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <sqlite_orm/sqlite_orm.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotman {
namespace db {

/**
 * RAII guard for SQLite prepared statements.
 * Automatically finalizes the statement when destroyed.
 */
class StmtGuard {
  public:
	explicit StmtGuard(sqlite3_stmt *stmt) : m_stmt(stmt) {}

	// Non-copyable
	StmtGuard(const StmtGuard &) = delete;
	StmtGuard &operator=(const StmtGuard &) = delete;

	// Movable
	StmtGuard(StmtGuard &&other) noexcept : m_stmt(other.m_stmt) {
		other.m_stmt = nullptr;
	}
	StmtGuard &operator=(StmtGuard &&other) noexcept {
		if (this != &other) {
			if (m_stmt)
				sqlite3_finalize(m_stmt);
			m_stmt = other.m_stmt;
			other.m_stmt = nullptr;
		}
		return *this;
	}

	~StmtGuard() {
		if (m_stmt)
			sqlite3_finalize(m_stmt);
	}

	sqlite3_stmt *get() const {
		return m_stmt;
	}
	sqlite3_stmt *release() {
		sqlite3_stmt *tmp = m_stmt;
		m_stmt = nullptr;
		return tmp;
	}

  private:
	sqlite3_stmt *m_stmt = nullptr;
};

/**
 * ORM model structs that map directly to database tables.
 * These are lightweight plain-old-data types used for database operations.
 */

struct Owner {
	std::string lot_name;
	std::string owner;
};

struct Parent {
	std::string lot_name;
	std::string parent;
};

struct Path {
	std::string lot_name;
	std::string path;
	int recursive; // SQLite doesn't have native bool, stored as int
	int exclude;   // If true, this path is excluded from the lot's tracking
};

struct ManagementPolicyAttributes {
	std::string lot_name;
	double dedicated_GB;
	double opportunistic_GB;
	int64_t max_num_objects;
	int64_t creation_time;
	int64_t expiration_time;
	int64_t deletion_time;
};

struct LotUsage {
	std::string lot_name;
	double self_GB;
	double children_GB;
	int64_t self_objects;
	int64_t children_objects;
	double self_GB_being_written;
	double children_GB_being_written;
	int64_t self_objects_being_written;
	int64_t children_objects_being_written;
};

/**
 * Tracks the database schema version for migration support.
 * There is always exactly one row in the schema_versions table with id=1.
 * The 'version' field indicates the current schema version of the database.
 */
struct SchemaVersion {
	int id;		 // Always 1 (single-row table)
	int version; // Current schema version number
};

/**
 * Tracks how a child lot's MPA values are attributed across its parents.
 * Each row describes a (child, parent, MPA_key) triple and the fraction
 * of the child's MPA that is counted against that parent.
 */
struct ParentChildAttribution {
	std::string child_lot_name;
	std::string parent_lot_name;
	std::string mpa_key; // e.g. "dedicated_GB", "opportunistic_GB", "max_num_objects"
	double fraction;	 // 0.0 to 1.0, how much of child's MPA is attributed to this parent
};

/**
 * A ledger entry recording that a lot has been (or will be at `reclaimed_at`)
 * reclaimed by the storage provider. The presence of a row -- combined with
 * `reclaimed_at <= now` -- indicates that LotMan should treat the lot as a
 * historical record only. The accounting tie between the lot's paths and
 * the lot itself is severed: capacity / Axiom-2 sweeps ignore reclaimed
 * children, dir-based usage updates skip the reclaimed lot's self-write
 * (parent subtraction math is unchanged), and most mutating APIs reject
 * writes against a reclaimed lot. Removing the lot also removes its
 * reclamation row.
 *
 * The PK on `lot_name` enforces at most one reclamation row per lot.
 * `reclaimed_at` is milliseconds since epoch (matching MPA timestamps).
 * `reclaimed_reason` is opaque free-form text (may be empty).
 */
struct Reclamation {
	std::string lot_name;
	int64_t reclaimed_at;
	std::string reclaimed_reason;
};

/**
 * Creates the sqlite_orm storage definition.
 * This defines the schema mapping between C++ structs and SQLite tables.
 */
inline auto create_storage(const std::string &db_path) {
	using namespace sqlite_orm;

	return sqlite_orm::make_storage(
		db_path,
		make_table("schema_versions", make_column("id", &SchemaVersion::id, primary_key()),
				   make_column("version", &SchemaVersion::version)),
		make_table("owners", make_column("lot_name", &Owner::lot_name, primary_key()),
				   make_column("owner", &Owner::owner)),
		make_table("parents", make_column("lot_name", &Parent::lot_name), make_column("parent", &Parent::parent),
				   primary_key(&Parent::lot_name, &Parent::parent)),
		make_table("paths", make_column("lot_name", &Path::lot_name), make_column("path", &Path::path),
				   make_column("recursive", &Path::recursive), make_column("exclude", &Path::exclude, default_value(0)),
				   primary_key(&Path::lot_name, &Path::path)),
		make_table("management_policy_attributes",
				   make_column("lot_name", &ManagementPolicyAttributes::lot_name, primary_key()),
				   make_column("dedicated_GB", &ManagementPolicyAttributes::dedicated_GB),
				   make_column("opportunistic_GB", &ManagementPolicyAttributes::opportunistic_GB),
				   make_column("max_num_objects", &ManagementPolicyAttributes::max_num_objects),
				   make_column("creation_time", &ManagementPolicyAttributes::creation_time),
				   make_column("expiration_time", &ManagementPolicyAttributes::expiration_time),
				   make_column("deletion_time", &ManagementPolicyAttributes::deletion_time)),
		make_table("lot_usage", make_column("lot_name", &LotUsage::lot_name, primary_key()),
				   make_column("self_GB", &LotUsage::self_GB), make_column("children_GB", &LotUsage::children_GB),
				   make_column("self_objects", &LotUsage::self_objects),
				   make_column("children_objects", &LotUsage::children_objects),
				   make_column("self_GB_being_written", &LotUsage::self_GB_being_written),
				   make_column("children_GB_being_written", &LotUsage::children_GB_being_written),
				   make_column("self_objects_being_written", &LotUsage::self_objects_being_written),
				   make_column("children_objects_being_written", &LotUsage::children_objects_being_written)),
		make_table("parent_child_attributions", make_column("child_lot_name", &ParentChildAttribution::child_lot_name),
				   make_column("parent_lot_name", &ParentChildAttribution::parent_lot_name),
				   make_column("mpa_key", &ParentChildAttribution::mpa_key),
				   make_column("fraction", &ParentChildAttribution::fraction),
				   primary_key(&ParentChildAttribution::child_lot_name, &ParentChildAttribution::parent_lot_name,
							   &ParentChildAttribution::mpa_key)),
		make_table("reclamations", make_column("lot_name", &Reclamation::lot_name, primary_key()),
				   make_column("reclaimed_at", &Reclamation::reclaimed_at),
				   make_column("reclaimed_reason", &Reclamation::reclaimed_reason)));
}

// Type alias for the storage type
using Storage = decltype(create_storage(""));

/**
 * Storage manager that provides lazy-initialized access to the database.
 * The storage instance is created on first access and can be reset when
 * the database path changes (e.g., during testing).
 */
class StorageManager {
  public:
	/**
	 * Get the storage instance. Initializes if needed.
	 * @return Reference to the storage, or throws on error
	 */
	static Storage &get_storage();

	/**
	 * Get the database file path, creating directories if needed.
	 * @return Pair of (success, path_or_error_message)
	 */
	static std::pair<bool, std::string> get_db_path();

	/**
	 * Reset the storage (useful for testing or re-initialization).
	 *
	 * WARNING: Not thread-safe. Only call when no other threads are using
	 * the database (e.g., during test setup or application shutdown).
	 * Clears the connection pool and statement cache before resetting storage.
	 */
	static void reset();

  private:
	static std::unique_ptr<Storage> m_storage;
	static bool m_initialized;
	static bool m_wal_enabled;
};

/**
 * Raw SQL utility functions for complex/dynamic queries.
 * These are used when ORM expressions are insufficient or overly complex.
 */

/**
 * RAII wrapper for SQLite connections with transaction support.
 * Automatically closes the connection when destroyed and rolls back
 * any uncommitted transaction.
 */
class ScopedConnection {
  public:
	enum class TransactionType { None, Deferred, Immediate, Exclusive };

	/**
	 * Opens a connection to the database.
	 * @param txn_type Type of transaction to begin (default: None)
	 * @throws std::runtime_error if connection fails
	 */
	explicit ScopedConnection(TransactionType txn_type = TransactionType::None);

	// Non-copyable
	ScopedConnection(const ScopedConnection &) = delete;
	ScopedConnection &operator=(const ScopedConnection &) = delete;

	// Movable
	ScopedConnection(ScopedConnection &&other) noexcept;
	ScopedConnection &operator=(ScopedConnection &&other) noexcept;

	~ScopedConnection();

	/**
	 * Get the raw sqlite3 handle.
	 */
	sqlite3 *get() const {
		return m_db;
	}

	/**
	 * Commit the transaction (if one was started).
	 * @return true on success, false on failure
	 */
	bool commit();

	/**
	 * Rollback the transaction (if one was started).
	 */
	void rollback();

	/**
	 * Check if connection is valid.
	 */
	bool valid() const {
		return m_db != nullptr;
	}

	/**
	 * Get any error message from the last operation.
	 */
	const std::string &error() const {
		return m_error;
	}

  private:
	sqlite3 *m_db = nullptr;
	bool m_in_transaction = false;
	bool m_committed = false;
	std::string m_error;
};

/**
 * Connection pool for SQLite connections.
 * Reuses connections instead of opening/closing for each operation.
 * Thread-safe via mutex protection.
 */
class ConnectionPool {
  public:
	/**
	 * Get a connection from the pool, creating one if none available.
	 * @return sqlite3* handle (caller must return via release())
	 */
	static sqlite3 *acquire();

	/**
	 * Return a connection to the pool for reuse.
	 * @param conn Connection to return
	 */
	static void release(sqlite3 *conn);

	/**
	 * Clear all pooled connections (e.g., on shutdown or reset)
	 */
	static void clear();

	/**
	 * Set maximum pool size (default: 5)
	 */
	static void set_max_size(size_t size);

  private:
	static std::vector<sqlite3 *> m_pool;
	static std::mutex m_mutex;
	static size_t m_max_size;
};

/**
 * RAII wrapper that acquires a connection from the pool and releases it on destruction.
 */
class PooledConnection {
  public:
	enum class TransactionType { None, Deferred, Immediate, Exclusive };

	explicit PooledConnection(TransactionType txn_type = TransactionType::None);

	// Non-copyable
	PooledConnection(const PooledConnection &) = delete;
	PooledConnection &operator=(const PooledConnection &) = delete;

	// Movable
	PooledConnection(PooledConnection &&other) noexcept;
	PooledConnection &operator=(PooledConnection &&other) noexcept;

	~PooledConnection();

	sqlite3 *get() const {
		return m_db;
	}
	bool commit();
	void rollback();
	bool valid() const {
		return m_db != nullptr;
	}
	const std::string &error() const {
		return m_error;
	}

  private:
	sqlite3 *m_db = nullptr;
	bool m_in_transaction = false;
	bool m_committed = false;
	std::string m_error;
};

/**
 * Cache for prepared statements.
 * Caches statements per connection to avoid re-preparing.
 */
class PreparedStatementCache {
  public:
	/**
	 * Get or create a prepared statement for the given query.
	 * @param db SQLite connection
	 * @param query SQL query string
	 * @return Pair of (statement, error_message). Statement is nullptr on error.
	 */
	static std::pair<sqlite3_stmt *, std::string> get_or_prepare(sqlite3 *db, const std::string &query);

	/**
	 * Reset and return a statement to the cache after use.
	 * The statement should be reset (sqlite3_reset) before returning.
	 * @param db SQLite connection the statement belongs to
	 * @param query The original query string
	 * @param stmt The prepared statement to cache
	 */
	static void return_statement(sqlite3 *db, const std::string &query, sqlite3_stmt *stmt);

	/**
	 * Clear all cached statements for a connection (call before closing connection).
	 * @param db Connection whose statements should be cleared
	 */
	static void clear_for_connection(sqlite3 *db);

	/**
	 * Clear all cached statements (e.g., on shutdown)
	 */
	static void clear_all();

  private:
	// Map: connection -> (query -> statement)
	static std::unordered_map<sqlite3 *, std::unordered_map<std::string, sqlite3_stmt *>> m_cache;
	static std::mutex m_mutex;
};

/**
 * RAII guard for cached prepared statements.
 * Returns the statement to cache on destruction.
 */
class CachedStmtGuard {
  public:
	CachedStmtGuard(sqlite3 *db, const std::string &query, sqlite3_stmt *stmt)
		: m_db(db), m_query(query), m_stmt(stmt) {}

	// Non-copyable
	CachedStmtGuard(const CachedStmtGuard &) = delete;
	CachedStmtGuard &operator=(const CachedStmtGuard &) = delete;

	// Movable
	CachedStmtGuard(CachedStmtGuard &&other) noexcept
		: m_db(other.m_db), m_query(std::move(other.m_query)), m_stmt(other.m_stmt) {
		other.m_stmt = nullptr;
	}
	CachedStmtGuard &operator=(CachedStmtGuard &&other) noexcept {
		if (this != &other) {
			return_to_cache();
			m_db = other.m_db;
			m_query = std::move(other.m_query);
			m_stmt = other.m_stmt;
			other.m_stmt = nullptr;
		}
		return *this;
	}

	~CachedStmtGuard() {
		return_to_cache();
	}

	sqlite3_stmt *get() const {
		return m_stmt;
	}

	// Discard the statement (don't cache it)
	void discard() {
		if (m_stmt) {
			sqlite3_finalize(m_stmt);
			m_stmt = nullptr;
		}
	}

  private:
	void return_to_cache() {
		if (m_stmt) {
			sqlite3_reset(m_stmt);
			sqlite3_clear_bindings(m_stmt);
			PreparedStatementCache::return_statement(m_db, m_query, m_stmt);
			m_stmt = nullptr;
		}
	}

	sqlite3 *m_db = nullptr;
	std::string m_query;
	sqlite3_stmt *m_stmt = nullptr;
};

std::pair<std::vector<std::string>, std::string>
SQL_get_matches(const std::string &dynamic_query,
				const std::map<std::string, std::vector<int>> &str_map = std::map<std::string, std::vector<int>>(),
				const std::map<int64_t, std::vector<int>> &int_map = std::map<int64_t, std::vector<int>>(),
				const std::map<double, std::vector<int>> &double_map = std::map<double, std::vector<int>>());

std::pair<std::vector<std::vector<std::string>>, std::string> SQL_get_matches_multi_col(
	const std::string &dynamic_query, int num_returns,
	const std::map<std::string, std::vector<int>> &str_map = std::map<std::string, std::vector<int>>(),
	const std::map<int64_t, std::vector<int>> &int_map = std::map<int64_t, std::vector<int>>(),
	const std::map<double, std::vector<int>> &double_map = std::map<double, std::vector<int>>());

} // namespace db
} // namespace lotman

#endif // LOTMAN_DB_H
