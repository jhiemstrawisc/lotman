// Tests covering the dynamic-ownership / descendancy rule for path resources.
//
// Background: Lotman previously enforced exclusive path ownership via an
// exact-path temporal-overlap check; carve-outs were expressed via explicit
// `exclude` rows. The dynamic-ownership model lets a sublot whose path is a
// strict subpath of an ancestor's recursive path automatically claim its
// subtree without an explicit exclusion row. The descendancy rule is
// enforced when adding/updating non-excluded paths:
//   - Same-path collisions during overlapping windows are rejected.
//   - If another live lot's recursive path is a strict prefix of the
//     candidate path, the candidate must be a recursive descendant of that
//     other lot.
//   - If the candidate path is recursive and would cover an existing lot's
//     path, that other lot must be a recursive descendant of the candidate.
//   - `exclude=1` rows on either side bypass the check (escape hatch).

#include "../src/lotman.h"
#include "test_utils.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

class DynamicOwnershipTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	void SetUp() override {
		tmp_dir = create_temp_directory("lotman_dyn_test");
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
		rv = lotman_set_context_str("caller", "owner1", &raw_err);
		err.reset(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
		rv = lotman_set_context_str("strict_hierarchy", "false", &raw_err);
		err.reset(raw_err);
		ASSERT_EQ(rv, 0) << err.get();

		// Common scaffold: default lot + a root that recursively owns /foo.
		addLot(R"({
			"lot_name": "default",
			"owner": "owner1",
			"parents": ["default"],
			"paths": [{"path": "/default", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 1000,
				"opportunistic_GB": 500,
				"max_num_objects": 10000,
				"creation_time": 0,
				"expiration_time": 0,
				"deletion_time": 0
			}
		})");
	}

	void TearDown() override {
		std::filesystem::remove_all(tmp_dir);
	}

	void addLot(const char *json_str) {
		char *raw_err = nullptr;
		int rv = lotman_add_lot(json_str, &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << "addLot failed: " << (err.get() ? err.get() : "unknown");
	}

	int tryAddLot(const char *json_str, std::string &err_out) {
		char *raw_err = nullptr;
		int rv = lotman_add_lot(json_str, &raw_err);
		if (raw_err) {
			err_out.assign(raw_err);
			free(raw_err);
		} else {
			err_out.clear();
		}
		return rv;
	}

	std::string resolveDir(const char *dir, int64_t query_time) {
		char **lots = nullptr;
		char *raw_err = nullptr;
		int rv = lotman_get_lots_from_dir(dir, false, query_time, &lots, &raw_err);
		UniqueCString err(raw_err);
		if (rv != 0 || !lots || !lots[0]) {
			lotman_free_string_list(lots);
			return std::string("<error:") + (err.get() ? err.get() : "unknown") + ">";
		}
		std::string result = lots[0];
		lotman_free_string_list(lots);
		return result;
	}

	// Helper: build a recursive lot owning /foo with parent=default.
	void addLotFooRecursive(const char *name = "lot1") {
		std::string body = R"({
			"lot_name": ")";
		body += name;
		body += R"(",
			"owner": "owner1",
			"parents": ["default"],
			"paths": [{"path": "/foo", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 100,
				"opportunistic_GB": 50,
				"max_num_objects": 1000,
				"creation_time": 100,
				"expiration_time": 9000,
				"deletion_time": 9500
			}
		})";
		addLot(body.c_str());
	}
};

// F1: forward sublot allowed — sublot of an ancestor with recursive coverage
// claims a strict subpath without needing an exclude row, and resolution
// picks the correct lot for each query path.
TEST_F(DynamicOwnershipTest, ForwardSublotAllowedAndResolves) {
	addLotFooRecursive("lot1");

	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	EXPECT_EQ(resolveDir("/foo/other", 500), "lot1");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot2");
	EXPECT_EQ(resolveDir("/foo/bar/baz", 500), "lot2");
	EXPECT_EQ(resolveDir("/foo", 500), "lot1");
}

