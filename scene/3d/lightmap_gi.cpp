/**************************************************************************/
/*  lightmap_gi.cpp                                                       */
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

#include "lightmap_gi.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/delaunay_3d.h"
#include "core/math/geometry_3d.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/lightmap_probe.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/sky.h"
#include "servers/rendering/rendering_server.h"

#include "modules/modules_enabled.gen.h" // IWYU pragma: keep. For lightmapper_rd.

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
#include "servers/display/display_server.h"
#endif

void LightmapGIData::add_user(const NodePath &p_path, const Rect2 &p_uv_scale, int p_slice_index, int32_t p_sub_instance) {
	User user;
	user.path = p_path;
	user.uv_scale = p_uv_scale;
	user.slice_index = p_slice_index;
	user.sub_instance = p_sub_instance;
	users.push_back(user);
}

int LightmapGIData::get_user_count() const {
	return users.size();
}

NodePath LightmapGIData::get_user_path(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), NodePath());
	return users[p_user].path;
}

int32_t LightmapGIData::get_user_sub_instance(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].sub_instance;
}

Rect2 LightmapGIData::get_user_lightmap_uv_scale(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), Rect2());
	return users[p_user].uv_scale;
}

int LightmapGIData::get_user_lightmap_slice_index(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].slice_index;
}

void LightmapGIData::clear_users() {
	users.clear();
}

void LightmapGIData::_set_user_data(const Array &p_data) {
	ERR_FAIL_COND((p_data.size() % 4) != 0);
	users.clear();
	for (int i = 0; i < p_data.size(); i += 4) {
		add_user(p_data[i + 0], p_data[i + 1], p_data[i + 2], p_data[i + 3]);
	}
}

Array LightmapGIData::_get_user_data() const {
	Array ret;
	for (int i = 0; i < users.size(); i++) {
		ret.push_back(users[i].path);
		ret.push_back(users[i].uv_scale);
		ret.push_back(users[i].slice_index);
		ret.push_back(users[i].sub_instance);
	}
	return ret;
}

PackedStringArray LightmapGIData::_collect_texture_paths(const TypedArray<TextureLayered> &p_textures) {
	PackedStringArray paths;
	for (int i = 0; i < p_textures.size(); i++) {
		Ref<TextureLayered> texture = p_textures[i];
		if (texture.is_null()) {
			return PackedStringArray();
		}
		const String path = texture->get_path();
		// A texture that only lives in memory can't be reloaded later, and a texture
		// built into another file (an "::" sub-resource) is kept alive by its owner,
		// so releasing it would not return any video memory. A single such entry
		// makes the whole set non-streamable.
		if (path.is_empty() || path.contains("::")) {
			return PackedStringArray();
		}
		paths.push_back(path);
	}
	return paths;
}

Ref<TextureLayered> LightmapGIData::combine_textures(const TypedArray<TextureLayered> &p_textures) {
	if (p_textures.is_empty()) {
		return Ref<TextureLayered>();
	}

	if (p_textures.size() == 1) {
		return p_textures[0];
	}

	// A bake whose atlas exceeds Image::MAX_HEIGHT is stored as several textures and
	// has to be merged into the one array the renderer samples.
	Vector<Ref<Image>> images;
	for (int i = 0; i < p_textures.size(); i++) {
		Ref<TextureLayered> texture = p_textures[i];
		ERR_FAIL_COND_V_MSG(texture.is_null(), Ref<TextureLayered>(), vformat("Invalid TextureLayered at index %d.", i));
		for (int j = 0; j < texture->get_layers(); j++) {
			images.push_back(texture->get_layer_data(j));
		}
	}

	Ref<Texture2DArray> combined_texture;
	combined_texture.instantiate();
	combined_texture->create_from_images(images);
	return combined_texture;
}

void LightmapGIData::set_lightmap_textures(const TypedArray<TextureLayered> &p_data) {
	storage_light_textures = p_data;
	light_texture_paths = _collect_texture_paths(p_data);
	textures_resident = true;
	combined_light_texture = combine_textures(p_data);
	_reset_lightmap_textures();
}

TypedArray<TextureLayered> LightmapGIData::get_lightmap_textures() const {
	return storage_light_textures;
}

void LightmapGIData::set_shadowmask_textures(const TypedArray<TextureLayered> &p_data) {
	storage_shadowmask_textures = p_data;
	shadowmask_texture_paths = _collect_texture_paths(p_data);
	combined_shadowmask_texture = combine_textures(p_data);
	_reset_shadowmask_textures();
}

TypedArray<TextureLayered> LightmapGIData::get_shadowmask_textures() const {
	return storage_shadowmask_textures;
}

void LightmapGIData::clear_shadowmask_textures() {
	RS::get_singleton()->lightmap_set_shadowmask_textures(lightmap, RID());
	storage_shadowmask_textures.clear();
	shadowmask_texture_paths.clear();
	combined_shadowmask_texture.unref();
}

bool LightmapGIData::has_shadowmask_textures() {
	// The paths alone are enough to answer this while the atlases are released,
	// which is the state a streaming lightmap spends most of its time in.
	if (!shadowmask_texture_paths.is_empty()) {
		return true;
	}
	return !storage_shadowmask_textures.is_empty() && combined_shadowmask_texture.is_valid();
}

void LightmapGIData::_set_light_texture_paths(const PackedStringArray &p_paths) {
	light_texture_paths = p_paths;
	// A resource saved for streaming carries paths but no atlases, so residency has
	// to follow what is actually loaded rather than the default of "everything is".
	textures_resident = !storage_light_textures.is_empty();
}

void LightmapGIData::_set_shadowmask_texture_paths(const PackedStringArray &p_paths) {
	shadowmask_texture_paths = p_paths;
}

void LightmapGIData::_validate_property(PropertyInfo &p_property) const {
	// Only honored when the paths can actually stand in for the atlases. Dropping
	// the atlases without a usable path to reload them from would destroy the bake
	// on the next save.
	const bool store_paths_instead = streaming && !light_texture_paths.is_empty();

	if (p_property.name == "lightmap_textures" || p_property.name == "shadowmask_textures") {
		if (store_paths_instead) {
			// Serializing the atlases here is what makes loading this resource pull
			// them into video memory. The paths below stand in for them instead.
			p_property.usage &= ~PROPERTY_USAGE_STORAGE;
		}
	} else if (p_property.name == "light_texture_paths" || p_property.name == "shadowmask_texture_paths") {
		if (!store_paths_instead) {
			// Without streaming the paths are recovered from the atlases themselves.
			p_property.usage &= ~PROPERTY_USAGE_STORAGE;
		}
	}
}

void LightmapGIData::set_streaming_enabled(bool p_enabled) {
	if (streaming == p_enabled) {
		return;
	}
	streaming = p_enabled;
	notify_property_list_changed();
}

bool LightmapGIData::is_streaming_enabled() const {
	return streaming;
}

bool LightmapGIData::are_textures_resident() const {
	return textures_resident;
}

bool LightmapGIData::is_streamable() const {
	return !light_texture_paths.is_empty();
}

PackedStringArray LightmapGIData::get_light_texture_paths() const {
	return light_texture_paths;
}

PackedStringArray LightmapGIData::get_shadowmask_texture_paths() const {
	return shadowmask_texture_paths;
}

void LightmapGIData::release_textures() {
	if (!textures_resident) {
		return;
	}
	ERR_FAIL_COND_MSG(light_texture_paths.is_empty(), "Cannot release the textures of a LightmapGIData whose textures have no resource path, they could not be reloaded afterwards.");

	// A texture that outlives its release keeps its video memory, which silently
	// defeats streaming, so under --verbose the object identities are kept and
	// checked once the references are gone.
	const bool verbose = OS::get_singleton()->is_stdout_verbose();
	LocalVector<ObjectID> watched_ids;
	if (verbose) {
		for (int i = 0; i < storage_light_textures.size(); i++) {
			Ref<TextureLayered> texture = storage_light_textures[i];
			if (texture.is_valid()) {
				watched_ids.push_back(texture->get_instance_id());
			}
		}
	}

	// Order matters. The renderer keeps the light texture RID in a global slot array
	// (LightStorage::lightmap_textures) and nothing notifies it when a texture is
	// freed, so the slot has to be released before the last reference dies.
	RS::get_singleton()->lightmap_set_textures(lightmap, RID(), uses_spherical_harmonics);
	RS::get_singleton()->lightmap_set_shadowmask_textures(lightmap, RID());

	combined_light_texture.unref();
	combined_shadowmask_texture.unref();
	// Detached rather than cleared in place: an Array shares its storage with
	// whoever passed it in, so clearing would empty their copy as well.
	storage_light_textures = TypedArray<TextureLayered>();
	storage_shadowmask_textures = TypedArray<TextureLayered>();

	textures_resident = false;

	// Checked by object identity rather than through ResourceCache, which does not
	// track every way a texture can be loaded.
	for (const ObjectID &id : watched_ids) {
		const Resource *survivor = Object::cast_to<Resource>(ObjectDB::get_instance(id));
		if (survivor != nullptr) {
			WARN_PRINT(vformat("Lightmap texture \"%s\" outlived its release and is still held by %d other reference(s). Its video memory will not be freed.",
					survivor->get_path(), survivor->get_reference_count()));
		}
	}
}

void LightmapGIData::restore_textures(const TypedArray<TextureLayered> &p_light_textures, const Ref<TextureLayered> &p_combined_light, const TypedArray<TextureLayered> &p_shadowmask_textures, const Ref<TextureLayered> &p_combined_shadowmask) {
	ERR_FAIL_COND(p_combined_light.is_null());

	storage_light_textures = p_light_textures;
	combined_light_texture = p_combined_light;
	storage_shadowmask_textures = p_shadowmask_textures;
	combined_shadowmask_texture = p_combined_shadowmask;
	textures_resident = true;

	// The paths are deliberately left untouched: they still describe this data and
	// are what a later release/restore cycle reloads from.
	_reset_lightmap_textures();
	_reset_shadowmask_textures();
}

RID LightmapGIData::get_rid() const {
	return lightmap;
}

void LightmapGIData::clear() {
	users.clear();
}

void LightmapGIData::_reset_lightmap_textures() {
	RS::get_singleton()->lightmap_set_textures(lightmap, combined_light_texture.is_valid() ? combined_light_texture->get_rid() : RID(), uses_spherical_harmonics);
}

void LightmapGIData::_reset_shadowmask_textures() {
	RS::get_singleton()->lightmap_set_shadowmask_textures(lightmap, combined_shadowmask_texture.is_valid() ? combined_shadowmask_texture->get_rid() : RID());
}

void LightmapGIData::set_uses_spherical_harmonics(bool p_enable) {
	uses_spherical_harmonics = p_enable;
	_reset_lightmap_textures();
}

bool LightmapGIData::is_using_spherical_harmonics() const {
	return uses_spherical_harmonics;
}

void LightmapGIData::_set_uses_packed_directional(bool p_enable) {
	_uses_packed_directional = p_enable;
}

bool LightmapGIData::_is_using_packed_directional() const {
	return _uses_packed_directional;
}

void LightmapGIData::update_shadowmask_mode(ShadowmaskMode p_mode) {
	RS::get_singleton()->lightmap_set_shadowmask_mode(lightmap, (RSE::ShadowmaskMode)p_mode);
}

LightmapGIData::ShadowmaskMode LightmapGIData::get_shadowmask_mode() const {
	return (ShadowmaskMode)RS::get_singleton()->lightmap_get_shadowmask_mode(lightmap);
}

void LightmapGIData::set_capture_data(const AABB &p_bounds, bool p_interior, const PackedVector3Array &p_points, const PackedColorArray &p_point_sh, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree, float p_baked_exposure, uint32_t p_lightprobe_hash) {
	if (p_points.size()) {
		int pc = p_points.size();
		ERR_FAIL_COND(pc * 9 != p_point_sh.size());
		ERR_FAIL_COND((p_tetrahedra.size() % 4) != 0);
		ERR_FAIL_COND((p_bsp_tree.size() % 6) != 0);
		RS::get_singleton()->lightmap_set_probe_capture_data(lightmap, p_points, p_point_sh, p_tetrahedra, p_bsp_tree);
		RS::get_singleton()->lightmap_set_probe_bounds(lightmap, p_bounds);
		RS::get_singleton()->lightmap_set_probe_interior(lightmap, p_interior);
	} else {
		RS::get_singleton()->lightmap_set_probe_capture_data(lightmap, PackedVector3Array(), PackedColorArray(), PackedInt32Array(), PackedInt32Array());
		RS::get_singleton()->lightmap_set_probe_bounds(lightmap, AABB());
		RS::get_singleton()->lightmap_set_probe_interior(lightmap, false);
	}
	RS::get_singleton()->lightmap_set_baked_exposure_normalization(lightmap, p_baked_exposure);
	baked_exposure = p_baked_exposure;
	lightprobe_hash = p_lightprobe_hash;
	interior = p_interior;
	bounds = p_bounds;
}

PackedVector3Array LightmapGIData::get_capture_points() const {
	return RS::get_singleton()->lightmap_get_probe_capture_points(lightmap);
}

PackedColorArray LightmapGIData::get_capture_sh() const {
	return RS::get_singleton()->lightmap_get_probe_capture_sh(lightmap);
}

PackedInt32Array LightmapGIData::get_capture_tetrahedra() const {
	return RS::get_singleton()->lightmap_get_probe_capture_tetrahedra(lightmap);
}

PackedInt32Array LightmapGIData::get_capture_bsp_tree() const {
	return RS::get_singleton()->lightmap_get_probe_capture_bsp_tree(lightmap);
}

uint32_t LightmapGIData::get_lightprobe_hash() const {
	return lightprobe_hash;
}

AABB LightmapGIData::get_capture_bounds() const {
	return bounds;
}

bool LightmapGIData::is_interior() const {
	return interior;
}

float LightmapGIData::get_baked_exposure() const {
	return baked_exposure;
}

void LightmapGIData::_set_probe_data(const Dictionary &p_data) {
	ERR_FAIL_COND(!p_data.has("bounds"));
	ERR_FAIL_COND(!p_data.has("points"));
	ERR_FAIL_COND(!p_data.has("tetrahedra"));
	ERR_FAIL_COND(!p_data.has("bsp"));
	ERR_FAIL_COND(!p_data.has("sh"));
	ERR_FAIL_COND(!p_data.has("interior"));
	ERR_FAIL_COND(!p_data.has("baked_exposure"));

	uint32_t phash = 0;
	if (p_data.has("lightprobe_hash")) { // Older versions will not have it.
		phash = p_data["lightprobe_hash"];
	}
	set_capture_data(p_data["bounds"], p_data["interior"], p_data["points"], p_data["sh"], p_data["tetrahedra"], p_data["bsp"], p_data["baked_exposure"], phash);
}

