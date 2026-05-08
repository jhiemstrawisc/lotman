/**
 * Reclamation Ledger Tests for LotMan
 *
 * These tests cover the dedicated `reclamations` table introduced as a
 * pure ledger of "this lot has been reclaimed" facts. They exercise the
 * `lotman_reclaim_lot` C API plus the reclamation-aware filters added to
 * the various read/write paths.
 *
 * Coverage includes:
 *   * Happy-path reclaim writes a row, cascades to descendants, and emits
 *     a `reclamation` block via `lotman_get_lot_as_json`.
 *   * Re-reclaim of an already-reclaimed lot returns the already-reclaimed
 *     status and leaves the original row untouched.
 *   * Cascades skip already-reclaimed descendants and continue reclaiming
 *     unreclaimed lots.
 *   * Authorization (caller must own the root).
 *   * Argument validation (null lot, non-existent lot, default lot,
 *     non-positive `reclaimed_at`).
 *   * Mutation guards on `update_lot`, `update_lot_usage`, `add_to_lot`,
 *     `rm_parents_from_lot`, `rm_paths_from_lots`.
 *   * `update_lot_usage_by_dir` silently skips reclaimed lots.
 *   * `lotman_get_lots_from_dir` excludes reclaimed lots.
 *   * The five `past_*` queries respect the `include_reclaimed` flag.
 *   * Future-dated reclamations remain "scheduled" until the wall-clock
 *     reaches `reclaimed_at` (filters use `reclaimed_at <= now`).
 *   * `lotman_remove_lot` cleans up the reclamation row.
 */

#include "../src/lotman.h"
#include "../src/lotman_db.h"
#include "test_utils.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr int64_t PAST_TS = 1; // Always in the past relative to wall-clock.
// A long way into the future (year ~2286): use as "scheduled" reclamation.
constexpr int64_t FUTURE_TS = 9999999999999LL;

class ReclamationTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	void SetUp() override {
		tmp_dir = create_temp_directory("lotman_reclaim_test");
		char *raw_err = nullptr;
		ASSERT_EQ(lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err), 0) << (raw_err ? raw_err : "");
		free(raw_err);
		raw_err = nullptr;
		ASSERT_EQ(lotman_set_context_str("caller", "owner1", &raw_err), 0) << (raw_err ? raw_err : "");
		free(raw_err);
		raw_err = nullptr;
		ASSERT_EQ(lotman_set_context_str("strict_hierarchy", "false", &raw_err), 0) << (raw_err ? raw_err : "");
		free(raw_err);
		raw_err = nullptr;
		ASSERT_EQ(lotman_set_context_str("contraction_policy", "none", &raw_err), 0) << (raw_err ? raw_err : "");
		free(raw_err);
	}

	void TearDown() override {
		std::filesystem::remove_all(tmp_dir);
	}

	void addLot(const std::string &lot_json) {
		char *raw_err = nullptr;
		int rv = lotman_add_lot(lot_json.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "addLot failed: " << (err.get() ? err.get() : "");
	}

	void addDefaultLot() {
		addLot(R"({
			"lot_name": "default",
			"owner": "owner1",
			"parents": ["default"],
			"paths": [{"path": "/default", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 1000,
				"opportunistic_GB": 500,
				"max_num_objects": 10000,
				"creation_time": 100,
				"expiration_time": 9999999999999,
				"deletion_time": 9999999999999
			}
		})");
	}

	// Build a small tree: parent → mid → leaf, all owned by owner1, all with
	// generous, non-expired, non-deleted MPA windows (so they don't leak into
	// past_exp / past_del queries unless we mean them to).
	void addParentMidLeaf() {
		addLot(R"({
			"lot_name": "parent",
			"owner": "owner1",
			"parents": ["parent"],
			"paths": [{"path": "/parent", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 1000,
				"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
			}
		})");
		addLot(R"({
			"lot_name": "mid",
			"owner": "owner1",
			"parents": ["parent"],
			"paths": [{"path": "/parent/mid", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 50, "opportunistic_GB": 25, "max_num_objects": 500,
				"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
			}
		})");
		addLot(R"({
			"lot_name": "leaf",
			"owner": "owner1",
			"parents": ["mid"],
			"paths": [{"path": "/parent/mid/leaf", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
				"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
			}
		})");
	}

	bool hasReclamationRow(const std::string &lot_name) {
		char *raw_err = nullptr;
		char *raw_out = nullptr;
		int rv = lotman_get_lot_as_json(lot_name.c_str(), false, &raw_out, &raw_err);
		UniqueCString err(raw_err);
		UniqueCString out(raw_out);
		EXPECT_EQ(rv, 0) << "get_lot_as_json failed for " << lot_name << ": " << (err.get() ? err.get() : "");
		if (rv != 0 || !out.get())
			return false;
		auto parsed = json::parse(std::string(out.get()), nullptr, false);
		if (parsed.is_discarded())
			return false;
		return parsed.contains("reclamation");
	}

	json getReclamationRow(const std::string &lot_name) {
		char *raw_err = nullptr;
		char *raw_out = nullptr;
		int rv = lotman_get_lot_as_json(lot_name.c_str(), false, &raw_out, &raw_err);
		UniqueCString err(raw_err);
		UniqueCString out(raw_out);
		EXPECT_EQ(rv, 0) << "get_lot_as_json failed for " << lot_name << ": " << (err.get() ? err.get() : "");
		if (rv != 0 || !out.get())
			return json();
		auto parsed = json::parse(std::string(out.get()), nullptr, false);
		if (parsed.is_discarded() || !parsed.contains("reclamation"))
			return json();
		return parsed["reclamation"];
	}

	int reclaim(const std::string &lot_name, int64_t ts, const std::string &reason, std::string *err_out = nullptr) {
		char *raw_err = nullptr;
		int rv = lotman_reclaim_lot(lot_name.c_str(), ts, reason.c_str(), &raw_err);
		UniqueCString err(raw_err);
		if (err_out)
			*err_out = err.get() ? err.get() : "";
		return rv;
	}
};

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, RejectsNullLotName) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_reclaim_lot(nullptr, PAST_TS, "test", &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.get(), nullptr);
}

