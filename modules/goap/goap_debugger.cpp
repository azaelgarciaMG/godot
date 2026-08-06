/**************************************************************************/
/*  goap_debugger.cpp                                                     */
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

#include "goap_debugger.h"

#include "goap_agent.h"

#include "core/debugger/engine_debugger.h"
#include "core/object/object.h"

HashSet<ObjectID> GoapDebugger::agents;

namespace {

List<Ref<EngineProfiler>> goap_profilers;

} // namespace

void GoapDebugger::AgentInfo::write_to_array(Array &r_arr) const {
	r_arr.push_back(path);
	r_arr.push_back(active);
	r_arr.push_back(idle);
	r_arr.push_back(planning);
	r_arr.push_back(goal);
	r_arr.push_back(plan);
	r_arr.push_back(cursor);
	r_arr.push_back(plan_cost);
	r_arr.push_back(last_result);
	r_arr.push_back(last_iterations);
	r_arr.push_back(world_state);
}

bool GoapDebugger::AgentInfo::read_from_array(const Array &p_arr, int p_offset) {
	ERR_FAIL_COND_V(p_offset + FIELD_COUNT > p_arr.size(), false);
	path = p_arr[p_offset + 0];
	active = p_arr[p_offset + 1];
	idle = p_arr[p_offset + 2];
	planning = p_arr[p_offset + 3];
	goal = p_arr[p_offset + 4];
	plan = p_arr[p_offset + 5];
	cursor = p_arr[p_offset + 6];
	plan_cost = p_arr[p_offset + 7];
	last_result = p_arr[p_offset + 8];
	last_iterations = p_arr[p_offset + 9];
	world_state = p_arr[p_offset + 10];
	return true;
}

Array GoapDebugger::AgentFrame::serialize() const {
	Array arr;
	arr.push_back(infos.size());
	for (const AgentInfo &info : infos) {
		info.write_to_array(arr);
	}
	return arr;
}

bool GoapDebugger::AgentFrame::deserialize(const Array &p_array) {
	ERR_FAIL_COND_V(p_array.is_empty(), false);
	const int count = p_array[0];
	ERR_FAIL_COND_V(count < 0, false);
	ERR_FAIL_COND_V(p_array.size() != 1 + count * AgentInfo::FIELD_COUNT, false);

	infos.clear();
	infos.resize(count);
	for (int i = 0; i < count; i++) {
		if (!infos.write[i].read_from_array(p_array, 1 + i * AgentInfo::FIELD_COUNT)) {
			return false;
		}
	}
	return true;
}

void GoapDebugger::AgentProfiler::toggle(bool p_enable, const Array &p_opts) {
	enabled = p_enable;
	elapsed = 0.0;
	if (enabled) {
		// Answer the moment the panel is opened rather than after a full period.
		GoapDebugger::send_snapshot();
	}
}

void GoapDebugger::AgentProfiler::tick(double p_frame_time, double p_process_time, double p_physics_time, double p_physics_frame_time) {
	if (!enabled) {
		return;
	}
	elapsed += p_frame_time;
	if (elapsed < interval) {
		return;
	}
	elapsed = 0.0;
	GoapDebugger::send_snapshot();
}

void GoapDebugger::initialize() {
	Ref<AgentProfiler> profiler;
	profiler.instantiate();
	profiler->bind("goap");
	goap_profilers.push_back(profiler);
}

void GoapDebugger::deinitialize() {
	goap_profilers.clear();
	agents.clear();
}

void GoapDebugger::register_agent(GoapAgent *p_agent) {
	ERR_FAIL_NULL(p_agent);
	agents.insert(p_agent->get_instance_id());
}

void GoapDebugger::unregister_agent(GoapAgent *p_agent) {
	ERR_FAIL_NULL(p_agent);
	agents.erase(p_agent->get_instance_id());
}

void GoapDebugger::send_snapshot() {
	if (!EngineDebugger::is_active()) {
		return;
	}

	AgentFrame frame;
	LocalVector<ObjectID> stale;

	for (const ObjectID &id : agents) {
		GoapAgent *agent = ObjectDB::get_instance<GoapAgent>(id);
		if (agent == nullptr) {
			stale.push_back(id);
			continue;
		}
		if (!agent->is_inside_tree()) {
			// Its path would be meaningless, and it is not being stepped.
			continue;
		}

		AgentInfo info;
		info.path = String(agent->get_path());
		info.active = agent->is_active();
		info.idle = agent->is_idle();
		info.last_result = (int)agent->get_last_plan_result();
		info.world_state = agent->get_world_state();

		Ref<GoapPlanner> planner = agent->get_planner();
		if (planner.is_valid()) {
			info.planning = planner->is_planning();
			info.last_iterations = planner->get_last_iterations();
		}

		Ref<GoapGoal> goal = agent->get_current_goal();
		if (goal.is_valid()) {
			info.goal = goal->get_goal_name();
		}

		Ref<GoapPlan> plan = agent->get_plan();
		if (plan.is_valid()) {
			info.cursor = plan->get_cursor();
			info.plan_cost = plan->get_total_cost();
			const Vector<Ref<GoapAction>> &steps = plan->get_action_list();
			for (const Ref<GoapAction> &action : steps) {
				info.plan.push_back(action.is_valid() ? String(action->get_action_name()) : String("<null>"));
			}
		}

		frame.infos.push_back(info);
	}

	// Agents freed without leaving the tree first would otherwise pile up.
	for (const ObjectID &id : stale) {
		agents.erase(id);
	}

	EngineDebugger::get_singleton()->send_message("goap:agents", frame.serialize());
}
