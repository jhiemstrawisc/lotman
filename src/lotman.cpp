#include "lotman.h"

#include "lotman_db.h"
#include "lotman_internal.h"
#include "lotman_version.h"
#include "schemas.h"

#include <chrono>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>
#include <string.h>

/*
Initialize some context globals
*/

// caller
std::shared_ptr<std::string> lotman::Context::m_caller = std::make_shared<std::string>("");

// Lot home
std::shared_ptr<std::string> lotman::Context::m_home = std::make_shared<std::string>("");

// Strict hierarchy enforcement (off by default for backward compatibility)
bool lotman::Context::m_strict_hierarchy = false;

// Contraction policy ("none" by default for backward compatibility)
std::string lotman::Context::m_contraction_policy = "none";

// Admin override (off by default)
bool lotman::Context::m_admin_override = false;

std::shared_ptr<int> lotman_db_timeout = std::make_shared<int>(5000); // in ms

using json = nlohmann::json;

const char *lotman_version() {
	std::string major = std::to_string(Lotman_VERSION_MAJOR);
	std::string minor = std::to_string(Lotman_VERSION_MINOR);
	std::string patch = std::to_string(Lotman_VERSION_PATCH);
	static std::string version = "v" + major + "." + minor + "." + patch;

	return version.c_str();
}