TEST_F(ReclamationTest, RejectsNonPositiveReclaimedAt) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	EXPECT_NE(reclaim("parent", 0, "test", &err), 0);
	EXPECT_FALSE(err.empty());
	EXPECT_NE(reclaim("parent", -1, "test", &err), 0);
	EXPECT_FALSE(err.empty());
	// And no row should have been created.
	EXPECT_FALSE(hasReclamationRow("parent"));
}

TEST_F(ReclamationTest, RejectsUnknownLot) {
	addDefaultLot();
	std::string err;
	EXPECT_NE(reclaim("does_not_exist", PAST_TS, "test", &err), 0);
	EXPECT_FALSE(err.empty());
}

TEST_F(ReclamationTest, RejectsDefaultLot) {
	addDefaultLot();
	std::string err;
	EXPECT_NE(reclaim("default", PAST_TS, "test", &err), 0);
	EXPECT_FALSE(err.empty());
	EXPECT_FALSE(hasReclamationRow("default"));
}

TEST_F(ReclamationTest, RequiresOwnerCaller) {
	addDefaultLot();
	addParentMidLeaf();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_set_context_str("caller", "stranger", &raw_err), 0);
	free(raw_err);
	std::string err;
	EXPECT_NE(reclaim("parent", PAST_TS, "denied", &err), 0);
	EXPECT_FALSE(err.empty());
	EXPECT_FALSE(hasReclamationRow("parent"));
}

// ---------------------------------------------------------------------------
// Happy path + cascade
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, ReclaimCascadesToAllDescendants) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("parent", PAST_TS, "EXPIRED", &err), 0) << err;
	EXPECT_TRUE(hasReclamationRow("parent"));
	EXPECT_TRUE(hasReclamationRow("mid"));
	EXPECT_TRUE(hasReclamationRow("leaf"));
}

TEST_F(ReclamationTest, ReclaimLeafOnlyDoesNotTouchAncestors) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;
	EXPECT_FALSE(hasReclamationRow("parent"));
	EXPECT_FALSE(hasReclamationRow("mid"));
	EXPECT_TRUE(hasReclamationRow("leaf"));
}