Dictionary LightmapGIData::_get_probe_data() const {
	Dictionary d;
	d["bounds"] = get_capture_bounds();
	d["points"] = get_capture_points();
	d["tetrahedra"] = get_capture_tetrahedra();
	d["bsp"] = get_capture_bsp_tree();
	d["sh"] = get_capture_sh();
	d["interior"] = is_interior();
	d["baked_exposure"] = get_baked_exposure();
	d["lightprobe_hash"] = lightprobe_hash;
	return d;
}

#ifndef DISABLE_DEPRECATED
void LightmapGIData::set_light_texture(const Ref<TextureLayered> &p_light_texture) {
	TypedArray<TextureLayered> arr = { p_light_texture };
	set_lightmap_textures(arr);
}

Ref<TextureLayered> LightmapGIData::get_light_texture() const {
	if (storage_light_textures.is_empty()) {
		return Ref<TextureLayered>();
	}
	return storage_light_textures.get(0);
}

void LightmapGIData::_set_light_textures_data(const Array &p_data) {
	set_lightmap_textures(p_data);
}

Array LightmapGIData::_get_light_textures_data() const {
	return Array(storage_light_textures);
}
#endif

void LightmapGIData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_user_data", "data"), &LightmapGIData::_set_user_data);
	ClassDB::bind_method(D_METHOD("_get_user_data"), &LightmapGIData::_get_user_data);

	ClassDB::bind_method(D_METHOD("set_lightmap_textures", "light_textures"), &LightmapGIData::set_lightmap_textures);
	ClassDB::bind_method(D_METHOD("get_lightmap_textures"), &LightmapGIData::get_lightmap_textures);

	ClassDB::bind_method(D_METHOD("set_shadowmask_textures", "shadowmask_textures"), &LightmapGIData::set_shadowmask_textures);
	ClassDB::bind_method(D_METHOD("get_shadowmask_textures"), &LightmapGIData::get_shadowmask_textures);

	ClassDB::bind_method(D_METHOD("set_streaming_enabled", "enabled"), &LightmapGIData::set_streaming_enabled);
	ClassDB::bind_method(D_METHOD("is_streaming_enabled"), &LightmapGIData::is_streaming_enabled);

	ClassDB::bind_method(D_METHOD("are_textures_resident"), &LightmapGIData::are_textures_resident);
	ClassDB::bind_method(D_METHOD("is_streamable"), &LightmapGIData::is_streamable);
	ClassDB::bind_method(D_METHOD("get_light_texture_paths"), &LightmapGIData::get_light_texture_paths);
	ClassDB::bind_method(D_METHOD("get_shadowmask_texture_paths"), &LightmapGIData::get_shadowmask_texture_paths);

	ClassDB::bind_method(D_METHOD("_set_light_texture_paths", "paths"), &LightmapGIData::_set_light_texture_paths);
	ClassDB::bind_method(D_METHOD("_set_shadowmask_texture_paths", "paths"), &LightmapGIData::_set_shadowmask_texture_paths);

	ClassDB::bind_method(D_METHOD("set_uses_spherical_harmonics", "uses_spherical_harmonics"), &LightmapGIData::set_uses_spherical_harmonics);
	ClassDB::bind_method(D_METHOD("is_using_spherical_harmonics"), &LightmapGIData::is_using_spherical_harmonics);

	ClassDB::bind_method(D_METHOD("_set_uses_packed_directional", "_uses_packed_directional"), &LightmapGIData::_set_uses_packed_directional);
	ClassDB::bind_method(D_METHOD("_is_using_packed_directional"), &LightmapGIData::_is_using_packed_directional);

	ClassDB::bind_method(D_METHOD("add_user", "path", "uv_scale", "slice_index", "sub_instance"), &LightmapGIData::add_user);
	ClassDB::bind_method(D_METHOD("get_user_count"), &LightmapGIData::get_user_count);
	ClassDB::bind_method(D_METHOD("get_user_path", "user_idx"), &LightmapGIData::get_user_path);
	ClassDB::bind_method(D_METHOD("clear_users"), &LightmapGIData::clear_users);

	ClassDB::bind_method(D_METHOD("_set_probe_data", "data"), &LightmapGIData::_set_probe_data);
	ClassDB::bind_method(D_METHOD("_get_probe_data"), &LightmapGIData::_get_probe_data);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "lightmap_textures", PROPERTY_HINT_ARRAY_TYPE, "TextureLayered", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "set_lightmap_textures", "get_lightmap_textures");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "shadowmask_textures", PROPERTY_HINT_ARRAY_TYPE, "TextureLayered", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "set_shadowmask_textures", "get_shadowmask_textures");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "set_streaming_enabled", "is_streaming_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "light_texture_paths", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_light_texture_paths", "get_light_texture_paths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "shadowmask_texture_paths", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_shadowmask_texture_paths", "get_shadowmask_texture_paths");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "uses_spherical_harmonics", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "set_uses_spherical_harmonics", "is_using_spherical_harmonics");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "user_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_user_data", "_get_user_data");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "probe_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_probe_data", "_get_probe_data");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "_uses_packed_directional", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_uses_packed_directional", "_is_using_packed_directional");

#ifndef DISABLE_DEPRECATED
	ClassDB::bind_method(D_METHOD("set_light_texture", "light_texture"), &LightmapGIData::set_light_texture);
	ClassDB::bind_method(D_METHOD("get_light_texture"), &LightmapGIData::get_light_texture);

	ClassDB::bind_method(D_METHOD("_set_light_textures_data", "data"), &LightmapGIData::_set_light_textures_data);
	ClassDB::bind_method(D_METHOD("_get_light_textures_data"), &LightmapGIData::_get_light_textures_data);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "light_texture", PROPERTY_HINT_RESOURCE_TYPE, TextureLayered::get_class_static(), PROPERTY_USAGE_NONE), "set_light_texture", "get_light_texture");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "light_textures", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_INTERNAL), "_set_light_textures_data", "_get_light_textures_data");
#endif

	BIND_ENUM_CONSTANT(SHADOWMASK_MODE_NONE);
	BIND_ENUM_CONSTANT(SHADOWMASK_MODE_REPLACE);
	BIND_ENUM_CONSTANT(SHADOWMASK_MODE_OVERLAY);
}

LightmapGIData::LightmapGIData() {
	lightmap = RS::get_singleton()->lightmap_create();
}

LightmapGIData::~LightmapGIData() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(lightmap);
}

///////////////////////////

void LightmapGI::_find_meshes_and_lights(Node *p_at_node, Vector<MeshesFound> &meshes, Vector<LightsFound> &lights, Vector<Vector3> &probes) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			bool all_have_uv2_and_normal = true;
			bool surfaces_found = false;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				if (mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
					continue;
				}
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_TEX_UV2)) {
					all_have_uv2_and_normal = false;
					break;
				}
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_NORMAL)) {
					all_have_uv2_and_normal = false;
					break;
				}
				surfaces_found = true;
			}

			if (surfaces_found && all_have_uv2_and_normal) {
				//READY TO BAKE! size hint could be computed if not found, actually..

				MeshesFound mf;
				mf.xform = get_global_transform().affine_inverse() * mi->get_global_transform();
				mf.node_path = get_path_to(mi);
				mf.subindex = -1;
				mf.mesh = mesh;
				mf.lightmap_scale = mi->get_lightmap_texel_scale();

				Ref<Material> all_override = mi->get_material_override();
				for (int i = 0; i < mesh->get_surface_count(); i++) {
					if (all_override.is_valid()) {
						mf.overrides.push_back(all_override);
					} else {
						mf.overrides.push_back(mi->get_surface_override_material(i));
					}
				}

				meshes.push_back(mf);
			}
		}
	}

	Node3D *s = Object::cast_to<Node3D>(p_at_node);

	if (!mi && s) {
		Array bmeshes = p_at_node->call("get_bake_meshes");
		if (bmeshes.size() && (bmeshes.size() & 1) == 0) {
			Transform3D xf = get_global_transform().affine_inverse() * s->get_global_transform();
			for (int i = 0; i < bmeshes.size(); i += 2) {
				Ref<Mesh> mesh = bmeshes[i];
				if (mesh.is_null()) {
					continue;
				}

				MeshesFound mf;

				Transform3D mesh_xf = bmeshes[i + 1];
				mf.xform = xf * mesh_xf;
				mf.node_path = get_path_to(s);
				mf.subindex = i / 2;
				mf.lightmap_scale = 1.0;
				mf.mesh = mesh;

				meshes.push_back(mf);
			}
		}
	}

	Light3D *light = Object::cast_to<Light3D>(p_at_node);

	if (light && light->get_bake_mode() != Light3D::BAKE_DISABLED) {
		LightsFound lf;
		lf.xform = get_global_transform().affine_inverse() * light->get_global_transform();
		lf.light = light;
		lights.push_back(lf);
	}

	LightmapProbe *probe = Object::cast_to<LightmapProbe>(p_at_node);

	if (probe) {
		Transform3D xf = get_global_transform().affine_inverse() * probe->get_global_transform();
		probes.push_back(xf.origin);
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		Node *child = p_at_node->get_child(i);
		if (!child->get_owner()) {
			continue; //maybe a helper
		}

		_find_meshes_and_lights(child, meshes, lights, probes);
	}
}

int LightmapGI::_bsp_get_simplex_side(const LocalVector<Vector3> &p_points, const LocalVector<BSPSimplex> &p_simplices, const Plane &p_plane, uint32_t p_simplex) const {
	int over = 0;
	int under = 0;
	const BSPSimplex &s = p_simplices[p_simplex];
	for (int i = 0; i < 4; i++) {
		const Vector3 v = p_points[s.vertices[i]];
		// The tolerance used here comes from experiments on scenes up to
		// 1000x1000x100 meters. If it's any smaller, some simplices will
		// appear to self-intersect due to a lack of precision in Plane.
		if (p_plane.has_point(v, 1.0 / (1 << 13))) {
			// Coplanar.
		} else if (p_plane.is_point_over(v)) {
			over++;
		} else {
			under++;
		}
	}

	ERR_FAIL_COND_V(under == 0 && over == 0, -2); //should never happen, we discarded flat simplices before, but in any case drop it from the bsp tree and throw an error
	if (under == 0) {
		return 1; // all over
	} else if (over == 0) {
		return -1; // all under
	} else {
		return 0; // crossing
	}
}

//#define DEBUG_BSP

