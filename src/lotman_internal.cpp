#include "lotman_internal.h"

#include "lotman_db.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>

using json = nlohmann::json;
using namespace lotman;

namespace {

// Sweep-line event representing a change in concurrent resource usage at a point in time.
struct SweepEvent {
	int64_t time;
	double delta_ded;
	double delta_opp;
	double delta_obj;
	bool is_start; // true=addition, false=removal (used for tie-breaking)
};

// Peak concurrent resource usage found by the sweep-line.
struct SweepResult {
	double peak_ded = 0.0;
	double peak_opp = 0.0;
	double peak_obj = 0.0;
	double peak_total = 0.0; // peak of (ded + opp) at a single point in time
};

// Sort events and sweep to find peak concurrent usage.
// Events are sorted by time, with removals before additions at the same time.
SweepResult run_sweep_line(std::vector<SweepEvent> &events) {
	std::sort(events.begin(), events.end(), [](const SweepEvent &a, const SweepEvent &b) {
		if (a.time != b.time)
			return a.time < b.time;
		return a.is_start < b.is_start; // false (removal) < true (addition)
	});

	double cur_ded = 0.0, cur_opp = 0.0, cur_obj = 0.0;
	SweepResult result;

	for (const auto &ev : events) {
		cur_ded += ev.delta_ded;
		cur_opp += ev.delta_opp;
		cur_obj += ev.delta_obj;

		if (cur_ded > result.peak_ded)
			result.peak_ded = cur_ded;
		if (cur_opp > result.peak_opp)
			result.peak_opp = cur_opp;
		if (cur_obj > result.peak_obj)
			result.peak_obj = cur_obj;

		double cur_total = cur_ded + cur_opp;
		if (cur_total > result.peak_total)
			result.peak_total = cur_total;
	}
	return result;
}

// Helper to build sweep events from a parent's children's attributions.
// If time window is specified (start_time < end_time), events are clipped to [start_time, end_time).
// Returns events vector (may be empty if no children or none overlap window).
std::vector<SweepEvent> build_attribution_events(const std::string &parent_lot_name, int64_t start_time = 0,
												 int64_t end_time = 0) {
	auto &storage = db::StorageManager::get_storage();
	using namespace sqlite_orm;

	std::vector<SweepEvent> events;
	bool has_window = (start_time < end_time);

	// Get all children of this parent (non-self)
	auto child_names = storage.select(&db::Parent::lot_name, where(c(&db::Parent::parent) == parent_lot_name and
																   c(&db::Parent::lot_name) != parent_lot_name));

	for (const auto &child_name : child_names) {
		auto child_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(child_name);
		if (!child_mpa)
			continue;

		// Non-expiring children (sentinel: all timestamps are 0) are treated
		// as if they span (-infinity, +infinity), so they are always present
		// during any query window.
		const bool child_non_expiring =
			is_non_expiring(child_mpa->creation_time, child_mpa->expiration_time, child_mpa->deletion_time);

		// If window specified, skip children that don't overlap it
		if (!child_non_expiring && has_window &&
			(child_mpa->creation_time >= end_time || child_mpa->expiration_time <= start_time))
			continue;

		// Get attributions from this parent to this child
		auto attrs = storage.get_all<db::ParentChildAttribution>(
			where(c(&db::ParentChildAttribution::parent_lot_name) == parent_lot_name and
				  c(&db::ParentChildAttribution::child_lot_name) == child_name));

		// Compute attributed fractions
		std::map<std::string, double> fractions;
		for (const auto &attr : attrs) {
			fractions[attr.mpa_key] = attr.fraction;
		}

		// Fail fast if attribution rows are missing for an active child-parent edge
		// when strict hierarchy is enabled
		if (attrs.empty() && lotman::Context::get_strict_hierarchy()) {
			throw std::runtime_error("Missing attribution rows for child '" + child_name + "' under parent '" +
									 parent_lot_name + "'");
		}

		// Guard sentinel values: treat unbounded axes (value == -1) as contributing
		// 0 to the sweep line.  Callers that only care about bounded axes (e.g.
		// validate_axiom2, get_available_capacity) already short-circuit via the
		// parent_unb_* flags, so an unbounded child never corrupts a cap check.
		double attr_ded = (!is_unbounded_dedicated(child_mpa->dedicated_GB) && fractions.count("dedicated_GB"))
							  ? fractions.at("dedicated_GB") * child_mpa->dedicated_GB
							  : 0.0;
		double attr_opp =
			(!is_unbounded_opportunistic(child_mpa->opportunistic_GB) && fractions.count("opportunistic_GB"))
				? fractions.at("opportunistic_GB") * child_mpa->opportunistic_GB
				: 0.0;
		double attr_obj = (!is_unbounded_objects(child_mpa->max_num_objects) && fractions.count("max_num_objects"))
							  ? std::round(fractions.at("max_num_objects") * child_mpa->max_num_objects)
							  : 0.0;

		// Determine event times: use child's full interval or clip to window
		int64_t event_start = child_non_expiring ? std::numeric_limits<int64_t>::min() : child_mpa->creation_time;
		int64_t event_end = child_non_expiring ? std::numeric_limits<int64_t>::max() : child_mpa->expiration_time;
		if (has_window) {
			event_start = std::max(event_start, start_time);
			event_end = std::min(event_end, end_time);
		}

		events.push_back({event_start, attr_ded, attr_opp, attr_obj, true});
		events.push_back({event_end, -attr_ded, -attr_opp, -attr_obj, false});
	}

	return events;
}

} // anonymous namespace

// TODO: Go through and make things const where they should be declared as such
// TODO: Optimize functions that instantiate a whole lot, when that isn't really needed

/**
 * Functions specific to Lot class
 */

std::pair<bool, std::string> lotman::Lot::init_full(json lot_JSON) {
	lot_name = lot_JSON["lot_name"];
	owner = lot_JSON["owner"];
	parents = lot_JSON["parents"];
	if (!lot_JSON["children"].is_null()) {
		children = lot_JSON["children"];
	}
	if (!lot_JSON["paths"].is_null()) {
		paths = lot_JSON["paths"];
	}

	man_policy_attr.dedicated_GB = lot_JSON["management_policy_attrs"]["dedicated_GB"];
	man_policy_attr.opportunistic_GB = lot_JSON["management_policy_attrs"]["opportunistic_GB"];
	man_policy_attr.max_num_objects = lot_JSON["management_policy_attrs"]["max_num_objects"];
	man_policy_attr.creation_time = lot_JSON["management_policy_attrs"]["creation_time"];
	man_policy_attr.expiration_time = lot_JSON["management_policy_attrs"]["expiration_time"];
	man_policy_attr.deletion_time = lot_JSON["management_policy_attrs"]["deletion_time"];

	// Validate per-axis MPA invariants. A value of 0 on a quota MPA marks
	// that axis as unbounded; the storage axis (dedicated_GB +
	// opportunistic_GB) is validated for self-consistency.
	auto mpa_rp = validate_mpa_invariants(man_policy_attr.dedicated_GB, man_policy_attr.opportunistic_GB,
										  man_policy_attr.max_num_objects);
	if (!mpa_rp.first) {
		return mpa_rp;
	}

	// Validate timestamp invariants. The all-zero tuple is a sentinel for a
	// non-expiring lot; otherwise creation_time < expiration_time is required.
	auto time_rp = validate_time_invariants(man_policy_attr.creation_time, man_policy_attr.expiration_time,
											man_policy_attr.deletion_time);
	if (!time_rp.first) {
		return time_rp;
	}

	usage.self_GB = 0;
	usage.children_GB = 0;
	usage.self_GB_being_written = 0;
	usage.children_GB_being_written = 0;
	usage.self_objects = 0;
	usage.children_objects = 0;
	usage.self_objects_being_written = 0;
	usage.children_objects_being_written = 0;

	full_lot = true;
	return std::make_pair(true, "");
}

std::pair<bool, std::string>
lotman::Lot::init_reassignment_policy(const bool assign_LTBR_parent_as_parent_to_orphans,
									  const bool assign_LTBR_parent_as_parent_to_non_orphans,
									  const bool assign_policy_to_children) {

	reassignment_policy.assign_LTBR_parent_as_parent_to_orphans = assign_LTBR_parent_as_parent_to_orphans;
	reassignment_policy.assign_LTBR_parent_as_parent_to_non_orphans = assign_LTBR_parent_as_parent_to_non_orphans;
	reassignment_policy.assign_policy_to_children = assign_policy_to_children;
	has_reassignment_policy = true;
	return std::make_pair(true, "");
}

void lotman::Lot::init_self_usage() {
	usage.self_GB = 0;
	usage.self_GB_update_staged = false;
	usage.children_GB = 0;
	usage.self_objects = 0;
	usage.self_objects_update_staged = false;
	usage.children_objects = 0;
	usage.self_GB_being_written = 0;
	usage.self_GB_being_written_update_staged = false;
	usage.children_GB_being_written = 0;
	usage.self_objects_being_written = 0;
	usage.self_objects_being_written_update_staged = false;
	usage.children_objects_being_written = 0;
}

std::pair<bool, std::string> lotman::Lot::store_lot(const json &parent_attributions_json) {
	if (!full_lot) {
		return std::make_pair(false, "Lot was not fully initialized");
	}

	// Check that any specified parents already exist, unless the lot has itself as parent
	for (auto &parent : parents) {
		if (parent != lot_name && !lot_exists(parent).first) {
			return std::make_pair(false, "A parent specified for the lot to be added does not exist in the database.");
		}
	}

	// Check that any specified children already exist
	if (children.size() > 0) {
		for (auto &child : children) {
			if (!lot_exists(child).first) {
				return std::make_pair(false,
									  "A child specified for the lot to be added does not exist in the database");
			}
		}
	}

	// Check that the added lot won't introduce any cycles
	bool self_parent; // When checking for cycles, we only care about lots who specify a parent other than themselves
	auto self_parent_iter = std::find(parents.begin(), parents.end(), lot_name);

	self_parent = (self_parent_iter != parents.end());
	if (!children.empty() && ((parents.size() == 1 && !self_parent) ||
							  (parents.size() > 1))) { // If there are children and a non-self parent
		bool cycle_exists = lotman::Checks::cycle_check(lot_name, parents, children);
		if (cycle_exists) {
			return std::make_pair(
				false, "The lot cannot be added because the combination of parents/children would introduce a "
					   "dependency cycle in the data structure."); // Return false, don't do anything with the lot
		}
	}

	// Store lot, compute attributions, and validate in a single transaction.
	// If any step fails, all DB changes are rolled back atomically.
	{
		auto &storage = db::StorageManager::get_storage();
		std::string txn_error;
		bool committed = storage.transaction([&] {
			auto rp_inner = this->write_new();
			if (!rp_inner.first) {
				txn_error = "Failure to store new lot: " + rp_inner.second;
				return false;
			}

			if (lot_name != "default") {
				rp_inner = compute_and_store_attributions(parent_attributions_json);
				if (!rp_inner.first) {
					txn_error = "Failed to compute attributions: " + rp_inner.second;
					return false;
				}

				auto vr = apply_validation_predicates(build_axiom_predicates(lot_name));
				if (!vr.first) {
					txn_error = vr.second;
					return false;
				}
			}

			// Insertion adjustment: if the new lot is being inserted between
			// existing parent-child relationships, update children's parent pointers.
			for (auto &parents_iter : parents) {
				for (auto &children_iter : children) {
					if (lotman::Checks::insertion_check(lot_name, parents_iter, children_iter)) {
						Lot child(children_iter);
						json update_arr = json::array();
						update_arr.push_back({{"current", parents_iter}, {"new", lot_name}});
						if (!child.update_parents_impl(update_arr, txn_error)) {
							return false;
						}
						if (child.lot_name != "default") {
							if (!child.reload_and_recompute_attributions(txn_error)) {
								return false;
							}
						}
					}
				}
			}

			return true;
		});

		if (!committed) {
			return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
		}
	}

	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Lot::lot_exists(const std::string &lot_name) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;
		auto count = storage.count<db::ManagementPolicyAttributes>(
			where(c(&db::ManagementPolicyAttributes::lot_name) == lot_name));
		return std::make_pair(count > 0, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("lot_exists failed: ") + e.what());
	}
}

std::pair<bool, std::string> lotman::Lot::check_if_root() {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;
		auto parent_records = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == lot_name));

		if (parent_records.size() == 1 && parent_records[0] == lot_name) {
			// lot_name has only itself as a parent, indicating root
			is_root = true;
			return std::make_pair(true, "");
		} else {
			is_root = false;
			return std::make_pair(false, "");
		}
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("check_if_root failed: ") + e.what());
	}
}

std::pair<bool, std::string> lotman::Lot::destroy_lot() {

	/*
	FUNCTION FLOW
	Prechecks:
	* Check for reassignment policy. Lots MUST have a reassignment policy before deletion
	* Check if lot-to-be-removed (LTBR) is the default lot. The default lot MUST NOT be deleted while other lots exist.
	LotMan provides no method for deleting the default lot.
	* Check that LTBR actually exists in the database

	Meat:
	* Get the LTBR's immediate children, who need to be reassigned
	* Handle children according to policy
	* Delete LTBR
	*/

	if (!has_reassignment_policy) {
		return std::make_pair(false, "The lot has no defined reassignment policy.");
	}
	if (lot_name == "default") {
		return std::make_pair(false, "The default lot cannot be deleted.");
	}

	// Contraction policy: deletion = contraction to zero
	{
		auto cp = check_contraction_for_deletion(lot_name);
		if (!cp.first) {
			return cp;
		}
	}

	// Prechecks complete, get the children for LTBR
	auto rp_lotvec_str = this->get_children();
	if (!rp_lotvec_str.second.empty()) { // There is an error message
		std::string int_err = rp_lotvec_str.second;
		std::string ext_err = "Failed to get lot children: ";
		return std::make_pair(false, ext_err + int_err);
	}

	// --- Read-only phase: determine which children need reparenting ---
	std::vector<Lot *> children_to_reparent;
	for (auto &child : self_children) {
		bool orphaned = lotman::Checks::will_be_orphaned(lot_name, child.lot_name);
		if (orphaned && !reassignment_policy.assign_LTBR_parent_as_parent_to_orphans) {
			return std::make_pair(false, "The operation cannot be completed as requested because deleting the lot "
										 "would create an orphan that requires explicit assignment to the default "
										 "lot. Set assign_LTBR_parent_as_parent_to_orphans=true.");
		}
		if ((orphaned && reassignment_policy.assign_LTBR_parent_as_parent_to_orphans) ||
			reassignment_policy.assign_LTBR_parent_as_parent_to_non_orphans) {
			children_to_reparent.push_back(&child);
		}
	}

	if (!children_to_reparent.empty()) {
		auto rp_bool_str = this->check_if_root();
		if (!rp_bool_str.second.empty()) {
			return std::make_pair(false, "Function call to lotman::Lot::check_if_root failed: " + rp_bool_str.second);
		}
		if (is_root) {
			return std::make_pair(false,
								  "The lot being removed is a root, and has no parents to assign to its children.");
		}
		this->get_parents();
	}

	// --- Write phase: reparent children + delete LTBR in a single transaction ---
	{
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;
		std::string txn_error;
		bool committed = storage.transaction([&] {
			// Step 1: Add LTBR's parents to children that need reparenting
			for (auto *child : children_to_reparent) {
				auto rp = child->store_new_parents(self_parents);
				if (!rp.first) {
					txn_error = "Failed to add parents to child lot '" + child->lot_name + "': " + rp.second;
					return false;
				}
			}

			// Step 2: Delete LTBR (also cleans up reverse parent refs + attributions involving LTBR)
			auto drp = delete_lot_from_db();
			if (!drp.first) {
				txn_error = "Failed to delete lot: " + drp.second;
				return false;
			}

			// Step 3: Recompute attributions for ALL children that had LTBR as a parent.
			// Reparented children had LTBR replaced by LTBR's parents; non-reparented
			// children simply lost LTBR as a parent. In both cases the attribution
			// rows referencing LTBR were deleted in Step 2, so the remaining
			// attributions no longer sum correctly. Reload from DB and recompute.
			for (auto &child : self_children) {
				if (child.lot_name == "default")
					continue;

				if (!child.reload_and_recompute_attributions(txn_error)) {
					return false;
				}
			}

			return true;
		});

		if (!committed) {
			return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
		}
	}

	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Lot::destroy_lot_recursive() {

	if (lot_name == "default") {
		return std::make_pair(false, "The default lot cannot be deleted.");
	}

	// Contraction policy: deletion = contraction to zero
	{
		auto cp = check_contraction_for_deletion(lot_name);
		if (!cp.first) {
			return cp;
		}
	}

	// Prechecks complete, get the children for LTBR
	auto rp_lotvec_str = this->get_children(true);
	if (!rp_lotvec_str.second.empty()) { // There is an error message
		std::string int_err = rp_lotvec_str.second;
		std::string ext_err = "Failed to get lot children: ";
		return std::make_pair(false, ext_err + int_err);
	}

	// Contraction policy must also be checked for every child in the subtree
	for (const auto &child : recursive_children) {
		auto cp = check_contraction_for_deletion(child.lot_name);
		if (!cp.first) {
			return cp;
		}
	}

	// Delete all children and self in a single atomic transaction.
	{
		auto &storage = db::StorageManager::get_storage();
		std::string txn_error;
		bool committed = storage.transaction([&] {
			for (auto &child : recursive_children) {
				auto rp_bool_str = child.delete_lot_from_db();
				if (!rp_bool_str.first) {
					txn_error = "Failed to delete child lot '" + child.lot_name + "': " + rp_bool_str.second;
					return false;
				}
			}
			auto rp_bool_str = this->delete_lot_from_db();
			if (!rp_bool_str.first) {
				txn_error = "Failed to delete lot '" + lot_name + "': " + rp_bool_str.second;
				return false;
			}
			return true;
		});

		if (!committed) {
			return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
		}
	}

	return std::make_pair(true, "");
}

std::pair<std::vector<Lot>, std::string> lotman::Lot::get_parents(const bool recursive, const bool get_self) {

	std::vector<Lot> parents;
	std::vector<std::string> parent_names_vec;

	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		if (get_self) {
			parent_names_vec = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == lot_name));
		} else {
			parent_names_vec = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == lot_name and
																		 c(&db::Parent::parent) != lot_name));
		}

		if (recursive) {
			std::vector<std::string> current_parents{parent_names_vec};
			while (current_parents.size() > 0) {
				std::vector<std::string> grandparent_names;

				// Do not set const, because we may be updating these parents later
				for (auto &parent : current_parents) {
					auto gp = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == parent and
																		c(&db::Parent::parent) != parent));
					grandparent_names.insert(grandparent_names.end(), gp.begin(), gp.end());
				}

				// grandparent_names might have duplicates. Sort and make unique
				std::sort(grandparent_names.begin(), grandparent_names.end());
				auto last = std::unique(grandparent_names.begin(), grandparent_names.end());
				grandparent_names.erase(last, grandparent_names.end());

				current_parents = grandparent_names;
				parent_names_vec.insert(parent_names_vec.end(), grandparent_names.begin(), grandparent_names.end());
			}
		}

		// Final sort
		std::sort(parent_names_vec.begin(), parent_names_vec.end());
		auto last = std::unique(parent_names_vec.begin(), parent_names_vec.end());
		parent_names_vec.erase(last, parent_names_vec.end());

		// children_names now has names of all children according to get_self and recursion.
		// Create lot objects and return vector of lots
		for (const auto &parent_name : parent_names_vec) {
			Lot lot(parent_name);
			parents.push_back(lot);
		}

		// Assign to lot member vars.
		if (recursive) {
			recursive_parents = parents;
			recursive_parents_loaded = true;
		} else {
			self_parents = parents;
			self_parents_loaded = true;
		}
		return std::make_pair(parents, "");
	} catch (const std::exception &e) {
		return std::make_pair(std::vector<Lot>(), std::string("get_parents failed: ") + e.what());
	}
}

