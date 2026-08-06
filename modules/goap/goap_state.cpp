/**************************************************************************/
/*  goap_state.cpp                                                        */
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

#include "goap_state.h"

#include "core/math/math_funcs.h"
#include "core/templates/hashfuncs.h"

namespace {

// Splits a dictionary value into its leading operator and the rest. Returns an
// empty operator when the value is not a string or does not start with one.
String extract_operator(const Variant &p_value, bool p_delta) {
	if (p_value.get_type() != Variant::STRING && p_value.get_type() != Variant::STRING_NAME) {
		return String();
	}
	const String text = p_value;
	if (p_delta) {
		if (text.begins_with("+=") || text.begins_with("-=")) {
			return text.substr(0, 2);
		}
		return String();
	}
	// Two-character operators come first so that ">=" is not read as ">".
	if (text.begins_with(">=") || text.begins_with("<=") || text.begins_with("==")) {
		return text.substr(0, 2);
	}
	if (text.begins_with(">") || text.begins_with("<")) {
		return text.substr(0, 1);
	}
	return String();
}

// True when the value carries an operator that is not followed by a number.
bool has_broken_operator(const Variant &p_value, bool p_delta) {
	const String op = extract_operator(p_value, p_delta);
	if (op.is_empty()) {
		return false;
	}
	const String operand = String(p_value).substr(op.length()).strip_edges();
	return !operand.is_valid_float();
}

} // namespace

bool GoapState::values_match(const Variant &p_a, const Variant &p_b) {
	if (p_a.get_type() == p_b.get_type()) {
		return p_a == p_b;
	}

	// Mismatched types: let the Variant operator decide, so that comparable
	// types such as `int` and `float` still compare equal.
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_EQUAL, p_a, p_b, result, valid);
	return valid && result.operator bool();
}

bool GoapState::value_as_number(const Variant &p_value, double &r_number) {
	switch (p_value.get_type()) {
		case Variant::NIL: {
			r_number = 0.0;
			return true;
		}
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT: {
			r_number = p_value;
			return true;
		}
		default: {
			return false;
		}
	}
}

uint32_t GoapState::hash_value(const Variant &p_value) {
	// `values_match` compares `1`, `1.0` and `true` as equal, so they have to
	// hash the same. `null` is left alone: it matches nothing but itself.
	double number = 0.0;
	if (p_value.get_type() != Variant::NIL && value_as_number(p_value, number)) {
		return hash_murmur3_one_double(number);
	}
	return p_value.hash();
}

void GoapState::from_dictionary(const Dictionary &p_dictionary) {
	facts.clear();
	facts.reserve(p_dictionary.size());
	for (const KeyValue<Variant, Variant> &E : p_dictionary) {
		facts[E.key] = E.value;
	}
}

Dictionary GoapState::to_dictionary() const {
	Dictionary dictionary;
	for (const KeyValue<StringName, Variant> &E : facts) {
		dictionary[E.key] = E.value;
	}
	return dictionary;
}

GoapCondition GoapCondition::from_value(const Variant &p_value) {
	GoapCondition condition;

	const String op = extract_operator(p_value, false);
	if (op.is_empty()) {
		condition.exact = p_value;
		return condition;
	}

	const String operand = String(p_value).substr(op.length()).strip_edges();
	if (!operand.is_valid_float()) {
		// Not a comparison after all; the string is a fact value of its own.
		condition.exact = p_value;
		return condition;
	}

	const double number = operand.to_float();
	if (op == "==") {
		condition.exact = number;
		return condition;
	}

	condition.kind = KIND_RANGE;
	if (op == ">=" || op == ">") {
		condition.min = number;
		condition.min_exclusive = op == ">";
	} else {
		condition.max = number;
		condition.max_exclusive = op == "<";
	}
	return condition;
}

bool GoapCondition::is_malformed(const Variant &p_value) {
	return has_broken_operator(p_value, false);
}