// F2: a candidate whose path falls inside an unrelated lot's recursive
// coverage (overlapping in time) must be rejected.
TEST_F(DynamicOwnershipTest, ForwardUnrelatedRejected) {
	addLotFooRecursive("lot1");

	// lot2 has parent=default, so it's NOT a descendant of lot1.
	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
					   err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.find("Descendancy violation"), std::string::npos) << "err=" << err;
	EXPECT_NE(err.find("lot1"), std::string::npos) << "err=" << err;
}

// F3: a sublot may not claim the EXACT same path as an ancestor's recursive
// path during overlapping windows.
TEST_F(DynamicOwnershipTest, SamePathAsAncestorRejected) {
	addLotFooRecursive("lot1");

	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
					   err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.find("temporal overlap"), std::string::npos) << "err=" << err;
}

// F4: reverse insertion — lot2 already owns /foo/bar; later add lot1 with
// `/foo` recursive AND list lot2 as a child so the insertion adjustment
// rewires lot2.parent → lot1 inside the same transaction. The path check
// runs AFTER insertion adjustment, so descendancy is satisfied.
TEST_F(DynamicOwnershipTest, ReverseInsertionAllowedWithChildEdge) {
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"children": ["lot2"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	EXPECT_EQ(resolveDir("/foo", 500), "lot1");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot2");
	EXPECT_EQ(resolveDir("/foo/other", 500), "lot1");
}

// F5: reverse insertion without the child edge is rejected.
TEST_F(DynamicOwnershipTest, ReverseInsertionRejectedWithoutChildEdge) {
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
					   err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.find("Descendancy violation"), std::string::npos) << "err=" << err;
}

// F6: multi-ancestor coverage — when more than one live, non-excluded
// recursive owner of a parent path overlaps the candidate's window, the
// candidate must descend from EVERY one of them. Two unrelated lots can
// both legally own `/foo` recursively as long as their windows don't
// overlap; a candidate sublot at `/foo/bar` whose window straddles BOTH
// owners' windows must therefore descend from both.
TEST_F(DynamicOwnershipTest, MultiAncestorMustDescendFromAll) {
	// lot1 owns /foo recursively in window [1, 100).
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1,
			"expiration_time": 100,
			"deletion_time": 150
		}
	})");
	// lot2 owns the same /foo recursively in disjoint window [200, 300).
	// (Disjoint windows on the same path are allowed under the existing
	// EXACT-overlap rule, so this addition itself is legal.)
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 300,
			"deletion_time": 350
		}
	})");

	// Candidate sublot at /foo/bar whose window [50, 250) overlaps both
	// owners. With only lot1 as parent, it must be rejected because lot2's
	// /foo row also overlaps and lot2 is not an ancestor.
	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "sublot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 50,
			"expiration_time": 250,
			"deletion_time": 300
		}
	})",
					   err);
	EXPECT_NE(rv, 0) << "sublot should be rejected: lot2 covers /foo in overlapping window and is not an ancestor";
	EXPECT_NE(err.find("Descendancy violation"), std::string::npos) << "err=" << err;
	EXPECT_NE(err.find("lot2"), std::string::npos) << "err=" << err;

	// Same candidate, but now declaring BOTH lot1 and lot2 as parents,
	// must succeed.
	std::string err2;
	int rv2 = tryAddLot(R"({
		"lot_name": "sublot",
		"owner": "owner1",
		"parents": ["lot1", "lot2"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 50,
			"expiration_time": 250,
			"deletion_time": 300
		}
	})",
						err2);
	ASSERT_EQ(rv2, 0) << "Adding sublot under both lot1 and lot2 should succeed: " << err2;
	// Resolution: in lot1's window, /foo/bar resolves to sublot; in lot2's
	// window, also sublot.
	EXPECT_EQ(resolveDir("/foo/bar", 75), "sublot");
	EXPECT_EQ(resolveDir("/foo/bar", 225), "sublot");
}

// F6b (renamed): non-overlapping ancestor windows do NOT trigger the
// must-descend-from-all rule. lot1 owns /foo in [100, 9000); a second,
// unrelated owner of /foo lives strictly after lot1's deletion_time and
// therefore does not constrain a sublot whose window overlaps lot1 only.
TEST_F(DynamicOwnershipTest, NonOverlappingAncestorWindowSkipsRule) {
	addLotFooRecursive("lot1");

	addLot(R"({
		"lot_name": "lotLate",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 9500,
			"expiration_time": 9999,
			"deletion_time": 10000
		}
	})");

	// sublot's window is fully inside lot1's window and disjoint from
	// lotLate's; therefore lotLate's /foo must NOT trigger a descendancy
	// failure even though sublot is not a descendant of lotLate.
	addLot(R"({
		"lot_name": "sublot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "sublot");
}

