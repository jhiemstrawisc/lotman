#include <nlohmann/json-schema.hpp>

using json = nlohmann::json;
using json_validator = nlohmann::json_schema::json_validator;

namespace lotman_schemas {

// Shared schema fragment for parent_attributions, used by new_lot_schema,
// lot_update_schema, and lot_additions_schema.
const json parent_attributions_def = R"({
    "description": "How the child's MPAs are attributed across its parents. Keys are parent lot names.",
    "type": "object",
    "additionalProperties": {
        "type": "object",
        "additionalProperties": false,
        "properties": {
            "dedicated_GB": {
                "description": "Dedicated GB attributed from this parent. -1 propagates the unbounded sentinel from the parent on the dedicated axis.",
                "type": "number",
                "minimum": -1
            },
            "opportunistic_GB": {
                "description": "Opportunistic GB attributed from this parent. -1 propagates the unbounded sentinel from the parent on the opportunistic axis.",
                "type": "number",
                "minimum": -1
            },
            "max_num_objects": {
                "description": "Object count attributed from this parent. -1 propagates the unbounded sentinel from the parent on the object axis.",
                "type": "number",
                "minimum": -1,
                "multipleOf": 1
            }
        }
    }
})"_json;

inline json new_lot_schema = []() {
	json s = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "new lot obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Lot Name",
            "type": "string",
            "minLength": 1
        },
        "owner": {
            "description": "Entity who owns the lot",
            "type": "string",
            "minLength": 1
        },
        "parents": {
            "description": "The names of parent lots",
            "type": "array",
            "items": {
                "type": "string"
            },
            "minItems":1
        },
        "children": {
            "description": "The names of children lots",
            "type": "array",
            "items": {
                "type": "string"
            }
        },
        "paths": {
            "description": "paths array",
            "type": "array",
            "items": {
                "type": "object",
                "description": "path object",
                "additionalProperties": false,
                "properties": {
                    "path": {
                        "type": "string",
                        "minLength": 1
                    },
                    "recursive": {
                        "type": "boolean"
                    },
                    "exclude": {
                        "type": "boolean",
                        "description": "Escape hatch: when true, this path row is ignored by both path-to-lot resolution and the descendancy/temporal overlap check. Prefer the dynamic-ownership model (a sublot whose path is a strict subpath of an ancestor's recursive path automatically claims its subtree) over explicit exclusions."
                    }
                },
                "required": ["path", "recursive"]
            }
        },
        "management_policy_attrs": {
            "description": "management policy attributes",
            "type": "object",
            "additionalProperties": false,
            "properties": {
                "dedicated_GB": {
                    "description": "Amount of dedicated storage attributed to the lot, in GB. -1 marks the dedicated axis as unbounded; in that case opportunistic_GB must also be -1.",
                    "type": "number",
                    "minimum": -1
                },
                "opportunistic_GB": {
                    "description": "Amount of opportunistic storage attributed to the lot, in GB. -1 marks the opportunistic axis as unbounded.",
                    "type": "number",
                    "minimum": -1
                },
                "max_num_objects": {
                    "description": "Max number of objects a lot should have. -1 marks the object axis as unbounded.",
                    "type": "number",
                    "minimum": -1,
                    "multipleOf": 1
                },
                "creation_time": {
                    "description": "Creation time of the lot, in ms since Unix epoch",
                    "type": "number",
                    "minimum": 0,
                    "multipleOf": 1
                },
                "expiration_time": {
                    "description": "Expiration time of the lot, in ms since Unix epoch",
                    "type": "number",
                    "minimum": 0,
                    "multipleOf": 1
                },
                "deletion_time": {
                    "description": "Deletion time of the lot, in ms since Unix epoch",
                    "type": "number",
                    "minimum": 0,
                    "multipleOf": 1
                }
            },
            "required": ["dedicated_GB", "opportunistic_GB", "max_num_objects", "creation_time", "expiration_time", "deletion_time"]
        }
    },
    "required": ["lot_name", "owner", "parents", "management_policy_attrs"]
}
)"_json;
	s["properties"]["parent_attributions"] = parent_attributions_def;
	return s;
}();