std::pair<std::vector<Lot>, std::string> lotman::Lot::get_children(const bool recursive, const bool get_self) {

	/*
	FUNCTION FLOW
	* Create queries and maps based on get_self
	* Get vector of immediate children
	* If recursive, get children of children
	*/

	std::vector<std::string> children_names;
	std::vector<Lot> children;
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Get first round of children
		if (get_self) {
			children_names = storage.select(&db::Parent::lot_name, where(c(&db::Parent::parent) == lot_name));
		} else {
			children_names = storage.select(&db::Parent::lot_name, where(c(&db::Parent::parent) == lot_name and
																		 c(&db::Parent::lot_name) != lot_name));
		}

		if (recursive) {
			std::vector<std::string> current_children_names{children_names};
			while (current_children_names.size() > 0) {
				std::vector<std::string> grandchildren_names;
				for (const auto &child_name : current_children_names) {
					auto gc = storage.select(&db::Parent::lot_name, where(c(&db::Parent::parent) == child_name and
																		  c(&db::Parent::lot_name) != child_name));
					grandchildren_names.insert(grandchildren_names.end(), gc.begin(), gc.end());
				}
				// grandchildren_names might have duplicates. Sort and make unique
				std::sort(grandchildren_names.begin(), grandchildren_names.end());
				auto last = std::unique(grandchildren_names.begin(), grandchildren_names.end());
				grandchildren_names.erase(last, grandchildren_names.end());

				current_children_names = grandchildren_names;
				children_names.insert(children_names.end(), grandchildren_names.begin(), grandchildren_names.end());
			}
		}

		// Final sort
		std::sort(children_names.begin(), children_names.end());
		auto last = std::unique(children_names.begin(), children_names.end());
		children_names.erase(last, children_names.end());

		// children_names now has names of all children according to get_self and recursion.
		// Create lot objects and return vector of lots
		for (const auto &child_name : children_names) {
			Lot lot(child_name);
			children.push_back(lot);
		}

		// Assign to lot member vars
		if (recursive) {
			recursive_children = children;
			recursive_children_loaded = true;
		} else {
			self_children = children;
			self_children_loaded = true;
		}
		return std::make_pair(children, "");
	} catch (const std::exception &e) {
		return std::make_pair(std::vector<Lot>(), std::string("get_children failed: ") + e.what());
	}
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::get_owners(const bool recursive) {
	std::vector<std::string> lot_owners_vec;

	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		auto owners = storage.select(&db::Owner::owner, where(c(&db::Owner::lot_name) == lot_name));
		if (!owners.empty()) {
			lot_owners_vec.push_back(owners[0]);
		}

		if (recursive) {
			auto rp2 = this->get_parents(true, false);
			if (!rp2.second.empty()) { // There is an error message
				std::string int_err = rp2.second;
				std::string ext_err = "Failure to get parents: ";
				return std::make_pair(std::vector<std::string>(), ext_err + int_err);
			}
			std::vector<Lot> parents = rp2.first;

			for (const auto &parent : parents) {
				auto parent_owners =
					storage.select(&db::Owner::owner, where(c(&db::Owner::lot_name) == parent.lot_name));
				lot_owners_vec.insert(lot_owners_vec.end(), parent_owners.begin(), parent_owners.end());
			}
		}

		// Sort and remove any duplicates
		std::sort(lot_owners_vec.begin(), lot_owners_vec.end());
		auto last = std::unique(lot_owners_vec.begin(), lot_owners_vec.end());
		lot_owners_vec.erase(last, lot_owners_vec.end());

		// Assign to lot vars
		if (recursive) {
			recursive_owners = lot_owners_vec;
		} else {
			if (!lot_owners_vec.empty()) {
				self_owner = lot_owners_vec[0]; // Lots only have one explicit owner
			}
		}
		return std::make_pair(lot_owners_vec, "");
	} catch (const std::exception &e) {
		return std::make_pair(std::vector<std::string>(), std::string("get_owners failed: ") + e.what());
	}
}

std::pair<json, std::string> lotman::Lot::get_restricting_attribute(const std::string &key, const bool recursive) {
	json internal_obj;
	std::vector<std::string> value;
	std::array<std::string, 6> allowed_keys{
		{"dedicated_GB", "opportunistic_GB", "max_num_objects", "creation_time", "expiration_time", "deletion_time"}};
	if (std::find(allowed_keys.begin(), allowed_keys.end(), key) != allowed_keys.end()) {
		std::string policy_attr_query = "SELECT " + key + " FROM management_policy_attributes WHERE lot_name = ?;";
		std::map<std::string, std::vector<int>> policy_attr_query_str_map{{lot_name, {1}}};
		auto rp = lotman::db::SQL_get_matches(policy_attr_query, policy_attr_query_str_map);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(json(), ext_err + int_err);
		}
		value = rp.first;
		std::string restricting_parent_name = lot_name;

		if (recursive) {
			auto rp2 = this->get_parents(true);
			if (!rp2.second.empty()) { // There was an error
				std::string int_err = rp2.second;
				std::string ext_err = "Failure to get lot parents: ";
				return std::make_pair(json(), ext_err + int_err);
			}

			std::vector<lotman::Lot> parents = rp2.first;
			for (const auto &parent : parents) {
				std::map<std::string, std::vector<int>> policy_attr_query_parent_str_map{{parent.lot_name, {1}}};
				rp = lotman::db::SQL_get_matches(policy_attr_query, policy_attr_query_parent_str_map);
				if (!rp.second.empty()) { // There was an error
					std::string int_err = rp.second;
					std::string ext_err = "Failure on call to SQL_get_matches: ";
					return std::make_pair(json(), ext_err + int_err);
				}

				std::vector<std::string> compare_value = rp.first;
				if (!compare_value.empty() && !value.empty() && std::stod(compare_value[0]) < std::stod(value[0])) {
					value[0] = compare_value[0];
					restricting_parent_name = parent.lot_name;
				}
			}
			if (!value.empty()) {
				internal_obj["lot_name"] = restricting_parent_name;
				internal_obj["value"] = std::stod(value[0]);
			} else {
				return std::make_pair(json(), "No valid policy attribute value found");
			}
		} else {
			if (value.empty()) {
				return std::make_pair(json(), "Policy attribute query returned empty result");
			}
			internal_obj["value"] = std::stod(value[0]);
		}
		return std::make_pair(internal_obj, "");
	} else {
		return std::make_pair(json(), " The key \"" + key + "\" is not recognized.");
	}
}

std::pair<json, std::string> lotman::Lot::get_lot_dirs(const bool recursive) {
	json path_arr = json::array();

	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		auto path_records = storage.get_all<db::Path>(where(c(&db::Path::lot_name) == lot_name));

		for (const auto &path_rec : path_records) {
			json path_obj_internal;
			path_obj_internal["lot_name"] = this->lot_name;
			path_obj_internal["recursive"] = static_cast<bool>(path_rec.recursive);
			path_obj_internal["path"] = path_rec.path;
			path_obj_internal["exclude"] = static_cast<bool>(path_rec.exclude);
			path_arr.push_back(path_obj_internal);
		}

		if (recursive) { // Not recursion of path, but recursion of dirs associated to a lot
			auto rp_vec_str = this->get_children(true);
			if (!rp_vec_str.second.empty()) { // There was an error
				std::string int_err = rp_vec_str.second;
				std::string ext_err = "Failure to get children.";
				return std::make_pair(json::array(), ext_err + int_err);
			}
			for (const auto &child : recursive_children) {
				auto child_path_records = storage.get_all<db::Path>(where(c(&db::Path::lot_name) == child.lot_name));

				for (const auto &path_rec : child_path_records) {
					json path_obj_internal;
					path_obj_internal["lot_name"] = child.lot_name;
					path_obj_internal["recursive"] = static_cast<bool>(path_rec.recursive);
					path_obj_internal["path"] = path_rec.path;
					path_obj_internal["exclude"] = static_cast<bool>(path_rec.exclude);
					path_arr.push_back(path_obj_internal);
				}
			}
		}

		return std::make_pair(path_arr, "");
	} catch (const std::exception &e) {
		return std::make_pair(path_arr, std::string("get_lot_dirs failed: ") + e.what());
	}
}

std::pair<std::string, std::string> lotman::Lot::get_lot_from_dir(const std::string &dir_path, int64_t query_time) {
	try {
		// Normalize path with trailing slash to match stored format
		std::string normalized_path = ensure_trailing_slash(dir_path);

		std::string query = "SELECT p.lot_name FROM paths p "
							"JOIN management_policy_attributes mpa ON p.lot_name = mpa.lot_name "
							"WHERE p.path = ?1 AND mpa.creation_time <= ?2 AND mpa.expiration_time > ?2 "
							"LIMIT 1;";
		std::map<std::string, std::vector<int>> str_map{{normalized_path, {1}}};
		std::map<int64_t, std::vector<int>> int_map{{query_time, {2}}};
		auto rp = db::SQL_get_matches(query, str_map, int_map);

		if (!rp.second.empty()) {
			return std::make_pair("", std::string("get_lot_from_dir query failed: ") + rp.second);
		}
		if (rp.first.empty()) {
			return std::make_pair("", "");
		}
		return std::make_pair(rp.first[0], "");
	} catch (const std::exception &e) {
		return std::make_pair("", std::string("get_lot_from_dir failed: ") + e.what());
	}
}