// F7: no covering ancestor exists — lot2 with /foo/bar should be allowed
// regardless of hierarchy when nobody recursively owns /foo.
TEST_F(DynamicOwnershipTest, NoCoveringAncestorAllowed) {
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot2");
	// Nobody owns /foo, so it should resolve to default.
	EXPECT_EQ(resolveDir("/foo", 500), "default");
}

// F8: non-recursive ancestor + descendant subpath: no descendancy required.
TEST_F(DynamicOwnershipTest, NonRecursiveAncestorNoRequirement) {
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": false}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// lot2 unrelated (parent=default), claims /foo/bar — should be allowed
	// because lot1's /foo is non-recursive.
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
	EXPECT_EQ(resolveDir("/foo", 500), "lot1");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot2");
	EXPECT_EQ(resolveDir("/foo/other", 500), "default") << "non-recursive lot1 should not cover /foo/other";
}

// F9: default lot fallback — paths with no covering lot resolve to default
// without requiring descendancy from default.
TEST_F(DynamicOwnershipTest, DefaultLotIsFallbackOnly) {
	EXPECT_EQ(resolveDir("/anything/else", 500), "default");

	// A lot whose path doesn't overlap default's /default coverage and is
	// unrelated to default should still be addable.
	addLot(R"({
		"lot_name": "lotX",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/elsewhere", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
}

// F10: removing a sublot reverts ownership to the ancestor.
TEST_F(DynamicOwnershipTest, SublotRemovalRevertsOwnership) {
	addLotFooRecursive("lot1");
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot2");

	char *raw_err = nullptr;
	int rv = lotman_remove_lots_recursive("lot2", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot1");
}

// F11: removing a parent edge that legitimized a path must reject mid-txn.
TEST_F(DynamicOwnershipTest, ParentRemovalRejectedWhenItInvalidatesPath) {
	addLotFooRecursive("lot1");
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Adding default as a second parent first so the removal of lot1 leaves
	// lot2 with at least one parent.
	char *raw_err = nullptr;
	int rv = lotman_update_lot(R"({
		"lot_name": "lot2",
		"parents": [{"current": "lot1", "new": "default"}]
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Expected rejection because lot2's path /foo/bar would no longer have lot1 as ancestor";
	EXPECT_NE(std::string(err.get() ? err.get() : "").find("Descendancy violation"), std::string::npos);
}

// F12: exclude=1 escape hatch — an excluded row continues to bypass the
// descendancy check. This is a regression guard for the "keep exclude as
// escape hatch" decision.
TEST_F(DynamicOwnershipTest, ExcludeRowBypassesDescendancyRule) {
	addLotFooRecursive("lot1");

	// lot2 unrelated, but with exclude=1 — the descendancy check should be
	// skipped for excluded rows even though /foo/bar falls under lot1's
	// recursive coverage.
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true, "exclude": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
	// Resolution still returns lot1 since lot2's row is excluded.
	EXPECT_EQ(resolveDir("/foo/bar", 500), "lot1");
}

// F13: temporal stagger preserved — when windows don't overlap, no
// descendancy required even for unrelated lots.
TEST_F(DynamicOwnershipTest, TemporalStaggerPreserved) {
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 200,
			"deletion_time": 300
		}
	})");

	// Unrelated lot2 with window strictly after lot1 — allowed.
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 300,
			"expiration_time": 400,
			"deletion_time": 500
		}
	})");
	EXPECT_EQ(resolveDir("/foo/bar", 350), "lot2");
	EXPECT_EQ(resolveDir("/foo/bar", 150), "lot1") << "lot1 still covers in its window";
}