int32_t LightmapGI::_compute_bsp_tree(const LocalVector<Vector3> &p_points, const LocalVector<Plane> &p_planes, LocalVector<int32_t> &planes_tested, const LocalVector<BSPSimplex> &p_simplices, const LocalVector<int32_t> &p_simplex_indices, LocalVector<BSPNode> &bsp_nodes) {
	ERR_FAIL_COND_V(p_simplex_indices.size() < 2, -1);

	int32_t node_index = (int32_t)bsp_nodes.size();
	bsp_nodes.push_back(BSPNode());

	//test with all the simplex planes
	Plane best_plane;
	float best_plane_score = -1.0;

	for (const int idx : p_simplex_indices) {
		const BSPSimplex &s = p_simplices[idx];
		for (int j = 0; j < 4; j++) {
			uint32_t plane_index = s.planes[j];
			if (planes_tested[plane_index] == node_index) {
				continue; //tested this plane already
			}

			planes_tested[plane_index] = node_index;

			static const int face_order[4][3] = {
				{ 0, 1, 2 },
				{ 0, 2, 3 },
				{ 0, 1, 3 },
				{ 1, 2, 3 }
			};

			// despite getting rid of plane duplicates, we should still use here the actual plane to avoid numerical error
			// from thinking this same simplex is intersecting rather than on a side
			Vector3 v0 = p_points[s.vertices[face_order[j][0]]];
			Vector3 v1 = p_points[s.vertices[face_order[j][1]]];
			Vector3 v2 = p_points[s.vertices[face_order[j][2]]];

			Plane plane(v0, v1, v2);

			//test with all the simplices
			int over_count = 0;
			int under_count = 0;

			for (const int &index : p_simplex_indices) {
				int side = _bsp_get_simplex_side(p_points, p_simplices, plane, index);
				if (side == -2) {
					continue; //this simplex is invalid, skip for now
				} else if (side < 0) {
					under_count++;
				} else if (side > 0) {
					over_count++;
				}
			}

			if (under_count == 0 && over_count == 0) {
				continue; //most likely precision issue with a flat simplex, do not try this plane
			}

			if (under_count > over_count) { //make sure under is always less than over, so we can compute the same ratio
				SWAP(under_count, over_count);
			}

			float score = 0; //by default, score is 0 (worst)
			if (over_count > 0) {
				// Simplices that are intersected by the plane are moved into both the over
				// and under subtrees which makes the entire tree deeper, so the best plane
				// will have the least intersections while separating the simplices evenly.
				float balance = float(under_count) / over_count;
				float separation = float(over_count + under_count) / p_simplex_indices.size();
				score = balance * separation * separation;
			}

			if (score > best_plane_score) {
				best_plane = plane;
				best_plane_score = score;
			}
		}
	}

	// We often end up with two (or on rare occasions, three) simplices that are
	// either disjoint or share one vertex and don't have a separating plane
	// among their faces. The fallback is to loop through new planes created
	// with one vertex of the first simplex and two vertices of the second until
	// we find a winner.
	if (best_plane_score == 0) {
		const BSPSimplex &simplex0 = p_simplices[p_simplex_indices[0]];
		const BSPSimplex &simplex1 = p_simplices[p_simplex_indices[1]];

		for (uint32_t i = 0; i < 4 && !best_plane_score; i++) {
			Vector3 v0 = p_points[simplex0.vertices[i]];
			for (uint32_t j = 0; j < 3 && !best_plane_score; j++) {
				if (simplex0.vertices[i] == simplex1.vertices[j]) {
					break;
				}
				Vector3 v1 = p_points[simplex1.vertices[j]];
				for (uint32_t k = j + 1; k < 4; k++) {
					if (simplex0.vertices[i] == simplex1.vertices[k]) {
						break;
					}
					Vector3 v2 = p_points[simplex1.vertices[k]];

					Plane plane = Plane(v0, v1, v2);
					if (plane == Plane()) { // When v0, v1, and v2 are collinear, they can't form a plane.
						continue;
					}
					int32_t side0 = _bsp_get_simplex_side(p_points, p_simplices, plane, p_simplex_indices[0]);
					int32_t side1 = _bsp_get_simplex_side(p_points, p_simplices, plane, p_simplex_indices[1]);
					if ((side0 == 1 && side1 == -1) || (side0 == -1 && side1 == 1)) {
						best_plane = plane;
						best_plane_score = 1.0;
						break;
					}
				}
			}
		}
	}

	LocalVector<int32_t> indices_over;
	LocalVector<int32_t> indices_under;

	//split again, but add to list
	for (const uint32_t index : p_simplex_indices) {
		int side = _bsp_get_simplex_side(p_points, p_simplices, best_plane, index);

		if (side == -2) {
			continue; //simplex sits on the plane, does not make sense to use it
		}
		if (side <= 0) {
			indices_under.push_back(index);
		}

		if (side >= 0) {
			indices_over.push_back(index);
		}
	}

#ifdef DEBUG_BSP
	print_line("node " + itos(node_index) + " found plane: " + best_plane + " score:" + rtos(best_plane_score) + " - over " + itos(indices_over.size()) + " under " + itos(indices_under.size()) + " intersecting " + itos(intersecting));
#endif

	if (best_plane_score < 0.0 || indices_over.size() == p_simplex_indices.size() || indices_under.size() == p_simplex_indices.size()) {
		// Failed to separate the tetrahedrons using planes
		// this means Delaunay broke at some point.
		// Luckily, because we are using tetrahedrons, we can resort to
		// less precise but still working ways to generate the separating plane
		// this will most likely look bad when interpolating, but at least it will not crash.
		// and the artifact will most likely also be very small, so too difficult to notice.

		//find the longest axis

		WARN_PRINT("Inconsistency found in triangulation while building BSP, probe interpolation quality may degrade a bit.");

		LocalVector<Vector3> centers;
		AABB bounds_all;
		for (uint32_t i = 0; i < p_simplex_indices.size(); i++) {
			AABB bounds;
			for (uint32_t j = 0; j < 4; j++) {
				Vector3 p = p_points[p_simplices[p_simplex_indices[i]].vertices[j]];
				if (j == 0) {
					bounds.position = p;
				} else {
					bounds.expand_to(p);
				}
			}
			if (i == 0) {
				centers.push_back(bounds.get_center());
			} else {
				bounds_all.merge_with(bounds);
			}
		}
		Vector3::Axis longest_axis = Vector3::Axis(bounds_all.get_longest_axis_index());

		//find the simplex that will go under
		uint32_t min_d_idx = 0xFFFFFFFF;
		float min_d_dist = 1e20;

		for (uint32_t i = 0; i < centers.size(); i++) {
			if (centers[i][longest_axis] < min_d_dist) {
				min_d_idx = i;
				min_d_dist = centers[i][longest_axis];
			}
		}
		//rebuild best_plane and over/under arrays
		best_plane = Plane();
		best_plane.normal[longest_axis] = 1.0;
		best_plane.d = min_d_dist;

		indices_under.clear();
		indices_under.push_back(min_d_idx);

		indices_over.clear();

		for (uint32_t i = 0; i < p_simplex_indices.size(); i++) {
			if (i == min_d_idx) {
				continue;
			}
			indices_over.push_back(p_simplex_indices[i]);
		}
	}

	BSPNode node;
	node.plane = best_plane;

	if (indices_under.is_empty()) {
		//nothing to do here
		node.under = BSPNode::EMPTY_LEAF;
	} else if (indices_under.size() == 1) {
		node.under = -(indices_under[0] + 1);
	} else {
		node.under = _compute_bsp_tree(p_points, p_planes, planes_tested, p_simplices, indices_under, bsp_nodes);
	}

	if (indices_over.is_empty()) {
		//nothing to do here
		node.over = BSPNode::EMPTY_LEAF;
	} else if (indices_over.size() == 1) {
		node.over = -(indices_over[0] + 1);
	} else {
		node.over = _compute_bsp_tree(p_points, p_planes, planes_tested, p_simplices, indices_over, bsp_nodes);
	}

	bsp_nodes[node_index] = node;

	return node_index;
}

bool LightmapGI::_lightmap_bake_step_function(float p_completion, const String &p_text, void *ud, bool p_refresh) {
	BakeStepUD *bsud = (BakeStepUD *)ud;
	bool ret = false;
	if (bsud->func) {
		ret = bsud->func(bsud->from_percent + p_completion * (bsud->to_percent - bsud->from_percent), p_text, bsud->ud, p_refresh);
	}
	return ret;
}

void LightmapGI::_plot_triangle_into_octree(GenProbesOctree *p_cell, float p_cell_size, const Vector3 *p_triangle) {
	for (int i = 0; i < 8; i++) {
		Vector3i pos = p_cell->offset;
		uint32_t half_size = p_cell->size / 2;
		if (i & 1) {
			pos.x += half_size;
		}
		if (i & 2) {
			pos.y += half_size;
		}
		if (i & 4) {
			pos.z += half_size;
		}

		AABB subcell;
		subcell.position = Vector3(pos) * p_cell_size;
		subcell.size = Vector3(half_size, half_size, half_size) * p_cell_size;

		if (!Geometry3D::triangle_box_overlap(subcell.get_center(), subcell.size * 0.5, p_triangle)) {
			continue;
		}

		if (p_cell->children[i] == nullptr) {
			GenProbesOctree *child = memnew(GenProbesOctree);
			child->offset = pos;
			child->size = half_size;
			p_cell->children[i] = child;
		}

		if (half_size > 1) {
			//still levels missing
			_plot_triangle_into_octree(p_cell->children[i], p_cell_size, p_triangle);
		}
	}
}

void LightmapGI::_gen_new_positions_from_octree(const GenProbesOctree *p_cell, float p_cell_size, const Vector<Vector3> &probe_positions, LocalVector<Vector3> &new_probe_positions, HashMap<Vector3i, bool> &positions_used, const AABB &p_bounds) {
	for (int i = 0; i < 8; i++) {
		Vector3i pos = p_cell->offset;
		if (i & 1) {
			pos.x += p_cell->size;
		}
		if (i & 2) {
			pos.y += p_cell->size;
		}
		if (i & 4) {
			pos.z += p_cell->size;
		}

		if (p_cell->size == 1 && !positions_used.has(pos)) {
			//new position to insert!
			Vector3 real_pos = p_bounds.position + Vector3(pos) * p_cell_size;
			//see if a user submitted probe is too close
			int ppcount = probe_positions.size();
			const Vector3 *pp = probe_positions.ptr();
			bool exists = false;
			for (int j = 0; j < ppcount; j++) {
				if (pp[j].distance_to(real_pos) < (p_cell_size * 0.5f)) {
					exists = true;
					break;
				}
			}

			if (!exists) {
				new_probe_positions.push_back(real_pos);
			}

			positions_used[pos] = true;
		}

		if (p_cell->children[i] != nullptr) {
			_gen_new_positions_from_octree(p_cell->children[i], p_cell_size, probe_positions, new_probe_positions, positions_used, p_bounds);
		}
	}
}

LightmapGI::BakeError LightmapGI::_save_and_reimport_atlas_textures(const Ref<Lightmapper> p_lightmapper, const String &p_base_name, TypedArray<TextureLayered> &r_textures, bool p_is_shadowmask) const {
	Vector<Ref<Image>> images;
	images.resize(p_is_shadowmask ? p_lightmapper->get_shadowmask_texture_count() : p_lightmapper->get_bake_texture_count());

	for (int i = 0; i < images.size(); i++) {
		images.set(i, p_is_shadowmask ? p_lightmapper->get_shadowmask_texture(i) : p_lightmapper->get_bake_texture(i));
	}

	const int slice_count = images.size();
	const int slice_width = images[0]->get_width();
	const int slice_height = images[0]->get_height();

	const int slices_per_texture = Image::MAX_HEIGHT / slice_height;
	const int texture_count = Math::ceil(slice_count / (float)slices_per_texture);
	const int last_count = slice_count % slices_per_texture;

	r_textures.resize(texture_count);

	for (int i = 0; i < texture_count; i++) {
		const int texture_slice_count = (i == texture_count - 1 && last_count != 0) ? last_count : slices_per_texture;

		Ref<Image> texture_image = Image::create_empty(slice_width, slice_height * texture_slice_count, false, images[0]->get_format());

		for (int j = 0; j < texture_slice_count; j++) {
			texture_image->blit_rect(images[i * slices_per_texture + j], Rect2i(0, 0, slice_width, slice_height), Point2i(0, slice_height * j));
		}

		const String atlas_path = (texture_count > 1 ? p_base_name + "_" + itos(i) : p_base_name) + (p_is_shadowmask ? ".png" : ".exr");
		const String config_path = atlas_path + ".import";

		Ref<ConfigFile> config;
		config.instantiate();

		// Load an import configuration if present.
		if (FileAccess::exists(config_path)) {
			config->load(config_path);
		}

		config->set_value("remap", "importer", "2d_array_texture");
		config->set_value("remap", "type", "CompressedTexture2DArray");
		if (!config->has_section_key("params", "compress/mode")) {
			// Do not override an existing compression mode.
			config->set_value("params", "compress/mode", 2);
		}
		config->set_value("params", "compress/channel_pack", 1);
		config->set_value("params", "mipmaps/generate", false);
		config->set_value("params", "slices/horizontal", 1);
		config->set_value("params", "slices/vertical", texture_slice_count);

		config->save(config_path);

		if (supersampling_enabled) {
			texture_image->resize(texture_image->get_width() / supersampling_factor, texture_image->get_height() / supersampling_factor, Image::INTERPOLATE_TRILINEAR);
		}

		// Save the file.
		Error save_err;
		if (p_is_shadowmask) {
			save_err = texture_image->save_png(atlas_path);
		} else {
			save_err = texture_image->save_exr(atlas_path, false);
		}

		ERR_FAIL_COND_V(save_err, LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE);

		// Reimport the file.
		ResourceLoader::import(atlas_path);
		Ref<TextureLayered> t = ResourceLoader::load(atlas_path); // If already loaded, it will be updated on refocus?
		ERR_FAIL_COND_V(t.is_null(), LightmapGI::BAKE_ERROR_CANT_CREATE_IMAGE);

		// Store the atlas in the array.
		r_textures[i] = t;
	}

	return LightmapGI::BAKE_ERROR_OK;
}

void LightmapGI::_build_area_light_texture_atlas(const Vector<LightmapGI::LightsFound> &lights_found, HashMap<Ref<Texture2D>, AreaLightAtlasTexture> &r_textures, Size2i &r_atlas_size, int &r_mipmaps) const {
	if (RenderingServer::get_singleton()->get_current_rendering_method() != "gl_compatibility") { // area light textures unsupported in compat
		r_mipmaps = 8;
		r_atlas_size = Size2i(pow(2, r_mipmaps), pow(2, r_mipmaps));

		for (int i = 0; i < lights_found.size(); i++) {
			Light3D *light = lights_found[i].light;
			if (Object::cast_to<AreaLight3D>(light)) {
				AreaLight3D *l = Object::cast_to<AreaLight3D>(light);
				if (l->get_area_texture().is_valid() && !r_textures.has(l->get_area_texture())) {
					r_textures[l->get_area_texture()] = AreaLightAtlasTexture();
				}
			}
		}
	}

	if (r_textures.size() == 0) {
		r_mipmaps = 1;
		r_atlas_size = Size2i(1, 1);
		return;
	}

	struct SortItem {
		Ref<Texture2D> texture;
		Size2i pixel_size;
		Size2i size;
		Point2i pos;

		bool operator<(const SortItem &p_item) const {
			//sort larger to smaller
			if (size.height == p_item.size.height) {
				return size.width > p_item.size.width;
			} else {
				return size.height > p_item.size.height;
			}
		}
	};

	//generate atlas
	Vector<SortItem> itemsv;
	itemsv.resize(r_textures.size());
	uint32_t base_size = 1;

	int idx = 0;
	int border = 1 << (r_mipmaps - 1);

	for (const KeyValue<Ref<Texture2D>, AreaLightAtlasTexture> &E : r_textures) {
		Ref<Texture2D> tex = E.key;
		Size2i tex_size = Size2i(tex->get_width(), tex->get_height());

		SortItem &si = itemsv.write[idx];

		Vector2i b_size = Vector2i(Math::ceil(float(tex_size.width) / border), Math::ceil(float(tex_size.height) / border));
		si.size.width = b_size.width + 1;
		si.size.height = b_size.height + 1;
		si.pixel_size = b_size * border; // components are either small powers of 2 or N * border
		if (tex_size.width < border) {
			si.pixel_size.width = Math::nearest_power_of_2_templated(tex_size.width);
		}
		if (tex_size.height < border) {
			si.pixel_size.height = Math::nearest_power_of_2_templated(tex_size.height);
		}

		if (base_size < (uint32_t)si.size.width) {
			base_size = Math::nearest_power_of_2_templated(si.size.width);
		}

		si.texture = tex;
		idx++;
	}

	//sort items by size
	itemsv.sort();

	//attempt to create atlas
	int item_count = itemsv.size();
	SortItem *items = itemsv.ptrw();

	int atlas_height = 0;

	while (true) {
		Vector<int> v_offsetsv;
		v_offsetsv.resize(base_size);

		int *v_offsets = v_offsetsv.ptrw();
		memset(v_offsets, 0, sizeof(int) * base_size);

		int max_height = 0;

		for (int i = 0; i < item_count; i++) {
			//best fit
			SortItem &si = items[i];
			int best_idx = -1;
			int best_height = 0x7FFFFFFF;
			for (uint32_t j = 0; j <= base_size - si.size.width; j++) {
				int height = 0;
				for (int k = 0; k < si.size.width; k++) {
					int h = v_offsets[k + j];
					if (h > height) {
						height = h;
						if (height > best_height) {
							break; //already bad
						}
					}
				}

				if (height < best_height) {
					best_height = height;
					best_idx = j;
				}
			}

			//update
			for (int k = 0; k < si.size.width; k++) {
				v_offsets[k + best_idx] = best_height + si.size.height;
			}

			si.pos.x = best_idx;
			si.pos.y = best_height;

			if (si.pos.y + si.size.height > max_height) {
				max_height = si.pos.y + si.size.height;
			}
		}

		if ((uint32_t)max_height <= base_size * 2) {
			atlas_height = max_height;
			break; //good ratio, break;
		}

		base_size *= 2;
	}

	r_atlas_size.width = base_size * border;
	r_atlas_size.height = Math::nearest_power_of_2_templated(atlas_height * border);

	for (int i = 0; i < item_count; i++) {
		Rect2 uv_rect;
		uv_rect.position = items[i].pos * border;
		uv_rect.size = items[i].pixel_size;

		uv_rect.position /= Size2(r_atlas_size);
		uv_rect.size /= Size2(r_atlas_size);
		r_textures[items[i].texture].texture_rect = uv_rect;
		float max_mipmap = MIN(Math::floor(Math::log2(MAX(MIN(items[i].pixel_size.x, items[i].pixel_size.y), 1.0f))), r_mipmaps) - 1.0f;
		r_textures[items[i].texture].max_mipmap = max_mipmap;
	}
}

