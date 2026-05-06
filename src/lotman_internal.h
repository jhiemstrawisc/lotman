// #include <algorithm>
// #include <stdio.h>
// #include <string>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace lotman {

/**
 * Ensures that a path has a trailing slash.
 * This is used to normalize all paths stored in the database, preventing
 * string matching bugs where paths like "/foo" and "/foobar" could be
 * incorrectly matched.
 *
 * @param path The input path (should be non-empty in normal usage)
 * @return The path with a trailing slash appended if not already present.
 *         Empty paths return "/" as a sensible default.
 */
inline std::string ensure_trailing_slash(const std::string &path) {
	if (path.empty()) {
		return "/";
	}
	if (path.back() != '/') {
		return path + "/";
	}
	return path;
}

class Checks;
class Context;
/**
 * The Lot class has member functions for handling top-level actions with lots
 * Eg, adding lots, removing lots, updating lots etc
 */

using json = nlohmann::json;

// Sentinel: a lot whose creation_time, expiration_time, and deletion_time are
// all zero is treated as "non-expiring". This sentinel is all-or-nothing --
// any mix of zero and non-zero timestamps on a single lot is an error.
inline bool is_non_expiring(int64_t creation_time, int64_t expiration_time, int64_t deletion_time) {
	return creation_time == 0 && expiration_time == 0 && deletion_time == 0;
}

// Resource-axis sentinels: a value of -1 on a quota MPA marks that axis as
// "unbounded". 0 is therefore reserved for its literal meaning ("zero
// guaranteed storage", "zero burst capacity", "zero objects allowed"), so
// callers can express purely-opportunistic lots, no-burst lots, etc.
//
// The three resource MPAs are treated as three independent sub-axes:
//   * dedicated_GB:     -1 == unbounded, 0 == no guaranteed storage.
//   * opportunistic_GB: -1 == unbounded burst, 0 == no burst capacity.
//   * max_num_objects:  -1 == unbounded, 0 == no objects allowed.
//
// Cross-axis storage rule: an unbounded dedicated cap with a finite
// opportunistic cap is meaningless (opportunistic is "data over the
// dedicated allotment"); therefore (dedicated_GB == -1 && opportunistic_GB
// != -1) is rejected. The reverse (finite dedicated, unbounded opportunistic)
// is allowed and means "guaranteed N GB plus unlimited burst".
inline bool is_unbounded_dedicated(double dedicated_GB) {
	return dedicated_GB == -1.0;
}

inline bool is_unbounded_opportunistic(double opportunistic_GB) {
	return opportunistic_GB == -1.0;
}

inline bool is_unbounded_objects(int64_t max_num_objects) {
	return max_num_objects == -1;
}

// True when the storage axis is in the forbidden combination
// (dedicated_GB == -1 with opportunistic_GB != -1). This may occur as a
// transient state mid-transaction during a multi-field update where
// dedicated_GB has been written first; hierarchy validation predicates
// short-circuit when they encounter it and rely on a post-loop check in
// lotman_update_lot to reject any persisting partial state.
inline bool is_partial_storage_sentinel(double dedicated_GB, double opportunistic_GB) {
	return dedicated_GB == -1.0 && opportunistic_GB != -1.0;
}