// F14: MPA expansion re-validates path overlaps. lot2 unrelated owns
// /foo/bar in [200, 300). lot1 lives [400, 500) recursively owning /foo.
// Expanding lot1's expiration to 600 — still no overlap. Expanding to
// e.g. 250 (so lot1 now overlaps lot2's window) must reject.
TEST_F(DynamicOwnershipTest, MpaExpansionRevalidates) {
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 300,
			"deletion_time": 350
		}
	})");
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 400,
			"expiration_time": 500,
			"deletion_time": 600
		}
	})");

	// Expand lot1's creation_time backward to 100 so it overlaps lot2's
	// [200, 300) window. Note: expansion of creation_time backward (i.e.
	// decreasing it) requires admin_override under "alive" contraction
	// policy, but with policy="none" (default) it is permitted.
	char *raw_err = nullptr;
	int rv = lotman_update_lot(R"({
		"lot_name": "lot1",
		"management_policy_attrs": {"creation_time": 100}
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Expected rejection: lot1 expansion would now cover unrelated lot2's /foo/bar";
	EXPECT_NE(std::string(err.get() ? err.get() : "").find("path conflict"), std::string::npos);
}

// F15: PREFIX-OUT vs. unrelated non-recursive subpath. lot1 (unrelated)
// owns /foo/bar NON-recursively. A second unrelated lot2 attempts to
// recursively claim the parent /foo. Even though deepest-prefix resolution
// could in principle keep lot1 as the owner of the single point /foo/bar
// while lot2 picks up everything else under /foo, we INTENTIONALLY reject
// this. Lotman's philosophy is that owners must consent (via a parent edge)
// before another lot reshapes the policy environment around their paths;
// reshaping the children of /foo/bar to be tracked by an unrelated lot2
// without lot1's consent would silently change lot1's neighborhood. This
// test locks in the conservative rejection.
TEST_F(DynamicOwnershipTest, RecursiveCoverOverNonRecursiveSubpathRejected) {
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": false}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
					   err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.find("Descendancy violation"), std::string::npos) << "err=" << err;
	EXPECT_NE(err.find("/foo/bar"), std::string::npos) << "err=" << err;
}

// F16: Diagnostic precision — when a descendancy violation is reported,
// the message must name the conflicting ancestor lot specifically (not a
// generic storage error). This guards against the previous behavior in
// which a transient SQLite error inside is_recursive_ancestor was silently
// reinterpreted as "not an ancestor", producing a misleading violation
// message that pointed at no useful lot.
TEST_F(DynamicOwnershipTest, ViolationMessageIdentifiesAncestor) {
	addLotFooRecursive("lot1");

	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "intruder",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/sub", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
					   err);
	ASSERT_NE(rv, 0);
	EXPECT_NE(err.find("Descendancy violation"), std::string::npos) << err;
	EXPECT_NE(err.find("'lot1'"), std::string::npos) << "Expected the offending ancestor lot1 to be named: " << err;
	EXPECT_EQ(err.find("storage error"), std::string::npos)
		<< "A genuine descendancy denial should not contain a storage-error fragment: " << err;
}