std::pair<json, std::string> lotman::Lot::get_lot_usage(const std::string &key, const bool recursive) {

	// TODO: Introduce some notion of verbocity to give options for output, like:
	// {"dedicated_GB" : 10} vs {"dedicated_GB" : {"personal": 5, "children" : 5}} vs {"dedicated_GB" : {"personal" : 5,
	// "child1" : 2.5, "child2" : 2.5}} Think a bit more about whether this makes sense.

	// TODO: Might be worthwhile to join some of these sections that share a common preamble

	json output_obj;
	std::array<std::string, 6> allowed_keys = {"dedicated_GB", "opportunistic_GB", "total_GB",
											   "num_objects",  "GB_being_written", "objects_being_written"};
	if (std::find(allowed_keys.begin(), allowed_keys.end(), key) == allowed_keys.end()) {
		return std::make_pair(json(), "The key \"" + key + "\" is not recognized.");
	}

	std::vector<std::string> query_output;
	std::vector<std::vector<std::string>> query_multi_out;

	if (key == "dedicated_GB") {
		if (recursive) {
			std::string rec_ded_usage_query =
				"SELECT "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN lot_usage.self_GB + lot_usage.children_GB "
				"WHEN lot_usage.self_GB + lot_usage.children_GB <= management_policy_attributes.dedicated_GB THEN "
				"lot_usage.self_GB + lot_usage.children_GB "
				"ELSE management_policy_attributes.dedicated_GB "
				"END AS total, " // For readability, not actually referencing these column names
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN lot_usage.self_GB "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN "
				"management_policy_attributes.dedicated_GB "
				"ELSE lot_usage.self_GB "
				"END AS self_contrib, "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN lot_usage.children_GB "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN '0' "
				"WHEN lot_usage.self_GB + lot_usage.children_GB >= management_policy_attributes.dedicated_GB THEN "
				"management_policy_attributes.dedicated_GB - lot_usage.self_GB "
				"ELSE lot_usage.children_GB "
				"END AS children_contrib "
				"FROM lot_usage "
				"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
				"WHERE lot_usage.lot_name = ?;";
			std::map<std::string, std::vector<int>> ded_GB_query_str_map{{lot_name, {1}}};
			auto rp_multi = lotman::db::SQL_get_matches_multi_col(rec_ded_usage_query, 3, ded_GB_query_str_map);
			if (!rp_multi.second.empty()) { // There was an error
				std::string int_err = rp_multi.second;
				std::string ext_err = "Failure on call to SQL_get_matches_multi_col: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_multi_out = rp_multi.first;
			if (query_multi_out.empty() || query_multi_out[0].size() < 3) {
				return std::make_pair(json(), "Multi-column query returned insufficient results for dedicated_GB");
			}
			output_obj["total"] = std::stod(query_multi_out[0][0]);
			output_obj["self_contrib"] = std::stod(query_multi_out[0][1]);
			output_obj["children_contrib"] = std::stod(query_multi_out[0][2]);
		} else {
			std::string ded_GB_query =
				"SELECT "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN lot_usage.self_GB "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN "
				"management_policy_attributes.dedicated_GB "
				"ELSE lot_usage.self_GB "
				"END AS total "
				"FROM "
				"lot_usage "
				"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
				"WHERE lot_usage.lot_name = ?;";

			std::map<std::string, std::vector<int>> ded_GB_query_str_map{{lot_name, {1}}};
			auto rp_single = lotman::db::SQL_get_matches(ded_GB_query, ded_GB_query_str_map);
			if (!rp_single.second.empty()) { // There was an error
				std::string int_err = rp_single.second;
				std::string ext_err = "Failure on call to SQL_get_matches: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_output = rp_single.first;
			if (query_output.empty()) {
				return std::make_pair(json(), "Query returned empty result for dedicated_GB");
			}
			output_obj["self_contrib"] = std::stod(query_output[0]);
		}
	}

	else if (key == "opportunistic_GB") {
		if (recursive) {
			std::string rec_opp_usage_query =
				"SELECT "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN '0' "
				"WHEN management_policy_attributes.opportunistic_GB = -1 THEN "
				"CASE WHEN lot_usage.self_GB + lot_usage.children_GB > management_policy_attributes.dedicated_GB THEN "
				"lot_usage.self_GB + lot_usage.children_GB - management_policy_attributes.dedicated_GB ELSE '0' END "
				"WHEN lot_usage.self_GB + lot_usage.children_GB >= management_policy_attributes.opportunistic_GB "
				"+management_policy_attributes.dedicated_GB THEN management_policy_attributes.opportunistic_GB "
				"WHEN lot_usage.self_GB + lot_usage.children_GB >= management_policy_attributes.dedicated_GB THEN "
				"lot_usage.self_GB + lot_usage.children_GB - management_policy_attributes.dedicated_GB "
				"ELSE '0' "
				"END AS total, "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN '0' "
				"WHEN management_policy_attributes.opportunistic_GB = -1 THEN "
				"CASE WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN "
				"lot_usage.self_GB - management_policy_attributes.dedicated_GB ELSE '0' END "
				"WHEN lot_usage.self_GB >= management_policy_attributes.opportunistic_GB + "
				"management_policy_attributes.dedicated_GB THEN management_policy_attributes.opportunistic_GB "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN  lot_usage.self_GB - "
				"management_policy_attributes.dedicated_GB "
				"ELSE '0' "
				"END AS self_contrib, "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN '0' "
				"WHEN management_policy_attributes.opportunistic_GB = -1 THEN "
				"CASE WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN lot_usage.children_GB "
				"WHEN lot_usage.self_GB + lot_usage.children_GB > management_policy_attributes.dedicated_GB THEN "
				"lot_usage.self_GB + lot_usage.children_GB - management_policy_attributes.dedicated_GB "
				"ELSE '0' END "
				"WHEN lot_usage.self_GB >= management_policy_attributes.opportunistic_GB + "
				"management_policy_attributes.dedicated_GB THEN '0' "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB AND lot_usage.self_GB + "
				"lot_usage.children_GB >= management_policy_attributes.opportunistic_GB + "
				"management_policy_attributes.dedicated_GB THEN management_policy_attributes.opportunistic_GB + "
				"management_policy_attributes.dedicated_GB - lot_usage.self_GB "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB AND lot_usage.self_GB + "
				"lot_usage.children_GB < management_policy_attributes.opportunistic_GB + "
				"management_policy_attributes.dedicated_GB THEN lot_usage.children_GB "
				"WHEN lot_usage.self_GB < management_policy_attributes.dedicated_GB AND lot_usage.self_GB + "
				"lot_usage.children_GB >= management_policy_attributes.opportunistic_GB + "
				"management_policy_attributes.dedicated_GB THEN management_policy_attributes.opportunistic_GB "
				"WHEN lot_usage.self_GB < management_policy_attributes.dedicated_GB AND lot_usage.self_GB + "
				"lot_usage.children_GB > management_policy_attributes.dedicated_GB THEN lot_usage.self_GB + "
				"lot_usage.children_GB - management_policy_attributes.dedicated_GB "
				"ELSE '0' "
				"END AS children_contrib "
				"FROM "
				"lot_usage "
				"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
				"WHERE lot_usage.lot_name = ?;";
			std::map<std::string, std::vector<int>> opp_GB_query_str_map{{lot_name, {1}}};
			auto rp_multi = lotman::db::SQL_get_matches_multi_col(rec_opp_usage_query, 3, opp_GB_query_str_map);
			if (!rp_multi.second.empty()) { // There was an error
				std::string int_err = rp_multi.second;
				std::string ext_err = "Failure on call to SQL_get_matches_multi_col: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_multi_out = rp_multi.first;
			if (query_multi_out.empty() || query_multi_out[0].size() < 3) {
				return std::make_pair(json(), "Multi-column query returned insufficient results for opportunistic_GB");
			}
			output_obj["total"] = std::stod(query_multi_out[0][0]);
			output_obj["self_contrib"] = std::stod(query_multi_out[0][1]);
			output_obj["children_contrib"] = std::stod(query_multi_out[0][2]);
		} else {
			std::string opp_GB_query =
				"SELECT "
				"CASE "
				"WHEN management_policy_attributes.dedicated_GB = -1 THEN '0' "
				"WHEN management_policy_attributes.opportunistic_GB = -1 THEN "
				"CASE WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN "
				"lot_usage.self_GB - management_policy_attributes.dedicated_GB ELSE '0' END "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB + "
				"management_policy_attributes.opportunistic_GB THEN management_policy_attributes.opportunistic_GB "
				"WHEN lot_usage.self_GB >= management_policy_attributes.dedicated_GB THEN lot_usage.self_GB - "
				"management_policy_attributes.dedicated_GB "
				"ELSE '0' "
				"END AS total "
				"FROM "
				"lot_usage "
				"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
				"WHERE lot_usage.lot_name = ?;";

			std::map<std::string, std::vector<int>> opp_GB_query_str_map{{lot_name, {1}}};
			auto rp_single = lotman::db::SQL_get_matches(opp_GB_query, opp_GB_query_str_map);
			if (!rp_single.second.empty()) { // There was an error
				std::string int_err = rp_single.second;
				std::string ext_err = "Failure on call to SQL_get_matches: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_output = rp_single.first;
			if (query_output.empty()) {
				return std::make_pair(json(), "Query returned empty result for opportunistic_GB");
			}
			output_obj["self_contrib"] = std::stod(query_output[0]);
		}
	}

	else if (key == "total_GB") {
		// Get the total usage
		if (recursive) {
			// Need to consider usage from children
			std::string child_usage_GB_query = "SELECT self_GB, children_GB FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> child_usage_GB_str_map{{lot_name, {1}}};
			auto rp_multi = lotman::db::SQL_get_matches_multi_col(child_usage_GB_query, 2, child_usage_GB_str_map);
			if (!rp_multi.second.empty()) { // There was an error
				std::string int_err = rp_multi.second;
				std::string ext_err = "Failure on call to SQL_get_matches_multi_col: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_multi_out = rp_multi.first;
			if (query_multi_out.empty() || query_multi_out[0].size() < 2) {
				return std::make_pair(json(), "Multi-column query returned insufficient results for num_objects");
			}
			output_obj["self_contrib"] = std::stod(query_multi_out[0][0]);
			output_obj["children_contrib"] = std::stod(query_multi_out[0][1]);
			output_obj["total"] = std::stod(query_multi_out[0][0]) + std::stod(query_multi_out[0][1]);
		} else {
			std::string usage_GB_query = "SELECT self_GB FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> usage_GB_str_map{{lot_name, {1}}};
			auto rp_single = lotman::db::SQL_get_matches(usage_GB_query, usage_GB_str_map);
			if (!rp_single.second.empty()) { // There was an error
				std::string int_err = rp_single.second;
				std::string ext_err = "Failure on call to SQL_get_matches: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_output = rp_single.first;
			if (query_output.empty()) {
				return std::make_pair(json(), "Query returned empty result for num_objects");
			}
			output_obj["self_contrib"] = std::stod(query_output[0]);
		}
	}

	else if (key == "num_objects") {
		if (recursive) {
			std::string rec_num_obj_query = "SELECT self_objects, children_objects FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> rec_num_obj_str_map{{lot_name, {1}}};
			auto rp_multi = lotman::db::SQL_get_matches_multi_col(rec_num_obj_query, 2, rec_num_obj_str_map);
			if (!rp_multi.second.empty()) { // There was an error
				std::string int_err = rp_multi.second;
				std::string ext_err = "Failure on call to SQL_get_matches_multi_col: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_multi_out = rp_multi.first;
			if (query_multi_out.empty() || query_multi_out[0].size() < 2) {
				return std::make_pair(json(), "Multi-column query returned insufficient results for GB_being_written");
			}
			output_obj["self_contrib"] = std::stod(query_multi_out[0][0]);
			output_obj["children_contrib"] = std::stod(query_multi_out[0][1]);
			output_obj["total"] = std::stod(query_multi_out[0][0]) + std::stod(query_multi_out[0][1]);
		} else {

			std::string num_obj_query = "SELECT self_objects FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> num_obj_str_map{{lot_name, {1}}};
			auto rp_single = lotman::db::SQL_get_matches(num_obj_query, num_obj_str_map);
			if (!rp_single.second.empty()) { // There was an error
				std::string int_err = rp_single.second;
				std::string ext_err = "Failure on call to SQL_get_matches: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_output = rp_single.first;
			if (query_output.empty()) {
				return std::make_pair(json(), "Query returned empty result for GB_being_written");
			}
			output_obj["self_contrib"] = std::stod(query_output[0]);
		}
	}

	else if (key == "GB_being_written") {
		if (recursive) {
			std::string rec_GB_being_written_query =
				"SELECT self_GB_being_written, children_GB_being_written FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> rec_GB_being_written_str_map{{lot_name, {1}}};
			auto rp_multi =
				lotman::db::SQL_get_matches_multi_col(rec_GB_being_written_query, 2, rec_GB_being_written_str_map);
			if (!rp_multi.second.empty()) { // There was an error
				std::string int_err = rp_multi.second;
				std::string ext_err = "Failure on call to SQL_get_matches_multi_col: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_multi_out = rp_multi.first;
			if (query_multi_out.empty() || query_multi_out[0].size() < 2) {
				return std::make_pair(json(),
									  "Multi-column query returned insufficient results for objects_being_written");
			}
			output_obj["self_contrib"] = std::stod(query_multi_out[0][0]);
			output_obj["children_contrib"] = std::stod(query_multi_out[0][1]);
			output_obj["total"] = std::stod(query_multi_out[0][0]) + std::stod(query_multi_out[0][1]);
		} else {

			std::string GB_being_written_query = "SELECT self_GB_being_written FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> GB_being_written_str_map{{lot_name, {1}}};
			auto rp_single = lotman::db::SQL_get_matches(GB_being_written_query, GB_being_written_str_map);
			if (!rp_single.second.empty()) { // There was an error
				std::string int_err = rp_single.second;
				std::string ext_err = "Failure on call to SQL_get_matches: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_output = rp_single.first;
			if (query_output.empty()) {
				return std::make_pair(json(), "Query returned empty result for objects_being_written");
			}
			output_obj["self_contrib"] = std::stod(query_output[0]);
		}
	}

	else if (key == "objects_being_written") {
		if (recursive) {
			std::string rec_objects_being_written_query =
				"SELECT self_objects_being_written, children_objects_being_written FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> rec_objects_being_written_str_map{{lot_name, {1}}};
			auto rp_multi = lotman::db::SQL_get_matches_multi_col(rec_objects_being_written_query, 2,
																  rec_objects_being_written_str_map);
			if (!rp_multi.second.empty()) { // There was an error
				std::string int_err = rp_multi.second;
				std::string ext_err = "Failure on call to SQL_get_matches_multi_col: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_multi_out = rp_multi.first;
			if (query_multi_out.empty() || query_multi_out[0].size() < 2) {
				return std::make_pair(json(), "Multi-column query returned insufficient results for max_num_objects");
			}
			output_obj["self_contrib"] = std::stod(query_multi_out[0][0]);
			output_obj["children_contrib"] = std::stod(query_multi_out[0][1]);
			output_obj["total"] = std::stod(query_multi_out[0][0]) + std::stod(query_multi_out[0][1]);
		} else {

			std::string objects_being_written_query =
				"SELECT self_objects_being_written FROM lot_usage WHERE lot_name = ?;";
			std::map<std::string, std::vector<int>> objects_being_written_str_map{{lot_name, {1}}};
			auto rp_single = lotman::db::SQL_get_matches(objects_being_written_query, objects_being_written_str_map);
			if (!rp_single.second.empty()) { // There was an error
				std::string int_err = rp_single.second;
				std::string ext_err = "Failure on call to SQL_get_matches: ";
				return std::make_pair(json(), ext_err + int_err);
			}
			query_output = rp_single.first;
			if (query_output.empty()) {
				return std::make_pair(json(), "Query returned empty result for max_num_objects");
			}
			output_obj["self_contrib"] = std::stod(query_output[0]);
		}
	}

	return std::make_pair(output_obj, "");
}

// Non-transactional helper: reloads parents and MPAs from DB, clears old attributions,
// recomputes with the supplied per-parent shares (equal split for any unspecified
// parent), and validates axioms. Caller MUST hold an active transaction.
bool lotman::Lot::reload_and_recompute_attributions(std::string &txn_error, const json &parent_attributions_json) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Reload parents from DB
		auto parent_records = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == lot_name));
		parents.clear();
		for (const auto &p : parent_records) {
			parents.push_back(p);
		}

		// Reload MPAs
		auto mpa_ptr = storage.get_pointer<db::ManagementPolicyAttributes>(lot_name);
		if (!mpa_ptr) {
			txn_error = "Lot '" + lot_name + "' not found in management_policy_attributes";
			return false;
		}
		man_policy_attr.dedicated_GB = mpa_ptr->dedicated_GB;
		man_policy_attr.opportunistic_GB = mpa_ptr->opportunistic_GB;
		man_policy_attr.max_num_objects = mpa_ptr->max_num_objects;

		// Clear old attributions and recompute, honoring any explicit per-parent
		// shares supplied by the caller; remaining parents get an equal split of
		// the remainder.
		storage.remove_all<db::ParentChildAttribution>(
			where(c(&db::ParentChildAttribution::child_lot_name) == lot_name));
		auto attr_rp = compute_and_store_attributions(parent_attributions_json);
		if (!attr_rp.first) {
			txn_error = "Failed to recompute attributions for '" + lot_name + "': " + attr_rp.second;
			return false;
		}

		auto vr = apply_validation_predicates(build_axiom_predicates(lot_name));
		if (!vr.first) {
			txn_error = "Hierarchy violation for '" + lot_name + "': " + vr.second;
			return false;
		}

		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to reload and recompute attributions: ") + e.what();
		return false;
	}
}

// Non-transactional helper: stores parents, reloads state, recomputes attributions, validates.
// Caller MUST hold an active storage.transaction().
bool lotman::Lot::add_parents_impl(const std::vector<Lot> &parents, std::string &txn_error,
								   const json &parent_attributions_json) {
	try {
		auto rp = store_new_parents(parents);
		if (!rp.first) {
			txn_error = "Call to lotman::Lot::store_new_parents failed: " + rp.second;
			return false;
		}

		if (lot_name != "default") {
			if (!reload_and_recompute_attributions(txn_error, parent_attributions_json)) {
				return false;
			}
		}

		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to add parents: ") + e.what();
		return false;
	}
}

std::pair<bool, std::string> lotman::Lot::add_parents(const std::vector<Lot> &parents) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed = storage.transaction([&] { return add_parents_in_txn(parents, txn_error); });

	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}
	return std::make_pair(true, "");
}

bool lotman::Lot::add_parents_in_txn(const std::vector<Lot> &parents, std::string &txn_error,
									 const json &parent_attributions_json) {
	// Perform a cycle check
	// Build the list of all proposed parents
	std::vector<std::string> parent_names;
	this->get_parents(true, true);
	for (const auto &parent : recursive_parents) {
		parent_names.push_back(parent.lot_name);
	}
	for (const auto &parent_lot : parents) {
		parent_names.push_back(parent_lot.lot_name);
	}

	// Build list of all children, minus self
	std::vector<std::string> children_names;
	this->get_children(true, false);
	for (const auto &child : recursive_children) {
		children_names.push_back(child.lot_name);
	}

	// Perform the cycle check
	if (Checks::cycle_check(lot_name, parent_names, children_names)) {
		txn_error = "The requested parent addition would introduce a dependency cycle.";
		return false;
	}

	return add_parents_impl(parents, txn_error, parent_attributions_json);
}

std::pair<bool, std::string> lotman::Lot::add_paths(const std::vector<json> &paths) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed = storage.transaction([&] { return add_paths_in_txn(paths, txn_error); });
	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}
	return std::make_pair(true, "");
}

bool lotman::Lot::add_paths_in_txn(const std::vector<json> &paths, std::string &txn_error) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		auto this_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(lot_name);
		if (this_mpa) {
			for (const auto &path_json : paths) {
				std::string path_str = path_json.at("path").get<std::string>();
				std::string normalized = ensure_trailing_slash(path_str);

				bool exclude = path_json.contains("exclude") ? path_json["exclude"].get<bool>() : false;
				if (!exclude) {
					auto overlap = check_path_temporal_overlap(lot_name, normalized, this_mpa->creation_time,
															   this_mpa->expiration_time);
					if (!overlap.first) {
						txn_error = overlap.second;
						return false;
					}
				}
			}
		}

		for (const auto &path : paths) {
			std::string normalized = ensure_trailing_slash(path["path"].get<std::string>());
			bool recursive = path["recursive"].get<bool>();
			bool exclude = path.contains("exclude") ? path["exclude"].get<bool>() : false;
			storage.replace(db::Path{lot_name, normalized, static_cast<int>(recursive), static_cast<int>(exclude)});
		}
		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to add paths: ") + e.what();
		return false;
	}
}

std::pair<bool, std::string> lotman::Lot::remove_parents(const std::vector<std::string> &parents_to_remove) {
	/*
	First we need to check whether removing the specified parents would break the lot
	data structure, and if it would, the function should fail without deleting anything
	*/

	// Get the lot's actual set of non-recursive parents
	this->get_parents(false, true); // non recursive, but include self if self parent
	int remaining_parents = self_parents.size();

	// Sort and deduplicate, just in case...
	std::vector<std::string> parents_copy = parents_to_remove;
	std::sort(parents_copy.begin(), parents_copy.end());			   // sort vector to group duplicate elements
	auto last = std::unique(parents_copy.begin(), parents_copy.end()); // remove consecutive duplicates
	parents_copy.erase(last, parents_copy.end());					   // erase the duplicates

	for (const auto &parent : self_parents) {
		if (std::find(parents_copy.begin(), parents_copy.end(), parent.lot_name) != parents_copy.end()) {
			remaining_parents -= 1;
		}
	}

	// Make sure that there's at least one responsible parent left to be associated
	// with the lot if all the specified parents are removed.
	if (remaining_parents < 1) {
		return std::make_pair(false, "Could not remove parents because doing so would orphan the lot.");
	}

	// Remove parents, recompute attributions, and validate in a single transaction.
	// If any step fails, all DB changes are rolled back atomically.
	{
		auto &storage = db::StorageManager::get_storage();
		std::string txn_error;
		bool committed = storage.transaction([&] {
			auto rp = remove_parents_from_db(parents_copy);
			if (!rp.first) {
				txn_error = "Call to lotman::Lot::remove_parents failed: " + rp.second;
				return false;
			}

			if (lot_name != "default") {
				if (!reload_and_recompute_attributions(txn_error)) {
					return false;
				}
			}

			return true;
		});

		if (!committed) {
			return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
		}
	}

	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Lot::remove_paths(const std::vector<std::string> &paths) {
	auto rp = remove_paths_from_db(paths);
	if (!rp.first) {
		std::string int_err = rp.second;
		std::string ext_err = "Call to lotman::Lot::remove_paths failed: ";
		return std::make_pair(false, ext_err + int_err);
	}
	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Lot::update_owner(const std::string &update_val) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed = storage.transaction([&] { return update_owner_in_txn(update_val, txn_error); });
	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}
	return std::make_pair(true, "");
}

bool lotman::Lot::update_owner_in_txn(const std::string &update_val, std::string &txn_error) {
	try {
		auto &storage = db::StorageManager::get_storage();
		// Owner has lot_name as primary key, so replace performs an in-place update.
		storage.replace(db::Owner{lot_name, update_val});
		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to update owner: ") + e.what();
		return false;
	}
}

// Non-transactional helper: updates parent records using ORM.
// Caller MUST hold an active storage.transaction().
bool lotman::Lot::update_parents_impl(const json &update_arr, std::string &txn_error) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		for (const auto &update_obj : update_arr) {
			std::string current_parent = update_obj["current"].get<std::string>();
			std::string new_parent = update_obj["new"].get<std::string>();

			// Remove old parent record and insert the updated one.
			// Parent table has composite PK (lot_name, parent), so we must
			// remove + replace rather than update-in-place.
			storage.remove_all<db::Parent>(
				where(c(&db::Parent::lot_name) == lot_name and c(&db::Parent::parent) == current_parent));
			storage.replace(db::Parent{lot_name, new_parent});
		}

		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to update parents: ") + e.what();
		return false;
	}
}

std::pair<bool, std::string> lotman::Lot::update_parents(const json &update_arr) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed = storage.transaction([&] { return update_parents_in_txn(update_arr, txn_error); });

	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}

	return std::make_pair(true, "");
}

bool lotman::Lot::update_parents_in_txn(const json &update_arr, std::string &txn_error) {
	// First, perform a cycle check on the whole update arr, and fail if any introduce a cycle
	// Cycle check takes in three arguments -- The start node (in this case, lot_name), and vectors of parents/children
	// of the start node as strings

	// Get all the existing parents
	std::vector<std::string> parent_list;
	this->get_parents(
		true, true); // get_self is true because either we need get_self to be true for get_parents or get_children.

	for (const auto &parent_lot : recursive_parents) {
		parent_list.push_back(parent_lot.lot_name);
	}
	// for each existing parent, if it's being updated, swap it out with the new parent.
	for (const auto &update : update_arr) {
		auto parent_iter = std::find(parent_list.begin(), parent_list.end(), update["current"]);
		if (parent_iter != parent_list.end()) {
			*parent_iter = update["new"];
		} else {
			txn_error = "One of the current parents, " + update["current"].get<std::string>() +
						", to be updated is not actually a parent.";
			return false;
		}
	}

	std::vector<std::string> children;
	this->get_children(true, false);
	for (const auto &child_lot : recursive_children) {
		children.push_back(child_lot.lot_name);
	}

	if (Checks::cycle_check(lot_name, parent_list, children)) {
		txn_error = "The requested parent update would introduce a dependency cycle.";
		return false;
	}

	if (!update_parents_impl(update_arr, txn_error)) {
		return false;
	}
	if (lot_name != "default") {
		if (!reload_and_recompute_attributions(txn_error)) {
			return false;
		}
	}
	return true;
}

std::pair<bool, std::string> lotman::Lot::update_paths(const json &update_arr) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed = storage.transaction([&] { return update_paths_in_txn(update_arr, txn_error); });
	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}
	return std::make_pair(true, "");
}

