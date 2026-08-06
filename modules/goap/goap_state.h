/**************************************************************************/
/*  goap_state.h                                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/math_defs.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Internal representation of the symbolic world: an unordered set of concrete
// key/value facts. The scripting API speaks `Dictionary`, which is converted to
// these structs once at the boundary of a planning query so that the search can
// hash and compare states without touching Variant dictionaries in its inner
// loop.
//
// None of this is exposed to ClassDB on purpose; they are plain structs, not
// Objects.
struct GoapState {
	HashMap<StringName, Variant> facts;

	// Value equality used for every exact precondition/effect comparison. Falls
	// back to the Variant operator when the types differ so that facts written
	// as `1` from one script and `1.0` from another still match.
	static bool values_match(const Variant &p_a, const Variant &p_b);

	// Reads a fact as a number. `null` counts as 0 so that counters do not have
	// to be seeded in the world state before an action increments them.
	static bool value_as_number(const Variant &p_value, double &r_number);

	// Hash that agrees with `values_match`: numerically equal values of
	// different types must not hash differently, or the planner's visited set
	// would miss duplicate states.
	static uint32_t hash_value(const Variant &p_value);

	bool is_empty() const { return facts.is_empty(); }
	int size() const { return facts.size(); }
	bool has(const StringName &p_key) const { return facts.has(p_key); }
	const Variant *getptr(const StringName &p_key) const { return facts.getptr(p_key); }
	void set_fact(const StringName &p_key, const Variant &p_value) { facts[p_key] = p_value; }
	void clear() { facts.clear(); }

	void from_dictionary(const Dictionary &p_dictionary);
	Dictionary to_dictionary() const;
};

// One requirement on a single fact.
//
// A plain Variant keeps its historical meaning of exact equality. A string of
// the form ">=3", ">3", "<=3", "<3" or "==3" is parsed into a numeric interval
// instead, which is what lets the planner reason about resources rather than
// flags. Anything else, including a string that merely starts with an operator
// but is not followed by a number, is taken literally.
struct GoapCondition {
	enum Kind {
		KIND_EXACT, // The fact must equal `exact`.
		KIND_RANGE, // The fact must be a number inside the interval.
	};

	Kind kind = KIND_EXACT;
	Variant exact;
	double min = -Math::INF;
	double max = Math::INF;
	bool min_exclusive = false;
	bool max_exclusive = false;

	static GoapCondition from_value(const Variant &p_value);
	// True when the value looks like a comparison but does not parse as one, so
	// that a typo can be reported instead of silently becoming an unmatchable
	// string literal.
	static bool is_malformed(const Variant &p_value);

	bool is_range() const { return kind == KIND_RANGE; }

	// True when the fact meets the requirement. `p_value` is null for a fact
	// that is absent from the world state: exact conditions treat that as
	// unsatisfied, range conditions read it as 0.
	bool accepts(const Variant *p_value) const;

	// +1 when the fact has to grow to meet the requirement, -1 when it has to
	// shrink, 0 when it already does or when no numeric change could help.
	int direction_from(const Variant *p_value) const;

	// Fewest changes of magnitude `p_step` that could close the gap. Returns 0
	// when the requirement already holds, and 1 when `p_step` is infinite (some
	// action assigns the fact outright).
	int steps_from(const Variant *p_value, double p_step) const;

	// Rewrites the requirement into the one that had to hold *before* an effect
	// added `p_offset` to the fact. Fails on a non-numeric exact condition,
	// which no delta could ever have produced.
	bool shift(double p_offset);

	// Narrows this condition so that it also implies `p_other`. Returns false
	// when the two cannot hold at the same time.
	bool intersect(const GoapCondition &p_other);

	uint32_t hash() const;
	bool operator==(const GoapCondition &p_other) const;
};

// What an action does to a single fact: either assign a value outright, or add
// a signed amount to it. Deltas are written as "+=1" / "-=2".
struct GoapEffect {
	bool is_delta = false;
	Variant value; // Assigned value when `is_delta` is false.
	double delta = 0.0;

	static GoapEffect from_value(const Variant &p_value);
	static bool is_malformed(const Variant &p_value);

	// The value the fact takes once the effect has run. `p_current` is null for
	// a fact that is absent; a delta then starts from 0.
	Variant apply(const Variant *p_current) const;
};

// An unordered set of requirements, used both for an action's preconditions and
// for the conditions the planner still has to close.
struct GoapConditions {
	HashMap<StringName, GoapCondition> conditions;

	bool is_empty() const { return conditions.is_empty(); }
	int size() const { return conditions.size(); }
	bool has(const StringName &p_key) const { return conditions.has(p_key); }
	const GoapCondition *getptr(const StringName &p_key) const { return conditions.getptr(p_key); }
	void set_condition(const StringName &p_key, const GoapCondition &p_condition) { conditions[p_key] = p_condition; }
	void clear() { conditions.clear(); }

	void from_dictionary(const Dictionary &p_dictionary);

	// True when every requirement holds in `p_state`.
	bool satisfied_by(const GoapState &p_state) const;
	// Number of requirements `p_state` does not currently meet.
	int count_unsatisfied(const GoapState &p_state) const;
	// Narrows every requirement so that `p_other` also holds. Returns false
	// when the two sets contradict each other.
	bool intersect(const GoapConditions &p_other);

	uint32_t hash() const;
	bool operator==(const GoapConditions &p_other) const;
	bool operator!=(const GoapConditions &p_other) const { return !(*this == p_other); }
};

// An unordered set of effects.
struct GoapEffects {
	HashMap<StringName, GoapEffect> effects;

	bool is_empty() const { return effects.is_empty(); }
	int size() const { return effects.size(); }
	bool has(const StringName &p_key) const { return effects.has(p_key); }
	const GoapEffect *getptr(const StringName &p_key) const { return effects.getptr(p_key); }

	void from_dictionary(const Dictionary &p_dictionary);
	Dictionary apply_to(const Dictionary &p_world_state) const;
};