// F17 (PR #51 review, comment 1): toggling the `recursive` flag on an
// existing path is itself a state change that can introduce a PREFIX-OUT
// conflict, even when the path string and exclude flag are untouched.
// Behavior-locking guard ensuring such a flip is rejected. NOTE: with the
// current public-API surface, `lotman_update_lot` runs an end-of-txn
// revalidation that catches this bug regardless of the per-row inline
// check. The corresponding source fix (also re-checking on
// `recursive_changed` in update_paths_in_txn) is therefore defense-in-
// depth — it produces a clearer per-row error and protects against
// regressions if a future caller of update_paths_in_txn skips the
// end-of-txn backstop. This test guards the externally-observable
// behavior; if either layer of defense is broken in isolation it still
// passes, but if both regress it will fail.
TEST_F(DynamicOwnershipTest, FlippingRecursiveTrueRetriggersDescendancyCheck) {
	// lot1 owns /foo NON-recursively, so lot2 (unrelated) is free to claim
	// /foo/bar at this moment: there is no PREFIX-IN conflict because lot1
	// is non-recursive.
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo", "recursive": false}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	addLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo/bar", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Now flip lot1's /foo to recursive. lot2 is unrelated, so this would
	// create a PREFIX-OUT conflict (lot1's recursive /foo would cover
	// lot2's /foo/bar without lot2 being a sublot of lot1).
	char *raw_err = nullptr;
	int rv = lotman_update_lot(R"({
		"lot_name": "lot1",
		"paths": [{"current": "/foo", "new": "/foo", "recursive": true}]
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Expected the recursive=true flip on /foo to be rejected because unrelated lot2 owns /foo/bar";
	EXPECT_NE(std::string(err.get() ? err.get() : "").find("Descendancy violation"), std::string::npos)
		<< "err=" << (err.get() ? err.get() : "");
}

// F18 (PR #51 review, comment 2): SQL prefix matching must treat path
// strings literally. SQLite's LIKE treats '_' as a single-character
// wildcard and '%' as any-string, so paths containing those characters
// would falsely match unrelated paths. Regression guard for the prior
// `?4 LIKE p.path || '%'` formulation; the fix uses substr() instead.
TEST_F(DynamicOwnershipTest, PathWithUnderscoreNotTreatedAsWildcard) {
	// lot1 recursively owns /foo_bar/. Under LIKE matching, the prefix
	// expression '/foo_bar/%' would match '/fooXbar/baz/' because '_'
	// matches any single character. Under literal substr matching it does
	// not, so an unrelated lot may legally claim /fooXbar/baz/.
	addLot(R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo_bar/", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "lot2",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/fooXbar/baz/", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
					   err);
	EXPECT_EQ(rv, 0) << "/fooXbar/baz/ is not a true subpath of /foo_bar/; '_' must compare literally. err=" << err;

	// Sanity: a *real* descendant (literal '_' match) is still rejected
	// when the descendancy rule isn't satisfied.
	std::string err2;
	int rv2 = tryAddLot(R"({
		"lot_name": "lot3",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/foo_bar/baz/", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						err2);
	EXPECT_NE(rv2, 0) << "Genuine subpath of recursive lot1 must still trigger descendancy check";
	EXPECT_NE(err2.find("Descendancy violation"), std::string::npos) << "err=" << err2;
}

// F19 (PR #51 review, comment 3): is_recursive_ancestor was reimplemented
// to prefetch all parent edges in a single query and BFS in memory.
// Functional sanity test for a deep ancestry chain (5 levels) to confirm
// the rewrite preserves correctness across multi-hop ancestry.
TEST_F(DynamicOwnershipTest, DeepAncestryChainResolvesCorrectly) {
	// Build chain: a -> b -> c -> d -> e (default -> a, a -> b, ...)
	// "x -> y" here means y has x as parent.
	auto addChainLot = [&](const char *name, const char *parent) {
		std::string body = R"({"lot_name": ")";
		body += name;
		body += R"(", "owner": "owner1", "parents": [")";
		body += parent;
		body += R"("], "paths": [], "management_policy_attrs": {)";
		body += R"("dedicated_GB":10,"opportunistic_GB":5,"max_num_objects":100,)";
		body += R"("creation_time":100,"expiration_time":9000,"deletion_time":9500}})";
		addLot(body.c_str());
	};
	// Root with a recursive path that the deep descendant will try to claim under.
	addLot(R"({
		"lot_name": "root_chain",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/chain", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	addChainLot("a", "root_chain");
	addChainLot("b", "a");
	addChainLot("c", "b");
	addChainLot("d", "c");

	// e is 5 hops below root_chain. is_recursive_ancestor must walk the
	// full chain to confirm e descends from root_chain and accept the
	// deeply-nested recursive subpath.
	std::string err;
	int rv = tryAddLot(R"({
		"lot_name": "e",
		"owner": "owner1",
		"parents": ["d"],
		"paths": [{"path": "/chain/x/y/z", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
					   err);
	EXPECT_EQ(rv, 0) << "Deep descendant of root_chain must be allowed under /chain. err=" << err;

	// Negative control: an unrelated lot at the same depth must be rejected.
	std::string err2;
	int rv2 = tryAddLot(R"({
		"lot_name": "intruder",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/chain/x/y/w", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						err2);
	EXPECT_NE(rv2, 0) << "Unrelated lot must still be rejected under deep recursive root. err=" << err2;
	EXPECT_NE(err2.find("Descendancy violation"), std::string::npos) << "err=" << err2;
}

} // namespace