bool lotman::Lot::update_paths_in_txn(const json &update_arr, std::string &txn_error) {
	auto &storage = db::StorageManager::get_storage();
	using namespace sqlite_orm;
	try {
		for (const auto &update_obj : update_arr) {
			std::string current_path = ensure_trailing_slash(update_obj["current"].get<std::string>());
			std::string new_path = ensure_trailing_slash(update_obj["new"].get<std::string>());

			// Read the current path record
			auto rows = storage.get_all<db::Path>(
				where(c(&db::Path::lot_name) == lot_name and c(&db::Path::path) == current_path));
			if (rows.empty()) {
				txn_error = "Path not found for update: " + current_path;
				return false;
			}
			auto path_record = rows[0];

			// Apply field updates
			path_record.recursive = update_obj["recursive"].get<int>();
			bool exclude_changed_to_false = false;
			if (update_obj.contains("exclude")) {
				int new_exclude = update_obj["exclude"].get<int>();
				if (path_record.exclude && !new_exclude) {
					exclude_changed_to_false = true;
				}
				path_record.exclude = new_exclude;
			}

			// Check temporal overlap if: (1) path changes, OR (2) exclude flips true→false
			// In case (2), a previously-excluded path becomes active and might conflict
			if (current_path != new_path || exclude_changed_to_false) {
				// Check temporal overlap for the path (if not excluded)
				if (!path_record.exclude) {
					auto this_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(lot_name);
					if (this_mpa) {
						// Use new_path for case (1), current_path for case (2)
						std::string path_to_check = (current_path != new_path) ? new_path : current_path;
						auto overlap = check_path_temporal_overlap(lot_name, path_to_check, this_mpa->creation_time,
																   this_mpa->expiration_time);
						if (!overlap.first) {
							txn_error = overlap.second;
							return false;
						}
					}
				}
			}

			// If path itself changed, we must remove+replace since path is part of composite PK
			if (current_path != new_path) {
				storage.remove_all<db::Path>(
					where(c(&db::Path::lot_name) == lot_name and c(&db::Path::path) == current_path));
				path_record.path = new_path;
			}
			storage.replace(path_record);
		}
		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to update paths: ") + e.what();
		return false;
	}
}

std::pair<bool, std::string> lotman::Lot::update_man_policy_attrs(const std::string &update_key, double update_val) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed =
		storage.transaction([&] { return update_man_policy_attrs_in_txn(update_key, update_val, txn_error); });
	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}
	return std::make_pair(true, "");
}

bool lotman::Lot::update_man_policy_attrs_in_txn(const std::string &update_key, double update_val,
												 std::string &txn_error) {
	auto &storage = db::StorageManager::get_storage();
	using namespace sqlite_orm;
	auto mpa_ptr = storage.get_pointer<db::ManagementPolicyAttributes>(lot_name);
	if (!mpa_ptr) {
		txn_error = "Lot '" + lot_name + "' not found in management_policy_attributes";
		return false;
	}
	auto mpa = *mpa_ptr;

	// Contraction policy enforcement (inside transaction for consistency).
	// Floating-point comparisons use a small epsilon so trivial round-trip noise
	// (e.g. JSON re-parse) is not flagged as a contraction; integer fields are
	// compared exactly.
	std::string contraction_policy = Context::get_contraction_policy();
	if (contraction_policy != "none" && !Context::get_admin_override()) {
		constexpr double kContractionEps = 1e-9;
		bool is_contraction = false;
		if (update_key == "dedicated_GB" && update_val < mpa.dedicated_GB - kContractionEps)
			is_contraction = true;
		if (update_key == "opportunistic_GB" && update_val < mpa.opportunistic_GB - kContractionEps)
			is_contraction = true;
		if (update_key == "max_num_objects" && update_val < mpa.max_num_objects)
			is_contraction = true;
		if (update_key == "creation_time" && update_val > mpa.creation_time)
			is_contraction = true;
		if (update_key == "expiration_time" && update_val < mpa.expiration_time)
			is_contraction = true;
		if (update_key == "deletion_time" && update_val < mpa.deletion_time)
			is_contraction = true;

		if (is_contraction) {
			if (contraction_policy == "always") {
				txn_error = "Contraction policy 'always' blocks reduction of '" + update_key + "' on lot '" + lot_name +
							"'. Set admin_override to bypass.";
				return false;
			}
			if (contraction_policy == "alive") {
				auto alive_rp = is_lot_alive(lot_name);
				if (!alive_rp.second.empty()) {
					txn_error = "Failed contraction policy check: " + alive_rp.second;
					return false;
				}
				if (alive_rp.first) {
					txn_error = "Contraction policy 'alive' blocks reduction of '" + update_key +
								"' on currently-alive lot '" + lot_name + "'. Set admin_override to bypass.";
					return false;
				}
			}
		}
	}

	// Update the appropriate field
	if (update_key == "dedicated_GB")
		mpa.dedicated_GB = update_val;
	else if (update_key == "opportunistic_GB")
		mpa.opportunistic_GB = update_val;
	else if (update_key == "max_num_objects")
		mpa.max_num_objects = static_cast<int64_t>(update_val);
	else if (update_key == "creation_time")
		mpa.creation_time = static_cast<int64_t>(update_val);
	else if (update_key == "expiration_time")
		mpa.expiration_time = static_cast<int64_t>(update_val);
	else if (update_key == "deletion_time")
		mpa.deletion_time = static_cast<int64_t>(update_val);
	else {
		txn_error = "Update key not found or not recognized.";
		return false;
	}

	// After a timestamp update, verify the half-open interval invariant for
	// non-sentinel lots. We deliberately allow an intermediate state where one
	// or two of the three time fields are zero (the non-expiring sentinel is
	// all-or-nothing), because a single update_lot call may set all three
	// fields one at a time inside the same transaction. The final post-loop
	// check in lotman_update_lot rejects any persisting partial-zero state.
	if (update_key == "creation_time" || update_key == "expiration_time") {
		bool any_zero = (mpa.creation_time == 0) || (mpa.expiration_time == 0) || (mpa.deletion_time == 0);
		if (!any_zero && mpa.creation_time >= mpa.expiration_time) {
			txn_error = "Update would make creation_time >= expiration_time on lot '" + lot_name +
						"' (half-open interval [creation, expiration) must be non-empty)";
			return false;
		}
	}

	try {
		storage.replace(mpa);
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to write MPA update: ") + e.what();
		return false;
	}

	// If timestamps changed, re-check path temporal overlaps with other lots
	if (update_key == "creation_time" || update_key == "expiration_time") {
		auto lot_paths =
			storage.get_all<db::Path>(where(c(&db::Path::lot_name) == lot_name and c(&db::Path::exclude) == false));
		for (const auto &p : lot_paths) {
			auto overlap = check_path_temporal_overlap(lot_name, p.path, mpa.creation_time, mpa.expiration_time);
			if (!overlap.first) {
				txn_error = "MPA update on '" + lot_name + "' would create path conflict: " + overlap.second;
				return false;
			}
		}
	}

	// Re-validate axioms after MPA update
	// check_children=true because changing this lot's MPAs can break axioms
	// for children whose attributions reference this lot.
	auto vr = apply_validation_predicates(build_axiom_predicates(lot_name, /*check_children=*/true));
	if (!vr.first) {
		txn_error = "MPA update on '" + lot_name + "' would violate hierarchy: " + vr.second;
		return false;
	}

	return true;
}

std::pair<bool, std::string>
lotman::Lot::update_parent_usage(Lot parent, const std::string &update_stmt,
								 const std::map<std::string, std::vector<int>> &update_str_map,
								 const std::map<int64_t, std::vector<int>> &update_int_map,
								 const std::map<double, std::vector<int>> &update_dbl_map) {
	auto rp = parent.store_updates(update_stmt, update_str_map, update_int_map, update_dbl_map);
	if (!rp.first) { // There was an error
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to store_updates for parent: ";
		return std::make_pair(false, ext_err + int_err);
	}
	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Lot::update_self_usage(const std::string &key, const double value,
															bool deltaMode) {

	/*
	Function Flow for update_lot_usage:
	* Sanitize inputs by making sure key is allowed/known
	* Get the current usage, used to calculate delta for updating parents' children_* columns
	* Calculate delta
	* Update lot proper
	* For each parent of lot proper, update children_key += delta
	*/

	std::array<std::string, 2> allowed_int_keys = {"self_objects", "self_objects_being_written"};
	std::array<std::string, 2> allowed_double_keys = {"self_GB", "self_GB_being_written"};

	// Validate key before any SQL construction (defense-in-depth against column-name injection)
	if (std::find(allowed_int_keys.begin(), allowed_int_keys.end(), key) == allowed_int_keys.end() &&
		std::find(allowed_double_keys.begin(), allowed_double_keys.end(), key) == allowed_double_keys.end()) {
		return std::make_pair(false, "Unrecognized usage key: " + key);
	}

	std::string children_key =
		"children" + key.substr(4); // here, we strip out the "self" from the key to target the children col
	std::string update_parent_usage_stmt =
		"UPDATE lot_usage SET " + children_key + " = " + children_key + " + ? WHERE lot_name = ?;";

	// Get the current usage, which is needed in later sections
	std::string get_usage_query = "SELECT " + key + " FROM lot_usage WHERE lot_name = ?;";
	std::map<std::string, std::vector<int>> get_usage_query_str_map{{lot_name, {1}}};
	auto rp_vec_str = lotman::db::SQL_get_matches(get_usage_query, get_usage_query_str_map);

	if (!rp_vec_str.second.empty()) { // There was an error
		std::string int_err = rp_vec_str.second;
		std::string ext_err = "Failure on call to SQL_get_matches: ";
		return std::make_pair(false, ext_err + int_err);
	}

	if (deltaMode) {
		std::map<std::string, std::vector<int>> update_usage_str_map = {{lot_name, {2}}};
		std::string update_usage_delta_stmt = "UPDATE lot_usage SET " + key + " = " + key + " + ? WHERE lot_name = ?;";

		if (std::find(allowed_int_keys.begin(), allowed_int_keys.end(), key) != allowed_int_keys.end()) {
			int delta = value;
			int current_usage = std::stod(rp_vec_str.first[0]);

			if (current_usage + delta < 0) {
				return std::make_pair(
					false,
					"The attempted delta update would result in storing negative values for the key " + key + ".");
			}

			// Store updates for lot proper
			std::map<int64_t, std::vector<int>> update_usage_int_map = {{value, {1}}};
			auto rp_bool_str = this->store_updates(update_usage_delta_stmt, update_usage_str_map, update_usage_int_map);
			if (!rp_bool_str.first) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to store_updates: ";
				return std::make_pair(false, ext_err + int_err);
			}

			// // Get the parents that need to be updated
			// auto rp_lotvec_str = this->get_parents(true);
			// if (!rp_lotvec_str.second.empty()) { // There was an error
			//     std::string int_err = rp_lotvec_str.second;
			//     std::string ext_err = "Failure on call to get_parents: ";
			//     return std::make_pair(false, ext_err + int_err);
			// }

			// for (const auto &parent : recursive_parents) {
			//     std::map<std::string, std::vector<int>> update_parent_str_map{{parent.lot_name, {2}}};
			//     std::map<int64_t, std::vector<int>> update_parent_int_map{{delta, {1}}}; // Update children_key to
			//     current_usage + delta rp_bool_str = this->update_parent_usage(parent, update_parent_usage_stmt,
			//     update_parent_str_map, update_parent_int_map); if (!rp_bool_str.first) { // There was an error
			//         std::string int_err = rp_bool_str.second;
			//         std::string ext_err = "Failure on call to store_updates when updating parent usage: ";
			//         return std::make_pair(false, ext_err + int_err);
			//     }
			// }
		} else if (std::find(allowed_double_keys.begin(), allowed_double_keys.end(), key) !=
				   allowed_double_keys.end()) {
			double delta = value;
			double current_usage = std::stod(rp_vec_str.first[0]);

			if (current_usage + delta < 0) {
				return std::make_pair(
					false,
					"The attempted delta update would result in storing negative values for the key " + key + ".");
			}

			std::map<double, std::vector<int>> update_usage_double_map = {{value, {1}}};
			std::map<int64_t, std::vector<int>> plc_hldr_int_map;
			auto rp_bool_str = this->store_updates(update_usage_delta_stmt, update_usage_str_map, plc_hldr_int_map,
												   update_usage_double_map);
			if (!rp_bool_str.first) { // There was an error
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to store_updates for lot proper: ";
				return std::make_pair(false, ext_err + int_err);
			}

			// // Get parents to be updated
			// auto rp_lotvec_str = this->get_parents(true);
			// if (!rp_lotvec_str.second.empty()) { // There was an error
			//     std::string int_err = rp_lotvec_str.second;
			//     std::string ext_err = "Failure on call to get_parents: ";
			//     return std::make_pair(false, ext_err + int_err);
			// }

			// // Update the parents
			// for (const auto &parent : recursive_parents) {
			//     std::map<std::string, std::vector<int>> update_parent_str_map{{parent.lot_name, {2}}};
			//     std::map<double, std::vector<int>> update_parent_dbl_map{{delta, {1}}}; // Update children_key to
			//     current_usage + delta std::map<int64_t, std::vector<int>> plc_hldr_int_map; rp_bool_str =
			//     this->update_parent_usage(parent, update_parent_usage_stmt, update_parent_str_map, plc_hldr_int_map,
			//     update_parent_dbl_map); if (!rp_bool_str.first) { // There was an error
			//         std::string int_err = rp_bool_str.second;
			//         std::string ext_err = "Failure on call to store_updates";
			//         return std::make_pair(false, ext_err + int_err);
			//     }
			// }
		}
	} else {
		std::map<std::string, std::vector<int>> update_usage_str_map = {{lot_name, {2}}};
		std::string update_usage_stmt = "UPDATE lot_usage SET " + key + "=? WHERE lot_name=?;";
		std::string get_usage_query = "SELECT " + key + " FROM lot_usage WHERE lot_name = ?;";

		// std::string children_key = "children" + key.substr(4);
		std::string parent_usage_query = "SELECT " + children_key + " FROM lot_usage WHERE lot_name = ?;";
		// std::string update_parent_usage_stmt = "UPDATE lot_usage SET " + children_key + "=? WHERE lot_name=?;";

		if (std::find(allowed_int_keys.begin(), allowed_int_keys.end(), key) != allowed_int_keys.end()) {
			int current_usage = std::stoi(rp_vec_str.first[0]);
			int delta = value - current_usage;

			// Update lot proper
			std::map<int64_t, std::vector<int>> update_usage_int_map = {{value, {1}}};

			auto rp_bool_str = this->store_updates(update_usage_stmt, update_usage_str_map, update_usage_int_map);
			if (!rp_bool_str.first) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to store_updates: ";
				return std::make_pair(false, ext_err + int_err);
			}

			/* TODO: need some kind of recovery file if we start updating parents and then fail --> for each parent,
			   store  in temp file current usage for key and delete temp file after done. Use a function "repair_db"
			   that checks for existence of temp file and restores things to those values, deleting the temp file upon
			   completion.
			*/

			// // Update parents
			// auto rp_lotvec_str = this->get_parents(true);
			// if (!rp_lotvec_str.second.empty()) { // There was an error
			//     std::string int_err = rp_lotvec_str.second;
			//     std::string ext_err = "Failure on call to get_parents: ";
			//     return std::make_pair(false, ext_err + int_err);
			// }

			// for (const auto &parent : recursive_parents) {
			//     std::map<std::string, std::vector<int>> parent_usage_query_str_map{{parent.lot_name, {1}}};
			//     auto rp_vec_str = lotman::db::SQL_get_matches(parent_usage_query, parent_usage_query_str_map);
			//     if (!rp_vec_str.second.empty()) { // There was an error
			//         std::string int_err = rp_vec_str.second;
			//         std::string ext_err = "Failure on call to get_parents: ";
			//         return std::make_pair(false, ext_err + int_err);
			//     }
			//     int current_usage = std::stoi(rp_vec_str.first[0]);
			//     std::map<std::string, std::vector<int>> update_parent_str_map{{parent.lot_name, {2}}};
			//     std::map<int64_t, std::vector<int>> update_parent_dbl_map{{delta, {1}}}; // Update children_key to
			//     current_usage + delta rp_bool_str = this->update_parent_usage(parent, update_parent_usage_stmt,
			//     update_parent_str_map, update_parent_dbl_map); if (!rp_bool_str.first) { // There was an error
			//         std::string int_err = rp_bool_str.second;
			//         std::string ext_err = "Failure on call to store_updates when updating parent usage: ";
			//         return std::make_pair(false, ext_err + int_err);
			//     }
			// }

		} else if (std::find(allowed_double_keys.begin(), allowed_double_keys.end(), key) !=
				   allowed_double_keys.end()) {
			double current_usage = std::stod(rp_vec_str.first[0]);
			double delta = value - current_usage;

			// Update lot proper
			std::map<std::string, std::vector<int>> update_usage_str_map = {{lot_name, {2}}};
			std::map<double, std::vector<int>> update_usage_dbl_map = {{value, {1}}};
			std::map<int64_t, std::vector<int>> plc_hldr_int_map;
			auto rp_bool_str =
				this->store_updates(update_usage_stmt, update_usage_str_map, plc_hldr_int_map, update_usage_dbl_map);
			if (!rp_bool_str.first) { // There was an error
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to store_updates for lot proper: ";
				return std::make_pair(false, ext_err + int_err);
			}

			// // Update parents
			// auto rp_lotvec_str = this->get_parents(true);
			// if (!rp_lotvec_str.second.empty()) { // There was an error
			//     std::string int_err = rp_lotvec_str.second;
			//     std::string ext_err = "Failure on call to get_parents: ";
			//     return std::make_pair(false, ext_err + int_err);
			// }

			// std::string children_key = "children" + key.substr(4);
			// for (const auto &parent : recursive_parents) {
			//     std::map<std::string, std::vector<int>> parent_usage_query_str_map{{parent.lot_name, {1}}};
			//     rp_vec_str = lotman::db::SQL_get_matches(parent_usage_query, parent_usage_query_str_map);
			//     if (!rp_vec_str.second.empty()) { // There was an error
			//         std::string int_err = rp_vec_str.second;
			//         std::string ext_err = "Failure on call to SQL_get_matches: ";
			//         return std::make_pair(false, ext_err + int_err);
			//     }
			//     double current_usage = std::stod(rp_vec_str.first[0]);
			//     std::map<std::string, std::vector<int>> update_parent_str_map{{parent.lot_name, {2}}};
			//     std::map<double, std::vector<int>> update_parent_dbl_map{{delta, {1}}}; // Update children_key to
			//     current_usage + delta std::map<int64_t, std::vector<int>> plc_hldr_int_map; rp_bool_str =
			//     this->update_parent_usage(parent, update_parent_usage_stmt, update_parent_str_map, plc_hldr_int_map,
			//     update_parent_dbl_map); if (!rp_bool_str.first) { // There was an error
			//         std::string int_err = rp_bool_str.second;
			//         std::string ext_err = "Failure on call to store_updates for parents: ";
			//         return std::make_pair(false, ext_err + int_err);
			//     }
			// }
		}
	}
	return std::make_pair(true, "");
}

