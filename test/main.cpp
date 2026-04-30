#include "../src/lotman.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <typeinfo>

using json = nlohmann::json;

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

// Test class to handle setup and teardown for each individual test
class LotManTest : public ::testing::Test {
  protected:
	std::string tmp_dir;

	std::string create_temp_directory() {
		// Generate a unique name for the temporary directory
		std::string temp_dir_template = "/tmp/lotman_test_XXXXXX";
		char temp_dir_name[temp_dir_template.size() + 1]; // +1 for the null-terminator
		std::strcpy(temp_dir_name, temp_dir_template.c_str());

		// mkdtemp replaces 'X's with a unique directory name and creates the directory
		char *mkdtemp_result = mkdtemp(temp_dir_name);
		if (mkdtemp_result == nullptr) {
			std::cerr << "Failed to create temporary directory\n";
			exit(1);
		}

		return std::string(mkdtemp_result);
	}

	// Called before each test
	void SetUp() override {
		tmp_dir = create_temp_directory();
		char *raw_err = nullptr;
		auto rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
		UniqueCString err(raw_err);
		if (rv != 0) {
			std::cerr << "Failed to set lot_home: " << err.get() << std::endl;
			exit(1);
		}
		raw_err = nullptr;
		rv = lotman_set_context_str("caller", "owner1", &raw_err);
		UniqueCString err2(raw_err);
		if (rv != 0) {
			std::cerr << "Failed to set caller: " << err2.get() << std::endl;
			exit(1);
		}
	}

	// Called after each test
	void TearDown() override {
		std::filesystem::remove_all(tmp_dir);
	}

	// Helper to add a lot and assert success
	void addLot(const char *lot_json) {
		char *raw_err = nullptr;
		int rv = lotman_add_lot(lot_json, &raw_err);
		UniqueCString err_msg(raw_err);
		ASSERT_EQ(rv, 0) << "Failed to add lot: " << err_msg.get();
	}

	// Helper to add default lot - MUST be created before any other lots
	void addDefaultLot() {
		const char *default_lot = R"({
			"lot_name": "default",
			"owner": "owner2",
			"parents": ["default"],
			"paths": [{"path": "/default/paths", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 5,
				"opportunistic_GB": 2.5,
				"max_num_objects": 100,
				"creation_time": 123,
				"expiration_time": 234,
				"deletion_time": 345
			}
		})";
		addLot(default_lot);
	}

	// Helper to add lot1 (self-parent root lot)
	void addLot1() {
		const char *lot1 = R"({
			"lot_name": "lot1",
			"owner": "owner1",
			"parents": ["lot1"],
			"paths": [
				{"path": "/1/2/3", "recursive": false},
				{"path": "/foo/bar", "recursive": true}
			],
			"management_policy_attrs": {
				"dedicated_GB": 5,
				"opportunistic_GB": 2.5,
				"max_num_objects": 20,
				"creation_time": 123,
				"expiration_time": 234,
				"deletion_time": 345
			}
		})";
		addLot(lot1);
	}

	// Helper to add lot2 (child of lot1)
	void addLot2() {
		const char *lot2 = R"({
			"lot_name": "lot2",
			"owner": "owner1",
			"parents": ["lot1"],
			"paths": [
				{"path": "/1/2/4", "recursive": true},
				{"path": "/foo/baz", "recursive": true}
			],
			"management_policy_attrs": {
				"dedicated_GB": 6,
				"opportunistic_GB": 1.5,
				"max_num_objects": 100,
				"creation_time": 123,
				"expiration_time": 233,
				"deletion_time": 355
			}
		})";
		addLot(lot2);
	}

	// Helper to add lot3 (self-parent root lot)
	void addLot3() {
		const char *lot3 = R"({
			"lot_name": "lot3",
			"owner": "owner1",
			"parents": ["lot3"],
			"paths": [
				{"path": "/another/path", "recursive": false},
				{"path": "/123", "recursive": true}
			],
			"management_policy_attrs": {
				"dedicated_GB": 3,
				"opportunistic_GB": 2.0,
				"max_num_objects": 60,
				"creation_time": 123,
				"expiration_time": 232,
				"deletion_time": 325
			}
		})";
		addLot(lot3);
	}

	// Helper to add lot4 (child of lot2 and lot3)
	void addLot4() {
		const char *lot4 = R"({
			"lot_name": "lot4",
			"owner": "owner1",
			"parents": ["lot2", "lot3"],
			"paths": [
				{"path": "/1/2/3/4", "recursive": true},
				{"path": "/345", "recursive": true}
			],
			"management_policy_attrs": {
				"dedicated_GB": 3,
				"opportunistic_GB": 2.1,
				"max_num_objects": 40,
				"creation_time": 123,
				"expiration_time": 231,
				"deletion_time": 315
			}
		})";
		addLot(lot4);
	}

	// Helper to add lot5 (child of lot3, parent of lot4)
	void addLot5() {
		const char *lot5 = R"({
			"lot_name": "lot5",
			"owner": "owner1",
			"parents": ["lot3"],
			"children": ["lot4"],
			"paths": [
				{"path": "/456", "recursive": false},
				{"path": "/567", "recursive": true}
			],
			"management_policy_attrs": {
				"dedicated_GB": 10,
				"opportunistic_GB": 3.5,
				"max_num_objects": 20,
				"creation_time": 100,
				"expiration_time": 200,
				"deletion_time": 300
			}
		})";
		addLot(lot5);
	}

	// Helper to add sep_node (self-parent root lot with long expiration)
	void addSepNode() {
		const char *sep_node = R"({
			"lot_name": "sep_node",
			"owner": "owner1",
			"parents": ["sep_node"],
			"paths": [{"path": "/sep/node", "recursive": true}],
			"management_policy_attrs": {
				"dedicated_GB": 3,
				"opportunistic_GB": 2.1,
				"max_num_objects": 10,
				"creation_time": 123,
				"expiration_time": 99679525853643,
				"deletion_time": 9267952553643
			}
		})";
		addLot(sep_node);
	}

	// Helper to set up the standard lot hierarchy: default, lot1 -> lot2 -> lot4, lot3 -> lot4, sep_node
	void setupStandardHierarchy() {
		addDefaultLot(); // Must be created first
		addLot1();
		addLot2();
		addLot3();
		addLot4();
		addSepNode();
	}

	// Helper to set up the full hierarchy with lot5 inserted between lot3 and lot4
	void setupFullHierarchy() {
		addDefaultLot(); // Must be created first
		addLot1();
		addLot2();
		addLot3();
		addLot4();
		addSepNode();
		addLot5(); // lot5 becomes parent of lot4, child of lot3
	}
};

