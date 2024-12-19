/**************************************************************************/
/*  renderer_scene_occlusion_cull.h                                       */
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

#ifndef RENDERER_SCENE_OCCLUSION_CULL_H
#define RENDERER_SCENE_OCCLUSION_CULL_H

#include "core/math/projection.h"
#include "core/templates/local_vector.h"
#include "servers/rendering_server.h"

#include <unistd.h>

class RendererSceneOcclusionCull {
protected:
	static RendererSceneOcclusionCull *singleton;

public:
	class HZBuffer {
	protected:
		static const Vector3 corners[8];
		static const Vector2i children[4];

		LocalVector<float> data;
		LocalVector<Size2i> sizes;
		LocalVector<float *> min_mips;
		LocalVector<float *> max_mips;

		RID debug_texture;
		Ref<Image> debug_image;
		PackedByteArray debug_data;
		float debug_tex_range = 0.0f;

		uint64_t occlusion_frame = 0;
		Size2i occlusion_buffer_size;

		_FORCE_INLINE_ bool _is_occluded(const real_t p_bounds[6], const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, real_t p_near) const {
			if (is_empty()) {
				return false;
			}

			Vector3 closest_point = p_cam_position.clamp(Vector3(p_bounds[0], p_bounds[1], p_bounds[2]), Vector3(p_bounds[3], p_bounds[4], p_bounds[5]));

			if (closest_point == p_cam_position) {
				return false;
			}

			Vector3 closest_point_view = p_cam_inv_transform.xform(closest_point);
			if (closest_point_view.z > -p_near) {
				return false;
			}

			float min_depth = (closest_point - p_cam_position).length();

			Vector2 rect_min = Vector2(FLT_MAX, FLT_MAX);
			Vector2 rect_max = Vector2(FLT_MIN, FLT_MIN);

			for (int j = 0; j < 8; j++) {
				const Vector3 &c = RendererSceneOcclusionCull::HZBuffer::corners[j];
				Vector3 nc = Vector3(1, 1, 1) - c;
				Vector3 corner = Vector3(p_bounds[0] * c.x + p_bounds[3] * nc.x, p_bounds[1] * c.y + p_bounds[4] * nc.y, p_bounds[2] * c.z + p_bounds[5] * nc.z);
				Vector3 view = p_cam_inv_transform.xform(corner);

				if (p_cam_projection.is_orthogonal()) {
					min_depth = MIN(min_depth, -view.z);
				}

				Plane vp = Plane(view, 1.0);
				Plane projected = p_cam_projection.xform4(vp);

				float w = projected.d;
				if (w < 1.0) {
					rect_min = Vector2(0.0f, 0.0f);
					rect_max = Vector2(1.0f, 1.0f);
					break;
				}

				Vector2 normalized = Vector2(projected.normal.x / w * 0.5f + 0.5f, projected.normal.y / w * 0.5f + 0.5f);
				rect_min = rect_min.min(normalized);
				rect_max = rect_max.max(normalized);
			}

			rect_max = rect_max.minf(1);
			rect_min = rect_min.maxf(0);

			// Calculate rect bounds on mip[0]
			int w_min = CLAMP(floor(rect_min.x * sizes[0].x), 0 , sizes[0].x - 1);
			int w_max = CLAMP(ceil(rect_max.x * sizes[0].x), 0 , sizes[0].x - 1);
			int h_min = CLAMP(floor(rect_min.y * sizes[0].y), 0 , sizes[0].y - 1);
			int h_max = CLAMP(ceil(rect_max.y * sizes[0].y), 0 , sizes[0].y - 1);

			int minx = w_min;
			int maxx = w_max;
			int miny = h_min;
			int maxy = h_max;

			// Calculate starting lod
			int lod = 0;
			while ((maxx - minx + 1) * (maxy - miny + 1) > 4) {
				minx = minx >> 1;
				maxx = maxx >> 1;
				miny = miny >> 1;
				maxy = maxy >> 1;
				lod++;
			}

			// Setup breadth first search memory bounds
			const int max_nodes = 512;
			Vector2i nodes[max_nodes];

			// Add initial nodes to explore
			int total_nodes = 0;
			for (int xi = minx; xi <= maxx; xi++) {
				for (int yi = miny; yi <= maxy; yi++) {
					nodes[total_nodes] = {xi,yi};
					total_nodes++;
				}
			}

			int clod_node = 0;
			int nlod_node = total_nodes;
			int w = sizes[lod].x;

			// Loop while we haven't exceeded the max node count (So that we don't take to long)
			while (true) {
				// Check if LOD is full
				if (clod_node >= nlod_node) {
					if (clod_node >= total_nodes) {
						// No more nodes to explore

						// Getting here means either
						// 1. All nodes got pruned due to them being before the object
						// 2. We checked all nodes of lod:0 without the object being visible
						// In both we can safely assume the object is occluded.
						return true;
					}

					// Go a level deeper
					nlod_node = total_nodes;
					lod--;
					minx = w_min >> lod;
					maxx = w_max >> lod;
					miny = h_min >> lod;
					maxy = h_max >> lod;
					w = sizes[lod].x;
				}

				int x = nodes[clod_node].x;
				int y = nodes[clod_node].y;
				clod_node++;

				if (x < minx || x > maxx || y < miny || y > maxy) {
					// Noce is not with the object bounds for this lod, skip...
					continue;
				}

				// Check if min_depth is visible for the current node by comparing it with the min_mips
				if (min_mips[lod][y * w + x] > min_depth) {
					return false;
				}

				// Check if we expect the nodes children to contain a solution by comparing it with the max_mips
				if (max_mips[lod][y * w + x] > min_depth) {

					if (lod == 0) {
						// At max depth no more nodes to add.
						continue;
					}

					// Only add nodes if we can go a level deeper and there is space in max_nodes
					if (total_nodes + 4 < max_nodes) {
						x *= 2;
						y *= 2;
						nodes[total_nodes++] = {x + children[0].x, y + children[0].y};
						nodes[total_nodes++] = {x + children[1].x, y + children[1].y};
						nodes[total_nodes++] = {x + children[2].x, y + children[2].y};
						nodes[total_nodes++] = {x + children[3].x, y + children[3].y};
					}
					else if (total_nodes + 4 >= max_nodes)
					{
						// Expended all of our search space exit.
						return false;
					}
				}
			}
		}

