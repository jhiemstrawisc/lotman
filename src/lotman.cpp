#include "lotman.h"

#include "lotman_db.h"
#include "lotman_internal.h"
#include "lotman_version.h"
#include "schemas.h"

#include <chrono>
#include <mutex>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>
#include <string.h>

// Process-wide mutex serializing every public lotman_* C API entry point.
//
// libLotMan internally uses a singleton SQLite handle (see StorageManager in
// lotman_db.h). SQLite is built without SQLITE_OPEN_FULLMUTEX, and
// sqlite_orm's prepared-statement cache is not thread-safe either, so
// concurrent access from multiple OS threads corrupts the connection and
// crashes (typically inside update_db_children_usage()).
//
// In practice this matters because libLotMan is loaded into a single process
// by *two* runtimes that the library itself cannot coordinate: Pelican's Go
// code (renewal / GC goroutines) and xrootd's C++ purge thread
// (XrdPurgeLotMan). Each runtime can supply its own mutex for its own
// callers, but they share no synchronization primitive across the language
// boundary. The only place where both can be serialized correctly is inside
// libLotMan itself.
//
// The mutex is recursive because a few public entry points internally
// re-enter other public entry points (notably the past_* readers call into
// update helpers).
static std::recursive_mutex g_lotman_api_mutex;
#define LOTMAN_API_LOCK() std::lock_guard<std::recursive_mutex> _lotman_api_lock(g_lotman_api_mutex)

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

namespace {
// Returns 0 if `lot_name` is not reclaimed (or the lot does not exist; in
// which case the caller's lot_exists check handles the not-found error).
// Returns -1 with err_msg populated when the lot has a reclamation row whose
// reclaimed_at <= now -- mutating APIs must reject in that case so the
// historical accounting record is preserved.
//
// TODO: Do functions like this need a temporal signature so someone can check reclamation in the future?
int reject_if_reclaimed(const std::string &lot_name, const char *op, char **err_msg) {
	auto now = std::chrono::system_clock::now();
	int64_t now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now).time_since_epoch().count();
	auto rp = lotman::Lot::is_reclaimed(lot_name, now_ms);
	if (!rp.second.empty()) {
		if (err_msg) {
			std::string msg = std::string("Failure on call to is_reclaimed while attempting ") + op + ": " + rp.second;
			*err_msg = strdup(msg.c_str());
		}
		return -1;
	}
	if (rp.first) {
		if (err_msg) {
			std::string msg = "Lot '" + lot_name + "' is reclaimed and cannot be modified (operation: " + op + ").";
			*err_msg = strdup(msg.c_str());
		}
		return -1;
	}
	return 0;
}
} // namespace