inline json lot_update_schema = []() {
	json s = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "update obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot being modified",
            "type": "string",
            "minLength": 1
        },

        "owner": {
            "description": "New entity to whom the lot should belong",
            "type": "string",
            "minLength": 1
        },

        "parents": {
            "description": "Array of parent updates",
            "type": "array",
            "items": {
                "type": "object",
                "description": "parent update object",
                "additionalProperties": false,
                "properties": {
                    "current": {
                        "type": "string",
                        "minLength": 1
                    },
                    "new": {
                        "type": "string",
                        "minLength": 1
                    }
                },
                "required": ["current", "new"]
            }
        },

        "paths": {
            "description": "Array of path updates",
            "type": "array",
            "items": {
                "type": "object",
                "description": "path update object",
                "additionalProperties": false,
                "properties": {
                    "current": {
                        "description": "path to be updated",
                        "type": "string",
                        "minLength": 1
                    },
                    "new": {
                        "description": "new path",
                        "type": "string",
                        "minLength": 1
                    },
                    "recursive": {
                        "description": "whether new path is recursive",
                        "type": "boolean"
                    },
                    "exclude": {
                        "description": "whether new path is an exclusion",
                        "type": "boolean"
                    }
                },
                "required": ["current", "new", "recursive"]
            }
        },

        "management_policy_attrs": {
            "description": "Management policy attribute updates",
            "type": "object",
            "additionalProperties": false,
            "properties": {
                "dedicated_GB": {
                    "description": "Amount of dedicated storage attributed to the lot, in GB. -1 marks the dedicated axis as unbounded; in that case opportunistic_GB must also be -1.",
                    "type": "number",
                    "minimum": -1
                },
                "opportunistic_GB": {
                    "description": "Amount of opportunistic storage attributed to the lot, in GB. -1 marks the opportunistic axis as unbounded.",
                    "type": "number",
                    "minimum": -1
                },
                "max_num_objects": {
                    "description": "Max number of objects a lot should have. -1 marks the object axis as unbounded.",
                    "type": "number",
                    "minimum": -1,
                    "multipleOf": 1
                },
                "creation_time": {
                    "description": "Creation time of the lot, in ms since Unix epoch. A value of 0 (with expiration_time and deletion_time also 0) marks the lot as non-expiring.",
                    "type": "number",
                    "minimum": 0,
                    "multipleOf": 1
                },
                "expiration_time": {
                    "description": "Expiration time of the lot, in ms since Unix epoch",
                    "type": "number",
                    "minimum": 0,
                    "multipleOf": 1
                },
                "deletion_time": {
                    "description": "Deletion time of the lot, in ms since Unix epoch",
                    "type": "number",
                    "minimum": 0,
                    "multipleOf": 1
                }
            }
        }
    },
    "required": ["lot_name"]
}
)"_json;
	// `parent_attributions` is accepted in the same envelope as the rest of the
	// update payload (see lotman_update_lot). Wholesale-replace semantics: any
	// parent omitted from this object gets the equal-split remainder of the
	// child's MPAs.
	s["properties"]["parent_attributions"] = parent_attributions_def;
	return s;
}();

inline json lot_rm_parents_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "subtraction obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot being modified",
            "type": "string",
            "minLength": 1
        },

        "parents": {
            "description": "The names of parent lots to be removed",
            "type": "array",
            "items": {
                "type": "string"
            },
            "minItems":1
        }
    },
    "required": ["lot_name", "parents"]
}
)"_json;

inline json lot_rm_paths_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "subtraction obj",
    "additionalProperties": false,
    "properties": {
        "paths": {
            "description": "The paths to be removed from the lot",
            "type": "array",
            "items": {
                "type": "string"
            },
            "minItems":1
        }
    },
    "required": ["paths"]
}
)"_json;

inline json lot_additions_schema = []() {
	json s = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "additions obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot being modified",
            "type": "string",
            "minLength": 1
        },

        "paths": {
            "description": "New paths array",
            "type": "array",
            "items": {
                "description": "new path obj",
                "type": "object",
                "additionalProperties": false,
                "properties": {
                    "path": {
                        "type": "string",
                        "minLength": 1
                    },
                    "recursive": {
                        "type": "boolean"
                    },
                    "exclude": {
                        "type": "boolean",
                        "description": "Escape hatch: when true, this path row is ignored by both path-to-lot resolution and the descendancy/temporal overlap check. Prefer the dynamic-ownership model (a sublot whose path is a strict subpath of an ancestor's recursive path automatically claims its subtree) over explicit exclusions."
                    }
                },
                "required": ["path", "recursive"]
            }
        },

        "parents": {
            "description": "Array of parents to assign to lot",
            "type": "array",
            "items": {
                "description": "Names of new parent lots",
                "type": "string",
                "minLength": 1
            }
        }
    },
    "required": ["lot_name"]
}
)"_json;
	// Allow callers to specify per-parent shares for the post-addition lot;
	// any parent omitted from this object gets the equal-split remainder.
	s["properties"]["parent_attributions"] = parent_attributions_def;
	return s;
}();

inline json get_policy_attrs_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "get policy attrs obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot whose attrs are to be obtained",
            "type": "string",
            "minLength": 1
        },

        "dedicated_GB": {
            "description": "true -> get most restrictive attr, false -> get attr recorded for lot",
            "type": "boolean"
        },

        "opportunistic_GB": {
            "description": "true -> get most restrictive attr, false -> get attr recorded for lot",
            "type": "boolean"
        },

        "max_num_objects": {
            "description": "true -> get most restrictive attr, false -> get attr recorded for lot",
            "type": "boolean"
        },

        "creation_time": {
            "description": "true -> get most restrictive attr, false -> get attr recorded for lot",
            "type": "boolean"
        },

        "expiration_time": {
            "description": "true -> get most restrictive attr, false -> get attr recorded for lot",
            "type": "boolean"
        },

        "deletion_time": {
            "description": "true -> get most restrictive attr, false -> get attr recorded for lot",
            "type": "boolean"
        }
    },
    "required": ["lot_name"]
}
)"_json;