bool GoapCondition::accepts(const Variant *p_value) const {
	if (kind == KIND_EXACT) {
		return p_value != nullptr && GoapState::values_match(*p_value, exact);
	}

	double number = 0.0;
	if (p_value != nullptr && !GoapState::value_as_number(*p_value, number)) {
		// A fact that is not a number can never sit inside an interval.
		return false;
	}
	if (number < min || (min_exclusive && number == min)) {
		return false;
	}
	if (number > max || (max_exclusive && number == max)) {
		return false;
	}
	return true;
}

int GoapCondition::direction_from(const Variant *p_value) const {
	double current = 0.0;
	if (p_value != nullptr && !GoapState::value_as_number(*p_value, current)) {
		return 0;
	}

	if (kind == KIND_EXACT) {
		double target = 0.0;
		if (!GoapState::value_as_number(exact, target)) {
			// Nothing numeric to move towards; only an assignment can help.
			return 0;
		}
		if (current < target) {
			return 1;
		}
		return current > target ? -1 : 0;
	}

	if (current < min || (min_exclusive && current == min)) {
		return 1;
	}
	if (current > max || (max_exclusive && current == max)) {
		return -1;
	}
	return 0;
}

int GoapCondition::steps_from(const Variant *p_value, double p_step) const {
	if (accepts(p_value)) {
		return 0;
	}
	if (p_step <= 0.0 || Math::is_inf(p_step)) {
		// Either no delta can move this fact, or a single action assigns it
		// outright. One action is the lower bound either way.
		return 1;
	}

	double current = 0.0;
	if (p_value != nullptr && !GoapState::value_as_number(*p_value, current)) {
		return 1;
	}

	double gap = 0.0;
	if (kind == KIND_EXACT) {
		double target = 0.0;
		if (!GoapState::value_as_number(exact, target)) {
			return 1;
		}
		gap = Math::abs(target - current);
	} else if (current < min) {
		gap = min - current;
	} else if (current > max) {
		gap = current - max;
	}

	// A gap of zero here means an exclusive bound is grazed, which still costs
	// at least one action.
	return MAX(1, (int)Math::ceil(gap / p_step));
}

bool GoapCondition::shift(double p_offset) {
	if (kind == KIND_EXACT) {
		double target = 0.0;
		if (!GoapState::value_as_number(exact, target)) {
			return false;
		}
		exact = target - p_offset;
		return true;
	}

	if (!Math::is_inf(min)) {
		min -= p_offset;
	}
	if (!Math::is_inf(max)) {
		max -= p_offset;
	}
	return true;
}

bool GoapCondition::intersect(const GoapCondition &p_other) {
	if (kind == KIND_EXACT && p_other.kind == KIND_EXACT) {
		return GoapState::values_match(exact, p_other.exact);
	}
	if (kind == KIND_EXACT) {
		// An exact value is the stronger of the two; keep it if it fits.
		return p_other.accepts(&exact);
	}
	if (p_other.kind == KIND_EXACT) {
		if (!accepts(&p_other.exact)) {
			return false;
		}
		*this = p_other;
		return true;
	}

	// Two intervals: keep the tighter bound on each side.
	if (p_other.min > min || (p_other.min == min && p_other.min_exclusive)) {
		min = p_other.min;
		min_exclusive = p_other.min_exclusive;
	}
	if (p_other.max < max || (p_other.max == max && p_other.max_exclusive)) {
		max = p_other.max;
		max_exclusive = p_other.max_exclusive;
	}
	if (min > max) {
		return false;
	}
	return !(min == max && (min_exclusive || max_exclusive));
}

uint32_t GoapCondition::hash() const {
	if (kind == KIND_EXACT) {
		return hash_murmur3_one_32(1, GoapState::hash_value(exact));
	}
	uint32_t h = hash_murmur3_one_double(min, 2);
	h = hash_murmur3_one_double(max, h);
	return hash_murmur3_one_32((uint32_t)min_exclusive | ((uint32_t)max_exclusive << 1), h);
}

bool GoapCondition::operator==(const GoapCondition &p_other) const {
	if (kind != p_other.kind) {
		return false;
	}
	if (kind == KIND_EXACT) {
		return GoapState::values_match(exact, p_other.exact);
	}
	return min == p_other.min && max == p_other.max &&
			min_exclusive == p_other.min_exclusive && max_exclusive == p_other.max_exclusive;
}