// Validate the per-axis MPA invariants for a lot:
//   * Each MPA must be either >= 0 or exactly -1 (the unbounded sentinel).
//   * Storage axis: dedicated_GB == -1 requires opportunistic_GB == -1.
// Returns (true, "") when valid, otherwise (false, message).
inline std::pair<bool, std::string> validate_mpa_invariants(double dedicated_GB, double opportunistic_GB,
															int64_t max_num_objects) {
	auto bad = [](double v) { return v < 0.0 && v != -1.0; };
	if (bad(dedicated_GB) || bad(opportunistic_GB) || (max_num_objects < 0 && max_num_objects != -1)) {
		return std::make_pair(false, std::string("MPAs must be >= 0 or exactly -1 (the unbounded sentinel); got "
												 "dedicated_GB=") +
										 std::to_string(dedicated_GB) +
										 ", opportunistic_GB=" + std::to_string(opportunistic_GB) +
										 ", max_num_objects=" + std::to_string(max_num_objects) + ".");
	}
	if (is_partial_storage_sentinel(dedicated_GB, opportunistic_GB)) {
		return std::make_pair(
			false, std::string("Storage-axis sentinel violation: when dedicated_GB is -1 (unbounded), "
							   "opportunistic_GB must also be -1. An unbounded dedicated allotment "
							   "makes a finite opportunistic cap meaningless. Got dedicated_GB=") +
					   std::to_string(dedicated_GB) + ", opportunistic_GB=" + std::to_string(opportunistic_GB) + ".");
	}
	return std::make_pair(true, "");
}

// Validate the time-field invariants for a lot's MPAs. Enforces:
//   * If any of creation_time/expiration_time/deletion_time is 0, all three
//     must be 0 (non-expiring sentinel).
//   * Otherwise, creation_time must be strictly less than expiration_time
//     (the active interval [creation_time, expiration_time) must be non-empty).
// Returns (true, "") when valid, otherwise (false, message).
inline std::pair<bool, std::string> validate_time_invariants(int64_t creation_time, int64_t expiration_time,
															 int64_t deletion_time) {
	bool any_zero = (creation_time == 0) || (expiration_time == 0) || (deletion_time == 0);
	bool all_zero = is_non_expiring(creation_time, expiration_time, deletion_time);
	if (any_zero && !all_zero) {
		return std::make_pair(false, std::string("A lot using 0 (the unix epoch) as a timestamp sentinel for "
												 "non-expiring lots must use 0 for all of creation_time, "
												 "expiration_time, and deletion_time. Got creation_time=") +
										 std::to_string(creation_time) +
										 ", expiration_time=" + std::to_string(expiration_time) +
										 ", deletion_time=" + std::to_string(deletion_time) + ".");
	}
	if (all_zero) {
		return std::make_pair(true, "");
	}
	if (creation_time >= expiration_time) {
		return std::make_pair(false, "creation_time must be strictly less than expiration_time (half-open interval "
									 "[creation, expiration) must be non-empty)");
	}
	if (deletion_time < expiration_time) {
		return std::make_pair(false, "deletion_time must be greater than or equal to expiration_time (a lot cannot "
									 "be deleted before it expires)");
	}
	return std::make_pair(true, "");
}

// std::vector<std::string> get_lname_str_vec(std::vector<Lot> lot_vec) {
//     std::vector<std::string> str_vec(lot_vec.size());
//     std::transform(lot_vec.begin(), lot_vec.end(), str_vec.begin(),
//                     [](const Lot& lot) { return lot.lot_name; });
//     return str_vec;
// }

// std::vector<Lot> get_union_without_intersect(std::vector<std::vector<Lot>> input_vecs) {
//     std::vector<std::string> vecs_union;
//     std::vector<std::string> vecs_intersect;

//     // Convert to strings
//     std::vector<std::vector<std::string>> str_vecs;
//     for (const auto &vec : input_vecs) {
//         str_vecs.push_back(get_lname_str_vec(vec));
//     }

//     // Get the union of the vecs
//     for (const auto &vec : str_vecs) {
//         for (const auto &elem : vec) {
//             if (std::find(vecs_union.begin(), vecs_union.end(), elem) == vecs_union.end()) {
//                 vecs_union.push_back(elem);
//             }
//         }
//     }

//     // Get the intersection
//     vecs_intersect = str_vecs[0];
//     // Sort the first vector to prepare for binary search
//     std::sort(vecs_intersect.begin(), vecs_intersect.end());

//     // Iterate through the remaining vectors
//     for (size_t i = 1; i < str_vecs.size(); ++i) {
//         std::vector<std::string> currentVector = str_vecs[i];
//         std::sort(currentVector.begin(), currentVector.end());