// SECTION UNDER MAINTENANCE

std::pair<bool, std::string> lotman::Lot::update_db_children_usage() {
	/*
	Function flow:
	- enumerate all the lots
	- For each lot, update it's children_usage.
	*/

	// Enumerate lots
	auto rp = list_all_lots();
	if (!rp.second.empty()) {
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to lotman::Lot::list_all_lots: ";
		return std::make_pair(false, ext_err + int_err);
	}

	// For each lot, update usage
	for (auto &lot_name : rp.first) {
		Lot lot(lot_name);
		auto rp_bool_str = lot.recalculate_children_usage();
		if (!rp_bool_str.first) {
			std::string int_err = rp_bool_str.second;
			std::string ext_err = "Failure on call to recalculate_children_usage for lot " + lot_name + ": ";
		}
	}

	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Lot::recalculate_children_usage() {
	/*
	Function flow for the lot this is being called on:
	- Get all the children
	- Sum the childrens' usage stats
	- Write to db
	*/

	if (!recursive_children_loaded) {
		this->get_children(true, false);
	}

	std::vector<std::vector<std::string>> updated_usages;
	if (recursive_children.size() > 0) {
		std::map<std::string, std::vector<int>> sum_str_map{};
		std::string sum_query =
			"SELECT SUM(self_GB), SUM(self_GB_being_written), SUM(self_objects), SUM(self_objects_being_written) "
			"FROM lot_usage WHERE lot_name IN (";

		// For each child we need to update both the query and the str map
		for (int i = 0; i < recursive_children.size(); i++) {
			// Update the query
			sum_query += "?";
			if (i != recursive_children.size() - 1) {
				sum_query += ", ";
			}

			// Update the str map
			sum_str_map[recursive_children[i].lot_name] = {i + 1};
		}
		sum_query += ");";

		// Get the sum
		auto rp_vec_vec_str = lotman::db::SQL_get_matches_multi_col(sum_query, 4, sum_str_map);
		if (!rp_vec_vec_str.second.empty()) {
			std::string int_err = rp_vec_vec_str.second;
			std::string ext_err = "Failure on call to SQL_get_matches_multi_col while summing child usage: ";
			return std::make_pair(false, ext_err + int_err);
		}
		if (rp_vec_vec_str.first.size() == 0) {
			return std::make_pair(false, "lotman::db::SQL_get_matches_multi_col returned an empty vector when "
										 "querying for child usage sums, but it shouldn't have");
		}
		updated_usages = rp_vec_vec_str.first;
	} else {
		updated_usages.push_back({"0", "0", "0", "0"}); // set the values to 0, there are no children!
	}

	// // Get current usages
	// std::string current_usage_query =   "SELECT children_GB, children_GB_being_written, children_objects,
	// children_objects_being_written "
	//                                     "FROM lot_usage WHERE lot_name = ?;";
	// std::map<std::string, std::vector<int>> current_usage_str_map{{this->lot_name, {1}}};
	// auto rp_vec_vec_str = lotman::db::SQL_get_matches_multi_col(current_usage_query, 4, current_usage_str_map);
	// if (!rp_vec_vec_str.second.empty()) {
	//     std::string int_err = rp_vec_vec_str.second;
	//     std::string ext_err = "Failure on call to SQL_get_matches_multi_col while getting current lot's child usage:
	//     "; return std::make_pair(false, ext_err + int_err);
	// }
	// if (rp_vec_vec_str.first.size() == 0) {
	//     return std::make_pair(false, "lotman::db::SQL_get_matches_multi_col returned an empty vector when
	//     querying for the lot's current child usage, but it shouldn't have");
	// }
	// std::vector<std::vector<std::string>> outdated_usages = rp_vec_vec_str.first;

	// // Calculate the deltas
	double children_GB = 0, children_GB_being_written = 0;
	int64_t children_objects = 0, children_objects_being_written = 0;
	children_GB = std::stod(updated_usages[0][0]);
	children_GB_being_written = std::stod(updated_usages[0][1]);
	// std::stoi will narrow the number, which should be int64_t, so we interpret string as double and cast to int64_t
	children_objects = (int64_t)std::stod(updated_usages[0][2]);
	children_objects_being_written = (int64_t)std::stod(updated_usages[0][3]);

	std::string update_stmt = "UPDATE lot_usage "
							  "SET "
							  "children_GB = ?, children_GB_being_written = ?, "
							  "children_objects = ?, children_objects_being_written = ? "
							  "WHERE lot_name = ?;";

	std::map<std::string, std::vector<int>> update_str_map{{this->lot_name, {5}}};
	std::map<double, std::vector<int>> update_dbl_map;
	std::map<int64_t, std::vector<int>> update_int_map;
	if (children_GB == children_GB_being_written) {
		update_dbl_map = {{children_GB, {1, 2}}};
	} else {
		update_dbl_map = {{children_GB, {1}}, {children_GB_being_written, {2}}};
	}
	if (children_objects == children_objects_being_written) {
		update_int_map = {{children_objects, {3, 4}}};
	} else {
		update_int_map = {{children_objects, {3}}, {children_objects_being_written, {4}}};
	}

	// Perform the updates
	auto rp_bool_str = lotman::Lot::store_updates(update_stmt, update_str_map, update_int_map, update_dbl_map);
	if (!rp_bool_str.first) {
		std::string int_err = rp_bool_str.second;
		std::string ext_err = "Failure while storing child usage delta updates: ";
		return std::make_pair(false, ext_err + int_err);
	}

	return std::make_pair(true, "");
}

// END SECTION

std::pair<bool, std::string> lotman::Lot::update_usage_by_dirs(const json &update_JSON, bool deltaMode,
															   int64_t query_time) {
	// TODO: Should lots who don't show up when connecting lots to dirs be reset to have
	//       0 usage, or should the be kept the way they are?
	// --> kept the way they are, probably.

	DirUsageUpdate dirUpdate;
	dirUpdate.m_query_time = query_time;
	std::vector<Lot> return_lots;
	auto rp = dirUpdate.JSON_math(update_JSON, &return_lots);
	if (!rp.first) {
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to JSON_math: ";
		return std::make_pair(false, ext_err + int_err);
	}

	for (auto &lot : return_lots) {
		// Since we don't know the lots beforehand, we have to check for their existence here.
		auto exists = lot_exists(lot.lot_name);
		if (!exists.second.empty()) {
			std::string int_err = exists.second;
			std::string ext_err = "Failed to check if lot exists: ";
			return std::make_pair(false, ext_err + int_err);
		}

		if (!exists.first) {
			std::string err = "The lot " + lot.lot_name + " does not exist in the db, so it cannot be updated...";
			return std::make_pair(false, err);
		}

		if (lot.usage.self_GB_update_staged) {
			auto rp_bool_str = lot.update_self_usage("self_GB", lot.usage.self_GB, deltaMode);
			if (!rp_bool_str.first) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure to update lot's self_GB: ";
				return std::make_pair(false, ext_err + int_err);
			}
		}

		if (lot.usage.self_objects_update_staged) {
			auto rp_bool_str = lot.update_self_usage("self_objects", lot.usage.self_objects, deltaMode);
			if (!rp_bool_str.first) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure to update lot's self_objects: ";
				return std::make_pair(false, ext_err + int_err);
			}
		}

		if (lot.usage.self_GB_being_written_update_staged) {
			auto rp_bool_str =
				lot.update_self_usage("self_GB_being_written", lot.usage.self_GB_being_written, deltaMode);
			if (!rp_bool_str.first) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure to update lot's self_GB_being_written: ";
				return std::make_pair(false, ext_err + int_err);
			}
		}

		if (lot.usage.self_objects_being_written_update_staged) {
			auto rp_bool_str =
				lot.update_self_usage("self_objects_being_written", lot.usage.self_objects_being_written, deltaMode);
			if (!rp_bool_str.first) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure to update lot's self_objects_being_written: ";
				return std::make_pair(false, ext_err + int_err);
			}
		}
	}

	return std::make_pair(true, "");
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::get_lots_past_exp(const bool recursive) {
	std::vector<std::string> expired_lots;
	auto now = std::chrono::system_clock::now();
	int64_t ms_since_epoch = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();

	std::string expired_query =
		"SELECT lot_name FROM management_policy_attributes WHERE expiration_time != 0 AND expiration_time <= ?;";
	std::map<int64_t, std::vector<int>> expired_map{{ms_since_epoch, {1}}};
	auto rp = lotman::db::SQL_get_matches(expired_query, std::map<std::string, std::vector<int>>(), expired_map);
	if (!rp.second.empty()) { // There was an error
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to SQL_get_matches: ";
		return std::make_pair(std::vector<std::string>(), ext_err + int_err);
	}

	expired_lots = rp.first;
	if (recursive) { // Any child of an expired lot is also expired
		std::vector<std::string> tmp;
		for (auto &lot_name : expired_lots) {
			Lot _lot(lot_name);
			auto rp_lotvec_str = _lot.get_children(true);
			if (!rp_lotvec_str.second.empty()) { // There was an error
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_children.";
				return std::make_pair(std::vector<std::string>(), ext_err + int_err);
			}

			std::vector<std::string> tmp;
			for (const auto &child : _lot.recursive_children) {
				tmp.push_back(child.lot_name);
			}
		}
		expired_lots.insert(expired_lots.end(), tmp.begin(), tmp.end());

		// Sort and remove any duplicates
		std::sort(expired_lots.begin(), expired_lots.end());
		auto last = std::unique(expired_lots.begin(), expired_lots.end());
		expired_lots.erase(last, expired_lots.end());
	}

	return std::make_pair(expired_lots, "");
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::get_lots_past_del(const bool recursive) {
	auto now = std::chrono::system_clock::now();
	int64_t ms_since_epoch = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();

	std::string deletion_query =
		"SELECT lot_name FROM management_policy_attributes WHERE deletion_time != 0 AND deletion_time <= ?;";
	std::map<int64_t, std::vector<int>> deletion_map{{ms_since_epoch, {1}}};
	auto rp = lotman::db::SQL_get_matches(deletion_query, std::map<std::string, std::vector<int>>(), deletion_map);
	if (!rp.second.empty()) { // There was an error
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to SQL_get_matches: ";
		return std::make_pair(std::vector<std::string>(), ext_err + int_err);
	}

	std::vector<std::string> deletion_lots = rp.first;

	if (recursive) { // Any child of an expired lot is also expired
		std::vector<std::string> tmp;
		for (auto &lot_name : deletion_lots) {
			Lot _lot(lot_name);
			auto rp_lotvec_str = _lot.get_children(true);
			if (!rp_lotvec_str.second.empty()) { // There was an error
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_children.";
				return std::make_pair(std::vector<std::string>(), ext_err + int_err);
			}

			for (const auto &child : _lot.recursive_children) {
				tmp.push_back(child.lot_name);
			}
		}
		deletion_lots.insert(deletion_lots.end(), tmp.begin(), tmp.end());

		// Sort and remove any duplicates
		std::sort(deletion_lots.begin(), deletion_lots.end());
		auto last = std::unique(deletion_lots.begin(), deletion_lots.end());
		deletion_lots.erase(last, deletion_lots.end());
	}

	return std::make_pair(deletion_lots, "");
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::get_lots_past_opp(const bool recursive_quota,
																				const bool recursive_children) {
	std::vector<std::string> lots_past_opp;
	if (recursive_quota) {
		// Skip lots whose dedicated or opportunistic axis is the unbounded
		// sentinel (-1). When dedicated_GB == -1 the storage axis is fully
		// unbounded; when opportunistic_GB == -1 the burst capacity is
		// unbounded, so the (ded+opp) total is meaningless. In either case
		// the lot can never be "past" its opportunistic quota.
		std::string rec_opp_usage_query =
			"SELECT "
			"lot_usage.lot_name "
			"FROM lot_usage "
			"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
			"WHERE management_policy_attributes.dedicated_GB != -1 "
			"  AND management_policy_attributes.opportunistic_GB != -1 "
			"AND lot_usage.self_GB + lot_usage.children_GB >= management_policy_attributes.dedicated_GB + "
			"management_policy_attributes.opportunistic_GB;";
		auto rp = lotman::db::SQL_get_matches(rec_opp_usage_query);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(std::vector<std::string>(), ext_err + int_err);
		}
		lots_past_opp = rp.first;
	} else {
		std::string opp_usage_query =
			"SELECT "
			"lot_usage.lot_name "
			"FROM lot_usage "
			"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
			"WHERE management_policy_attributes.dedicated_GB != -1 "
			"  AND management_policy_attributes.opportunistic_GB != -1 "
			"AND lot_usage.self_GB >= management_policy_attributes.dedicated_GB + "
			"management_policy_attributes.opportunistic_GB;";

		auto rp = lotman::db::SQL_get_matches(opp_usage_query);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(std::vector<std::string>(), ext_err + int_err);
		}
		lots_past_opp = rp.first;
	}

	if (recursive_children) { // Get all children of the lots past opp
		std::vector<std::string> tmp;
		for (const auto lot_past_opp : lots_past_opp) {
			Lot _lot(lot_past_opp);
			auto rp_lotvec_str = _lot.get_children(true);
			if (!rp_lotvec_str.second.empty()) { // There was an error
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_children.";
				return std::make_pair(std::vector<std::string>(), ext_err + int_err);
			}

			for (const auto &child : _lot.recursive_children) {
				tmp.push_back(child.lot_name);
			}
		}
		lots_past_opp.insert(lots_past_opp.end(), tmp.begin(), tmp.end());

		// Sort and remove any duplicates
		std::sort(lots_past_opp.begin(), lots_past_opp.end());
		auto last = std::unique(lots_past_opp.begin(), lots_past_opp.end());
		lots_past_opp.erase(last, lots_past_opp.end());
	}

	return std::make_pair(lots_past_opp, "");
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::get_lots_past_ded(const bool recursive_quota,
																				const bool recursive_children) {
	std::vector<std::string> lots_past_ded;
	if (recursive_quota) {
		// Skip lots with dedicated_GB == -1 (unbounded-dedicated sentinel).
		// A lot with dedicated_GB == 0 (literal: no guaranteed storage) is
		// kept and is "past" its dedicated quota the moment it has any usage.
		std::string rec_ded_usage_query =
			"SELECT "
			"lot_usage.lot_name "
			"FROM lot_usage "
			"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
			"WHERE management_policy_attributes.dedicated_GB != -1 "
			"AND lot_usage.self_GB + lot_usage.children_GB >= management_policy_attributes.dedicated_GB;";

		auto rp = lotman::db::SQL_get_matches(rec_ded_usage_query);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(std::vector<std::string>(), ext_err + int_err);
		}
		lots_past_ded = rp.first;
	} else {
		std::string ded_usage_query =
			"SELECT "
			"lot_usage.lot_name "
			"FROM lot_usage "
			"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
			"WHERE management_policy_attributes.dedicated_GB != -1 "
			"AND lot_usage.self_GB >= management_policy_attributes.dedicated_GB;";

		auto rp = lotman::db::SQL_get_matches(ded_usage_query);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(std::vector<std::string>(), ext_err + int_err);
		}
		lots_past_ded = rp.first;
	}

	if (recursive_children) { // Get all children of the lots past opp
		std::vector<std::string> tmp;
		for (const auto lot_past_ded : lots_past_ded) {
			Lot _lot(lot_past_ded);
			auto rp_lotvec_str = _lot.get_children(true);
			if (!rp_lotvec_str.second.empty()) { // There was an error
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_children.";
				return std::make_pair(std::vector<std::string>(), ext_err + int_err);
			}

			for (const auto &child : _lot.recursive_children) {
				tmp.push_back(child.lot_name);
			}
		}
		lots_past_ded.insert(lots_past_ded.end(), tmp.begin(), tmp.end());

		// Sort and remove any duplicates
		std::sort(lots_past_ded.begin(), lots_past_ded.end());
		auto last = std::unique(lots_past_ded.begin(), lots_past_ded.end());
		lots_past_ded.erase(last, lots_past_ded.end());
	}

	return std::make_pair(lots_past_ded, "");
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::get_lots_past_obj(const bool recursive_quota,
																				const bool recursive_children) {
	std::vector<std::string> lots_past_obj;
	if (recursive_quota) {
		// Skip lots with max_num_objects == -1 (unbounded-objects sentinel).
		std::string rec_obj_usage_query =
			"SELECT "
			"lot_usage.lot_name "
			"FROM lot_usage "
			"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
			"WHERE management_policy_attributes.max_num_objects != -1 "
			"AND lot_usage.self_objects + lot_usage.children_objects >= "
			"management_policy_attributes.max_num_objects;";

		auto rp = lotman::db::SQL_get_matches(rec_obj_usage_query);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(std::vector<std::string>(), ext_err + int_err);
		}
		lots_past_obj = rp.first;
	} else {
		std::string obj_usage_query =
			"SELECT "
			"lot_usage.lot_name "
			"FROM lot_usage "
			"INNER JOIN management_policy_attributes ON lot_usage.lot_name=management_policy_attributes.lot_name "
			"WHERE management_policy_attributes.max_num_objects != -1 "
			"AND lot_usage.self_objects >= management_policy_attributes.max_num_objects;";

		auto rp = lotman::db::SQL_get_matches(obj_usage_query);
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to SQL_get_matches: ";
			return std::make_pair(std::vector<std::string>(), ext_err + int_err);
		}
		lots_past_obj = rp.first;
	}

	if (recursive_children) { // Get all children of the lots past opp
		std::vector<std::string> tmp;
		for (const auto lot_past_obj : lots_past_obj) {
			Lot _lot(lot_past_obj);
			auto rp_lotvec_str = _lot.get_children(true);
			if (!rp_lotvec_str.second.empty()) { // There was an error
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_children.";
				return std::make_pair(std::vector<std::string>(), ext_err + int_err);
			}

			for (const auto &child : _lot.recursive_children) {
				tmp.push_back(child.lot_name);
			}
		}
		lots_past_obj.insert(lots_past_obj.end(), tmp.begin(), tmp.end());

		// Sort and remove any duplicates
		std::sort(lots_past_obj.begin(), lots_past_obj.end());
		auto last = std::unique(lots_past_obj.begin(), lots_past_obj.end());
		lots_past_obj.erase(last, lots_past_obj.end());
	}

	return std::make_pair(lots_past_obj, "");
}