GoapEffect GoapEffect::from_value(const Variant &p_value) {
	GoapEffect effect;

	const String op = extract_operator(p_value, true);
	if (op.is_empty()) {
		effect.value = p_value;
		return effect;
	}

	const String operand = String(p_value).substr(op.length()).strip_edges();
	if (!operand.is_valid_float()) {
		effect.value = p_value;
		return effect;
	}

	effect.is_delta = true;
	effect.delta = op == "-=" ? -operand.to_float() : operand.to_float();
	effect.value = effect.delta;
	return effect;
}

bool GoapEffect::is_malformed(const Variant &p_value) {
	return has_broken_operator(p_value, true);
}

Variant GoapEffect::apply(const Variant *p_current) const {
	if (!is_delta) {
		return value;
	}

	double current = 0.0;
	if (p_current != nullptr) {
		// A fact that is not a number restarts from 0 rather than poisoning the
		// world state with an error value.
		GoapState::value_as_number(*p_current, current);
	}
	const double result = current + delta;

	// Keep integer counters integral so that they still match exact conditions
	// written as `3` rather than `3.0`.
	const bool current_is_int = p_current == nullptr || p_current->get_type() == Variant::INT || p_current->get_type() == Variant::NIL;
	if (current_is_int && result == Math::floor(result)) {
		return (int64_t)result;
	}
	return result;
}

void GoapConditions::from_dictionary(const Dictionary &p_dictionary) {
	conditions.clear();
	conditions.reserve(p_dictionary.size());
	for (const KeyValue<Variant, Variant> &E : p_dictionary) {
		conditions[E.key] = GoapCondition::from_value(E.value);
	}
}

bool GoapConditions::satisfied_by(const GoapState &p_state) const {
	for (const KeyValue<StringName, GoapCondition> &E : conditions) {
		if (!E.value.accepts(p_state.getptr(E.key))) {
			return false;
		}
	}
	return true;
}

int GoapConditions::count_unsatisfied(const GoapState &p_state) const {
	int unsatisfied = 0;
	for (const KeyValue<StringName, GoapCondition> &E : conditions) {
		if (!E.value.accepts(p_state.getptr(E.key))) {
			unsatisfied++;
		}
	}
	return unsatisfied;
}

bool GoapConditions::intersect(const GoapConditions &p_other) {
	for (const KeyValue<StringName, GoapCondition> &E : p_other.conditions) {
		GoapCondition *existing = conditions.getptr(E.key);
		if (existing == nullptr) {
			conditions[E.key] = E.value;
			continue;
		}
		if (!existing->intersect(E.value)) {
			return false;
		}
	}
	return true;
}

uint32_t GoapConditions::hash() const {
	// Conditions are unordered, so the per-entry hashes are combined with an
	// operation that does not depend on iteration order.
	uint32_t h = 0;
	for (const KeyValue<StringName, GoapCondition> &E : conditions) {
		h += hash_murmur3_one_32(E.key.hash(), E.value.hash());
	}
	return hash_fmix32(h ^ (uint32_t)conditions.size());
}

bool GoapConditions::operator==(const GoapConditions &p_other) const {
	if (conditions.size() != p_other.conditions.size()) {
		return false;
	}
	for (const KeyValue<StringName, GoapCondition> &E : conditions) {
		const GoapCondition *other = p_other.conditions.getptr(E.key);
		if (other == nullptr || !(E.value == *other)) {
			return false;
		}
	}
	return true;
}

void GoapEffects::from_dictionary(const Dictionary &p_dictionary) {
	effects.clear();
	effects.reserve(p_dictionary.size());
	for (const KeyValue<Variant, Variant> &E : p_dictionary) {
		effects[E.key] = GoapEffect::from_value(E.value);
	}
}

Dictionary GoapEffects::apply_to(const Dictionary &p_world_state) const {
	Dictionary result = p_world_state.duplicate();
	for (const KeyValue<StringName, GoapEffect> &E : effects) {
		const Variant key = E.key;
		const Variant *current = p_world_state.getptr(key);
		result[key] = E.value.apply(current);
	}
	return result;
}
