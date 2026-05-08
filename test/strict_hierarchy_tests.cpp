#include "../src/lotman.h"
#include "../src/lotman_db.h"
#include "../src/lotman_internal.h"
#include "test_utils.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Test fixture for strict hierarchy tests
class StrictHierarchyTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	void SetUp() override {
		tmp_dir = create_temp_directory("lotman_strict_test");
		char *raw_err = nullptr;
		int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
		rv = lotman_set_context_str("caller", "owner1", &raw_err);
		err.reset(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
		// Reset all context to defaults at the start of each test
		rv = lotman_set_context_str("strict_hierarchy", "false", &raw_err);
		err.reset(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
		rv = lotman_set_context_str("contraction_policy", "none", &raw_err);
		err.reset(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
		rv = lotman_set_context_str("admin_override", "false", &raw_err);
		err.reset(raw_err);
		ASSERT_EQ(rv, 0) << err.get();
	}

	void TearDown() override {
		// Reset context so other tests aren't affected
		char *raw_err = nullptr;
		lotman_set_context_str("strict_hierarchy", "false", &raw_err);
		free(raw_err);
		raw_err = nullptr;
		lotman_set_context_str("contraction_policy", "none", &raw_err);
		free(raw_err);
		raw_err = nullptr;
		lotman_set_context_str("admin_override", "false", &raw_err);
		free(raw_err);
		std::filesystem::remove_all(tmp_dir);
	}

	void addLot(const char *lot_json) {
		char *raw_err = nullptr;
		int rv = lotman_add_lot(lot_json, &raw_err);
		UniqueCString err_msg(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to add lot: " << (err_msg.get() ? err_msg.get() : "unknown");
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
				"expiration_time": 9999,
				"deletion_time": 99999
			}
		})");
	}

	// A root lot with generous resources
	void addRootLot() {
		addLot(R"({
			"lot_name": "root",
			"owner": "owner1",
			"parents": ["root"],
			"paths": [{"path": "/root/data", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 100,
				"opportunistic_GB": 50,
				"max_num_objects": 1000,
				"creation_time": 100,
				"expiration_time": 9000,
				"deletion_time": 9500
			}
		})");
	}
};