LightmapGI::BakeError LightmapGI::bake(Node *p_from_node, String p_image_data_path, Lightmapper::BakeStepFunc p_bake_step, void *p_bake_userdata) {
	if (p_image_data_path.is_empty()) {
		if (get_light_data().is_null()) {
			return BAKE_ERROR_NO_SAVE_PATH;
		}

		p_image_data_path = get_light_data()->get_path();
		if (!p_image_data_path.is_resource_file()) {
			return BAKE_ERROR_NO_SAVE_PATH;
		}
	}

	Ref<Lightmapper> lightmapper = Lightmapper::create();
	ERR_FAIL_COND_V(lightmapper.is_null(), BAKE_ERROR_NO_LIGHTMAPPER);

	BakeStepUD bsud;
	bsud.func = p_bake_step;
	bsud.ud = p_bake_userdata;
	bsud.from_percent = 0.2;
	bsud.to_percent = 0.8;

	if (p_bake_step) {
		p_bake_step(0.0, RTR("Finding meshes, lights and probes"), p_bake_userdata, true);
	}
	/* STEP 1, FIND MESHES, LIGHTS AND PROBES */
	Vector<Lightmapper::MeshData> mesh_data;
	Vector<LightsFound> lights_found;
	Vector<Vector3> probes_found;
	AABB bounds;
	{
		Vector<MeshesFound> meshes_found;
		_find_meshes_and_lights(p_from_node ? p_from_node : get_parent(), meshes_found, lights_found, probes_found);

		if (meshes_found.is_empty()) {
			return BAKE_ERROR_NO_MESHES;
		}
		// create mesh data for insert

		//get the base material textures, help compute atlas size and bounds
		for (int m_i = 0; m_i < meshes_found.size(); m_i++) {
			if (p_bake_step) {
				float p = (float)(m_i) / meshes_found.size();
				p_bake_step(p * 0.1, vformat(RTR("Preparing geometry %d/%d"), m_i, meshes_found.size()), p_bake_userdata, false);
			}

			MeshesFound &mf = meshes_found.write[m_i];

			Size2i mesh_lightmap_size = mf.mesh->get_lightmap_size_hint();
			if (mesh_lightmap_size == Size2i(0, 0)) {
				// TODO we should compute a size if no lightmap hint is set, as we did in 3.x.
				// For now set to basic size to avoid crash.
				mesh_lightmap_size = Size2i(64, 64);
			}
			// Double lightmap texel density if downsampling is enabled, as the final texture size will be halved before saving lightmaps.
			Size2i lightmap_size = Size2i(Size2(mesh_lightmap_size) * mf.lightmap_scale * texel_scale) * (supersampling_enabled ? supersampling_factor : 1.0);
			ERR_FAIL_COND_V(lightmap_size.x == 0 || lightmap_size.y == 0, BAKE_ERROR_LIGHTMAP_TOO_SMALL);

			TypedArray<RID> overrides;
			overrides.resize(mf.overrides.size());
			for (int i = 0; i < mf.overrides.size(); i++) {
				if (mf.overrides[i].is_valid()) {
					overrides[i] = mf.overrides[i]->get_rid();
				}
			}
			TypedArray<Image> images = RS::get_singleton()->bake_render_uv2(mf.mesh->get_rid(), overrides, lightmap_size);

			ERR_FAIL_COND_V(images.is_empty(), BAKE_ERROR_CANT_CREATE_IMAGE);

			Ref<Image> albedo = images[RSE::BAKE_CHANNEL_ALBEDO_ALPHA];
			Ref<Image> orm = images[RSE::BAKE_CHANNEL_ORM];

			//multiply albedo by metal

			Lightmapper::MeshData md;

			{
				Dictionary d;
				d["path"] = mf.node_path;
				if (mf.subindex >= 0) {
					d["subindex"] = mf.subindex;
				}
				md.userdata = d;
			}

			{
				if (albedo->get_format() != Image::FORMAT_RGBA8) {
					albedo->convert(Image::FORMAT_RGBA8);
				}
				if (orm->get_format() != Image::FORMAT_RGBA8) {
					orm->convert(Image::FORMAT_RGBA8);
				}
				Vector<uint8_t> albedo_alpha = albedo->get_data();
				Vector<uint8_t> orm_data = orm->get_data();

				Vector<uint8_t> albedom;
				uint32_t len = albedo_alpha.size();
				albedom.resize(len);
				const uint8_t *r_aa = albedo_alpha.ptr();
				const uint8_t *r_orm = orm_data.ptr();
				uint8_t *w_albedo = albedom.ptrw();

				for (uint32_t i = 0; i < len; i += 4) {
					w_albedo[i + 0] = uint8_t(CLAMP(float(r_aa[i + 0]) * (1.0 - float(r_orm[i + 2] / 255.0)), 0, 255));
					w_albedo[i + 1] = uint8_t(CLAMP(float(r_aa[i + 1]) * (1.0 - float(r_orm[i + 2] / 255.0)), 0, 255));
					w_albedo[i + 2] = uint8_t(CLAMP(float(r_aa[i + 2]) * (1.0 - float(r_orm[i + 2] / 255.0)), 0, 255));
					w_albedo[i + 3] = r_aa[i + 3];
				}

				md.albedo_on_uv2.instantiate();
				md.albedo_on_uv2->set_data(lightmap_size.width, lightmap_size.height, false, Image::FORMAT_RGBA8, albedom);
			}

			md.emission_on_uv2 = images[RSE::BAKE_CHANNEL_EMISSION];
			if (md.emission_on_uv2->get_format() != Image::FORMAT_RGBAH) {
				md.emission_on_uv2->convert(Image::FORMAT_RGBAH);
			}

			//get geometry

			Basis normal_xform = mf.xform.basis.inverse().transposed();

			for (int i = 0; i < mf.mesh->get_surface_count(); i++) {
				if (mf.mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
					continue;
				}
				Array a = mf.mesh->surface_get_arrays(i);
				Ref<Material> mat = mf.mesh->surface_get_material(i);
				RID mat_rid;
				if (mat.is_valid()) {
					mat_rid = mat->get_rid();
				}

				Vector<Vector3> vertices = a[Mesh::ARRAY_VERTEX];
				const Vector3 *vr = vertices.ptr();
				Vector<Vector2> uv = a[Mesh::ARRAY_TEX_UV2];
				const Vector2 *uvr = nullptr;
				Vector<Vector3> normals = a[Mesh::ARRAY_NORMAL];
				const Vector3 *nr = nullptr;
				Vector<int> index = a[Mesh::ARRAY_INDEX];

				ERR_CONTINUE(uv.is_empty());
				ERR_CONTINUE(normals.is_empty());

				uvr = uv.ptr();
				nr = normals.ptr();

				int facecount;
				const int *ir = nullptr;

				if (index.size()) {
					facecount = index.size() / 3;
					ir = index.ptr();
				} else {
					facecount = vertices.size() / 3;
				}

				for (int j = 0; j < facecount; j++) {
					uint32_t vidx[3];

					if (ir) {
						for (int k = 0; k < 3; k++) {
							vidx[k] = ir[j * 3 + k];
						}
					} else {
						for (int k = 0; k < 3; k++) {
							vidx[k] = j * 3 + k;
						}
					}

					for (int k = 0; k < 3; k++) {
						Vector3 v = mf.xform.xform(vr[vidx[k]]);
						if (bounds == AABB()) {
							bounds.position = v;
						} else {
							bounds.expand_to(v);
						}
						md.points.push_back(v);

						md.uv2.push_back(uvr[vidx[k]]);
						md.normal.push_back(normal_xform.xform(nr[vidx[k]]).normalized());
						md.material.push_back(mat_rid);
					}
				}
			}

			mesh_data.push_back(md);
		}
	}

	/* STEP 2, CREATE PROBES */

	if (p_bake_step) {
		p_bake_step(0.3, RTR("Creating probes"), p_bake_userdata, true);
	}

	//bounds need to include the user probes
	for (int i = 0; i < probes_found.size(); i++) {
		bounds.expand_to(probes_found[i]);
	}

	bounds.grow_by(bounds.size.length() * 0.001);

	if (gen_probes == GENERATE_PROBES_DISABLED) {
		// generate 8 probes on bound endpoints
		for (int i = 0; i < 8; i++) {
			probes_found.push_back(bounds.get_endpoint(i));
		}
	} else {
		// detect probes from geometry
		static const int subdiv_values[6] = { 0, 4, 8, 16, 32 };
		int subdiv = subdiv_values[gen_probes];

		float subdiv_cell_size;
		Vector3i bound_limit;
		{
			int longest_axis = bounds.get_longest_axis_index();
			subdiv_cell_size = bounds.size[longest_axis] / subdiv;
			int axis_n1 = (longest_axis + 1) % 3;
			int axis_n2 = (longest_axis + 2) % 3;

			bound_limit[longest_axis] = subdiv;
			bound_limit[axis_n1] = int(Math::ceil(bounds.size[axis_n1] / subdiv_cell_size));
			bound_limit[axis_n2] = int(Math::ceil(bounds.size[axis_n2] / subdiv_cell_size));
			//compensate bounds
			bounds.size[axis_n1] = bound_limit[axis_n1] * subdiv_cell_size;
			bounds.size[axis_n2] = bound_limit[axis_n2] * subdiv_cell_size;
		}

		GenProbesOctree octree;
		octree.size = subdiv;

		for (int i = 0; i < mesh_data.size(); i++) {
			if (p_bake_step) {
				float p = (float)(i) / mesh_data.size();
				p_bake_step(0.3 + p * 0.1, vformat(RTR("Creating probes from mesh %d/%d"), i, mesh_data.size()), p_bake_userdata, false);
			}

			for (int j = 0; j < mesh_data[i].points.size(); j += 3) {
				Vector3 points[3] = { mesh_data[i].points[j + 0] - bounds.position, mesh_data[i].points[j + 1] - bounds.position, mesh_data[i].points[j + 2] - bounds.position };
				_plot_triangle_into_octree(&octree, subdiv_cell_size, points);
			}
		}

		LocalVector<Vector3> new_probe_positions;
		HashMap<Vector3i, bool> positions_used;
		for (uint32_t i = 0; i < 8; i++) { //insert bounding endpoints
			Vector3i pos;
			if (i & 1) {
				pos.x += bound_limit.x;
			}
			if (i & 2) {
				pos.y += bound_limit.y;
			}
			if (i & 4) {
				pos.z += bound_limit.z;
			}

			positions_used[pos] = true;
			Vector3 real_pos = bounds.position + Vector3(pos) * subdiv_cell_size; //use same formula for numerical stability
			new_probe_positions.push_back(real_pos);
		}
		//skip first level, since probes are always added at bounds endpoints anyway (code above this)
		for (int i = 0; i < 8; i++) {
			if (octree.children[i]) {
				_gen_new_positions_from_octree(octree.children[i], subdiv_cell_size, probes_found, new_probe_positions, positions_used, bounds);
			}
		}

		for (const Vector3 &position : new_probe_positions) {
			probes_found.push_back(position);
		}
	}

	// Add everything to lightmapper
	const bool use_physical_light_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");
	if (p_bake_step) {
		p_bake_step(0.4, RTR("Preparing Lightmapper"), p_bake_userdata, true);
	}

	{
		Size2i area_light_atlas_size;
		int area_light_atlas_mipmaps = 1;
		HashMap<Ref<Texture2D>, AreaLightAtlasTexture> area_light_atlas_textures;
		PackedByteArray area_light_atlas_data;
		_build_area_light_texture_atlas(lights_found, area_light_atlas_textures, area_light_atlas_size, area_light_atlas_mipmaps);
		if (area_light_atlas_textures.size() > 0) {
			TypedArray<RID> area_light_textures;
			TypedArray<Rect2> area_light_texture_rects;
			for (const KeyValue<Ref<Texture2D>, AreaLightAtlasTexture> &E : area_light_atlas_textures) {
				area_light_textures.push_back(E.key->get_rid());
				area_light_texture_rects.push_back(E.value.texture_rect);
			}
			area_light_atlas_data = RS::get_singleton()->bake_render_area_light_atlas(area_light_textures, area_light_texture_rects, area_light_atlas_size, area_light_atlas_mipmaps);
		} else {
			area_light_atlas_data.resize_initialized(4); // 1 pixel
		}
		lightmapper->add_area_light_atlas(area_light_atlas_size, area_light_atlas_mipmaps, area_light_atlas_data);

		for (int i = 0; i < mesh_data.size(); i++) {
			lightmapper->add_mesh(mesh_data[i]);
		}
		for (int i = 0; i < lights_found.size(); i++) {
			Light3D *light = lights_found[i].light;
			if (light->is_editor_only()) {
				// Don't include editor-only lights in the lightmap bake,
				// as this results in inconsistent visuals when running the project.
				continue;
			}

			Transform3D xf = lights_found[i].xform;

			// For the lightmapper, the indirect energy represents the multiplier for the indirect bounces caused by the light, so the value is not converted when using physical units.
			float indirect_energy = light->get_param(Light3D::PARAM_INDIRECT_ENERGY);
			Color linear_color = light->get_color().srgb_to_linear();
			float energy = light->get_param(Light3D::PARAM_ENERGY);
			if (use_physical_light_units) {
				energy *= light->get_param(Light3D::PARAM_INTENSITY);
				linear_color *= light->get_correlated_color().srgb_to_linear();
			}

			if (Object::cast_to<DirectionalLight3D>(light)) {
				DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(light);
				if (l->get_sky_mode() != DirectionalLight3D::SKY_MODE_SKY_ONLY) {
					lightmapper->add_directional_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, -xf.basis.get_column(Vector3::AXIS_Z).normalized(), linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR));
				}
			} else if (Object::cast_to<OmniLight3D>(light)) {
				OmniLight3D *l = Object::cast_to<OmniLight3D>(light);
				if (use_physical_light_units) {
					energy *= (1.0 / (Math::PI * 4.0));
				}
				lightmapper->add_omni_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, xf.origin, linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_RANGE), l->get_param(Light3D::PARAM_ATTENUATION), l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR));
			} else if (Object::cast_to<SpotLight3D>(light)) {
				SpotLight3D *l = Object::cast_to<SpotLight3D>(light);
				if (use_physical_light_units) {
					energy *= (1.0 / Math::PI);
				}
				lightmapper->add_spot_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, xf.origin, -xf.basis.get_column(Vector3::AXIS_Z).normalized(), linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_RANGE), l->get_param(Light3D::PARAM_ATTENUATION), l->get_param(Light3D::PARAM_SPOT_ANGLE), l->get_param(Light3D::PARAM_SPOT_ATTENUATION), l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR));
			} else if (Object::cast_to<AreaLight3D>(light)) {
				AreaLight3D *l = Object::cast_to<AreaLight3D>(light);
				if (use_physical_light_units) {
					energy *= (1.0 / Math::PI * 2.0);
				}
				Vector3 area_vec_x = xf.basis.get_column(Vector3::AXIS_X).normalized() * l->get_area_size().x;
				Vector3 area_vec_y = xf.basis.get_column(Vector3::AXIS_Y).normalized() * l->get_area_size().y;
				if (l->is_area_normalizing_energy()) {
					float surface_area = l->get_area_size().x * l->get_area_size().y;
					energy /= surface_area;
				}
				AreaLightAtlasTexture tex;
				if (l->get_area_texture().is_valid()) {
					tex = area_light_atlas_textures[l->get_area_texture()];
				}
				lightmapper->add_area_light(light->get_name(), light->get_bake_mode() == Light3D::BAKE_STATIC, xf.origin, -xf.basis.get_column(Vector3::AXIS_Z).normalized(), linear_color, energy, indirect_energy, l->get_param(Light3D::PARAM_RANGE), l->get_param(Light3D::PARAM_ATTENUATION), area_vec_x, area_vec_y, l->get_param(Light3D::PARAM_SIZE), l->get_param(Light3D::PARAM_SHADOW_BLUR), tex.texture_rect, tex.max_mipmap);
			}
		}
		for (int i = 0; i < probes_found.size(); i++) {
			lightmapper->add_probe(probes_found[i]);
		}
	}

	Ref<Image> environment_image;
	Basis environment_transform;

	// Add everything to lightmapper
	if (environment_mode != ENVIRONMENT_MODE_DISABLED) {
		if (p_bake_step) {
			p_bake_step(4.1, RTR("Preparing Environment"), p_bake_userdata, true);
		}

		environment_transform = get_global_transform().basis;

		switch (environment_mode) {
			case ENVIRONMENT_MODE_DISABLED: {
				//nothing
			} break;
			case ENVIRONMENT_MODE_SCENE: {
				Ref<World3D> world = get_world_3d();
				if (world.is_valid()) {
					Ref<Environment> env = world->get_environment();
					if (env.is_null()) {
						env = world->get_fallback_environment();
					}

					if (env.is_valid()) {
						environment_image = RS::get_singleton()->environment_bake_panorama(env->get_rid(), true, Size2i(128, 64));
						environment_transform = Basis::from_euler(env->get_sky_rotation()).inverse();
					}
				}
			} break;
			case ENVIRONMENT_MODE_CUSTOM_SKY: {
				if (environment_custom_sky.is_valid()) {
					environment_image = RS::get_singleton()->sky_bake_panorama(environment_custom_sky->get_rid(), environment_custom_energy, true, Size2i(128, 64));
				}

			} break;
			case ENVIRONMENT_MODE_CUSTOM_COLOR: {
				environment_image.instantiate();
				environment_image->initialize_data(128, 64, false, Image::FORMAT_RGBAF);
				Color c = environment_custom_color;
				c.r *= environment_custom_energy;
				c.g *= environment_custom_energy;
				c.b *= environment_custom_energy;
				environment_image->fill(c);

			} break;
		}
	}

	float exposure_normalization = 1.0;
	if (camera_attributes.is_valid()) {
		exposure_normalization = camera_attributes->get_exposure_multiplier();
		if (use_physical_light_units) {
			exposure_normalization = camera_attributes->calculate_exposure_normalization();
		}
	}

	Lightmapper::BakeError bake_err = lightmapper->bake(Lightmapper::BakeQuality(bake_quality), use_denoiser, denoiser_strength, denoiser_range, bounces,
			bounce_indirect_energy, bias, max_texture_size, directional, shadowmask_mode != LightmapGIData::SHADOWMASK_MODE_NONE, use_texture_for_bounces,
			Lightmapper::GenerateProbes(gen_probes), environment_image, environment_transform, _lightmap_bake_step_function, &bsud, exposure_normalization, (supersampling_enabled ? supersampling_factor : 1));

	if (bake_err == Lightmapper::BAKE_ERROR_TEXTURE_EXCEEDS_MAX_SIZE) {
		return BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL;
	} else if (bake_err == Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES) {
		return BAKE_ERROR_MESHES_INVALID;
	} else if (bake_err == Lightmapper::BAKE_ERROR_ATLAS_TOO_SMALL) {
		return BAKE_ERROR_ATLAS_TOO_SMALL;
	} else if (bake_err == Lightmapper::BAKE_ERROR_USER_ABORTED) {
		return BAKE_ERROR_USER_ABORTED;
	}

	// POSTBAKE: Save Textures.
	TypedArray<TextureLayered> lightmap_textures;
	TypedArray<TextureLayered> shadowmask_textures;

	const String texture_filename = p_image_data_path.get_basename();
	const int shadowmask_texture_count = lightmapper->get_shadowmask_texture_count();
	const bool save_shadowmask = shadowmask_mode != LightmapGIData::SHADOWMASK_MODE_NONE && shadowmask_texture_count > 0;

	// Save the lightmap atlases.
	BakeError save_err = _save_and_reimport_atlas_textures(lightmapper, texture_filename, lightmap_textures, false);
	ERR_FAIL_COND_V(save_err != BAKE_ERROR_OK, save_err);

	if (save_shadowmask) {
		// Save the shadowmask atlases.
		save_err = _save_and_reimport_atlas_textures(lightmapper, texture_filename + "_shadow", shadowmask_textures, true);
		ERR_FAIL_COND_V(save_err != BAKE_ERROR_OK, save_err);
	}

	// POSTBAKE: Save Light Data.
	Ref<LightmapGIData> gi_data;

	if (get_light_data().is_valid()) {
		gi_data = get_light_data();
		set_light_data(Ref<LightmapGIData>()); // Clear.
		gi_data->clear();

	} else {
		gi_data.instantiate();
	}

	gi_data->set_lightmap_textures(lightmap_textures);

	if (save_shadowmask) {
		gi_data->set_shadowmask_textures(shadowmask_textures);
	} else {
		gi_data->clear_shadowmask_textures();
	}

	gi_data->set_uses_spherical_harmonics(directional);
	gi_data->_set_uses_packed_directional(directional); // New SH lightmaps are packed automatically.

	for (int i = 0; i < lightmapper->get_bake_mesh_count(); i++) {
		Dictionary d = lightmapper->get_bake_mesh_userdata(i);
		NodePath np = d["path"];
		int32_t subindex = -1;
		if (d.has("subindex")) {
			subindex = d["subindex"];
		}

		Rect2 uv_scale = lightmapper->get_bake_mesh_uv_scale(i);
		int slice_index = lightmapper->get_bake_mesh_texture_slice(i);
		gi_data->add_user(np, uv_scale, slice_index, subindex);
	}

	int probe_count = lightmapper->get_bake_probe_count();

	// Probe SH may change between bakes.
	LocalVector<Color> probe_sh;
	LocalVector<Vector3> probe_points;
	probe_sh.resize(probe_count * 9);
	probe_points.resize(probe_count);

	uint32_t bake_probe_hash = HASH_MURMUR3_SEED;
	for (int i = 0; i < probe_count; i++) {
		// Calculate the hash from probe positions.
		Vector3 point = lightmapper->get_bake_probe_point(i);
		bake_probe_hash = hash_murmur3_one_double(point.x, bake_probe_hash);
		bake_probe_hash = hash_murmur3_one_double(point.y, bake_probe_hash);
		bake_probe_hash = hash_murmur3_one_double(point.z, bake_probe_hash);

		probe_points[i] = point;
		Vector<Color> colors = lightmapper->get_bake_probe_sh(i);
		ERR_CONTINUE(colors.size() != 9);
		for (int j = 0; j < 9; j++) {
			probe_sh[i * 9 + j] = colors[j];
		}
	}

	// If the probe hash doesn't match, build the BSP tree from scratch.
	if (bake_probe_hash != gi_data->get_lightprobe_hash()) {
		// Obtain solved simplices.
		if (p_bake_step) {
			p_bake_step(0.8, RTR("Generating Probe Volumes"), p_bake_userdata, true);
		}

		Vector<Delaunay3D::OutputSimplex> solved_simplices = Delaunay3D::tetrahedralize(Vector<Vector3>(probe_points));
		int64_t simplex_count = solved_simplices.size();

		LocalVector<BSPSimplex> bsp_simplices;
		LocalVector<Plane> bsp_planes;
		LocalVector<int32_t> bsp_simplex_indices;
		PackedInt32Array tetrahedrons;

		for (int i = 0; i < simplex_count; i++) {
			//Prepare a special representation of the simplex, which uses a BSP Tree
			BSPSimplex bsp_simplex;
			for (int j = 0; j < 4; j++) {
				bsp_simplex.vertices[j] = solved_simplices[i].points[j];
			}
			for (int j = 0; j < 4; j++) {
				static const int face_order[4][3] = {
					{ 0, 1, 2 },
					{ 0, 2, 3 },
					{ 0, 1, 3 },
					{ 1, 2, 3 }
				};
				Vector3 a = probe_points[solved_simplices[i].points[face_order[j][0]]];
				Vector3 b = probe_points[solved_simplices[i].points[face_order[j][1]]];
				Vector3 c = probe_points[solved_simplices[i].points[face_order[j][2]]];

				//store planes in an array, but ensure they are reused, to speed up processing

				Plane p(a, b, c);
				int plane_index = -1;
				for (uint32_t k = 0; k < bsp_planes.size(); k++) {
					if (bsp_planes[k].is_equal_approx_any_side(p)) {
						plane_index = k;
						break;
					}
				}

				if (plane_index == -1) {
					plane_index = bsp_planes.size();
					bsp_planes.push_back(p);
				}

				bsp_simplex.planes[j] = plane_index;

				//also fill simplex array
				tetrahedrons.push_back(solved_simplices[i].points[j]);
			}

			bsp_simplex_indices.push_back(bsp_simplices.size());
			bsp_simplices.push_back(bsp_simplex);
		}

//#define DEBUG_SIMPLICES_AS_OBJ_FILE
#ifdef DEBUG_SIMPLICES_AS_OBJ_FILE
		{
			Ref<FileAccess> f = FileAccess::open("res://bsp.obj", FileAccess::WRITE);
			for (uint32_t i = 0; i < bsp_simplices.size(); i++) {
				f->store_line("o Simplex" + itos(i));
				for (int j = 0; j < 4; j++) {
					f->store_line(vformat("v %f %f %f", probe_points[bsp_simplices[i].vertices[j]].x, probe_points[bsp_simplices[i].vertices[j]].y, probe_points[bsp_simplices[i].vertices[j]].z));
				}
				static const int face_order[4][3] = {
					{ 1, 2, 3 },
					{ 1, 3, 4 },
					{ 1, 2, 4 },
					{ 2, 3, 4 }
				};

				for (int j = 0; j < 4; j++) {
					f->store_line(vformat("f %d %d %d", 4 * i + face_order[j][0], 4 * i + face_order[j][1], 4 * i + face_order[j][2]));
				}
			}
		}
#endif

		LocalVector<BSPNode> bsp_nodes;
		LocalVector<int32_t> planes_tested;
		planes_tested.resize(bsp_planes.size());
		for (int &index : planes_tested) {
			index = 0x7FFFFFFF;
		}

		if (p_bake_step) {
			p_bake_step(0.9, RTR("Generating Probe Acceleration Structures"), p_bake_userdata, true);
		}

		// Compute a BSP tree of the simplices, so it's easy to find the exact one.
		_compute_bsp_tree(probe_points, bsp_planes, planes_tested, bsp_simplices, bsp_simplex_indices, bsp_nodes);

		PackedInt32Array bsp_array;
		bsp_array.resize(bsp_nodes.size() * 6); // six 32 bits values used for each BSP node
		{
			float *fptr = (float *)bsp_array.ptrw();
			int32_t *iptr = (int32_t *)bsp_array.ptrw();
			for (uint32_t i = 0; i < bsp_nodes.size(); i++) {
				fptr[i * 6 + 0] = bsp_nodes[i].plane.normal.x;
				fptr[i * 6 + 1] = bsp_nodes[i].plane.normal.y;
				fptr[i * 6 + 2] = bsp_nodes[i].plane.normal.z;
				fptr[i * 6 + 3] = bsp_nodes[i].plane.d;
				iptr[i * 6 + 4] = bsp_nodes[i].over;
				iptr[i * 6 + 5] = bsp_nodes[i].under;
			}
//#define DEBUG_BSP_TREE
#ifdef DEBUG_BSP_TREE
			Ref<FileAccess> f = FileAccess::open("res://bsp.txt", FileAccess::WRITE);
			for (uint32_t i = 0; i < bsp_nodes.size(); i++) {
				f->store_line(itos(i) + " - plane: " + bsp_nodes[i].plane + " over: " + itos(bsp_nodes[i].over) + " under: " + itos(bsp_nodes[i].under));
			}
#endif
		}

		gi_data->set_capture_data(bounds, interior, Vector<Vector3>(probe_points), Vector<Color>(probe_sh), tetrahedrons, bsp_array, exposure_normalization, bake_probe_hash);
	} else {
		gi_data->set_capture_data(bounds, interior, Vector<Vector3>(probe_points), Vector<Color>(probe_sh), gi_data->get_capture_tetrahedra(), gi_data->get_capture_bsp_tree(), exposure_normalization, bake_probe_hash);
	}

	// Saved by path rather than by reference when this lightmap streams, so that
	// loading a scene does not bring every section's atlas into video memory.
	gi_data->set_streaming_enabled(streaming_mode != STREAMING_MODE_DISABLED);

	gi_data->set_path(p_image_data_path, true);
	Error err = ResourceSaver::save(gi_data);

	if (err != OK) {
		return BAKE_ERROR_CANT_CREATE_IMAGE;
	}

	set_light_data(gi_data);
	update_configuration_warnings();

	return BAKE_ERROR_OK;
}