//         std::vector<std::string> tempVector;

//         // Find the common elements using binary search
//         std::set_intersection(vecs_intersect.begin(), vecs_intersect.end(),
//                               currentVector.begin(), currentVector.end(),
//                               std::back_inserter(tempVector));

//         vecs_intersect = std::move(tempVector);
//     }

//     // Now go through union and only collect things not in intersection
//     std::vector<std::string> u_without_i;
//     for (const auto &elem : vecs_union) {
//         if (std::find(vecs_intersect.begin(), vecs_intersect.end(), elem) == vecs_intersect.end()) {
//             u_without_i.push_back(elem);
//         }
//     }

//     return u_without_i;
// }

class Lot {
  public:
	// Non-object values used for lot initialization
	std::string lot_name;
	std::string owner;
	std::vector<std::string> parents;
	std::vector<std::string> children;
	std::vector<json> paths;

	// Things that belong to the lot when including parent/child relationships
	std::string self_owner;
	std::vector<Lot> self_parents;
	bool self_parents_loaded = false;
	std::vector<Lot> self_children;
	bool self_children_loaded = false;

	// Things that belong to the lot when including parent/child relationships
	std::vector<std::string> recursive_owners;
	std::vector<Lot> recursive_parents;
	bool recursive_parents_loaded = false;
	std::vector<Lot> recursive_children;
	bool recursive_children_loaded = false;

	// management policy attributes
	struct {
		double dedicated_GB;
		double opportunistic_GB;
		int64_t max_num_objects;
		int64_t creation_time;
		int64_t expiration_time;
		int64_t deletion_time;
	} man_policy_attr;

	// usage attributes
	struct {
		double self_GB;
		bool self_GB_update_staged;
		double children_GB;
		int64_t self_objects;
		bool self_objects_update_staged;
		int64_t children_objects;
		double self_GB_being_written;
		bool self_GB_being_written_update_staged;
		double children_GB_being_written;
		int64_t self_objects_being_written;
		bool self_objects_being_written_update_staged;
		int64_t children_objects_being_written;
	} usage;

	struct {
		bool assign_LTBR_parent_as_parent_to_orphans;
		bool assign_LTBR_parent_as_parent_to_non_orphans;
		bool assign_policy_to_children;
	} reassignment_policy;

	// Should re-evaluate whether all/any of these are needed...
	bool full_lot = false;
	bool has_name = false;
	bool has_reassignment_policy = false;
	bool is_root;

	// Constructors
	Lot() {};
	Lot(const char *lot_name) : lot_name{lot_name}, has_name{true} {};
	Lot(std::string lot_name) : lot_name{lot_name}, has_name{true} {};
	Lot(json lot_JSON) {
		init_full(lot_JSON);
	};

	std::pair<bool, std::string> init_full(json lot_JSON);
	std::pair<bool, std::string> init_reassignment_policy(const bool assign_LTBR_parent_as_parent_to_orphans,
														  const bool assign_LTBR_parent_as_parent_to_non_orphans,
														  const bool assign_policy_to_children);
	void init_self_usage();
	static std::pair<bool, std::string> lot_exists(const std::string &lot_name);
	std::pair<bool, std::string> check_if_root();
	std::pair<bool, std::string> store_lot(const json &parent_attributions_json = json());
	std::pair<bool, std::string> destroy_lot();
	std::pair<bool, std::string> destroy_lot_recursive();

	std::pair<std::vector<Lot>, std::string> get_children(const bool recursive = false, const bool get_self = false);

	std::pair<std::vector<Lot>, std::string> get_parents(const bool recursive = false, const bool get_self = false);

	std::pair<std::vector<std::string>, std::string> get_owners(const bool recursive = false);

	std::pair<json, std::string> get_restricting_attribute(const std::string &key, const bool recursive);

	std::pair<json, std::string> get_lot_dirs(const bool recursive);
	static std::pair<std::string, std::string> get_lot_from_dir(const std::string &dir_path, int64_t query_time);