inline json get_usage_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "get usage obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot whose attrs are to be obtained",
            "type": "string",
            "minLength": 1
        },

        "dedicated_GB": {
            "description": "true -> count children toward usage, false -> don't",
            "type": "boolean"
        },

        "opportunistic_GB": {
            "description": "true -> count children toward usage, false -> don't",
            "type": "boolean"
        },

        "total_GB": {
            "description": "true -> count children toward usage, false -> don't",
            "type": "boolean"
        },

        "num_objects": {
            "description": "true -> count children toward usage, false -> don't",
            "type": "boolean"
        },

        "GB_being_written": {
            "description": "true -> count children toward usage, false -> don't",
            "type": "boolean"
        },

        "objects_being_written": {
            "description": "true -> count children toward usage, false -> don't",
            "type": "boolean"
        }
    },
    "required": ["lot_name"]
}
)"_json;

inline json update_usage_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "update usage obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot whose attrs are to be obtained",
            "type": "string",
            "minLength": 1
        },
        "self_GB": {
            "description": "The number of GB a lot is using, not including children usage.",
            "type": "number",
            "minimum": 0
        },
        "self_objects": {
            "description": "The number of objects attributed to a lot, not including children.",
            "type": "number",
            "minimum": 0,
            "multipleOf": 1
        },
        "self_GB_being_written": {
            "description": "GB currently being written to a lot, not including children.",
            "type": "number",
            "minimum": 0
        },
        "self_objects_being_written": {
            "description": "The number of objects being written to a lot, not including children.",
            "type": "number",
            "minimum": 0,
            "multipleOf": 1
        }
    },
    "required": ["lot_name"]
}
)"_json;

inline json update_usage_delta_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "update usage obj",
    "additionalProperties": false,
    "properties": {
        "lot_name": {
            "description": "Name of lot whose attrs are to be obtained",
            "type": "string",
            "minLength": 1
        },
        "self_GB": {
            "description": "The number of GB a lot is using, not including children usage.",
            "type": "number"
        },
        "self_objects": {
            "description": "The number of objects attributed to a lot, not including children.",
            "type": "number",
            "multipleOf": 1
        },
        "self_GB_being_written": {
            "description": "GB currently being written to a lot, not including children.",
            "type": "number"
        },
        "self_objects_being_written": {
            "description": "The number of objects being written to a lot, not including children.",
            "type": "number",
            "multipleOf": 1
        }
    },
    "required": ["lot_name"]
}
)"_json;
/*
NOTE: This schema only validates a single top level object, but does get the array of objects
	  after "subdirs". To validate the top array of these objects, iterate through the array
	  and test against each object.

TODO: Fix this so it validates the top level array instead of validating each individualobject
	  in the array
*/
inline json update_usage_by_dir_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "update usage by dir obj",
    "additionalProperties": false,
    "properties": {
        "path": {
            "description": "The path/object name.",
            "type": "string",
            "minLength": 1
        },
        "size_GB": {
            "description": "The size of the resource.",
            "type": "number",
            "minimum": 0
        },
        "num_obj": {
            "description": "The number of objects, if the resource itself is not an object.",
            "type": "number",
            "minimum": 0,
            "multipleOf": 1
        },
        "includes_subdirs": {
            "description": "Whether or not any subdirs are counted toward the size_GB or num_obj values.",
            "type": "boolean"
        },
        "subdirs": {
            "description": "JSON of any of the subdirs, required if include_subdirs is true.",
            "type": "array",
            "items": {
                "$ref": "#"
            }
        }
    },
    "required": ["path", "includes_subdirs"]
}
)"_json;

inline json update_usage_by_dir_delta_schema = R"(
{
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "title": "update usage by dir obj",
    "additionalProperties": false,
    "properties": {
        "path": {
            "description": "The path/object name.",
            "type": "string",
            "minLength": 1
        },
        "size_GB": {
            "description": "The size of the resource.",
            "type": "number"
        },
        "num_obj": {
            "description": "The number of objects, if the resource itself is not an object.",
            "type": "number",
            "multipleOf": 1
        },
        "includes_subdirs": {
            "description": "Whether or not any subdirs are counted toward the size_GB or num_obj values.",
            "type": "boolean"
        },
        "subdirs": {
            "description": "JSON of any of the subdirs, required if include_subdirs is true.",
            "type": "array",
            "items": {
                "$ref": "#"
            }
        }
    },
    "required": ["path", "includes_subdirs"]
}
)"_json;

} // namespace lotman_schemas