TEST_F(ReclamationTest, GetLotAsJsonEmitsReclamationBlock) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", 1234567, "EXPIRED", &err), 0) << err;

	char *raw_err = nullptr;
	char *raw_out = nullptr;
	ASSERT_EQ(lotman_get_lot_as_json("leaf", false, &raw_out, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueCString out(raw_out);
	auto parsed = json::parse(std::string(out.get()));
	ASSERT_TRUE(parsed.contains("reclamation"));
	EXPECT_EQ(parsed["reclamation"]["reclaimed_at"].get<int64_t>(), 1234567);
	EXPECT_EQ(parsed["reclamation"]["reason"].get<std::string>(), "EXPIRED");

	// A non-reclaimed sibling has no `reclamation` key.
	raw_err = nullptr;
	char *raw_out2 = nullptr;
	ASSERT_EQ(lotman_get_lot_as_json("parent", false, &raw_out2, &raw_err), 0);
	UniqueCString cleanup_err2(raw_err);
	UniqueCString out2(raw_out2);
	auto parsed2 = json::parse(std::string(out2.get()));
	EXPECT_FALSE(parsed2.contains("reclamation"));
}

// ---------------------------------------------------------------------------
// Rereclaim semantics
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, IdempotentReReclaimWithMatchingTuple) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", 42, "EXPIRED", &err), LOTMAN_RECLAIM_OK) << err;
	EXPECT_EQ(reclaim("leaf", 42, "EXPIRED", &err), LOTMAN_RECLAIM_ALREADY_RECLAIMED) << err;
	EXPECT_TRUE(hasReclamationRow("leaf"));
}

TEST_F(ReclamationTest, ReReclaimWithDifferentTimestampSkipsExistingRow) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", 42, "EXPIRED", &err), LOTMAN_RECLAIM_OK) << err;
	EXPECT_EQ(reclaim("leaf", 43, "EXPIRED", &err), LOTMAN_RECLAIM_ALREADY_RECLAIMED);

	// Original ledger row must be unchanged.
	auto row = getReclamationRow("leaf");
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["reclaimed_at"].get<int64_t>(), 42);
	EXPECT_EQ(row["reason"].get<std::string>(), "EXPIRED");
}

TEST_F(ReclamationTest, ReReclaimWithDifferentReasonSkipsExistingRow) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", 42, "EXPIRED", &err), LOTMAN_RECLAIM_OK) << err;
	EXPECT_EQ(reclaim("leaf", 42, "DELETED", &err), LOTMAN_RECLAIM_ALREADY_RECLAIMED);
	auto row = getReclamationRow("leaf");
	ASSERT_FALSE(row.is_null());
	EXPECT_EQ(row["reclaimed_at"].get<int64_t>(), 42);
	EXPECT_EQ(row["reason"].get<std::string>(), "EXPIRED");
}

TEST_F(ReclamationTest, CascadeSkipsAlreadyReclaimedDescendant) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	// Pre-reclaim the leaf with a specific tuple.
	ASSERT_EQ(reclaim("leaf", 100, "EXPIRED", &err), LOTMAN_RECLAIM_OK) << err;

	// Now cascade-reclaim from `parent` with a different tuple. The existing
	// leaf row is skipped, while parent and mid receive new rows.
	EXPECT_EQ(reclaim("parent", 200, "RECURSIVE", &err), LOTMAN_RECLAIM_OK) << err;

	EXPECT_TRUE(hasReclamationRow("parent"));
	EXPECT_TRUE(hasReclamationRow("mid"));
	auto parent_row = getReclamationRow("parent");
	auto mid_row = getReclamationRow("mid");
	auto leaf_row = getReclamationRow("leaf");
	ASSERT_FALSE(parent_row.is_null());
	ASSERT_FALSE(mid_row.is_null());
	ASSERT_FALSE(leaf_row.is_null());
	EXPECT_EQ(parent_row["reclaimed_at"].get<int64_t>(), 200);
	EXPECT_EQ(mid_row["reclaimed_at"].get<int64_t>(), 200);
	EXPECT_EQ(leaf_row["reclaimed_at"].get<int64_t>(), 100);
	EXPECT_EQ(leaf_row["reason"].get<std::string>(), "EXPIRED");
}

