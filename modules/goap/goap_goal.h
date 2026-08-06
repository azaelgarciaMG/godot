/**************************************************************************/
/*  goap_goal.h                                                           */
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

// A world state the agent would like to bring about. The planner picks the
// highest priority goal it can actually build a plan for, so goals can be
// listed in a fixed order and re-prioritized at runtime through `_get_priority`.
class GoapGoal : public Resource {
	GDCLASS(GoapGoal, Resource);

	StringName goal_name;
	double priority = 1.0;
	Dictionary desired_state;

protected:
	static void _bind_methods();

	GDVIRTUAL1RC(bool, _is_valid, Ref<GoapContext>);
	GDVIRTUAL1RC(double, _get_priority, Ref<GoapContext>);
	GDVIRTUAL1RC(Dictionary, _get_desired_state, Ref<GoapContext>);

public:
	void set_goal_name(const StringName &p_name);
	StringName get_goal_name() const { return goal_name; }

	void set_priority(double p_priority);
	double get_priority() const { return priority; }

	void set_desired_state(const Dictionary &p_desired_state);
	Dictionary get_desired_state() const { return desired_state; }

	// Each of these consults the matching virtual method first and falls back
	// to the exported property.
	bool is_valid(const Ref<GoapContext> &p_context) const;
	double get_effective_priority(const Ref<GoapContext> &p_context) const;
	Dictionary get_effective_desired_state(const Ref<GoapContext> &p_context) const;

	// True when the desired state is computed from script instead of exported,
	// in which case static validation cannot inspect it.
	bool has_dynamic_desired_state() const;

	bool is_satisfied(const Dictionary &p_world_state, const Ref<GoapContext> &p_context) const;
};
