/**
 * Regression tests for usage-attribution correctness across UUID-lot
 * generation rotations.
 *
 * Background
 * ----------
 * Pelican mints fresh UUID-named lots for each managed namespace on a
 * rotating schedule. A typical sequence for `/my-prefix/` is:
 *
 *     Gen A:  creation=[T-60s, T)
 *     Gen B:  creation=[T,     T+60s)
 *     Gen C:  creation=[T+60s, T+120s)
 *
 * The xrootd cache plugin's purge thread periodically calls
 * `lotman_update_lot_usage_by_dir(..., deltaMode=false, query_time=now)`
 * to push the on-disk byte counts into lotman.
 *
 * Before this fix, `get_lots_from_dir` (the helper used to attribute a
 * directory to a lot during usage updates) hard-filtered out any lot
 * whose [creation_time, expiration_time) window did not contain
 * query_time, and silently fell back to the synthetic "default" lot
 * when nothing matched. Two real-world scenarios hit that path:
 *
 *   1. Bootstrap. The cache plugin's first purge tick can fire before
 *      Pelican's renewal goroutine has minted any generation. On-disk
 *      bytes get attributed to "default" and stay there permanently,
 *      because subsequent absolute-mode updates do not redistribute
 *      already-attributed bytes off the default lot.
 *
 *   2. Rotation gap. If the planner ever produces non-abutting
 *      generations (e.g. the expiration_time of Gen A is strictly less
 *      than the creation_time of Gen B), a purge tick that lands in
 *      the gap mis-attributes to "default" the same way.
 *
 * The fix adds a `for_attribution=true` mode to `get_lots_from_dir`
 * that re-runs the longest-prefix path match without the active-window
 * restriction when the strict query returns no result. The
 * temporally-closest covering lot wins. Reclaimed lots remain
 * excluded.
 *
 * These tests would have caught the production misattribution where a
 * 200MB file under `/my-prefix/` was billed to "default" despite three
 * generations of `/my-prefix/` UUID lots existing in the database.
 */

#include "../src/lotman.h"
#include "test_utils.h"

#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <string>

using json = nlohmann::json;

namespace {

class AttributionTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	void SetUp() override {
		tmp_dir = create_temp_directory("lotman_attribution_test");

		// Use a fresh err* per call so a non-null err set by one call is
		// not silently leaked (or, worse, mistakenly passed in to the
		// next call which expects out-only semantics). Free in-between.
		{
			char *err = nullptr;
			ASSERT_EQ(0, lotman_set_context_str("lot_home", tmp_dir.c_str(), &err)) << (err ? err : "");
			if (err) {
				free(err);
			}
		}
		{
			char *err = nullptr;
			ASSERT_EQ(0, lotman_set_context_str("caller", "owner1", &err)) << (err ? err : "");
			if (err) {
				free(err);
			}
		}