	std::pair<json, std::string> get_lot_usage(const std::string &key, const bool recursive);

	std::pair<bool, std::string> add_parents(const std::vector<Lot> &parents);
	std::pair<bool, std::string> add_paths(const std::vector<json> &paths);

	std::pair<bool, std::string> remove_parents(const std::vector<std::string> &parents);
	std::pair<bool, std::string> remove_paths(const std::vector<std::string> &paths);

	std::pair<bool, std::string> update_owner(const std::string &update_val);
	std::pair<bool, std::string> update_parents(const json &update_arr);
	std::pair<bool, std::string> update_paths(const json &update_arr);
	std::pair<bool, std::string> update_man_policy_attrs(const std::string &update_key, double update_val);
	std::pair<bool, std::string> update_self_usage(const std::string &key, const double value, bool deltaMode);

	std::pair<bool, std::string> recalculate_children_usage();
	static std::pair<bool, std::string> update_db_children_usage();
	std::pair<bool, std::string> update_parent_usage(
		Lot parent, const std::string &update_stmt,
		const std::map<std::string, std::vector<int>> &update_str_map = std::map<std::string, std::vector<int>>(),
		const std::map<int64_t, std::vector<int>> &update_int_map = std::map<int64_t, std::vector<int>>(),
		const std::map<double, std::vector<int>> &update_dbl_map = std::map<double, std::vector<int>>());
	static std::pair<bool, std::string> update_usage_by_dirs(const json &update_JSON, bool deltaMode,
															 int64_t query_time);
	std::pair<bool, std::string> check_context_for_parents(const std::vector<std::string> &parents,
														   bool include_self = false, bool new_lot = false);
	std::pair<bool, std::string> check_context_for_parents(const std::vector<Lot> &parents, bool include_self = false,
														   bool new_lot = false);

	std::pair<bool, std::string> check_context_for_children(const std::vector<std::string> &children,
															bool include_self = false);
	std::pair<bool, std::string> check_context_for_children(const std::vector<Lot> &children,
															bool include_self = false);
	static std::pair<std::vector<std::string>, std::string> get_lots_past_exp(const bool recursive);
	static std::pair<std::vector<std::string>, std::string> get_lots_past_del(const bool recursive);
	static std::pair<std::vector<std::string>, std::string> get_lots_past_opp(const bool recursive_quota,
																			  const bool recursive_children);
	static std::pair<std::vector<std::string>, std::string> get_lots_past_ded(const bool recursive_quota,
																			  const bool recursive_children);
	static std::pair<std::vector<std::string>, std::string> get_lots_past_obj(const bool recursive_quota,
																			  const bool recursive_children);

	// Hierarchical-aware overloads: when hierarchical=true, uses adjusted usage
	// and returns depth-ordered results (deepest/leaf lots first).
	// NOTE: When hierarchical=true, recursive_quota and recursive_children are
	// not used — the hierarchical SQL query has its own built-in logic.
	// They are only forwarded to the non-hierarchical path when hierarchical=false.
	static std::pair<std::vector<std::string>, std::string>
	get_lots_past_opp(const bool recursive_quota, const bool recursive_children, const bool hierarchical);
	static std::pair<std::vector<std::string>, std::string>
	get_lots_past_ded(const bool recursive_quota, const bool recursive_children, const bool hierarchical);
	static std::pair<std::vector<std::string>, std::string>
	get_lots_past_obj(const bool recursive_quota, const bool recursive_children, const bool hierarchical);

	// Strict hierarchy validation methods
	static std::pair<bool, std::string> validate_axiom1(const std::string &child_lot_name);
	static std::pair<bool, std::string> validate_axiom2_for_parents_of(const std::string &child_lot_name);
	static std::pair<bool, std::string> validate_axiom3(const std::string &child_lot_name);

	// Validation predicate: returns (success, error_message)
	using ValidationPredicate = std::function<std::pair<bool, std::string>()>;