TEST_F(ReclamationTest, ReclaimAllAlreadyReclaimedSubtreeReturnsAlreadyReclaimed) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("parent", 100, "EXPIRED", &err), LOTMAN_RECLAIM_OK) << err;

	EXPECT_EQ(reclaim("parent", 200, "RECURSIVE", &err), LOTMAN_RECLAIM_ALREADY_RECLAIMED);

	auto parent_row = getReclamationRow("parent");
	auto mid_row = getReclamationRow("mid");
	auto leaf_row = getReclamationRow("leaf");
	ASSERT_FALSE(parent_row.is_null());
	ASSERT_FALSE(mid_row.is_null());
	ASSERT_FALSE(leaf_row.is_null());
	EXPECT_EQ(parent_row["reclaimed_at"].get<int64_t>(), 100);
	EXPECT_EQ(mid_row["reclaimed_at"].get<int64_t>(), 100);
	EXPECT_EQ(leaf_row["reclaimed_at"].get<int64_t>(), 100);
}

// ---------------------------------------------------------------------------
// Mutation guards
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, UpdateLotRejectsReclaimedTarget) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	char *raw_err = nullptr;
	int rv = lotman_update_lot(R"({"lot_name":"leaf","management_policy_attrs":{"dedicated_GB":99}})", &raw_err);
	UniqueCString eg(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(eg.get(), nullptr);
}

TEST_F(ReclamationTest, UpdateLotUsageRejectsReclaimedTarget) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(R"({"lot_name":"leaf","self_GB":1.0})", false, &raw_err);
	UniqueCString eg(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(eg.get(), nullptr);
}

TEST_F(ReclamationTest, AddToLotRejectsReclaimedTarget) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	char *raw_err = nullptr;
	int rv = lotman_add_to_lot(R"({"lot_name":"leaf","paths":[{"path":"/extra","recursive":true}]})", &raw_err);
	UniqueCString eg(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(eg.get(), nullptr);
}

TEST_F(ReclamationTest, RmParentsFromLotRejectsReclaimedTarget) {
	addDefaultLot();
	addParentMidLeaf();
	// Give `leaf` a second parent so the rm has something legitimate to do.
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_to_lot(R"({"lot_name":"leaf","parents":["parent"]})", &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	int rv = lotman_rm_parents_from_lot(R"({"lot_name":"leaf","parents":["parent"]})", &raw_err);
	UniqueCString eg(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(eg.get(), nullptr);
}

TEST_F(ReclamationTest, RmPathsFromLotsRejectsReclaimedTarget) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	char *raw_err = nullptr;
	int rv = lotman_rm_paths_from_lots(R"({"paths":["/parent/mid/leaf"]})", &raw_err);
	UniqueCString eg(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(eg.get(), nullptr);
}

// ---------------------------------------------------------------------------
// `update_lot_usage_by_dir` skips reclaimed lots silently
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, UpdateLotUsageByDirSkipsReclaimedSilently) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	// A live (`mid`) and a reclaimed (`leaf`) target in one call. Per the
	// `update_usage_by_dir_schema`, the top-level shape is an array of
	// {path, includes_subdirs, size_GB?, num_obj?, subdirs?} entries.
	const char *update = R"([
		{
			"path": "/parent/mid",
			"includes_subdirs": true,
			"size_GB": 5.0,
			"num_obj": 1,
			"subdirs": [
				{"path": "leaf", "includes_subdirs": false, "size_GB": 7.0, "num_obj": 1, "subdirs": []}
			]
		}
	])";

	int64_t now_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();

	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage_by_dir(update, false, now_ms, &raw_err);
	UniqueCString eg(raw_err);
	EXPECT_EQ(rv, 0) << "update_lot_usage_by_dir should silently skip reclaimed lots: " << (eg.get() ? eg.get() : "");
}