namespace {

TEST_F(LotManTest, DefaultLotTests) {
	// The default lot must be created first before any other lots
	addDefaultLot();

	// Test that we can add a self-parent lot after default exists
	char *raw_err = nullptr;
	const char *lot1 = R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/1/2/3", "recursive": false},
			{"path": "/foo/bar", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2.5,
			"max_num_objects": 100,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";

	// Add lot1 (should succeed as self-parent after default exists)
	auto rv = lotman_add_lot(lot1, &raw_err);
	UniqueCString err1(raw_err);
	ASSERT_EQ(rv, 0) << err1.get();

	// Try to remove the default lot - should fail
	raw_err = nullptr;
	rv = lotman_remove_lot("default", true, true, true, false, &raw_err);
	UniqueCString err2(raw_err);
	ASSERT_NE(rv, 0) << err2.get();
}

TEST_F(LotManTest, AddRemoveSublot) {
	// The default lot must be created first
	addDefaultLot();

	// Set up fresh database with lot1
	const char *lot1 = R"({
		"lot_name": "lot1",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "1/2/3", "recursive": false},
			{"path": "/foo/bar", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2.5,
			"max_num_objects": 100,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";

	char *raw_err = nullptr;
	auto rv = lotman_add_lot(lot1, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Remove the lot
	const char *deleted_lot = "lot1";
	raw_err = nullptr;
	rv = lotman_remove_lot(deleted_lot, false, false, false, false, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify it no longer exists
	raw_err = nullptr;
	rv = lotman_lot_exists(deleted_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_FALSE(rv) << err_msg.get();

	// Try to remove a lot that doesn't exist
	const char *non_existent_lot = "non_existent_lot";
	raw_err = nullptr;
	rv = lotman_remove_lot(non_existent_lot, false, false, false, false, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0) << err_msg.get();
}

TEST_F(LotManTest, AddInvalidLots) {
	// Set up fresh database with standard hierarchy
	setupStandardHierarchy();

	char *raw_err = nullptr;

	// Try to add a lot with cyclic dependency
	// Cycle created by trying to add lot5 with lot1 as a child
	const char *cyclic_lot = R"({
		"lot_name": "lot5",
		"owner": "owner1",
		"parents": ["lot4"],
		"children": ["lot1"],
		"paths": [
			{"path": "/456", "recursive": false},
			{"path": "/567", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2.5,
			"max_num_objects": 100,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";
	auto rv = lotman_add_lot(cyclic_lot, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0) << err_msg.get();

	// Try to add lot with no parent
	const char *no_parents_lot = R"({
		"lot_name": "lot5",
		"owner": "owner1",
		"parents": [],
		"children": ["lot1"],
		"paths": [
			{"path": "/456", "recursive": false},
			{"path": "/567", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 111111,
			"opportunistic_GB": 2.5,
			"max_num_objects": 100,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";
	raw_err = nullptr;
	rv = lotman_add_lot(no_parents_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0) << err_msg.get();
}

TEST_F(LotManTest, InsertionTest) {
	// Set up fresh database with standard hierarchy
	setupStandardHierarchy();

	char *raw_err = nullptr;
	// Insert lot5 between lot3 and lot4
	const char *lot5 = R"({
		"lot_name": "lot5",
		"owner": "owner1",
		"parents": ["lot3"],
		"children": ["lot4"],
		"paths": [
			{"path": "/456", "recursive": false},
			{"path": "/567", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 3.5,
			"max_num_objects": 20,
			"creation_time": 100,
			"expiration_time": 200,
			"deletion_time": 300
		}
	})";
	int rv = lotman_add_lot(lot5, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Check that lot5 is a parent to lot4 and that lot3 is a parent to lot5
	char **raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_parent_names("lot4", false, false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	bool check = false;
	for (int iter = 0; output.get()[iter]; iter++) {
		if (static_cast<std::string>(output.get()[iter]) == "lot5") {
			check = true;
		}
	}
	ASSERT_TRUE(check);

	check = false;
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_children_names("lot3", false, false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output2(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	for (int iter = 0; output2.get()[iter]; iter++) {
		if (static_cast<std::string>(output2.get()[iter]) == "lot5") {
			check = true;
		}
	}
	ASSERT_TRUE(check);
}

TEST_F(LotManTest, ModifyLotTest) {
	// Set up fresh database with standard hierarchy (already includes default lot)
	setupStandardHierarchy();

	char *raw_err = nullptr;

	// Try to modify a non-existent lot
	const char *non_existent_lot = R"({
		"lot_name": "non_existent_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/1/2/3", "recursive": false},
			{"path": "/foo/bar", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2.5,
			"max_num_objects": 100,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";
	int rv = lotman_update_lot(non_existent_lot, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	// Modify lot3: change owner, change parent from self to lot2, update paths
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"owner": "not owner1",
		"parents": [{"current": "lot3", "new": "lot2"}],
		"paths": [
			{"current": "/another/path", "new": "/another/path", "recursive": true},
			{"current": "/123", "new": "/updated/path", "recursive": false}
		],
		"management_policy_attrs": {
			"dedicated_GB": 10.111,
			"opportunistic_GB": 6.6,
			"max_num_objects": 50,
			"expiration_time": 222,
			"deletion_time": 333
		}
	})";
	raw_err = nullptr;
	rv = lotman_update_lot(modified_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Try to add a cycle --> this should fail
	const char *modified_lot2 = R"({
		"lot_name": "lot2",
		"parents": [{"current": "lot1", "new": "lot3"}]
	})";
	raw_err = nullptr;
	rv = lotman_update_lot(modified_lot2, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	char **raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_owners("lot3", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList owner_out(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	bool check_old = false, check_new = false;
	for (int iter = 0; owner_out.get()[iter]; iter++) {
		if (strcmp(owner_out.get()[iter], "not owner1") == 0) {
			check_new = true;
		}
		if (strcmp(owner_out.get()[iter], "owner1") == 0) {
			check_old = true;
		}
	}
	ASSERT_TRUE(check_new & !check_old);

	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_parent_names("lot3", false, true, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList parents_out(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	check_old = false, check_new = false;
	for (int iter = 0; parents_out.get()[iter]; iter++) {
		if (static_cast<std::string>(parents_out.get()[iter]) == "lot2") {
			check_new = true;
		}
		if (static_cast<std::string>(parents_out.get()[iter]) == "lot3") {
			check_old = true;
		}
	}
	ASSERT_TRUE(check_new & !check_old);

	const char *addition_JSON = R"({
		"lot_name": "lot3",
		"paths": [{"path": "/foo/barr", "recursive": true}],
		"parents": ["sep_node"]
	})";
	raw_err = nullptr;
	rv = lotman_add_to_lot(addition_JSON, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Try to add a cycle --> this should fail
	const char *addition_JSON2 = R"({
		"lot_name": "lot1",
		"parents": ["lot2"]
	})";
	raw_err = nullptr;
	rv = lotman_add_to_lot(addition_JSON2, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	// Test removing parents
	// Add default as parent to sep_node, then remove
	const char *addition_JSON3 = R"({
		"lot_name": "sep_node",
		"parents": ["default"]
	})";
	raw_err = nullptr;
	rv = lotman_add_to_lot(addition_JSON3, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Try (and hopefully fail) to remove all of the parents and orphan the lot
	const char *removal_JSON1 = R"({
		"lot_name": "sep_node",
		"parents": ["default", "sep_node", "non_existent_parent"]
	})";
	raw_err = nullptr;
	rv = lotman_rm_parents_from_lot(removal_JSON1, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	const char *removal_JSON2 = R"({
		"lot_name": "sep_node",
		"parents": ["default"]
	})";
	raw_err = nullptr;
	rv = lotman_rm_parents_from_lot(removal_JSON2, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Test adding paths to a few lots, then remove
	const char *addition_JSON4 = R"({
		"lot_name": "sep_node",
		"paths": [{"path": "/here/is/a/path", "recursive": true}]
	})";
	raw_err = nullptr;
	rv = lotman_add_to_lot(addition_JSON4, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	const char *addition_JSON5 = R"({
		"lot_name": "lot1",
		"paths": [{"path": "/here/is/another/path", "recursive": true}]
	})";
	raw_err = nullptr;
	rv = lotman_add_to_lot(addition_JSON5, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	const char *removal_JSON3 = R"({
		"paths": ["/here/is/a/path", "/path/does/not/exist", "/here/is/another/path"]
	})";
	raw_err = nullptr;
	rv = lotman_rm_paths_from_lots(removal_JSON3, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
}

TEST_F(LotManTest, RemovePathsAdvancedTest) {
	setupStandardHierarchy();
	char *raw_err = nullptr;

	// Add multiple paths to lot1 (owned by owner1)
	const char *add_paths = R"({
		"lot_name": "lot1",
		"paths": [
			{"path": "/test/path1", "recursive": false},
			{"path": "/test/path2", "recursive": true},
			{"path": "/test/path3", "recursive": false}
		]
	})";
	int rv = lotman_add_to_lot(add_paths, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify paths were added (lot1 originally has 2 paths)
	char *raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs("lot1", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json paths_before = json::parse(output.get());
	ASSERT_EQ(paths_before.size(), 5); // 2 original + 3 new

	// Remove multiple paths from same lot
	const char *remove_multiple = R"({
		"paths": ["/test/path1", "/test/path3"]
	})";
	raw_err = nullptr;
	rv = lotman_rm_paths_from_lots(remove_multiple, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify paths were removed
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs("lot1", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json paths_after = json::parse(output.get());
	ASSERT_EQ(paths_after.size(), 3); // 2 original + 1 remaining (/test/path2)

	// Verify the correct path remains
	bool found_path2 = false;
	bool found_path1 = false;
	bool found_path3 = false;
	for (const auto &path_obj : paths_after) {
		std::string path = path_obj["path"];
		if (path == "/test/path2/") {
			found_path2 = true;
		}
		if (path == "/test/path1/") {
			found_path1 = true;
		}
		if (path == "/test/path3/") {
			found_path3 = true;
		}
	}
	ASSERT_TRUE(found_path2);
	ASSERT_FALSE(found_path1);
	ASSERT_FALSE(found_path3);

	// Try to remove path from a lot we don't own (should fail)
	// First, change context to different owner
	raw_err = nullptr;
	rv = lotman_set_context_str("caller", "not_owner1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	const char *unauthorized_remove = R"({
		"paths": ["/test/path2"]
	})";
	raw_err = nullptr;
	rv = lotman_rm_paths_from_lots(unauthorized_remove, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0); // Should fail due to context check

	// Restore context
	raw_err = nullptr;
	rv = lotman_set_context_str("caller", "owner1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify path was NOT removed (still 3 paths)
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs("lot1", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json paths_final = json::parse(output.get());
	ASSERT_EQ(paths_final.size(), 3);

	// Test with duplicate paths in removal list (should succeed, removing path once)
	const char *duplicate_remove = R"({
		"paths": ["/test/path2", "/test/path2"]
	})";
	raw_err = nullptr;
	rv = lotman_rm_paths_from_lots(duplicate_remove, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify path2 is now gone
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs("lot1", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json paths_dup_test = json::parse(output.get());
	ASSERT_EQ(paths_dup_test.size(), 2); // Back to original 2 paths
}

TEST_F(LotManTest, SetGetUsageTest) {
	// Set up fresh database with full hierarchy (already includes default lot and lot5)
	setupFullHierarchy();

	char *raw_err = nullptr;
	char *raw_output = nullptr;

	// Update/Get usage for a lot that doesn't exist
	bool deltaMode = false;
	const char *bad_usage_update_JSON = R"({
		"lot_name": "non_existent_lot",
		"self_GB": 10.5,
		"self_objects": 4,
		"self_GB_being_written": 2.2,
		"self_objects_being_written": 2
	})";
	int rv = lotman_update_lot_usage(bad_usage_update_JSON, deltaMode, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	const char *bad_usage_query_JSON = R"({
		"lot_name": "non_existent_lot",
		"dedicated_GB": true,
		"opportunistic_GB": true,
		"total_GB": true
	})";
	raw_err = nullptr;
	rv = lotman_get_lot_usage(bad_usage_query_JSON, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	// Update by lot
	const char *usage1_update_JSON = R"({
		"lot_name": "lot4",
		"self_GB": 10.5,
		"self_objects": 4,
		"self_GB_being_written": 2.2,
		"self_objects_being_written": 2
	})";
	raw_err = nullptr;
	rv = lotman_update_lot_usage(usage1_update_JSON, deltaMode, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	const char *usage2_update_JSON = R"({
		"lot_name": "lot5",
		"self_GB": 3.5,
		"self_objects": 7,
		"self_GB_being_written": 1.2,
		"self_objects_being_written": 5
	})";
	raw_err = nullptr;
	rv = lotman_update_lot_usage(usage2_update_JSON, deltaMode, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	const char *usage_query_JSON = R"({
		"lot_name": "lot5",
		"dedicated_GB": true,
		"opportunistic_GB": true,
		"total_GB": true
	})";
	raw_err = nullptr;
	rv = lotman_get_lot_usage(usage_query_JSON, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json json_out = json::parse(output.get());
	ASSERT_TRUE(json_out["dedicated_GB"]["children_contrib"] == 6.5 &&
				json_out["dedicated_GB"]["self_contrib"] == 3.5 && json_out["dedicated_GB"]["total"] == 10 &&
				json_out["opportunistic_GB"]["children_contrib"] == 3.5 &&
				json_out["opportunistic_GB"]["self_contrib"] == 0 && json_out["opportunistic_GB"]["total"] == 3.5 &&
				json_out["total_GB"]["children_contrib"] == 10.5 && json_out["total_GB"]["self_contrib"] == 3.5 &&
				json_out["total_GB"]["total"] == 14);

	// Update by dir -- This ends up updating default, lot1 and lot4
	const char *update_JSON_str = R"([
		{
			"includes_subdirs": true,
			"num_obj": 40,
			"path": "/1/2/3",
			"size_GB": 5.12,
			"subdirs": [
				{
					"includes_subdirs": true,
					"num_obj": 6,
					"path": "4",
					"size_GB": 3.14,
					"subdirs": [
						{"includes_subdirs": false, "num_obj": 0, "path": "5", "size_GB": 1.6, "subdirs": []}
					]
				},
				{"includes_subdirs": false, "num_obj": 0, "path": "5/6", "size_GB": 0.5, "subdirs": []},
				{"includes_subdirs": false, "num_obj": 0, "path": "6", "size_GB": 0.25, "subdirs": []}
			]
		},
		{
			"includes_subdirs": true,
			"num_obj": 6,
			"path": "foo/bar",
			"size_GB": 9.153,
			"subdirs": [
				{
					"includes_subdirs": true,
					"num_obj": 0,
					"path": "baz",
					"size_GB": 5.35,
					"subdirs": [
						{"includes_subdirs": false, "num_obj": 0, "path": "more_more_files", "size_GB": 2.2, "subdirs": []}
					]
				}
			]
		}
	])";
	raw_err = nullptr;
	rv = lotman_update_lot_usage_by_dir(update_JSON_str, deltaMode, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Check output for lot1
	const char *lot1_usage_query = R"({
		"lot_name": "lot1",
		"total_GB": false,
		"num_objects": false
	})";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(lot1_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString lot1_output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json lot1_json_out = json::parse(lot1_output.get());
	ASSERT_TRUE(lot1_json_out["total_GB"]["self_contrib"] == 10.383 &&
				lot1_json_out["num_objects"]["self_contrib"] == 40);

	// Check output for lot4
	const char *lot4_usage_query = R"({
		"lot_name": "lot4",
		"total_GB": false,
		"num_objects": false
	})";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(lot4_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString lot4_output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json lot4_json_out = json::parse(lot4_output.get());
	ASSERT_TRUE(lot4_json_out["total_GB"]["self_contrib"] == 3.14 && lot4_json_out["num_objects"]["self_contrib"] == 6);