int lotman_add_lot(const char *lotman_JSON_str, char **err_msg) {
	try {
		json lot_JSON_obj = json::parse(lotman_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::new_lot_schema);
		validator.validate(lot_JSON_obj);

		// Data checks
		auto rp = lotman::Lot::lot_exists("default");
		if (!rp.first && lot_JSON_obj["lot_name"] != "default") {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("The default lot named \"default\" must be created first.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
			}
			return -1;
		}

		// Make sure lot doesn't already exist
		rp = lotman::Lot::lot_exists(lot_JSON_obj["lot_name"]);
		if (rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot already exists
					*err_msg = strdup("The lot already exists and cannot be recreated. Maybe you meant to modify it?");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot;
		{
			auto init_rp = lot.init_full(lot_JSON_obj);
			if (!init_rp.first) {
				if (err_msg) {
					*err_msg = strdup(init_rp.second.c_str());
				}
				return -1;
			}
		}

		// Extract parent_attributions if provided
		json parent_attributions;
		if (lot_JSON_obj.contains("parent_attributions") && !lot_JSON_obj["parent_attributions"].is_null()) {
			parent_attributions = lot_JSON_obj["parent_attributions"];
		}

		// Check for context and make sure caller is allowed to add the lot as specified
		rp = lot.check_context_for_parents(lot.parents, false, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		rp = lot.check_context_for_children(lot.children);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for children: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		rp = lot.store_lot(parent_attributions);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failed to store lot: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_remove_lot(const char *lot_name, const bool assign_LTBR_parent_as_parent_to_orphans,
					  const bool assign_LTBR_parent_as_parent_to_non_orphans, const bool assign_policy_to_children,
					  const bool override_policy, char **err_msg) {
	try {
		auto rp = lotman::Lot::lot_exists(lot_name);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so it doesn't have to be removed.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot(lot_name);

		// To destroy a lot, caller must own the lot (not just the contents of the lot)
		// This implies caller owns a parent of the the lot.
		lot.get_parents(true, true); // Lots that are self owners own the lot as well as its content
		rp = lot.check_context_for_parents(lot.recursive_parents, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		// Use this block when you've created reassignment policies in the database
		// if (override_policy) { // Don't bother to load the assigned policy, just initialize
		//     rp = lot.init_reassignment_policy(assign_LTBR_parent_as_parent_to_orphans,
		//     assign_LTBR_parent_as_parent_to_non_orphans, assign_policy_to_children); if (!rp.first) {
		//         if (err_msg) {
		//             std::string int_err = rp.second;
		//             std::string ext_err = "Function call to init_reassignment_policy failed: ";
		//             *err_msg = strdup((ext_err + int_err).c_str());
		//         }
		//     }
		// }
		// else {
		//     rp = lot.load_reassignment_policy();
		//     if (!rp.first) {
		//         if (rp.second.empty()) { // function worked, but lot has no stored reassignment policy
		//             rp = lot.init_reassignment_policy(assign_LTBR_parent_as_parent_to_orphans,
		//             assign_LTBR_parent_as_parent_to_non_orphans, assign_policy_to_children); if (!rp.first) {
		//                 if (err_msg) {
		//                     std::string int_err = rp.second;
		//                     std::string ext_err = "Function call to init_reassignment_policy failed: ";
		//                     *err_msg = strdup((ext_err + int_err).c_str());
		//                 }
		//             }
		//         }
		//         else { // function did not work
		//             if (err_msg) {
		//                 std::string int_err = rp.second;
		//                 std::string ext_err = "Failed to load reassignment policy: ";
		//                 *err_msg = strdup((ext_err + int_err).c_str());
		//             }
		//             return -1;
		//         }
		//     }
		// }

		rp = lot.init_reassignment_policy(assign_LTBR_parent_as_parent_to_orphans,
										  assign_LTBR_parent_as_parent_to_non_orphans, assign_policy_to_children);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Function call to init_reassignment_policy failed: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		rp = lot.destroy_lot();
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failed to remove lot from database: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_remove_lots_recursive(const char *lot_name, char **err_msg) {
	try {
		auto rp = lotman::Lot::lot_exists(lot_name);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so it doesn't have to be removed.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot(lot_name);

		// To destroy a lot, caller must own the lot (not just the contents of the lot)
		// This implies caller owns a parent of the the lot.
		lot.get_parents(true, true); // Lots that are self owners own the lot as well as its content
		rp = lot.check_context_for_parents(lot.recursive_parents, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		rp = lot.destroy_lot_recursive();
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failed to remove lot from database: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_update_lot(const char *lotman_JSON_str, char **err_msg) {
	try {
		json update_JSON_obj = json::parse(lotman_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::lot_update_schema);
		validator.validate(update_JSON_obj);

		// Check that lot exists
		auto rp = lotman::Lot::lot_exists(update_JSON_obj["lot_name"]);
		if (!rp.first) {
			if (err_msg) {
				if (!rp.second.empty()) { // There was an error
					std::string int_err = rp.second;
					std::string ext_err = "Failure on call to lot_exists: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				} else { // Lot does not exist
					std::string err = "Lot does not exist";
					*err_msg = strdup(err.c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(update_JSON_obj["lot_name"].get<std::string>());

		// Check for context
		lot.get_parents(true, true);
		rp = lot.check_context_for_parents(lot.recursive_parents, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		// Start checking which keys to operate on. All write operations are run
		// inside a single outer storage.transaction() so the entire update is
		// atomic: if any sub-step fails, every preceding sub-step is rolled back.
		auto &storage = lotman::db::StorageManager::get_storage();
		std::string txn_error;
		bool committed = storage.transaction([&]() -> bool {
			if (update_JSON_obj.contains("owner")) {
				if (!lot.update_owner_in_txn(update_JSON_obj["owner"].get<std::string>(), txn_error)) {
					txn_error = "Failed on call to lot.update_owner: " + txn_error;
					return false;
				}
			}

			if (update_JSON_obj.contains("parents")) {
				if (!lot.update_parents_in_txn(update_JSON_obj["parents"], txn_error)) {
					txn_error = "Failed on call to lot.update_parents: " + txn_error;
					return false;
				}
			}

			if (update_JSON_obj.contains("paths")) {
				if (!lot.update_paths_in_txn(update_JSON_obj["paths"], txn_error)) {
					txn_error = "Failed on call to lot.update_paths: " + txn_error;
					return false;
				}
			}

			if (update_JSON_obj.contains("management_policy_attrs")) {
				for (const auto &update_attr : update_JSON_obj["management_policy_attrs"].items()) {
					if (!lot.update_man_policy_attrs_in_txn(update_attr.key(), update_attr.value(), txn_error)) {
						txn_error = "Failed on call to lot.update_man_policy_attrs: " + txn_error;
						return false;
					}
				}

				// After all per-field MPA updates have been applied, re-load
				// the lot's final timestamp triple and enforce the sentinel
				// invariant. Per-field updates intentionally tolerate
				// transient partial-zero state inside the transaction so a
				// caller can flip a lot to/from non-expiring atomically; this
				// final pass rejects any update whose end state leaves the
				// timestamps in an inconsistent state. We also re-run the
				// hierarchy time-containment axiom (axiom 3) here because
				// per-field axiom checks defer for partial-zero intermediate
				// states.
				auto &storage_check = lotman::db::StorageManager::get_storage();
				auto mpa_final = storage_check.get_pointer<lotman::db::ManagementPolicyAttributes>(
					update_JSON_obj["lot_name"].get<std::string>());
				if (!mpa_final) {
					txn_error = "Lot '" + update_JSON_obj["lot_name"].get<std::string>() +
								"' not found in management_policy_attributes after MPA update";
					return false;
				}
				auto inv_rp = lotman::validate_time_invariants(mpa_final->creation_time, mpa_final->expiration_time,
															   mpa_final->deletion_time);
				if (!inv_rp.first) {
					txn_error = "Update on lot '" + update_JSON_obj["lot_name"].get<std::string>() +
								"' rejected: " + inv_rp.second;
					return false;
				}
				if (lotman::Context::get_strict_hierarchy()) {
					auto a3 = lotman::Lot::validate_axiom3(update_JSON_obj["lot_name"].get<std::string>());
					if (!a3.first) {
						txn_error = a3.second;
						return false;
					}
				}
			}

			if (update_JSON_obj.contains("parent_attributions")) {
				if (!lot.update_attributions_in_txn(update_JSON_obj["parent_attributions"], txn_error)) {
					txn_error = "Failed on call to lot.update_attributions: " + txn_error;
					return false;
				}
			}

			return true;
		});

		if (!committed) {
			if (err_msg) {
				*err_msg = strdup(txn_error.empty() ? "Update transaction failed" : txn_error.c_str());
			}
			return -1;
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_rm_parents_from_lot(const char *lotman_JSON_str, char **err_msg) {
	try {
		json subtraction_JSON_obj = json::parse(lotman_JSON_str);
		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::lot_rm_parents_schema);
		validator.validate(subtraction_JSON_obj);

		// Check that lot exists
		auto rp = lotman::Lot::lot_exists(subtraction_JSON_obj["lot_name"]);
		if (!rp.first) {
			if (err_msg) {
				if (!rp.second.empty()) { // There was an error
					std::string int_err = rp.second;
					std::string ext_err = "Failure on call to lot_exists: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				} else { // Lot does not exist
					std::string err = "Lot does not exist";
					*err_msg = strdup(err.c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(subtraction_JSON_obj["lot_name"].get<std::string>());

		// Check for context
		lot.get_parents(true, true);
		rp = lot.check_context_for_parents(lot.recursive_parents, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		rp = lot.remove_parents(subtraction_JSON_obj["parents"]);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failed on call to lot.remove_parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_rm_paths_from_lots(const char *lotman_JSON_str, char **err_msg) {
	try {
		json subtraction_JSON_obj = json::parse(lotman_JSON_str);
		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::lot_rm_paths_schema);
		validator.validate(subtraction_JSON_obj);

		// Phase 1 (read-only): For each path, find matching lots and check context.
		// Collect (lot_name, path) pairs to remove.
		struct PathRemoval {
			std::string lot_name;
			std::string path;
		};
		std::vector<PathRemoval> removals;

		for (const auto &path : subtraction_JSON_obj["paths"]) {
			std::string path_str{path.get<std::string>()};

			// Check if this path actually exists in the database (no temporal filtering —
			// path removal should work regardless of lot's time range)
			std::string normalized_path = path_str;
			if (normalized_path.empty() || normalized_path.back() != '/') {
				normalized_path += '/';
			}
			std::string query = "SELECT lot_name FROM paths WHERE path = ?1;";
			std::map<std::string, std::vector<int>> str_map{{normalized_path, {1}}};
			std::map<int64_t, std::vector<int>> int_map;
			auto rp_str_str = lotman::db::SQL_get_matches(query, str_map, int_map);
			if (!rp_str_str.second.empty()) { // There was an error
				if (err_msg) {
					std::string int_err = rp_str_str.second;
					std::string ext_err = "Failed to get lot name: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}

			if (rp_str_str.first.empty()) { // No lot found for this exact path
				// There's nothing to remove, so jump to next path
				continue;
			}

			// The same path can appear in multiple lots (with non-overlapping time
			// ranges), so iterate over every matching lot_name.
			for (const auto &matching_lot_name : rp_str_str.first) {
				lotman::Lot lot(matching_lot_name);

				// Check for context
				lot.get_parents(true, true);
				auto rp = lot.check_context_for_parents(lot.recursive_parents, true);
				if (!rp.first) {
					if (err_msg) {
						std::string int_err = rp.second;
						std::string ext_err = "Error while checking context for parents: ";
						*err_msg = strdup((ext_err + int_err).c_str());
					}
					return -1;
				}

				removals.push_back({matching_lot_name, path_str});
			}
		}

		// Phase 2 (write): Execute all removals in a single atomic transaction.
		if (!removals.empty()) {
			auto &storage = lotman::db::StorageManager::get_storage();
			using namespace sqlite_orm;
			std::string txn_error;
			bool committed = storage.transaction([&] {
				try {
					for (const auto &rm : removals) {
						std::string normalized = lotman::ensure_trailing_slash(rm.path);
						storage.remove_all<lotman::db::Path>(where(c(&lotman::db::Path::lot_name) == rm.lot_name and
																   c(&lotman::db::Path::path) == normalized));
					}
				} catch (const std::exception &e) {
					txn_error = e.what();
					return false; // rollback
				}
				return true;
			});

			if (!committed) {
				if (err_msg) {
					*err_msg = strdup(txn_error.empty() ? "Transaction failed" : txn_error.c_str());
				}
				return -1;
			}
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_add_to_lot(const char *lotman_JSON_str, char **err_msg) {
	try {
		json addition_obj = json::parse(lotman_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::lot_additions_schema);
		validator.validate(addition_obj);

		// Assert lot exists
		auto rp = lotman::Lot::lot_exists(addition_obj["lot_name"]);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so nothing can be added to it.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot(addition_obj["lot_name"].get<std::string>());

		// Check for context
		lot.get_parents(true, true);
		rp = lot.check_context_for_parents(lot.recursive_parents, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		// Run all additions inside a single outer storage.transaction() so the
		// envelope is atomic: a late failure (e.g. attribution validation)
		// rolls back any preceding parent/path additions.
		auto &storage = lotman::db::StorageManager::get_storage();
		std::string txn_error;
		bool committed = storage.transaction([&]() -> bool {
			if (addition_obj.contains("parents")) {
				std::vector<lotman::Lot> parent_lots;
				for (const auto &parent_name : addition_obj["parents"]) {
					lotman::Lot parent_lot(parent_name.get<std::string>());
					parent_lots.push_back(parent_lot);
				}

				if (!lot.add_parents_in_txn(parent_lots, txn_error)) {
					txn_error = "Failure to add parents: " + txn_error;
					return false;
				}
			}

			if (addition_obj.contains("paths")) {
				if (!lot.add_paths_in_txn(addition_obj["paths"], txn_error)) {
					txn_error = "Failure to add paths: " + txn_error;
					return false;
				}
			}

			// `parent_attributions` runs last so any parents added above are visible
			// when attributions are recomputed.
			if (addition_obj.contains("parent_attributions")) {
				if (!lot.update_attributions_in_txn(addition_obj["parent_attributions"], txn_error)) {
					txn_error = "Failure to update parent attributions: " + txn_error;
					return false;
				}
			}

			return true;
		});

		if (!committed) {
			if (err_msg) {
				*err_msg = strdup(txn_error.empty() ? "Add-to-lot transaction failed" : txn_error.c_str());
			}
			return -1;
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_is_root(const char *lot_name, char **err_msg) {
	if (!lot_name) {
		if (err_msg) {
			*err_msg = strdup("Name for the lot whose rootness is to be determined must not be nullpointer.");
		}
		return -1;
	}

	try {
		auto rp = lotman::Lot::lot_exists(lot_name);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("The lot does not exist");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(lot_name);
		rp = lot.check_if_root();
		if (rp.second.empty()) {
			return rp.first;
		} else { // There was an error
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Function call to lotman::Lot::check_if_root failed: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_lot_exists(const char *lot_name, char **err_msg) {
	if (!lot_name) {
		if (err_msg) {
			*err_msg = strdup("Name for the lot whose existence is to be determined must not be nullpointer.");
		}
		return -1;
	}

	try {
		auto rp = lotman::Lot::lot_exists(lot_name);
		if (rp.second.empty()) { // no error message indicates success --> will change when inner function can handle
								 // error propagation
			return rp.first;
		} else {
			std::string int_err = rp.second;
			std::string ext_err = "Call to lotman::Lot::lot_exists failed: ";
			if (err_msg) {
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_owners(const char *lot_name, const bool recursive, char ***output, char **err_msg) {
	if (!lot_name) {
		if (err_msg) {
			*err_msg = strdup("Name for the lot whose owners are to be obtained must not be nullpointer.");
		}
		return -1;
	}

	try {
		auto rp_bool_str = lotman::Lot::lot_exists(lot_name);
		if (!rp_bool_str.first) {
			if (err_msg) {
				if (rp_bool_str.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("The default lot named \"default\" must be created first.");
				} else {
					std::string int_err = rp_bool_str.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(lot_name);
		auto rp_vec_str = lot.get_owners(recursive);
		if (!rp_vec_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_vec_str.second;
				std::string ext_err = "Function call to lotman::Lot::get_owners failed: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> owners_list = rp_vec_str.first;
		auto owners_list_c = static_cast<char **>(malloc(sizeof(char *) * (owners_list.size() + 1)));
		owners_list_c[owners_list.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : owners_list) {
			owners_list_c[idx] = strdup(iter.c_str());
			if (!owners_list_c[idx]) {
				lotman_free_string_list(owners_list_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = owners_list_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

void lotman_free_string_list(char **str_list) {
	int idx = 0;
	do {
		free(str_list[idx++]);
	} while (str_list[idx]);
	free(str_list);
}

int lotman_get_parent_names(const char *lot_name, const bool recursive, const bool get_self, char ***output,
							char **err_msg) {
	if (!lot_name) {
		if (err_msg) {
			*err_msg = strdup("Name for the lot whose parents are to be obtained must not be nullpointer.");
		}
		return -1;
	}

	try {
		auto rp_bool_str = lotman::Lot::lot_exists(lot_name);
		if (!rp_bool_str.first) {
			if (err_msg) {
				if (rp_bool_str.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("The default lot named \"default\" must be created first.");
				} else {
					std::string int_err = rp_bool_str.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(lot_name);

		auto rp_vec_str = lot.get_parents(recursive, get_self);
		if (!rp_vec_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_vec_str.second;
				std::string ext_err = "Function call to lotman::Lot::get_parents failed: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<lotman::Lot> parents = rp_vec_str.first;
		std::vector<std::string> parents_list;
		for (const auto &parent : parents) {
			parents_list.push_back(parent.lot_name);
		}
		auto parents_list_c = static_cast<char **>(malloc(sizeof(char *) * (parents_list.size() + 1)));
		parents_list_c[parents_list.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : parents_list) {
			parents_list_c[idx] = strdup(iter.c_str());
			if (!parents_list_c[idx]) {
				lotman_free_string_list(parents_list_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = parents_list_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_children_names(const char *lot_name, const bool recursive, const bool get_self, char ***output,
							  char **err_msg) {
	if (!lot_name) {
		if (err_msg) {
			*err_msg = strdup("Name for the lot whose children are to be obtained must not be nullpointer.");
		}
		return -1;
	}

	try {
		auto rp_bool_str = lotman::Lot::lot_exists(lot_name);
		if (!rp_bool_str.first) {
			if (err_msg) {
				if (rp_bool_str.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("The default lot named \"default\" must be created first.");
				} else {
					std::string int_err = rp_bool_str.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(lot_name);

		auto rp_vec_str = lot.get_children(recursive, get_self);
		if (!rp_vec_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_vec_str.second;
				std::string ext_err = "Function call to lotman::Lot::get_children failed: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<lotman::Lot> children = rp_vec_str.first;
		std::vector<std::string> children_list;
		for (const auto &child : children) {
			children_list.push_back(child.lot_name);
		}
		auto children_list_c = static_cast<char **>(malloc(sizeof(char *) * (children_list.size() + 1)));
		children_list_c[children_list.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : children_list) {
			children_list_c[idx] = strdup(iter.c_str());
			if (!children_list_c[idx]) {
				lotman_free_string_list(children_list_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = children_list_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_policy_attributes(const char *policy_attributes_JSON_str, char **output, char **err_msg) {
	try {
		json get_attrs_obj = json::parse(policy_attributes_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::get_policy_attrs_schema);
		validator.validate(get_attrs_obj);

		// Assert lot exists
		auto rp = lotman::Lot::lot_exists(get_attrs_obj["lot_name"]);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so it has no policy attributes.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot(get_attrs_obj["lot_name"].get<std::string>());

		json output_obj;
		for (const auto &pair : get_attrs_obj.items()) {
			if (pair.key() != "lot_name") {
				auto rp_json_bool = lot.get_restricting_attribute(pair.key(), pair.value());
				if (!rp_json_bool.second.empty()) { // There was an error
					if (err_msg) {
						std::string int_err = rp.second;
						std::string ext_err = "Failed to initialize lot name: ";
						*err_msg = strdup((ext_err + int_err).c_str());
					}
					return -1;
				}
				if (pair.value() == "true") {
					// In this case, we unpack the returned object
					output_obj[pair.key()] = rp_json_bool.first["value"];
				}
				output_obj[pair.key()] = rp_json_bool.first;
			}
		}

		std::string output_str = output_obj.dump();
		auto output_str_c = static_cast<char *>(malloc(sizeof(char) * (output_str.length() + 1)));
		output_str_c = strdup(output_str.c_str());
		*output = output_str_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lot_dirs(const char *lot_name, const bool recursive, char **output, char **err_msg) {
	if (!lot_name) {
		if (err_msg) {
			*err_msg = strdup("Name for the lot whose directories are to be obtained must not be nullpointer.");
		}
		return -1;
	}

	try {
		auto rp = lotman::Lot::lot_exists(lot_name);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so nothing can be added to it.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot(lot_name);

		json output_obj;
		auto rp_json_str = lot.get_lot_dirs(recursive);
		if (!rp_json_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_json_str.second;
				std::string ext_err = "Failure on call to get_lot_dirs: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		output_obj = rp_json_str.first;
		std::string output_obj_str = output_obj.dump();
		auto output_obj_str_c = static_cast<char *>(malloc(sizeof(char) * (output_obj_str.length() + 1)));
		output_obj_str_c = strdup(output_obj_str.c_str());
		*output = output_obj_str_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_update_lot_usage(const char *update_JSON_str, bool deltaMode, char **err_msg) {
	try {
		json update_usage_JSON = json::parse(update_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(deltaMode ? lotman_schemas::update_usage_delta_schema
											: lotman_schemas::update_usage_schema);
		validator.validate(update_usage_JSON);

		// Assert lot exists
		auto rp = lotman::Lot::lot_exists(update_usage_JSON["lot_name"]);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so nothing can be added to it.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		lotman::Lot lot(update_usage_JSON["lot_name"].get<std::string>());

		// Check for context
		lot.get_parents(true, true);
		rp = lot.check_context_for_parents(lot.recursive_parents, true);
		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Error while checking context for parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		for (const auto &pair : update_usage_JSON.items()) {
			if (pair.key() != "lot_name") {
				rp = lot.update_self_usage(pair.key(), pair.value(), deltaMode);
				if (!rp.first) { // There was an error
					if (err_msg) {
						std::string int_err = rp.second;
						std::string ext_err = "Failure on call to update_self_usage: ";
						*err_msg = strdup((ext_err + int_err).c_str());
					}
					return -1;
				}
			}
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_update_lot_usage_by_dir(const char *update_JSON_str, bool deltaMode, int64_t query_time, char **err_msg) {
	try {
		json update_JSON = json::parse(update_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(deltaMode ? lotman_schemas::update_usage_by_dir_delta_schema
											: lotman_schemas::update_usage_by_dir_schema);

		// Current schema only works to validate each update obj.
		// Eventually, the schema should be updated to correctly work on the whole array
		for (const auto &update : update_JSON) {
			validator.validate(update);
		}

		auto rp = lotman::Lot::update_usage_by_dirs(update_JSON, deltaMode, query_time);

		if (!rp.first) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to update_usage_by_dirs: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lot_usage(const char *usage_attributes_JSON_str, char **output, char **err_msg) {
	try {
		json get_usage_obj = json::parse(usage_attributes_JSON_str);

		// Validate the incoming JSON
		json_validator validator;
		validator.set_root_schema(lotman_schemas::get_usage_schema);
		validator.validate(get_usage_obj);

		// Assert lot exists
		auto rp = lotman::Lot::lot_exists(get_usage_obj["lot_name"]);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup(
						"That was easy! The lot does not exist, so nothing can be added to it. My work is done here.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		lotman::Lot lot(get_usage_obj["lot_name"].get<std::string>());

		json output_obj;
		for (const auto &pair : get_usage_obj.items()) {
			if (pair.key() != "lot_name") {
				auto rp_json_str = lot.get_lot_usage(pair.key(), pair.value());
				if (!rp_json_str.second.empty()) { // There was an error
					if (err_msg) {
						std::string int_err = rp_json_str.second;
						std::string ext_err = "Failure on call to get_lot_usage: ";
						*err_msg = strdup((ext_err + int_err).c_str());
					}
					return -1;
				}

				output_obj[pair.key()] = rp_json_str.first;
			}
		}

		std::string output_str = output_obj.dump();
		auto output_str_c = static_cast<char *>(malloc(sizeof(char) * (output_str.length() + 1)));
		output_str_c = strdup(output_str.c_str());
		*output = output_str_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_check_db_health(char **err_msg) {
	if (err_msg) {
		*err_msg = strdup("This function is not yet implemented...");
	}
	return -1;
}

int lotman_get_lots_past_exp(const bool recursive, char ***output, char **err_msg) {
	try {
		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		auto rp = lotman::Lot::get_lots_past_exp(recursive);
		if (!rp.second.empty()) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to get_lots_past_exp: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		std::vector<std::string> expired_lots = rp.first;

		auto expired_lots_c = static_cast<char **>(malloc(sizeof(char *) * (expired_lots.size() + 1)));
		expired_lots_c[expired_lots.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : expired_lots) {
			expired_lots_c[idx] = strdup(iter.c_str());
			if (!expired_lots_c[idx]) {
				lotman_free_string_list(expired_lots_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = expired_lots_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lots_past_del(const bool recursive, char ***output, char **err_msg) {
	try {
		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		auto rp = lotman::Lot::get_lots_past_del(recursive);
		if (!rp.second.empty()) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to get_lots_past_del: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> deletion_lots = rp.first;

		auto deletion_lots_c = static_cast<char **>(malloc(sizeof(char *) * (deletion_lots.size() + 1)));
		deletion_lots_c[deletion_lots.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : deletion_lots) {
			deletion_lots_c[idx] = strdup(iter.c_str());
			if (!deletion_lots_c[idx]) {
				lotman_free_string_list(deletion_lots_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = deletion_lots_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lots_past_opp(const bool recursive_quota, const bool recursive_children, char ***output,
							 const bool hierarchical, char **err_msg) {
	try {
		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		auto rp = lotman::Lot::get_lots_past_opp(recursive_quota, recursive_children, hierarchical);
		if (!rp.second.empty()) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to get_lots_past_del: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> past_opp_lots = rp.first;

		auto past_opp_lots_c = static_cast<char **>(malloc(sizeof(char *) * (past_opp_lots.size() + 1)));
		past_opp_lots_c[past_opp_lots.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : past_opp_lots) {
			past_opp_lots_c[idx] = strdup(iter.c_str());
			if (!past_opp_lots_c[idx]) {
				lotman_free_string_list(past_opp_lots_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = past_opp_lots_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lots_past_ded(const bool recursive_quota, const bool recursive_children, char ***output,
							 const bool hierarchical, char **err_msg) {
	try {
		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		auto rp = lotman::Lot::get_lots_past_ded(recursive_quota, recursive_children, hierarchical);
		if (!rp.second.empty()) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to get_lots_past_del: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> past_ded_lots = rp.first;

		auto past_ded_lots_c = static_cast<char **>(malloc(sizeof(char *) * (past_ded_lots.size() + 1)));
		past_ded_lots_c[past_ded_lots.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : past_ded_lots) {
			past_ded_lots_c[idx] = strdup(iter.c_str());
			if (!past_ded_lots_c[idx]) {
				lotman_free_string_list(past_ded_lots_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = past_ded_lots_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lots_past_obj(const bool recursive_quota, const bool recursive_children, char ***output,
							 const bool hierarchical, char **err_msg) {
	try {
		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		auto rp = lotman::Lot::get_lots_past_obj(recursive_quota, recursive_children, hierarchical);
		if (!rp.second.empty()) {
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to get_lots_past_del: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> past_obj_lots = rp.first;

		auto past_obj_lots_c = static_cast<char **>(malloc(sizeof(char *) * (past_obj_lots.size() + 1)));
		past_obj_lots_c[past_obj_lots.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : past_obj_lots) {
			past_obj_lots_c[idx] = strdup(iter.c_str());
			if (!past_obj_lots_c[idx]) {
				lotman_free_string_list(past_obj_lots_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = past_obj_lots_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_available_capacity(const char *parent_lot_name, int64_t start_time, int64_t end_time, char **output,
								  char **err_msg) {
	try {
		if (!parent_lot_name) {
			if (err_msg) {
				*err_msg = strdup("parent_lot_name must not be a null pointer.");
			}
			return -1;
		}
		if (!output) {
			if (err_msg) {
				*err_msg = strdup("output must not be a null pointer.");
			}
			return -1;
		}

		auto rp = lotman::Lot::get_available_capacity(parent_lot_name, start_time, end_time);
		if (!rp.second.empty()) {
			if (err_msg) {
				*err_msg = strdup(rp.second.c_str());
			}
			return -1;
		}

		*output = strdup(rp.first.dump().c_str());
		if (!*output) {
			if (err_msg) {
				*err_msg = strdup("Failed to allocate output buffer.");
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_list_all_lots(char ***output, char **err_msg) {
	try {
		auto rp = lotman::Lot::list_all_lots();
		if (!rp.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to list_all_lots: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> all_lots = rp.first;

		auto all_lots_c = static_cast<char **>(malloc(sizeof(char *) * (all_lots.size() + 1)));
		all_lots_c[all_lots.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : all_lots) {
			all_lots_c[idx] = strdup(iter.c_str());
			if (!all_lots_c[idx]) {
				lotman_free_string_list(all_lots_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = all_lots_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

// Given a lot_name and a recursive flag, generate the lot's information and return it as JSON. Recursive in this case
// indicates that we want to look up/down the tree of lots to determine the most restrictive values associated with
// parents/children.
int lotman_get_lot_as_json(const char *lot_name, const bool recursive, char **output, char **err_msg) {
	try {
		if (!lot_name) {
			if (err_msg) {
				*err_msg = strdup("Name for the lot to be returned as JSON must not be nullpointer.");
			}
			return -1;
		}

		// Check for existence of the lot we're asking about
		auto rp = lotman::Lot::lot_exists(lot_name);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) { // function worked, but lot does not exist
					*err_msg = strdup("That was easy! The lot does not exist, so there's nothing to return.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
		}

		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage()";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		lotman::Lot lot(lot_name);

		json output_obj;
		// Start populating fields in output_obj

		// Add name
		output_obj["lot_name"] = lot_name;

		// Add owner(s) according to recursive flag
		std::pair<std::vector<std::string>, std::string> rp_vec_str;
		rp_vec_str = lot.get_owners(recursive);
		if (!rp_vec_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_vec_str.second;
				std::string ext_err = "Failure on call to get_owners: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		if (recursive) {
			output_obj["owners"] = rp_vec_str.first;
		} else {
			if (rp_vec_str.first.empty()) {
				if (err_msg) {
					*err_msg = strdup("get_owners returned empty result");
				}
				return -1;
			}
			output_obj["owner"] = rp_vec_str.first[0]; // Only one owner, this is where it will be.
		}

		// Add parents according to recursive flag
		std::pair<std::vector<lotman::Lot>, std::string> rp_lotvec_str;
		rp_lotvec_str = lot.get_parents(recursive, true);
		if (!rp_lotvec_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_parents: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		std::vector<std::string> tmp;
		for (const auto &parent : rp_lotvec_str.first) {
			tmp.push_back(parent.lot_name);
		}
		output_obj["parents"] = tmp;

		// Add children according to recursive flag
		rp_lotvec_str = lot.get_children(recursive, false);
		if (!rp_lotvec_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_lotvec_str.second;
				std::string ext_err = "Failure on call to get_children: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		tmp = {};
		for (const auto &child : rp_lotvec_str.first) {
			tmp.push_back(child.lot_name);
		}
		output_obj["children"] = tmp;

		// Add paths according to recursive flag
		std::pair<json, std::string> rp_json_str;
		rp_json_str = lot.get_lot_dirs(recursive);
		if (!rp_json_str.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp_json_str.second;
				std::string ext_err = "Failure on call to get_lot_dirs: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}
		output_obj["paths"] = rp_json_str.first;

		// Add management policy attributes according to recursive flag
		std::array<std::string, 6> man_pol_keys = {"dedicated_GB",	"opportunistic_GB", "max_num_objects",
												   "creation_time", "deletion_time",	"expiration_time"};
		json internal_man_pol_obj;
		json internal_man_pol_obj_restrictive;
		for (const auto &key : man_pol_keys) {
			rp_json_str = lot.get_restricting_attribute(key, false);
			if (!rp_json_str.second.empty()) { // There was an error
				if (err_msg) {
					std::string int_err = rp_json_str.second;
					std::string ext_err = "Failure on call to get_restricting_attribute: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}

			internal_man_pol_obj[key] = rp_json_str.first["value"];

			if (recursive) {
				rp_json_str = lot.get_restricting_attribute(key, true);
				if (!rp_json_str.second.empty()) { // There was an error
					if (err_msg) {
						std::string int_err = rp_json_str.second;
						std::string ext_err = "Failure on call to get_restricting_attribute: ";
						*err_msg = strdup((ext_err + int_err).c_str());
					}
					return -1;
				}

				internal_man_pol_obj_restrictive[key] = rp_json_str.first;
			}
		}
		output_obj["management_policy_attrs"] = internal_man_pol_obj;

		if (recursive) {
			output_obj["restrictive_management_policy_attrs"] = internal_man_pol_obj_restrictive;
		}

		// Add usage according to recursive flag
		std::array<std::string, 6> usage_keys = {"dedicated_GB", "opportunistic_GB", "total_GB",
												 "num_objects",	 "GB_being_written", "objects_being_written"};
		json internal_usage_obj;
		for (const auto &key : usage_keys) {
			rp_json_str = lot.get_lot_usage(key, recursive);
			if (!rp_json_str.second.empty()) { // There was an error
				if (err_msg) {
					std::string int_err = rp_json_str.second;
					std::string ext_err = "Failure on call to get_lot_usage: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
				return -1;
			}
			internal_usage_obj[key] = rp_json_str.first;
		}
		output_obj["usage"] = internal_usage_obj;

		// Copy the object to output
		std::string output_str = output_obj.dump();
		auto output_str_c = static_cast<char *>(malloc(sizeof(char) * (output_str.length() + 1)));
		output_str_c = strdup(output_str.c_str());
		*output = output_str_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_lots_from_dir(const char *dir, const bool recursive, int64_t query_time, char ***output,
							 char **err_msg) {
	try {

		auto rp = lotman::Lot::get_lots_from_dir(dir, recursive, query_time);
		if (!rp.second.empty()) { // There was an error
			if (err_msg) {
				std::string int_err = rp.second;
				std::string ext_err = "Failure on call to list_all_lots: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		std::vector<std::string> lots_from_dir = rp.first;

		auto lots_from_dir_c = static_cast<char **>(malloc(sizeof(char *) * (lots_from_dir.size() + 1)));
		lots_from_dir_c[lots_from_dir.size()] = nullptr;
		int idx = 0;
		for (const auto &iter : lots_from_dir) {
			lots_from_dir_c[idx] = strdup(iter.c_str());
			if (!lots_from_dir_c[idx]) {
				lotman_free_string_list(lots_from_dir_c);
				if (err_msg) {
					*err_msg = strdup("Failed to create a copy of string entry in list");
				}
				return -1;
			}
			idx++;
		}
		*output = lots_from_dir_c;
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_set_context_str(const char *key, const char *value, char **err_msg) {
	try {
		if (!key) {
			if (err_msg) {
				*err_msg = strdup("A key must be provided.");
			}
			return -1;
		}

		if (strcmp(key, "caller") == 0) {
			lotman::Context::set_caller(value);
		} else if (strcmp(key, "lot_home") == 0) {
			lotman::Context::set_lot_home(value);
		} else if (strcmp(key, "strict_hierarchy") == 0) {
			if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
				lotman::Context::set_strict_hierarchy(true);
			} else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
				lotman::Context::set_strict_hierarchy(false);
			} else {
				if (err_msg) {
					*err_msg = strdup("Value for strict_hierarchy must be 'true', 'false', '1', or '0'.");
				}
				return -1;
			}
		} else if (strcmp(key, "contraction_policy") == 0) {
			auto rv = lotman::Context::set_contraction_policy(value);
			if (!rv.first) {
				if (err_msg) {
					*err_msg = strdup(rv.second.c_str());
				}
				return -1;
			}
		} else if (strcmp(key, "admin_override") == 0) {
			if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
				lotman::Context::set_admin_override(true);
			} else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
				lotman::Context::set_admin_override(false);
			} else {
				if (err_msg) {
					*err_msg = strdup("Value for admin_override must be 'true', 'false', '1', or '0'.");
				}
				return -1;
			}
		}

		else {
			if (err_msg) {
				std::string err = "Unrecognized key: " + static_cast<std::string>(key);
				*err_msg = strdup(err.c_str());
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_context_str(const char *key, char **output, char **err_msg) {
	try {
		if (!key) {
			if (err_msg) {
				*err_msg = strdup("A key must be provided.");
			}
			return -1;
		}

		if (strcmp(key, "caller") == 0) {
			*output = strdup(lotman::Context::get_caller().c_str());
		} else if (strcmp(key, "lot_home") == 0) {
			*output = strdup(lotman::Context::get_lot_home().c_str());
		} else if (strcmp(key, "strict_hierarchy") == 0) {
			*output = strdup(lotman::Context::get_strict_hierarchy() ? "true" : "false");
		} else if (strcmp(key, "contraction_policy") == 0) {
			*output = strdup(lotman::Context::get_contraction_policy().c_str());
		} else if (strcmp(key, "admin_override") == 0) {
			*output = strdup(lotman::Context::get_admin_override() ? "true" : "false");
		} else {
			if (err_msg) {
				std::string err = "Unrecognized key: " + static_cast<std::string>(key);
				*err_msg = strdup(err.c_str());
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_set_context_int(const char *key, const int value, char **err_msg) {
	try {
		if (!key) {
			if (err_msg) {
				*err_msg = strdup("A key must be provided.");
			}
			return -1;
		}

		if (strcmp(key, "db_timeout") == 0) {
			*lotman_db_timeout = value;
		}

		else {
			if (err_msg) {
				std::string err = "Unrecognized key: " + static_cast<std::string>(key);
				*err_msg = strdup(err.c_str());
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_get_context_int(const char *key, int *output, char **err_msg) {
	try {
		if (!key) {
			if (err_msg) {
				*err_msg = strdup("A key must be provided.");
			}
			return -1;
		}

		if (strcmp(key, "db_timeout") == 0) {
			*output = *lotman_db_timeout;
		} else {
			if (err_msg) {
				std::string err = "Unrecognized key: " + static_cast<std::string>(key);
				*err_msg = strdup(err.c_str());
			}
			return -1;
		}
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}