// ---------------------------------------------------------------------------
// `lotman_get_lots_from_dir` filters reclaimed lots
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, GetLotsFromDirExcludesReclaimed) {
	addDefaultLot();
	addParentMidLeaf();

	int64_t now_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();

	// Sanity: leaf claims its dir before reclaim.
	char *raw_err = nullptr;
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_from_dir("/parent/mid/leaf", false, now_ms, &raw_lots, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	EXPECT_TRUE(before.count("leaf")) << "leaf should own its dir before reclaim";

	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;

	raw_err = nullptr;
	char **raw_lots2 = nullptr;
	ASSERT_EQ(lotman_get_lots_from_dir("/parent/mid/leaf", false, now_ms, &raw_lots2, &raw_err), 0);
	UniqueCString cleanup_err2(raw_err);
	UniqueStringList lots2(raw_lots2);
	std::set<std::string> after;
	for (int i = 0; lots2.get()[i]; ++i)
		after.insert(lots2.get()[i]);
	EXPECT_FALSE(after.count("leaf")) << "leaf should be filtered out after reclaim";
}

TEST_F(ReclamationTest, GetLotsFromDirIncludesFutureReclaimedUntilDue) {
	// A future-dated reclaim (reclaimed_at > now) must NOT filter the lot
	// out yet — it represents a scheduled, not-yet-effective reclamation.
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", FUTURE_TS, "SCHEDULED", &err), 0) << err;

	int64_t now_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count();
	char *raw_err = nullptr;
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_from_dir("/parent/mid/leaf", false, now_ms, &raw_lots, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> got;
	for (int i = 0; lots.get()[i]; ++i)
		got.insert(lots.get()[i]);
	EXPECT_TRUE(got.count("leaf")) << "scheduled (future) reclamations must not filter yet";
}

// ---------------------------------------------------------------------------
// past_* queries respect include_reclaimed
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, PastExpQueryRespectsIncludeReclaimed) {
	addDefaultLot();
	// An expired lot (expiration_time well in the past, deletion_time in the future).
	addLot(R"({
		"lot_name": "expired",
		"owner": "owner1",
		"parents": ["expired"],
		"paths": [{"path": "/expired", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 1000, "deletion_time": 9999999999999
		}
	})");

	// Sanity: appears in past_exp before reclaim.
	char *raw_err = nullptr;
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_exp(false, /*include_reclaimed=*/true, &raw_lots, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	ASSERT_TRUE(before.count("expired"));

	// Reclaim it.
	std::string err;
	ASSERT_EQ(reclaim("expired", PAST_TS, "EXPIRED", &err), 0) << err;

	// include_reclaimed=true -> still present.
	raw_err = nullptr;
	char **raw_lots_inc = nullptr;
	ASSERT_EQ(lotman_get_lots_past_exp(false, true, &raw_lots_inc, &raw_err), 0);
	UniqueCString eg1(raw_err);
	UniqueStringList lots_inc(raw_lots_inc);
	std::set<std::string> with_inc;
	for (int i = 0; lots_inc.get()[i]; ++i)
		with_inc.insert(lots_inc.get()[i]);
	EXPECT_TRUE(with_inc.count("expired"));

	// include_reclaimed=false -> filtered out.
	raw_err = nullptr;
	char **raw_lots_exc = nullptr;
	ASSERT_EQ(lotman_get_lots_past_exp(false, false, &raw_lots_exc, &raw_err), 0);
	UniqueCString eg2(raw_err);
	UniqueStringList lots_exc(raw_lots_exc);
	std::set<std::string> without;
	for (int i = 0; lots_exc.get()[i]; ++i)
		without.insert(lots_exc.get()[i]);
	EXPECT_FALSE(without.count("expired"));
}

TEST_F(ReclamationTest, PastDelQueryRespectsIncludeReclaimed) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "deletable",
		"owner": "owner1",
		"parents": ["deletable"],
		"paths": [{"path": "/deletable", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 500, "deletion_time": 1000
		}
	})");

	char *raw_err = nullptr;
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_del(false, true, &raw_lots, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	ASSERT_TRUE(before.count("deletable"));

	std::string err;
	ASSERT_EQ(reclaim("deletable", PAST_TS, "DELETED", &err), 0) << err;

	raw_err = nullptr;
	char **raw_lots_exc = nullptr;
	ASSERT_EQ(lotman_get_lots_past_del(false, false, &raw_lots_exc, &raw_err), 0);
	UniqueCString eg(raw_err);
	UniqueStringList lots_exc(raw_lots_exc);
	std::set<std::string> after;
	for (int i = 0; lots_exc.get()[i]; ++i)
		after.insert(lots_exc.get()[i]);
	EXPECT_FALSE(after.count("deletable"));
}

TEST_F(ReclamationTest, PastDedQueryRespectsIncludeReclaimed) {
	addDefaultLot();
	// A self-parented lot with a tiny dedicated quota that we'll exceed.
	addLot(R"({
		"lot_name": "fat",
		"owner": "owner1",
		"parents": ["fat"],
		"paths": [{"path": "/fat", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1000,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_update_lot_usage(R"({"lot_name":"fat","self_GB":50.0})", false, &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_ded(false, false, /*include_reclaimed=*/true, &raw_lots,
									   /*hierarchical=*/false, &raw_err),
			  0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	ASSERT_TRUE(before.count("fat"));

	std::string err;
	ASSERT_EQ(reclaim("fat", PAST_TS, "EXPIRED", &err), 0) << err;

	raw_err = nullptr;
	char **raw_lots_exc = nullptr;
	ASSERT_EQ(lotman_get_lots_past_ded(false, false, false, &raw_lots_exc, false, &raw_err), 0);
	UniqueCString eg(raw_err);
	UniqueStringList lots_exc(raw_lots_exc);
	std::set<std::string> after;
	for (int i = 0; lots_exc.get()[i]; ++i)
		after.insert(lots_exc.get()[i]);
	EXPECT_FALSE(after.count("fat"));
}

TEST_F(ReclamationTest, PastObjQueryRespectsIncludeReclaimed) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "manyobj",
		"owner": "owner1",
		"parents": ["manyobj"],
		"paths": [{"path": "/manyobj", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 1,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_update_lot_usage(R"({"lot_name":"manyobj","self_objects":1000})", false, &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_obj(false, false, true, &raw_lots, false, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	ASSERT_TRUE(before.count("manyobj"));

	std::string err;
	ASSERT_EQ(reclaim("manyobj", PAST_TS, "EXPIRED", &err), 0) << err;

	raw_err = nullptr;
	char **raw_lots_exc = nullptr;
	ASSERT_EQ(lotman_get_lots_past_obj(false, false, false, &raw_lots_exc, false, &raw_err), 0);
	UniqueCString eg(raw_err);
	UniqueStringList lots_exc(raw_lots_exc);
	std::set<std::string> after;
	for (int i = 0; lots_exc.get()[i]; ++i)
		after.insert(lots_exc.get()[i]);
	EXPECT_FALSE(after.count("manyobj"));
}

TEST_F(ReclamationTest, PastQueriesIncludeFutureScheduledReclamation) {
	// A future-dated reclamation (reclaimed_at > now) must NOT exclude the
	// lot from past_* queries until the wall-clock catches up.
	addDefaultLot();
	addLot(R"({
		"lot_name": "expired",
		"owner": "owner1",
		"parents": ["expired"],
		"paths": [{"path": "/expired", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 1000, "deletion_time": 9999999999999
		}
	})");
	std::string err;
	ASSERT_EQ(reclaim("expired", FUTURE_TS, "SCHEDULED", &err), 0) << err;

	char *raw_err = nullptr;
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_exp(false, /*include_reclaimed=*/false, &raw_lots, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> got;
	for (int i = 0; lots.get()[i]; ++i)
		got.insert(lots.get()[i]);
	EXPECT_TRUE(got.count("expired"))
		<< "future-dated reclamation must not filter past_exp until wall-clock reaches reclaimed_at";
}

// ---------------------------------------------------------------------------
// Removal cleans up the reclamation row
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, RemoveLotAlsoRemovesReclamationRow) {
	addDefaultLot();
	addParentMidLeaf();
	std::string err;
	ASSERT_EQ(reclaim("leaf", PAST_TS, "EXPIRED", &err), 0) << err;
	ASSERT_TRUE(hasReclamationRow("leaf"));

	char *raw_err = nullptr;
	int rv = lotman_remove_lots_recursive("leaf", &raw_err);
	UniqueCString eg(raw_err);
	ASSERT_EQ(rv, 0) << (eg.get() ? eg.get() : "");

	// Lot is gone; ergo, no reclamation row about it can be looked up via
	// `lotman_get_lot_as_json`. Re-add it with the same name and assert no
	// stale `reclamation` block is attached.
	addLot(R"({
		"lot_name": "leaf",
		"owner": "owner1",
		"parents": ["mid"],
		"paths": [{"path": "/parent/mid/leaf", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	EXPECT_FALSE(hasReclamationRow("leaf")) << "Re-created lot must not inherit a stale reclamation row";
}

// ---------------------------------------------------------------------------
// Adding a live child under a reclaimed parent is rejected
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, AddLotRejectedUnderReclaimedParent) {
	// Reclamation is a terminal ledger fact: a reclaimed subtree has been
	// hoovered into the default lot. Adding a live child would resurrect the
	// subtree and break that invariant.
	addDefaultLot();
	addLot(R"({
		"lot_name": "parent",
		"owner": "owner1",
		"parents": ["parent"],
		"paths": [{"path": "/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 1000,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	std::string err;
	ASSERT_EQ(reclaim("parent", PAST_TS, "EXPIRED", &err), 0) << err;
	ASSERT_TRUE(hasReclamationRow("parent"));

	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "graft_attempt",
		"owner": "owner1",
		"parents": ["parent"],
		"paths": [{"path": "/parent/graft", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 10,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})",
							&raw_err);
	UniqueCString eg(raw_err);
	EXPECT_NE(rv, 0) << "lotman_add_lot must reject grafting a live child onto a reclaimed parent";
	EXPECT_NE(eg.get(), nullptr);
}

TEST_F(ReclamationTest, AddLotAllowedAlongsideReclaimedSibling) {
	// Sibling reclamation must not block adding new live siblings under the
	// same (live) parent.
	addDefaultLot();
	addLot(R"({
		"lot_name": "parent",
		"owner": "owner1",
		"parents": ["parent"],
		"paths": [{"path": "/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 1000,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	addLot(R"({
		"lot_name": "sib_a",
		"owner": "owner1",
		"parents": ["parent"],
		"paths": [{"path": "/parent/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	std::string err;
	ASSERT_EQ(reclaim("sib_a", PAST_TS, "EXPIRED", &err), 0) << err;

	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "sib_b",
		"owner": "owner1",
		"parents": ["parent"],
		"paths": [{"path": "/parent/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})",
							&raw_err);
	UniqueCString eg(raw_err);
	EXPECT_EQ(rv, 0) << "Live siblings should be addable even if a sibling is reclaimed: "
					 << (eg.get() ? eg.get() : "");
}

// ---------------------------------------------------------------------------
// Hierarchical past_* must not count reclaimed children's overage and must
// not return reclaimed parents.
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, HierarchicalPastDedExcludesReclaimedChildOverage) {
	// Parent has a moderate dedicated quota; a live child blows past its own
	// quota by enough to push the parent past its quota via the hierarchical
	// adjusted-usage calculation. Once the child is reclaimed, its usage has
	// been released to the default lot, so it must no longer push the live
	// parent into the result set.
	addDefaultLot();
	addLot(R"({
		"lot_name": "p",
		"owner": "owner1",
		"parents": ["p"],
		"paths": [{"path": "/p", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 0, "max_num_objects": 1000,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	addLot(R"({
		"lot_name": "c",
		"owner": "owner1",
		"parents": ["p"],
		"paths": [{"path": "/p/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	char *raw_err = nullptr;
	// Child overage = 500 - 10 = 490, which is well past parent's 100 quota.
	ASSERT_EQ(lotman_update_lot_usage(R"({"lot_name":"c","self_GB":500.0})", false, &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	// Sanity: hierarchical past_ded surfaces parent before the child is reclaimed.
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_ded(false, false, /*include_reclaimed=*/true, &raw_lots,
									   /*hierarchical=*/true, &raw_err),
			  0);
	UniqueCString eg1(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	ASSERT_TRUE(before.count("p")) << "Sanity: child overage should push live parent into hierarchical past_ded";

	std::string err;
	ASSERT_EQ(reclaim("c", PAST_TS, "EXPIRED", &err), 0) << err;

	raw_err = nullptr;
	char **raw_lots2 = nullptr;
	ASSERT_EQ(lotman_get_lots_past_ded(false, false, /*include_reclaimed=*/true, &raw_lots2,
									   /*hierarchical=*/true, &raw_err),
			  0);
	UniqueCString eg2(raw_err);
	UniqueStringList lots2(raw_lots2);
	std::set<std::string> after;
	for (int i = 0; lots2.get()[i]; ++i)
		after.insert(lots2.get()[i]);
	EXPECT_FALSE(after.count("p")) << "Reclaimed child's overage must not count against live parent";
}

TEST_F(ReclamationTest, HierarchicalPastDedExcludesReclaimedParent) {
	// A reclaimed parent has released its storage to the default lot and can
	// no longer be "past" its own quota. The hierarchical SQL must drop it
	// regardless of include_reclaimed (which only governs the post-filter
	// for non-hierarchical paths).
	addDefaultLot();
	addLot(R"({
		"lot_name": "fat_root",
		"owner": "owner1",
		"parents": ["fat_root"],
		"paths": [{"path": "/fat_root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1000,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_update_lot_usage(R"({"lot_name":"fat_root","self_GB":50.0})", false, &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	// Sanity: present before reclaim.
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_ded(false, false, true, &raw_lots, /*hierarchical=*/true, &raw_err), 0);
	UniqueCString eg1(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> before;
	for (int i = 0; lots.get()[i]; ++i)
		before.insert(lots.get()[i]);
	ASSERT_TRUE(before.count("fat_root"));

	std::string err;
	ASSERT_EQ(reclaim("fat_root", PAST_TS, "EXPIRED", &err), 0) << err;

	raw_err = nullptr;
	char **raw_lots2 = nullptr;
	// include_reclaimed=true is intentional: hierarchical SQL must still drop
	// reclaimed parents because the math depends on them being live.
	ASSERT_EQ(lotman_get_lots_past_ded(false, false, true, &raw_lots2, /*hierarchical=*/true, &raw_err), 0);
	UniqueCString eg2(raw_err);
	UniqueStringList lots2(raw_lots2);
	std::set<std::string> after;
	for (int i = 0; lots2.get()[i]; ++i)
		after.insert(lots2.get()[i]);
	EXPECT_FALSE(after.count("fat_root")) << "Hierarchical past_ded must drop a reclaimed parent unconditionally";
}

// ---------------------------------------------------------------------------
// Pre-existing recursive-expansion fix in get_lots_past_exp
// ---------------------------------------------------------------------------

TEST_F(ReclamationTest, GetLotsPastExpRecursiveIncludesDescendants) {
	// Regression: the recursive branch of get_lots_past_exp previously
	// shadowed its accumulator vector, dropping recursive children entirely.
	addDefaultLot();
	addLot(R"({
		"lot_name": "exp_root",
		"owner": "owner1",
		"parents": ["exp_root"],
		"paths": [{"path": "/exp_root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 1000, "deletion_time": 9999999999999
		}
	})");
	addLot(R"({
		"lot_name": "exp_child",
		"owner": "owner1",
		"parents": ["exp_root"],
		"paths": [{"path": "/exp_root/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 5, "opportunistic_GB": 2, "max_num_objects": 50,
			"creation_time": 100, "expiration_time": 9999999999999, "deletion_time": 9999999999999
		}
	})");

	char *raw_err = nullptr;
	char **raw_lots = nullptr;
	ASSERT_EQ(lotman_get_lots_past_exp(/*recursive=*/true, /*include_reclaimed=*/true, &raw_lots, &raw_err), 0);
	UniqueCString cleanup_err(raw_err);
	UniqueStringList lots(raw_lots);
	std::set<std::string> got;
	for (int i = 0; lots.get()[i]; ++i)
		got.insert(lots.get()[i]);
	EXPECT_TRUE(got.count("exp_root"));
	EXPECT_TRUE(got.count("exp_child")) << "Recursive past_exp must include children of expired ancestors";
}

} // namespace