	// Check output for default
	const char *default_usage_query = R"({
		"lot_name": "default",
		"total_GB": false,
		"num_objects": false
	})";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(default_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString default_output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json default_json_out = json::parse(default_output.get());
	ASSERT_TRUE(default_json_out["total_GB"]["self_contrib"] == 0.75 &&
				default_json_out["num_objects"]["self_contrib"] == 0);

	// Update by dir in delta mode -- This ends up updating lot4
	deltaMode = true;
	const char *update2_JSON_str = R"([{
		"includes_subdirs": false,
		"num_obj": -3,
		"path": "/1/2/3/4",
		"size_GB": 2,
		"subdirs": []
	}])";
	raw_err = nullptr;
	rv = lotman_update_lot_usage_by_dir(update2_JSON_str, deltaMode, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Check output for lot4
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_usage(lot4_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	lot4_output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	lot4_json_out = json::parse(lot4_output.get());
	ASSERT_TRUE(lot4_json_out["total_GB"]["self_contrib"] == 5.14 && lot4_json_out["num_objects"]["self_contrib"] == 3)
		<< lot4_json_out.dump();

	// Update by dir in delta mode, but attempt to make self_gb negative (should fail)
	const char *update3_JSON_str = R"([{
		"includes_subdirs": false,
		"num_obj": 0,
		"path": "/1/2/3/4",
		"size_GB": -10,
		"subdirs": []
	}])";
	raw_err = nullptr;
	rv = lotman_update_lot_usage_by_dir(update3_JSON_str, deltaMode, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0) << err_msg.get();
}

TEST_F(LotManTest, GetOwnersTest) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Modify lot3 owner to "not owner1" for testing recursive owner lookup
	char *raw_err = nullptr;
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"owner": "not owner1"
	})";
	int rv = lotman_update_lot(modified_lot, &raw_err);
	UniqueCString mod_err(raw_err);
	ASSERT_EQ(rv, 0) << mod_err.get();

	// Try with a lot that doesn't exist
	const char *non_existent_lot = "non_existent_lot";
	const bool recursive = true;
	char **raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_owners(non_existent_lot, recursive, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	const char *lot_name = "lot4";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_owners(lot_name, recursive, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0);
	for (int iter = 0; output.get()[iter]; iter++) {
		ASSERT_TRUE(static_cast<std::string>(output.get()[iter]) == "owner1" ||
					static_cast<std::string>(output.get()[iter]) == "not owner1");
	}
}

TEST_F(LotManTest, GetParentsTest) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Modify lot3 to have lot2 as parent (instead of self) and add sep_node as parent
	char *raw_err = nullptr;
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"parents": [{"current": "lot3", "new": "lot2"}]
	})";
	int rv = lotman_update_lot(modified_lot, &raw_err);
	UniqueCString mod_err(raw_err);
	ASSERT_EQ(rv, 0) << mod_err.get();

	raw_err = nullptr;
	const char *addition_JSON = R"({
		"lot_name": "lot3",
		"parents": ["sep_node"]
	})";
	rv = lotman_add_to_lot(addition_JSON, &raw_err);
	mod_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << mod_err.get();

	char **raw_output = nullptr;
	raw_err = nullptr;
	const char *lot_name = "lot4";
	const bool recursive = true;
	const bool get_self = true;
	rv = lotman_get_parent_names(lot_name, recursive, get_self, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0);
	for (int iter = 0; output.get()[iter]; iter++) {
		ASSERT_TRUE(static_cast<std::string>(output.get()[iter]) == "lot1" ||
					static_cast<std::string>(output.get()[iter]) == "lot2" ||
					static_cast<std::string>(output.get()[iter]) == "lot3" ||
					static_cast<std::string>(output.get()[iter]) == "lot5" ||
					static_cast<std::string>(output.get()[iter]) == "sep_node");
	}
}

TEST_F(LotManTest, GetChildrenTest) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Modify lot3 to have lot2 as parent (instead of self) so lot1 has lot3 as descendant
	char *raw_err = nullptr;
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"parents": [{"current": "lot3", "new": "lot2"}]
	})";
	int rv = lotman_update_lot(modified_lot, &raw_err);
	UniqueCString mod_err(raw_err);
	ASSERT_EQ(rv, 0) << mod_err.get();

	// Try with a lot that doesn't exist
	const char *non_existent_lot = "non_existent_lot";
	const bool recursive = true;
	const bool get_self = false;
	char **raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_children_names(non_existent_lot, recursive, get_self, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	// Now test that checks for good output
	const char *lot_name = "lot1";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_children_names(lot_name, recursive, get_self, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0);

	for (int iter = 0; output.get()[iter]; iter++) {
		ASSERT_TRUE(static_cast<std::string>(output.get()[iter]) == "lot2" ||
					static_cast<std::string>(output.get()[iter]) == "lot3" ||
					static_cast<std::string>(output.get()[iter]) == "lot4" ||
					static_cast<std::string>(output.get()[iter]) == "lot5");
	}
}