	public:
		static bool occlusion_jitter_enabled;

		bool is_empty() const;
		virtual void clear();
		virtual void resize(const Size2i &p_size);

		void update_mips();

		// Thin wrapper around _is_occluded(),
		// allowing occlusion timers to delay the disappearance
		// of objects to prevent flickering when using jittering.
		_FORCE_INLINE_ bool is_occluded(const real_t p_bounds[6], const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, real_t p_near, uint64_t &r_occlusion_timeout) const {
			bool occluded = _is_occluded(p_bounds, p_cam_position, p_cam_inv_transform, p_cam_projection, p_near);

			// Special case, temporal jitter disabled,
			// so we don't use occlusion timers.
			if (!occlusion_jitter_enabled) {
				return occluded;
			}

			if (!occluded) {
//#define DEBUG_RASTER_OCCLUSION_JITTER
#ifdef DEBUG_RASTER_OCCLUSION_JITTER
				r_occlusion_timeout = occlusion_frame + 1;
#else
				r_occlusion_timeout = occlusion_frame + 9;
#endif
			} else if (r_occlusion_timeout) {
				// Regular timeout, allow occlusion culling
				// to proceed as normal after the delay.
				if (occlusion_frame >= r_occlusion_timeout) {
					r_occlusion_timeout = 0;
				}
			}

			return occluded && !r_occlusion_timeout;
		}

		RID get_debug_texture();
		const Size2i &get_occlusion_buffer_size() const { return occlusion_buffer_size; }

		virtual ~HZBuffer() {}
	};

	static RendererSceneOcclusionCull *get_singleton() { return singleton; }

	void _print_warning() {
		WARN_PRINT_ONCE("Occlusion culling is disabled at build-time.");
	}

	virtual bool is_occluder(RID p_rid) { return false; }
	virtual RID occluder_allocate() { return RID(); }
	virtual void occluder_initialize(RID p_occluder) {}
	virtual void free_occluder(RID p_occluder) { _print_warning(); }
	virtual void occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) { _print_warning(); }

	virtual void add_scenario(RID p_scenario) {}
	virtual void remove_scenario(RID p_scenario) {}
	virtual void scenario_set_instance(RID p_scenario, RID p_instance, RID p_occluder, const Transform3D &p_xform, bool p_enabled) { _print_warning(); }
	virtual void scenario_remove_instance(RID p_scenario, RID p_instance) { _print_warning(); }

	virtual void add_buffer(RID p_buffer) { _print_warning(); }
	virtual void remove_buffer(RID p_buffer) { _print_warning(); }
	virtual HZBuffer *buffer_get_ptr(RID p_buffer) {
		return nullptr;
	}
	virtual void buffer_set_scenario(RID p_buffer, RID p_scenario) { _print_warning(); }
	virtual void buffer_set_size(RID p_buffer, const Vector2i &p_size) { _print_warning(); }
	virtual void buffer_update(RID p_buffer, const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal) {}

	virtual RID buffer_get_debug_texture(RID p_buffer) {
		_print_warning();
		return RID();
	}

	virtual void set_build_quality(RS::ViewportOcclusionCullingBuildQuality p_quality) {}

	RendererSceneOcclusionCull() {
		singleton = this;
	}

	virtual ~RendererSceneOcclusionCull() {
		singleton = nullptr;
	}
};

#endif // RENDERER_SCENE_OCCLUSION_CULL_H