// Helper: sort lot names by depth descending (deepest/leaf first).
// Uses a single recursive CTE instead of N+1 individual queries.
static void sort_by_depth_descending(std::vector<std::string> &lots) {
	if (lots.empty())
		return;

	std::string depth_query = "WITH RECURSIVE depth_cte(lot_name, depth) AS ("
							  "  SELECT lot_name, 0 FROM parents WHERE lot_name = parent "
							  "  UNION ALL "
							  "  SELECT p.lot_name, d.depth + 1 "
							  "  FROM parents p JOIN depth_cte d ON p.parent = d.lot_name "
							  "  WHERE p.lot_name != p.parent"
							  ") "
							  "SELECT lot_name, MAX(depth) FROM depth_cte GROUP BY lot_name ORDER BY MAX(depth) DESC;";

	auto rp = lotman::db::SQL_get_matches_multi_col(depth_query, 2);
	if (!rp.second.empty() || rp.first.empty())
		return; // On error, leave unsorted

	// Build lot_name → depth map
	std::map<std::string, int> depth_map;
	for (const auto &row : rp.first) {
		if (row.size() >= 2) {
			try {
				depth_map[row[0]] = std::stoi(row[1]);
			} catch (...) {
				depth_map[row[0]] = 0;
			}
		}
	}

	std::sort(lots.begin(), lots.end(), [&depth_map](const std::string &a, const std::string &b) {
		int da = depth_map.count(a) ? depth_map.at(a) : 0;
		int db = depth_map.count(b) ? depth_map.at(b) : 0;
		return da > db;
	});
}

// Parameterized helper for hierarchical lot-threshold queries.
// Builds an "adjusted usage" query that accounts for children's overage,
// executes it, and returns results sorted by depth (deepest first).
//
// Parameters:
//   self_usage_col: the lot_usage column for the lot's own usage (e.g. "self_GB")
//   child_usage_expr: aggregate expression for a child's total usage
//                     (e.g. "c_usage.self_GB + c_usage.children_GB")
//   child_threshold_expr: the child's MPA threshold expression
//                         (e.g. "c_mpa.dedicated_GB" or "c_mpa.dedicated_GB + c_mpa.opportunistic_GB")
//   parent_threshold_expr: the parent's MPA threshold to compare against
//                          (e.g. "p_mpa.dedicated_GB" or "p_mpa.dedicated_GB + p_mpa.opportunistic_GB")
//   error_context: string for error messages
static std::pair<std::vector<std::string>, std::string>
get_lots_past_threshold_hierarchical(const std::string &self_usage_col, const std::string &child_usage_expr,
									 const std::string &child_threshold_expr, const std::string &parent_threshold_expr,
									 const std::string &parent_unbounded_predicate,
									 const std::string &child_unbounded_predicate, const std::string &error_context) {

	// Defense-in-depth: validate all SQL fragments against known-good values.
	// All callers pass compile-time constants, but this prevents future misuse.
	static const std::set<std::string> allowed_usage_cols = {"self_GB", "self_objects"};
	static const std::set<std::string> allowed_exprs = {"c_usage.self_GB + c_usage.children_GB",
														"c_usage.self_objects + c_usage.children_objects",
														"c_mpa.dedicated_GB",
														"c_mpa.dedicated_GB + c_mpa.opportunistic_GB",
														"c_mpa.max_num_objects",
														"p_mpa.dedicated_GB",
														"p_mpa.dedicated_GB + p_mpa.opportunistic_GB",
														"p_mpa.max_num_objects"};
	static const std::set<std::string> allowed_unbounded_predicates = {
		"p_mpa.dedicated_GB = -1",
		"p_mpa.dedicated_GB = -1 OR p_mpa.opportunistic_GB = -1",
		"p_mpa.max_num_objects = -1",
		"c_mpa.dedicated_GB = -1",
		"c_mpa.dedicated_GB = -1 OR c_mpa.opportunistic_GB = -1",
		"c_mpa.max_num_objects = -1"};

	if (allowed_usage_cols.find(self_usage_col) == allowed_usage_cols.end() ||
		allowed_exprs.find(child_usage_expr) == allowed_exprs.end() ||
		allowed_exprs.find(child_threshold_expr) == allowed_exprs.end() ||
		allowed_exprs.find(parent_threshold_expr) == allowed_exprs.end() ||
		allowed_unbounded_predicates.find(parent_unbounded_predicate) == allowed_unbounded_predicates.end() ||
		allowed_unbounded_predicates.find(child_unbounded_predicate) == allowed_unbounded_predicates.end()) {
		return std::make_pair(std::vector<std::string>(),
							  "Internal error: unrecognized SQL fragment in hierarchical query");
	}

	// Per-axis sentinel handling (-1 marks an axis as unbounded):
	//   * Parents that are unbounded on the relevant axis are excluded
	//     entirely -- they can never be "past" their quota on that axis.
	//   * Children that are unbounded on the axis can only legally exist
	//     under an unbounded parent (rejected by axiom 1 otherwise);
	//     defensively we treat their overflow contribution as 0 so they can
	//     never push a parent past its own quota.
	std::string query = "SELECT p_usage.lot_name "
						"FROM lot_usage p_usage "
						"JOIN management_policy_attributes p_mpa ON p_usage.lot_name = p_mpa.lot_name "
						"WHERE NOT (" +
						parent_unbounded_predicate +
						") "
						"AND p_usage." +
						self_usage_col +
						" + COALESCE("
						"    (SELECT SUM(CASE WHEN (" +
						child_unbounded_predicate + ") THEN 0 ELSE MAX(0, (" + child_usage_expr + ") - (" +
						child_threshold_expr +
						")) END) "
						"     FROM parents c_par "
						"     JOIN lot_usage c_usage ON c_par.lot_name = c_usage.lot_name "
						"     JOIN management_policy_attributes c_mpa ON c_par.lot_name = c_mpa.lot_name "
						"     WHERE c_par.parent = p_usage.lot_name AND c_par.lot_name != c_par.parent), 0"
						") >= " +
						parent_threshold_expr + ";";

	auto rp = lotman::db::SQL_get_matches(query);
	if (!rp.second.empty()) {
		return std::make_pair(std::vector<std::string>(), "Failure on " + error_context + ": " + rp.second);
	}

	auto lots = rp.first;
	sort_by_depth_descending(lots);
	return std::make_pair(lots, "");
}

std::pair<std::vector<std::string>, std::string>
lotman::Lot::get_lots_past_ded(const bool recursive_quota, const bool recursive_children, const bool hierarchical) {
	if (!hierarchical) {
		return get_lots_past_ded(recursive_quota, recursive_children);
	}
	return get_lots_past_threshold_hierarchical("self_GB", "c_usage.self_GB + c_usage.children_GB",
												"c_mpa.dedicated_GB", "p_mpa.dedicated_GB", "p_mpa.dedicated_GB = -1",
												"c_mpa.dedicated_GB = -1", "adjusted dedicated query");
}

std::pair<std::vector<std::string>, std::string>
lotman::Lot::get_lots_past_opp(const bool recursive_quota, const bool recursive_children, const bool hierarchical) {
	if (!hierarchical) {
		return get_lots_past_opp(recursive_quota, recursive_children);
	}
	return get_lots_past_threshold_hierarchical(
		"self_GB", "c_usage.self_GB + c_usage.children_GB", "c_mpa.dedicated_GB + c_mpa.opportunistic_GB",
		"p_mpa.dedicated_GB + p_mpa.opportunistic_GB", "p_mpa.dedicated_GB = -1 OR p_mpa.opportunistic_GB = -1",
		"c_mpa.dedicated_GB = -1 OR c_mpa.opportunistic_GB = -1", "adjusted opportunistic query");
}

std::pair<std::vector<std::string>, std::string>
lotman::Lot::get_lots_past_obj(const bool recursive_quota, const bool recursive_children, const bool hierarchical) {
	if (!hierarchical) {
		return get_lots_past_obj(recursive_quota, recursive_children);
	}
	return get_lots_past_threshold_hierarchical(
		"self_objects", "c_usage.self_objects + c_usage.children_objects", "c_mpa.max_num_objects",
		"p_mpa.max_num_objects", "p_mpa.max_num_objects = -1", "c_mpa.max_num_objects = -1", "adjusted objects query");
}

std::pair<json, std::string> lotman::Lot::get_available_capacity(const std::string &parent_lot_name, int64_t start_time,
																 int64_t end_time) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Get parent's MPAs
		auto parent_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(parent_lot_name);
		if (!parent_mpa) {
			return std::make_pair(json(), "Parent lot '" + parent_lot_name + "' not found");
		}

		// Build sweep-line events from children's attributions, clipped to query window
		auto events = build_attribution_events(parent_lot_name, start_time, end_time);
		auto peak = run_sweep_line(events);

		// Per-axis sentinel handling: a parent that is unbounded on a sub-axis
		// has no meaningful "available" value on that axis (the cap is +inf).
		// Report null for those fields rather than a negative or misleading
		// number; callers can detect unbounded by checking for null.
		const bool parent_unb_ded = is_unbounded_dedicated(parent_mpa->dedicated_GB);
		const bool parent_unb_opp = is_unbounded_opportunistic(parent_mpa->opportunistic_GB);
		const bool parent_unbounded_objects = is_unbounded_objects(parent_mpa->max_num_objects);

		json result;
		result["available_dedicated_GB"] =
			parent_unb_ded ? json(nullptr) : json(parent_mpa->dedicated_GB - peak.peak_ded);
		result["available_opportunistic_GB"] =
			parent_unb_opp ? json(nullptr) : json(parent_mpa->opportunistic_GB - peak.peak_opp);
		// available_total_GB is meaningful only when both sub-axes are bounded.
		if (parent_unb_ded || parent_unb_opp) {
			result["available_total_GB"] = nullptr;
		} else {
			double parent_total = parent_mpa->dedicated_GB + parent_mpa->opportunistic_GB;
			result["available_total_GB"] = parent_total - peak.peak_total;
		}
		if (parent_unbounded_objects) {
			result["available_max_num_objects"] = nullptr;
		} else {
			result["available_max_num_objects"] = parent_mpa->max_num_objects - static_cast<int64_t>(peak.peak_obj);
		}
		result["peak_dedicated_GB"] = peak.peak_ded;
		result["peak_opportunistic_GB"] = peak.peak_opp;
		result["peak_max_num_objects"] = static_cast<int64_t>(peak.peak_obj);
		// peak_total is the true max of (ded+opp) at a single point in time
		result["peak_total_GB"] = peak.peak_total;

		return std::make_pair(result, "");
	} catch (const std::exception &e) {
		return std::make_pair(json(), std::string("get_available_capacity failed: ") + e.what());
	}
}

std::pair<std::vector<std::string>, std::string> lotman::Lot::list_all_lots() {
	try {
		auto &storage = db::StorageManager::get_storage();
		// Surjection between lots and owners means we'll get every lot without duplicates.
		auto lot_names = storage.select(&db::Owner::lot_name);
		return std::make_pair(lot_names, "");
	} catch (const std::exception &e) {
		return std::make_pair(std::vector<std::string>(), std::string("list_all_lots failed: ") + e.what());
	}
}

std::pair<std::vector<std::string>, std::string>
lotman::Lot::get_lots_from_dir(const std::string &dir_input, const bool recursive, int64_t query_time) {
	// Normalize: ensure input dir has trailing slash for consistent comparison
	// Database paths always have trailing slashes (e.g., "/foo/bar/")
	std::string dir = ensure_trailing_slash(dir_input);

	// For the LIKE comparison, we need the input path without trailing slash.
	// This is because: stored path "/foo/" with LIKE pattern means we check if
	// the input starts with "/foo/". Using "/foo/bar" LIKE "/foo/" || '%' works,
	// but "/foobar" LIKE "/foo/" || '%' correctly fails (no match).
	std::string dir_for_like = dir;
	if (dir_for_like.length() > 1 && dir_for_like.back() == '/') {
		dir_for_like.pop_back();
	}

	// Query logic for path exclusions:
	// We need to find the best matching path rule for this directory. The algorithm is:
	// 1. Find all path rules (both inclusions and exclusions) that match this directory
	// 2. The longest matching path "wins" - if it's an exclusion, the path is not in that lot
	// 3. If the longest match is an exclusion, we need to find the next longest inclusion
	//    that is NOT overridden by an exclusion
	//
	// The query uses a subquery to check if there's a longer exclusion that would override
	// any given inclusion match.
	//
	// Path matching rules:
	// - path = ?1 : exact match (normalized input matches stored path exactly)
	// - ?2 LIKE path || '%' : input is a subdirectory of a stored recursive path
	// - For non-recursive paths, only exact matches count
	// - exclude = 0 means this is an inclusion (the path IS tracked)
	// - exclude = 1 means this is an exclusion (the path is NOT tracked by this lot)
	//
	// We select the longest non-excluded path that doesn't have a longer exclusion overriding it
	std::string lots_from_dir_query =
		"SELECT p.lot_name FROM paths p "
		"JOIN management_policy_attributes mpa ON p.lot_name = mpa.lot_name "
		"WHERE "
		"(p.path = ?1 OR ?2 LIKE p.path || '%') " // Exact match or subdirectory of stored path
		"AND "
		"(p.recursive OR p.path = ?3) " // If not recursive, only match exact path
		"AND "
		"p.exclude = 0 " // Only consider inclusion paths
		// Lot must be active during the query window. Treat the all-zero
		// timestamp triple as the non-expiring sentinel.
		"AND ((mpa.creation_time = 0 AND mpa.expiration_time = 0 AND mpa.deletion_time = 0) "
		"     OR (mpa.creation_time <= ?4 AND mpa.expiration_time > ?4)) "
		"AND NOT EXISTS ( " // Ensure no longer exclusion overrides this inclusion
		"    SELECT 1 FROM paths e "
		"    WHERE e.lot_name = p.lot_name "			  // Same lot
		"    AND e.exclude = 1 "						  // Is an exclusion
		"    AND (e.path = ?1 OR ?2 LIKE e.path || '%') " // Matches the input path
		"    AND (e.recursive OR e.path = ?3) "			  // Respects recursive flag
		"    AND LENGTH(e.path) > LENGTH(p.path) "		  // Exclusion is more specific (longer)
		") "
		"ORDER BY LENGTH(p.path) DESC LIMIT 1;"; // Prefer longest matching inclusion
	std::map<std::string, std::vector<int>> dir_str_map{{dir, {1, 3}}, {dir_for_like, {2}}};
	std::map<int64_t, std::vector<int>> dir_int_map{{query_time, {4}}};
	auto rp = lotman::db::SQL_get_matches(lots_from_dir_query, dir_str_map, dir_int_map);
	if (!rp.second.empty()) {
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to SQL_get_matches: ";
		return std::make_pair(std::vector<std::string>(), ext_err + int_err);
	}

	std::vector<std::string> matching_lots_vec;
	if (rp.first.empty()) { // No associated lots were found, indicating the directory should be associated with the
							// default lot
		matching_lots_vec = {"default"};
	} else {
		matching_lots_vec = rp.first;
	}

	if (recursive) { // Indicates we want all of the parent lots.
		Lot lot(matching_lots_vec[0]);
		lot.get_parents(true, false);
		for (auto &parent : lot.recursive_parents) {
			matching_lots_vec.push_back(parent.lot_name);
		}
	}

	return std::make_pair(matching_lots_vec, "");
}

std::pair<bool, std::string> lotman::Lot::check_context_for_parents(const std::vector<std::string> &parents,
																	bool include_self, bool new_lot) {
	if (new_lot && parents.size() == 1 &&
		parents[0] == lot_name) { // This is a self parent new lot with no other parents. No need to check context.
		return std::make_pair(true, "");
	}

	std::string caller = lotman::Context::get_caller();
	bool allowed = false;
	if (!include_self) {
		for (const auto &parent : parents) {

			if (parent != lot_name) {
				Lot parent_lot(parent);
				auto rp = parent_lot.get_owners(true);
				if (!rp.second.empty()) { // There was an error
					std::string int_err = rp.second;
					std::string ext_err = "Failed to get parent owners while checking validity of context: ";
					return std::make_pair(false, ext_err + int_err);
				}
				if (std::find(parent_lot.recursive_owners.begin(), parent_lot.recursive_owners.end(), caller) !=
					parent_lot.recursive_owners.end()) { // Caller is an owner
					allowed = true;
					break;
				}
			}
		}
	} else {
		for (const auto &parent : parents) {
			Lot parent_lot(parent);
			auto rp = parent_lot.get_owners(true);
			if (!rp.second.empty()) { // There was an error
				std::string int_err = rp.second;
				std::string ext_err = "Failed to get parent owners while checking validity of context: ";
				return std::make_pair(false, ext_err + int_err);
			}
			if (std::find(parent_lot.recursive_owners.begin(), parent_lot.recursive_owners.end(), caller) !=
				parent_lot.recursive_owners.end()) { // Caller is an owner
				allowed = true;
				break;
			}
		}
	}
	if (!allowed) {
		return std::make_pair(false, "Current context prohibits action on lot: Caller does not have proper ownership.");
	}

	return std::make_pair(true, "");
}
std::pair<bool, std::string> lotman::Lot::check_context_for_parents(const std::vector<Lot> &parents, bool include_self,
																	bool new_lot) {
	if (new_lot && parents.size() == 1 &&
		parents[0].lot_name ==
			lot_name) { // This is a self parent new lot with no other parents. No need to check context.
		return std::make_pair(true, "");
	}

	std::string caller = lotman::Context::get_caller();
	bool allowed = false;
	if (!include_self) {
		if (parents.size() == 1 && parents[0].lot_name == lot_name) {
			allowed = true;
		}
		for (const auto &parent : parents) {
			if (parent.lot_name != lot_name) {
				Lot temp_lot(parent.lot_name);
				auto rp = temp_lot.get_owners(true);
				if (!rp.second.empty()) {
					return std::make_pair(false, "Failed to get parent owners while checking validity of context: " +
													 rp.second);
				}
				const auto &owners = rp.first;
				if (std::find(owners.begin(), owners.end(), caller) != owners.end()) {
					allowed = true;
					break;
				}
			}
		}
	} else {
		for (const auto &parent : parents) {
			Lot temp_lot(parent.lot_name);
			auto rp = temp_lot.get_owners(true);
			if (!rp.second.empty()) {
				return std::make_pair(false,
									  "Failed to get parent owners while checking validity of context: " + rp.second);
			}
			const auto &owners = rp.first;
			if (std::find(owners.begin(), owners.end(), caller) != owners.end()) {
				allowed = true;
				break;
			}
		}
	}
	if (!allowed) {
		return std::make_pair(false, "Current context prohibits action on lot: Caller does not have proper ownership.");
	}
	return std::make_pair(true, "");
}
std::pair<bool, std::string> lotman::Lot::check_context_for_children(const std::vector<std::string> &children,
																	 bool include_self) {
	if (children.size() == 0) { // No children means no need to check for context.
		return std::make_pair(true, "");
	}

	std::string caller = lotman::Context::get_caller();
	bool allowed = false;
	if (!include_self) {
		for (const auto &child : children) {
			if (child != lot_name) {
				Lot child_lot(child);
				auto rp = child_lot.get_owners(true);
				if (!rp.second.empty()) { // There was an error
					std::string int_err = rp.second;
					std::string ext_err = "Failed to get child owners while checking validity of context: ";
					return std::make_pair(false, ext_err + int_err);
				}
				if (std::find(child_lot.recursive_owners.begin(), child_lot.recursive_owners.end(), caller) !=
					child_lot.recursive_owners.end()) { // Caller is an owner
					allowed = true;
					break;
				}
			}
		}
	} else {
		for (const auto &child : children) {
			Lot child_lot(child);
			auto rp = child_lot.get_owners(true);
			if (!rp.second.empty()) { // There was an error
				std::string int_err = rp.second;
				std::string ext_err = "Failed to get child owners while checking validity of context: ";
				return std::make_pair(false, ext_err + int_err);
			}
			if (std::find(child_lot.recursive_owners.begin(), child_lot.recursive_owners.end(), caller) !=
				child_lot.recursive_owners.end()) { // Caller is an owner
				allowed = true;
				break;
			}
		}
	}
	if (!allowed) {
		return std::make_pair(false, "Current context prohibits action on lot: Caller does not have proper ownership.");
	}
	return std::make_pair(true, "");
}
std::pair<bool, std::string> lotman::Lot::check_context_for_children(const std::vector<Lot> &children,
																	 bool include_self) {
	if (children.size() == 0) { // No children means no need to check for context.
		return std::make_pair(true, "");
	}

	std::string caller = lotman::Context::get_caller();
	bool allowed = false;
	if (!include_self) {
		for (const auto &child : children) {
			if (child.lot_name != lot_name) {
				Lot temp_lot(child.lot_name);
				auto rp = temp_lot.get_owners(true);
				if (!rp.second.empty()) {
					return std::make_pair(false, "Failed to get child owners while checking validity of context: " +
													 rp.second);
				}
				const auto &owners = rp.first;
				if (std::find(owners.begin(), owners.end(), caller) != owners.end()) {
					allowed = true;
					break;
				}
			}
		}
	} else {
		for (const auto &child : children) {
			Lot temp_lot(child.lot_name);
			auto rp = temp_lot.get_owners(true);
			if (!rp.second.empty()) {
				return std::make_pair(false,
									  "Failed to get child owners while checking validity of context: " + rp.second);
			}
			const auto &owners = rp.first;
			if (std::find(owners.begin(), owners.end(), caller) != owners.end()) {
				allowed = true;
				break;
			}
		}
	}
	if (!allowed) {
		return std::make_pair(false, "Current context prohibits action on lot: Caller does not have proper ownership.");
	}
	return std::make_pair(true, "");
}