TEST_F(LotManTest, GetPolicyAttrs) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Modify lot3 to have lot2 as parent (instead of self) and add sep_node as parent
	// This matches the hierarchy state expected by this test
	char *raw_err = nullptr;
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"parents": [{"current": "lot3", "new": "lot2"}]
	})";
	int rv = lotman_update_lot(modified_lot, &raw_err);
	UniqueCString mod_err(raw_err);
	ASSERT_EQ(rv, 0) << mod_err.get();

	raw_err = nullptr;
	const char *addition_JSON = R"({
		"lot_name": "lot3",
		"parents": ["sep_node"]
	})";
	rv = lotman_add_to_lot(addition_JSON, &raw_err);
	mod_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << mod_err.get();

	// Try to get policy attributes for a lot that doesn't exist
	const char *bad_policy_attrs_JSON = R"({
		"lot_name": "non_existent_lot",
		"dedicated_GB": true,
		"opportunistic_GB": true,
		"max_num_objects": true,
		"creation_time": true,
		"expiration_time": true,
		"deletion_time": true
	})";
	char *raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_policy_attributes(bad_policy_attrs_JSON, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	// Try with a key that doesn't exist
	const char *bad_policy_attrs_JSON2 = R"({
		"lot_name": "lot4",
		"bad_key": true,
		"opportunistic_GB": true,
		"max_num_objects": true,
		"creation_time": true,
		"expiration_time": true,
		"deletion_time": true,
		"non_existent_key": true
	})";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_policy_attributes(bad_policy_attrs_JSON2, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	const char *policy_attrs_JSON_str = R"({
		"lot_name": "lot4",
		"dedicated_GB": true,
		"opportunistic_GB": true,
		"max_num_objects": true,
		"creation_time": true,
		"expiration_time": true,
		"deletion_time": true
	})";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_policy_attributes(policy_attrs_JSON_str, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json json_out = json::parse(output.get());
	ASSERT_TRUE(json_out["creation_time"]["lot_name"] == "lot5" && json_out["creation_time"]["value"] == 100 &&
				json_out["dedicated_GB"]["lot_name"] == "lot4" && json_out["dedicated_GB"]["value"] == 3.0 &&
				json_out["deletion_time"]["lot_name"] == "lot5" && json_out["deletion_time"]["value"] == 300 &&
				json_out["expiration_time"]["lot_name"] == "lot5" && json_out["expiration_time"]["value"] == 200 &&
				json_out["max_num_objects"]["lot_name"] == "sep_node" && json_out["max_num_objects"]["value"] == 10 &&
				json_out["opportunistic_GB"]["lot_name"] == "lot2" && json_out["opportunistic_GB"]["value"] == 1.5);
}

TEST_F(LotManTest, GetLotDirs) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Try to get a lot that doesn't exist
	const char *non_existent_lot = "non_existent_lot";
	const bool recursive = true;
	char *raw_output = nullptr;
	char *raw_err = nullptr;
	int rv = lotman_get_lot_dirs(non_existent_lot, recursive, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	const char *lot_name = "lot5";
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_dirs(lot_name, recursive, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0);

	json json_out = json::parse(output.get());
	for (const auto &path_obj : json_out) {
		if (path_obj["path"] == "/1/2/3/4/" && path_obj["recursive"] == true && path_obj["lot_name"] == "lot4") {
			continue;
		} else if (path_obj["path"] == "/345/" && path_obj["recursive"] == true && path_obj["lot_name"] == "lot4") {
			continue;
		} else if (path_obj["path"] == "/456/" && path_obj["recursive"] == false && path_obj["lot_name"] == "lot5") {
			continue;
		} else if (path_obj["path"] == "/567/" && path_obj["recursive"] == true && path_obj["lot_name"] == "lot5") {
			continue;
		} else {
			ASSERT_TRUE(false) << "Unexpected path object: " << output.get();
		}
	}
}

TEST_F(LotManTest, ContextTest) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Any actions that modify the properties of a lot must have proper auth
	// These tests should all fail (context set to nonexistent owner)

	char *raw_err = nullptr;
	const char *lot6 = R"({
		"lot_name": "lot6",
		"owner": "owner1",
		"parents": ["lot5"],
		"paths": [],
		"management_policy_attrs": {
			"dedicated_GB": 3,
			"opportunistic_GB": 2.1,
			"max_num_objects": 40,
			"creation_time": 123,
			"expiration_time": 231,
			"deletion_time": 315
		}
	})";

	// Try to add a lot without correct context
	auto rv = lotman_set_context_str("caller", "notAnOwner", &raw_err);
	UniqueCString err_msg(raw_err);

	raw_err = nullptr;
	rv = lotman_add_lot(lot6, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0) << err_msg.get();

	raw_err = nullptr;
	rv = lotman_lot_exists("lot6", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 1);

	// Try to remove a lot without correct context
	raw_err = nullptr;
	rv = lotman_remove_lots_recursive("lot1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	raw_err = nullptr;
	rv = lotman_lot_exists("lot1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 1);

	// Try to update a lot without correct context
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"owner": "Bad Update"
	})";
	raw_err = nullptr;
	rv = lotman_update_lot(modified_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);

	// Try to update lot usage without correct context
	const char *usage_update_JSON = R"({
		"lot_name": "lot5",
		"self_GB": 99
	})";
	raw_err = nullptr;
	rv = lotman_update_lot_usage(usage_update_JSON, false, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_NE(rv, 0);
}

TEST_F(LotManTest, LotsQueryTest) {
	// Set up fresh database with full hierarchy (already includes default lot)
	setupFullHierarchy();

	char *raw_err = nullptr;
	char **raw_output = nullptr;

	// Check for lots past expiration
	auto rv = lotman_get_lots_past_exp(true, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	bool check = false;
	for (int iter = 0; output.get()[iter]; iter++) {
		if (strcmp(output.get()[iter], "sep_node") == 0) {
			check = true;
		}
	}
	ASSERT_FALSE(check);

	// Check for lots past deletion
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_del(true, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output2(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	check = false;
	for (int iter = 0; output2.get()[iter]; iter++) {
		if (strcmp(output2.get()[iter], "sep_node") == 0) {
			check = true;
		}
	}
	ASSERT_FALSE(check);

	// Check for lots past opportunistic storage limit
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_opp(true, true, &raw_output, false, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output3(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	check = false;
	for (int iter = 0; output3.get()[iter]; iter++) {
		if (strcmp(output3.get()[iter], "default") == 0) {
			check = true;
		}
	}
	ASSERT_FALSE(check);

	// Check for lots past dedicated storage limit
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_ded(true, true, &raw_output, false, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output4(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	check = false;
	for (int iter = 0; output4.get()[iter]; iter++) {
		if (strcmp(output4.get()[iter], "default") == 0) {
			check = true;
		}
	}
	ASSERT_FALSE(check);

	// Check for lots past object storage limit
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lots_past_obj(true, true, &raw_output, false, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output5(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	check = false;
	for (int iter = 0; output5.get()[iter]; iter++) {
		if (strcmp(output5.get()[iter], "default") == 0) {
			check = true;
		}
	}
	ASSERT_FALSE(check);
}

TEST_F(LotManTest, GetAllLotsTest) {
	// Set up fresh database with full hierarchy (7 lots: default, lot1-5, sep_node)
	setupFullHierarchy();

	char *raw_err = nullptr;
	char **raw_output = nullptr;
	auto rv = lotman_list_all_lots(&raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	UniqueStringList output(raw_output);
	int size = 0;
	for (size; output.get()[size]; ++size) {}
	ASSERT_EQ(size, 7);
}

TEST_F(LotManTest, GetLotJSONTest) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Set up the exact state expected by this test:
	// 1. Modify lot3: owner -> "not owner1", parent lot3 -> lot2, add sep_node as parent
	// 2. Update paths on lot3
	// 3. Add usage data to lot4 and lot5
	char *raw_err = nullptr;

	// Modify lot3
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"owner": "not owner1",
		"parents": [{"current": "lot3", "new": "lot2"}],
		"paths": [
			{"current": "/another/path", "new": "/another/path", "recursive": true},
			{"current": "/123", "new": "/updated/path", "recursive": false}
		],
		"management_policy_attrs": {
			"dedicated_GB": 10.111,
			"opportunistic_GB": 6.6,
			"max_num_objects": 50,
			"expiration_time": 222,
			"deletion_time": 333
		}
	})";
	int rv = lotman_update_lot(modified_lot, &raw_err);
	UniqueCString setup_err(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	// Add sep_node as parent and /foo/barr path to lot3
	const char *addition_JSON = R"({
		"lot_name": "lot3",
		"paths": [{"path": "/foo/barr", "recursive": true}],
		"parents": ["sep_node"]
	})";
	raw_err = nullptr;
	rv = lotman_add_to_lot(addition_JSON, &raw_err);
	setup_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	// Add usage data to lot4 and lot5
	const char *usage1_update_JSON = R"({
		"lot_name": "lot4",
		"self_GB": 10.5,
		"self_objects": 4,
		"self_GB_being_written": 2.2,
		"self_objects_being_written": 2
	})";
	raw_err = nullptr;
	rv = lotman_update_lot_usage(usage1_update_JSON, false, &raw_err);
	setup_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	const char *usage2_update_JSON = R"({
		"lot_name": "lot5",
		"self_GB": 3.5,
		"self_objects": 7,
		"self_GB_being_written": 1.2,
		"self_objects_being_written": 5
	})";
	raw_err = nullptr;
	rv = lotman_update_lot_usage(usage2_update_JSON, false, &raw_err);
	setup_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	// Update usage by dir to set lot4 usage to specific values
	const char *update_JSON_str = R"([{
		"includes_subdirs": true,
		"num_obj": 6,
		"path": "/1/2/3/4",
		"size_GB": 3.14,
		"subdirs": []
	}])";
	raw_err = nullptr;
	rv = lotman_update_lot_usage_by_dir(update_JSON_str, false, 150, &raw_err);
	setup_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	// Delta update to get exact values expected
	const char *update2_JSON_str = R"([{
		"includes_subdirs": false,
		"num_obj": -3,
		"path": "/1/2/3/4",
		"size_GB": 2,
		"subdirs": []
	}])";
	raw_err = nullptr;
	rv = lotman_update_lot_usage_by_dir(update2_JSON_str, true, 150, &raw_err);
	setup_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	// Try to get a lot that doesn't exist
	char *raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_as_json("non_existent_lot", true, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_NE(rv, 0);

	// Non-recursive test
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_as_json("lot3", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json output_JSON = json::parse(output.get());
	json expected_output = R"({
		"children": ["lot5"],
		"lot_name": "lot3",
		"management_policy_attrs": {
			"creation_time": 123.0,
			"dedicated_GB": 10.111,
			"deletion_time": 333.0,
			"expiration_time": 222.0,
			"max_num_objects": 50.0,
			"opportunistic_GB": 6.6
		},
		"owner": "not owner1",
		"parents": [
			"lot2",
			"sep_node"
		],
		"paths": [
			{
				"exclude": false,
				"lot_name": "lot3",
				"path": "/another/path/",
				"recursive": true
			},
			{
				"exclude": false,
				"lot_name": "lot3",
				"path": "/foo/barr/",
				"recursive": true
			},
			{
				"exclude": false,
				"lot_name": "lot3",
				"path": "/updated/path/",
				"recursive": false
			}
		],
		"usage": {
			"GB_being_written": { "self_contrib": 0.0 },
			"dedicated_GB": { "self_contrib": 0.0 },
			"num_objects": { "self_contrib": 0.0 },
			"objects_being_written": { "self_contrib": 0.0 },
			"opportunistic_GB": { "self_contrib": 0.0 },
			"total_GB": { "self_contrib": 0.0 }
		}
	})"_json;
	ASSERT_EQ(output_JSON, expected_output) << output_JSON;

	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_lot_as_json("lot3", true, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output2(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json output_JSON2 = json::parse(output2.get());
	json expected_output2 = R"({
		"children": [
			"lot4",
			"lot5"
		],
		"lot_name": "lot3",
		"management_policy_attrs": {
			"creation_time": 123.0,
			"dedicated_GB": 10.111,
			"deletion_time": 333.0,
			"expiration_time": 222.0,
			"max_num_objects": 50.0,
			"opportunistic_GB": 6.6
		},
		"owners": [
			"not owner1",
			"owner1"
		],
		"parents": [
			"lot1",
			"lot2",
			"sep_node"
		],
		"paths": [
			{
				"exclude": false,
				"lot_name": "lot3",
				"path": "/another/path/",
				"recursive": true
			},
			{
				"exclude": false,
				"lot_name": "lot3",
				"path": "/foo/barr/",
				"recursive": true
			},
			{
				"exclude": false,
				"lot_name": "lot3",
				"path": "/updated/path/",
				"recursive": false
			},
			{
				"exclude": false,
				"lot_name": "lot4",
				"path": "/1/2/3/4/",
				"recursive": true
			},
			{
				"exclude": false,
				"lot_name": "lot4",
				"path": "/345/",
				"recursive": true
			},
			{
				"exclude": false,
				"lot_name": "lot5",
				"path": "/456/",
				"recursive": false
			},
			{
				"exclude": false,
				"lot_name": "lot5",
				"path": "/567/",
				"recursive": true
			}
		],
		"restrictive_management_policy_attrs": {
			"creation_time": {
				"lot_name": "lot3",
				"value": 123.0
			},
			"dedicated_GB": {
				"lot_name": "sep_node",
				"value": 3.0
			},
			"deletion_time": {
				"lot_name": "lot3",
				"value": 333.0
			},
			"expiration_time": {
				"lot_name": "lot3",
				"value": 222.0
			},
			"max_num_objects": {
				"lot_name": "sep_node",
				"value": 10.0
			},
			"opportunistic_GB": {
				"lot_name": "lot2",
				"value": 1.5
			}
		},
		"usage": {
			"GB_being_written": {
				"children_contrib": 3.4,
				"self_contrib": 0.0,
				"total": 3.4
			},
			"dedicated_GB": {
				"children_contrib": 8.64,
				"self_contrib": 0.0,
				"total": 8.64
			},
			"num_objects": {
				"children_contrib": 10.0,
				"self_contrib": 0.0,
				"total": 10.0
			},
			"objects_being_written": {
				"children_contrib": 7.0,
				"self_contrib": 0.0,
				"total": 7.0
			},
			"opportunistic_GB": {
				"children_contrib": 0.0,
				"self_contrib": 0.0,
				"total": 0.0
			},
			"total_GB": {
				"children_contrib": 8.64,
				"self_contrib": 0.0,
				"total": 8.64
			}
		}
	})"_json;
	ASSERT_EQ(output_JSON2, expected_output2) << output_JSON2;
}