	// Apply predicates sequentially; return first failure or (true, "")
	static std::pair<bool, std::string> apply_validation_predicates(const std::vector<ValidationPredicate> &predicates);

	// Build axiom predicates for a lot. If check_children is true, also validates
	// each direct child against the lot. Returns empty vector if strict hierarchy is off.
	static std::vector<ValidationPredicate> build_axiom_predicates(const std::string &lot_name,
																   bool check_children = false);

	// Attribution computation: compute how a child's MPAs are split across its parents,
	// then store into parent_child_attributions table.
	// parent_attributions_json is optional; missing parents get equal split of remainder.
	std::pair<bool, std::string> compute_and_store_attributions(const json &parent_attributions_json = json());

	// Update existing attributions for this lot
	std::pair<bool, std::string> update_attributions(const json &parent_attributions_json);

	// Contraction policy check for lot deletion. Returns (false, reason) if blocked.
	static std::pair<bool, std::string> check_contraction_for_deletion(const std::string &lot_name);

	// Check whether a lot is currently "alive" (creation_time <= now < expiration_time;
	// i.e. the active interval is half-open).
	// Returns (true, "") if alive, (false, "") if not alive, or (false, error) on failure.
	static std::pair<bool, std::string> is_lot_alive(const std::string &lot_name);
	static std::pair<std::vector<std::string>, std::string> list_all_lots();
	static std::pair<std::vector<std::string>, std::string> get_lots_from_dir(const std::string &dir,
																			  const bool recursive, int64_t query_time);

	// Advisory: compute available capacity under a parent lot during a time window.
	// Returns JSON with available and peak values for each resource type.
	// WARNING: This is NOT the reservation mechanism. Capacity enforcement is
	// performed atomically by validate_axiom2_for_parents_of inside the lot creation transaction.
	// Use this method only for informational/monitoring purposes.
	static std::pair<json, std::string> get_available_capacity(const std::string &parent_lot_name, int64_t start_time,
															   int64_t end_time);

	// Non-transactional update helpers used by the C-API dispatchers
	// (lotman_update_lot / lotman_add_to_lot) so the entire dispatch can
	// run inside a single outer storage.transaction(). Each *_in_txn helper
	// performs the same work as its public counterpart (including any
	// per-step axiom revalidation), but does NOT open its own transaction.
	// Returns false and sets txn_error on failure to trigger rollback.
	bool update_owner_in_txn(const std::string &update_val, std::string &txn_error);
	bool update_parents_in_txn(const json &update_arr, std::string &txn_error);
	bool update_paths_in_txn(const json &update_arr, std::string &txn_error);
	bool update_man_policy_attrs_in_txn(const std::string &update_key, double update_val, std::string &txn_error);
	bool update_attributions_in_txn(const json &parent_attributions_json, std::string &txn_error);
	bool add_paths_in_txn(const std::vector<json> &paths, std::string &txn_error);
	// `parent_attributions_json` lets the caller supply explicit per-parent shares
	// to use for the post-add equal-split / axiom validation; pass json() to fall
	// back to a pure equal split across all parents.
	bool add_parents_in_txn(const std::vector<Lot> &parents, std::string &txn_error,
							const json &parent_attributions_json = json());

  private:
	std::pair<bool, std::string> write_new();
	std::pair<bool, std::string> delete_lot_from_db();
	std::pair<bool, std::string> store_new_parents(const std::vector<Lot> &new_parents);
	std::pair<bool, std::string> store_updates(
		const std::string &update_query,
		const std::map<std::string, std::vector<int>> &update_str_map = std::map<std::string, std::vector<int>>(),
		const std::map<int64_t, std::vector<int>> &update_int_map = std::map<int64_t, std::vector<int>>(),
		const std::map<double, std::vector<int>> &update_dbl_map = std::map<double, std::vector<int>>());
	std::pair<bool, std::string> remove_parents_from_db(const std::vector<std::string> &parents);
	std::pair<bool, std::string> remove_paths_from_db(const std::vector<std::string> &paths);

