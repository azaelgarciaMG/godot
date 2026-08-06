/**************************************************************************/
/*  goap_action.h                                                         */
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

#include "goap_context.h"

#include "core/io/resource.h"

// A single step a GoapAgent can take. Actions are pure data as far as the
// planner is concerned: a set of preconditions that must hold before the action
// can run and a set of effects the planner assumes will hold afterwards.
//
// Scripts extend this to add procedural checks (`_is_valid`, `_get_cost`) and
// the actual behavior (`_enter`, `_tick`, `_exit`).
class GoapAction : public Resource {
	GDCLASS(GoapAction, Resource);

public:
	enum Status {
		STATUS_RUNNING,
		STATUS_SUCCEEDED,
		STATUS_FAILED,
	};

private:
	StringName action_name;
	double cost = 1.0;
	Dictionary preconditions;
	Dictionary effects;
	bool auto_apply_effects = true;

protected:
	static void _bind_methods();

	// `_tick` returns an `int` rather than `Status` because the enum is not
	// registered with the Variant type system until after this class body.
	GDVIRTUAL1RC(bool, _is_valid, Ref<GoapContext>);
	GDVIRTUAL1RC(double, _get_cost, Ref<GoapContext>);
	GDVIRTUAL1RC(Dictionary, _get_preconditions, Ref<GoapContext>);
	GDVIRTUAL1RC(Dictionary, _get_effects, Ref<GoapContext>);
	GDVIRTUAL1(_enter, Ref<GoapContext>);
	GDVIRTUAL2R(int, _tick, Ref<GoapContext>, double);
	GDVIRTUAL2(_exit, Ref<GoapContext>, bool);

public:
	void set_action_name(const StringName &p_name);
	StringName get_action_name() const { return action_name; }

	void set_cost(double p_cost);
	double get_cost() const { return cost; }

	void set_preconditions(const Dictionary &p_preconditions);
	Dictionary get_preconditions() const { return preconditions; }

	void set_effects(const Dictionary &p_effects);
	Dictionary get_effects() const { return effects; }

	void set_auto_apply_effects(bool p_enabled);
	bool is_auto_apply_effects() const { return auto_apply_effects; }

	// Planning interface. Each of these consults the matching virtual method
	// first and falls back to the exported property.
	bool is_valid(const Ref<GoapContext> &p_context) const;
	double get_effective_cost(const Ref<GoapContext> &p_context) const;
	Dictionary get_effective_preconditions(const Ref<GoapContext> &p_context) const;
	Dictionary get_effective_effects(const Ref<GoapContext> &p_context) const;

	// True when the resource computes these from script instead of exporting
	// them, in which case static validation cannot inspect them.
	bool has_dynamic_preconditions() const;
	bool has_dynamic_effects() const;

	// Convenience for callers that only have a plain world state dictionary.
	bool check_preconditions(const Dictionary &p_world_state, const Ref<GoapContext> &p_context) const;
	Dictionary apply_effects(const Dictionary &p_world_state, const Ref<GoapContext> &p_context) const;

	// Execution interface, driven by GoapAgent.
	void enter(const Ref<GoapContext> &p_context);
	Status tick(const Ref<GoapContext> &p_context, double p_delta);
	void exit(const Ref<GoapContext> &p_context, bool p_succeeded);
};

VARIANT_ENUM_CAST(GoapAction::Status);