int lotman_add_lot(const char *lotman_JSON_str, char **err_msg) {
	LOTMAN_API_LOCK();
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

		// Reclamation is a terminal ledger fact: a reclaimed lot's subtree has
		// been "released" back to the default lot. Adding a live child under a
		// reclaimed parent would resurrect that subtree and violate the
		// invariant that reclaimed storage is hoovered into default. Reject.
		for (const auto &parent_name : lot.parents) {
			if (parent_name == lot.lot_name) {
				continue; // Self-parent is a root marker, not an actual ancestor edge.
			}
			if (reject_if_reclaimed(parent_name, "lotman_add_lot", err_msg) != 0) {
				return -1;
			}
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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

int lotman_reclaim_lot(const char *lot_name, int64_t reclaimed_at, const char *reason, char **err_msg) {
	LOTMAN_API_LOCK();
	try {
		if (!lot_name) {
			if (err_msg) {
				*err_msg = strdup("lot_name must not be a null pointer.");
			}
			return -1;
		}
		if (reclaimed_at <= 0) {
			if (err_msg) {
				*err_msg = strdup("reclaimed_at must be a positive Unix timestamp in milliseconds.");
			}
			return -1;
		}
		std::string lot_name_s{lot_name};
		std::string reason_s = reason ? std::string{reason} : std::string{};

		auto rp = lotman::Lot::lot_exists(lot_name_s);
		if (!rp.first) {
			if (err_msg) {
				if (rp.second.empty()) {
					*err_msg = strdup("Lot does not exist, so it cannot be reclaimed.");
				} else {
					std::string int_err = rp.second;
					std::string ext_err = "Function call to lotman::Lot::lot_exists failed: ";
					*err_msg = strdup((ext_err + int_err).c_str());
				}
			}
			return -1;
		}

		lotman::Lot lot(lot_name_s);

		// Authorization: caller must own the lot, defined the same way as
		// lotman_remove_lots_recursive (caller owns at least one of its parents,
		// or owns it via self-parent). Authorization to the root authorizes the
		// cascade.
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

		auto reclaim_rp = lot.reclaim_lot_with_descendants(reclaimed_at, reason_s);
		if (reclaim_rp.first == LOTMAN_RECLAIM_ERROR) {
			if (err_msg) {
				std::string int_err = reclaim_rp.second;
				std::string ext_err = "Failed to reclaim lot: ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return LOTMAN_RECLAIM_ERROR;
		}
		return reclaim_rp.first;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return LOTMAN_RECLAIM_ERROR;
	}
}

int lotman_update_lot(const char *lotman_JSON_str, char **err_msg) {
	LOTMAN_API_LOCK();
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

		if (reject_if_reclaimed(lot.lot_name, "lotman_update_lot", err_msg) != 0) {
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
				if (!lot.update_parents_in_txn(update_JSON_obj["parents"], txn_error,
											   /*revalidate_paths=*/false)) {
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
				// the lot's final MPA triple and enforce the sentinel
				// invariants. Per-field updates intentionally tolerate
				// transient partial-zero state inside the transaction so a
				// caller can flip a lot to/from non-expiring (timestamps) or
				// to/from unbounded storage (dedicated_GB/opportunistic_GB)
				// atomically; this final pass rejects any update whose end
				// state leaves the timestamps or storage axis in an
				// inconsistent state. We also re-run the hierarchy axioms
				// here because per-field axiom checks defer for partial
				// intermediate states.
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
				auto mpa_inv_rp = lotman::validate_mpa_invariants(mpa_final->dedicated_GB, mpa_final->opportunistic_GB,
																  mpa_final->max_num_objects);
				if (!mpa_inv_rp.first) {
					txn_error = "Update on lot '" + update_JSON_obj["lot_name"].get<std::string>() +
								"' rejected: " + mpa_inv_rp.second;
					return false;
				}
				if (lotman::Context::get_strict_hierarchy()) {
					const std::string &final_name = update_JSON_obj["lot_name"].get<std::string>();
					auto a1 = lotman::Lot::validate_axiom1(final_name);
					if (!a1.first) {
						txn_error = a1.second;
						return false;
					}
					auto a2 = lotman::Lot::validate_axiom2_for_parents_of(final_name);
					if (!a2.first) {
						txn_error = a2.second;
						return false;
					}
					auto a3 = lotman::Lot::validate_axiom3(final_name);
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

			// Final descendancy revalidation against the fully-committed
			// state of this transaction. update_parents_in_txn was asked to
			// skip its inline revalidation so that simultaneous parent +
			// path edits are evaluated against the final ancestry rather
			// than an intermediate state. We only need to revalidate when
			// the caller actually touched parents or paths;
			// revalidate_paths_in_txn itself is a no-op for the default lot.
			if (update_JSON_obj.contains("parents") || update_JSON_obj.contains("paths")) {
				if (!lot.revalidate_paths_in_txn(txn_error)) {
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
	LOTMAN_API_LOCK();
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

		if (reject_if_reclaimed(lot.lot_name, "lotman_rm_parents_from_lot", err_msg) != 0) {
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
	LOTMAN_API_LOCK();
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

				if (reject_if_reclaimed(matching_lot_name, "lotman_rm_paths_from_lots", err_msg) != 0) {
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
	LOTMAN_API_LOCK();
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

		if (reject_if_reclaimed(lot.lot_name, "lotman_add_to_lot", err_msg) != 0) {
			return -1;
		}

		// Run all additions inside a single outer storage.transaction() so the
		// envelope is atomic: a late failure (e.g. attribution validation)
		// rolls back any preceding parent/path additions.
		auto &storage = lotman::db::StorageManager::get_storage();
		std::string txn_error;
		bool committed = storage.transaction([&]() -> bool {
			// If the caller supplied parent_attributions, route the JSON through
			// the parent-add flow so the post-add equal-split / axiom validation
			// uses the explicit shares rather than auto-dividing the lot's MPAs
			// across all (existing + newly-added) parents and rejecting the
			// auto-split before the explicit shares can take effect.
			const json explicit_attrs =
				addition_obj.contains("parent_attributions") ? addition_obj["parent_attributions"] : json();

			if (addition_obj.contains("parents")) {
				std::vector<lotman::Lot> parent_lots;
				for (const auto &parent_name : addition_obj["parents"]) {
					lotman::Lot parent_lot(parent_name.get<std::string>());
					parent_lots.push_back(parent_lot);
				}

				if (!lot.add_parents_in_txn(parent_lots, txn_error, explicit_attrs)) {
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

			// If parent_attributions was supplied alongside `parents`, the
			// attributions were already applied inside add_parents_in_txn above
			// using the merged parent set. Only re-run when no `parents` were
			// added (i.e. caller is just rewriting attributions on the existing
			// parent set).
			if (addition_obj.contains("parent_attributions") && !addition_obj.contains("parents")) {
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
	int idx = 0;
	do {
		free(str_list[idx++]);
	} while (str_list[idx]);
	free(str_list);
}

int lotman_get_parent_names(const char *lot_name, const bool recursive, const bool get_self, char ***output,
							char **err_msg) {
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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

		if (reject_if_reclaimed(lot.lot_name, "lotman_update_lot_usage", err_msg) != 0) {
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
	if (err_msg) {
		*err_msg = strdup("This function is not yet implemented...");
	}
	return -1;
}

int lotman_get_lots_past_exp(int64_t query_time, const bool recursive, const bool include_reclaimed, char ***output,
							 char **err_msg) {
	LOTMAN_API_LOCK();
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

		auto rp = lotman::Lot::get_lots_past_exp(query_time, recursive, include_reclaimed);
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

int lotman_get_lots_past_del(int64_t query_time, const bool recursive, const bool include_reclaimed, char ***output,
							 char **err_msg) {
	LOTMAN_API_LOCK();
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

		auto rp = lotman::Lot::get_lots_past_del(query_time, recursive, include_reclaimed);
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

int lotman_get_lots_past_opp(const bool recursive_quota, const bool recursive_children, const bool include_reclaimed,
							 char ***output, const bool hierarchical, char **err_msg) {
	LOTMAN_API_LOCK();
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

		auto rp = lotman::Lot::get_lots_past_opp(recursive_quota, recursive_children, include_reclaimed, hierarchical);
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

int lotman_get_lots_past_ded(const bool recursive_quota, const bool recursive_children, const bool include_reclaimed,
							 char ***output, const bool hierarchical, char **err_msg) {
	LOTMAN_API_LOCK();
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

		auto rp = lotman::Lot::get_lots_past_ded(recursive_quota, recursive_children, include_reclaimed, hierarchical);
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

int lotman_get_lots_past_obj(const bool recursive_quota, const bool recursive_children, const bool include_reclaimed,
							 char ***output, const bool hierarchical, char **err_msg) {
	LOTMAN_API_LOCK();
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

		auto rp = lotman::Lot::get_lots_past_obj(recursive_quota, recursive_children, include_reclaimed, hierarchical);
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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

// Helper: build the JSON object for a single lot. Caller is responsible for
// ensuring the lot exists and that update_db_children_usage has been invoked
// once for the batch (so this helper can be called many times in a row from
// list-returning APIs without re-running expensive work each iteration).
// Mirrors the field set documented for lotman_get_lot_as_json.
static std::pair<json, std::string> build_lot_json_obj(const std::string &lot_name, bool recursive) {
	json output_obj;
	output_obj["lot_name"] = lot_name;

	lotman::Lot lot(lot_name);

	auto rp_owners = lot.get_owners(recursive);
	if (!rp_owners.second.empty()) {
		return {json(), "Failure on call to get_owners: " + rp_owners.second};
	}
	if (recursive) {
		output_obj["owners"] = rp_owners.first;
	} else {
		if (rp_owners.first.empty()) {
			return {json(), "get_owners returned empty result"};
		}
		output_obj["owner"] = rp_owners.first[0];
	}

	auto rp_parents = lot.get_parents(recursive, true);
	if (!rp_parents.second.empty()) {
		return {json(), "Failure on call to get_parents: " + rp_parents.second};
	}
	std::vector<std::string> tmp;
	for (const auto &parent : rp_parents.first) {
		tmp.push_back(parent.lot_name);
	}
	output_obj["parents"] = tmp;

	auto rp_children = lot.get_children(recursive, false);
	if (!rp_children.second.empty()) {
		return {json(), "Failure on call to get_children: " + rp_children.second};
	}
	tmp.clear();
	for (const auto &child : rp_children.first) {
		tmp.push_back(child.lot_name);
	}
	output_obj["children"] = tmp;

	auto rp_dirs = lot.get_lot_dirs(recursive);
	if (!rp_dirs.second.empty()) {
		return {json(), "Failure on call to get_lot_dirs: " + rp_dirs.second};
	}
	output_obj["paths"] = rp_dirs.first;

	std::array<std::string, 6> man_pol_keys = {"dedicated_GB",	"opportunistic_GB", "max_num_objects",
											   "creation_time", "deletion_time",	"expiration_time"};
	json internal_man_pol_obj;
	json internal_man_pol_obj_restrictive;
	for (const auto &key : man_pol_keys) {
		auto rp_attr = lot.get_restricting_attribute(key, false);
		if (!rp_attr.second.empty()) {
			return {json(), "Failure on call to get_restricting_attribute: " + rp_attr.second};
		}
		internal_man_pol_obj[key] = rp_attr.first["value"];

		if (recursive) {
			auto rp_attr_rec = lot.get_restricting_attribute(key, true);
			if (!rp_attr_rec.second.empty()) {
				return {json(), "Failure on call to get_restricting_attribute: " + rp_attr_rec.second};
			}
			internal_man_pol_obj_restrictive[key] = rp_attr_rec.first;
		}
	}
	output_obj["management_policy_attrs"] = internal_man_pol_obj;
	if (recursive) {
		output_obj["restrictive_management_policy_attrs"] = internal_man_pol_obj_restrictive;
	}

	std::array<std::string, 6> usage_keys = {"dedicated_GB", "opportunistic_GB", "total_GB",
											 "num_objects",	 "GB_being_written", "objects_being_written"};
	json internal_usage_obj;
	for (const auto &key : usage_keys) {
		auto rp_usage = lot.get_lot_usage(key, recursive);
		if (!rp_usage.second.empty()) {
			return {json(), "Failure on call to get_lot_usage: " + rp_usage.second};
		}
		internal_usage_obj[key] = rp_usage.first;
	}
	output_obj["usage"] = internal_usage_obj;

	auto rp_attr = lot.get_parent_attributions();
	if (!rp_attr.second.empty()) {
		return {json(), "Failure on call to get_parent_attributions: " + rp_attr.second};
	}
	output_obj["parent_attributions"] = rp_attr.first;

	try {
		auto &storage = lotman::db::StorageManager::get_storage();
		auto reclaim_row = storage.get_pointer<lotman::db::Reclamation>(lot_name);
		if (reclaim_row) {
			json r;
			r["reclaimed_at"] = reclaim_row->reclaimed_at;
			r["reason"] = reclaim_row->reclaimed_reason;
			output_obj["reclamation"] = r;
		}
	} catch (const std::exception &e) {
		return {json(), std::string("Failure on reclamation lookup: ") + e.what()};
	}

	return {output_obj, ""};
}

// Given a lot_name and a recursive flag, generate the lot's information and return it as JSON. Recursive in this case
// indicates that we want to look up/down the tree of lots to determine the most restrictive values associated with
// parents/children.
int lotman_get_lot_as_json(const char *lot_name, const bool recursive, char **output, char **err_msg) {
	LOTMAN_API_LOCK();
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

		auto rp_obj = build_lot_json_obj(lot_name, recursive);
		if (!rp_obj.second.empty()) {
			if (err_msg) {
				*err_msg = strdup(rp_obj.second.c_str());
			}
			return -1;
		}

		std::string output_str = rp_obj.first.dump();
		*output = strdup(output_str.c_str());
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
	LOTMAN_API_LOCK();
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

int lotman_get_lots_for_path(const char *path, bool recursive, int64_t time_lo_ms, int64_t time_hi_ms,
							 bool include_reclaimed, char **output, char **err_msg) {
	LOTMAN_API_LOCK();
	try {
		if (!path) {
			if (err_msg) {
				*err_msg = strdup("path must not be nullpointer.");
			}
			return -1;
		}

		auto rp_bool_str = lotman::Lot::update_db_children_usage();
		if (!rp_bool_str.first) {
			if (err_msg) {
				std::string int_err = rp_bool_str.second;
				std::string ext_err = "Failure on call to update_db_children_usage(): ";
				*err_msg = strdup((ext_err + int_err).c_str());
			}
			return -1;
		}

		auto rp = lotman::Lot::get_lots_for_path(path, recursive, time_lo_ms, time_hi_ms, include_reclaimed);
		if (!rp.second.empty()) {
			if (err_msg) {
				std::string ext_err = "Failure on call to get_lots_for_path: ";
				*err_msg = strdup((ext_err + rp.second).c_str());
			}
			return -1;
		}

		// Build a JSON array, one full lot object per winner. Use the same
		// shape as lotman_get_lot_as_json(name, recursive=false) so callers
		// can treat each element identically. update_db_children_usage was
		// already called once above, so build_lot_json_obj is cheap to call
		// per-lot here.
		json arr = json::array();
		for (const auto &lot_name : rp.first) {
			auto rp_obj = build_lot_json_obj(lot_name, /*recursive=*/false);
			if (!rp_obj.second.empty()) {
				if (err_msg) {
					std::string ext_err = "Failure building JSON for lot '" + lot_name + "': ";
					*err_msg = strdup((ext_err + rp_obj.second).c_str());
				}
				return -1;
			}
			arr.push_back(rp_obj.first);
		}

		std::string output_str = arr.dump();
		*output = strdup(output_str.c_str());
		return 0;
	} catch (std::exception &exc) {
		if (err_msg) {
			*err_msg = strdup(exc.what());
		}
		return -1;
	}
}

int lotman_set_context_str(const char *key, const char *value, char **err_msg) {
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
	LOTMAN_API_LOCK();
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