	// Check whether a path conflicts with another lot's claim during overlapping time ranges.
	// Caller MUST be inside an active storage.transaction().
	static std::pair<bool, std::string> check_path_temporal_overlap(const std::string &lot_name,
																	const std::string &normalized_path,
																	int64_t creation_time, int64_t expiration_time);

	// Non-transactional helpers for composing inside an outer transaction.
	// Callers MUST hold an active storage.transaction().
	bool add_parents_impl(const std::vector<Lot> &parents, std::string &txn_error,
						  const json &parent_attributions_json = json());
	bool update_parents_impl(const json &update_arr, std::string &txn_error);

	// Reload parents + MPAs from DB, clear old attributions, recompute with the
	// supplied per-parent shares (equal split for any unspecified parents), and
	// validate axioms. Caller MUST hold an active storage.transaction().
	// Returns false and sets txn_error on failure.
	bool reload_and_recompute_attributions(std::string &txn_error, const json &parent_attributions_json = json());
};

class DirUsageUpdate : public Lot {
  public:
	int m_depth;
	std::string m_current_path;
	std::string m_parent_prefix;

	int64_t m_query_time;

	DirUsageUpdate() : m_depth{0}, m_current_path{}, m_parent_prefix{}, m_query_time{0} {}
	DirUsageUpdate(int depth, std::string path, int64_t query_time)
		: m_depth{depth}, m_current_path{}, m_parent_prefix{path}, m_query_time{query_time} {}