		// The default lot must always exist before any other lot can be
		// created. It is the synthetic fallback used by get_lots_from_dir
		// when no covering lot can be found, and lotman_add_lot refuses
		// to create any other lot until it is present.
		addLot(R"({
			"lot_name": "default",
			"owner": "owner1",
			"parents": ["default"],
			"paths": [{"path": "/", "recursive": false}],
			"management_policy_attrs": {
				"dedicated_GB": 1000000, "opportunistic_GB": -1,
				"max_num_objects": -1,
				"creation_time": 0, "expiration_time": 0, "deletion_time": 0
			}
		})");
	}

	void TearDown() override {
		std::filesystem::remove_all(tmp_dir);
	}

	void addLot(const std::string &lot_json) {
		char *err = nullptr;
		int rv = lotman_add_lot(lot_json.c_str(), &err);
		ASSERT_EQ(0, rv) << "addLot failed: " << (err ? err : "<null>") << " for json: " << lot_json;
		if (err) {
			free(err);
		}
	}

	// Push absolute usage for /my-prefix/some.bin = bytes_GB, evaluated at
	// query_time_ms. The cache plugin always frames its updates with the
	// leaf file's parent directory at the top of the JSON tree.
	void pushUsage(double bytes_GB, int64_t query_time_ms) {
		json update = json::array();
		json top;
		top["path"] = "/my-prefix";
		top["size_GB"] = bytes_GB;
		top["includes_subdirs"] = false;
		update.push_back(top);

		char *err = nullptr;
		int rv = lotman_update_lot_usage_by_dir(update.dump().c_str(), /*deltaMode=*/false, query_time_ms, &err);
		ASSERT_EQ(0, rv) << "pushUsage failed: " << (err ? err : "<null>");
		if (err) {
			free(err);
		}
	}

	// Read back self_GB for `lot_name` straight from the SQLite db.
	double readSelfGB(const std::string &lot_name) {
		auto db = open_sqlite3_db(tmp_dir + "/.lot/lotman_cpp.sqlite");
		sqlite3_stmt *stmt = nullptr;
		int rc = sqlite3_prepare_v2(db.get(), "SELECT self_GB FROM lot_usage WHERE lot_name = ?;", -1, &stmt, nullptr);
		if (rc != SQLITE_OK) {
			ADD_FAILURE() << "prepare failed: " << sqlite3_errmsg(db.get());
			return -1.0;
		}
		sqlite3_bind_text(stmt, 1, lot_name.c_str(), -1, SQLITE_TRANSIENT);
		double out = 0.0;
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
			out = sqlite3_column_double(stmt, 0);
		} else if (rc != SQLITE_DONE) {
			ADD_FAILURE() << "step failed: " << sqlite3_errmsg(db.get());
			out = -1.0;
		} else {
			// No row at all — treat as "lot has no usage record".
			out = 0.0;
		}
		sqlite3_finalize(stmt);
		return out;
	}
};

// The bootstrap case. The cache plugin has on-disk bytes under a managed
// namespace, but the very first purge tick fires before any UUID
// generation has been minted for that namespace. The on-disk bytes
// belong to the namespace owner; they must NOT be permanently parked on
// the default lot, because subsequent absolute-mode updates won't
// redistribute them.
//
// Before the fix: this would put 0.2 GB on `default`, and even after
// Gen B is minted the bytes stay on `default`.
//
// After the fix: the very first update has nowhere to land (no covering
// lot exists yet at any time) and falls through to `default`, but as
// soon as Gen B (or any covering lot) exists in the DB, subsequent
// updates correctly attribute to it. This test exercises the second
// half — the on-disk state at the time of the first non-trivial purge
// tick.
TEST_F(AttributionTest, AbsoluteUpdateAttributesToCoveringLotNotDefault) {
	const int64_t T = test_now_ms();

	// Gen B for `/my-prefix/`, currently active.
	std::string gen_b = R"({
		"lot_name": "gen-b",
		"owner": "owner1",
		"parents": ["gen-b"],
		"paths": [{"path": "/my-prefix", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": )" +
						std::to_string(T - 5000) + R"(, "expiration_time": )" + std::to_string(T + 55000) +
						R"(, "deletion_time": )" + std::to_string(T + 115000) + R"(
		}
	})";
	addLot(gen_b);

	pushUsage(/*bytes_GB=*/0.2, /*query_time_ms=*/T);

	EXPECT_NEAR(0.2, readSelfGB("gen-b"), 1e-9)
		<< "Active covering lot must receive the bytes when its window contains query_time.";
	EXPECT_NEAR(0.0, readSelfGB("default"), 1e-9) << "default lot must not be touched when a covering lot exists.";
}