namespace {

// ============================================================================
// Context string set/get tests
// ============================================================================

TEST_F(StrictHierarchyTest, ContextSetGetStrictHierarchy) {
	char *raw_err = nullptr;
	char *raw_output = nullptr;

	// Default should be false
	int rv = lotman_get_context_str("strict_hierarchy", &raw_output, &raw_err);
	UniqueCString err(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "false");

	// Set to true via "true"
	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("strict_hierarchy", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "true");

	// Set to false via "0"
	rv = lotman_set_context_str("strict_hierarchy", "0", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("strict_hierarchy", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "false");

	// Set to true via "1"
	rv = lotman_set_context_str("strict_hierarchy", "1", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("strict_hierarchy", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "true");

	// Invalid value should fail
	rv = lotman_set_context_str("strict_hierarchy", "yes", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.get(), nullptr);
}

TEST_F(StrictHierarchyTest, ContextSetGetContractionPolicy) {
	char *raw_err = nullptr;
	char *raw_output = nullptr;

	// Default should be "none"
	int rv = lotman_get_context_str("contraction_policy", &raw_output, &raw_err);
	UniqueCString err(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "none");

	// Set to "alive"
	rv = lotman_set_context_str("contraction_policy", "alive", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("contraction_policy", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "alive");

	// Set to "always"
	rv = lotman_set_context_str("contraction_policy", "always", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("contraction_policy", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "always");

	// Set back to "none"
	rv = lotman_set_context_str("contraction_policy", "none", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("contraction_policy", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "none");

	// Invalid value
	rv = lotman_set_context_str("contraction_policy", "invalid", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_NE(err.get(), nullptr);
}

TEST_F(StrictHierarchyTest, ContextSetGetAdminOverride) {
	char *raw_err = nullptr;
	char *raw_output = nullptr;

	// Default should be false
	int rv = lotman_get_context_str("admin_override", &raw_output, &raw_err);
	UniqueCString err(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "false");

	// Set to true
	rv = lotman_set_context_str("admin_override", "true", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_get_context_str("admin_override", &raw_output, &raw_err);
	err.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err.get();
	EXPECT_STREQ(output.get(), "true");

	// Invalid value
	rv = lotman_set_context_str("admin_override", "maybe", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0);
}

// ============================================================================
// Axiom 1: child's attributed portion must not exceed parent's MPAs
// ============================================================================

TEST_F(StrictHierarchyTest, Axiom1ViolationDedicatedGB) {
	addDefaultLot();

	// Root lot with 10 GB dedicated
	addLot(R"({
		"lot_name": "parent_a1",
		"owner": "owner1",
		"parents": ["parent_a1"],
		"paths": [{"path": "/a1/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Enable strict hierarchy
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Try to create a child that attributes MORE dedicated_GB to parent than parent has.
	// Child claims 20 dedicated_GB, and attributes all 20 to parent_a1 (which only has 10).
	const char *child_json = R"({
		"lot_name": "child_a1",
		"owner": "owner1",
		"parents": ["parent_a1"],
		"paths": [{"path": "/a1/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})";
	// With equal-split default attribution, all 20 GB goes to parent_a1 (only parent).
	// parent_a1 only has 10 GB. This should violate Axiom 1.
	rv = lotman_add_lot(child_json, &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Adding a child that exceeds parent's dedicated_GB should fail under strict hierarchy";
	EXPECT_NE(err.get(), nullptr);
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("exceeds the parent's") != std::string::npos)
		<< "Error should report that the child's allocation exceeds the parent's, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom1ViolationMaxObjects) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_obj",
		"owner": "owner1",
		"parents": ["parent_obj"],
		"paths": [{"path": "/obj/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with 200 max_num_objects, parent only has 50
	rv = lotman_add_lot(R"({
		"lot_name": "child_obj",
		"owner": "owner1",
		"parents": ["parent_obj"],
		"paths": [{"path": "/obj/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child with more objects than parent should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("exceeds the parent's") != std::string::npos)
		<< "Error should report that the child's allocation exceeds the parent's, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom1PassesWhenChildFitsInParent) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_fit",
		"owner": "owner1",
		"parents": ["parent_fit"],
		"paths": [{"path": "/fit/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child well within parent's limits
	rv = lotman_add_lot(R"({
		"lot_name": "child_fit",
		"owner": "owner1",
		"parents": ["parent_fit"],
		"paths": [{"path": "/fit/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Child within parent's limits should succeed: " << (err.get() ? err.get() : "");
}

// ============================================================================
// Axiom 2: sum of all children's attributions must not exceed parent's MPAs
// ============================================================================

TEST_F(StrictHierarchyTest, Axiom2ViolationSumExceedsParent) {
	addDefaultLot();

	// Parent with 10 GB dedicated
	addLot(R"({
		"lot_name": "parent_a2",
		"owner": "owner1",
		"parents": ["parent_a2"],
		"paths": [{"path": "/a2/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// First child: 6 dedicated_GB (fits within 10)
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2_1",
		"owner": "owner1",
		"parents": ["parent_a2"],
		"paths": [{"path": "/a2/child1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 6,
			"opportunistic_GB": 2,
			"max_num_objects": 40,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "First child should succeed: " << (err.get() ? err.get() : "");

	// Second child: 6 dedicated_GB. Now total attributed = 6 + 6 = 12 > 10. Should fail.
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2_2",
		"owner": "owner1",
		"parents": ["parent_a2"],
		"paths": [{"path": "/a2/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 6,
			"opportunistic_GB": 2,
			"max_num_objects": 40,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Second child should fail — sum of children exceeds parent's dedicated_GB";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("peak concurrent") != std::string::npos)
		<< "Error should report that combined concurrent children exceed the parent's capacity, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom2PassesSumWithinParent) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_a2p",
		"owner": "owner1",
		"parents": ["parent_a2p"],
		"paths": [{"path": "/a2p/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Two children each with 8 dedicated_GB. Sum=16, fits within 20.
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2p_1",
		"owner": "owner1",
		"parents": ["parent_a2p"],
		"paths": [{"path": "/a2p/child1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 8,
			"opportunistic_GB": 4,
			"max_num_objects": 80,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	rv = lotman_add_lot(R"({
		"lot_name": "child_a2p_2",
		"owner": "owner1",
		"parents": ["parent_a2p"],
		"paths": [{"path": "/a2p/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 8,
			"opportunistic_GB": 4,
			"max_num_objects": 80,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Sum 16 <= 20 should pass: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, Axiom2ViolationSumDedOppExceedsParentTotal) {
	// Dedicated and opportunistic are independent storage pools, so axiom 2
	// enforces them independently:
	//   (a) sum(child attr_ded) <= parent.ded
	//   (b) sum(child attr_opp) <= parent.opp
	// This test exercises check (b): each child's dedicated_GB stays well
	// within the parent's dedicated_GB, but the concurrent sum of
	// opportunistic_GB across children exceeds the parent's opportunistic_GB.
	addDefaultLot();

	// Parent: ded=20, opp=5
	addLot(R"({
		"lot_name": "parent_a2do",
		"owner": "owner1",
		"parents": ["parent_a2do"],
		"paths": [{"path": "/a2do/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 5,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child 1: ded=9, opp=5 → attributed ded=9 ≤ parent ded=20 ✓
	//          attributed opp=5 ≤ parent opp=5 ✓
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2do_1",
		"owner": "owner1",
		"parents": ["parent_a2do"],
		"paths": [{"path": "/a2do/child1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 9,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err1(raw_err);
	ASSERT_EQ(rv, 0) << "First child should succeed: " << (err1.get() ? err1.get() : "");

	// Child 2: ded=9, opp=5 → attributed ded=9+9=18 ≤ parent ded=20 ✓
	//          attributed opp=5+5=10 > parent opp=5 ✗
	// Should fail on the per-axis opportunistic_GB check.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2do_2",
		"owner": "owner1",
		"parents": ["parent_a2do"],
		"paths": [{"path": "/a2do/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 9,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0) << "Second child should fail — concurrent sum of opportunistic_GB exceeds parent opportunistic_GB";
	std::string err_str(err2.get() ? err2.get() : "");
	EXPECT_TRUE(err_str.find("opportunistic_GB") != std::string::npos)
		<< "Error should mention the opportunistic_GB axis, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom2PassesDedOppWithinParentTotal) {
	// Complement to the above: two children whose combined ded+opp fits.
	addDefaultLot();

	// Parent: ded=20, opp=10 → total=30
	addLot(R"({
		"lot_name": "parent_a2dop",
		"owner": "owner1",
		"parents": ["parent_a2dop"],
		"paths": [{"path": "/a2dop/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child 1: ded=9, opp=5 → total attributed so far = 14
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2dop_1",
		"owner": "owner1",
		"parents": ["parent_a2dop"],
		"paths": [{"path": "/a2dop/child1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 9,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err1(raw_err);
	ASSERT_EQ(rv, 0) << (err1.get() ? err1.get() : "");

	// Child 2: ded=9, opp=5 → total attributed = 14+14=28 ≤ 30 ✓
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child_a2dop_2",
		"owner": "owner1",
		"parents": ["parent_a2dop"],
		"paths": [{"path": "/a2dop/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 9,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_EQ(rv, 0) << "Sum ded+opp within parent total should pass: " << (err2.get() ? err2.get() : "");
}

// ============================================================================
// Axiom 3: child timestamps must fit within parent's timestamps
// ============================================================================

TEST_F(StrictHierarchyTest, Axiom3ViolationCreationTimeTooEarly) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_a3",
		"owner": "owner1",
		"parents": ["parent_a3"],
		"paths": [{"path": "/a3/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 500,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child creation_time (100) < parent creation_time (500) → violation
	rv = lotman_add_lot(R"({
		"lot_name": "child_a3_early",
		"owner": "owner1",
		"parents": ["parent_a3"],
		"paths": [{"path": "/a3/child_early", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child with earlier creation_time than parent should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("time window") != std::string::npos)
		<< "Error should report that the child's time window is not contained within the parent's, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom3ViolationExpirationTimeTooLate) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_a3e",
		"owner": "owner1",
		"parents": ["parent_a3e"],
		"paths": [{"path": "/a3e/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 5000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child expiration_time (8000) > parent expiration_time (5000) → violation
	rv = lotman_add_lot(R"({
		"lot_name": "child_a3_late",
		"owner": "owner1",
		"parents": ["parent_a3e"],
		"paths": [{"path": "/a3e/child_late", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child with later expiration_time than parent should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("time window") != std::string::npos)
		<< "Error should report that the child's time window is not contained within the parent's, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom3ViolationDeletionTimeTooLate) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_a3d",
		"owner": "owner1",
		"parents": ["parent_a3d"],
		"paths": [{"path": "/a3d/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 5000,
			"deletion_time": 6000
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child deletion_time (9000) > parent deletion_time (6000) → violation
	rv = lotman_add_lot(R"({
		"lot_name": "child_a3_del",
		"owner": "owner1",
		"parents": ["parent_a3d"],
		"paths": [{"path": "/a3d/child_del", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 4000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child with later deletion_time than parent should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("time window") != std::string::npos)
		<< "Error should report that the child's time window is not contained within the parent's, got: " << err_str;
}

TEST_F(StrictHierarchyTest, Axiom3PassesTimestampsWithinParent) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "parent_a3ok",
		"owner": "owner1",
		"parents": ["parent_a3ok"],
		"paths": [{"path": "/a3ok/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child timestamps strictly within parent's
	rv = lotman_add_lot(R"({
		"lot_name": "child_a3_ok",
		"owner": "owner1",
		"parents": ["parent_a3ok"],
		"paths": [{"path": "/a3ok/child_ok", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Child within parent timestamps should pass: " << (err.get() ? err.get() : "");
}

// ============================================================================
// Attribution logic: joint child lot sharing quotas from two parents
// ============================================================================

TEST_F(StrictHierarchyTest, AttributionJointChildEqualSplit) {
	addDefaultLot();

	// Two root lots, each with 20 dedicated_GB
	addLot(R"({
		"lot_name": "parent_A",
		"owner": "owner1",
		"parents": ["parent_A"],
		"paths": [{"path": "/attr/parentA", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "parent_B",
		"owner": "owner1",
		"parents": ["parent_B"],
		"paths": [{"path": "/attr/parentB", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Joint child with 30 dedicated_GB, parents=["parent_A", "parent_B"].
	// With equal-split (no explicit attributions), each parent gets 15 GB attributed.
	// Each parent has 20 GB, so 15 <= 20 → Axiom 1 passes.
	// Each parent has only this child, so sum = 15 <= 20 → Axiom 2 passes.
	rv = lotman_add_lot(R"({
		"lot_name": "joint_child",
		"owner": "owner1",
		"parents": ["parent_A", "parent_B"],
		"paths": [{"path": "/attr/joint", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Joint child with equal split across two parents should succeed: "
					 << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, AttributionJointChildExplicitSplit) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "pA_explicit",
		"owner": "owner1",
		"parents": ["pA_explicit"],
		"paths": [{"path": "/attr_ex/parentA", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "pB_explicit",
		"owner": "owner1",
		"parents": ["pB_explicit"],
		"paths": [{"path": "/attr_ex/parentB", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with 20 dedicated_GB, explicit attribution: 15 to pA, 5 to pB.
	// pA has 30, so 15 <= 30 ✓. pB has 10, so 5 <= 10 ✓.
	rv = lotman_add_lot(R"({
		"lot_name": "joint_explicit",
		"owner": "owner1",
		"parents": ["pA_explicit", "pB_explicit"],
		"paths": [{"path": "/attr_ex/joint", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		},
		"parent_attributions": {
			"pA_explicit": {
				"dedicated_GB": 15,
				"opportunistic_GB": 7,
				"max_num_objects": 70
			},
			"pB_explicit": {
				"dedicated_GB": 5,
				"opportunistic_GB": 3,
				"max_num_objects": 30
			}
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Explicit attribution within limits should succeed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, AttributionJointChildExplicitExceedsOneParent) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "pA_exceed",
		"owner": "owner1",
		"parents": ["pA_exceed"],
		"paths": [{"path": "/attr_exc/parentA", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "pB_exceed",
		"owner": "owner1",
		"parents": ["pB_exceed"],
		"paths": [{"path": "/attr_exc/parentB", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with 20 dedicated_GB, explicit: 5 to pA, 15 to pB.
	// pB only has 10 → Axiom 1 violation for pB.
	rv = lotman_add_lot(R"({
		"lot_name": "joint_exceed",
		"owner": "owner1",
		"parents": ["pA_exceed", "pB_exceed"],
		"paths": [{"path": "/attr_exc/joint", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		},
		"parent_attributions": {
			"pA_exceed": {
				"dedicated_GB": 5,
				"opportunistic_GB": 3,
				"max_num_objects": 30
			},
			"pB_exceed": {
				"dedicated_GB": 15,
				"opportunistic_GB": 7,
				"max_num_objects": 70
			}
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Explicit attribution exceeding one parent should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("exceeds the parent's") != std::string::npos)
		<< "Error should report that the child's allocation exceeds the parent's, got: " << err_str;
}

TEST_F(StrictHierarchyTest, AttributionThreeParentsMixedExplicitAndSplit) {
	// One child with 3 parents. Attribution to parent A is explicit;
	// the remainder is equal-split across parents B and C.
	addDefaultLot();

	addLot(R"({
		"lot_name": "mp_A",
		"owner": "owner1",
		"parents": ["mp_A"],
		"paths": [{"path": "/mp/parentA", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 20,
			"max_num_objects": 400,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "mp_B",
		"owner": "owner1",
		"parents": ["mp_B"],
		"paths": [{"path": "/mp/parentB", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 15,
			"max_num_objects": 300,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "mp_C",
		"owner": "owner1",
		"parents": ["mp_C"],
		"paths": [{"path": "/mp/parentC", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 15,
			"max_num_objects": 300,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child: ded=30, opp=12, obj=300
	// Explicit: mp_A gets ded=10, opp=4, obj=100
	// Remainder: ded=20, opp=8, obj=200 → split equally between mp_B and mp_C
	//   mp_B gets ded=10, opp=4, obj=100
	//   mp_C gets ded=10, opp=4, obj=100
	// All fit: mp_A(40) >= 10, mp_B(30) >= 10, mp_C(30) >= 10
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "mp_child",
		"owner": "owner1",
		"parents": ["mp_A", "mp_B", "mp_C"],
		"paths": [{"path": "/mp/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 12,
			"max_num_objects": 300,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		},
		"parent_attributions": {
			"mp_A": {
				"dedicated_GB": 10,
				"opportunistic_GB": 4,
				"max_num_objects": 100
			}
		}
	})",
						&raw_err);
	UniqueCString err1(raw_err);
	EXPECT_EQ(rv, 0) << "Mixed explicit + equal-split across 3 parents should succeed: "
					 << (err1.get() ? err1.get() : "");
}

TEST_F(StrictHierarchyTest, AttributionThreeParentsMixedExplicitOverloadsOneParent) {
	// Same 3-parent layout, but the equal-split remainder overloads one parent.
	addDefaultLot();

	addLot(R"({
		"lot_name": "mpo_A",
		"owner": "owner1",
		"parents": ["mpo_A"],
		"paths": [{"path": "/mpo/parentA", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 20,
			"max_num_objects": 400,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "mpo_B",
		"owner": "owner1",
		"parents": ["mpo_B"],
		"paths": [{"path": "/mpo/parentB", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 3,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "mpo_C",
		"owner": "owner1",
		"parents": ["mpo_C"],
		"paths": [{"path": "/mpo/parentC", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 20,
			"max_num_objects": 400,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child: ded=30, opp=12, obj=300
	// Explicit: mpo_A gets ded=10, opp=4, obj=100
	// Remainder: ded=20, opp=8, obj=200 → split equally between mpo_B and mpo_C
	//   mpo_B gets ded=10, opp=4, obj=100
	//   mpo_C gets ded=10, opp=4, obj=100
	// mpo_B only has ded=5 → Axiom 1 violation (10 > 5)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "mpo_child",
		"owner": "owner1",
		"parents": ["mpo_A", "mpo_B", "mpo_C"],
		"paths": [{"path": "/mpo/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 12,
			"max_num_objects": 300,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		},
		"parent_attributions": {
			"mpo_A": {
				"dedicated_GB": 10,
				"opportunistic_GB": 4,
				"max_num_objects": 100
			}
		}
	})",
						&raw_err);
	UniqueCString err1(raw_err);
	EXPECT_NE(rv, 0) << "Equal-split remainder overloading mpo_B should fail";
	std::string err_str(err1.get() ? err1.get() : "");
	EXPECT_TRUE(err_str.find("exceeds the parent's") != std::string::npos)
		<< "Error should report that the child's allocation exceeds the parent's, got: " << err_str;
}

// ============================================================================
// Contraction policy tests: non-strict mode (no contraction_policy)
// ============================================================================

TEST_F(StrictHierarchyTest, ContractionAllowedInNonStrictMode) {
	// With default settings (contraction_policy="none"), all contractions should work.
	addDefaultLot();
	addLot(R"({
		"lot_name": "contract_lot",
		"owner": "owner1",
		"parents": ["contract_lot"],
		"paths": [{"path": "/contract/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;

	// Shrink dedicated_GB: 50 → 10
	const char *up1 = R"({"lot_name": "contract_lot", "management_policy_attrs": {"dedicated_GB": 10}})";
	int rv = lotman_update_lot(up1, &raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Dedicated_GB contraction should be allowed: " << (err.get() ? err.get() : "");

	// Shrink opportunistic_GB: 25 → 5
	rv = lotman_update_lot(R"({"lot_name": "contract_lot", "management_policy_attrs": {"opportunistic_GB": 5}})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Opportunistic_GB contraction should be allowed: " << (err.get() ? err.get() : "");

	// Shrink max_num_objects: 500 → 50
	rv = lotman_update_lot(R"({"lot_name": "contract_lot", "management_policy_attrs": {"max_num_objects": 50}})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Max_num_objects contraction should be allowed: " << (err.get() ? err.get() : "");

	// Shrink expiration_time: 9000 → 5000 (ending sooner = contraction)
	rv = lotman_update_lot(R"({"lot_name": "contract_lot", "management_policy_attrs": {"expiration_time": 5000}})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Expiration_time contraction should be allowed: " << (err.get() ? err.get() : "");

	// Shrink deletion_time: 9500 → 6000
	rv = lotman_update_lot(R"({"lot_name": "contract_lot", "management_policy_attrs": {"deletion_time": 6000}})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Deletion_time contraction should be allowed: " << (err.get() ? err.get() : "");
}

// ============================================================================
// Contraction policy "always": all contractions blocked
// ============================================================================

TEST_F(StrictHierarchyTest, ContractionBlockedAlwaysPolicy) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "always_lot",
		"owner": "owner1",
		"parents": ["always_lot"],
		"paths": [{"path": "/always/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("contraction_policy", "always", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Each contraction should be blocked
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "always_lot", "management_policy_attrs": {"dedicated_GB": 10}})", &raw_err);
	UniqueCString err1(raw_err);
	EXPECT_NE(rv, 0) << "Dedicated_GB contraction should be blocked by 'always' policy";
	{
		std::string err_str(err1.get() ? err1.get() : "");
		EXPECT_TRUE(err_str.find("always") != std::string::npos) << "Error: " << err_str;
	}

	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "always_lot", "management_policy_attrs": {"opportunistic_GB": 5}})",
						   &raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0) << "Opportunistic_GB contraction should be blocked";

	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "always_lot", "management_policy_attrs": {"max_num_objects": 50}})",
						   &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Max_num_objects contraction should be blocked";

	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "always_lot", "management_policy_attrs": {"expiration_time": 5000}})",
						   &raw_err);
	UniqueCString err4(raw_err);
	EXPECT_NE(rv, 0) << "Expiration_time contraction should be blocked";

	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "always_lot", "management_policy_attrs": {"deletion_time": 6000}})",
						   &raw_err);
	UniqueCString err5(raw_err);
	EXPECT_NE(rv, 0) << "Deletion_time contraction should be blocked";

	// But expansions should still work
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "always_lot", "management_policy_attrs": {"dedicated_GB": 100}})", &raw_err);
	UniqueCString err6(raw_err);
	EXPECT_EQ(rv, 0) << "Dedicated_GB expansion should be allowed: " << (err6.get() ? err6.get() : "");
}

// ============================================================================
// Contraction policy "alive": blocks only for currently-alive lots
// ============================================================================

TEST_F(StrictHierarchyTest, ContractionBlockedAlivePolicy) {
	addDefaultLot();
	// Create a lot that is currently alive (creation far in the past, expiration far in the future)
	addLot(R"({
		"lot_name": "alive_lot",
		"owner": "owner1",
		"parents": ["alive_lot"],
		"paths": [{"path": "/alive/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 1,
			"expiration_time": 99999999999999,
			"deletion_time": 99999999999999
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("contraction_policy", "alive", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// This lot is currently alive, so contraction should be blocked
	rv = lotman_update_lot(R"({"lot_name": "alive_lot", "management_policy_attrs": {"dedicated_GB": 10}})", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Contraction on alive lot should be blocked by 'alive' policy";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("alive") != std::string::npos) << "Error: " << err_str;
}

TEST_F(StrictHierarchyTest, ContractionAllowedForExpiredLotUnderAlivePolicy) {
	addDefaultLot();
	// Create a lot that expired long ago
	addLot(R"({
		"lot_name": "expired_lot",
		"owner": "owner1",
		"parents": ["expired_lot"],
		"paths": [{"path": "/expired/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 1,
			"expiration_time": 2,
			"deletion_time": 3
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("contraction_policy", "alive", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Lot is expired (expiration_time=2), so contraction should be allowed
	rv = lotman_update_lot(R"({"lot_name": "expired_lot", "management_policy_attrs": {"dedicated_GB": 10}})", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Contraction on expired lot should be allowed under 'alive' policy: "
					 << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ContractionBypassedWithAdminOverride) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "override_lot",
		"owner": "owner1",
		"parents": ["override_lot"],
		"paths": [{"path": "/override/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("contraction_policy", "always", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	rv = lotman_set_context_str("admin_override", "true", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0);

	// With admin_override, contraction should be allowed even with "always" policy
	rv =
		lotman_update_lot(R"({"lot_name": "override_lot", "management_policy_attrs": {"dedicated_GB": 10}})", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Admin override should bypass contraction policy: " << (err.get() ? err.get() : "");
}

// ============================================================================
// Contraction in delete: "always" policy blocks lot deletion
// ============================================================================

TEST_F(StrictHierarchyTest, ContractionBlocksDeletionAlwaysPolicy) {
	addDefaultLot();
	// Use a far-future expiration_time so the lot is alive at test time.
	// The 'always' policy should block deletion of alive lots.
	addLot(R"({
		"lot_name": "del_always",
		"owner": "owner1",
		"parents": ["del_always"],
		"paths": [{"path": "/del_always/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 1,
			"expiration_time": 99999999999999,
			"deletion_time": 99999999999999
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("contraction_policy", "always", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	rv = lotman_remove_lots_recursive("del_always", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Deletion of an alive lot should be blocked by 'always' contraction policy";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("always") != std::string::npos) << "Error: " << err_str;
}

TEST_F(StrictHierarchyTest, ExpiredLotDeletableWithoutAdminOverrideUnderAlwaysPolicy) {
	addDefaultLot();
	// Lot whose expiration_time is in the distant past — it is expired.
	addLot(R"({
		"lot_name": "del_expired",
		"owner": "owner1",
		"parents": ["del_expired"],
		"paths": [{"path": "/del_expired/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 1,
			"expiration_time": 2,
			"deletion_time": 3
		}
	})");

	char *raw_err = nullptr;
	// Use the most restrictive contraction policy without admin_override.
	int rv = lotman_set_context_str("contraction_policy", "always", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// The lot is expired, so the appropriate caller should be able to delete it
	// without needing admin_override, even under the 'always' contraction policy.
	rv = lotman_remove_lots_recursive("del_expired", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Expired lot should be deletable without admin_override under 'always' policy: "
					 << (err.get() ? err.get() : "");
}

// ============================================================================
// Parent shrink that would break hierarchy for children
// ============================================================================

TEST_F(StrictHierarchyTest, ParentShrinkBlockedByChildRevalidation) {
	// C1 fix: Axiom re-validation now runs on children when updating a parent's MPAs.
	// Shrinking a parent below a child's attributed value is correctly blocked.
	addDefaultLot();

	// Parent with 20 dedicated_GB
	addLot(R"({
		"lot_name": "shrink_parent",
		"owner": "owner1",
		"parents": ["shrink_parent"],
		"paths": [{"path": "/shrink/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with 15 dedicated_GB (fits within 20)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "shrink_child",
		"owner": "owner1",
		"parents": ["shrink_parent"],
		"paths": [{"path": "/shrink/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 15,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << "Child should be created: " << (err2.get() ? err2.get() : "");

	// Shrink parent to 10 dedicated_GB — this is now blocked because child
	// re-validation detects that the child's attribution (15) exceeds the
	// parent's new dedicated_GB (10).
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "shrink_parent", "management_policy_attrs": {"dedicated_GB": 10}})",
						   &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Parent shrink should be blocked by child re-validation";
	EXPECT_NE(err3.get(), nullptr);
}

TEST_F(StrictHierarchyTest, ParentShrinkExpirationBlockedByChildRevalidation) {
	// C1 fix: Timestamp changes now re-validate Axiom 3 for children.
	// Shrinking a parent's expiration below a child's expiration is blocked.
	addDefaultLot();

	addLot(R"({
		"lot_name": "tshrink_parent",
		"owner": "owner1",
		"parents": ["tshrink_parent"],
		"paths": [{"path": "/tshrink/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with expiration_time=8000 (within parent's 9000)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "tshrink_child",
		"owner": "owner1",
		"parents": ["tshrink_parent"],
		"paths": [{"path": "/tshrink/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Shrink parent's expiration to 7000. Child's expiration (8000) > 7000.
	// This is now blocked because child re-validation detects the Axiom 3
	// violation (child expiration exceeds parent expiration).
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "tshrink_parent", "management_policy_attrs": {"expiration_time": 7000}})",
						   &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Parent timestamp shrink should be blocked by child re-validation";
	EXPECT_NE(err3.get(), nullptr);
}

// ============================================================================
// Temporal path querying: multiple lots on same path at different times
// ============================================================================

TEST_F(StrictHierarchyTest, TemporalPathQueryCorrectLotReturned) {
	addDefaultLot();

	// Lot A: owns /temporal/data from time 1000 to 2000
	addLot(R"({
		"lot_name": "temporal_A",
		"owner": "owner1",
		"parents": ["temporal_A"],
		"paths": [{"path": "/temporal/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 2000,
			"deletion_time": 3000
		}
	})");

	// Lot B: owns /temporal/data from time 3000 to 4000 (no overlap)
	addLot(R"({
		"lot_name": "temporal_B",
		"owner": "owner1",
		"parents": ["temporal_B"],
		"paths": [{"path": "/temporal/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 3000,
			"expiration_time": 4000,
			"deletion_time": 5000
		}
	})");

	char *raw_err = nullptr;
	char **lots_output = nullptr;

	// Query BEFORE either lot's time (t=500): should return "default"
	int rv = lotman_get_lots_from_dir("/temporal/data", false, 500, &lots_output, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Before either lot's time, should return default";
	lotman_free_string_list(lots_output);

	// Query DURING lot A (t=1500): should return "temporal_A"
	rv = lotman_get_lots_from_dir("/temporal/data", false, 1500, &lots_output, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "temporal_A") << "During lot A's time, should return temporal_A";
	lotman_free_string_list(lots_output);

	// Query BETWEEN lots (t=2500): should return "default"
	rv = lotman_get_lots_from_dir("/temporal/data", false, 2500, &lots_output, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Between lots' times, should return default";
	lotman_free_string_list(lots_output);

	// Query DURING lot B (t=3500): should return "temporal_B"
	rv = lotman_get_lots_from_dir("/temporal/data", false, 3500, &lots_output, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "temporal_B") << "During lot B's time, should return temporal_B";
	lotman_free_string_list(lots_output);

	// Query AFTER both lots (t=5000): should return "default"
	rv = lotman_get_lots_from_dir("/temporal/data", false, 5000, &lots_output, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "After both lots' times, should return default";
	lotman_free_string_list(lots_output);
}

// ============================================================================
// Temporal overlap: two lots with same path overlapping in time should fail
// ============================================================================

TEST_F(StrictHierarchyTest, TemporalOverlapFailsWhenAddingPath) {
	addDefaultLot();

	// Lot A: owns /overlap/data from 1000 to 3000
	addLot(R"({
		"lot_name": "overlap_A",
		"owner": "owner1",
		"parents": ["overlap_A"],
		"paths": [{"path": "/overlap/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 3000,
			"deletion_time": 4000
		}
	})");

	// Lot B: created WITHOUT the overlapping path initially
	addLot(R"({
		"lot_name": "overlap_B",
		"owner": "owner1",
		"parents": ["overlap_B"],
		"paths": [{"path": "/overlap/other", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 2500,
			"expiration_time": 4000,
			"deletion_time": 5000
		}
	})");

	// Now try to ADD the conflicting path to lot B via lotman_add_to_lot.
	// This goes through store_new_paths which has the temporal overlap check.
	char *raw_err = nullptr;
	int rv = lotman_add_to_lot(R"({
		"lot_name": "overlap_B",
		"paths": [{"path": "/overlap/data", "recursive": true}]
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Adding a path with temporal overlap should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("temporal overlap") != std::string::npos || err_str.find("overlap") != std::string::npos)
		<< "Error should mention temporal overlap, got: " << err_str;
}

TEST_F(StrictHierarchyTest, TemporalOverlapFailsWhenCreatingLot) {
	addDefaultLot();

	// Lot A: owns /create_overlap/data from 1000 to 3000
	addLot(R"({
		"lot_name": "create_overlap_A",
		"owner": "owner1",
		"parents": ["create_overlap_A"],
		"paths": [{"path": "/create_overlap/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 3000,
			"deletion_time": 4000
		}
	})");

	// Try to CREATE Lot B with the same path and overlapping time range.
	// This goes through write_new() which should now have the temporal overlap check.
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "create_overlap_B",
		"owner": "owner1",
		"parents": ["create_overlap_B"],
		"paths": [{"path": "/create_overlap/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 2500,
			"expiration_time": 4000,
			"deletion_time": 5000
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Creating a lot with a temporally overlapping path should fail";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("temporal overlap") != std::string::npos || err_str.find("overlap") != std::string::npos)
		<< "Error should mention temporal overlap, got: " << err_str;

	// Verify the failed lot doesn't exist
	char *lot_json_str = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_as_json("create_overlap_B", false, &lot_json_str, &raw_err);
	UniqueCString err2(raw_err);
	UniqueCString lotStr(lot_json_str);
	EXPECT_NE(rv, 0) << "Failed lot should not exist in database";
}

TEST_F(StrictHierarchyTest, TemporalNoOverlapSucceeds) {
	addDefaultLot();

	// Lot A: owns /nooverlap/data from 1000 to 2000
	addLot(R"({
		"lot_name": "nooverlap_A",
		"owner": "owner1",
		"parents": ["nooverlap_A"],
		"paths": [{"path": "/nooverlap/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 2000,
			"deletion_time": 3000
		}
	})");

	// Lot B: owns same path from 3000 to 4000 (no overlap — gap between 2000 and 3000)
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "nooverlap_B",
		"owner": "owner1",
		"parents": ["nooverlap_B"],
		"paths": [{"path": "/nooverlap/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 3000,
			"expiration_time": 4000,
			"deletion_time": 5000
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Non-overlapping temporal ranges on same path should succeed: " << (err.get() ? err.get() : "");
}

// ============================================================================
// Hierarchical mode: depth-ordered lot lists
// ============================================================================

TEST_F(StrictHierarchyTest, HierarchicalDepthOrderedResults) {
	addDefaultLot();

	// Create a 3-level hierarchy: root → mid → leaf
	// Each lot has progressively usage that exceeds its dedicated_GB.
	addLot(R"({
		"lot_name": "h_root",
		"owner": "owner1",
		"parents": ["h_root"],
		"paths": [{"path": "/hier/root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 3,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "h_mid",
		"owner": "owner1",
		"parents": ["h_root"],
		"paths": [{"path": "/hier/mid", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 3,
			"opportunistic_GB": 2,
			"max_num_objects": 30,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "h_leaf",
		"owner": "owner1",
		"parents": ["h_mid"],
		"paths": [{"path": "/hier/leaf", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1,
			"opportunistic_GB": 1,
			"max_num_objects": 10,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Set usage on all lots so they all exceed their dedicated_GB:
	// h_leaf: 2 GB self (exceeds 1 GB ded)
	// h_mid: 4 GB self (exceeds 3 GB ded)
	// h_root: 6 GB self (exceeds 5 GB ded)
	char *raw_err = nullptr;
	const char *usage_leaf = R"({"lot_name": "h_leaf", "self_GB": 2})";
	int rv = lotman_update_lot_usage(usage_leaf, false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	const char *usage_mid = R"({"lot_name": "h_mid", "self_GB": 4})";
	rv = lotman_update_lot_usage(usage_mid, false, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	const char *usage_root = R"({"lot_name": "h_root", "self_GB": 6})";
	rv = lotman_update_lot_usage(usage_root, false, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	// Query lots_past_ded in hierarchical mode
	// All three lots are past their dedicated quota.
	// Results should be depth-ordered: deepest (leaf) first.
	char **raw_output = nullptr;
	rv = lotman_get_lots_past_ded(false, false, /*include_reclaimed=*/true, &raw_output, true, &raw_err);
	err.reset(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(output.get(), nullptr);

	// Collect the results
	std::vector<std::string> results;
	for (int i = 0; output.get()[i]; i++) {
		results.push_back(output.get()[i]);
	}

	// At minimum, h_leaf, h_mid, h_root should all be present
	EXPECT_GE(results.size(), 3u) << "Should have at least 3 lots past dedicated";

	// Find positions of our lots
	auto leaf_pos = std::find(results.begin(), results.end(), "h_leaf");
	auto mid_pos = std::find(results.begin(), results.end(), "h_mid");
	auto root_pos = std::find(results.begin(), results.end(), "h_root");

	ASSERT_NE(leaf_pos, results.end()) << "h_leaf should be in results";
	ASSERT_NE(mid_pos, results.end()) << "h_mid should be in results";
	ASSERT_NE(root_pos, results.end()) << "h_root should be in results";

	// Depth-descending order: leaf (depth 2) before mid (depth 1) before root (depth 0)
	EXPECT_LT(leaf_pos, mid_pos) << "h_leaf (deepest) should come before h_mid";
	EXPECT_LT(mid_pos, root_pos) << "h_mid should come before h_root (shallowest)";
}

TEST_F(StrictHierarchyTest, HierarchicalVsNonHierarchical) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "nh_root",
		"owner": "owner1",
		"parents": ["nh_root"],
		"paths": [{"path": "/nh/root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 3,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "nh_child",
		"owner": "owner1",
		"parents": ["nh_root"],
		"paths": [{"path": "/nh/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 2,
			"opportunistic_GB": 1,
			"max_num_objects": 20,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Give nh_child usage that exceeds dedicated
	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(R"({"lot_name": "nh_child", "self_GB": 3})", false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	// Non-hierarchical: just check raw self_GB vs dedicated_GB
	char **raw_output = nullptr;
	rv = lotman_get_lots_past_ded(false, false, /*include_reclaimed=*/true, &raw_output, false, &raw_err);
	err.reset(raw_err);
	UniqueStringList output_nonhier(raw_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	std::vector<std::string> nonhier_results;
	if (output_nonhier.get()) {
		for (int i = 0; output_nonhier.get()[i]; i++) {
			nonhier_results.push_back(output_nonhier.get()[i]);
		}
	}

	// nh_child should be in non-hierarchical results (3 > 2)
	EXPECT_NE(std::find(nonhier_results.begin(), nonhier_results.end(), "nh_child"), nonhier_results.end())
		<< "nh_child should appear in non-hierarchical results";
}

// ============================================================================
// Context functionality: strict_hierarchy enforces axioms on lot creation
// ============================================================================

TEST_F(StrictHierarchyTest, StrictHierarchyOffAllowsViolation) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "nostrict_parent",
		"owner": "owner1",
		"parents": ["nostrict_parent"],
		"paths": [{"path": "/nostrict/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// strict_hierarchy is OFF (default). Creating a child that would violate Axiom 1
	// should succeed because axioms are not enforced.
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "nostrict_child",
		"owner": "owner1",
		"parents": ["nostrict_parent"],
		"paths": [{"path": "/nostrict/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 999,
			"opportunistic_GB": 999,
			"max_num_objects": 9999,
			"creation_time": 1,
			"expiration_time": 99999,
			"deletion_time": 99999
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Without strict hierarchy, axiom violations should be allowed: "
					 << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, StrictHierarchyOnEnforcesAllAxioms) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "strict_parent",
		"owner": "owner1",
		"parents": ["strict_parent"],
		"paths": [{"path": "/strict/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 500,
			"expiration_time": 5000,
			"deletion_time": 6000
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// This child violates ALL three axioms:
	// - dedicated_GB=999 > parent's 10 (Axiom 1)
	// - creation_time=1 < parent's 500 (Axiom 3 — only checked if Axiom 1 passes)
	// The check should fail on the first axiom violation it encounters.
	rv = lotman_add_lot(R"({
		"lot_name": "strict_child",
		"owner": "owner1",
		"parents": ["strict_parent"],
		"paths": [{"path": "/strict/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 999,
			"opportunistic_GB": 999,
			"max_num_objects": 9999,
			"creation_time": 1,
			"expiration_time": 99999,
			"deletion_time": 99999
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Strict hierarchy should block axiom-violating lot creation";
	EXPECT_NE(err.get(), nullptr);
}

// ============================================================================
// Update attributions API test
// ============================================================================

TEST_F(StrictHierarchyTest, UpdateAttributionsRebalances) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "ua_parent1",
		"owner": "owner1",
		"parents": ["ua_parent1"],
		"paths": [{"path": "/ua/p1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "ua_parent2",
		"owner": "owner1",
		"parents": ["ua_parent2"],
		"paths": [{"path": "/ua/p2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Child with 20 dedicated_GB, equal split across two parents (10 each by default)
	addLot(R"({
		"lot_name": "ua_child",
		"owner": "owner1",
		"parents": ["ua_parent1", "ua_parent2"],
		"paths": [{"path": "/ua/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Now update attributions to assign 15 to parent1 and 5 to parent2
	char *raw_err = nullptr;
	const char *update_json = R"({
		"lot_name": "ua_child",
		"parent_attributions": {
			"ua_parent1": {
				"dedicated_GB": 15,
				"opportunistic_GB": 8,
				"max_num_objects": 150
			},
			"ua_parent2": {
				"dedicated_GB": 5,
				"opportunistic_GB": 2,
				"max_num_objects": 50
			}
		}
	})";

	int rv = lotman_update_lot(update_json, &raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Updating attributions should succeed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, UpdateAttributionsViolatesAxiomInStrictMode) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "uav_parent1",
		"owner": "owner1",
		"parents": ["uav_parent1"],
		"paths": [{"path": "/uav/p1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	addLot(R"({
		"lot_name": "uav_parent2",
		"owner": "owner1",
		"parents": ["uav_parent2"],
		"paths": [{"path": "/uav/p2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Child with 20 dedicated_GB
	addLot(R"({
		"lot_name": "uav_child",
		"owner": "owner1",
		"parents": ["uav_parent1", "uav_parent2"],
		"paths": [{"path": "/uav/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Enable strict hierarchy
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Try to attribute 2 to parent1 and 18 to parent2. But parent2 only has 5.
	const char *update_json = R"({
		"lot_name": "uav_child",
		"parent_attributions": {
			"uav_parent1": {
				"dedicated_GB": 2,
				"opportunistic_GB": 1,
				"max_num_objects": 20
			},
			"uav_parent2": {
				"dedicated_GB": 18,
				"opportunistic_GB": 9,
				"max_num_objects": 180
			}
		}
	})";

	rv = lotman_update_lot(update_json, &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Attribution update exceeding parent capacity should fail in strict mode";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("exceeds the parent's") != std::string::npos)
		<< "Error should report that the child's allocation exceeds the parent's, got: " << err_str;
}

// ============================================================================
// Migration test: v0→v1 adds parent_child_attributions table
// ============================================================================

TEST_F(StrictHierarchyTest, MigrationCreatesAttributionTable) {
	// The DB has already been initialized by SetUp (setting lot_home).
	// Verify the parent_child_attributions table exists by creating a lot
	// with attributions.
	addDefaultLot();

	addLot(R"({
		"lot_name": "migrate_parent",
		"owner": "owner1",
		"parents": ["migrate_parent"],
		"paths": [{"path": "/migrate/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Creating a child lot triggers compute_and_store_attributions which writes
	// to parent_child_attributions. If the table doesn't exist, this will crash.
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "migrate_child",
		"owner": "owner1",
		"parents": ["migrate_parent"],
		"paths": [{"path": "/migrate/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		},
		"parent_attributions": {
			"migrate_parent": {
				"dedicated_GB": 10,
				"opportunistic_GB": 5,
				"max_num_objects": 50
			}
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Creating lot with attributions should work on fresh DB: " << (err.get() ? err.get() : "");
}

// ============================================================================
// C6: Non-root lot MPA updates with strict hierarchy
// ============================================================================

TEST_F(StrictHierarchyTest, NonRootMPAUpdateAllowed) {
	// Updating a non-root (child) lot's MPAs should succeed when the change
	// still satisfies all axioms.
	addDefaultLot();

	addLot(R"({
		"lot_name": "nrm_parent",
		"owner": "owner1",
		"parents": ["nrm_parent"],
		"paths": [{"path": "/nrm/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with dedicated_GB=10 (<<< parent's 100)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "nrm_child",
		"owner": "owner1",
		"parents": ["nrm_parent"],
		"paths": [{"path": "/nrm/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << "Child creation: " << (err2.get() ? err2.get() : "");

	// Increase child's dedicated_GB to 20 — still fits within parent's 100
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "nrm_child", "management_policy_attrs": {"dedicated_GB": 20}})", &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_EQ(rv, 0) << "Non-root MPA increase should succeed: " << (err3.get() ? err3.get() : "");
}

TEST_F(StrictHierarchyTest, NonRootMPAUpdateBlockedByAxiom1) {
	// Increasing a child's dedicated_GB beyond its attributed portion to the
	// parent should fail Axiom 1 validation.
	addDefaultLot();

	addLot(R"({
		"lot_name": "nra1_parent",
		"owner": "owner1",
		"parents": ["nra1_parent"],
		"paths": [{"path": "/nra1/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with dedicated_GB=10, default attributions will set dedicated_GB=10
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "nra1_child",
		"owner": "owner1",
		"parents": ["nra1_parent"],
		"paths": [{"path": "/nra1/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Now try to increase child's dedicated_GB to 50. Even though parent has 30
	// total, the child's attribution to the parent was set to 10 at creation,
	// so 50 > 10 = Axiom 1 violation (child MPA exceeds attributed amount).
	// Note: Axiom 1 checks that each child→parent attribution covers the child's MPAs.
	// The attribution was 10, but now the child's MPA is 50 — the attribution
	// no longer covers the child.
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "nra1_child", "management_policy_attrs": {"dedicated_GB": 50}})", &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Non-root MPA update should be blocked by Axiom 1 (attribution too small)";
	EXPECT_NE(err3.get(), nullptr);
}

TEST_F(StrictHierarchyTest, NonRootTimestampShrinkBlockedByAxiom3) {
	// Narrowing a non-root child's timestamps outside its parent's range
	// should fail Axiom 3.
	addDefaultLot();

	addLot(R"({
		"lot_name": "nrt_parent",
		"owner": "owner1",
		"parents": ["nrt_parent"],
		"paths": [{"path": "/nrt/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1000,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with creation_time=1000, expiration_time=8000
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "nrt_child",
		"owner": "owner1",
		"parents": ["nrt_parent"],
		"paths": [{"path": "/nrt/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 1000,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Try to push child's expiration_time past parent's (9000 → 9500).
	// Axiom 3: child.expiration_time must <= parent.expiration_time.
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "nrt_child", "management_policy_attrs": {"expiration_time": 9500}})",
						   &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Child timestamp expansion beyond parent should be blocked by Axiom 3";
	EXPECT_NE(err3.get(), nullptr);
}

// ============================================================================
// C7: DB cleanliness after failed lot creation
// ============================================================================

TEST_F(StrictHierarchyTest, FailedLotCreationLeavesNoDBResidue) {
	// When lot creation fails axiom validation, the lot should be completely
	// removed from the database (rolled back via delete_lot_from_db).
	addDefaultLot();

	addLot(R"({
		"lot_name": "clean_parent",
		"owner": "owner1",
		"parents": ["clean_parent"],
		"paths": [{"path": "/clean/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	// Try to create child with dedicated_GB=50, exceeding parent's 10.
	// This should fail axiom validation.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "clean_child_fail",
		"owner": "owner1",
		"parents": ["clean_parent"],
		"paths": [{"path": "/clean/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0) << "Lot creation should fail with axiom violation";

	// Verify the lot does not exist in the database
	raw_err = nullptr;
	char *lot_json_str = nullptr;
	rv = lotman_get_lot_as_json("clean_child_fail", false, &lot_json_str, &raw_err);
	UniqueCString err3(raw_err);
	UniqueCString lotStr(lot_json_str);
	EXPECT_NE(rv, 0) << "Failed lot should not exist in database";
}

// ============================================================================
// C5: Schema validation for update_attributions
// ============================================================================

TEST_F(StrictHierarchyTest, UpdateAttributionsRejectsInvalidSchema) {
	// update_attributions should reject JSON that doesn't match the schema.
	addDefaultLot();

	addLot(R"({
		"lot_name": "schema_parent",
		"owner": "owner1",
		"parents": ["schema_parent"],
		"paths": [{"path": "/schema/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "schema_child",
		"owner": "owner1",
		"parents": ["schema_parent"],
		"paths": [{"path": "/schema/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Pass invalid attributions: dedicated_GB should be a number, not a string
	raw_err = nullptr;
	rv = lotman_update_lot(R"({
		"lot_name": "schema_child",
		"parent_attributions": {
			"schema_parent": {
				"dedicated_GB": "not_a_number"
			}
		}
	})",
						   &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Should reject invalid schema (string instead of number)";
	EXPECT_NE(err3.get(), nullptr);
}

TEST_F(StrictHierarchyTest, UpdateAttributionsRejectsNegativeValues) {
	// Negative values should be rejected by the schema (minimum: 0).
	addDefaultLot();

	addLot(R"({
		"lot_name": "neg_parent",
		"owner": "owner1",
		"parents": ["neg_parent"],
		"paths": [{"path": "/neg/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "neg_child",
		"owner": "owner1",
		"parents": ["neg_parent"],
		"paths": [{"path": "/neg/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Pass attributions with negative dedicated_GB
	raw_err = nullptr;
	rv = lotman_update_lot(R"({
		"lot_name": "neg_child",
		"parent_attributions": {
			"neg_parent": {
				"dedicated_GB": -5
			}
		}
	})",
						   &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Should reject negative attribution values";
	EXPECT_NE(err3.get(), nullptr);
}

// ============================================================================
// Bug fix: attributions recomputed correctly after add/remove parents
// ============================================================================

TEST_F(StrictHierarchyTest, AddParentRecomputesAttributions) {
	// Regression test: add_parents must reload parents & MPAs from DB
	// before calling compute_and_store_attributions.
	addDefaultLot();

	// Create two root lots that will be parents
	addLot(R"({
		"lot_name": "ap_parent1",
		"owner": "owner1",
		"parents": ["ap_parent1"],
		"paths": [{"path": "/ap/p1", "recursive": true}],
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
		"lot_name": "ap_parent2",
		"owner": "owner1",
		"parents": ["ap_parent2"],
		"paths": [{"path": "/ap/p2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Create child under parent1 only (strict mode on)
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "ap_child",
		"owner": "owner1",
		"parents": ["ap_parent1"],
		"paths": [{"path": "/ap/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Add parent2 as a second parent
	raw_err = nullptr;
	rv = lotman_add_to_lot(R"({"lot_name": "ap_child", "parents": ["ap_parent2"]})", &raw_err);
	UniqueCString err3(raw_err);
	ASSERT_EQ(rv, 0) << "add_parents should succeed: " << (err3.get() ? err3.get() : "");

	// Verify attribution records exist for both parents by querying DB directly
	{
		using namespace sqlite_orm;
		auto &storage = lotman::db::StorageManager::get_storage();
		auto attrs = storage.get_all<lotman::db::ParentChildAttribution>(
			where(c(&lotman::db::ParentChildAttribution::child_lot_name) == "ap_child"));

		// Should have attributions for both parents (3 MPA keys × 2 parents = 6 records)
		EXPECT_EQ(attrs.size(), 6u) << "Expected 6 attribution records (3 keys × 2 parents), got " << attrs.size();

		// Verify each parent has attributions
		int p1_count = 0, p2_count = 0;
		for (const auto &a : attrs) {
			if (a.parent_lot_name == "ap_parent1")
				p1_count++;
			if (a.parent_lot_name == "ap_parent2")
				p2_count++;
			// Equal split: each fraction should be 0.5
			EXPECT_NEAR(a.fraction, 0.5, 0.01)
				<< "Attribution for " << a.parent_lot_name << "/" << a.mpa_key << " should be 0.5, got " << a.fraction;
		}
		EXPECT_EQ(p1_count, 3) << "parent1 should have 3 attribution records";
		EXPECT_EQ(p2_count, 3) << "parent2 should have 3 attribution records";
	}
}

TEST_F(StrictHierarchyTest, RemoveParentRecomputesAttributions) {
	// Regression test: remove_parents must reload parents & MPAs from DB
	// before calling compute_and_store_attributions.
	addDefaultLot();

	addLot(R"({
		"lot_name": "rp_parent1",
		"owner": "owner1",
		"parents": ["rp_parent1"],
		"paths": [{"path": "/rp/p1", "recursive": true}],
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
		"lot_name": "rp_parent2",
		"owner": "owner1",
		"parents": ["rp_parent2"],
		"paths": [{"path": "/rp/p2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Create child under both parents
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0);

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "rp_child",
		"owner": "owner1",
		"parents": ["rp_parent1", "rp_parent2"],
		"paths": [{"path": "/rp/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	ASSERT_EQ(rv, 0) << (err2.get() ? err2.get() : "");

	// Remove parent2
	raw_err = nullptr;
	rv = lotman_rm_parents_from_lot(R"({"lot_name": "rp_child", "parents": ["rp_parent2"]})", &raw_err);
	UniqueCString err3(raw_err);
	ASSERT_EQ(rv, 0) << "remove_parents should succeed: " << (err3.get() ? err3.get() : "");

	// Verify attribution records: should only have parent1 with fraction=1.0
	{
		using namespace sqlite_orm;
		auto &storage = lotman::db::StorageManager::get_storage();
		auto attrs = storage.get_all<lotman::db::ParentChildAttribution>(
			where(c(&lotman::db::ParentChildAttribution::child_lot_name) == "rp_child"));

		// Should have 3 records (3 MPA keys × 1 remaining parent)
		EXPECT_EQ(attrs.size(), 3u) << "Expected 3 attribution records (3 keys × 1 parent), got " << attrs.size();

		for (const auto &a : attrs) {
			EXPECT_EQ(a.parent_lot_name, "rp_parent1")
				<< "All attributions should be to parent1 after removing parent2";
			// Sole parent: fraction should be 1.0
			EXPECT_NEAR(a.fraction, 1.0, 0.01)
				<< "Attribution fraction for sole parent should be 1.0, got " << a.fraction;
		}
	}
}

// ============================================================================
// Time-aware Axiom 2 tests (sweep-line)
// ============================================================================

TEST_F(StrictHierarchyTest, Axiom2NonOverlappingChildrenAllowed) {
	// Two non-overlapping children whose combined allocation exceeds parent,
	// but since they don't overlap temporally, peak is within limits → PASS
	addDefaultLot();
	addRootLot(); // 100 ded, 50 opp, 1000 obj, [100, 9000]

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: 60 ded, active [200, 3000) — exceeds half of parent's 100
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// Child B: 60 ded, active [3000, 8000) — non-overlapping with child_a
	// Combined 60+60=120 > 100, but peak at any moment is 60 ≤ 100
	addLot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 3000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
	// If we get here, both lots were created — no axiom violation
}

TEST_F(StrictHierarchyTest, Axiom2OverlappingChildrenBlocked) {
	// Two overlapping children whose combined allocation exceeds parent → FAIL
	addDefaultLot();
	addRootLot(); // 100 ded, [100, 9000]

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: 60 ded, active [200, 5000)
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Child B: 60 ded, active [4000, 8000) — overlaps child_a during [4000, 5000)
	// Peak = 60+60=120 > 100 → should fail
	rv = lotman_add_lot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Expected axiom 2 violation for overlapping children exceeding parent capacity";
	EXPECT_NE(std::string(err.get()).find("peak concurrent"), std::string::npos) << "Error: " << err.get();
}

TEST_F(StrictHierarchyTest, Axiom2BoundaryExactNoOverlap) {
	// Child A ends exactly when child B starts → no overlap → PASS
	addDefaultLot();
	addRootLot(); // 100 ded, [100, 9000]

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: 80 ded, active [200, 4000)
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 80,
			"opportunistic_GB": 10,
			"max_num_objects": 500,
			"creation_time": 200,
			"expiration_time": 4000,
			"deletion_time": 4500
		}
	})");

	// Child B: 80 ded, active [4000, 8000) — starts exactly when A expires
	// At time 4000: A's removal event fires before B's addition (tie-break)
	// so peak remains 80 ≤ 100 → should pass
	addLot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 80,
			"opportunistic_GB": 10,
			"max_num_objects": 500,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
}

TEST_F(StrictHierarchyTest, Axiom2ExtendExpirationCausesOverlap) {
	// Two children that don't overlap initially, then extending child A's
	// expiration makes them overlap → should fail on MPA update
	addDefaultLot();
	addRootLot(); // 100 ded, [100, 9000]

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: 60 ded, active [200, 3000)
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// Child B: 60 ded, active [3000, 8000) — no overlap
	addLot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 3000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Now extend child A's expiration to 5000 → overlaps B during [3000, 5000)
	// Peak = 120 > 100 → should fail
	rv =
		lotman_update_lot(R"({"lot_name": "child_a", "management_policy_attrs": {"expiration_time": 5000}})", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Expected axiom 2 violation when extending expiration creates overlap";
	EXPECT_NE(std::string(err.get()).find("peak concurrent"), std::string::npos) << "Error: " << err.get();
}

TEST_F(StrictHierarchyTest, Axiom2ThreeChildrenPartialOverlapUnderLimit) {
	// Three children with partial overlaps but peak at any moment ≤ parent → PASS
	addDefaultLot();
	addRootLot(); // 100 ded, 50 opp, 1000 obj, [100, 9000]

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// A: 40 ded, [200, 3000)
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// B: 40 ded, [2000, 5000) — overlaps A during [2000, 3000), peak=80 ≤ 100
	addLot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 2000,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// C: 40 ded, [4000, 8000) — overlaps B during [4000, 5000), peak=80 ≤ 100
	// No triple overlap; max concurrent at any time is 80 ≤ 100
	addLot(R"({
		"lot_name": "child_c",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");
}

TEST_F(StrictHierarchyTest, Axiom2ThreeChildrenPartialOverlapExceedsLimit) {
	// Three children with a moment of triple overlap that exceeds parent → FAIL
	addDefaultLot();
	addRootLot(); // 100 ded, [100, 9000]

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// A: 40 ded, [200, 5000)
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// B: 40 ded, [2000, 7000) — overlaps A during [2000, 5000), peak A+B=80
	addLot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 2000,
			"expiration_time": 7000,
			"deletion_time": 7500
		}
	})");

	// C: 40 ded, [3000, 8000) — triple overlap during [3000, 5000) = 120 > 100
	rv = lotman_add_lot(R"({
		"lot_name": "child_c",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 3000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Expected axiom 2 violation for triple overlap exceeding parent";
	EXPECT_NE(std::string(err.get()).find("peak concurrent"), std::string::npos) << "Error: " << err.get();
}

TEST_F(StrictHierarchyTest, Axiom2IndependentPeaksAtDifferentTimes) {
	// Sweep-line correctness with INDEPENDENT storage pools: ded and opp are
	// tracked as separate axes. Each axis has its own peak across staggered
	// time intervals; the per-axis peaks must not exceed the parent's per-axis
	// allotment, and they're allowed to occur at different moments.
	//
	// Setup: Parent ded=100, opp=60.
	//   A [100,400): 90 ded + 10 opp
	//   B [300,600): 10 ded + 40 opp
	//   C [500,800): 50 ded + 20 opp
	//
	// Per-axis peaks:
	//   ded peak at t∈[300,400): A+B = 100 ≤ 100 ✓
	//   opp peak at t∈[500,600): B+C = 60  ≤ 60  ✓
	// Independent peaks occur at different times; both axes fit individually.

	addDefaultLot();
	// Root: ded=100, opp=60, [100, 9000]
	addLot(R"({
		"lot_name": "root",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 60,
			"max_num_objects": 10000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: 90 ded, 10 opp, [100, 400)
	addLot(R"({
		"lot_name": "child_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 90,
			"opportunistic_GB": 10,
			"max_num_objects": 300,
			"creation_time": 100,
			"expiration_time": 400,
			"deletion_time": 500
		}
	})");

	// Child B: 10 ded, 40 opp, [300, 600) — overlaps A during [300, 400)
	addLot(R"({
		"lot_name": "child_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 40,
			"max_num_objects": 300,
			"creation_time": 300,
			"expiration_time": 600,
			"deletion_time": 700
		}
	})");

	// Child C: 50 ded, 20 opp, [500, 800) — overlaps B during [500, 600)
	// This should PASS because actual concurrent peak = 150 at t=300, under limit of 155
	rv = lotman_add_lot(R"({
		"lot_name": "child_c",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 20,
			"max_num_objects": 300,
			"creation_time": 500,
			"expiration_time": 800,
			"deletion_time": 900
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Should pass: per-axis peaks (ded=100, opp=60) each fit within parent's per-axis caps; "
						"independent peaks occur at different times. Error: "
					 << err.get();
}

// ============================================================================
// Re-validation gap tests (path-temporal overlap on MPA/path updates)
// ============================================================================

TEST_F(StrictHierarchyTest, ExtendExpirationCausesPathOverlap) {
	// Two lots with same path but non-overlapping times. Extending lot A's
	// expiration_time so it overlaps lot B should fail.
	addDefaultLot();
	addRootLot();

	// Lot A: path /root/data/shared, [200, 3000)
	addLot(R"({
		"lot_name": "lot_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// Lot B: same path, [3000, 6000) — no overlap
	addLot(R"({
		"lot_name": "lot_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 3000,
			"expiration_time": 6000,
			"deletion_time": 6500
		}
	})");

	// Now extend lot_a's expiration to 4000 → overlaps lot_b's path during [3000, 4000)
	char *raw_err = nullptr;
	int rv =
		lotman_update_lot(R"({"lot_name": "lot_a", "management_policy_attrs": {"expiration_time": 4000}})", &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Expected path temporal overlap failure when extending expiration";
	EXPECT_NE(std::string(err.get()).find("temporal overlap"), std::string::npos) << "Error: " << err.get();
}

TEST_F(StrictHierarchyTest, UpdatePathToConflictingPathFails) {
	// Two lots with different paths but overlapping times. Changing lot A's path
	// to match lot B's should fail.
	addDefaultLot();
	addRootLot();

	// Lot A: path /root/data/a, [200, 5000)
	addLot(R"({
		"lot_name": "lot_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Lot B: path /root/data/b, [1000, 4000) — overlapping times with A
	addLot(R"({
		"lot_name": "lot_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 4000,
			"deletion_time": 4500
		}
	})");

	// Change lot_a's path from /root/data/a to /root/data/b → temporal overlap with lot_b
	char *raw_err = nullptr;
	int rv = lotman_update_lot(
		R"({"lot_name": "lot_a", "paths": [{"current": "/root/data/a", "new": "/root/data/b", "recursive": true}]})",
		&raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Expected path temporal overlap when changing path to conflicting one";
	EXPECT_NE(std::string(err.get()).find("temporal overlap"), std::string::npos) << "Error: " << err.get();
}

TEST_F(StrictHierarchyTest, UpdatePathToNonConflictingPathSucceeds) {
	// Two lots with different paths and non-overlapping times. Changing lot A's path
	// to match lot B's should succeed since times don't overlap.
	addDefaultLot();
	addRootLot();

	// Lot A: path /root/data/a, [200, 3000)
	addLot(R"({
		"lot_name": "lot_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// Lot B: path /root/data/b, [4000, 7000) — non-overlapping times with A
	addLot(R"({
		"lot_name": "lot_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 4000,
			"expiration_time": 7000,
			"deletion_time": 7500
		}
	})");

	// Change lot_a's path to /root/data/b — should succeed (no temporal overlap)
	char *raw_err = nullptr;
	int rv = lotman_update_lot(
		R"({"lot_name": "lot_a", "paths": [{"current": "/root/data/a", "new": "/root/data/b", "recursive": true}]})",
		&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Path update should succeed for non-overlapping times: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, UpdatePathExcludeFalseConflictsFails) {
	// Regression test for C2 bug: flipping exclude from true→false must trigger temporal
	// overlap validation even when path string stays the same.
	//
	// Setup:
	//   Lot A: path /root/data/shared, exclude=true, [200, 5000)
	//   Lot B: path /root/data/shared, exclude=false, [1000, 4000) (overlapping times with A)
	//
	// Action: Update lot A to flip exclude=false (path string unchanged)
	// Expected: Reject due to temporal overlap with lot B on /root/data/shared
	addDefaultLot();
	addRootLot();

	// Lot A with excluded path during [200, 5000)
	addLot(R"({
		"lot_name": "lot_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true, "exclude": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Lot B with same path but exclude=false during overlapping times [1000, 4000)
	addLot(R"({
		"lot_name": "lot_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true, "exclude": false}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 4000,
			"deletion_time": 4500
		}
	})");

	// Attempt to flip lot_a's exclude flag from true → false (path string unchanged)
	// This should fail because lot A would now conflict with lot B on /root/data/shared during [1000, 4000)
	char *raw_err = nullptr;
	int rv = lotman_update_lot(
		R"({"lot_name": "lot_a", "paths": [{"current": "/root/data/shared", "new": "/root/data/shared", "recursive": true, "exclude": false}]})",
		&raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Flipping exclude to false should trigger temporal overlap check even when path unchanged";
	EXPECT_NE(std::string(err.get()).find("temporal overlap"), std::string::npos)
		<< "Error should mention temporal overlap. Got: " << err.get();
}

// ============================================================================
// Advisory capacity query tests (informational — not a reservation mechanism)
// ============================================================================

TEST_F(StrictHierarchyTest, AvailableCapacityNoChildren) {
	// Parent with no children should report full capacity available
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	auto [result, err_msg] = lotman::Lot::get_available_capacity("root", 100, 9000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;
	EXPECT_DOUBLE_EQ(result["available_dedicated_GB"].get<double>(), 100.0);
	EXPECT_DOUBLE_EQ(result["available_opportunistic_GB"].get<double>(), 50.0);
	EXPECT_EQ(result["available_max_num_objects"].get<int64_t>(), 1000);
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 0.0);
	EXPECT_DOUBLE_EQ(result["peak_opportunistic_GB"].get<double>(), 0.0);
	EXPECT_EQ(result["peak_max_num_objects"].get<int64_t>(), 0);
}

TEST_F(StrictHierarchyTest, AvailableCapacityOneChild) {
	// Parent with one child — capacity reduced during child's interval
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "child1",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	auto [result, err_msg] = lotman::Lot::get_available_capacity("root", 100, 9000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;
	EXPECT_DOUBLE_EQ(result["available_dedicated_GB"].get<double>(), 70.0);
	EXPECT_DOUBLE_EQ(result["available_opportunistic_GB"].get<double>(), 40.0);
	EXPECT_EQ(result["available_max_num_objects"].get<int64_t>(), 800);
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 30.0);
}

TEST_F(StrictHierarchyTest, AvailableCapacityPartialOverlap) {
	// Query window only partially overlaps child — still reports peak during overlap
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child lives [200, 5000)
	addLot(R"({
		"lot_name": "child1",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 20,
			"max_num_objects": 300,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Query window [3000, 9000) — overlaps child's [200, 5000) partially
	auto [result, err_msg] = lotman::Lot::get_available_capacity("root", 3000, 9000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;
	// Peak is from the single child's attribution during the overlap
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 40.0);
	EXPECT_DOUBLE_EQ(result["available_dedicated_GB"].get<double>(), 60.0);
}

TEST_F(StrictHierarchyTest, AvailableCapacityNonOverlappingChildren) {
	// Two children that don't overlap — peak is the max of each individually
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: [200, 3000), 30 GB dedicated
	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// Child B: [3000, 6000), 50 GB dedicated — starts exactly when A ends
	addLot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 20,
			"max_num_objects": 200,
			"creation_time": 3000,
			"expiration_time": 6000,
			"deletion_time": 6500
		}
	})");

	auto [result, err_msg] = lotman::Lot::get_available_capacity("root", 100, 9000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;
	// They don't overlap, so peak is the max of the two (50 GB)
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 50.0);
	EXPECT_DOUBLE_EQ(result["available_dedicated_GB"].get<double>(), 50.0);
}

TEST_F(StrictHierarchyTest, AvailableCapacityOverlappingChildren) {
	// Two children whose intervals overlap — peak is their sum during overlap
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A: [200, 5000), 30 GB dedicated
	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Child B: [4000, 8000), 50 GB dedicated — overlaps A during [4000, 5000)
	addLot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 20,
			"max_num_objects": 200,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	auto [result, err_msg] = lotman::Lot::get_available_capacity("root", 100, 9000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;
	// During [4000, 5000), both are active: 30 + 50 = 80 GB
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 80.0);
	EXPECT_DOUBLE_EQ(result["available_dedicated_GB"].get<double>(), 20.0);
}

TEST_F(StrictHierarchyTest, AvailableCapacityQueryOutsideChildren) {
	// Query window doesn't overlap any child — full capacity
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child: [200, 3000)
	addLot(R"({
		"lot_name": "child1",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 15,
			"max_num_objects": 300,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	// Query window [5000, 8000) — child already expired
	auto [result, err_msg] = lotman::Lot::get_available_capacity("root", 5000, 8000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 0.0);
	EXPECT_DOUBLE_EQ(result["available_dedicated_GB"].get<double>(), 100.0);
	EXPECT_DOUBLE_EQ(result["available_opportunistic_GB"].get<double>(), 50.0);
	EXPECT_EQ(result["available_max_num_objects"].get<int64_t>(), 1000);
}

TEST_F(StrictHierarchyTest, AvailableCapacityNonexistentParent) {
	// Querying a non-existent parent returns an error
	addDefaultLot();

	auto [result, err_msg] = lotman::Lot::get_available_capacity("no_such_lot", 100, 9000);
	EXPECT_FALSE(err_msg.empty());
	EXPECT_TRUE(result.empty());
}

TEST_F(StrictHierarchyTest, AvailableCapacityCApi) {
	// Exercise the C entry point lotman_get_available_capacity end-to-end.
	addDefaultLot();
	addRootLot(); // root: 100 ded, 50 opp, 1000 obj, [100, 9000)

	addLot(R"({
		"lot_name": "c_api_child",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/c_api_child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	char *raw_out = nullptr;
	char *raw_err = nullptr;
	int rv = lotman_get_available_capacity("root", 100, 9000, &raw_out, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "C API call failed: " << (err.get() ? err.get() : "");
	ASSERT_NE(raw_out, nullptr);

	auto out = nlohmann::json::parse(raw_out);
	free(raw_out);

	// Child's full attribution (equal-split with one parent) is its own MPAs.
	EXPECT_DOUBLE_EQ(out["peak_dedicated_GB"].get<double>(), 30.0);
	EXPECT_DOUBLE_EQ(out["available_dedicated_GB"].get<double>(), 70.0);
	EXPECT_DOUBLE_EQ(out["peak_opportunistic_GB"].get<double>(), 10.0);
	EXPECT_DOUBLE_EQ(out["available_opportunistic_GB"].get<double>(), 40.0);
	EXPECT_EQ(out["peak_max_num_objects"].get<int64_t>(), 200);
	EXPECT_EQ(out["available_max_num_objects"].get<int64_t>(), 800);

	// Null parent_lot_name should error
	raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_available_capacity(nullptr, 100, 9000, &raw_out, &raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_EQ(raw_out, nullptr);

	// Non-existent parent should error and not allocate output
	raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_available_capacity("nope", 100, 9000, &raw_out, &raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0);
	EXPECT_EQ(raw_out, nullptr);
}

// ============================================================================
// Atomic reservation test (lot creation is the reservation mechanism)
// ============================================================================

TEST_F(StrictHierarchyTest, LotCreationEnforcesCapacityAtomically) {
	// Verify that lot creation itself is the capacity enforcement mechanism:
	// creating a child that would exceed the parent's capacity fails atomically.
	addDefaultLot();
	addRootLot(); // root: 100 ded, 50 opp, 1000 obj, [100, 9000)
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child A consumes 60 of 100 dedicated_GB during [200, 5000)
	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Child B tries to take 50 during [4000, 8000) — overlaps A during [4000, 5000)
	// Peak would be 60+50=110 > 100, so Axiom 2 should block this atomically
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Lot creation should fail when overlapping children exceed parent capacity";
	ASSERT_NE(err.get(), nullptr);
	std::string err_str(err.get());
	EXPECT_TRUE(err_str.find("peak concurrent") != std::string::npos || err_str.find("exceeds") != std::string::npos)
		<< "Error should report combined concurrent children exceed parent capacity: " << err_str;

	// But Child B with non-overlapping time window [5000, 8000) should succeed
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 5000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Lot creation should succeed when children don't overlap: " << (err.get() ? err.get() : "");
}

// ============================================================================
// C API reservation edge-case tests
// These exercise the full reservation lifecycle through the external C API,
// testing every mutation path that can affect capacity enforcement.
// ============================================================================

// --- lotman_add_lot: reservation creation ---

TEST_F(StrictHierarchyTest, ReserveExactCapacity) {
	// A single child reserving exactly 100% of the parent's resources should succeed
	addDefaultLot();
	addRootLot(); // root: 100 ded, 50 opp, 1000 obj, [100, 9000)
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "full_child",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/full", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Reserving exactly 100% of parent capacity should succeed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ReserveOverCapacityDedicatedOnly) {
	// A single child exceeding parent's dedicated_GB must fail
	addDefaultLot();
	addRootLot(); // 100 ded
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "greedy",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/greedy", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 101,
			"opportunistic_GB": 0,
			"max_num_objects": 1,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Exceeding parent dedicated_GB should fail";
}

TEST_F(StrictHierarchyTest, ReserveOverCapacityDedPlusOpp) {
	// dedicated fits, but dedicated+opportunistic exceeds parent's ded+opp total
	addDefaultLot();
	addRootLot(); // 100 ded + 50 opp = 150 total
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "opp_hog",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/opp", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 90,
			"opportunistic_GB": 70,
			"max_num_objects": 1,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "ded(90)+opp(70)=160 exceeding parent total(150) should fail";
}

TEST_F(StrictHierarchyTest, ReserveOverCapacityMaxObjects) {
	// Only max_num_objects exceeds — should still fail
	addDefaultLot();
	addRootLot(); // 1000 max_num_objects
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "obj_hog",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/obj", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1,
			"opportunistic_GB": 1,
			"max_num_objects": 1001,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Exceeding parent max_num_objects should fail";
}

TEST_F(StrictHierarchyTest, ReserveSecondChildNonOverlappingTimeFitsExactly) {
	// First child takes 100% of capacity during [200, 5000).
	// Second child also takes 100% during [5000, 8000) — no temporal overlap, should succeed.
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "first",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/first", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "second",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/second", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 5000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Sequential 100% reservations should succeed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ReserveSecondChildOverlappingByOneUnitFails) {
	// First child: [200, 5001), 100% capacity
	// Second child: [5000, 8000), 100% capacity — overlap during [5000, 5001)
	// Peak = 200% → must fail
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "first",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/first", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 5001,
			"deletion_time": 5500
		}
	})");

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "second",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/second", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 5000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "One-unit temporal overlap at 200% should fail";
}

TEST_F(StrictHierarchyTest, ReservePathTemporalOverlapBlocked) {
	// Two lots claiming the same path with overlapping time windows — must fail
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "pathA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "pathB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Same path with overlapping times should fail";
}

TEST_F(StrictHierarchyTest, ReservePathTemporalNoOverlapAllowed) {
	// Two lots claiming the same path with NON-overlapping time windows — should succeed
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "pathA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 4000,
			"deletion_time": 4500
		}
	})");

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "pathB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Same path with non-overlapping times should succeed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ReserveFailedCreationLeavesNoDB) {
	// A failed reservation must not leave partial DB state
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Create child taking full capacity
	addLot(R"({
		"lot_name": "existing",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ex", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	// Try to create overlapping child — will fail Axiom 2
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "failed_lot",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/fail", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Over-capacity creation should fail";

	// Verify the failed lot does not exist
	raw_err = nullptr;
	rv = lotman_lot_exists("failed_lot", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Failed lot should not exist in DB: " << (err.get() ? err.get() : "");
}

// --- lotman_update_lot: post-creation mutations ---

TEST_F(StrictHierarchyTest, ShrinkExpirationFreesCapacity) {
	// Shrink first child's expiration so second child no longer overlaps
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 6000,
			"deletion_time": 6500
		}
	})");

	// childB at [5000, 8000) with 60 ded — overlaps childA during [5000, 6000), peak = 120 > 100 → fail
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 5000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Should fail while childA still overlaps";

	// Shrink childA's expiration to 5000, eliminating overlap
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "childA", "management_policy_attrs": {"expiration_time": 5000}})", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Shrinking expiration should succeed: " << (err.get() ? err.get() : "");

	// Now childB should succeed
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 5000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "After shrinking childA, childB should fit: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ExtendExpirationCausesCapacityViolation) {
	// Two non-overlapping children, then extending the first to overlap the second
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 4000,
			"deletion_time": 4500
		}
	})");

	addLot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 4000,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Extend childA's expiration to 5000 — now overlaps childB during [4000, 5000)
	// Peak = 60+60 = 120 > 100
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "childA", "management_policy_attrs": {"expiration_time": 5000}})", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Extending expiration to cause overlap should fail";
}

TEST_F(StrictHierarchyTest, ShrinkDedicatedGBToFitNewChild) {
	// Shrink existing child's dedicated_GB to make room for a new reservation
	addDefaultLot();
	addRootLot(); // 100 ded
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "big",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/big", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 80,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// New child wants 30 during overlapping period — 80+30=110 > 100
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "small",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/small", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Should fail: 80+30=110 > 100";

	// Shrink "big" from 80 to 60
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "big", "management_policy_attrs": {"dedicated_GB": 60}})", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Shrinking dedicated_GB should succeed: " << (err.get() ? err.get() : "");

	// Now 60+30=90 ≤ 100 → should succeed
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "small",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/small", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 5,
			"max_num_objects": 50,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "After shrinking, new child should fit: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, GrowDedicatedGBBlockedByAxiom2) {
	// Growing a child's dedicated_GB so that siblings now exceed parent
	addDefaultLot();
	addRootLot(); // 100 ded
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	addLot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Try to grow childA to 60 → 60+50=110 > 100
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "childA", "management_policy_attrs": {"dedicated_GB": 60}})", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Growing dedicated_GB to exceed parent capacity should fail";
}

// --- lotman_update_lot with paths: path temporal overlap on update ---

TEST_F(StrictHierarchyTest, UpdatePathToOverlappingTimeFails) {
	// Lot A has path /root/data/x during [200, 5000).
	// Lot B has path /root/data/y during [200, 5000).
	// Changing lot B's path to /root/data/x should fail (temporal overlap).
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "lotA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/x", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	addLot(R"({
		"lot_name": "lotB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/y", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	raw_err = nullptr;
	rv = lotman_update_lot(
		R"({"lot_name": "lotB", "paths": [{"current": "/root/data/y", "new": "/root/data/x", "recursive": true}]})",
		&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Changing path to one with overlapping time should fail";
}

// --- lotman_add_to_lot: adding paths after creation ---

TEST_F(StrictHierarchyTest, AddPathOverlappingExistingFails) {
	// Lot A claims /root/data/shared during [200, 5000).
	// Adding /root/data/shared to lot B (also [200, 5000)) should fail.
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "lotA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	addLot(R"({
		"lot_name": "lotB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/other", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})");

	raw_err = nullptr;
	rv = lotman_add_to_lot(R"({"lot_name": "lotB", "paths": [{"path": "/root/data/shared", "recursive": true}]})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Adding path that overlaps another lot's time range should fail";
}

TEST_F(StrictHierarchyTest, AddPathNonOverlappingSucceeds) {
	// Lot A has /root/data/shared during [200, 3000).
	// Adding /root/data/shared to lot B [4000, 7000) should succeed.
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "lotA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 3000,
			"deletion_time": 3500
		}
	})");

	addLot(R"({
		"lot_name": "lotB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/other", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 4000,
			"expiration_time": 7000,
			"deletion_time": 7500
		}
	})");

	raw_err = nullptr;
	rv = lotman_add_to_lot(R"({"lot_name": "lotB", "paths": [{"path": "/root/data/shared", "recursive": true}]})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Adding path with non-overlapping times should succeed: " << (err.get() ? err.get() : "");
}

// --- lotman_update_lot with parent_attributions: re-attribution mutations ---

TEST_F(StrictHierarchyTest, UpdateAttributionsCausesAxiom2Violation) {
	// Two children under root, each attributed 50% of parent's 100 ded.
	// Re-attributing one child to take 60% causes peak = 60+50 = 110 > 100.
	addDefaultLot();
	addRootLot(); // 100 ded
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// childA: 50 ded during [200, 8000) — attributed as 50/100 = 50%
	addLot(R"({
		"lot_name": "childA",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/ca", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// childB: 50 ded during [200, 8000) — attributed as 50/100 = 50%
	addLot(R"({
		"lot_name": "childB",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/cb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Now grow childA's MPAs to 60 ded — Axiom 1 passes (60 ≤ 100), but Axiom 2 peak = 60+50 = 110 > 100
	raw_err = nullptr;
	rv = lotman_update_lot(R"({"lot_name": "childA", "management_policy_attrs": {"dedicated_GB": 60}})", &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Growing sibling capacity to exceed parent should fail via Axiom 2";
}

// --- lotman_remove_lot / lotman_remove_lots_recursive: releasing reservations ---

TEST_F(StrictHierarchyTest, DeleteLotFreesCapacityForNew) {
	// Create child consuming 80/100 ded. Delete it. Create new child with full 100.
	addDefaultLot();
	addRootLot();
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "to_delete",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/del", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 80,
			"opportunistic_GB": 40,
			"max_num_objects": 800,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Can't create full child while to_delete exists
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "full_replacement",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/rep", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Should fail while to_delete still holds capacity";

	// Delete the old lot
	raw_err = nullptr;
	rv = lotman_remove_lots_recursive("to_delete", &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Delete should succeed: " << (err.get() ? err.get() : "");

	// Now the full replacement should succeed
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "full_replacement",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/rep", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "After deletion, full replacement should succeed: " << (err.get() ? err.get() : "");
}

// --- Strict hierarchy on/off: reservations only enforced when strict_hierarchy=true ---

TEST_F(StrictHierarchyTest, ReservationNotEnforcedWhenStrictOff) {
	// With strict_hierarchy=false, over-subscription is allowed
	addDefaultLot();
	addRootLot(); // 100 ded
	// strict_hierarchy defaults to false in setUp

	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "over",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/over", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 200,
			"opportunistic_GB": 100,
			"max_num_objects": 2000,
			"creation_time": 200,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Over-subscription should be allowed when strict_hierarchy is off: "
					 << (err.get() ? err.get() : "");
}

// --- Multi-parent reservation: child attributed across multiple parents ---

TEST_F(StrictHierarchyTest, MultiParentReservationFitsEachParent) {
	// Child has two parents. Attribution is split evenly.
	// Each parent must independently satisfy its share.
	addDefaultLot();
	addRootLot(); // root: 100 ded

	// Second parent
	addLot(R"({
		"lot_name": "root2",
		"owner": "owner1",
		"parents": ["root2"],
		"paths": [{"path": "/root2/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child with 120 ded, two parents → each gets 60 attributed (within each parent's 100)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "dual_parent_child",
		"owner": "owner1",
		"parents": ["root", "root2"],
		"paths": [{"path": "/root/data/dual", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 120,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Child with 120 ded split across 2 parents (60 each ≤ 100) should succeed: "
					 << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, MultiParentReservationExceedsOneParent) {
	// Child has two parents but one has limited capacity → Axiom 1 or 2 should catch it
	addDefaultLot();

	// Big parent
	addLot(R"({
		"lot_name": "big_parent",
		"owner": "owner1",
		"parents": ["big_parent"],
		"paths": [{"path": "/big/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 200,
			"opportunistic_GB": 100,
			"max_num_objects": 2000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Small parent (only 10 ded total)
	addLot(R"({
		"lot_name": "small_parent",
		"owner": "owner1",
		"parents": ["small_parent"],
		"paths": [{"path": "/small/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child with 100 ded → equal split = 50 per parent. small_parent(10) < 50 → Axiom 1 or 2 violation
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "too_big",
		"owner": "owner1",
		"parents": ["big_parent", "small_parent"],
		"paths": [{"path": "/big/data/tb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child exceeding one of its parents should fail";
}

// --- Timestamp boundary edge cases ---

TEST_F(StrictHierarchyTest, ChildExpirationEqualsParentExpiration) {
	// Child's expiration exactly equals parent's expiration — Axiom 3 should pass
	addDefaultLot();
	addRootLot(); // expiration 9000
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "exact_exp",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/exact", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Child expiration == parent expiration should be allowed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ChildExpirationExceedsParentFails) {
	// Child's expiration exceeds parent's expiration — Axiom 3 violation
	addDefaultLot();
	addRootLot(); // expiration 9000
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "late_exp",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/late", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 9001,
			"deletion_time": 9500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child expiration exceeding parent should fail (Axiom 3)";
}

TEST_F(StrictHierarchyTest, ChildCreationBeforeParentFails) {
	// Child's creation_time is before parent's creation_time — Axiom 3 violation
	addDefaultLot();
	addRootLot(); // creation 100
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "early_child",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/early", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 10,
			"max_num_objects": 100,
			"creation_time": 50,
			"expiration_time": 5000,
			"deletion_time": 5500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Child creation before parent should fail (Axiom 3)";
}

// --- Three-level hierarchy: grandchild reservation enforcement ---

TEST_F(StrictHierarchyTest, GrandchildCapacityConstrainedByParentNotGrandparent) {
	// Root (100 ded) -> middle (50 ded) -> grandchild
	// Grandchild's capacity is bounded by middle (50), not root (100)
	addDefaultLot();
	addRootLot(); // 100 ded
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	addLot(R"({
		"lot_name": "middle",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/mid", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Grandchild asking for 60 ded — within root's 100 but exceeds middle's 50
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "grandchild",
		"owner": "owner1",
		"parents": ["middle"],
		"paths": [{"path": "/root/data/mid/gc", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 300,
			"expiration_time": 7000,
			"deletion_time": 7500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Grandchild exceeding parent middle should fail (Axiom 1)";

	// Grandchild asking for 40 ded — within middle's 50
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "grandchild",
		"owner": "owner1",
		"parents": ["middle"],
		"paths": [{"path": "/root/data/mid/gc", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 300,
			"expiration_time": 7000,
			"deletion_time": 7500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Grandchild within parent middle should succeed: " << (err.get() ? err.get() : "");
}

// ============================================================================
// Bug regression tests
// ============================================================================

// Bug 1: lotman_rm_paths_from_lots used LIMIT 1, so if the same path appeared
// in multiple lots (with non-overlapping time ranges) only one lot's path entry
// was removed.  After the fix, all lots sharing that path are cleaned up.
TEST_F(StrictHierarchyTest, RmPathsRemovesFromAllLotsWithSharedPath) {
	addDefaultLot();

	char *raw_err = nullptr;
	int rv;

	// Enable strict hierarchy so temporal overlap enforcement is active
	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Root lot that spans a wide time range
	addLot(R"({
		"lot_name": "root",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/shared", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 200,
			"opportunistic_GB": 100,
			"max_num_objects": 5000,
			"creation_time": 100,
			"expiration_time": 90000,
			"deletion_time": 99000
		}
	})");

	// lot_a owns /shared/data during [1000, 2000)
	addLot(R"({
		"lot_name": "lot_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/shared/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 1000,
			"expiration_time": 2000,
			"deletion_time": 3000
		}
	})");

	// lot_b owns /shared/data during [3000, 4000) — non-overlapping
	addLot(R"({
		"lot_name": "lot_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/shared/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 3000,
			"expiration_time": 4000,
			"deletion_time": 5000
		}
	})");

	// Remove the shared path
	raw_err = nullptr;
	rv = lotman_rm_paths_from_lots(R"({"paths": ["/shared/data"]})", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "rm_paths failed: " << (err.get() ? err.get() : "");

	// Verify lot_a no longer has the path
	char *dirs_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs("lot_a", false, &dirs_output, &raw_err);
	UniqueCString dirs_err(raw_err);
	UniqueCString dirs_str(dirs_output);
	ASSERT_EQ(rv, 0) << (dirs_err.get() ? dirs_err.get() : "");
	json lot_a_dirs = json::parse(dirs_str.get());
	EXPECT_TRUE(lot_a_dirs.empty()) << "lot_a should have no paths after removal, got: " << lot_a_dirs.dump();

	// Verify lot_b no longer has the path either
	dirs_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs("lot_b", false, &dirs_output, &raw_err);
	dirs_err.reset(raw_err);
	dirs_str.reset(dirs_output);
	ASSERT_EQ(rv, 0) << (dirs_err.get() ? dirs_err.get() : "");
	json lot_b_dirs = json::parse(dirs_str.get());
	EXPECT_TRUE(lot_b_dirs.empty()) << "lot_b should have no paths after removal, got: " << lot_b_dirs.dump();
}

// Bug 2: When store_lot inserts a lot between an existing parent-child pair
// (insertion adjustment), the child's attributions were not recomputed.  This
// could allow a sibling to over-allocate from the newly-inserted parent because
// Axiom 2 did not count the existing child's share.
TEST_F(StrictHierarchyTest, InsertionAdjustmentRecomputesChildAttributions) {
	addDefaultLot();

	char *raw_err = nullptr;
	int rv;

	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Root with 100 ded
	addLot(R"({
		"lot_name": "root",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// child with 30 ded, parent = root
	addLot(R"({
		"lot_name": "child",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 10,
			"max_num_objects": 200,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Insert "middle" between root and child.  middle has 50 ded.
	// After insertion, child's parent becomes middle (not root).
	// child's 30 ded should be counted against middle's 50.
	addLot(R"({
		"lot_name": "middle",
		"owner": "owner1",
		"parents": ["root"],
		"children": ["child"],
		"paths": [{"path": "/root/data/mid", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 150,
			"expiration_time": 8500,
			"deletion_time": 9000
		}
	})");

	// Verify child's parent is now middle
	char **parent_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_parent_names("child", false, false, &parent_output, &raw_err);
	err.reset(raw_err);
	UniqueStringList parents(parent_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(parents.get(), nullptr);
	EXPECT_STREQ(parents.get()[0], "middle");

	// Now try to add child2 under middle with 30 ded.
	// middle has 50 total; child already uses 30 → only 20 remains.
	// child2 asking for 30 should FAIL if attributions were properly recomputed.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child2",
		"owner": "owner1",
		"parents": ["middle"],
		"paths": [{"path": "/root/data/mid/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "child2 should fail: child already uses 30 of middle's 50 ded";

	// But asking for 20 should succeed (exactly fills the remaining capacity)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child2",
		"owner": "owner1",
		"parents": ["middle"],
		"paths": [{"path": "/root/data/mid/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "child2 with 20 ded should fit: " << (err.get() ? err.get() : "");
}

// Bug 3: update_parents did not call reload_and_recompute_attributions, unlike
// add_parents and remove_parents.  After swapping a child's parent, Axiom 2
// accounting on the new parent was stale.
TEST_F(StrictHierarchyTest, UpdateParentsRecomputesAttributions) {
	addDefaultLot();

	char *raw_err = nullptr;
	int rv;

	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Root with 200 ded
	addLot(R"({
		"lot_name": "root",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 200,
			"opportunistic_GB": 100,
			"max_num_objects": 5000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// parent_a with 50 ded
	addLot(R"({
		"lot_name": "parent_a",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 20,
			"max_num_objects": 500,
			"creation_time": 200,
			"expiration_time": 8500,
			"deletion_time": 9000
		}
	})");

	// parent_b with 40 ded
	addLot(R"({
		"lot_name": "parent_b",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/root/data/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 40,
			"opportunistic_GB": 15,
			"max_num_objects": 400,
			"creation_time": 200,
			"expiration_time": 8500,
			"deletion_time": 9000
		}
	})");

	// child with 25 ded under parent_a
	addLot(R"({
		"lot_name": "child",
		"owner": "owner1",
		"parents": ["parent_a"],
		"paths": [{"path": "/root/data/a/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 25,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 300,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	// Move child from parent_a to parent_b via update_lot
	raw_err = nullptr;
	rv = lotman_update_lot(R"({
		"lot_name": "child",
		"parents": [{"current": "parent_a", "new": "parent_b"}]
	})",
						   &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "update_parents failed: " << (err.get() ? err.get() : "");

	// Now child uses 25 of parent_b's 40, leaving only 15.
	// Adding child2 under parent_b with 20 should fail.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child2",
		"owner": "owner1",
		"parents": ["parent_b"],
		"paths": [{"path": "/root/data/b/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 300,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "child2 with 20 ded should fail: child already uses 25 of parent_b's 40";

	// But 15 should succeed (exactly fills remaining)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "child2",
		"owner": "owner1",
		"parents": ["parent_b"],
		"paths": [{"path": "/root/data/b/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 15,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 300,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "child2 with 15 ded should fit: " << (err.get() ? err.get() : "");
}

// Bug 4/5: Temporal queries used closed intervals [creation, expiration] instead
// of half-open [creation, expiration).  This test verifies half-open semantics:
// a lot is NOT found at its exact expiration_time, and an adjacent lot starting
// at that same time IS found.
TEST_F(StrictHierarchyTest, HalfOpenIntervalGetLotsFromDir) {
	addDefaultLot();

	char *raw_err = nullptr;
	int rv;

	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Root spanning a wide range
	addLot(R"({
		"lot_name": "root",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/temporal", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 200,
			"opportunistic_GB": 100,
			"max_num_objects": 5000,
			"creation_time": 100,
			"expiration_time": 90000,
			"deletion_time": 99000
		}
	})");

	// lot_early: owns /temporal/data during [1000, 2000)
	addLot(R"({
		"lot_name": "lot_early",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/temporal/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 1000,
			"expiration_time": 2000,
			"deletion_time": 3000
		}
	})");

	// lot_late: owns /temporal/data during [2000, 3000) — immediately follows lot_early
	addLot(R"({
		"lot_name": "lot_late",
		"owner": "owner1",
		"parents": ["root"],
		"paths": [{"path": "/temporal/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": 500,
			"creation_time": 2000,
			"expiration_time": 3000,
			"deletion_time": 4000
		}
	})");

	// Helper lambda to query which lot owns /temporal/data at a given time
	auto query_lot = [&](int64_t t) -> std::string {
		char **lots_output = nullptr;
		char *qerr = nullptr;
		int qrv = lotman_get_lots_from_dir("/temporal/data", false, t, &lots_output, &qerr);
		UniqueCString qerr_str(qerr);
		UniqueStringList lots(lots_output);
		if (qrv != 0 || !lots.get() || !lots.get()[0])
			return "";
		return std::string(lots.get()[0]);
	};

	// Before lot_early's creation: should fall through to default or root
	EXPECT_NE(query_lot(999), "lot_early") << "Before creation_time, lot_early should not own the path";

	// At lot_early's creation_time (inclusive start): lot_early should own it
	EXPECT_EQ(query_lot(1000), "lot_early") << "At creation_time, lot_early should own the path";

	// Mid-interval: lot_early
	EXPECT_EQ(query_lot(1500), "lot_early");

	// One tick before lot_early expires: still lot_early
	EXPECT_EQ(query_lot(1999), "lot_early");

	// At lot_early's expiration_time (half-open: excluded for lot_early, included for lot_late)
	EXPECT_NE(query_lot(2000), "lot_early") << "At expiration_time, lot_early should no longer own the path";
	EXPECT_EQ(query_lot(2000), "lot_late") << "At lot_late's creation_time, lot_late should own the path";

	// Mid-interval lot_late
	EXPECT_EQ(query_lot(2500), "lot_late");

	// One tick before lot_late expires
	EXPECT_EQ(query_lot(2999), "lot_late");

	// At lot_late's expiration: neither should own it
	EXPECT_NE(query_lot(3000), "lot_late") << "At expiration_time, lot_late should no longer own the path";
}

// Verify get_lot_from_dir (the internal single-lot lookup) also uses half-open
// semantics by testing with lotman_update_lot_usage_by_dir at boundary times.
TEST_F(StrictHierarchyTest, HalfOpenIntervalGetLotFromDir) {
	addDefaultLot();

	char *raw_err = nullptr;
	int rv;

	// A simple lot with known time range [1000, 2000)
	addLot(R"({
		"lot_name": "timed_lot",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/timed/path", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1000,
			"expiration_time": 2000,
			"deletion_time": 3000
		}
	})");

	// Query at 1999 (inside) — should find timed_lot
	char **lots_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_from_dir("/timed/path", false, 1999, &lots_output, &raw_err);
	UniqueCString err(raw_err);
	UniqueStringList lots(lots_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots.get(), nullptr);
	EXPECT_STREQ(lots.get()[0], "timed_lot");

	// Query at 2000 (expiration, excluded by half-open) — should NOT find timed_lot
	lots_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_from_dir("/timed/path", false, 2000, &lots_output, &raw_err);
	err.reset(raw_err);
	lots.reset(lots_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	// Should fall through to default
	ASSERT_NE(lots.get(), nullptr);
	EXPECT_STREQ(lots.get()[0], "default") << "At expiration_time, path should fall through to default";

	// Query at 1000 (creation, included) — should find timed_lot
	lots_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_from_dir("/timed/path", false, 1000, &lots_output, &raw_err);
	err.reset(raw_err);
	lots.reset(lots_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots.get(), nullptr);
	EXPECT_STREQ(lots.get()[0], "timed_lot");

	// Query at 999 (before creation, excluded) — should NOT find timed_lot
	lots_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_from_dir("/timed/path", false, 999, &lots_output, &raw_err);
	err.reset(raw_err);
	lots.reset(lots_output);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	ASSERT_NE(lots.get(), nullptr);
	EXPECT_STREQ(lots.get()[0], "default") << "Before creation_time, path should fall through to default";
}

// --- Multi-parent overlap stress & edge cases (3+ parents) ---

TEST_F(StrictHierarchyTest, ThreeParentChildSweepLineOverlap) {
	// Three root lots as parents, each with 50 ded.
	// A child with 90 ded split equally across 3 parents (30 each ≤ 50) should succeed.
	// A sibling from one of those parents with 25 ded pushes that parent to 55 > 50 → fail.
	addDefaultLot();

	addLot(R"({"lot_name": "pA", "owner": "owner1", "parents": ["pA"],
		"paths": [{"path": "/pA/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");
	addLot(R"({"lot_name": "pB", "owner": "owner1", "parents": ["pB"],
		"paths": [{"path": "/pB/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");
	addLot(R"({"lot_name": "pC", "owner": "owner1", "parents": ["pC"],
		"paths": [{"path": "/pC/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Joint child under all three parents — 90 ded / 3 = 30 each ≤ 50
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "tri_child", "owner": "owner1",
		"parents": ["pA", "pB", "pC"],
		"paths": [{"path": "/pA/data/tri", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 90, "opportunistic_GB": 0, "max_num_objects": 60,
			"creation_time": 200, "expiration_time": 8000, "deletion_time": 8500}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "90 ded split across 3 parents (30 each ≤ 50) should succeed: " << (err.get() ? err.get() : "");

	// Now add a sibling under pA with 25 ded → pA concurrent load = 30 + 25 = 55 > 50
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "pA_sibling", "owner": "owner1",
		"parents": ["pA"],
		"paths": [{"path": "/pA/data/sib", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 25, "opportunistic_GB": 0, "max_num_objects": 10,
			"creation_time": 200, "expiration_time": 8000, "deletion_time": 8500}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Sibling that tips pA over capacity should be rejected by Axiom 2";
}

TEST_F(StrictHierarchyTest, ThreeParentTemporalStaggerAvoidsSweepConflict) {
	// Three parents, child under all three. A sibling under pA does NOT overlap
	// with the child in time → sweep-line should allow it even though the sum of
	// raw allocations would exceed pA if concurrent.
	addDefaultLot();

	addLot(R"({"lot_name": "pA", "owner": "owner1", "parents": ["pA"],
		"paths": [{"path": "/pA/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");
	addLot(R"({"lot_name": "pB", "owner": "owner1", "parents": ["pB"],
		"paths": [{"path": "/pB/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");
	addLot(R"({"lot_name": "pC", "owner": "owner1", "parents": ["pC"],
		"paths": [{"path": "/pC/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child under all three, lives [200, 4000)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "early_child", "owner": "owner1",
		"parents": ["pA", "pB", "pC"],
		"paths": [{"path": "/pA/data/early", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 90, "opportunistic_GB": 0, "max_num_objects": 60,
			"creation_time": 200, "expiration_time": 4000, "deletion_time": 4500}})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	// Sibling under pA only, lives [5000, 8000) — no temporal overlap with early_child
	// So pA's peak concurrent load is max(30, 45) = 45 ≤ 50
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "late_sib", "owner": "owner1",
		"parents": ["pA"],
		"paths": [{"path": "/pA/data/late", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 45, "opportunistic_GB": 0, "max_num_objects": 80,
			"creation_time": 5000, "expiration_time": 8000, "deletion_time": 8500}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Non-overlapping sibling should succeed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, ThreeParentObjectCapacity) {
	// Three parents with different object capacities.
	// A child attributed across all three must satisfy the smallest parent.
	addDefaultLot();

	addLot(R"({"lot_name": "pX", "owner": "owner1", "parents": ["pX"],
		"paths": [{"path": "/pX/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 0, "max_num_objects": 30,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");
	addLot(R"({"lot_name": "pY", "owner": "owner1", "parents": ["pY"],
		"paths": [{"path": "/pY/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 0, "max_num_objects": 30,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");
	addLot(R"({"lot_name": "pZ", "owner": "owner1", "parents": ["pZ"],
		"paths": [{"path": "/pZ/data", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 0, "max_num_objects": 30,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Child with 90 objects / 3 parents = 30 each. 30 ≤ 30 → should succeed
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "obj_child", "owner": "owner1",
		"parents": ["pX", "pY", "pZ"],
		"paths": [{"path": "/pX/data/oc", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 0, "max_num_objects": 90,
			"creation_time": 200, "expiration_time": 8000, "deletion_time": 8500}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "90 objects across 3 parents (30 each = 30 limit) should succeed: "
					 << (err.get() ? err.get() : "");

	// Another child under pX with 1 object → pX now has 30 + 1 = 31 > 30
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "extra_obj", "owner": "owner1",
		"parents": ["pX"],
		"paths": [{"path": "/pX/data/xo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1,
			"creation_time": 200, "expiration_time": 8000, "deletion_time": 8500}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "One extra object should push pX past its 30-object limit";
}

// ---------------------------------------------------------------------------
// Tests for attribution input-validation fixes
// ---------------------------------------------------------------------------

// Fix 1: When every non-self parent is explicitly attributed for an MPA key
// but the sum falls short of the child's total, we must get an error rather
// than silently dropping the remainder.
TEST_F(StrictHierarchyTest, ExplicitAttributionShortfallRejected) {
	addDefaultLot();
	char *raw_err = nullptr;

	// Two parents with plenty of capacity
	int rv = lotman_add_lot(R"({"lot_name": "shortfall_p1", "owner": "owner1",
		"parents": ["shortfall_p1"],
		"paths": [{"path": "/shortfall/p1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 500,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
							&raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "shortfall_p1 creation failed: " << (err.get() ? err.get() : "");

	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "shortfall_p2", "owner": "owner1",
		"parents": ["shortfall_p2"],
		"paths": [{"path": "/shortfall/p2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 500,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "shortfall_p2 creation failed: " << (err.get() ? err.get() : "");

	raw_err = nullptr;
	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0);

	// Child with 20 dedicated_GB, but explicitly attribute only 5+5 = 10 to the
	// two parents. Since every parent is explicit for dedicated_GB, the missing
	// 10 GB cannot be distributed and must be flagged as an error.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "shortfall_child", "owner": "owner1",
		"parents": ["shortfall_p1", "shortfall_p2"],
		"paths": [{"path": "/shortfall/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20, "opportunistic_GB": 10, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500},
		"parent_attributions": {
			"shortfall_p1": {"dedicated_GB": 5, "opportunistic_GB": 5, "max_num_objects": 50},
			"shortfall_p2": {"dedicated_GB": 5, "opportunistic_GB": 5, "max_num_objects": 50}
		}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Should reject explicit attributions that sum to less than child total";
	if (err.get()) {
		EXPECT_TRUE(std::string(err.get()).find("remainder") != std::string::npos ||
					std::string(err.get()).find("sum") != std::string::npos)
			<< "Error message should mention the shortfall: " << err.get();
	}

	// Same via lotman_update_lot with parent_attributions: create child with equal split first
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "shortfall_child2", "owner": "owner1",
		"parents": ["shortfall_p1", "shortfall_p2"],
		"paths": [{"path": "/shortfall/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20, "opportunistic_GB": 10, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "shortfall_child2 creation failed: " << (err.get() ? err.get() : "");

	raw_err = nullptr;
	rv = lotman_update_lot(
		R"({"lot_name": "shortfall_child2",
			"parent_attributions": {
				"shortfall_p1": {"dedicated_GB": 5, "opportunistic_GB": 5, "max_num_objects": 50},
				"shortfall_p2": {"dedicated_GB": 5, "opportunistic_GB": 5, "max_num_objects": 50}
			}})",
		&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "update_lot with parent_attributions should also reject shortfall";
}

// Fix 2: Attribution JSON keys that don't match any actual parent must be
// rejected rather than silently ignored (catches typos).
TEST_F(StrictHierarchyTest, UnknownParentKeyInAttributionRejected) {
	addDefaultLot();
	char *raw_err = nullptr;

	// Create a parent
	int rv = lotman_add_lot(R"({"lot_name": "typo_parent", "owner": "owner1",
		"parents": ["typo_parent"],
		"paths": [{"path": "/typo/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 500,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
							&raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "typo_parent creation failed: " << (err.get() ? err.get() : "");

	raw_err = nullptr;
	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0);

	// Create child with a typo in the attribution key ("typo_praent" instead
	// of "typo_parent"). Previously this was silently ignored and all allocation
	// would go to equal-split. Now it must error.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "typo_child", "owner": "owner1",
		"parents": ["typo_parent"],
		"paths": [{"path": "/typo/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20, "opportunistic_GB": 10, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500},
		"parent_attributions": {
			"typo_praent": {"dedicated_GB": 20, "opportunistic_GB": 10, "max_num_objects": 100}
		}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Should reject attribution JSON with unknown parent key";
	if (err.get()) {
		EXPECT_TRUE(std::string(err.get()).find("unknown parent") != std::string::npos ||
					std::string(err.get()).find("typo_praent") != std::string::npos)
			<< "Error should mention the bad key: " << err.get();
	}

	// Same via lotman_update_lot with parent_attributions
	// First create the child normally (equal split)
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "typo_child2", "owner": "owner1",
		"parents": ["typo_parent"],
		"paths": [{"path": "/typo/child2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 20, "opportunistic_GB": 10, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "typo_child2 creation failed: " << (err.get() ? err.get() : "");

	raw_err = nullptr;
	rv = lotman_update_lot(
		R"({"lot_name": "typo_child2",
			"parent_attributions": {"typo_praent": {"dedicated_GB": 20, "opportunistic_GB": 10, "max_num_objects": 100}}})",
		&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "update_lot with parent_attributions should reject unknown parent key";
}

// Fixes 3 & 4: When attribution rows are missing for a parent (e.g. DB
// corruption or incomplete migration), validate_axiom1 and the sweep-line
// (build_attribution_events) must detect and report the problem rather than
// silently treating the attributed usage as 0.
TEST_F(StrictHierarchyTest, MissingAttributionRowsDetected) {
	addDefaultLot();
	char *raw_err = nullptr;

	// Parent with modest capacity
	int rv = lotman_add_lot(R"({"lot_name": "missing_attr_p", "owner": "owner1",
		"parents": ["missing_attr_p"],
		"paths": [{"path": "/missattr/p", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 20, "max_num_objects": 200,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
							&raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << "missing_attr_p creation failed: " << (err.get() ? err.get() : "");

	raw_err = nullptr;
	rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0);

	// Child that uses up some capacity
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "missing_attr_c", "owner": "owner1",
		"parents": ["missing_attr_p"],
		"paths": [{"path": "/missattr/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 30, "opportunistic_GB": 10, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "missing_attr_c creation failed: " << (err.get() ? err.get() : "");

	// Direct DB surgery: delete all attribution rows for this child
	{
		using namespace sqlite_orm;
		auto &storage = lotman::db::StorageManager::get_storage();
		storage.remove_all<lotman::db::ParentChildAttribution>(
			where(c(&lotman::db::ParentChildAttribution::child_lot_name) == "missing_attr_c"));

		// Verify they're gone
		auto remaining = storage.count<lotman::db::ParentChildAttribution>(
			where(c(&lotman::db::ParentChildAttribution::child_lot_name) == "missing_attr_c"));
		ASSERT_EQ(remaining, 0) << "Attribution rows should be deleted";
	}

	// Now try to add a sibling — this triggers validate_axiom2_for_parents_of
	// which calls build_attribution_events for missing_attr_p. With strict
	// hierarchy enabled, the missing attribution rows should be detected.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({"lot_name": "missing_attr_c2", "owner": "owner1",
		"parents": ["missing_attr_p"],
		"paths": [{"path": "/missattr/c2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 50,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500}})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Adding a sibling should fail because attribution rows are missing for missing_attr_c";
	if (err.get()) {
		EXPECT_TRUE(std::string(err.get()).find("attribution") != std::string::npos ||
					std::string(err.get()).find("Missing") != std::string::npos)
			<< "Error should mention missing attributions: " << err.get();
	}
}

// ============================================================================
// Rollback verification: failed axiom validation must leave DB unchanged
// ============================================================================

TEST_F(StrictHierarchyTest, FailedAxiomValidationRollsBackMpaUpdate) {
	// Verifies that when an MPA update would violate the parent-capacity invariant,
	// the in-transaction rollback leaves the lot's stored MPA unchanged. Without
	// proper rollback, a partial write would leave the lot DB in an invalid state.
	addDefaultLot();
	char *raw_err = nullptr;

	// Parent with 100 GB dedicated.
	addLot(R"({
		"lot_name": "rb_parent", "owner": "owner1", "parents": ["rb_parent"],
		"paths": [{"path": "/rb/parent", "recursive": true}],
		"management_policy_attrs": {"dedicated_GB": 100, "opportunistic_GB": 0,
			"max_num_objects": 1000, "creation_time": 100,
			"expiration_time": 9000, "deletion_time": 9500}})");

	// Child fully attributed to parent at 80 GB dedicated.
	addLot(R"({
		"lot_name": "rb_child", "owner": "owner1", "parents": ["rb_parent"],
		"paths": [{"path": "/rb/child", "recursive": true}],
		"management_policy_attrs": {"dedicated_GB": 80, "opportunistic_GB": 0,
			"max_num_objects": 100, "creation_time": 200,
			"expiration_time": 8000, "deletion_time": 8500}})");

	// Enable strict hierarchy and try to shrink parent below child's allocation.
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_update_lot(R"({"lot_name": "rb_parent",
		"management_policy_attrs": {"dedicated_GB": 50}})",
						   &raw_err);
	err.reset(raw_err);
	ASSERT_NE(rv, 0) << "Shrinking parent below child's allocation must fail under strict hierarchy";

	// Verify rollback: parent's stored dedicated_GB must still be 100.
	raw_err = nullptr;
	char *parent_out = nullptr;
	rv = lotman_get_lot_as_json("rb_parent", false, &parent_out, &raw_err);
	UniqueCString parent_str(parent_out);
	UniqueCString err_p(raw_err);
	ASSERT_EQ(rv, 0) << (err_p.get() ? err_p.get() : "");
	ASSERT_NE(parent_str.get(), nullptr);
	auto parent_json = json::parse(parent_str.get());
	EXPECT_DOUBLE_EQ(parent_json["management_policy_attrs"]["dedicated_GB"].get<double>(), 100.0)
		<< "Parent MPA must be unchanged after rolled-back update; got: " << parent_str.get();

	// Verify the child is also untouched and the attribution still holds.
	raw_err = nullptr;
	char *child_out = nullptr;
	rv = lotman_get_lot_as_json("rb_child", false, &child_out, &raw_err);
	UniqueCString child_str(child_out);
	UniqueCString err_c(raw_err);
	ASSERT_EQ(rv, 0) << (err_c.get() ? err_c.get() : "");
	ASSERT_NE(child_str.get(), nullptr);
	auto child_json = json::parse(child_str.get());
	EXPECT_DOUBLE_EQ(child_json["management_policy_attrs"]["dedicated_GB"].get<double>(), 80.0)
		<< "Child MPA must be unchanged after rolled-back update; got: " << child_str.get();
}

TEST_F(StrictHierarchyTest, ContractionPolicyToleratesFloatingPointNoise) {
	// Re-applying a lot's current MPA value (e.g. via a JSON round-trip that
	// introduces sub-epsilon floating-point noise) must not be misinterpreted as
	// a contraction when contraction_policy='always'. Without an epsilon the
	// check `update_val < mpa.dedicated_GB` could spuriously fire.
	addDefaultLot();
	addLot(R"({
		"lot_name": "fp_lot", "owner": "owner1", "parents": ["fp_lot"],
		"paths": [{"path": "/fp/lot", "recursive": true}],
		"management_policy_attrs": {"dedicated_GB": 10, "opportunistic_GB": 5,
			"max_num_objects": 100, "creation_time": 100,
			"expiration_time": 9000, "deletion_time": 9500}})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("contraction_policy", "always", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Sub-epsilon decrease (well below 1e-9) — should be treated as a no-op,
	// not as a contraction, and therefore must succeed.
	rv = lotman_update_lot(R"({"lot_name": "fp_lot",
		"management_policy_attrs": {"dedicated_GB": 9.9999999999999}})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Sub-epsilon MPA noise must not trip the contraction policy. Got: "
					 << (err.get() ? err.get() : "");

	// A real contraction (well above the epsilon) must still be blocked.
	rv = lotman_update_lot(R"({"lot_name": "fp_lot",
		"management_policy_attrs": {"dedicated_GB": 9.5}})",
						   &raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "A genuine contraction must still be blocked by contraction_policy='always'";
}

TEST_F(StrictHierarchyTest, UpdateLotIsAtomicAcrossSubOperations) {
	// Verifies that lotman_update_lot wraps all of its sub-operations
	// (owner, paths, management_policy_attrs, ...) in a single transaction:
	// when a late sub-operation fails (here, an MPA update that violates a
	// strict-hierarchy axiom), every earlier sub-operation in the same
	// envelope must also be rolled back.
	addDefaultLot();
	char *raw_err = nullptr;

	addLot(R"({
		"lot_name": "atomic_parent", "owner": "owner1", "parents": ["atomic_parent"],
		"paths": [{"path": "/atomic/parent", "recursive": true}],
		"management_policy_attrs": {"dedicated_GB": 100, "opportunistic_GB": 0,
			"max_num_objects": 1000, "creation_time": 100,
			"expiration_time": 9000, "deletion_time": 9500}})");

	addLot(R"({
		"lot_name": "atomic_child", "owner": "owner1", "parents": ["atomic_parent"],
		"paths": [{"path": "/atomic/child", "recursive": true}],
		"management_policy_attrs": {"dedicated_GB": 80, "opportunistic_GB": 0,
			"max_num_objects": 100, "creation_time": 200,
			"expiration_time": 8000, "deletion_time": 8500}})");

	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Single envelope: change the owner (would succeed on its own), then
	// attempt to shrink dedicated_GB below the child's allocation (must
	// fail under strict hierarchy). The owner change must be rolled back.
	rv = lotman_update_lot(R"({
		"lot_name": "atomic_parent",
		"owner": "owner_should_be_rolled_back",
		"management_policy_attrs": {"dedicated_GB": 50}
	})",
						   &raw_err);
	err.reset(raw_err);
	ASSERT_NE(rv, 0) << "Envelope must fail when a sub-operation fails";

	// Verify owner was rolled back.
	char **raw_owners = nullptr;
	raw_err = nullptr;
	rv = lotman_get_owners("atomic_parent", false, &raw_owners, &raw_err);
	UniqueCString owners_err(raw_err);
	UniqueStringList owners(raw_owners);
	ASSERT_EQ(rv, 0) << (owners_err.get() ? owners_err.get() : "");
	bool found_original = false;
	bool found_pending = false;
	for (int i = 0; owners.get()[i]; ++i) {
		if (std::string(owners.get()[i]) == "owner1")
			found_original = true;
		if (std::string(owners.get()[i]) == "owner_should_be_rolled_back")
			found_pending = true;
	}
	EXPECT_TRUE(found_original) << "Original owner must remain after rolled-back envelope";
	EXPECT_FALSE(found_pending) << "Owner write must be rolled back when a later sub-op fails";

	// And verify the MPA itself is also unchanged (sanity).
	char *parent_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_as_json("atomic_parent", false, &parent_out, &raw_err);
	UniqueCString parent_str(parent_out);
	UniqueCString err_p(raw_err);
	ASSERT_EQ(rv, 0) << (err_p.get() ? err_p.get() : "");
	auto parent_json = json::parse(parent_str.get());
	EXPECT_DOUBLE_EQ(parent_json["management_policy_attrs"]["dedicated_GB"].get<double>(), 100.0);
}

// ============================================================================
// Non-expiring lot sentinel: a creation/expiration/deletion triple of all
// zeros marks a lot as never-expiring. The sentinel is all-or-nothing.
// ============================================================================

namespace {
// Helper: read the timestamps for a stored lot via lotman_get_lot_as_json.
struct LotTimes {
	int64_t creation_time;
	int64_t expiration_time;
	int64_t deletion_time;
};

LotTimes get_lot_times(const std::string &lot_name) {
	char *raw_out = nullptr;
	char *raw_err = nullptr;
	int rv = lotman_get_lot_as_json(lot_name.c_str(), false, &raw_out, &raw_err);
	UniqueCString out(raw_out);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << (err.get() ? err.get() : "");
	auto j = json::parse(out.get());
	return LotTimes{j["management_policy_attrs"]["creation_time"].get<int64_t>(),
					j["management_policy_attrs"]["expiration_time"].get<int64_t>(),
					j["management_policy_attrs"]["deletion_time"].get<int64_t>()};
}

const char *kNonExpiringRoot = R"({
	"lot_name": "ne_root",
	"owner": "owner1",
	"parents": ["ne_root"],
	"paths": [{"path": "/ne/root", "recursive": true}],
	"management_policy_attrs": {
		"dedicated_GB": 100,
		"opportunistic_GB": 50,
		"max_num_objects": 1000,
		"creation_time": 0,
		"expiration_time": 0,
		"deletion_time": 0
	}
})";
} // namespace

TEST_F(StrictHierarchyTest, AddNonExpiringRootLotSucceeds) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(kNonExpiringRoot, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	auto t = get_lot_times("ne_root");
	EXPECT_EQ(t.creation_time, 0);
	EXPECT_EQ(t.expiration_time, 0);
	EXPECT_EQ(t.deletion_time, 0);
}

TEST_F(StrictHierarchyTest, AddLotRejectsPartialZeroTimestamps) {
	addDefaultLot();
	char *raw_err = nullptr;

	// creation_time = 0 with non-zero expiration/deletion -> rejected.
	int rv = lotman_add_lot(R"({
		"lot_name": "partial_zero_a", "owner": "owner1", "parents": ["partial_zero_a"],
		"paths": [{"path": "/pz/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1,
			"creation_time": 0, "expiration_time": 100, "deletion_time": 200}})",
							&raw_err);
	UniqueCString err1(raw_err);
	EXPECT_NE(rv, 0) << "Mixing zero and non-zero timestamps must be rejected";
	ASSERT_NE(err1.get(), nullptr);
	EXPECT_NE(std::string(err1.get()).find("non-expiring"), std::string::npos)
		<< "Error message should mention the non-expiring sentinel; got: " << err1.get();

	// Only expiration_time = 0 -> rejected.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "partial_zero_b", "owner": "owner1", "parents": ["partial_zero_b"],
		"paths": [{"path": "/pz/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1,
			"creation_time": 100, "expiration_time": 0, "deletion_time": 200}})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0) << "Mixing zero and non-zero timestamps must be rejected";

	// Only deletion_time = 0 -> rejected.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "partial_zero_c", "owner": "owner1", "parents": ["partial_zero_c"],
		"paths": [{"path": "/pz/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1,
			"creation_time": 100, "expiration_time": 200, "deletion_time": 0}})",
						&raw_err);
	UniqueCString err3(raw_err);
	EXPECT_NE(rv, 0) << "Mixing zero and non-zero timestamps must be rejected";

	// None of the rejected lots should have been persisted.
	raw_err = nullptr;
	EXPECT_EQ(lotman_lot_exists("partial_zero_a", &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;
	EXPECT_EQ(lotman_lot_exists("partial_zero_b", &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;
	EXPECT_EQ(lotman_lot_exists("partial_zero_c", &raw_err), 0);
	free(raw_err);
}

TEST_F(StrictHierarchyTest, NonExpiringChildAllowedUnderNonExpiringParentStrict) {
	// Strict-hierarchy mode: the non-expiring parent's "infinite" window
	// absorbs the non-expiring child, and Axiom 3 must accept the child.
	addDefaultLot();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_lot(kNonExpiringRoot, &raw_err), 0);
	free(raw_err);

	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	rv = lotman_add_lot(R"({
		"lot_name": "ne_child",
		"owner": "owner1",
		"parents": ["ne_root"],
		"paths": [{"path": "/ne/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 0, "expiration_time": 0, "deletion_time": 0
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Non-expiring child under non-expiring parent must be allowed: "
					 << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, FiniteChildAllowedUnderNonExpiringParentStrict) {
	// Strict-hierarchy mode: a non-expiring parent absorbs any finite child
	// window because the parent's window is effectively (-inf, +inf).
	addDefaultLot();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_lot(kNonExpiringRoot, &raw_err), 0);
	free(raw_err);

	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	rv = lotman_add_lot(R"({
		"lot_name": "finite_under_ne",
		"owner": "owner1",
		"parents": ["ne_root"],
		"paths": [{"path": "/ne/finite", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 1, "expiration_time": 999999999, "deletion_time": 999999999
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Finite child under non-expiring parent must be allowed: " << (err.get() ? err.get() : "");
}

TEST_F(StrictHierarchyTest, NonExpiringChildRejectedUnderFiniteParentStrict) {
	// Strict-hierarchy mode: a non-expiring child cannot fit inside a finite
	// parent window, so Axiom 3 must reject the child.
	addDefaultLot();
	char *raw_err = nullptr;

	addLot(R"({
		"lot_name": "finite_parent",
		"owner": "owner1",
		"parents": ["finite_parent"],
		"paths": [{"path": "/finite/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 1000,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500
		}
	})");

	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	rv = lotman_add_lot(R"({
		"lot_name": "ne_under_finite",
		"owner": "owner1",
		"parents": ["finite_parent"],
		"paths": [{"path": "/finite/ne_child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 50,
			"creation_time": 0, "expiration_time": 0, "deletion_time": 0
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Non-expiring child under finite parent must be rejected";
	ASSERT_NE(err.get(), nullptr);
	EXPECT_NE(std::string(err.get()).find("non-expiring"), std::string::npos)
		<< "Error should mention non-expiring; got: " << err.get();

	// Confirm child wasn't persisted.
	raw_err = nullptr;
	EXPECT_EQ(lotman_lot_exists("ne_under_finite", &raw_err), 0);
	free(raw_err);
}

TEST_F(StrictHierarchyTest, NonExpiringLotIsAlwaysAlive) {
	// is_lot_alive (used by the "alive" contraction policy) must treat a
	// non-expiring lot as alive even though now > 0.
	addDefaultLot();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_lot(kNonExpiringRoot, &raw_err), 0);
	free(raw_err);

	// Set contraction_policy=alive and try to deletion-shrink the lot's
	// dedicated_GB; if the lot were treated as not-alive, the contraction
	// would be permitted. We expect rejection.
	ASSERT_EQ(lotman_set_context_str("contraction_policy", "alive", &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	int rv = lotman_update_lot(R"({
		"lot_name": "ne_root",
		"management_policy_attrs": {"dedicated_GB": 1}
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Non-expiring lot must be considered alive and contraction must be blocked";
}

TEST_F(StrictHierarchyTest, NonExpiringLotNotReturnedFromPastExpQuery) {
	addDefaultLot();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_lot(kNonExpiringRoot, &raw_err), 0);
	free(raw_err);

	// Default lot in this fixture has expiration_time=9999, which is far in
	// the past relative to "now"; it should be returned. ne_root must NOT.
	char **raw_lots = nullptr;
	raw_err = nullptr;
	int rv = lotman_get_lots_past_exp(/*recursive=*/false, /*include_reclaimed=*/true, &raw_lots, &raw_err);
	UniqueStringList lots(raw_lots);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");
	bool found_default = false;
	bool found_ne_root = false;
	for (int i = 0; lots.get()[i]; ++i) {
		if (std::string(lots.get()[i]) == "default")
			found_default = true;
		if (std::string(lots.get()[i]) == "ne_root")
			found_ne_root = true;
	}
	EXPECT_TRUE(found_default);
	EXPECT_FALSE(found_ne_root) << "Non-expiring lots must never appear in past-expiration queries";

	// Same expectation for past-deletion.
	raw_lots = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_del(/*recursive=*/false, /*include_reclaimed=*/true, &raw_lots, &raw_err);
	UniqueStringList del_lots(raw_lots);
	UniqueCString del_err(raw_err);
	ASSERT_EQ(rv, 0) << (del_err.get() ? del_err.get() : "");
	bool found_ne_in_del = false;
	for (int i = 0; del_lots.get()[i]; ++i) {
		if (std::string(del_lots.get()[i]) == "ne_root")
			found_ne_in_del = true;
	}
	EXPECT_FALSE(found_ne_in_del) << "Non-expiring lots must never appear in past-deletion queries";
}

TEST_F(StrictHierarchyTest, UpdateLotToNonExpiringIsAtomic) {
	// Flipping a finite lot to the non-expiring sentinel by updating all
	// three timestamps in a single envelope must succeed; per-field
	// validators tolerate the transient partial-zero state inside the
	// transaction so the caller can perform the transition atomically.
	addDefaultLot();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_lot(kNonExpiringRoot, &raw_err), 0);
	free(raw_err);
	raw_err = nullptr;

	addLot(R"({
		"lot_name": "transitions",
		"owner": "owner1",
		"parents": ["ne_root"],
		"paths": [{"path": "/ne/transitions", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 1, "expiration_time": 1000, "deletion_time": 1500
		}
	})");

	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "");

	// Atomic flip to non-expiring.
	rv = lotman_update_lot(R"({
		"lot_name": "transitions",
		"management_policy_attrs": {
			"creation_time": 0, "expiration_time": 0, "deletion_time": 0
		}
	})",
						   &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Atomic flip to non-expiring must succeed: " << (err.get() ? err.get() : "");
	auto t = get_lot_times("transitions");
	EXPECT_EQ(t.creation_time, 0);
	EXPECT_EQ(t.expiration_time, 0);
	EXPECT_EQ(t.deletion_time, 0);

	// And back: atomic flip from non-expiring to finite.
	raw_err = nullptr;
	rv = lotman_update_lot(R"({
		"lot_name": "transitions",
		"management_policy_attrs": {
			"creation_time": 2, "expiration_time": 2000, "deletion_time": 2500
		}
	})",
						   &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Atomic flip back to finite must succeed: " << (err.get() ? err.get() : "");
	t = get_lot_times("transitions");
	EXPECT_EQ(t.creation_time, 2);
	EXPECT_EQ(t.expiration_time, 2000);
	EXPECT_EQ(t.deletion_time, 2500);
}

TEST_F(StrictHierarchyTest, UpdateLotPartialZeroEndStateRejectedAndRolledBack) {
	// If an update envelope ends with a partial-zero timestamp triple, the
	// post-loop sentinel check must reject it and roll back the entire
	// envelope (no per-field write may persist).
	addDefaultLot();
	char *raw_err = nullptr;

	addLot(R"({
		"lot_name": "pz_lot",
		"owner": "owner1",
		"parents": ["pz_lot"],
		"paths": [{"path": "/pz/lot", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10, "opportunistic_GB": 5, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 1000, "deletion_time": 1500
		}
	})");

	// Attempt to set only creation_time and expiration_time to 0 while
	// leaving deletion_time non-zero -> must be rejected.
	int rv = lotman_update_lot(R"({
		"lot_name": "pz_lot",
		"management_policy_attrs": {
			"creation_time": 0, "expiration_time": 0
		}
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Partial-zero end state must be rejected";

	// Original timestamps must be intact (full rollback).
	auto t = get_lot_times("pz_lot");
	EXPECT_EQ(t.creation_time, 100);
	EXPECT_EQ(t.expiration_time, 1000);
	EXPECT_EQ(t.deletion_time, 1500);
}

TEST_F(StrictHierarchyTest, NonExpiringLotPathOverlapsAnyOtherLot) {
	// Two non-expiring lots may not claim the same path, and a non-expiring
	// lot must conflict with a finite lot claiming the same path because
	// the non-expiring window is treated as covering all time.
	addDefaultLot();
	char *raw_err = nullptr;
	ASSERT_EQ(lotman_add_lot(kNonExpiringRoot, &raw_err), 0);
	free(raw_err);

	// Another non-expiring lot claiming the same path -> reject.
	raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "ne_root_dup",
		"owner": "owner1",
		"parents": ["ne_root_dup"],
		"paths": [{"path": "/ne/root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1,
			"creation_time": 0, "expiration_time": 0, "deletion_time": 0
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Two non-expiring lots claiming the same path must conflict";

	// A finite lot claiming the same path -> reject (sentinel covers all time).
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "finite_dup",
		"owner": "owner1",
		"parents": ["finite_dup"],
		"paths": [{"path": "/ne/root", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1, "opportunistic_GB": 0, "max_num_objects": 1,
			"creation_time": 100, "expiration_time": 200, "deletion_time": 300
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0) << "Finite lot must conflict with a non-expiring lot on the same path";
}

// ============================================================================
// Unbounded MPA sentinel (per-axis -1 == "no bound") tests
// ============================================================================
//
// Resource axes:
//   * Storage axis: dedicated_GB and opportunistic_GB are independent pools.
//     - dedicated_GB == -1 means "unbounded dedicated allotment". Because
//       opportunistic_GB tracks data over the dedicated allotment, an
//       unbounded dedicated allotment is meaningless without an unbounded
//       opportunistic axis, so dedicated_GB == -1 requires opportunistic_GB
//       == -1; any other combo with dedicated_GB == -1 is rejected.
//     - opportunistic_GB == -1 with a finite (>= 0) dedicated_GB is allowed
//       (finite guaranteed allotment + unbounded burst).
//     - dedicated_GB == 0 means "no guaranteed storage" (a purely
//       opportunistic lot); opportunistic_GB may be >= 0 or -1.
//     - dedicated_GB > 0 with opportunistic_GB == 0 is allowed (no burst).
//   * Object axis: max_num_objects.
//     - max_num_objects == -1 means "unbounded objects".
//     - max_num_objects == 0 means "no objects allowed".
// Different axes are independent.

// Add a lot that is unbounded on the storage axis but bounded on objects.
TEST_F(StrictHierarchyTest, AddLotUnboundedStorageBoundedObjectsSucceeds) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "unbounded_storage",
		"owner": "owner1",
		"parents": ["unbounded_storage"],
		"paths": [{"path": "/unbounded/storage", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Unbounded storage with bounded objects should succeed: "
					 << (err.get() ? err.get() : "<no error>");
}

// Add a lot that is unbounded on the object axis but bounded on storage.
TEST_F(StrictHierarchyTest, AddLotUnboundedObjectsBoundedStorageSucceeds) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "unbounded_objs",
		"owner": "owner1",
		"parents": ["unbounded_objs"],
		"paths": [{"path": "/unbounded/objs", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Unbounded objects with bounded storage should succeed: "
					 << (err.get() ? err.get() : "<no error>");
}

// Add a lot that is unbounded on every axis.
TEST_F(StrictHierarchyTest, AddLotFullyUnboundedSucceeds) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "fully_unbounded",
		"owner": "owner1",
		"parents": ["fully_unbounded"],
		"paths": [{"path": "/unbounded/all", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Fully unbounded lot should succeed: " << (err.get() ? err.get() : "<no error>");
}

// dedicated_GB == -1 with a finite opportunistic_GB must be rejected: an
// unbounded dedicated allotment requires an unbounded opportunistic axis,
// since opportunistic tracks data over the dedicated allotment.
TEST_F(StrictHierarchyTest, AddLotRejectsUnboundedDedicatedFiniteOpportunistic) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "bad_storage",
		"owner": "owner1",
		"parents": ["bad_storage"],
		"paths": [{"path": "/bad/storage", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": 50,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "dedicated_GB == -1 with a finite opportunistic_GB must be rejected";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("Storage-axis") != std::string::npos || err_str.find("dedicated_GB") != std::string::npos)
		<< "Error message should mention the storage axis violation; got: " << err_str;
}

// dedicated_GB == 0 with opportunistic_GB > 0 is now LEGAL: the lot has
// no guaranteed allotment but may use opportunistic burst.
TEST_F(StrictHierarchyTest, AddLotZeroDedicatedNonZeroOpportunisticAllowedAsPurelyOpportunistic) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "purely_opportunistic_finite",
		"owner": "owner1",
		"parents": ["purely_opportunistic_finite"],
		"paths": [{"path": "/purely/opp/finite", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 0,
			"opportunistic_GB": 50,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "dedicated_GB == 0 with finite opportunistic_GB should succeed (purely opportunistic, finite "
						"burst): "
					 << (err.get() ? err.get() : "<no error>");
}

// dedicated_GB == 0 with opportunistic_GB == -1: a purely-opportunistic lot
// with unbounded burst is allowed (unbounded opp axis is independent of ded).
TEST_F(StrictHierarchyTest, AddLotZeroDedicatedUnboundedOpportunisticAllowed) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "purely_opportunistic_unbounded",
		"owner": "owner1",
		"parents": ["purely_opportunistic_unbounded"],
		"paths": [{"path": "/purely/opp/unb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 0,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "dedicated_GB == 0 with opportunistic_GB == -1 should succeed (purely opportunistic, unbounded "
						"burst): "
					 << (err.get() ? err.get() : "<no error>");
}

// dedicated_GB > 0 with opportunistic_GB == -1: finite guaranteed allotment
// plus unbounded burst is allowed.
TEST_F(StrictHierarchyTest, AddLotFiniteDedicatedUnboundedOpportunisticAllowed) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "ded_finite_opp_unb",
		"owner": "owner1",
		"parents": ["ded_finite_opp_unb"],
		"paths": [{"path": "/ded/finite/opp/unb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Finite dedicated + unbounded opportunistic should succeed: "
					 << (err.get() ? err.get() : "<no error>");
}

// dedicated_GB > 0 with opportunistic_GB == 0 is fine (no burst).
TEST_F(StrictHierarchyTest, AddLotZeroOpportunisticOnlySucceeds) {
	addDefaultLot();
	char *raw_err = nullptr;
	int rv = lotman_add_lot(R"({
		"lot_name": "no_burst",
		"owner": "owner1",
		"parents": ["no_burst"],
		"paths": [{"path": "/no/burst", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 0,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})",
							&raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "dedicated_GB > 0 with opportunistic_GB == 0 should be allowed (no burst capacity): "
					 << (err.get() ? err.get() : "<no error>");
}

// Strict hierarchy: a parent that is unbounded on the storage axis absorbs
// any finite child storage allocation.
TEST_F(StrictHierarchyTest, FiniteChildAllowedUnderUnboundedStorageParentStrict) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "us_parent",
		"owner": "owner1",
		"parents": ["us_parent"],
		"paths": [{"path": "/us/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_add_lot(R"({
		"lot_name": "us_child_finite",
		"owner": "owner1",
		"parents": ["us_parent"],
		"paths": [{"path": "/us/parent/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1000000,
			"opportunistic_GB": 1000000,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "An unbounded-storage parent must absorb any child storage allocation: "
					 << (err.get() ? err.get() : "<no error>");
}

// Strict hierarchy: an unbounded-storage child cannot live under a
// bounded-storage parent.
TEST_F(StrictHierarchyTest, UnboundedStorageChildRejectedUnderBoundedStorageParentStrict) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "bs_parent",
		"owner": "owner1",
		"parents": ["bs_parent"],
		"paths": [{"path": "/bs/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_add_lot(R"({
		"lot_name": "bs_child_unbounded",
		"owner": "owner1",
		"parents": ["bs_parent"],
		"paths": [{"path": "/bs/parent/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "An unbounded-storage child cannot live under a bounded-storage parent";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("unbounded") != std::string::npos)
		<< "Error message should mention unbounded; got: " << err_str;
}

// Object axis is independent of storage axis: a parent unbounded on objects
// only must still enforce its storage cap.
TEST_F(StrictHierarchyTest, AxesAreIndependentObjectsUnboundedOnly) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "axis_parent",
		"owner": "owner1",
		"parents": ["axis_parent"],
		"paths": [{"path": "/axis/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Unbounded-objects child fits because parent is also unbounded on objects.
	// But finite storage that exceeds parent's storage axis must fail.
	rv = lotman_add_lot(R"({
		"lot_name": "axis_child_bad_storage",
		"owner": "owner1",
		"parents": ["axis_parent"],
		"paths": [{"path": "/axis/parent/c1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1000,
			"opportunistic_GB": 0,
			"max_num_objects": -1,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "Finite storage > parent storage cap must still fail when only the object axis is unbounded";

	// A child whose storage fits and has unbounded objects succeeds.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "axis_child_ok",
		"owner": "owner1",
		"parents": ["axis_parent"],
		"paths": [{"path": "/axis/parent/c2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": -1,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_EQ(rv, 0) << "Storage-fitting unbounded-objects child under unbounded-objects parent should succeed: "
					 << (err2.get() ? err2.get() : "<no error>");
}

// Strict hierarchy: an unbounded-objects child cannot live under a
// bounded-objects parent.
TEST_F(StrictHierarchyTest, UnboundedObjectsChildRejectedUnderBoundedObjectsParentStrict) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "bo_parent",
		"owner": "owner1",
		"parents": ["bo_parent"],
		"paths": [{"path": "/bo/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	rv = lotman_add_lot(R"({
		"lot_name": "bo_child_unbounded",
		"owner": "owner1",
		"parents": ["bo_parent"],
		"paths": [{"path": "/bo/parent/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": 25,
			"max_num_objects": -1,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_NE(rv, 0) << "An unbounded-objects child cannot live under a bounded-objects parent";
	std::string err_str(err.get() ? err.get() : "");
	EXPECT_TRUE(err_str.find("unbounded") != std::string::npos || err_str.find("max_num_objects") != std::string::npos)
		<< "Error should mention unbounded/max_num_objects; got: " << err_str;
}

// Sweep line: a parent unbounded on the storage axis allows the sum of
// concurrently-active children's attributed storage to exceed any finite
// number.
TEST_F(StrictHierarchyTest, SweepLineUnboundedStorageParentAllowsAnyChildSum) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "sw_unb_parent",
		"owner": "owner1",
		"parents": ["sw_unb_parent"],
		"paths": [{"path": "/sw/unb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// Two large overlapping children both "exceed" any finite cap on storage.
	for (const char *child : {R"({
			"lot_name": "sw_unb_c1",
			"owner": "owner1",
			"parents": ["sw_unb_parent"],
			"paths": [{"path": "/sw/unb/c1", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 5000, "opportunistic_GB": 5000, "max_num_objects": 100,
				"creation_time": 200, "expiration_time": 8000, "deletion_time": 9000
			}
		})",
							  R"({
			"lot_name": "sw_unb_c2",
			"owner": "owner1",
			"parents": ["sw_unb_parent"],
			"paths": [{"path": "/sw/unb/c2", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 5000, "opportunistic_GB": 5000, "max_num_objects": 100,
				"creation_time": 300, "expiration_time": 8500, "deletion_time": 9000
			}
		})"}) {
		raw_err = nullptr;
		rv = lotman_add_lot(child, &raw_err);
		UniqueCString cerr(raw_err);
		EXPECT_EQ(rv, 0) << "Child should succeed under unbounded-storage parent: "
						 << (cerr.get() ? cerr.get() : "<no error>");
	}
}

// Sweep line: a bounded parent still rejects an over-allocation when only
// the object axis is unbounded.
TEST_F(StrictHierarchyTest, SweepLineBoundedStorageRejectsOverAllocationDespiteUnboundedObjects) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "sw_mix_parent",
		"owner": "owner1",
		"parents": ["sw_mix_parent"],
		"paths": [{"path": "/sw/mix", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 0,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// First child takes 60 GB during [200, 8000).
	rv = lotman_add_lot(R"({
		"lot_name": "sw_mix_c1",
		"owner": "owner1",
		"parents": ["sw_mix_parent"],
		"paths": [{"path": "/sw/mix/c1", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 60, "opportunistic_GB": 0, "max_num_objects": -1,
			"creation_time": 200, "expiration_time": 8000, "deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString err1(raw_err);
	ASSERT_EQ(rv, 0) << "First child should fit: " << (err1.get() ? err1.get() : "<no error>");

	// Second child takes 50 GB during [300, 7000), overlapping the first;
	// concurrent peak = 110 GB > parent cap of 100 GB.
	raw_err = nullptr;
	rv = lotman_add_lot(R"({
		"lot_name": "sw_mix_c2",
		"owner": "owner1",
		"parents": ["sw_mix_parent"],
		"paths": [{"path": "/sw/mix/c2", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50, "opportunistic_GB": 0, "max_num_objects": -1,
			"creation_time": 300, "expiration_time": 7000, "deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString err2(raw_err);
	EXPECT_NE(rv, 0) << "Concurrent children must not exceed bounded storage axis even when objects are unbounded";
}

// Sweep line: a non-expiring child that is unbounded on storage under a
// non-expiring, unbounded-storage parent is accepted (worst case combined
// with finite siblings).
TEST_F(StrictHierarchyTest, SweepLineNonExpiringUnboundedChildUnderNonExpiringUnboundedParent) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "ne_unb_parent",
		"owner": "owner1",
		"parents": ["ne_unb_parent"],
		"paths": [{"path": "/ne/unb/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1, "opportunistic_GB": -1, "max_num_objects": -1,
			"creation_time": 0, "expiration_time": 0, "deletion_time": 0
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	// A non-expiring child unbounded on every axis under a non-expiring
	// unbounded parent: must succeed.
	rv = lotman_add_lot(R"({
		"lot_name": "ne_unb_child",
		"owner": "owner1",
		"parents": ["ne_unb_parent"],
		"paths": [{"path": "/ne/unb/parent/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1, "opportunistic_GB": -1, "max_num_objects": -1,
			"creation_time": 0, "expiration_time": 0, "deletion_time": 0
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "Non-expiring fully-unbounded child under non-expiring fully-unbounded parent should succeed: "
					 << (err.get() ? err.get() : "<no error>");
}

// Past-quota query: an unbounded-storage lot is never returned from
// lotman_get_lots_past_ded / past_opp.
TEST_F(StrictHierarchyTest, UnboundedStorageLotNotReturnedFromPastDedQuery) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "pq_unbounded",
		"owner": "owner1",
		"parents": ["pq_unbounded"],
		"paths": [{"path": "/pq/unb", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1, "opportunistic_GB": -1, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500
		}
	})");

	// Push self_GB very high; an unbounded-storage lot can never be "past"
	// its dedicated quota.
	const char *usage_json = R"({
		"lot_name": "pq_unbounded",
		"self_GB": 999999.0
	})";
	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(usage_json, false, &raw_err);
	UniqueCString uerr(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set usage: " << (uerr.get() ? uerr.get() : "<no error>");

	char **lots = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_ded(false, false, /*include_reclaimed=*/true, &lots, false, &raw_err);
	UniqueCString perr(raw_err);
	ASSERT_EQ(rv, 0) << "lotman_get_lots_past_ded failed: " << (perr.get() ? perr.get() : "<no error>");
	bool found = false;
	if (lots) {
		for (size_t i = 0; lots[i] != nullptr; ++i) {
			if (std::string(lots[i]) == "pq_unbounded")
				found = true;
		}
		lotman_free_string_list(lots);
	}
	EXPECT_FALSE(found) << "An unbounded-storage lot must never appear in past-dedicated-quota queries";
}

// Past-quota query: an unbounded-objects lot is never returned from
// lotman_get_lots_past_obj.
TEST_F(StrictHierarchyTest, UnboundedObjectsLotNotReturnedFromPastObjQuery) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "pq_unb_obj",
		"owner": "owner1",
		"parents": ["pq_unb_obj"],
		"paths": [{"path": "/pq/unb/obj", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 0, "max_num_objects": -1,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500
		}
	})");

	const char *usage_json = R"({
		"lot_name": "pq_unb_obj",
		"self_objects": 999999
	})";
	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(usage_json, false, &raw_err);
	UniqueCString uerr(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to set usage: " << (uerr.get() ? uerr.get() : "<no error>");

	char **lots = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_obj(false, false, /*include_reclaimed=*/true, &lots, false, &raw_err);
	UniqueCString perr(raw_err);
	ASSERT_EQ(rv, 0) << "lotman_get_lots_past_obj failed: " << (perr.get() ? perr.get() : "<no error>");
	bool found = false;
	if (lots) {
		for (size_t i = 0; lots[i] != nullptr; ++i) {
			if (std::string(lots[i]) == "pq_unb_obj")
				found = true;
		}
		lotman_free_string_list(lots);
	}
	EXPECT_FALSE(found) << "An unbounded-objects lot must never appear in past-object-quota queries";
}

// Atomic flip via lotman_update_lot: set both dedicated_GB and
// opportunistic_GB to 0 in one envelope.
TEST_F(StrictHierarchyTest, UpdateLotToUnboundedStorageIsAtomic) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "flip_target",
		"owner": "owner1",
		"parents": ["flip_target"],
		"paths": [{"path": "/flip/target", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_update_lot(R"({
		"lot_name": "flip_target",
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1
		}
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_EQ(rv, 0) << "Atomic flip to unbounded storage should succeed: " << (err.get() ? err.get() : "<no error>");
}

// Atomic flip via lotman_update_lot: a partial flip leaving the storage
// axis in (dedicated_GB == -1, opportunistic_GB != -1) must be rejected and
// rolled back.
TEST_F(StrictHierarchyTest, UpdateLotPartialStorageFlipRejectedAndRolledBack) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "partial_flip",
		"owner": "owner1",
		"parents": ["partial_flip"],
		"paths": [{"path": "/partial/flip", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100, "opportunistic_GB": 50, "max_num_objects": 100,
			"creation_time": 100, "expiration_time": 9000, "deletion_time": 9500
		}
	})");

	// Try to set only dedicated_GB to -1 (leaving opportunistic_GB at 50).
	char *raw_err = nullptr;
	int rv = lotman_update_lot(R"({
		"lot_name": "partial_flip",
		"management_policy_attrs": {
			"dedicated_GB": -1
		}
	})",
							   &raw_err);
	UniqueCString err(raw_err);
	EXPECT_NE(rv, 0) << "Partial storage flip (dedicated_GB == -1 with opportunistic_GB != -1) must be rejected";

	// Confirm rollback: original values still present.
	char *out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_as_json("partial_flip", false, &out, &raw_err);
	UniqueCString gerr(raw_err);
	UniqueCString out_owned(out);
	ASSERT_EQ(rv, 0) << "Failed to read back lot: " << (gerr.get() ? gerr.get() : "<no error>");
	json parsed = json::parse(out_owned.get());
	EXPECT_DOUBLE_EQ(parsed["management_policy_attrs"]["dedicated_GB"].get<double>(), 100.0);
	EXPECT_DOUBLE_EQ(parsed["management_policy_attrs"]["opportunistic_GB"].get<double>(), 50.0);
}

// ============================================================================
// Bug regression tests
// ============================================================================

// Bug 1 regression: dedicated_GB and opportunistic_GB are independent storage
// pools, so axiom 1 must NOT enforce a combined (ded+opp) cap. A child whose
// per-axis attributions each fit under the parent's per-axis caps must be
// accepted even if (child.ded + child.opp) > (parent.ded + parent.opp).
TEST_F(StrictHierarchyTest, Axiom1NoCombinedDedOppCheckRegression) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "indep_parent",
		"owner": "owner1",
		"parents": ["indep_parent"],
		"paths": [{"path": "/indep/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 100,
			"max_num_objects": 1000,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString cerr(raw_err);
	ASSERT_EQ(rv, 0) << cerr.get();

	// Child: ded=100, opp=100. Per-axis: ded=100<=100 ✓, opp=100<=100 ✓.
	// Combined: 200 > parent combined of 200? No, equal. Use values that
	// individually fit but exceed combined: ded=80, opp=80 (combined=160 >
	// would-be-old-cap of (parent.ded + parent.opp == 200)? No still fits).
	// Use parent ded=50, opp=50 (combined=100). Child ded=40, opp=40
	// (combined=80 <= 100). That's fine. To EXCEED an old combined cap, we
	// need each axis to individually fit but the sum to exceed: that's
	// impossible if parent.ded+parent.opp == sum of caps. So this test just
	// confirms a child whose per-axis attributions each equal the parent cap
	// is accepted — which the old combined check would have allowed too, but
	// it's a useful boundary case.
	rv = lotman_add_lot(R"({
		"lot_name": "indep_child",
		"owner": "owner1",
		"parents": ["indep_parent"],
		"paths": [{"path": "/indep/parent/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 100,
			"max_num_objects": 100,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})",
						&raw_err);
	UniqueCString aerr(raw_err);
	EXPECT_EQ(rv, 0) << "Per-axis caps each fit; combined check has been removed (independent pools): "
					 << (aerr.get() ? aerr.get() : "<no error>");
}

// Bug 2 regression: lotman_add_to_lot must honor caller-supplied
// parent_attributions for the post-add validation rather than running an
// auto equal split (which can spuriously reject the addition before the
// explicit shares are applied).
TEST_F(StrictHierarchyTest, AddToLotHonorsExplicitParentAttributions) {
	addDefaultLot();

	// Parent A is bounded; parent B is fully unbounded so it can absorb the
	// full child allocation when the caller assigns A nothing.
	addLot(R"({
		"lot_name": "a2l_parent_a",
		"owner": "owner1",
		"parents": ["a2l_parent_a"],
		"paths": [{"path": "/a2l/a", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 0,
			"opportunistic_GB": 0,
			"max_num_objects": 0,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	addLot(R"({
		"lot_name": "a2l_parent_b",
		"owner": "owner1",
		"parents": ["a2l_parent_b"],
		"paths": [{"path": "/a2l/b", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	// Child starts with one parent (parent_b, which is unbounded so the
	// initial equal split trivially fits).
	addLot(R"({
		"lot_name": "a2l_child",
		"owner": "owner1",
		"parents": ["a2l_parent_b"],
		"paths": [{"path": "/a2l/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 1,
			"opportunistic_GB": 0,
			"max_num_objects": 10,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString cerr(raw_err);
	ASSERT_EQ(rv, 0) << cerr.get();

	// Now add parent_a (which has no capacity) but explicitly attribute 0 of
	// every MPA to it. Without bug 2 fixed, add_to_lot would run an auto
	// equal-split (0.5 GB to each of the two parents) and reject because
	// parent_a has 0 capacity. With bug 2 fixed, the explicit attribution of
	// 0 to parent_a (and the remainder to parent_b) is honored from the
	// start.
	rv = lotman_add_to_lot(R"({
		"lot_name": "a2l_child",
		"parents": ["a2l_parent_a"],
		"parent_attributions": {
			"a2l_parent_a": {
				"dedicated_GB": 0,
				"opportunistic_GB": 0,
				"max_num_objects": 0
			}
		}
	})",
						   &raw_err);
	UniqueCString aerr(raw_err);
	EXPECT_EQ(rv, 0) << "add_to_lot must honor explicit parent_attributions and not auto-equal-split: "
					 << (aerr.get() ? aerr.get() : "<no error>");
}

// ============================================================================
// Sentinel-safety regression tests: arithmetic with -1 unbounded MPA values
// ============================================================================

// REGRESSION: get_lot_usage("dedicated_GB") non-recursive with sentinel -1.
// Before the fix, every CASE branch compared self_GB >= dedicated_GB, which is
// always true when dedicated_GB = -1, so the query returned -1 (the sentinel)
// instead of actual usage.
TEST_F(StrictHierarchyTest, LotUsageDedicatedSentinelNonRecursive) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "ub_ded_lot",
		"owner": "owner1",
		"parents": ["ub_ded_lot"],
		"paths": [{"path": "/ub_ded", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv =
		lotman_update_lot_usage(R"({"lot_name": "ub_ded_lot", "self_GB": 5.0, "self_objects": 3})", false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	char *raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(R"({"lot_name": "ub_ded_lot", "dedicated_GB": false})", &raw_out, &raw_err);
	UniqueCString out_str(raw_out);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();
	json out = json::parse(out_str.get());
	// self_contrib must be actual usage (5.0), not the -1 sentinel
	EXPECT_DOUBLE_EQ(out["dedicated_GB"]["self_contrib"].get<double>(), 5.0)
		<< "dedicated_GB self_contrib must return actual usage, not the -1 sentinel";
}

// REGRESSION: get_lot_usage("dedicated_GB") recursive with sentinel -1.
// Before the fix, the ELSE branch returned dedicated_GB (= -1) as total,
// and the first WHEN (self_GB >= -1, always true) capped self_contrib at -1.
TEST_F(StrictHierarchyTest, LotUsageDedicatedSentinelRecursive) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "ub_ded_par",
		"owner": "owner1",
		"parents": ["ub_ded_par"],
		"paths": [{"path": "/ub_ded_par", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	addLot(R"({
		"lot_name": "ub_ded_child",
		"owner": "owner1",
		"parents": ["ub_ded_par"],
		"paths": [{"path": "/ub_ded_par/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 50,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(R"({"lot_name": "ub_ded_par", "self_GB": 4.0})", false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();
	raw_err = nullptr;
	rv = lotman_update_lot_usage(R"({"lot_name": "ub_ded_child", "self_GB": 3.0})", false, &raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	char *raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(R"({"lot_name": "ub_ded_par", "dedicated_GB": true})", &raw_out, &raw_err);
	UniqueCString out_str(raw_out);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();
	json out = json::parse(out_str.get());
	EXPECT_DOUBLE_EQ(out["dedicated_GB"]["self_contrib"].get<double>(), 4.0)
		<< "self_contrib must be actual usage, not the -1 sentinel";
	EXPECT_DOUBLE_EQ(out["dedicated_GB"]["children_contrib"].get<double>(), 3.0)
		<< "children_contrib must be actual children usage, not 0 forced by sentinel";
	EXPECT_DOUBLE_EQ(out["dedicated_GB"]["total"].get<double>(), 7.0)
		<< "total must be self + children, not capped at -1";
}

// REGRESSION: get_lot_usage("opportunistic_GB") non-recursive, dedicated = -1.
// When dedicated_GB = -1 (infinite dedicated), nothing is ever in the
// opportunistic tier; usage should be 0.  The sentinel contract also requires
// opportunistic_GB = -1 whenever dedicated_GB = -1.  Before the fix, the WHEN
// branch evaluated self_GB >= dedicated_GB + opportunistic_GB = (-1 + -1) = -2,
// which always fires, returning opportunistic_GB = -1 instead of 0.
TEST_F(StrictHierarchyTest, LotUsageOpportunisticWhenDedicatedUnbounded) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "ub_ded_opp_lot",
		"owner": "owner1",
		"parents": ["ub_ded_opp_lot"],
		"paths": [{"path": "/ub_ded_opp", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(R"({"lot_name": "ub_ded_opp_lot", "self_GB": 15.0})", false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	char *raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(R"({"lot_name": "ub_ded_opp_lot", "opportunistic_GB": false})", &raw_out, &raw_err);
	UniqueCString out_str(raw_out);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();
	json out = json::parse(out_str.get());
	// With infinite dedicated, nothing is ever in the opportunistic tier
	EXPECT_DOUBLE_EQ(out["opportunistic_GB"]["self_contrib"].get<double>(), 0.0)
		<< "opportunistic usage must be 0 when dedicated_GB is unbounded (-1)";
}

// REGRESSION: get_lot_usage("opportunistic_GB") non-recursive, opp = -1.
// When opportunistic_GB = -1 (unbounded burst), the code must not evaluate
// self_GB >= dedicated_GB + (-1) (which always fires, returning wrong cap).
// Spillover above dedicated must be returned uncapped.
TEST_F(StrictHierarchyTest, LotUsageOpportunisticWhenOpportunisticUnbounded) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "ub_opp_lot",
		"owner": "owner1",
		"parents": ["ub_opp_lot"],
		"paths": [{"path": "/ub_opp", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": -1,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_update_lot_usage(R"({"lot_name": "ub_opp_lot", "self_GB": 15.0})", false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	char *raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(R"({"lot_name": "ub_opp_lot", "opportunistic_GB": false})", &raw_out, &raw_err);
	UniqueCString out_str(raw_out);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();
	json out = json::parse(out_str.get());
	// 15 GB total, 10 GB dedicated cap, so 5 GB of opportunistic usage
	EXPECT_DOUBLE_EQ(out["opportunistic_GB"]["self_contrib"].get<double>(), 5.0)
		<< "opportunistic usage must be self_GB - dedicated_GB when opp is unbounded (-1)";
}

// REGRESSION: non-recursive opportunistic_GB, finite caps, SQL typo.
// The second WHEN branch in the old SQL was:
//   THEN lot_usage.self_GB = management_policy_attributes.dedicated_GB
// In SQL, '=' in a SELECT is a comparison (returns 0 or 1), not subtraction.
// So the result was always 0 instead of (self_GB - dedicated_GB).
TEST_F(StrictHierarchyTest, LotUsageOpportunisticNonRecursiveTypoRegression) {
	addDefaultLot();
	addLot(R"({
		"lot_name": "opp_typo_lot",
		"owner": "owner1",
		"parents": ["opp_typo_lot"],
		"paths": [{"path": "/opp_typo", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 20,
			"max_num_objects": 100,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	// 15 GB puts us 5 GB into the opportunistic tier (above the 10 GB ded cap)
	int rv = lotman_update_lot_usage(R"({"lot_name": "opp_typo_lot", "self_GB": 15.0})", false, &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << err.get();

	char *raw_out = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(R"({"lot_name": "opp_typo_lot", "opportunistic_GB": false})", &raw_out, &raw_err);
	UniqueCString out_str(raw_out);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << err.get();
	json out = json::parse(out_str.get());
	// Should be 15 - 10 = 5.  Before the typo fix this returned 0 (bool 15==10).
	EXPECT_DOUBLE_EQ(out["opportunistic_GB"]["self_contrib"].get<double>(), 5.0)
		<< "opportunistic self_contrib must be self_GB - dedicated_GB; returned 0 due to SQL '=' vs '-' typo";
}

// REGRESSION: build_attribution_events must not multiply the -1 sentinel.
// When a child has dedicated/opportunistic/max_num_objects = -1 (unbounded),
// its sweep-line contribution must be 0.  Before the fix, the contribution was
// fraction * -1 = -1, corrupting peak_* fields in get_available_capacity.
TEST_F(StrictHierarchyTest, AvailableCapacityUnboundedChildPeakIsZero) {
	addDefaultLot();
	// Unbounded parent (axiom 1 allows unbounded child only under unbounded parent)
	addLot(R"({
		"lot_name": "ub_par",
		"owner": "owner1",
		"parents": ["ub_par"],
		"paths": [{"path": "/ub_par", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");
	addLot(R"({
		"lot_name": "ub_child",
		"owner": "owner1",
		"parents": ["ub_par"],
		"paths": [{"path": "/ub_par/child", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": -1,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 8500
		}
	})");

	auto [result, err_msg] = lotman::Lot::get_available_capacity("ub_par", 100, 9000);
	ASSERT_TRUE(err_msg.empty()) << err_msg;

	// Parent is unbounded on every axis, so available_* must be null
	EXPECT_TRUE(result["available_dedicated_GB"].is_null());
	EXPECT_TRUE(result["available_opportunistic_GB"].is_null());
	EXPECT_TRUE(result["available_max_num_objects"].is_null());
	EXPECT_TRUE(result["available_total_GB"].is_null());

	// Before the fix, peaks were computed as 1.0 * -1 = -1 per axis.
	// After the fix, unbounded children contribute 0 to the sweep.
	EXPECT_DOUBLE_EQ(result["peak_dedicated_GB"].get<double>(), 0.0)
		<< "peak_dedicated_GB must be 0 for an unbounded child, not -1 (sentinel * fraction)";
	EXPECT_DOUBLE_EQ(result["peak_opportunistic_GB"].get<double>(), 0.0)
		<< "peak_opportunistic_GB must be 0 for an unbounded child, not -1";
	EXPECT_EQ(result["peak_max_num_objects"].get<int64_t>(), 0)
		<< "peak_max_num_objects must be 0 for an unbounded child, not -1";
}

// Regression: parent_attributions schema must accept the -1 unbounded sentinel
// on opportunistic_GB and max_num_objects so that callers (e.g. Pelican's
// nested-namespace tree allocator) can propagate a parent's unbounded axis to
// the child's per-parent attribution. Before the fix, parent_attributions_def
// declared "minimum": 0 on every axis, which rejected the same -1 value that
// the management_policy_attrs block accepted.
TEST_F(StrictHierarchyTest, ParentAttributionsAcceptsUnboundedSentinel) {
	addDefaultLot();

	// Parent is unbounded on opportunistic_GB and max_num_objects.
	addLot(R"({
		"lot_name": "pa_unb_parent",
		"owner": "owner1",
		"parents": ["pa_unb_parent"],
		"paths": [{"path": "/pa/unb/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "<no error>");

	// Child propagates -1 on the parent's unbounded axes via parent_attributions.
	rv = lotman_add_lot(R"({
		"lot_name": "pa_unb_child",
		"owner": "owner1",
		"parents": ["pa_unb_parent"],
		"paths": [{"path": "/pa/unb/parent/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		},
		"parent_attributions": {
			"pa_unb_parent": {
				"dedicated_GB": 50,
				"opportunistic_GB": -1,
				"max_num_objects": -1
			}
		}
	})",
						&raw_err);
	err.reset(raw_err);
	EXPECT_EQ(rv, 0) << "parent_attributions must accept the -1 unbounded sentinel: "
					 << (err.get() ? err.get() : "<no error>");
}

// Regression: lotman_get_lot_as_json must serialize parent_attributions so
// that callers can read back per-parent MPA shares (the data is in the DB
// but was previously omitted from the JSON envelope, breaking round-trip
// audit/diff and any in-process integration test of nested-lot creation).
TEST_F(StrictHierarchyTest, GetLotAsJsonSerializesParentAttributions) {
	addDefaultLot();

	addLot(R"({
		"lot_name": "ga_parent",
		"owner": "owner1",
		"parents": ["ga_parent"],
		"paths": [{"path": "/ga/parent", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 100,
			"expiration_time": 9000,
			"deletion_time": 9500
		}
	})");

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("strict_hierarchy", "true", &raw_err);
	UniqueCString err(raw_err);
	ASSERT_EQ(rv, 0) << (err.get() ? err.get() : "<no error>");

	// Child explicitly attributes 50 GB dedicated, -1 (unbounded) on the other axes.
	rv = lotman_add_lot(R"({
		"lot_name": "ga_child",
		"owner": "owner1",
		"parents": ["ga_parent"],
		"paths": [{"path": "/ga/parent/c", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 50,
			"opportunistic_GB": -1,
			"max_num_objects": -1,
			"creation_time": 200,
			"expiration_time": 8000,
			"deletion_time": 9000
		},
		"parent_attributions": {
			"ga_parent": {
				"dedicated_GB": 50,
				"opportunistic_GB": -1,
				"max_num_objects": -1
			}
		}
	})",
						&raw_err);
	err.reset(raw_err);
	ASSERT_EQ(rv, 0) << "child create should succeed: " << (err.get() ? err.get() : "<no error>");

	char *out = nullptr;
	rv = lotman_get_lot_as_json("ga_child", false, &out, &raw_err);
	UniqueCString gerr(raw_err);
	ASSERT_EQ(rv, 0) << "get_lot_as_json failed: " << (gerr.get() ? gerr.get() : "<no error>");
	ASSERT_NE(out, nullptr);

	std::string out_str(out);
	free(out);
	auto parsed = nlohmann::json::parse(out_str);

	ASSERT_TRUE(parsed.contains("parent_attributions"))
		<< "lotman_get_lot_as_json must include parent_attributions: " << parsed.dump();
	ASSERT_TRUE(parsed["parent_attributions"].contains("ga_parent"))
		<< "parent_attributions must contain key for non-self parent: " << parsed["parent_attributions"].dump();

	const auto &pa = parsed["parent_attributions"]["ga_parent"];
	EXPECT_DOUBLE_EQ(pa["dedicated_GB"].get<double>(), 50.0);
	EXPECT_DOUBLE_EQ(pa["opportunistic_GB"].get<double>(), -1.0)
		<< "unbounded sentinel must round-trip rather than being emitted as fraction*-1";
	EXPECT_EQ(pa["max_num_objects"].get<int64_t>(), -1)
		<< "unbounded sentinel must round-trip rather than being emitted as fraction*-1";

	// A self-parent-only lot has no non-self parents, so parent_attributions
	// must still be present but empty (so consumers can rely on the key
	// always existing).
	out = nullptr;
	rv = lotman_get_lot_as_json("ga_parent", false, &out, &raw_err);
	UniqueCString perr(raw_err);
	ASSERT_EQ(rv, 0) << (perr.get() ? perr.get() : "<no error>");
	auto parent_parsed = nlohmann::json::parse(out);
	free(out);
	ASSERT_TRUE(parent_parsed.contains("parent_attributions"));
	EXPECT_TRUE(parent_parsed["parent_attributions"].is_object());
	EXPECT_TRUE(parent_parsed["parent_attributions"].empty());
}

} // namespace