	std::pair<bool, std::string> JSON_math(json update_JSON, std::vector<Lot> *return_lots) {
		std::map<std::string, double> size_updates;
		std::map<std::string, int64_t> obj_updates;
		std::map<std::string, double> size_writing_updates;
		std::map<std::string, int64_t> obj_writing_updates;

		for (const auto &update : update_JSON) {

			double usage_GB;
			int64_t num_obj;
			double GB_being_written;
			int64_t objects_being_written;
			if (update.contains("size_GB")) {
				usage_GB = update["size_GB"].get<double>();
			}
			if (update.contains("num_obj")) {
				num_obj = update["num_obj"].get<int64_t>();
			}

			if (update.contains("GB_being_written")) {
				GB_being_written = update["GB_being_written"].get<double>();
			}
			if (update.contains("objects_being_written")) {
				objects_being_written = update["objects_being_written"].get<int64_t>();
			}

			std::string path = update["path"];

			if (path.substr(0, 1) != "/") { // get rid of preceding extra slash
				m_current_path = m_parent_prefix + "/" + path;
			} else {
				m_current_path = m_parent_prefix + path;
			}

			// Figure out which lot to associate with the dir
			auto rp_vec_str = get_lots_from_dir(m_current_path, false, m_query_time);
			if (!rp_vec_str.second.empty()) { // There was an error
				std::string int_err = rp_vec_str.second;
				std::string ext_err = "Failure on call to get_lots_from_dir: ";
				return std::make_pair(false, ext_err + int_err);
			}
			if (rp_vec_str.first.empty()) {
				return std::make_pair(false, "get_lots_from_dir returned empty result");
			}

			Lot lot(rp_vec_str.first[0]);
			lot.init_self_usage();

			// Need a way to check if the path is stored as recursive
			auto rp_json_str = lot.get_lot_dirs(
				false); // Probably don't need all the dirs unless the plan is to use this more than once...

			if (!rp_json_str.second.empty()) { // There was an error
				std::string int_err = rp_json_str.second;
				std::string ext_err = "Failure on call to get_lots_from_dir: ";
				return std::make_pair(false, ext_err + int_err);
			}

			json lot_dirs_json = rp_json_str.first;
			bool recursive = false;
			// Normalize path with trailing slash to match stored format
			std::string normalized_current_path = ensure_trailing_slash(m_current_path);
			// lot_dirs_json is an array of path objects, search for matching path
			for (const auto &path_obj : lot_dirs_json) {
				if (path_obj["path"].get<std::string>() == normalized_current_path) {
					recursive = path_obj["recursive"].get<bool>();
					break;
				}
			}

			// Handle subdirs when includes_subdirs is true.
			// We need to process subdirs in two cases:
			// 1. The lot's path is not recursive - standard behavior, process each subdir
			// 2. The lot's path IS recursive BUT some subdirs might be excluded - we need to
			//    check each subdir and attribute excluded subdirs to their proper lot
			//
			// To handle case 2, we check if any subdir maps to a different lot than the parent.
			// If so, we subtract that subdir's usage from the parent and process it separately.
			if (update["includes_subdirs"].get<bool>() && !update["subdirs"].empty()) {
				// Helper to subtract subdir values from parent totals
				auto subtract_subdir_values = [&](const json &subdir) {
					if (subdir.contains("size_GB")) {
						usage_GB -= subdir["size_GB"].get<double>();
					}
					if (subdir.contains("num_obj")) {
						num_obj -= subdir["num_obj"].get<int64_t>();
					}
					if (subdir.contains("GB_being_written")) {
						GB_being_written -= subdir["GB_being_written"].get<double>();
					}
					if (subdir.contains("objects_being_written")) {
						objects_being_written -= subdir["objects_being_written"].get<int64_t>();
					}
				};

				// Separate subdirs into those belonging to other lots (due to exclusions)
				json other_lot_subdirs = json::array();

				for (const auto &subdir : update["subdirs"]) {
					std::string subdir_path;
					if (subdir["path"].get<std::string>().substr(0, 1) != "/") {
						subdir_path = m_current_path + "/" + subdir["path"].get<std::string>();
					} else {
						subdir_path = m_current_path + subdir["path"].get<std::string>();
					}

					auto subdir_lot_rp = get_lots_from_dir(subdir_path, false, m_query_time);
					if (subdir_lot_rp.second.empty() && !subdir_lot_rp.first.empty() &&
						subdir_lot_rp.first[0] != lot.lot_name) {
						// Subdir belongs to a different lot (e.g., due to exclusion)
						other_lot_subdirs.push_back(subdir);
					}
					// If we can't determine the lot or it's the same lot, no special handling needed
				}

				// For non-recursive paths, always subtract ALL subdirs and process them
				// For recursive paths, only subtract subdirs that belong to OTHER lots
				if (!recursive) {
					// Non-recursive: subtract all subdirs from parent, process all subdirs
					for (const auto &subdir : update["subdirs"]) {
						subtract_subdir_values(subdir);
					}
					DirUsageUpdate dirUpdate(m_depth + 1, m_current_path, m_query_time);
					dirUpdate.JSON_math(update["subdirs"], return_lots);
				} else if (!other_lot_subdirs.empty()) {
					// Recursive path but some subdirs are excluded - subtract only those
					for (const auto &subdir : other_lot_subdirs) {
						subtract_subdir_values(subdir);
					}
					// Process only the subdirs that belong to other lots
					DirUsageUpdate dirUpdate(m_depth + 1, m_current_path, m_query_time);
					dirUpdate.JSON_math(other_lot_subdirs, return_lots);
				}
				// If recursive and all subdirs belong to same lot, do nothing extra -
				// the full usage_GB already includes them correctly
			}

			std::vector<std::string> lot_update_keys;
			// For each of the updates contained in the update json, start staging usage updates on per-lot basis
			if (update.contains("size_GB")) {
				lot.usage.self_GB += usage_GB;
				lot.usage.self_GB_update_staged = true;
			}
			if (update.contains("num_obj")) {
				lot.usage.self_objects += num_obj;
				lot.usage.self_objects_update_staged = true;
			}
			if (update.contains("GB_being_written")) {
				lot.usage.self_GB_being_written += GB_being_written;
				lot.usage.self_GB_being_written_update_staged = true;
			}
			if (update.contains("objects_being_written")) {
				lot.usage.self_objects_being_written += objects_being_written;
				lot.usage.self_objects_being_written_update_staged = true;
			}

			// Get a list of the lot names currently in return_lots
			// the vector of names acts as a proxy for the lots in return_lots
			std::vector<std::string> return_lot_names;
			for (auto &return_lot : *return_lots) {
				return_lot_names.push_back(return_lot.lot_name);
			}

			// If the current lot is not in return_lots, add it
			if (std::find(return_lot_names.begin(), return_lot_names.end(), lot.lot_name) == return_lot_names.end()) {
				return_lots->push_back(lot);
			} else {
				for (auto &return_lot : *return_lots) {
					if (return_lot.lot_name ==
						lot.lot_name) { // They should be treated as same obj, so add their values
						return_lot.usage.self_GB += lot.usage.self_GB;
						return_lot.usage.self_objects += lot.usage.self_objects;
						return_lot.usage.self_GB_being_written += lot.usage.self_GB_being_written;
						return_lot.usage.self_objects_being_written += lot.usage.self_objects_being_written;
					}
				}
			}
		}

		return std::make_pair(true, "");
	}
};