// The exact production scenario. The query_time happens to fall in a
// hairline gap between two generations: Gen A's expiration_time is
// strictly less than Gen B's creation_time. Without the fix, the
// active-window filter excludes both and we fall back to default. With
// the fix, the temporally-closest covering lot wins (here, Gen A whose
// expiration is closest to query_time).
TEST_F(AttributionTest, AbsoluteUpdateInRotationGapAttributesToNearestLot) {
	const int64_t T = test_now_ms();
	const int64_t gap_start = T - 1; // Gen A's expiration_time.
	const int64_t gap_end = T + 1;	 // Gen B's creation_time.

	// Gen A: ended 1ms ago.
	addLot(std::string(R"({
		"lot_name": "gen-a",
		"owner": "owner1",
		"parents": ["gen-a"],
		"paths": [{"path": "/my-prefix", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": -1, "max_num_objects": -1,
			"creation_time": )") +
		   std::to_string(gap_start - 60000) + R"(, "expiration_time": )" + std::to_string(gap_start) +
		   R"(, "deletion_time": )" + std::to_string(gap_start + 60000) + R"(
		}
	})");

	// Gen B: starts 1ms from now.
	addLot(std::string(R"({
		"lot_name": "gen-b",
		"owner": "owner1",
		"parents": ["gen-b"],
		"paths": [{"path": "/my-prefix", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": -1, "max_num_objects": -1,
			"creation_time": )") +
		   std::to_string(gap_end) + R"(, "expiration_time": )" + std::to_string(gap_end + 60000) +
		   R"(, "deletion_time": )" + std::to_string(gap_end + 120000) + R"(
		}
	})");

	pushUsage(/*bytes_GB=*/0.2, /*query_time_ms=*/T);

	const double on_default = readSelfGB("default");
	const double on_a = readSelfGB("gen-a");
	const double on_b = readSelfGB("gen-b");

	EXPECT_NEAR(0.0, on_default, 1e-9) << "default must not receive bytes when a covering lot exists at any time. "
										  "default="
									   << on_default << " gen-a=" << on_a << " gen-b=" << on_b;

	// Stronger assertion than "either covering lot". Bytes physically on
	// disk at query_time can only have been written by a generation that
	// already existed -- never by a generation whose creation_time is in
	// the future. The fallback ORDER BY therefore prefers
	// `creation_time <= query_time` (Gen A here) over a future lot whose
	// creation_time happens to be numerically closer (Gen B's
	// |T-(T+1)|=1 vs Gen A's |T-(T-60001)|=60001). Without that
	// preference the bytes would be attributed to a not-yet-minted lot,
	// which makes no physical sense.
	EXPECT_NEAR(0.2, on_a, 1e-9) << "Just-expired generation (which physically wrote the bytes) must win "
									"over a not-yet-started future generation. "
									"gen-a="
								 << on_a << " gen-b=" << on_b;
	EXPECT_NEAR(0.0, on_b, 1e-9) << "Future generation (creation_time > query_time) must not own bytes "
									"that physically existed before it was minted. "
									"gen-a="
								 << on_a << " gen-b=" << on_b;
}

// Sanity check: the "for_attribution" fallback does not change behaviour
// when the active window does contain query_time. We use the exact same
// shape as the production DB (three generations spanning T-60s..T+120s)
// and verify the currently-active generation gets the bytes.
TEST_F(AttributionTest, AbsoluteUpdateAttributesToActiveGenAcrossThree) {
	const int64_t T = test_now_ms();

	auto mkGen = [&](const char *name, int64_t create, int64_t expire, int64_t del) {
		std::string j = std::string(R"({
			"lot_name": ")") +
						name + R"(",
			"owner": "owner1",
			"parents": [")" +
						name + R"("],
			"paths": [{"path": "/my-prefix", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 1, "opportunistic_GB": -1, "max_num_objects": -1,
				"creation_time": )" +
						std::to_string(create) + R"(, "expiration_time": )" + std::to_string(expire) +
						R"(, "deletion_time": )" + std::to_string(del) + R"(
			}
		})";
		addLot(j);
	};

	mkGen("gen-a", T - 60000, T - 1, T + 60000);	   // ended just before now
	mkGen("gen-b", T - 1, T + 60000, T + 120000);	   // currently active (contains T)
	mkGen("gen-c", T + 60000, T + 120000, T + 180000); // future

	pushUsage(/*bytes_GB=*/0.2, /*query_time_ms=*/T);

	EXPECT_NEAR(0.0, readSelfGB("default"), 1e-9);
	EXPECT_NEAR(0.0, readSelfGB("gen-a"), 1e-9);
	EXPECT_NEAR(0.2, readSelfGB("gen-b"), 1e-9) << "Currently-active generation must be preferred over fallback.";
	EXPECT_NEAR(0.0, readSelfGB("gen-c"), 1e-9);
}