void LightmapGI::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POST_ENTER_TREE: {
			if (light_data.is_valid()) {
				ERR_FAIL_COND_MSG(
						light_data->is_using_spherical_harmonics() && !light_data->_is_using_packed_directional(),
						vformat(
								"%s (%s): The directional lightmap textures are stored in a format that isn't supported anymore. Please bake lightmaps again to make lightmaps display from this node again.",
								get_light_data()->get_path(), get_name()));

				if (last_owner && last_owner != get_owner()) {
					light_data->clear_users();
				}

				if (_streaming_is_active()) {
					streaming_derived_bounds_dirty = true;
					set_process_internal(true);

					// A resource saved for streaming arrives with no atlases at all,
					// while one saved normally arrives with them already in video
					// memory. Start from whichever is actually the case.
					streaming_state = light_data->are_textures_resident() ? STREAMING_STATE_RESIDENT : STREAMING_STATE_RELEASED;

					if (streaming_state == STREAMING_STATE_RESIDENT) {
						if (streaming_load_wanted) {
							// Already paid for by the scene load and wanted anyway.
							_assign_lightmaps();
						} else {
							_streaming_release();
						}
					}
					// When released, _streaming_process() acquires the atlases on the
					// first frame if the streaming policy asks for them.
				} else if (light_data->are_textures_resident()) {
					_assign_lightmaps();
				} else {
					// A resource saved for streaming carries paths instead of atlases,
					// so it has to be materialized first. It binds the meshes itself,
					// and deliberately leaves them unbound if the load fails: binding
					// to a released lightmap samples the default white array and would
					// render the whole section blown out.
					_streaming_force_resident();
				}
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			// A load already in flight is collected even if streaming just went
			// inactive, otherwise its budget slot and its loader tokens, which hold
			// references to the atlases, would never be released.
			if (_streaming_is_active() || streaming_state == STREAMING_STATE_LOADING) {
				_streaming_process();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			last_owner = get_owner();

			_streaming_abort_load();

			if (light_data.is_valid()) {
				_clear_lightmaps();
			}
		} break;
	}
}

