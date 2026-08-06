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

#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// Internal representation of a symbolic state: an unordered set of key/value
// facts. The scripting API speaks `Dictionary`, which is converted to this once
// at the boundary of a planning query so that the search can hash and compare
// states without touching Variant dictionaries in its inner loop.
//
// Not exposed to ClassDB on purpose; it is a plain struct, not an Object.
struct GoapState {
	HashMap<StringName, Variant> facts;

	// Value equality used for every precondition/effect comparison. Falls back
	// to the Variant operator when the types differ so that facts written as
	// `1` from one script and `1.0` from another still match.
	static bool values_match(const Variant &p_a, const Variant &p_b);

	bool is_empty() const { return facts.is_empty(); }
	int size() const { return facts.size(); }
	bool has(const StringName &p_key) const { return facts.has(p_key); }
	void set_fact(const StringName &p_key, const Variant &p_value) { facts[p_key] = p_value; }
	void clear() { facts.clear(); }

	// True when every fact in `p_conditions` is present here with a matching value.
	bool satisfies(const GoapState &p_conditions) const;
	// Number of facts in `p_conditions` this state does not currently match.
	// Used as the planner heuristic.
	int count_unsatisfied(const GoapState &p_conditions) const;
	// True when both states share a key but disagree on its value.
	bool conflicts_with(const GoapState &p_other) const;

	void from_dictionary(const Dictionary &p_dictionary);
	Dictionary to_dictionary() const;

	uint32_t hash() const;
	bool operator==(const GoapState &p_other) const;
	bool operator!=(const GoapState &p_other) const { return !(*this == p_other); }
};