// Defensive: if a path is truly uncovered (no lot owns it at any time),
// the legacy behaviour (fall through to default) must still kick in.
// Otherwise the cache loses visibility into uncategorised bytes.
TEST_F(AttributionTest, UncoveredPathStillFallsThroughToDefault) {
	const int64_t T = test_now_ms();

	// Add a covering lot for a DIFFERENT path. /my-prefix/ remains uncovered.
	addLot(std::string(R"({
		"lot_name": "covers-other",
		"owner": "owner1",
		"parents": ["covers-other"],
		"paths": [{"path": "/something-else", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": -1, "max_num_objects": -1,
			"creation_time": )") +
		   std::to_string(T - 5000) + R"(, "expiration_time": )" + std::to_string(T + 55000) +
		   R"(, "deletion_time": )" + std::to_string(T + 115000) + R"(
		}
	})");

	pushUsage(/*bytes_GB=*/0.2, /*query_time_ms=*/T);

	EXPECT_NEAR(0.2, readSelfGB("default"), 1e-9)
		<< "Truly uncovered path must continue to fall back to the default lot.";
}

// Regression for the "root.children_GB keeps growing forever" bug.
//
// Production scenario captured in /workspaces/pelican/.lot:
//   * Three back-to-back generations of UUID lots covering /my-prefix/,
//     /my-prefix/sub-prefix/, and /my-prefix2/ are minted in sequence.
//   * Each generation has ~0.21 GB pushed into its self_GB while it is
//     the active generation, then the generation is reclaimed and
//     replaced by the next one.
//   * After three rotations, the production db showed root.children_GB
//     = 6 * 0.21 GB ≈ 1.26 GB, i.e. every generation that had EVER
//     existed under root was still being summed -- including the
//     reclaimed ones whose accounting tie to their paths had been
//     severed in update_usage_by_dirs (which correctly skips reclaimed
//     lots when writing self_*).
//
// Root cause: `recalculate_children_usage` issued
//     SELECT SUM(self_GB) FROM lot_usage WHERE lot_name IN (...children...)
// with no reclamation filter, while `get_children` (which populates the
// IN-list) also did not filter reclaimed entries. So once a lot was
// reclaimed its frozen self_GB kept contributing to every ancestor's
// children_GB on every subsequent recompute, forever.
//
// Fix: `recalculate_children_usage` now LEFT-JOINs reclamations and
// keeps only rows with `r.lot_name IS NULL` (matching the idiom used
// elsewhere in this file for active-lot queries).
TEST_F(AttributionTest, ChildrenGBSumExcludesReclaimedDescendantsAfterRotation) {
	const int64_t T = test_now_ms();

	// "default" already exists from SetUp and is its own root. Make our
	// two generations children of default so that default.children_GB is
	// the aggregation we want to inspect (mirrors the production "root"
	// lot's relationship to its UUID grandchildren).
	auto mkGen = [&](const char *name, int64_t create, int64_t expire, int64_t del) {
		std::string j = std::string(R"({
			"lot_name": ")") +
						name + R"(",
			"owner": "owner1",
			"parents": ["default"],
			"paths": [{"path": "/my-prefix", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 1, "opportunistic_GB": -1, "max_num_objects": -1,
				"creation_time": )" +
						std::to_string(create) + R"(, "expiration_time": )" + std::to_string(expire) +
						R"(, "deletion_time": )" + std::to_string(del) + R"(
			}
		})";
		addLot(j);
	};

	// Gen A: ran from T-120s to T-60s.
	mkGen("gen-a", T - 120000, T - 60000, T);
	// Gen B: currently active.
	mkGen("gen-b", T - 60000, T + 60000, T + 120000);

	// Drop 0.2 GB into Gen A back when it was active.
	pushUsage(/*bytes_GB=*/0.2, /*query_time_ms=*/T - 90000);
	ASSERT_NEAR(0.2, readSelfGB("gen-a"), 1e-9) << "Sanity: Gen A should hold the bytes pushed during its window.";

	// Reclaim Gen A. From now on its self_GB must not contribute to any
	// ancestor's children_GB roll-up. The reclamations table is keyed
	// purely by lot_name, so any future recompute that joins against it
	// will see Gen A as reclaimed.
	{
		char *err = nullptr;
		int rv = lotman_reclaim_lot("gen-a", T - 30000, "exp", &err);
		ASSERT_EQ(0, rv) << "reclaim failed: " << (err ? err : "<null>");
		if (err) {
			free(err);
		}
	}

	// Drop 0.2 GB into Gen B while it is active.
	pushUsage(/*bytes_GB=*/0.2, /*query_time_ms=*/T);
	ASSERT_NEAR(0.2, readSelfGB("gen-b"), 1e-9) << "Sanity: Gen B should hold the bytes pushed during its window.";

	// Trigger recalculation of children_GB across the db. The simplest
	// public hook that calls update_db_children_usage is
	// lotman_get_lot_as_json. We discard the JSON output here; we only
	// care about the side-effect of recomputing lot_usage.children_GB.
	{
		char *out = nullptr;
		char *err = nullptr;
		int rv = lotman_get_lot_as_json("default", /*recursive=*/true, &out, &err);
		ASSERT_EQ(0, rv) << "get_lot_as_json failed: " << (err ? err : "<null>");
		if (out) {
			free(out);
		}
		if (err) {
			free(err);
		}
	}

	// Read children_GB directly from the db, same pattern as readSelfGB.
	auto readChildrenGB = [&](const std::string &lot_name) {
		auto db = open_sqlite3_db(tmp_dir + "/.lot/lotman_cpp.sqlite");
		sqlite3_stmt *stmt = nullptr;
		int rc =
			sqlite3_prepare_v2(db.get(), "SELECT children_GB FROM lot_usage WHERE lot_name = ?;", -1, &stmt, nullptr);
		EXPECT_EQ(SQLITE_OK, rc) << sqlite3_errmsg(db.get());
		sqlite3_bind_text(stmt, 1, lot_name.c_str(), -1, SQLITE_TRANSIENT);
		double out = 0.0;
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
			out = sqlite3_column_double(stmt, 0);
		}
		sqlite3_finalize(stmt);
		return out;
	};

	const double default_children = readChildrenGB("default");
	const double gen_a_self = readSelfGB("gen-a");
	const double gen_b_self = readSelfGB("gen-b");

	// Gen A's frozen self_GB must still be intact in the lot_usage row
	// (reclamation is a ledger, not a destructor); we are only asserting
	// it is excluded from the *aggregation*.
	EXPECT_NEAR(0.2, gen_a_self, 1e-9) << "Reclamation should not zero out a lot's frozen self_GB.";
	EXPECT_NEAR(0.2, gen_b_self, 1e-9);

	// The core assertion: default.children_GB must count Gen B's 0.2 GB
	// only, NOT also Gen A's 0.2 GB. Without the fix this comes back as
	// 0.4 GB and would scale linearly with the number of historical
	// generations.
	EXPECT_NEAR(0.2, default_children, 1e-9)
		<< "children_GB roll-up must exclude reclaimed descendants. "
		   "default.children_GB="
		<< default_children << " gen-a.self_GB=" << gen_a_self << " gen-b.self_GB=" << gen_b_self
		<< " (expected 0.2 = just Gen B; got 0.4 means Gen A leaked through)";
}

} // namespace