void LightmapGI::_assign_lightmaps() {
	ERR_FAIL_COND(light_data.is_null());

	Vector<String> missing_node_paths;

	for (int i = 0; i < light_data->get_user_count(); i++) {
		NodePath user_path = light_data->get_user_path(i);
		Node *node = get_node_or_null(user_path);
		if (!node) {
			missing_node_paths.push_back(String(user_path));
			continue;
		}
		int instance_idx = light_data->get_user_sub_instance(i);
		if (instance_idx >= 0) {
			RID instance_id = node->call("get_bake_mesh_instance", instance_idx);
			if (instance_id.is_valid()) {
				RS::get_singleton()->instance_geometry_set_lightmap(instance_id, get_instance(), light_data->get_user_lightmap_uv_scale(i), light_data->get_user_lightmap_slice_index(i));
			}
		} else {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			ERR_CONTINUE(!vi);
			RS::get_singleton()->instance_geometry_set_lightmap(vi->get_instance(), get_instance(), light_data->get_user_lightmap_uv_scale(i), light_data->get_user_lightmap_slice_index(i));
		}
	}

	if (!missing_node_paths.is_empty()) {
		String missing_paths_text;
		if (missing_node_paths.size() <= 3) {
			missing_paths_text = String(", ").join(missing_node_paths);
		} else {
			missing_paths_text = vformat("%s and %d more", String(", ").join(missing_node_paths.slice(0, 3)), missing_node_paths.size() - 3);
		}
		WARN_PRINT(vformat("%s couldn't find previously baked nodes and needs a rebake (missing nodes: %s).", get_name(), missing_paths_text));
	}
}

void LightmapGI::_clear_lightmaps() {
	ERR_FAIL_COND(light_data.is_null());
	for (int i = 0; i < light_data->get_user_count(); i++) {
		Node *node = get_node_or_null(light_data->get_user_path(i));
		if (!node) {
			continue;
		}
		int instance_idx = light_data->get_user_sub_instance(i);
		if (instance_idx >= 0) {
			RID instance_id = node->call("get_bake_mesh_instance", instance_idx);
			if (instance_id.is_valid()) {
				RS::get_singleton()->instance_geometry_set_lightmap(instance_id, RID(), Rect2(), 0);
			}
		} else {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			ERR_CONTINUE(!vi);
			RS::get_singleton()->instance_geometry_set_lightmap(vi->get_instance(), RID(), Rect2(), 0);
		}
	}
}

/* Streaming */

int LightmapGI::streaming_active_loads = 0;

// Merges already-loaded atlases off the main thread. Deliberately does no resource
// loading of its own: ResourceLoader::load() called from a pool task takes the
// LOAD_THREAD_SPAWN_SINGLE path, whose token this code cannot pair with a get(),
// which leaks a reference to the texture and pins its video memory forever.
struct LightmapGI::StreamingLoadRequest {
	// Inputs, written before the task is queued.
	TypedArray<TextureLayered> light_textures;
	TypedArray<TextureLayered> shadowmask_textures;

	// Outputs, only read after the pool reports the task as completed.
	Ref<TextureLayered> combined_light;
	Ref<TextureLayered> combined_shadowmask;

	SafeFlag cancelled;
	WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
};

static real_t _lightmap_aabb_distance_to(const AABB &p_aabb, const Vector3 &p_point) {
	const Vector3 end = p_aabb.position + p_aabb.size;
	Vector3 delta;
	for (int i = 0; i < 3; i++) {
		delta[i] = MAX((real_t)0.0, MAX(p_aabb.position[i] - p_point[i], p_point[i] - end[i]));
	}
	return delta.length();
}

bool LightmapGI::_streaming_load_sync(const PackedStringArray &p_paths, TypedArray<TextureLayered> &r_textures) {
	// Main-thread only. ResourceLoader::load() takes LOAD_THREAD_FROM_CURRENT here
	// and manages its own token; from a WorkerThreadPool task it would instead take
	// LOAD_THREAD_SPAWN_SINGLE and leave a token this code cannot claim, leaking a
	// reference to the texture. Threaded loads go through the request/get pair.
	for (const String &path : p_paths) {
		Ref<TextureLayered> texture = ResourceLoader::load(path, "TextureLayered");
		if (texture.is_null()) {
			ERR_PRINT(vformat("Failed to load lightmap texture \"%s\".", path));
			r_textures.clear();
			return false;
		}
		r_textures.push_back(texture);
	}
	return true;
}

void LightmapGI::_streaming_combine_task(void *p_userdata) {
	StreamingLoadRequest *request = static_cast<StreamingLoadRequest *>(p_userdata);

	// Checked between stages: a cancelled request that the pool never got to start
	// would otherwise run the full decompress on whoever waits for it.
	if (request->cancelled.is_set()) {
		return;
	}
	request->combined_light = LightmapGIData::combine_textures(request->light_textures);

	if (request->shadowmask_textures.is_empty() || request->cancelled.is_set()) {
		return;
	}
	request->combined_shadowmask = LightmapGIData::combine_textures(request->shadowmask_textures);
}

bool LightmapGI::_streaming_is_active() const {
	return streaming_mode != STREAMING_MODE_DISABLED &&
			!Engine::get_singleton()->is_editor_hint() &&
			light_data.is_valid() &&
			light_data->is_streamable();
}

AABB LightmapGI::_streaming_get_bounds() const {
	if (streaming_bounds.has_volume()) {
		return streaming_bounds;
	}
	if (!streaming_derived_bounds_dirty) {
		return streaming_derived_bounds;
	}
	ERR_FAIL_COND_V(light_data.is_null(), AABB());

	AABB bounds;
	bool bounds_found = false;

	const AABB capture_bounds = light_data->get_capture_bounds();
	if (capture_bounds.has_volume()) {
		bounds = capture_bounds;
		bounds_found = true;
	} else if (is_inside_tree()) {
		// Bakes made without light probes carry no capture bounds, so fall back to
		// the combined extents of the meshes this lightmap actually lights.
		const Transform3D to_local = get_global_transform().affine_inverse();
		for (int i = 0; i < light_data->get_user_count(); i++) {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(get_node_or_null(light_data->get_user_path(i)));
			if (!vi) {
				continue;
			}
			const AABB user_bounds = (to_local * vi->get_global_transform()).xform(vi->get_aabb());
			if (bounds_found) {
				bounds.merge_with(user_bounds);
			} else {
				bounds = user_bounds;
				bounds_found = true;
			}
		}
	}

	// Cached unconditionally, including when nothing could be derived. Retrying
	// would mean resolving every user node again on every single frame, and the
	// inputs only change on events that invalidate this explicitly.
	streaming_derived_bounds = bounds;
	streaming_derived_bounds_dirty = false;

	if (!bounds_found) {
		WARN_PRINT(vformat("%s cannot derive streaming bounds: the bake produced no light probes and none of its baked meshes could be resolved. Distance will be measured from the node's origin; set streaming_bounds explicitly to fix this.", get_name()));
	}
	return streaming_derived_bounds;
}

void LightmapGI::_streaming_process() {
	if (streaming_retry_cooldown > 0.0) {
		streaming_retry_cooldown = MAX(0.0, streaming_retry_cooldown - get_process_delta_time());
	}

	// Collect a finished load first so that a decision taken below acts on the
	// state this lightmap is actually in.
	if (streaming_state == STREAMING_STATE_LOADING) {
		if (streaming_request == nullptr) {
			// Still waiting on ResourceLoader.
			if (_streaming_poll_requests()) {
				_streaming_complete_load();
			}
		} else if (WorkerThreadPool::get_singleton()->is_task_completed(streaming_request->task_id)) {
			// The multi-atlas merge finished.
			_streaming_complete_load();
		}
	}

	// Everything below decides what residency *should* be, which only makes sense
	// while this lightmap is still streaming. Collecting the load above does not.
	if (!_streaming_is_active()) {
		return;
	}

	if (streaming_mode == STREAMING_MODE_CAMERA_DISTANCE) {
		const Camera3D *camera = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
		if (camera) {
			const AABB bounds = get_global_transform().xform(_streaming_get_bounds());
			const real_t distance = _lightmap_aabb_distance_to(bounds, camera->get_global_transform().origin);
			if (distance <= streaming_load_distance) {
				streaming_load_wanted = true;
			} else if (distance > streaming_unload_distance) {
				streaming_load_wanted = false;
			}
			// Between the two distances the previous decision stands. That gap is what
			// keeps a camera loitering on a section border from thrashing VRAM.
		}
	}

	if (streaming_load_wanted) {
		if (streaming_state == STREAMING_STATE_RELEASED) {
			_streaming_begin_load();
		}
	} else if (streaming_state != STREAMING_STATE_RELEASED) {
		_streaming_release();
	}
}

void LightmapGI::_streaming_begin_load() {
	ERR_FAIL_COND(streaming_request != nullptr);
	ERR_FAIL_COND(light_data.is_null());

	if (streaming_active_loads >= STREAMING_MAX_CONCURRENT_LOADS || streaming_retry_cooldown > 0.0) {
		return; // Over budget or backing off after a failure, retried later.
	}

	const PackedStringArray light_paths = light_data->get_light_texture_paths();
	// Without atlases to request there is nothing to wait for, and entering the
	// loading state would just fail and back off on the next frame.
	ERR_FAIL_COND(light_paths.is_empty());

	streaming_requested_light_paths = light_paths;
	streaming_requested_shadowmask_paths = light_data->get_shadowmask_texture_paths();

	// Iterated through const references so that reading them does not detach these
	// arrays from the copy-on-write storage they share with the resource.
	const PackedStringArray &shadowmask_paths = streaming_requested_shadowmask_paths;
	for (const String &path : light_paths) {
		ResourceLoader::load_threaded_request(path, "TextureLayered");
	}
	for (const String &path : shadowmask_paths) {
		ResourceLoader::load_threaded_request(path, "TextureLayered");
	}

	streaming_state = STREAMING_STATE_LOADING;
	streaming_active_loads++;
}

bool LightmapGI::_streaming_poll_requests() const {
	// True once every request has settled, whether loaded or failed. Both outcomes
	// still have to be claimed, so the caller must always follow up with
	// _streaming_claim_requests().
	for (const String &path : streaming_requested_light_paths) {
		if (ResourceLoader::load_threaded_get_status(path) == ResourceLoader::THREAD_LOAD_IN_PROGRESS) {
			return false;
		}
	}
	for (const String &path : streaming_requested_shadowmask_paths) {
		if (ResourceLoader::load_threaded_get_status(path) == ResourceLoader::THREAD_LOAD_IN_PROGRESS) {
			return false;
		}
	}
	return true;
}

void LightmapGI::_streaming_claim_requests(TypedArray<TextureLayered> &r_light, TypedArray<TextureLayered> &r_shadowmask) {
	// Every requested path is claimed, including on failure and cancellation. This
	// is what releases the loader's user token; skipping one keeps the token, and
	// with it a reference to the texture, alive for the rest of the session.
	// Iterated through const references to avoid detaching the copy-on-write
	// storage these arrays share with the resource.
	const PackedStringArray &light_paths = streaming_requested_light_paths;
	const PackedStringArray &shadowmask_paths = streaming_requested_shadowmask_paths;

	bool light_failed = false;
	for (const String &path : light_paths) {
		Ref<TextureLayered> texture = ResourceLoader::load_threaded_get(path);
		if (texture.is_null()) {
			ERR_PRINT(vformat("Failed to stream in lightmap texture \"%s\".", path));
			// Keep claiming the rest before giving up, so no token is left behind.
			light_failed = true;
			continue;
		}
		r_light.push_back(texture);
	}
	if (light_failed) {
		// A partial atlas set would be worse than none: slice indices would no
		// longer line up with what the meshes were baked against.
		r_light.clear();
	}
	for (const String &path : shadowmask_paths) {
		Ref<TextureLayered> texture = ResourceLoader::load_threaded_get(path);
		if (texture.is_valid()) {
			r_shadowmask.push_back(texture);
		}
	}

	streaming_requested_light_paths = PackedStringArray();
	streaming_requested_shadowmask_paths = PackedStringArray();
}