/**
 * Strict Hierarchy Enforcement Methods
 */

std::pair<bool, std::string> lotman::Lot::compute_and_store_attributions(const json &parent_attributions_json) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Get non-self parents
		std::vector<std::string> non_self_parents;
		for (const auto &p : parents) {
			if (p != lot_name) {
				non_self_parents.push_back(p);
			}
		}

		// If no non-self parents (root lot), nothing to attribute
		if (non_self_parents.empty()) {
			return std::make_pair(true, "");
		}

		// Validate that parent_attributions_json contains only known parent keys
		if (!parent_attributions_json.is_null() && parent_attributions_json.is_object()) {
			for (auto it = parent_attributions_json.begin(); it != parent_attributions_json.end(); ++it) {
				const auto &key = it.key();
				if (std::find(non_self_parents.begin(), non_self_parents.end(), key) == non_self_parents.end()) {
					return std::make_pair(false, "Attribution JSON contains unknown parent '" + key +
													 "' which is not a current non-self parent of lot '" + lot_name +
													 "'");
				}
			}
		}

		// Remove any existing attributions for this child
		storage.remove_all<db::ParentChildAttribution>(
			where(c(&db::ParentChildAttribution::child_lot_name) == lot_name));

		// The three MPA keys we track attributions for
		const std::vector<std::string> mpa_keys = {"dedicated_GB", "opportunistic_GB", "max_num_objects"};
		const std::map<std::string, double> child_mpas = {
			{"dedicated_GB", man_policy_attr.dedicated_GB},
			{"opportunistic_GB", man_policy_attr.opportunistic_GB},
			{"max_num_objects", static_cast<double>(man_policy_attr.max_num_objects)}};

		for (const auto &mpa_key : mpa_keys) {
			double total_child_value = child_mpas.at(mpa_key);

			// Unbounded child sentinel (-1): the unbounded designation flows
			// to each parent — every parent attribution stores fraction = 1.0
			// so that downstream consumers reconstruct "child.mpa * fraction"
			// as -1 (i.e. unbounded) for every parent. We skip explicit-sum
			// and remainder-distribution math entirely; if the caller supplied
			// an explicit value for an unbounded axis we treat that as "this
			// parent also propagates the unbounded designation". Hierarchy
			// constraints (unbounded child under a bounded parent) are caught
			// in validate_axiom1 / validate_axiom2_for_parents_of, not here.
			if (total_child_value == -1.0) {
				for (const auto &parent_name : non_self_parents) {
					db::ParentChildAttribution attr;
					attr.child_lot_name = lot_name;
					attr.parent_lot_name = parent_name;
					attr.mpa_key = mpa_key;
					attr.fraction = 1.0;
					storage.replace(attr);
				}
				continue;
			}

			double explicitly_attributed = 0.0;
			std::vector<std::string> unspecified_parents;

			// Process explicit attributions
			for (const auto &parent_name : non_self_parents) {
				if (!parent_attributions_json.is_null() && parent_attributions_json.contains(parent_name) &&
					parent_attributions_json[parent_name].contains(mpa_key)) {

					double explicit_value = parent_attributions_json[parent_name][mpa_key].get<double>();
					double fraction = (total_child_value > 0) ? explicit_value / total_child_value : 0.0;

					db::ParentChildAttribution attr;
					attr.child_lot_name = lot_name;
					attr.parent_lot_name = parent_name;
					attr.mpa_key = mpa_key;
					attr.fraction = fraction;
					storage.replace(attr);

					explicitly_attributed += explicit_value;
				} else {
					unspecified_parents.push_back(parent_name);
				}
			}

			// If all parents were explicitly attributed, verify the sum matches the total
			// (no shortfall and no overage). An overage would mean the child's MPA is being
			// double-counted across parents, which violates the attribution invariant.
			if (unspecified_parents.empty() && total_child_value > 0) {
				double shortfall = total_child_value - explicitly_attributed;
				if (shortfall > 1e-9) {
					return std::make_pair(
						false, "Explicit attributions for '" + mpa_key + "' sum to " +
								   std::to_string(explicitly_attributed) + " but child's total is " +
								   std::to_string(total_child_value) +
								   "; all parents are explicitly attributed so remainder cannot be distributed");
				}
				double overage = explicitly_attributed - total_child_value;
				if (overage > 1e-9) {
					return std::make_pair(
						false, "Explicit attributions for '" + mpa_key + "' sum to " +
								   std::to_string(explicitly_attributed) + " which exceeds child's total of " +
								   std::to_string(total_child_value) +
								   "; attributions cannot exceed the child's allocation (would double-count).");
				}
			}

			// Distribute remainder equally among unspecified parents
			if (!unspecified_parents.empty()) {
				double remainder = total_child_value - explicitly_attributed;
				if (remainder < -1e-9) {
					return std::make_pair(false, "Explicit attributions for '" + mpa_key +
													 "' exceed the child's total allocation");
				}
				if (remainder < 0)
					remainder = 0; // floating point tolerance

				double per_parent = remainder / unspecified_parents.size();
				// For max_num_objects, handle integer remainder
				int64_t int_per_parent = 0;
				int64_t int_remainder_extra = 0;
				if (mpa_key == "max_num_objects") {
					int64_t int_remainder = static_cast<int64_t>(remainder);
					int_per_parent = int_remainder / static_cast<int64_t>(unspecified_parents.size());
					int_remainder_extra = int_remainder % static_cast<int64_t>(unspecified_parents.size());
				}

				for (size_t i = 0; i < unspecified_parents.size(); ++i) {
					double value;
					if (mpa_key == "max_num_objects") {
						value = static_cast<double>(int_per_parent +
													(static_cast<int64_t>(i) < int_remainder_extra ? 1 : 0));
					} else {
						value = per_parent;
					}

					double fraction = (total_child_value > 0) ? value / total_child_value : 0.0;

					db::ParentChildAttribution attr;
					attr.child_lot_name = lot_name;
					attr.parent_lot_name = unspecified_parents[i];
					attr.mpa_key = mpa_key;
					attr.fraction = fraction;
					storage.replace(attr);
				}
			}
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to compute attributions: ") + e.what());
	}
}

std::pair<bool, std::string> lotman::Lot::update_attributions(const json &parent_attributions_json) {
	auto &storage = db::StorageManager::get_storage();
	std::string txn_error;
	bool committed =
		storage.transaction([&] { return update_attributions_in_txn(parent_attributions_json, txn_error); });
	if (!committed) {
		return std::make_pair(false, txn_error.empty() ? "Transaction failed" : txn_error);
	}
	return std::make_pair(true, "");
}

bool lotman::Lot::update_attributions_in_txn(const json &parent_attributions_json, std::string &txn_error) {
	try {
		// Load the lot's current MPAs and parents from DB
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		auto mpa_ptr = storage.get_pointer<db::ManagementPolicyAttributes>(lot_name);
		if (!mpa_ptr) {
			txn_error = "Lot '" + lot_name + "' not found in management_policy_attributes";
			return false;
		}
		man_policy_attr.dedicated_GB = mpa_ptr->dedicated_GB;
		man_policy_attr.opportunistic_GB = mpa_ptr->opportunistic_GB;
		man_policy_attr.max_num_objects = mpa_ptr->max_num_objects;

		// Load parents
		auto parent_records = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == lot_name));
		parents.clear();
		for (const auto &p : parent_records) {
			parents.push_back(p);
		}

		auto rp = compute_and_store_attributions(parent_attributions_json);
		if (!rp.first) {
			txn_error = rp.second;
			return false;
		}

		// Re-validate axioms 1 and 2 (not 3 — timestamps are unchanged)
		if (Context::get_strict_hierarchy()) {
			const std::string &name = lot_name;
			std::vector<ValidationPredicate> predicates = {[name]() { return validate_axiom1(name); },
														   [name]() { return validate_axiom2_for_parents_of(name); }};
			auto vr = apply_validation_predicates(predicates);
			if (!vr.first) {
				txn_error = vr.second;
				return false;
			}
		}

		return true;
	} catch (const std::exception &e) {
		txn_error = std::string("Failed to update attributions: ") + e.what();
		return false;
	}
}

std::pair<bool, std::string> lotman::Lot::is_lot_alive(const std::string &lot_name) {
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;
		auto mpa = storage.get_pointer<db::ManagementPolicyAttributes>(lot_name);
		if (!mpa) {
			return std::make_pair(false, "");
		}
		// A non-expiring lot (all-zero timestamp sentinel) is always alive.
		if (is_non_expiring(mpa->creation_time, mpa->expiration_time, mpa->deletion_time)) {
			return std::make_pair(true, "");
		}
		auto now =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
				.count();
		return std::make_pair(mpa->creation_time <= now && now < mpa->expiration_time, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Failed to check lot alive status: ") + e.what());
	}
}

std::pair<bool, std::string> lotman::Lot::check_contraction_for_deletion(const std::string &lot_name) {
	std::string contraction_policy = Context::get_contraction_policy();
	if (contraction_policy == "none" || Context::get_admin_override()) {
		return std::make_pair(true, "");
	}

	if (contraction_policy == "always") {
		// Expired lots are deletable by the appropriate caller without admin_override.
		// Only alive (non-expired) lots require admin_override under the "always" policy.
		auto alive_rp = is_lot_alive(lot_name);
		if (!alive_rp.second.empty()) {
			return std::make_pair(false,
								  std::string("Failed contraction policy check for deletion: ") + alive_rp.second);
		}
		if (alive_rp.first) {
			return std::make_pair(false,
								  "Contraction policy 'always' blocks deletion of lot '" + lot_name +
									  "'. Deletion is treated as contraction to zero. Set admin_override to bypass.");
		}
	}

	if (contraction_policy == "alive") {
		auto alive_rp = is_lot_alive(lot_name);
		if (!alive_rp.second.empty()) {
			return std::make_pair(false,
								  std::string("Failed contraction policy check for deletion: ") + alive_rp.second);
		}
		if (alive_rp.first) {
			return std::make_pair(false, "Contraction policy 'alive' blocks deletion of currently-alive lot '" +
											 lot_name + "'. Set admin_override to bypass.");
		}
	}

	return std::make_pair(true, "");
}

std::pair<bool, std::string>
lotman::Lot::apply_validation_predicates(const std::vector<ValidationPredicate> &predicates) {
	for (const auto &pred : predicates) {
		auto result = pred();
		if (!result.first) {
			return result;
		}
	}
	return std::make_pair(true, "");
}

std::vector<lotman::Lot::ValidationPredicate> lotman::Lot::build_axiom_predicates(const std::string &lot_name,
																				  bool check_children) {
	std::vector<ValidationPredicate> predicates;

	if (!Context::get_strict_hierarchy()) {
		return predicates;
	}

	predicates.push_back([lot_name]() { return validate_axiom1(lot_name); });
	predicates.push_back([lot_name]() { return validate_axiom2_for_parents_of(lot_name); });
	predicates.push_back([lot_name]() { return validate_axiom3(lot_name); });

	if (check_children) {
		predicates.push_back([lot_name]() -> std::pair<bool, std::string> {
			Lot lot(lot_name);
			auto children_rp = lot.get_children(false);
			if (!children_rp.second.empty()) {
				return std::make_pair(false, "Failed to get children for '" + lot_name + "': " + children_rp.second);
			}
			for (const auto &child : lot.self_children) {
				auto a1 = validate_axiom1(child.lot_name);
				if (!a1.first)
					return a1;
				auto a2 = validate_axiom2_for_parents_of(child.lot_name);
				if (!a2.first)
					return a2;
				auto a3 = validate_axiom3(child.lot_name);
				if (!a3.first)
					return a3;
			}
			return std::make_pair(true, "");
		});
	}

	return predicates;
}

std::pair<bool, std::string> lotman::Lot::validate_axiom1(const std::string &child_lot_name) {
	// Axiom 1: For each parent P of child C, the attributed amount from C to P
	// must not exceed P's own MPAs.
	//
	// Per-axis sentinel handling: each of the three sub-axes (dedicated_GB,
	// opportunistic_GB, max_num_objects) is checked independently.
	//   * If a parent is unbounded on a sub-axis (-1 sentinel), that sub-axis
	//     check is skipped entirely (no finite child value can exceed +inf).
	//   * If a child is unbounded on a sub-axis but the parent is bounded on
	//     the same sub-axis, the child cannot fit and the relationship is
	//     rejected.
	//   * If the child or parent is in the transient partial-storage state
	//     (dedicated_GB == -1 with opportunistic_GB != -1) during a multi-
	//     field update, defer; the post-loop invariant check in
	//     lotman_update_lot rejects any persisting partial state.
	//
	// Note: there is intentionally no "combined" (dedicated + opportunistic)
	// cap check. Dedicated and opportunistic are different storage pools, so
	// per-pool caps are sufficient and the combined check would conflate
	// independent sub-axes (and break in obvious ways once either pool is
	// the unbounded sentinel).
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Get child's MPAs
		auto child_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(child_lot_name);
		if (!child_mpa) {
			return std::make_pair(false, "Child lot '" + child_lot_name + "' not found");
		}

		if (is_partial_storage_sentinel(child_mpa->dedicated_GB, child_mpa->opportunistic_GB)) {
			// Defer: transient state inside an in-progress MPA update.
			return std::make_pair(true, "");
		}
		const bool child_unb_ded = is_unbounded_dedicated(child_mpa->dedicated_GB);
		const bool child_unb_opp = is_unbounded_opportunistic(child_mpa->opportunistic_GB);
		const bool child_unbounded_objects = is_unbounded_objects(child_mpa->max_num_objects);

		// Get non-self parents from the parents table (authoritative source)
		auto parent_records = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == child_lot_name));

		// Get all attributions for this child
		auto attributions = storage.get_all<db::ParentChildAttribution>(
			where(c(&db::ParentChildAttribution::child_lot_name) == child_lot_name));

		// Group by parent
		std::map<std::string, std::map<std::string, double>> parent_attributed; // parent -> {mpa_key -> fraction}
		for (const auto &attr : attributions) {
			parent_attributed[attr.parent_lot_name][attr.mpa_key] = attr.fraction;
		}

		// Iterate over actual parents from the parents table, not just attribution rows
		for (const auto &parent_name : parent_records) {
			if (parent_name == child_lot_name)
				continue; // skip self-parent

			// Ensure this parent has attribution rows
			if (parent_attributed.find(parent_name) == parent_attributed.end()) {
				return std::make_pair(false, "Hierarchy attribution error: parent lot '" + parent_name +
												 "' has no attribution rows for child lot '" + child_lot_name +
												 "'; attribution data may be missing or stale.");
			}

			const auto &fractions = parent_attributed.at(parent_name);
			auto parent_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(parent_name);
			if (!parent_mpa) {
				return std::make_pair(false, "Parent lot '" + parent_name + "' not found");
			}
			if (is_partial_storage_sentinel(parent_mpa->dedicated_GB, parent_mpa->opportunistic_GB)) {
				// Defer: transient state inside an in-progress MPA update.
				continue;
			}
			const bool parent_unb_ded = is_unbounded_dedicated(parent_mpa->dedicated_GB);
			const bool parent_unb_opp = is_unbounded_opportunistic(parent_mpa->opportunistic_GB);
			const bool parent_unbounded_objects = is_unbounded_objects(parent_mpa->max_num_objects);

			// An unbounded child cannot fit inside a bounded parent on the
			// same sub-axis: the child's effective allocation on that axis is
			// "infinite", which exceeds any finite parent cap.
			if (child_unb_ded && !parent_unb_ded) {
				return std::make_pair(false, "Hierarchy violation: child lot '" + child_lot_name +
												 "' has unbounded dedicated_GB (-1) but parent lot '" + parent_name +
												 "' has a bounded dedicated_GB. An unbounded child requires every "
												 "parent to also be unbounded on that sub-axis.");
			}
			if (child_unb_opp && !parent_unb_opp) {
				return std::make_pair(false, "Hierarchy violation: child lot '" + child_lot_name +
												 "' has unbounded opportunistic_GB (-1) but parent lot '" +
												 parent_name +
												 "' has a bounded opportunistic_GB. An unbounded child requires "
												 "every parent to also be unbounded on that sub-axis.");
			}
			if (child_unbounded_objects && !parent_unbounded_objects) {
				return std::make_pair(false, "Hierarchy violation: child lot '" + child_lot_name +
												 "' has unbounded max_num_objects (-1) but parent lot '" + parent_name +
												 "' has a bounded max_num_objects. An unbounded child requires "
												 "every parent to also be unbounded on that sub-axis.");
			}

			// Compute attributed values
			double attr_ded =
				fractions.count("dedicated_GB") ? fractions.at("dedicated_GB") * child_mpa->dedicated_GB : 0.0;
			double attr_opp = fractions.count("opportunistic_GB")
								  ? fractions.at("opportunistic_GB") * child_mpa->opportunistic_GB
								  : 0.0;
			double attr_obj = fractions.count("max_num_objects")
								  ? std::round(fractions.at("max_num_objects") * child_mpa->max_num_objects)
								  : 0.0;

			// Per-sub-axis cap checks. Each axis is skipped when the parent
			// is unbounded on it.
			if (!parent_unb_ded && attr_ded > parent_mpa->dedicated_GB + 1e-9) {
				return std::make_pair(false, "Hierarchy violation: child lot '" + child_lot_name + "' attributes " +
												 std::to_string(attr_ded) + " dedicated_GB to parent lot '" +
												 parent_name +
												 "', which exceeds the parent's dedicated_GB allocation of " +
												 std::to_string(parent_mpa->dedicated_GB) +
												 ". A child lot's allocation may not exceed any parent's.");
			}

			if (!parent_unb_opp && attr_opp > parent_mpa->opportunistic_GB + 1e-9) {
				return std::make_pair(false, "Hierarchy violation: child lot '" + child_lot_name + "' attributes " +
												 std::to_string(attr_opp) + " opportunistic_GB to parent lot '" +
												 parent_name +
												 "', which exceeds the parent's opportunistic_GB allocation of " +
												 std::to_string(parent_mpa->opportunistic_GB) +
												 ". A child lot's allocation may not exceed any parent's.");
			}

			// Object axis: skip if the parent's object axis is unbounded.
			if (!parent_unbounded_objects) {
				if (attr_obj > parent_mpa->max_num_objects) {
					return std::make_pair(false, "Hierarchy violation: child lot '" + child_lot_name + "' attributes " +
													 std::to_string(static_cast<int64_t>(attr_obj)) +
													 " max_num_objects to parent lot '" + parent_name +
													 "', which exceeds the parent's max_num_objects allocation of " +
													 std::to_string(parent_mpa->max_num_objects) +
													 ". A child lot's allocation may not exceed any parent's.");
				}
			}
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false,
							  std::string("Hierarchy validation (per-parent allocation check) failed: ") + e.what());
	}
}

