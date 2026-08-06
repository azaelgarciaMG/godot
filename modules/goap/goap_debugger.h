/**************************************************************************/
/*  goap_debugger.h                                                       */
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

#include "core/debugger/engine_profiler.h"
#include "core/object/object_id.h"
#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"

class GoapAgent;

// Runtime half of the GOAP debugger. Every GoapAgent announces itself while it
// is in the tree, and once the editor turns the "goap" profiler on, a snapshot
// of all of them is pushed across the debugger connection a few times a second.
//
// Nothing here runs unless the editor asks for it: with the profiler off the
// per-frame tick only accumulates a timer.
class GoapDebugger {
public:
	// One agent as the editor panel sees it.
	struct AgentInfo {
		String path;
		bool active = false;
		bool idle = false;
		bool planning = false;
		String goal;
		PackedStringArray plan;
		int cursor = 0;
		double plan_cost = 0.0;
		int last_result = 0;
		int last_iterations = 0;
		Dictionary world_state;

		void write_to_array(Array &r_arr) const;
		bool read_from_array(const Array &p_arr, int p_offset);

		// Number of array entries one agent occupies.
		static constexpr int FIELD_COUNT = 11;
	};

	struct AgentFrame {
		Vector<AgentInfo> infos;

		Array serialize() const;
		bool deserialize(const Array &p_array);
	};

private:
	class AgentProfiler : public EngineProfiler {
		GDSOFTCLASS(AgentProfiler, EngineProfiler);

		bool enabled = false;
		double interval = 0.25;
		double elapsed = 0.0;

	public:
		void toggle(bool p_enable, const Array &p_opts) override;
		void tick(double p_frame_time, double p_process_time, double p_physics_time, double p_physics_frame_time) override;
	};

	static HashSet<ObjectID> agents;

public:
	static void initialize();
	static void deinitialize();

	// Called by GoapAgent as it enters and leaves the tree. Cheap enough to run
	// unconditionally; the set is only read when a snapshot is requested.
	static void register_agent(GoapAgent *p_agent);
	static void unregister_agent(GoapAgent *p_agent);

	// Collects every live agent and sends it to the editor.
	static void send_snapshot();
};