void LightmapGI::_streaming_complete_load() {
	TypedArray<TextureLayered> light_textures;
	TypedArray<TextureLayered> shadowmask_textures;
	Ref<TextureLayered> combined_light;
	Ref<TextureLayered> combined_shadowmask;

	if (streaming_request == nullptr) {
		// Resource phase just finished. Claim everything that was requested.
		_streaming_claim_requests(light_textures, shadowmask_textures);

		if (light_textures.size() > 1) {
			// Several atlases have to be merged, and merging reads every layer back
			// from the GPU. Hand that to a worker and pick it up on a later frame.
			StreamingLoadRequest *request = memnew(StreamingLoadRequest);
			request->light_textures = light_textures;
			request->shadowmask_textures = shadowmask_textures;
			request->task_id = WorkerThreadPool::get_singleton()->add_native_task(&LightmapGI::_streaming_combine_task, request, false, vformat("LightmapGI atlas merge (%s)", get_name()));
			streaming_request = request;
			return; // Still loading; collected on a later frame.
		}

		combined_light = LightmapGIData::combine_textures(light_textures);
		combined_shadowmask = LightmapGIData::combine_textures(shadowmask_textures);
	} else {
		StreamingLoadRequest *request = streaming_request;
		streaming_request = nullptr;

		// Required even for an already finished task: this is what releases it.
		WorkerThreadPool::get_singleton()->wait_for_task_completion(request->task_id);

		light_textures = request->light_textures;
		shadowmask_textures = request->shadowmask_textures;
		combined_light = request->combined_light;
		combined_shadowmask = request->combined_shadowmask;
		memdelete(request);
	}

	streaming_active_loads--;

	if (light_data.is_valid() && combined_light.is_valid()) {
		light_data->restore_textures(light_textures, combined_light, shadowmask_textures, combined_shadowmask);
		streaming_state = STREAMING_STATE_RESIDENT;
		if (is_inside_tree()) {
			_assign_lightmaps();
		}
		emit_signal(SNAME("streaming_loaded"));
		streaming_retry_cooldown = 0.0;
	} else {
		// Errors were already reported while claiming. Back off before trying again:
		// this lightmap is still wanted, so without a cooldown the next frame would
		// start another load, and a persistent failure becomes a load storm.
		streaming_state = STREAMING_STATE_RELEASED;
		streaming_retry_cooldown = STREAMING_RETRY_COOLDOWN;
	}
}

void LightmapGI::_streaming_abort_load() {
	if (streaming_state != STREAMING_STATE_LOADING) {
		return;
	}

	if (streaming_request != nullptr) {
		StreamingLoadRequest *request = streaming_request;
		streaming_request = nullptr;

		request->cancelled.set();
		WorkerThreadPool::get_singleton()->wait_for_task_completion(request->task_id);
		memdelete(request);
	} else {
		// Outstanding ResourceLoader requests still have to be claimed even though
		// the result is thrown away, otherwise their tokens, and the textures they
		// reference, stay alive for the rest of the session.
		TypedArray<TextureLayered> discarded_light;
		TypedArray<TextureLayered> discarded_shadowmask;
		_streaming_claim_requests(discarded_light, discarded_shadowmask);
	}

	streaming_active_loads--;
	streaming_state = STREAMING_STATE_RELEASED;
}

void LightmapGI::_streaming_release() {
	_streaming_abort_load();

	if (light_data.is_null() || !light_data->are_textures_resident()) {
		streaming_state = STREAMING_STATE_RELEASED;
		return;
	}

	// Unbind the meshes before the textures go away. A vacated renderer slot is
	// refilled with an all-white default array, so any geometry still pointing at
	// this lightmap would render blown out for as long as it stayed bound.
	if (is_inside_tree()) {
		_clear_lightmaps();
	}
	light_data->release_textures();

	if (light_data->are_textures_resident()) {
		// release_textures() refused and reported why. Rebind, and do not claim a
		// release that did not happen: reporting freed memory that is still held
		// makes the leak look like a measurement problem instead of a real one.
		if (is_inside_tree()) {
			_assign_lightmaps();
		}
		return;
	}

	streaming_state = STREAMING_STATE_RELEASED;
	emit_signal(SNAME("streaming_unloaded"));
}

void LightmapGI::_streaming_force_resident() {
	_streaming_abort_load();

	if (light_data.is_valid() && !light_data->are_textures_resident()) {
		// Only reachable by explicitly opting out of streaming, so blocking here is
		// acceptable in exchange for the textures being back before this returns.
		TypedArray<TextureLayered> light_textures;
		TypedArray<TextureLayered> shadowmask_textures;

		if (_streaming_load_sync(light_data->get_light_texture_paths(), light_textures)) {
			_streaming_load_sync(light_data->get_shadowmask_texture_paths(), shadowmask_textures);

			Ref<TextureLayered> combined_light = LightmapGIData::combine_textures(light_textures);
			if (combined_light.is_valid()) {
				light_data->restore_textures(light_textures, combined_light, shadowmask_textures, LightmapGIData::combine_textures(shadowmask_textures));
				if (is_inside_tree()) {
					_assign_lightmaps();
				}
			}
		}
	}

	// A failed load leaves the textures released, and the state has to say so.
	streaming_state = (light_data.is_null() || light_data->are_textures_resident())
			? STREAMING_STATE_RESIDENT
			: STREAMING_STATE_RELEASED;
}

void LightmapGI::_streaming_refresh() {
	if (!is_inside_tree()) {
		return;
	}

	if (_streaming_is_active()) {
		streaming_derived_bounds_dirty = true;
		set_process_internal(true);
		if (streaming_state == STREAMING_STATE_RESIDENT && !streaming_load_wanted) {
			// Scene loading already paid for these textures; drop them and let the
			// streaming policy decide when they are worth paying for again.
			_streaming_release();
		}
	} else {
		set_process_internal(false);
		_streaming_force_resident();
	}
}

void LightmapGI::set_streaming_mode(StreamingMode p_mode) {
	ERR_FAIL_INDEX(p_mode, STREAMING_MODE_CAMERA_DISTANCE + 1);
	if (streaming_mode == p_mode) {
		return;
	}
	streaming_mode = p_mode;
	streaming_load_wanted = false;
	_streaming_refresh();
	notify_property_list_changed();
}

LightmapGI::StreamingMode LightmapGI::get_streaming_mode() const {
	return streaming_mode;
}

void LightmapGI::set_streaming_load_distance(float p_distance) {
	streaming_load_distance = MAX(0.0f, p_distance);
	streaming_unload_distance = MAX(streaming_unload_distance, streaming_load_distance);
}

float LightmapGI::get_streaming_load_distance() const {
	return streaming_load_distance;
}

void LightmapGI::set_streaming_unload_distance(float p_distance) {
	// Must stay at or above the load distance, otherwise the hysteresis band
	// inverts and the lightmap oscillates every frame.
	streaming_unload_distance = MAX(p_distance, streaming_load_distance);
}

float LightmapGI::get_streaming_unload_distance() const {
	return streaming_unload_distance;
}

void LightmapGI::set_streaming_bounds(const AABB &p_bounds) {
	streaming_bounds = p_bounds;
	streaming_derived_bounds_dirty = true;
	update_gizmos();
}

AABB LightmapGI::get_streaming_bounds() const {
	return streaming_bounds;
}

void LightmapGI::request_streaming_load() {
	// In STREAMING_MODE_CAMERA_DISTANCE this only holds until the next distance
	// evaluation, which stays authoritative for that mode.
	streaming_load_wanted = true;
	if (_streaming_is_active() && streaming_state == STREAMING_STATE_RELEASED) {
		_streaming_begin_load();
	}
}

void LightmapGI::request_streaming_unload() {
	streaming_load_wanted = false;
	if (_streaming_is_active() && streaming_state != STREAMING_STATE_RELEASED) {
		_streaming_release();
	}
}

bool LightmapGI::is_streaming_loaded() const {
	return streaming_state == STREAMING_STATE_RESIDENT;
}

bool LightmapGI::is_streaming_load_pending() const {
	return streaming_state == STREAMING_STATE_LOADING;
}

void LightmapGI::set_light_data(const Ref<LightmapGIData> &p_data) {
	if (light_data.is_valid()) {
		_streaming_abort_load();
		if (is_inside_tree()) {
			_clear_lightmaps();
		}
		set_base(RID());
	}
	light_data = p_data;
	// Data saved for streaming arrives with its atlases released.
	streaming_state = (light_data.is_null() || light_data->are_textures_resident())
			? STREAMING_STATE_RESIDENT
			: STREAMING_STATE_RELEASED;
	streaming_load_wanted = false;
	streaming_derived_bounds_dirty = true;

	if (light_data.is_valid()) {
		set_base(light_data->get_rid());
		if (is_inside_tree() && !_streaming_is_active()) {
			_assign_lightmaps();
		}
		light_data->update_shadowmask_mode(shadowmask_mode);
	}

	_streaming_refresh();
	update_gizmos();
}

Ref<LightmapGIData> LightmapGI::get_light_data() const {
	return light_data;
}

void LightmapGI::set_bake_quality(BakeQuality p_quality) {
	bake_quality = p_quality;
}

LightmapGI::BakeQuality LightmapGI::get_bake_quality() const {
	return bake_quality;
}

AABB LightmapGI::get_aabb() const {
	return AABB();
}

void LightmapGI::set_use_denoiser(bool p_enable) {
	use_denoiser = p_enable;
	notify_property_list_changed();
}

bool LightmapGI::is_using_denoiser() const {
	return use_denoiser;
}

void LightmapGI::set_denoiser_strength(float p_denoiser_strength) {
	denoiser_strength = p_denoiser_strength;
}

float LightmapGI::get_denoiser_strength() const {
	return denoiser_strength;
}

void LightmapGI::set_denoiser_range(int p_denoiser_range) {
	denoiser_range = p_denoiser_range;
}

int LightmapGI::get_denoiser_range() const {
	return denoiser_range;
}

void LightmapGI::set_directional(bool p_enable) {
	directional = p_enable;
}

bool LightmapGI::is_directional() const {
	return directional;
}

void LightmapGI::set_shadowmask_mode(LightmapGIData::ShadowmaskMode p_mode) {
	shadowmask_mode = p_mode;
	if (light_data.is_valid()) {
		light_data->update_shadowmask_mode(p_mode);
	}

	update_configuration_warnings();
}

LightmapGIData::ShadowmaskMode LightmapGI::get_shadowmask_mode() const {
	return shadowmask_mode;
}

void LightmapGI::set_use_texture_for_bounces(bool p_enable) {
	use_texture_for_bounces = p_enable;
}

bool LightmapGI::is_using_texture_for_bounces() const {
	return use_texture_for_bounces;
}

void LightmapGI::set_interior(bool p_enable) {
	interior = p_enable;
}

bool LightmapGI::is_interior() const {
	return interior;
}

void LightmapGI::set_environment_mode(EnvironmentMode p_mode) {
	environment_mode = p_mode;
	notify_property_list_changed();
}

LightmapGI::EnvironmentMode LightmapGI::get_environment_mode() const {
	return environment_mode;
}

void LightmapGI::set_environment_custom_sky(const Ref<Sky> &p_sky) {
	environment_custom_sky = p_sky;
}

Ref<Sky> LightmapGI::get_environment_custom_sky() const {
	return environment_custom_sky;
}

void LightmapGI::set_environment_custom_color(const Color &p_color) {
	environment_custom_color = p_color;
}

Color LightmapGI::get_environment_custom_color() const {
	return environment_custom_color;
}

void LightmapGI::set_environment_custom_energy(float p_energy) {
	environment_custom_energy = p_energy;
}

float LightmapGI::get_environment_custom_energy() const {
	return environment_custom_energy;
}

void LightmapGI::set_bounces(int p_bounces) {
	ERR_FAIL_COND(p_bounces < 0 || p_bounces > 16);
	bounces = p_bounces;
}

int LightmapGI::get_bounces() const {
	return bounces;
}

void LightmapGI::set_bounce_indirect_energy(float p_indirect_energy) {
	ERR_FAIL_COND(p_indirect_energy < 0.0);
	bounce_indirect_energy = p_indirect_energy;
}

float LightmapGI::get_bounce_indirect_energy() const {
	return bounce_indirect_energy;
}

void LightmapGI::set_bias(float p_bias) {
	ERR_FAIL_COND(p_bias < 0.00001);
	bias = p_bias;
}

float LightmapGI::get_bias() const {
	return bias;
}

void LightmapGI::set_texel_scale(float p_multiplier) {
	ERR_FAIL_COND(p_multiplier < (0.01 - CMP_EPSILON));
	texel_scale = p_multiplier;
}

float LightmapGI::get_texel_scale() const {
	return texel_scale;
}

void LightmapGI::set_max_texture_size(int p_size) {
	ERR_FAIL_COND_MSG(p_size < 2048, vformat("The LightmapGI maximum texture size supplied (%d) is too small. The minimum allowed value is 2048.", p_size));
	ERR_FAIL_COND_MSG(p_size > 16384, vformat("The LightmapGI maximum texture size supplied (%d) is too large. The maximum allowed value is 16384.", p_size));
	max_texture_size = p_size;
}

int LightmapGI::get_max_texture_size() const {
	return max_texture_size;
}

void LightmapGI::set_supersampling_enabled(bool p_enable) {
	supersampling_enabled = p_enable;

	notify_property_list_changed();
}

bool LightmapGI::is_supersampling_enabled() const {
	return supersampling_enabled;
}

void LightmapGI::set_supersampling_factor(float p_factor) {
	ERR_FAIL_COND(p_factor < 1);

	supersampling_factor = p_factor;
}

float LightmapGI::get_supersampling_factor() const {
	return supersampling_factor;
}

void LightmapGI::set_generate_probes(GenerateProbes p_generate_probes) {
	gen_probes = p_generate_probes;
}

LightmapGI::GenerateProbes LightmapGI::get_generate_probes() const {
	return gen_probes;
}

void LightmapGI::set_camera_attributes(const Ref<CameraAttributes> &p_camera_attributes) {
	camera_attributes = p_camera_attributes;
}

Ref<CameraAttributes> LightmapGI::get_camera_attributes() const {
	return camera_attributes;
}

PackedStringArray LightmapGI::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();

	if (streaming_mode != STREAMING_MODE_DISABLED && light_data.is_valid() && !light_data->is_streamable()) {
		warnings.push_back(RTR("Streaming is enabled, but the lightmap textures are not saved as separate files and could not be reloaded after being released. Streaming will be ignored for this lightmap."));
	}