// NOTE: Context is NOT thread-safe. All static members (m_caller, m_home,
// m_strict_hierarchy, m_contraction_policy, m_admin_override) are accessed
// without synchronization. Callers must ensure Context is configured from
// a single thread before invoking LotMan operations. This is a pre-existing
// design constraint — the PooledConnection pool handles DB-level concurrency,
// but Context state must be set up front.
class Context {
  public:
	Context() {}
	static void set_caller(const std::string caller);
	static std::pair<bool, std::string> set_lot_home(const std::string lot_home);

	static std::string get_caller() {
		return *m_caller;
	}
	static std::string get_lot_home() {
		return *m_home;
	}

	// Strict hierarchy enforcement: when true, lot creation/updates must satisfy
	// Axioms 1-3 (child MPAs within parent bounds, aggregate children within parent,
	// timestamp containment)
	static void set_strict_hierarchy(bool enabled);
	static bool get_strict_hierarchy() {
		return m_strict_hierarchy;
	}

	// Contraction policy: controls whether lot MPA reductions (and deletions) are allowed.
	// "none" (default): no restrictions on MPA reductions
	// "alive": block reductions on lots where creation_time <= now <= expiration_time
	// "always": block any MPA reduction regardless of time
	static std::pair<bool, std::string> set_contraction_policy(const std::string &policy);
	static std::string get_contraction_policy() {
		return m_contraction_policy;
	}

	// Admin override: when true, bypasses contraction policy checks.
	// The calling library is responsible for verifying the caller is a root lot owner
	// before setting this.
	static void set_admin_override(bool enabled);
	static bool get_admin_override() {
		return m_admin_override;
	}

  private:
	static std::shared_ptr<std::string> m_caller;
	static std::shared_ptr<std::string> m_home;
	static bool m_strict_hierarchy;
	static std::string m_contraction_policy;
	static bool m_admin_override;

	static std::vector<std::string> path_split(const std::string dir_path);
	static std::pair<bool, std::string> mkdir_and_parents_if_needed(const std::string dir_path);
};

/**
 * The checks class has member functions that check if functions in the Lot class should execute.
 * In this way, they check the validity of Lot functions executing with a specific set of input parameters
 * Eg, cycle_check() makes sure that a soon-to-be-added lot doesn't create any cyclic dependencies
 */

class Checks {
	friend class lotman::Lot;

  public:
	static bool
	cycle_check(const std::string &start_node, const std::vector<std::string> &start_parents,
				const std::vector<std::string> &start_children); // Only checks for cycles that return to start_node.
																 // Returns true if cycle found, false otherwise
	static bool insertion_check(const std::string &LTBA, const std::string &parent,
								const std::string &child); // Check if lot-to-be-added (LTBA) is being inserted between
														   // a parent/child, which should update data for the child
	static bool will_be_orphaned(const std::string &LTBR, const std::string &child);
};
} // namespace lotman