TEST_F(LotManTest, LotsFromDirTest) {
	// Set up fresh database with full hierarchy
	setupFullHierarchy();

	// Modify lot3 to have lot2 as parent and add sep_node, plus add /foo/barr path
	char *raw_err = nullptr;
	const char *modified_lot = R"({
		"lot_name": "lot3",
		"parents": [{"current": "lot3", "new": "lot2"}]
	})";
	int rv = lotman_update_lot(modified_lot, &raw_err);
	UniqueCString setup_err(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	raw_err = nullptr;
	const char *addition_JSON = R"({
		"lot_name": "lot3",
		"paths": [{"path": "/foo/barr", "recursive": true}],
		"parents": ["sep_node"]
	})";
	rv = lotman_add_to_lot(addition_JSON, &raw_err);
	setup_err.reset(raw_err);
	ASSERT_EQ(rv, 0) << setup_err.get();

	char **raw_output = nullptr;
	raw_err = nullptr;
	const char *dir = "/1/2/3/4"; // Get a path
	rv = lotman_get_lots_from_dir(dir, true, 150, &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	UniqueStringList output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	for (int iter = 0; output.get()[iter]; iter++) {
		ASSERT_TRUE(static_cast<std::string>(output.get()[iter]) == "lot4" ||
					static_cast<std::string>(output.get()[iter]) == "lot1" ||
					static_cast<std::string>(output.get()[iter]) == "lot2" ||
					static_cast<std::string>(output.get()[iter]) == "lot3" ||
					static_cast<std::string>(output.get()[iter]) == "lot5" ||
					static_cast<std::string>(output.get()[iter]) == "sep_node");
	}

	raw_output = nullptr;
	raw_err = nullptr;
	const char *dir2 = "/foo/barr"; // Make sure parsing doesn't grab lot associated with /foo/bar
	rv = lotman_get_lots_from_dir(dir2, false, 150, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueStringList output2(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	ASSERT_EQ(strcmp(output2.get()[0], "lot3"), 0);
}

TEST_F(LotManTest, GetVersionTest) {
	const char *version = lotman_version();
	std::string version_cpp(version);

	EXPECT_EQ(version_cpp, "v0.0.1");
}

TEST_F(LotManTest, IsRootTest) {
	// Set up a hierarchy: default (root), lot1 (root), lot2 (child of lot1), lot3 (root), lot4 (child)
	setupStandardHierarchy();

	char *raw_err = nullptr;

	// Test with non-existent lot
	auto rv = lotman_is_root("non_existent_lot", &raw_err);
	UniqueCString err_msg(raw_err);
	EXPECT_EQ(rv, -1) << "Expected error for non-existent lot";

	// "default" should be a root lot (self-parent)
	raw_err = nullptr;
	rv = lotman_is_root("default", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_GE(rv, 0) << err_msg.get();
	EXPECT_EQ(rv, 1) << "default should be a root lot";

	// "lot1" IS a root lot (self-parent)
	raw_err = nullptr;
	rv = lotman_is_root("lot1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_GE(rv, 0) << err_msg.get();
	EXPECT_EQ(rv, 1) << "lot1 should be a root lot (self-parent)";

	// "lot2" should NOT be a root (has lot1 as parent)
	raw_err = nullptr;
	rv = lotman_is_root("lot2", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_GE(rv, 0) << err_msg.get();
	EXPECT_EQ(rv, 0) << "lot2 should not be a root lot";

	// "lot3" IS a root lot (self-parent)
	raw_err = nullptr;
	rv = lotman_is_root("lot3", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_GE(rv, 0) << err_msg.get();
	EXPECT_EQ(rv, 1) << "lot3 should be a root lot (self-parent)";

	// "lot4" should NOT be a root (has lot2 and lot3 as parents)
	raw_err = nullptr;
	rv = lotman_is_root("lot4", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_GE(rv, 0) << err_msg.get();
	EXPECT_EQ(rv, 0) << "lot4 should not be a root lot";
}

TEST_F(LotManTest, ContextStrTest) {
	char *raw_err = nullptr;
	char *raw_output = nullptr;

	// Test getting lot_home that was set in SetUp
	auto rv = lotman_get_context_str("lot_home", &raw_output, &raw_err);
	UniqueCString err_msg(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(output.get(), nullptr);
	// The lot_home should contain our temp directory path
	std::string lot_home(output.get());
	EXPECT_FALSE(lot_home.empty());

	// Test getting caller that was set in SetUp
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_context_str("caller", &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(output.get(), nullptr);
	EXPECT_STREQ(output.get(), "owner1");

	// Test setting and getting a new caller
	raw_err = nullptr;
	rv = lotman_set_context_str("caller", "new_owner", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_context_str("caller", &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(output.get(), nullptr);
	EXPECT_STREQ(output.get(), "new_owner");

	// Reset caller back to owner1 for any subsequent operations
	raw_err = nullptr;
	rv = lotman_set_context_str("caller", "owner1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Test with invalid context key
	raw_output = nullptr;
	raw_err = nullptr;
	rv = lotman_get_context_str("invalid_key", &raw_output, &raw_err);
	err_msg.reset(raw_err);
	EXPECT_EQ(rv, -1) << "Expected error for invalid context key";
}

TEST_F(LotManTest, ContextIntTest) {
	char *raw_err = nullptr;
	int output = 0;

	// Test setting db_timeout
	auto rv = lotman_set_context_int("db_timeout", 5000, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Test getting db_timeout
	raw_err = nullptr;
	rv = lotman_get_context_int("db_timeout", &output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	EXPECT_EQ(output, 5000);

	// Test setting a different value
	raw_err = nullptr;
	rv = lotman_set_context_int("db_timeout", 10000, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	raw_err = nullptr;
	rv = lotman_get_context_int("db_timeout", &output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	EXPECT_EQ(output, 10000);

	// Test with invalid context key
	raw_err = nullptr;
	rv = lotman_set_context_int("invalid_key", 100, &raw_err);
	err_msg.reset(raw_err);
	EXPECT_EQ(rv, -1) << "Expected error for invalid context key";

	raw_err = nullptr;
	rv = lotman_get_context_int("invalid_key", &output, &raw_err);
	err_msg.reset(raw_err);
	EXPECT_EQ(rv, -1) << "Expected error for invalid context key";
}

TEST_F(LotManTest, PathTrailingSlashNormalizationTest) {
	// This test verifies that paths input without trailing slashes are:
	// 1. Stored with trailing slashes in the database
	// 2. Can be looked up both with and without trailing slashes
	// 3. Are returned with trailing slashes in API output

	char *raw_err = nullptr;
	int rv = lotman_set_context_str("lot_home", tmp_dir.c_str(), &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	rv = lotman_set_context_str("caller", "owner1", &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Create default lot (required)
	const char *default_lot = R"({
		"lot_name": "default",
		"owner": "owner1",
		"parents": ["default"],
		"paths": [{"path": "/default", "recursive": true}],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 1000,
			"creation_time": 1700000000,
			"expiration_time": 1800000000,
			"deletion_time": 1900000000
		}
	})";
	rv = lotman_add_lot(default_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Create a lot with paths that do NOT have trailing slashes
	const char *test_lot = R"({
		"lot_name": "slash_test_lot",
		"owner": "owner1",
		"parents": ["slash_test_lot"],
		"paths": [
			{"path": "/no/slash/here", "recursive": true},
			{"path": "/another/path/no/slash", "recursive": false}
		],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 1700000000,
			"expiration_time": 1800000000,
			"deletion_time": 1900000000
		}
	})";
	rv = lotman_add_lot(test_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify paths are returned WITH trailing slashes
	char *raw_output = nullptr;
	rv = lotman_get_lot_dirs("slash_test_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json paths_json = json::parse(output.get());
	ASSERT_EQ(paths_json.size(), 2);

	bool found_path1 = false;
	bool found_path2 = false;
	for (const auto &path_obj : paths_json) {
		std::string path = path_obj["path"];
		// All paths should end with /
		EXPECT_EQ(path.back(), '/') << "Path '" << path << "' should end with trailing slash";

		if (path == "/no/slash/here/") {
			found_path1 = true;
		}
		if (path == "/another/path/no/slash/") {
			found_path2 = true;
		}
	}
	EXPECT_TRUE(found_path1) << "Expected normalized path '/no/slash/here/'";
	EXPECT_TRUE(found_path2) << "Expected normalized path '/another/path/no/slash/'";

	// Verify lookup works WITHOUT trailing slash
	char **lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/no/slash/here", false, 1750000000, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "slash_test_lot");
	lotman_free_string_list(lots_output);

	// Verify lookup works WITH trailing slash
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/no/slash/here/", false, 1750000000, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "slash_test_lot");
	lotman_free_string_list(lots_output);

	// Verify subdirectory lookup works (for recursive path)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/no/slash/here/subdir/deeper", false, 1750000000, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "slash_test_lot");
	lotman_free_string_list(lots_output);

	// Verify path removal works without trailing slash
	const char *remove_path = R"({
		"paths": ["/another/path/no/slash"]
	})";
	rv = lotman_rm_paths_from_lots(remove_path, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Verify only one path remains
	raw_output = nullptr;
	rv = lotman_get_lot_dirs("slash_test_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	paths_json = json::parse(output.get());
	ASSERT_EQ(paths_json.size(), 1);
	EXPECT_EQ(paths_json[0]["path"], "/no/slash/here/");
}

TEST_F(LotManTest, PathExclusionTest) {
	// Test for path exclusion feature (GitHub issue #24)
	// This tests that a lot can have paths explicitly excluded from recursive tracking
	addDefaultLot();
	addLot1(); // lot1 is a self-parenting root lot with owner1

	// Create a lot with a recursive path and excluded subdirectories
	// This tests both recursive and non-recursive exclusions:
	// - /data/storage/cache is excluded recursively (subdirs also excluded)
	// - /data/storage/stuff is excluded non-recursively (subdirs are included)
	const char *lot_with_exclusions = R"({
		"lot_name": "exclusion_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/data/storage", "recursive": true, "exclude": false},
			{"path": "/data/storage/cache", "recursive": true, "exclude": true},
			{"path": "/data/storage/stuff", "recursive": false, "exclude": true},
			{"path": "/data/logs", "recursive": false}
		],
		"management_policy_attrs": {
			"dedicated_GB": 10,
			"opportunistic_GB": 5,
			"max_num_objects": 100,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";

	char *raw_err = nullptr;
	int rv = lotman_add_lot(lot_with_exclusions, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add lot with exclusions: " << err_msg.get();

	// Test get_lot_dirs returns all paths with exclude flags
	char *raw_output = nullptr;
	rv = lotman_get_lot_dirs("exclusion_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json paths_json = json::parse(output.get());
	ASSERT_EQ(paths_json.size(), 4);

	// Verify paths and their exclude flags
	bool found_storage = false, found_cache = false, found_stuff = false, found_logs = false;
	for (const auto &path_entry : paths_json) {
		std::string path = path_entry["path"];
		bool exclude = path_entry["exclude"];
		if (path == "/data/storage/") {
			found_storage = true;
			EXPECT_FALSE(exclude) << "Storage path should not be excluded";
			EXPECT_TRUE(path_entry["recursive"]);
		} else if (path == "/data/storage/cache/") {
			found_cache = true;
			EXPECT_TRUE(exclude) << "Cache path should be excluded";
			EXPECT_TRUE(path_entry["recursive"]) << "Cache exclusion should be recursive";
		} else if (path == "/data/storage/stuff/") {
			found_stuff = true;
			EXPECT_TRUE(exclude) << "Stuff path should be excluded";
			EXPECT_FALSE(path_entry["recursive"]) << "Stuff exclusion should NOT be recursive";
		} else if (path == "/data/logs/") {
			found_logs = true;
			EXPECT_FALSE(exclude) << "Logs path should not be excluded";
			EXPECT_FALSE(path_entry["recursive"]);
		}
	}
	EXPECT_TRUE(found_storage) << "Expected /data/storage/ path";
	EXPECT_TRUE(found_cache) << "Expected /data/storage/cache/ path";
	EXPECT_TRUE(found_stuff) << "Expected /data/storage/stuff/ path";
	EXPECT_TRUE(found_logs) << "Expected /data/logs/ path";

	// Test get_lots_from_dir: /data/storage should return the lot
	char **lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "exclusion_lot");
	lotman_free_string_list(lots_output);

	// Test get_lots_from_dir: /data/storage/subdir should return the lot (recursive, not excluded)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/subdir", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "exclusion_lot");
	lotman_free_string_list(lots_output);

	// Test get_lots_from_dir: /data/storage/cache itself should NOT return exclusion_lot (excluded)
	// It should return "default" since the path is excluded from exclusion_lot
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/cache", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Cache path should be excluded from exclusion_lot, returning default";
	lotman_free_string_list(lots_output);

	// Test get_lots_from_dir: /data/storage/cache/subdir should NOT return exclusion_lot (excluded recursively)
	// It should return "default" since the subdir is under a RECURSIVE exclusion
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/cache/subdir", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Cache subdir should be excluded (recursive exclusion)";
	lotman_free_string_list(lots_output);

	// Test get_lots_from_dir: /data/storage/cache/deep/nested/path should also be excluded (recursive exclusion)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/cache/deep/nested/path", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Deep cache path should be excluded (recursive exclusion)";
	lotman_free_string_list(lots_output);

	// Test NON-RECURSIVE exclusion: /data/storage/stuff itself should be excluded
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/stuff", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Stuff path should be excluded (non-recursive exclusion)";
	lotman_free_string_list(lots_output);

	// Test NON-RECURSIVE exclusion: /data/storage/stuff/foo should be INCLUDED
	// Because the exclusion is non-recursive, subdirs fall back to the parent inclusion /data/storage
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/stuff/foo", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "exclusion_lot")
		<< "Stuff subdir should be INCLUDED (non-recursive exclusion only excludes exact path)";
	lotman_free_string_list(lots_output);

	// Test NON-RECURSIVE exclusion: /data/storage/stuff/foo/bar/baz should also be INCLUDED
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/storage/stuff/foo/bar/baz", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "exclusion_lot") << "Deep stuff subdir should be INCLUDED";
	lotman_free_string_list(lots_output);

	// Test that default exclude value works (omitting exclude should default to false)
	const char *lot_default_exclude = R"({
		"lot_name": "default_exclude_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/test/path", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2,
			"max_num_objects": 50,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";

	rv = lotman_add_lot(lot_default_exclude, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add lot without explicit exclude: " << err_msg.get();

	raw_output = nullptr;
	rv = lotman_get_lot_dirs("default_exclude_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json default_paths_json = json::parse(output.get());
	ASSERT_EQ(default_paths_json.size(), 1);
	EXPECT_FALSE(default_paths_json[0]["exclude"]) << "Default exclude should be false";

	// Test adding exclusion via lotman_add_to_lot
	const char *add_exclusion = R"({
		"lot_name": "default_exclude_lot",
		"paths": [{"path": "/test/path/excluded", "recursive": true, "exclude": true}]
	})";

	rv = lotman_add_to_lot(add_exclusion, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add excluded path: " << err_msg.get();

	raw_output = nullptr;
	rv = lotman_get_lot_dirs("default_exclude_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json updated_paths_json = json::parse(output.get());
	ASSERT_EQ(updated_paths_json.size(), 2);

	// Verify the added exclusion
	bool found_excluded = false;
	for (const auto &path_entry : updated_paths_json) {
		if (path_entry["path"] == "/test/path/excluded/") {
			found_excluded = true;
			EXPECT_TRUE(path_entry["exclude"]) << "Added path should be excluded";
		}
	}
	EXPECT_TRUE(found_excluded) << "Should find the added excluded path";

	// Test that /test/path/subdir still returns the lot (not excluded)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/test/path/subdir", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default_exclude_lot");
	lotman_free_string_list(lots_output);

	// Test that /test/path/excluded returns "default" (excluded from default_exclude_lot)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/test/path/excluded", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Excluded path should return default lot";
	lotman_free_string_list(lots_output);

	// Test removing an excluded path (should succeed without error)
	const char *remove_exclusion = R"({
		"paths": ["/test/path/excluded"]
	})";
	rv = lotman_rm_paths_from_lots(remove_exclusion, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to remove excluded path: " << err_msg.get();

	// Verify path was removed
	raw_output = nullptr;
	rv = lotman_get_lot_dirs("default_exclude_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json final_paths_json = json::parse(output.get());
	ASSERT_EQ(final_paths_json.size(), 1);
	EXPECT_EQ(final_paths_json[0]["path"], "/test/path/");

	// Edge case: Test exclusion path that is a prefix of another valid path
	// e.g., exclude /data/cache but /data/cachedata should NOT be excluded
	const char *prefix_lot = R"({
		"lot_name": "prefix_test_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/data", "recursive": true, "exclude": false},
			{"path": "/data/cache", "recursive": true, "exclude": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2,
			"max_num_objects": 50,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";

	rv = lotman_add_lot(prefix_lot, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add prefix test lot: " << err_msg.get();

	// /data/cache should be excluded
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/cache", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	EXPECT_STREQ(lots_output[0], "default") << "/data/cache should be excluded";
	lotman_free_string_list(lots_output);

	// /data/cachedata should NOT be excluded (different path, just happens to have "cache" prefix)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/cachedata", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	EXPECT_STREQ(lots_output[0], "prefix_test_lot") << "/data/cachedata should NOT be excluded (different path)";
	lotman_free_string_list(lots_output);

	// /data/cache/subdir should be excluded (under recursive exclusion)
	lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/data/cache/subdir", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	EXPECT_STREQ(lots_output[0], "default") << "/data/cache/subdir should be excluded";
	lotman_free_string_list(lots_output);
}

TEST_F(LotManTest, PathExclusionUpdateTest) {
	// Test updating paths to change exclude flag
	addDefaultLot();
	addLot1(); // lot1 is a self-parenting root lot with owner1

	// Create a lot with a path
	const char *lot_json = R"({
		"lot_name": "update_exclude_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/update/test", "recursive": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 5,
			"opportunistic_GB": 2,
			"max_num_objects": 50,
			"creation_time": 123,
			"expiration_time": 234,
			"deletion_time": 345
		}
	})";

	char *raw_err = nullptr;
	int rv = lotman_add_lot(lot_json, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Update the path to be excluded
	const char *update_json = R"({
		"lot_name": "update_exclude_lot",
		"paths": [
			{"current": "/update/test", "new": "/update/test", "recursive": true, "exclude": true}
		]
	})";

	rv = lotman_update_lot(update_json, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to update path exclusion: " << err_msg.get();

	// Verify the exclude flag was updated
	char *raw_output = nullptr;
	rv = lotman_get_lot_dirs("update_exclude_lot", false, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();

	json paths_json = json::parse(output.get());
	ASSERT_EQ(paths_json.size(), 1);
	EXPECT_TRUE(paths_json[0]["exclude"]) << "Path should now be excluded";

	// Verify get_lots_from_dir respects the update - should return "default" now
	char **lots_output = nullptr;
	rv = lotman_get_lots_from_dir("/update/test", false, 150, &lots_output, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();
	ASSERT_NE(lots_output, nullptr);
	EXPECT_STREQ(lots_output[0], "default") << "Updated excluded path should return default lot";
	lotman_free_string_list(lots_output);
}

TEST_F(LotManTest, UpdateUsageByDirWithExclusionsTest) {
	// This test verifies that lotman_update_lot_usage_by_dir correctly respects
	// path exclusions when attributing usage to lots.
	addDefaultLot();
	addLot1(); // lot1 is a self-parenting root lot with owner1

	// Create a lot with:
	// - /project included recursively
	// - /project/cache excluded recursively (cache dirs shouldn't count)
	// - /project/tmp excluded non-recursively (tmp itself excluded, but tmp/subdir counts)
	const char *lot_with_exclusions = R"({
		"lot_name": "project_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/project", "recursive": true, "exclude": false},
			{"path": "/project/cache", "recursive": true, "exclude": true},
			{"path": "/project/tmp", "recursive": false, "exclude": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 10000,
			"creation_time": 123,
			"expiration_time": 999999999999,
			"deletion_time": 999999999999
		}
	})";

	char *raw_err = nullptr;
	int rv = lotman_add_lot(lot_with_exclusions, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to add lot with exclusions: " << err_msg.get();

	// Verify initial usage is zero
	const char *project_usage_query = R"({
		"lot_name": "project_lot",
		"total_GB": false,
		"num_objects": false
	})";
	char *raw_output = nullptr;
	rv = lotman_get_lot_usage(project_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json usage_json = json::parse(output.get());
	EXPECT_EQ(usage_json["total_GB"]["self_contrib"], 0.0);
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"], 0);

	// Update usage for various directories:
	// - /project/src (10 GB, 100 objects) -> should go to project_lot
	// - /project/cache (5 GB, 50 objects) -> should go to DEFAULT (recursive exclusion)
	// - /project/cache/build (3 GB, 30 objects) -> should go to DEFAULT (under recursive exclusion)
	// - /project/tmp (2 GB, 20 objects) -> should go to DEFAULT (non-recursive exclusion)
	// - /project/tmp/work (4 GB, 40 objects) -> should go to project_lot (non-recursive exclusion doesn't cover
	// subdirs)
	const char *usage_update = R"([
		{
			"path": "/project/src",
			"size_GB": 10.0,
			"num_obj": 100,
			"includes_subdirs": false,
			"subdirs": []
		},
		{
			"path": "/project/cache",
			"size_GB": 5.0,
			"num_obj": 50,
			"includes_subdirs": false,
			"subdirs": []
		},
		{
			"path": "/project/cache/build",
			"size_GB": 3.0,
			"num_obj": 30,
			"includes_subdirs": false,
			"subdirs": []
		},
		{
			"path": "/project/tmp",
			"size_GB": 2.0,
			"num_obj": 20,
			"includes_subdirs": false,
			"subdirs": []
		},
		{
			"path": "/project/tmp/work",
			"size_GB": 4.0,
			"num_obj": 40,
			"includes_subdirs": false,
			"subdirs": []
		}
	])";

	rv = lotman_update_lot_usage_by_dir(usage_update, false, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed to update usage by dir: " << err_msg.get();

	// Check project_lot usage:
	// Should have: /project/src (10 GB, 100 obj) + /project/tmp/work (4 GB, 40 obj) = 14 GB, 140 objects
	raw_output = nullptr;
	rv = lotman_get_lot_usage(project_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 14.0)
		<< "project_lot should have 14 GB (src + tmp/work)";
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 140)
		<< "project_lot should have 140 objects (src + tmp/work)";

	// Check default lot usage:
	// Should have: /project/cache (5 GB, 50 obj) + /project/cache/build (3 GB, 30 obj) + /project/tmp (2 GB, 20 obj)
	// = 10 GB, 100 objects
	const char *default_usage_query = R"({
		"lot_name": "default",
		"total_GB": false,
		"num_objects": false
	})";
	raw_output = nullptr;
	rv = lotman_get_lot_usage(default_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 10.0)
		<< "default should have 10 GB (cache + cache/build + tmp)";
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 100)
		<< "default should have 100 objects (cache + cache/build + tmp)";

	// Test delta mode update: Add more usage to excluded and non-excluded paths
	const char *delta_update = R"([
		{
			"path": "/project/src/new",
			"size_GB": 1.5,
			"num_obj": 15,
			"includes_subdirs": false,
			"subdirs": []
		},
		{
			"path": "/project/cache/new",
			"size_GB": 0.5,
			"num_obj": 5,
			"includes_subdirs": false,
			"subdirs": []
		}
	])";

	rv = lotman_update_lot_usage_by_dir(delta_update, true, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed delta update: " << err_msg.get();

	// Check project_lot after delta: 14 + 1.5 = 15.5 GB, 140 + 15 = 155 objects
	raw_output = nullptr;
	rv = lotman_get_lot_usage(project_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 15.5)
		<< "project_lot should have 15.5 GB after delta";
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 155)
		<< "project_lot should have 155 objects after delta";

	// Check default after delta: 10 + 0.5 = 10.5 GB, 100 + 5 = 105 objects
	raw_output = nullptr;
	rv = lotman_get_lot_usage(default_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 10.5)
		<< "default should have 10.5 GB after delta";
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 105)
		<< "default should have 105 objects after delta";
}

TEST_F(LotManTest, UpdateUsageByDirNestedExclusionsTest) {
	// Test nested directory structure with hierarchical updates
	// This tests the includes_subdirs=true case with exclusions
	addDefaultLot();
	addLot1();

	// Create a lot with a recursive exclusion
	const char *lot_json = R"({
		"lot_name": "data_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/data", "recursive": true, "exclude": false},
			{"path": "/data/scratch", "recursive": true, "exclude": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 10000,
			"creation_time": 123,
			"expiration_time": 999999999999,
			"deletion_time": 999999999999
		}
	})";

	char *raw_err = nullptr;
	int rv = lotman_add_lot(lot_json, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Update with nested structure where parent includes_subdirs=true
	// /data reports 20 GB total including subdirs
	// /data/important has 8 GB (not excluded, should go to data_lot)
	// /data/scratch has 12 GB (excluded, should go to default)
	//
	// Since /data is not stored as an exact path match (only /data/ is stored),
	// when includes_subdirs=true and recursive=false for the matching rule,
	// it subtracts subdir usage. But /data/ IS recursive, so subdirs are handled recursively.
	const char *nested_update = R"([
		{
			"path": "/data/important",
			"size_GB": 8.0,
			"num_obj": 80,
			"includes_subdirs": false,
			"subdirs": []
		},
		{
			"path": "/data/scratch",
			"size_GB": 12.0,
			"num_obj": 120,
			"includes_subdirs": true,
			"subdirs": [
				{
					"path": "temp",
					"size_GB": 5.0,
					"num_obj": 50,
					"includes_subdirs": false,
					"subdirs": []
				}
			]
		}
	])";

	rv = lotman_update_lot_usage_by_dir(nested_update, false, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed nested update: " << err_msg.get();

	// Check data_lot: should only have /data/important = 8 GB, 80 objects
	const char *data_usage_query = R"({
		"lot_name": "data_lot",
		"total_GB": false,
		"num_objects": false
	})";
	char *raw_output = nullptr;
	rv = lotman_get_lot_usage(data_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 8.0)
		<< "data_lot should only have usage from /data/important";
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 80);

	// Check default lot usage:
	// /data/scratch is excluded and has includes_subdirs=true with 12 GB total (including 5 GB temp subdir).
	// Since the default lot has no explicit path for /data/scratch, JSON_math treats it as non-recursive.
	// With recursive=false and includes_subdirs=true, the function:
	//   1. Subtracts subdir values: 12 - 5 = 7 GB for /data/scratch itself
	//   2. Processes /data/scratch/temp separately: 5 GB (also goes to default due to recursive exclusion)
	// Total default: 7 + 5 = 12 GB
	const char *default_usage_query = R"({
		"lot_name": "default",
		"total_GB": false,
		"num_objects": false
	})";
	raw_output = nullptr;
	rv = lotman_get_lot_usage(default_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 12.0)
		<< "default should have all scratch usage (12 GB total from scratch hierarchy)";
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 120)
		<< "default should have 120 objects from scratch hierarchy";
}