#ifdef MODULE_LIGHTMAPPER_RD_ENABLED
	if (!DisplayServer::get_singleton()->can_create_rendering_device()) {
		warnings.push_back(vformat(RTR("Lightmaps can only be baked from a GPU that supports the RenderingDevice backends.\nYour GPU (%s) does not support RenderingDevice, as it does not support Vulkan, Direct3D 12, or Metal.\nLightmap baking will not be available on this device, although rendering existing baked lightmaps will work."), RenderingServer::get_singleton()->get_video_adapter_name()));
		return warnings;
	}

	if (shadowmask_mode != LightmapGIData::SHADOWMASK_MODE_NONE && light_data.is_valid() && !light_data->has_shadowmask_textures()) {
		warnings.push_back(RTR("The lightmap has no baked shadowmask textures. Please rebake with the Shadowmask Mode set to anything other than None."));
	}

#elif defined(ANDROID_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	warnings.push_back(vformat(RTR("Lightmaps cannot be baked on %s. Rendering existing baked lightmaps will still work."), OS::get_singleton()->get_name()));
#else
	warnings.push_back(RTR("Lightmaps cannot be baked, as the `lightmapper_rd` module was disabled at compile-time. Rendering existing baked lightmaps will still work."));
#endif

	return warnings;
}

void LightmapGI::_validate_property(PropertyInfo &p_property) const {
	if (!Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (p_property.name == "supersampling_factor") {
		if (!supersampling_enabled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "streaming_load_distance" || p_property.name == "streaming_unload_distance") {
		if (streaming_mode != STREAMING_MODE_CAMERA_DISTANCE) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "streaming_bounds") {
		if (streaming_mode != STREAMING_MODE_CAMERA_DISTANCE) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_custom_sky") {
		if (environment_mode != ENVIRONMENT_MODE_CUSTOM_SKY) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_custom_color") {
		if (environment_mode != ENVIRONMENT_MODE_CUSTOM_COLOR) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "environment_custom_energy") {
		if (environment_mode != ENVIRONMENT_MODE_CUSTOM_COLOR && environment_mode != ENVIRONMENT_MODE_CUSTOM_SKY) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "denoiser_strength") {
		if (!use_denoiser) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	} else if (p_property.name == "denoiser_range") {
		if (!use_denoiser) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
}

void LightmapGI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_light_data", "data"), &LightmapGI::set_light_data);
	ClassDB::bind_method(D_METHOD("get_light_data"), &LightmapGI::get_light_data);

	ClassDB::bind_method(D_METHOD("set_streaming_mode", "mode"), &LightmapGI::set_streaming_mode);
	ClassDB::bind_method(D_METHOD("get_streaming_mode"), &LightmapGI::get_streaming_mode);

	ClassDB::bind_method(D_METHOD("set_streaming_load_distance", "distance"), &LightmapGI::set_streaming_load_distance);
	ClassDB::bind_method(D_METHOD("get_streaming_load_distance"), &LightmapGI::get_streaming_load_distance);

	ClassDB::bind_method(D_METHOD("set_streaming_unload_distance", "distance"), &LightmapGI::set_streaming_unload_distance);
	ClassDB::bind_method(D_METHOD("get_streaming_unload_distance"), &LightmapGI::get_streaming_unload_distance);

	ClassDB::bind_method(D_METHOD("set_streaming_bounds", "bounds"), &LightmapGI::set_streaming_bounds);
	ClassDB::bind_method(D_METHOD("get_streaming_bounds"), &LightmapGI::get_streaming_bounds);

	ClassDB::bind_method(D_METHOD("request_streaming_load"), &LightmapGI::request_streaming_load);
	ClassDB::bind_method(D_METHOD("request_streaming_unload"), &LightmapGI::request_streaming_unload);
	ClassDB::bind_method(D_METHOD("is_streaming_loaded"), &LightmapGI::is_streaming_loaded);
	ClassDB::bind_method(D_METHOD("is_streaming_load_pending"), &LightmapGI::is_streaming_load_pending);

	ClassDB::bind_method(D_METHOD("set_bake_quality", "bake_quality"), &LightmapGI::set_bake_quality);
	ClassDB::bind_method(D_METHOD("get_bake_quality"), &LightmapGI::get_bake_quality);

	ClassDB::bind_method(D_METHOD("set_bounces", "bounces"), &LightmapGI::set_bounces);
	ClassDB::bind_method(D_METHOD("get_bounces"), &LightmapGI::get_bounces);

	ClassDB::bind_method(D_METHOD("set_bounce_indirect_energy", "bounce_indirect_energy"), &LightmapGI::set_bounce_indirect_energy);
	ClassDB::bind_method(D_METHOD("get_bounce_indirect_energy"), &LightmapGI::get_bounce_indirect_energy);

	ClassDB::bind_method(D_METHOD("set_generate_probes", "subdivision"), &LightmapGI::set_generate_probes);
	ClassDB::bind_method(D_METHOD("get_generate_probes"), &LightmapGI::get_generate_probes);

	ClassDB::bind_method(D_METHOD("set_bias", "bias"), &LightmapGI::set_bias);
	ClassDB::bind_method(D_METHOD("get_bias"), &LightmapGI::get_bias);

	ClassDB::bind_method(D_METHOD("set_environment_mode", "mode"), &LightmapGI::set_environment_mode);
	ClassDB::bind_method(D_METHOD("get_environment_mode"), &LightmapGI::get_environment_mode);

	ClassDB::bind_method(D_METHOD("set_environment_custom_sky", "sky"), &LightmapGI::set_environment_custom_sky);
	ClassDB::bind_method(D_METHOD("get_environment_custom_sky"), &LightmapGI::get_environment_custom_sky);

	ClassDB::bind_method(D_METHOD("set_environment_custom_color", "color"), &LightmapGI::set_environment_custom_color);
	ClassDB::bind_method(D_METHOD("get_environment_custom_color"), &LightmapGI::get_environment_custom_color);

	ClassDB::bind_method(D_METHOD("set_environment_custom_energy", "energy"), &LightmapGI::set_environment_custom_energy);
	ClassDB::bind_method(D_METHOD("get_environment_custom_energy"), &LightmapGI::get_environment_custom_energy);

	ClassDB::bind_method(D_METHOD("set_texel_scale", "texel_scale"), &LightmapGI::set_texel_scale);
	ClassDB::bind_method(D_METHOD("get_texel_scale"), &LightmapGI::get_texel_scale);

	ClassDB::bind_method(D_METHOD("set_max_texture_size", "max_texture_size"), &LightmapGI::set_max_texture_size);
	ClassDB::bind_method(D_METHOD("get_max_texture_size"), &LightmapGI::get_max_texture_size);

	ClassDB::bind_method(D_METHOD("set_supersampling_enabled", "enable"), &LightmapGI::set_supersampling_enabled);
	ClassDB::bind_method(D_METHOD("is_supersampling_enabled"), &LightmapGI::is_supersampling_enabled);

	ClassDB::bind_method(D_METHOD("set_supersampling_factor", "factor"), &LightmapGI::set_supersampling_factor);
	ClassDB::bind_method(D_METHOD("get_supersampling_factor"), &LightmapGI::get_supersampling_factor);

	ClassDB::bind_method(D_METHOD("set_use_denoiser", "use_denoiser"), &LightmapGI::set_use_denoiser);
	ClassDB::bind_method(D_METHOD("is_using_denoiser"), &LightmapGI::is_using_denoiser);

	ClassDB::bind_method(D_METHOD("set_denoiser_strength", "denoiser_strength"), &LightmapGI::set_denoiser_strength);
	ClassDB::bind_method(D_METHOD("get_denoiser_strength"), &LightmapGI::get_denoiser_strength);

	ClassDB::bind_method(D_METHOD("set_denoiser_range", "denoiser_range"), &LightmapGI::set_denoiser_range);
	ClassDB::bind_method(D_METHOD("get_denoiser_range"), &LightmapGI::get_denoiser_range);

	ClassDB::bind_method(D_METHOD("set_interior", "enable"), &LightmapGI::set_interior);
	ClassDB::bind_method(D_METHOD("is_interior"), &LightmapGI::is_interior);

	ClassDB::bind_method(D_METHOD("set_directional", "directional"), &LightmapGI::set_directional);
	ClassDB::bind_method(D_METHOD("is_directional"), &LightmapGI::is_directional);

	ClassDB::bind_method(D_METHOD("set_shadowmask_mode", "mode"), &LightmapGI::set_shadowmask_mode);
	ClassDB::bind_method(D_METHOD("get_shadowmask_mode"), &LightmapGI::get_shadowmask_mode);

	ClassDB::bind_method(D_METHOD("set_use_texture_for_bounces", "use_texture_for_bounces"), &LightmapGI::set_use_texture_for_bounces);
	ClassDB::bind_method(D_METHOD("is_using_texture_for_bounces"), &LightmapGI::is_using_texture_for_bounces);

	ClassDB::bind_method(D_METHOD("set_camera_attributes", "camera_attributes"), &LightmapGI::set_camera_attributes);
	ClassDB::bind_method(D_METHOD("get_camera_attributes"), &LightmapGI::get_camera_attributes);

	//	ClassDB::bind_method(D_METHOD("bake", "from_node"), &LightmapGI::bake, DEFVAL(Variant()));

	ADD_GROUP("Tweaks", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "quality", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra"), "set_bake_quality", "get_bake_quality");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "supersampling"), "set_supersampling_enabled", "is_supersampling_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "supersampling_factor", PROPERTY_HINT_RANGE, "1,8,1"), "set_supersampling_factor", "get_supersampling_factor");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bounces", PROPERTY_HINT_RANGE, "0,6,1,or_greater"), "set_bounces", "get_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bounce_indirect_energy", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_bounce_indirect_energy", "get_bounce_indirect_energy");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "directional"), "set_directional", "is_directional");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shadowmask_mode", PROPERTY_HINT_ENUM, "None,Replace,Overlay"), "set_shadowmask_mode", "get_shadowmask_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_texture_for_bounces"), "set_use_texture_for_bounces", "is_using_texture_for_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "interior"), "set_interior", "is_interior");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_denoiser"), "set_use_denoiser", "is_using_denoiser");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "denoiser_strength", PROPERTY_HINT_RANGE, "0.001,0.2,0.001,or_greater"), "set_denoiser_strength", "get_denoiser_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "denoiser_range", PROPERTY_HINT_RANGE, "1,20"), "set_denoiser_range", "get_denoiser_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bias", PROPERTY_HINT_RANGE, "0.00001,0.1,0.00001,or_greater"), "set_bias", "get_bias");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "texel_scale", PROPERTY_HINT_RANGE, "0.01,100.0,0.01"), "set_texel_scale", "get_texel_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_texture_size", PROPERTY_HINT_RANGE, "2048,16384,1"), "set_max_texture_size", "get_max_texture_size");
	ADD_GROUP("Environment", "environment_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "environment_mode", PROPERTY_HINT_ENUM, "Disabled,Scene,Custom Sky,Custom Color"), "set_environment_mode", "get_environment_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment_custom_sky", PROPERTY_HINT_RESOURCE_TYPE, Sky::get_class_static()), "set_environment_custom_sky", "get_environment_custom_sky");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "environment_custom_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_environment_custom_color", "get_environment_custom_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "environment_custom_energy", PROPERTY_HINT_RANGE, "0,64,0.01"), "set_environment_custom_energy", "get_environment_custom_energy");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "camera_attributes", PROPERTY_HINT_RESOURCE_TYPE, "CameraAttributesPractical,CameraAttributesPhysical"), "set_camera_attributes", "get_camera_attributes");
	ADD_GROUP("Gen Probes", "generate_probes_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "generate_probes_subdiv", PROPERTY_HINT_ENUM, "Disabled,4,8,16,32"), "set_generate_probes", "get_generate_probes");
	ADD_GROUP("Streaming", "streaming_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "streaming_mode", PROPERTY_HINT_ENUM, "Disabled,Manual,Camera Distance"), "set_streaming_mode", "get_streaming_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streaming_load_distance", PROPERTY_HINT_RANGE, "0,4096,0.1,or_greater,suffix:m"), "set_streaming_load_distance", "get_streaming_load_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "streaming_unload_distance", PROPERTY_HINT_RANGE, "0,4096,0.1,or_greater,suffix:m"), "set_streaming_unload_distance", "get_streaming_unload_distance");
	ADD_PROPERTY(PropertyInfo(Variant::AABB, "streaming_bounds"), "set_streaming_bounds", "get_streaming_bounds");
	ADD_GROUP("Data", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "light_data", PROPERTY_HINT_RESOURCE_TYPE, LightmapGIData::get_class_static()), "set_light_data", "get_light_data");

	ADD_SIGNAL(MethodInfo("streaming_loaded"));
	ADD_SIGNAL(MethodInfo("streaming_unloaded"));

	BIND_ENUM_CONSTANT(BAKE_QUALITY_LOW);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_MEDIUM);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_HIGH);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_ULTRA);

	BIND_ENUM_CONSTANT(GENERATE_PROBES_DISABLED);
	BIND_ENUM_CONSTANT(GENERATE_PROBES_SUBDIV_4);
	BIND_ENUM_CONSTANT(GENERATE_PROBES_SUBDIV_8);
	BIND_ENUM_CONSTANT(GENERATE_PROBES_SUBDIV_16);
	BIND_ENUM_CONSTANT(GENERATE_PROBES_SUBDIV_32);

	BIND_ENUM_CONSTANT(BAKE_ERROR_OK);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SCENE_ROOT);
	BIND_ENUM_CONSTANT(BAKE_ERROR_FOREIGN_DATA);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_LIGHTMAPPER);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SAVE_PATH);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_MESHES);
	BIND_ENUM_CONSTANT(BAKE_ERROR_MESHES_INVALID);
	BIND_ENUM_CONSTANT(BAKE_ERROR_CANT_CREATE_IMAGE);
	BIND_ENUM_CONSTANT(BAKE_ERROR_USER_ABORTED);
	BIND_ENUM_CONSTANT(BAKE_ERROR_TEXTURE_SIZE_TOO_SMALL);
	BIND_ENUM_CONSTANT(BAKE_ERROR_LIGHTMAP_TOO_SMALL);
	BIND_ENUM_CONSTANT(BAKE_ERROR_ATLAS_TOO_SMALL);

	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_DISABLED);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_SCENE);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_CUSTOM_SKY);
	BIND_ENUM_CONSTANT(ENVIRONMENT_MODE_CUSTOM_COLOR);

	BIND_ENUM_CONSTANT(STREAMING_MODE_DISABLED);
	BIND_ENUM_CONSTANT(STREAMING_MODE_MANUAL);
	BIND_ENUM_CONSTANT(STREAMING_MODE_CAMERA_DISTANCE);
}

LightmapGI::LightmapGI() {
}

LightmapGI::~LightmapGI() {
	_streaming_abort_load();
}
