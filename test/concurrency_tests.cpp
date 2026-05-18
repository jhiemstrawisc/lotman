/**
 * Concurrency / thread-safety regression tests for libLotMan.
 *
 * libLotMan's C API is called from at least two runtimes inside the same
 * process: Pelican's Go code (renewal / GC goroutines) and xrootd's C++
 * purge thread (XrdPurgeLotMan). Each runtime can serialize its own
 * callers with its own mutex, but they share no mutex across the
 * language boundary. Internally libLotMan uses a singleton SQLite
 * connection (StorageManager) without SQLITE_OPEN_FULLMUTEX, and
 * sqlite_orm's prepared-statement cache is not thread-safe. The only
 * place where concurrent cross-runtime callers can be serialized
 * correctly is inside libLotMan itself.
 *
 * Production stack trace before the fix:
 *     lotman_get_lots_past_del
 *     XrdPurgeLotMan::completePurgePolicyBase
 *     XrdPfc::OldStylePurgeDriver
 *     XrdPfc::ResourceMonitor::perform_purge_task_cleanup
 * triggered when Pelican's GC goroutine ran concurrently with xrootd's
 * purge thread, both hitting the shared SQLite handle inside
 * update_db_children_usage(). Without an internal mutex this test
 * crashes (SIGSEGV) or surfaces SQLite misuse errors very quickly.
 */

#include "../src/lotman.h"
#include "test_utils.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

struct ErrorSink {
	std::atomic<int> count{0};
	std::mutex mu;
	std::string first;

	void record(const std::string &where, int rv, const char *msg) {
		count.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> g(mu);
		if (first.empty()) {
			first = where + " rv=" + std::to_string(rv) + " err=" + (msg ? msg : "<null>");
		}
	}
};

// Per-test hammer duration. Defaults to 2s, which on local hardware
// reliably surfaced the original cross-runtime SIGSEGV in well under a
// second; the extra second is slack for CI noise. CI environments that
// want to dial up the budget (e.g. to chase intermittent reports) can
// set LOTMAN_CONCURRENCY_TEST_SECONDS to any positive integer. The
// lower-bound rationale is documented here rather than at each call
// site so the trade-off is reviewable in one place.
std::chrono::seconds concurrency_test_duration() {
	if (const char *env = std::getenv("LOTMAN_CONCURRENCY_TEST_SECONDS")) {
		try {
			int secs = std::stoi(env);
			if (secs > 0) {
				return std::chrono::seconds(secs);
			}
		} catch (...) {
			// Fall through to default on unparsable input.
		}
	}
	return std::chrono::seconds(2);
}

class ConcurrencyTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	void SetUp() override {
		tmp_dir = create_temp_directory("lotman_concurrency_test");
		setContext("lot_home", tmp_dir);
		setContext("caller", "owner1");
		setContext("strict_hierarchy", "false");
		setContext("contraction_policy", "none");

		addLot(R"({
			"lot_name": "default",
			"owner": "owner1",
			"parents": ["default"],
			"paths": [{"path": "/default", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 1000, "opportunistic_GB": 500,
				"max_num_objects": 10000, "creation_time": 100,
				"expiration_time": 9999999999999, "deletion_time": 9999999999999
			}
		})");
		addLot(R"({
			"lot_name": "parent",
			"owner": "owner1",
			"parents": ["parent"],
			"paths": [{"path": "/parent", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 100, "opportunistic_GB": 50,
				"max_num_objects": 1000, "creation_time": 100,
				"expiration_time": 9999999999999, "deletion_time": 9999999999999
			}
		})");
		addLot(R"({
			"lot_name": "child",
			"owner": "owner1",
			"parents": ["parent"],
			"paths": [{"path": "/parent/child", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 10, "opportunistic_GB": 5,
				"max_num_objects": 100, "creation_time": 100,
				"expiration_time": 9999999999999, "deletion_time": 9999999999999
			}
		})");
	}

	void TearDown() override {
		std::filesystem::remove_all(tmp_dir);
	}

	void setContext(const std::string &key, const std::string &value) {
		char *raw_err = nullptr;
		int rv = lotman_set_context_str(key.c_str(), value.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "set_context(" << key << ") failed: " << (err.get() ? err.get() : "");
	}

	void addLot(const std::string &lot_json) {
		char *raw_err = nullptr;
		int rv = lotman_add_lot(lot_json.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "addLot failed: " << (err.get() ? err.get() : "");
	}

	static int64_t now_ms() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
				   std::chrono::system_clock::now().time_since_epoch())
			.count();
	}
};