TEST_F(LotManTest, UpdateUsageByDirIncludesSubdirsWithExclusionsTest) {
	// Test case: Parent directory uses includes_subdirs=true with explicit subdirs,
	// where some subdirs map to excluded paths. This tests the interaction between
	// the subdir processing logic and path exclusions.
	addDefaultLot();
	addLot1();

	// Create a lot with mixed included/excluded paths
	// /home/user is included recursively
	// /home/user/downloads is excluded (non-recursive)
	// /home/user/cache is excluded (recursive)
	const char *lot_json = R"({
		"lot_name": "user_lot",
		"owner": "owner1",
		"parents": ["lot1"],
		"paths": [
			{"path": "/home/user", "recursive": true, "exclude": false},
			{"path": "/home/user/downloads", "recursive": false, "exclude": true},
			{"path": "/home/user/cache", "recursive": true, "exclude": true}
		],
		"management_policy_attrs": {
			"dedicated_GB": 100,
			"opportunistic_GB": 50,
			"max_num_objects": 10000,
			"creation_time": 123,
			"expiration_time": 999999999999,
			"deletion_time": 999999999999
		}
	})";

	char *raw_err = nullptr;
	int rv = lotman_add_lot(lot_json, &raw_err);
	UniqueCString err_msg(raw_err);
	ASSERT_EQ(rv, 0) << err_msg.get();

	// Update /home/user with includes_subdirs=true and explicit subdirs.
	// Total reported: 50 GB with subdirs: documents(20), downloads(10), cache(8), .config(4)
	//
	// Since /home/user is recursive, excluded subdirs are detected and processed separately:
	//   - /home/user direct: 50 - 20 - 10 - 8 - 4 = 8 GB -> user_lot
	//   - documents: 20 GB -> user_lot (not excluded)
	//   - downloads: 10 - 3 = 7 GB -> default (excluded non-recursive)
	//   - downloads/archive: 3 GB -> user_lot (parent exclusion is non-recursive)
	//   - cache: 8 - 5 = 3 GB -> default (excluded recursive)
	//   - cache/temp: 5 GB -> default (under recursive exclusion)
	//   - .config: 4 GB -> user_lot (not excluded)
	//
	// Expected totals:
	//   user_lot: 8 + 20 + 3 + 4 = 35 GB, 350 objects
	//   default: 7 + 3 + 5 = 15 GB, 150 objects

	const char *usage_update = R"([
		{
			"path": "/home/user",
			"size_GB": 50.0,
			"num_obj": 500,
			"includes_subdirs": true,
			"subdirs": [
				{
					"path": "documents",
					"size_GB": 20.0,
					"num_obj": 200,
					"includes_subdirs": false,
					"subdirs": []
				},
				{
					"path": "downloads",
					"size_GB": 10.0,
					"num_obj": 100,
					"includes_subdirs": true,
					"subdirs": [
						{
							"path": "archive",
							"size_GB": 3.0,
							"num_obj": 30,
							"includes_subdirs": false,
							"subdirs": []
						}
					]
				},
				{
					"path": "cache",
					"size_GB": 8.0,
					"num_obj": 80,
					"includes_subdirs": true,
					"subdirs": [
						{
							"path": "temp",
							"size_GB": 5.0,
							"num_obj": 50,
							"includes_subdirs": false,
							"subdirs": []
						}
					]
				},
				{
					"path": ".config",
					"size_GB": 4.0,
					"num_obj": 40,
					"includes_subdirs": false,
					"subdirs": []
				}
			]
		}
	])";

	rv = lotman_update_lot_usage_by_dir(usage_update, false, 150, &raw_err);
	err_msg.reset(raw_err);
	ASSERT_EQ(rv, 0) << "Failed usage update: " << err_msg.get();

	// Verify user_lot: 8 (direct) + 20 (documents) + 3 (downloads/archive) + 4 (.config) = 35 GB
	const char *user_usage_query = R"({
		"lot_name": "user_lot",
		"total_GB": false,
		"num_objects": false
	})";
	char *raw_output = nullptr;
	rv = lotman_get_lot_usage(user_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	UniqueCString output(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	json usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 35.0)
		<< "user_lot should have 35 GB: 8 (direct) + 20 (documents) + 3 (downloads/archive) + 4 (.config)";
	// Objects: 500 - 200 - 100 - 80 - 40 = 80 (direct) + 200 + 30 + 40 = 350
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 350) << "user_lot should have 350 objects";

	// Verify default lot: 7 (downloads) + 3 (cache) + 5 (cache/temp) = 15 GB
	const char *default_usage_query = R"({
		"lot_name": "default",
		"total_GB": false,
		"num_objects": false
	})";
	raw_output = nullptr;
	rv = lotman_get_lot_usage(default_usage_query, &raw_output, &raw_err);
	err_msg.reset(raw_err);
	output.reset(raw_output);
	ASSERT_EQ(rv, 0) << err_msg.get();
	usage_json = json::parse(output.get());
	EXPECT_DOUBLE_EQ(usage_json["total_GB"]["self_contrib"].get<double>(), 15.0)
		<< "default should have 15 GB: 7 (downloads) + 3 (cache) + 5 (cache/temp)";
	// Objects: (100 - 30) + (80 - 50) + 50 = 70 + 30 + 50 = 150
	EXPECT_EQ(usage_json["num_objects"]["self_contrib"].get<int64_t>(), 150) << "default should have 150 objects";
}
} // namespace

int main(int argc, char **argv) {
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