std::pair<bool, std::string> lotman::Lot::validate_axiom2_for_parents_of(const std::string &child_lot_name) {
	// Axiom 2 (time-aware): For each parent P, at no point in time may the sum of
	// concurrently-active children's attributed MPAs exceed P's own MPAs.
	// Uses a sweep-line over children's [creation_time, expiration_time) intervals.
	//
	// Per-axis sentinel handling: each sub-axis (dedicated_GB, opportunistic_GB,
	// max_num_objects) is checked independently; the per-sub-axis cap check is
	// skipped when the parent is unbounded (-1) on that sub-axis. Cross-axis
	// containment (an unbounded child under a bounded parent on the same axis)
	// is rejected by axiom 1 before the sweep is reached, so children feeding
	// into the sweep are guaranteed to be bounded on each axis the parent is
	// bounded on. As in axiom 1, the per-pool checks are sufficient -- there
	// is no separate "combined" (ded+opp) cap.
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		// Get the parents of this child (non-self)
		auto parent_records = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == child_lot_name));

		for (const auto &parent_name : parent_records) {
			if (parent_name == child_lot_name)
				continue; // skip self-parent

			auto parent_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(parent_name);
			if (!parent_mpa) {
				return std::make_pair(false, "Parent lot '" + parent_name + "' not found");
			}
			if (is_partial_storage_sentinel(parent_mpa->dedicated_GB, parent_mpa->opportunistic_GB)) {
				// Defer: transient state inside an in-progress MPA update.
				continue;
			}
			const bool parent_unb_ded = is_unbounded_dedicated(parent_mpa->dedicated_GB);
			const bool parent_unb_opp = is_unbounded_opportunistic(parent_mpa->opportunistic_GB);
			const bool parent_unbounded_objects = is_unbounded_objects(parent_mpa->max_num_objects);

			// If the parent is unbounded on every sub-axis, no peak can exceed
			// it; skip the sweep entirely.
			if (parent_unb_ded && parent_unb_opp && parent_unbounded_objects)
				continue;

			// Build sweep-line events from children's attributions
			auto events = build_attribution_events(parent_name);
			auto peak = run_sweep_line(events);

			if (!parent_unb_ded && peak.peak_ded > parent_mpa->dedicated_GB + 1e-9) {
				return std::make_pair(
					false, "Hierarchy violation: peak concurrent dedicated_GB across children of parent lot '" +
							   parent_name + "' is " + std::to_string(peak.peak_ded) +
							   ", which exceeds the parent's dedicated_GB allocation of " +
							   std::to_string(parent_mpa->dedicated_GB) +
							   ". The combined allocations of children active at the same time may not exceed "
							   "the parent's capacity.");
			}

			if (!parent_unb_opp && peak.peak_opp > parent_mpa->opportunistic_GB + 1e-9) {
				return std::make_pair(
					false, "Hierarchy violation: peak concurrent opportunistic_GB across children of parent lot '" +
							   parent_name + "' is " + std::to_string(peak.peak_opp) +
							   ", which exceeds the parent's opportunistic_GB allocation of " +
							   std::to_string(parent_mpa->opportunistic_GB) +
							   ". The combined allocations of children active at the same time may not exceed "
							   "the parent's capacity.");
			}

			if (!parent_unbounded_objects) {
				// Check: peak objects ≤ parent objects
				if (std::round(peak.peak_obj) > parent_mpa->max_num_objects) {
					return std::make_pair(
						false, "Hierarchy violation: peak concurrent max_num_objects across children of parent lot '" +
								   parent_name + "' is " + std::to_string(static_cast<int64_t>(peak.peak_obj)) +
								   ", which exceeds the parent's max_num_objects allocation of " +
								   std::to_string(parent_mpa->max_num_objects) +
								   ". The combined allocations of children active at the same time may not exceed "
								   "the parent's capacity.");
				}
			}
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false, std::string("Hierarchy validation (concurrent children capacity check) failed: ") +
										 e.what());
	}
}

std::pair<bool, std::string> lotman::Lot::validate_axiom3(const std::string &child_lot_name) {
	// Axiom 3: A child lot's timestamps must fit within all parents' timestamps:
	// child.creation_time ≥ parent.creation_time
	// child.expiration_time ≤ parent.expiration_time
	// child.deletion_time ≤ parent.deletion_time
	//
	// Sentinel handling: a lot whose creation/expiration/deletion timestamps
	// are all zero is treated as "non-expiring" (effectively spans
	// [-infinity, +infinity)). Therefore:
	//   * If the parent is non-expiring, any child window fits inside it and
	//     all per-parent containment checks are skipped.
	//   * If the child is non-expiring, every parent must also be non-expiring;
	//     otherwise the child's infinite window cannot fit inside the parent's
	//     finite window.
	//   * If the child is in an intermediate "partial-zero" state (mid-update
	//     within a transaction), defer; the final post-update invariant check
	//     in lotman_update_lot will reject any persisting partial-zero state.
	try {
		auto &storage = db::StorageManager::get_storage();
		using namespace sqlite_orm;

		auto child_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(child_lot_name);
		if (!child_mpa) {
			return std::make_pair(false, "Child lot '" + child_lot_name + "' not found");
		}

		const bool child_non_expiring =
			is_non_expiring(child_mpa->creation_time, child_mpa->expiration_time, child_mpa->deletion_time);
		const bool child_partial_zero =
			!child_non_expiring &&
			((child_mpa->creation_time == 0) || (child_mpa->expiration_time == 0) || (child_mpa->deletion_time == 0));
		if (child_partial_zero) {
			// Intermediate state during a multi-step update; defer to the
			// post-loop invariant check in lotman_update_lot.
			return std::make_pair(true, "");
		}

		auto parent_records = storage.select(&db::Parent::parent, where(c(&db::Parent::lot_name) == child_lot_name));

		for (const auto &parent_name : parent_records) {
			if (parent_name == child_lot_name)
				continue; // skip self-parent

			auto parent_mpa = storage.get_pointer<db::ManagementPolicyAttributes>(parent_name);
			if (!parent_mpa) {
				return std::make_pair(false, "Parent lot '" + parent_name + "' not found");
			}

			const bool parent_non_expiring =
				is_non_expiring(parent_mpa->creation_time, parent_mpa->expiration_time, parent_mpa->deletion_time);
			const bool parent_partial_zero =
				!parent_non_expiring && ((parent_mpa->creation_time == 0) || (parent_mpa->expiration_time == 0) ||
										 (parent_mpa->deletion_time == 0));
			// Defer if the parent is in a transient partial-zero state mid-update.
			if (parent_partial_zero) {
				continue;
			}

			// Non-expiring parent absorbs any child window (including a
			// non-expiring child). Skip the per-parent containment checks.
			if (parent_non_expiring) {
				continue;
			}

			// A non-expiring child requires every parent to also be
			// non-expiring; the previous check already handled that case so
			// reaching this point with a non-expiring child means a finite
			// parent was found and the child cannot fit inside it.
			if (child_non_expiring) {
				return std::make_pair(
					false, "Hierarchy violation: child lot '" + child_lot_name +
							   "' is configured as non-expiring (all timestamps are 0) but parent lot '" + parent_name +
							   "' has a finite time window. A non-expiring child requires every parent to also be "
							   "non-expiring.");
			}

			if (child_mpa->creation_time < parent_mpa->creation_time) {
				return std::make_pair(
					false, "Hierarchy violation: child lot '" + child_lot_name + "' has a creation_time (" +
							   std::to_string(child_mpa->creation_time) + ") earlier than parent lot '" + parent_name +
							   "' creation_time (" + std::to_string(parent_mpa->creation_time) +
							   "). A child lot's time window must be contained within every parent's time window.");
			}

			if (child_mpa->expiration_time > parent_mpa->expiration_time) {
				return std::make_pair(
					false, "Hierarchy violation: child lot '" + child_lot_name + "' has an expiration_time (" +
							   std::to_string(child_mpa->expiration_time) + ") later than parent lot '" + parent_name +
							   "' expiration_time (" + std::to_string(parent_mpa->expiration_time) +
							   "). A child lot's time window must be contained within every parent's time window.");
			}

			if (child_mpa->deletion_time > parent_mpa->deletion_time) {
				return std::make_pair(
					false, "Hierarchy violation: child lot '" + child_lot_name + "' has a deletion_time (" +
							   std::to_string(child_mpa->deletion_time) + ") later than parent lot '" + parent_name +
							   "' deletion_time (" + std::to_string(parent_mpa->deletion_time) +
							   "). A child lot's time window must be contained within every parent's time window.");
			}
		}

		return std::make_pair(true, "");
	} catch (const std::exception &e) {
		return std::make_pair(false,
							  std::string("Hierarchy validation (time window containment check) failed: ") + e.what());
	}
}

/**
 * Functions specific to Checks class
 */

bool lotman::Checks::cycle_check(
	const std::string &start_node, const std::vector<std::string> &start_parents,
	const std::vector<std::string> &start_children) { // Returns true if invalid cycle is detected, else returns false
	// Basic DFS algorithm to check for cycle creation when adding a lot that has both parents and children.

	// Algorithm initialization
	std::vector<std::string> dfs_nodes_to_visit;
	dfs_nodes_to_visit.insert(dfs_nodes_to_visit.end(), start_parents.begin(), start_parents.end());
	for (const auto &children_iter : start_children) {
		auto check_iter = std::find(dfs_nodes_to_visit.begin(), dfs_nodes_to_visit.end(), children_iter);
		if (check_iter != dfs_nodes_to_visit.end()) {
			// Run, Luke! It's a trap! Erm... a cycle!
			return true;
		}
	}

	// Iterate
	while (dfs_nodes_to_visit.size() > 0) { // When dfs_nodes_to_visit has size 0, we're done checking
		lotman::Lot lot(dfs_nodes_to_visit[0]);
		auto rp = lot.get_parents();

		// TODO: expose errors
		if (!rp.second.empty()) { // There was an error
			std::string int_err = rp.second;
			std::string ext_err = "Failure on call to get_parents(): ";
			return false;
		}

		// convert to vec of strings instead of vec of lots
		std::vector<std::string> dfs_node_parents;
		for (const auto &parent : rp.first) {
			dfs_node_parents.push_back(parent.lot_name);
		}

		dfs_nodes_to_visit.insert(dfs_nodes_to_visit.end(), dfs_node_parents.begin(), dfs_node_parents.end());

		for (const auto &children_iter : start_children) { // For each specified child of start node ...
			auto check_iter = std::find(dfs_nodes_to_visit.begin(), dfs_nodes_to_visit.end(),
										children_iter); // ... check if that child is going to be visited ...
			if (check_iter != dfs_nodes_to_visit.end()) {
				// ... If it is going to be visited, cycle found!
				return true;
			}
		}
		dfs_nodes_to_visit.erase(
			dfs_nodes_to_visit.begin()); // Nothing consequential from this node, remove it from the nodes to visit
	}

	return false;
}

bool lotman::Checks::insertion_check(
	const std::string &LTBA, const std::string &parent,
	const std::string &child) { // Checks whether LTBA is being inserted between a parent and child.

	// TODO: expose errors
	lotman::Lot lot(child);
	auto rp = lot.get_parents();
	if (!rp.second.empty()) { // There was an error
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to get_parents(): ";
		return false;
	}

	std::vector<std::string> parents_vec;
	for (const auto &parent_lot : rp.first) {
		parents_vec.push_back(parent_lot.lot_name);
	}

	auto parent_iter = std::find(parents_vec.begin(), parents_vec.end(),
								 parent); // Check if the specified parent is listed as a parent to the child
	if (!(parent_iter == parents_vec.end())) {
		// Child has "parent" as a parent
		return true;
	}
	return false;
}

bool lotman::Checks::will_be_orphaned(const std::string &LTBR, const std::string &child) {

	// TODO: expose errors
	lotman::Lot lot(child);
	auto rp = lot.get_parents();
	if (!rp.second.empty()) { // There was an error
		std::string int_err = rp.second;
		std::string ext_err = "Failure on call to get_parents(): ";
		return false;
	}

	std::vector<std::string> parents_vec;
	for (const auto &parent : rp.first) {
		parents_vec.push_back(parent.lot_name);
	}

	if (parents_vec.size() == 1 && parents_vec[0] == LTBR) {
		return true;
	}
	return false;
}

void lotman::Context::set_caller(const std::string caller) {
	m_caller = std::make_shared<std::string>(caller);
}

void lotman::Context::set_strict_hierarchy(bool enabled) {
	m_strict_hierarchy = enabled;
}

std::pair<bool, std::string> lotman::Context::set_contraction_policy(const std::string &policy) {
	if (policy != "none" && policy != "alive" && policy != "always") {
		return std::make_pair(false, "Contraction policy must be one of: 'none', 'alive', 'always'. Got: " + policy);
	}
	m_contraction_policy = policy;
	return std::make_pair(true, "");
}

void lotman::Context::set_admin_override(bool enabled) {
	m_admin_override = enabled;
}

std::pair<bool, std::string> lotman::Context::set_lot_home(const std::string dir_path) {
	// If setting to "", then we should treat as though it is unsetting the
	// config
	if (dir_path.length() == 0) { // User is configuring to empty string
		m_home = std::make_shared<std::string>(dir_path);
		return std::make_pair(true, "");
	}

	std::vector<std::string> path_components = path_split(dir_path); // cleans any extraneous /'s
	std::string cleaned_dir_path;
	for (const auto &component : path_components) { // add the / back to the path components
		cleaned_dir_path += "/" + component;
	}

	// Check that the cache_home exists, and if not try to create it
	auto rp = mkdir_and_parents_if_needed(cleaned_dir_path); // Structured bindings not introduced until cpp 17
	if (!rp.first) {										 //
		std::string err_prefix{"An issue was encountered with the provided cache home path: "};
		return std::make_pair(false, err_prefix + rp.second);
	}

	// Now it exists and we can write to it, set the value and let
	// scitokens_cache handle the rest
	m_home = std::make_shared<std::string>(cleaned_dir_path);

	// Reset the ORM storage manager so it re-initializes with the new path
	lotman::db::StorageManager::reset();

	return std::make_pair(true, "");
}

std::pair<bool, std::string> lotman::Context::mkdir_and_parents_if_needed(const std::string dir_path) {
	// SciTokens-cpp already makes assumptions about using Linux file paths,
	// so making that assumption here as well.

	// Using these perms because that's what the actual cache file uses in
	// scitokens_cache
	mode_t mode = 0700; // Maybe these permissions should be configurable?

	int result;
	std::string currentLevel;
	std::vector<std::string> path_components = path_split(dir_path);
	for (const auto &component : path_components) {
		currentLevel += "/" + component;
		result = mkdir(currentLevel.c_str(), mode);
		if ((result < 0) && errno != EEXIST) {
			std::string err_prefix{"There was an error while creating/checking "
								   "the directory: mkdir error: "};
			return std::make_pair(false, err_prefix + strerror(errno));
		}
	}

	return std::make_pair(true, "");
}

std::vector<std::string> lotman::Context::path_split(std::string path) {
	std::vector<std::string> path_components;
	std::stringstream ss(path);
	std::string component;

	while (std::getline(ss, component, '/')) {
		if (!component.empty()) {
			path_components.push_back(component);
		}
	}

	if (path_components[0] == "") {
		path_components.erase(path_components.begin());
	}
	return path_components;
}