// Hammers lotman_get_lots_past_del (and friends) from multiple threads
// while another thread feeds the usage-by-dir updater. This mirrors the
// production xrootd purge thread racing against Pelican's GC goroutine.
TEST_F(ConcurrencyTest, ConcurrentPastDelAndUsageUpdate) {
	std::atomic<bool> stop{false};
	ErrorSink sink;
	std::atomic<uint64_t> reader_iters{0};
	std::atomic<uint64_t> writer_iters{0};

	auto past_del_reader = [&]() {
		while (!stop.load(std::memory_order_relaxed)) {
			char **lots = nullptr;
			char *err = nullptr;
			int rv = lotman_get_lots_past_del(now_ms(), true, false, &lots, &err);
			UniqueStringList lots_owner(lots);
			UniqueCString err_owner(err);
			if (rv != 0)
				sink.record("past_del", rv, err);
			reader_iters.fetch_add(1, std::memory_order_relaxed);
		}
	};

	auto usage_by_dir_writer = [&]() {
		const std::string update = R"([
			{"includes_subdirs": false, "num_obj": 1, "path": "parent/child/a", "size_GB": 0.001, "subdirs": []},
			{"includes_subdirs": false, "num_obj": 1, "path": "parent/child/b", "size_GB": 0.001, "subdirs": []}
		])";
		while (!stop.load(std::memory_order_relaxed)) {
			char *err = nullptr;
			int rv = lotman_update_lot_usage_by_dir(update.c_str(), false, now_ms(), &err);
			UniqueCString err_owner(err);
			if (rv != 0)
				sink.record("update_lot_usage_by_dir", rv, err);
			writer_iters.fetch_add(1, std::memory_order_relaxed);
		}
	};

	auto mixed_past_reader = [&]() {
		while (!stop.load(std::memory_order_relaxed)) {
			{
				char **lots = nullptr;
				char *err = nullptr;
				int rv = lotman_get_lots_past_exp(now_ms(), true, false, &lots, &err);
				UniqueStringList lots_owner(lots);
				UniqueCString err_owner(err);
				if (rv != 0)
					sink.record("past_exp", rv, err);
			}
			{
				char **lots = nullptr;
				char *err = nullptr;
				int rv = lotman_get_lots_past_opp(true, true, false, &lots, false, &err);
				UniqueStringList lots_owner(lots);
				UniqueCString err_owner(err);
				if (rv != 0)
					sink.record("past_opp", rv, err);
			}
			{
				char **lots = nullptr;
				char *err = nullptr;
				int rv = lotman_get_lots_past_ded(true, true, false, &lots, false, &err);
				UniqueStringList lots_owner(lots);
				UniqueCString err_owner(err);
				if (rv != 0)
					sink.record("past_ded", rv, err);
			}
			reader_iters.fetch_add(3, std::memory_order_relaxed);
		}
	};

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i)
		threads.emplace_back(past_del_reader);
	threads.emplace_back(usage_by_dir_writer);
	threads.emplace_back(mixed_past_reader);

	std::this_thread::sleep_for(concurrency_test_duration());
	stop.store(true, std::memory_order_relaxed);
	for (auto &t : threads)
		t.join();

	EXPECT_EQ(sink.count.load(), 0) << "first error: " << sink.first;
	EXPECT_GT(reader_iters.load(), 0u);
	EXPECT_GT(writer_iters.load(), 0u);
}

// Mixes read-only queries with mutation-style writes such as those the
// Pelican Go side performs (lot_exists, get_lot_as_json,
// get_children_names, update_lot_usage). Catches read/write races
// outside the past_* family.
TEST_F(ConcurrencyTest, ConcurrentReadersAndMutators) {
	std::atomic<bool> stop{false};
	ErrorSink sink;

	auto reader_basic = [&]() {
		while (!stop.load(std::memory_order_relaxed)) {
			{
				char *err = nullptr;
				int rv = lotman_lot_exists("parent", &err);
				UniqueCString err_owner(err);
				if (rv < 0)
					sink.record("lot_exists", rv, err);
			}
			{
				char *out = nullptr;
				char *err = nullptr;
				int rv = lotman_get_lot_as_json("parent", true, &out, &err);
				UniqueCString out_owner(out);
				UniqueCString err_owner(err);
				if (rv != 0)
					sink.record("get_lot_as_json", rv, err);
			}
		}
	};

	auto reader_children = [&]() {
		while (!stop.load(std::memory_order_relaxed)) {
			char **list = nullptr;
			char *err = nullptr;
			int rv = lotman_get_children_names("parent", true, false, &list, &err);
			UniqueStringList list_owner(list);
			UniqueCString err_owner(err);
			if (rv != 0)
				sink.record("get_children_names", rv, err);
		}
	};

	auto writer_update_usage = [&]() {
		const std::string usage = R"({
			"lot_name": "child",
			"self_GB": 0.001,
			"self_objects": 1,
			"self_GB_being_written": 0.0,
			"self_objects_being_written": 0
		})";
		while (!stop.load(std::memory_order_relaxed)) {
			char *err = nullptr;
			int rv = lotman_update_lot_usage(usage.c_str(), true, &err);
			UniqueCString err_owner(err);
			if (rv != 0)
				sink.record("update_lot_usage", rv, err);
		}
	};

	std::vector<std::thread> threads;
	for (int i = 0; i < 3; ++i)
		threads.emplace_back(reader_basic);
	for (int i = 0; i < 2; ++i)
		threads.emplace_back(reader_children);
	threads.emplace_back(writer_update_usage);

	std::this_thread::sleep_for(concurrency_test_duration());
	stop.store(true, std::memory_order_relaxed);
	for (auto &t : threads)
		t.join();

	EXPECT_EQ(sink.count.load(), 0) << "first error: " << sink.first;
}

} // namespace
