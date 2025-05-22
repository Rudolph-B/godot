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

#pragma once

#include "core/math/projection.h"
#include "core/templates/local_vector.h"
#include "servers/rendering_server.h"

class RendererSceneOcclusionCull {
protected:
	static RendererSceneOcclusionCull *singleton;

public:
	class HZBuffer {
	protected:
		LocalVector<float> data;
		LocalVector<Size2i> sizes;
		LocalVector<float *> mips;

		RID debug_texture;
		Ref<Image> debug_image;
		PackedByteArray debug_texture_data;
		LocalVector<float> debug_data;
		float debug_tex_range = 0.0f;

		uint64_t occlusion_frame = 0;
		Size2i occlusion_buffer_size;

		_FORCE_INLINE_ bool _is_occluded(const real_t p_bounds[6], const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, real_t p_near) {
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

			float min_depth = FLT_MAX;
			int mi = 0;

			Vector2 rect_min = Vector2(FLT_MAX, FLT_MAX);
			Vector2 rect_max = Vector2(FLT_MIN, FLT_MIN);
			Vector2 cs_proj[8];
			float cs_depth[8];

			for (int j = 0; j < 8; j++) {
				Vector3 corner = Vector3(
						p_bounds[0] * (j >> 0 & 1) + p_bounds[3] * (~j >> 0 & 1),
						p_bounds[1] * (j >> 1 & 1) + p_bounds[4] * (~j >> 1 & 1),
						p_bounds[2] * (j >> 2 & 1) + p_bounds[5] * (~j >> 2 & 1));
				Vector3 view = p_cam_inv_transform.xform(corner);
				cs_depth[j] = -view.z;

				if (-view.z < min_depth) {
					min_depth = -view.z;
					mi = j;
				}

				Vector3 projected = p_cam_projection.xform(view);
				cs_proj[j] = Vector2(projected.x * 0.5f + 0.5f, projected.y * 0.5f + 0.5f);

				rect_min = rect_min.min(cs_proj[j]);
				rect_max = rect_max.max(cs_proj[j]);
			}

			Vector3 vn[3];
			Vector3 cn[3];
			Vector3 pn[3];
			float adj = 1.0;
			float off = 0.05;
			if (!p_cam_projection.is_orthogonal()) {
				adj *= 0.9;
			}

			for (int i = 0; i < 3; ++i) {
				int j = mi ^ (1 << i);
				vn[i] = Vector3(
						cs_proj[j].x - cs_proj[mi].x,
						cs_proj[j].y - cs_proj[mi].y,
						adj * (cs_depth[j] - min_depth));
			}

			int p = 0;
			for (int i = 0; i < 3; ++i) {
				cn[i] = vn[(i + 1) % 3].cross(vn[(i + 2) % 3]);
				if (Math::abs(cn[p].z) < Math::abs(cn[i].z)) {
					p = i;
				}
			}

			for (int i = 0; i < 3; ++i) {
				if (cn[p].z * cn[i].z <= 0.0001) {
					pn[i] = Vector3(0.0, 0.0, 0.0);
				} else {
					pn[i] = Vector3(
							-cn[i].x / cn[i].z,
							-cn[i].y / cn[i].z,
							min_depth - off + (cn[i].x / cn[i].z) * cs_proj[mi].x + (cn[i].y / cn[i].z) * cs_proj[mi].y);
				}
			}

			rect_max = rect_max.minf(1);
			rect_min = rect_min.maxf(0);

			int mip_count = mips.size();

			Vector2 screen_diagonal = (rect_max - rect_min) * sizes[0];
			float size = MAX(screen_diagonal.x, screen_diagonal.y);
			float l = Math::ceil(Math::log2(size));
			int lod = CLAMP(l, 0, mip_count - 1);

			// const int max_samples = 512;
			// int sample_count = 0;
			bool visible = true;

			for (; lod >= 0; lod--) {
				int w = sizes[lod].x;
				int h = sizes[lod].y;

				int minx = CLAMP(rect_min.x * w - 1, 0, w - 1);
				int maxx = CLAMP(rect_max.x * w + 1, 0, w - 1);

				int miny = CLAMP(rect_min.y * h - 1, 0, h - 1);
				int maxy = CLAMP(rect_max.y * h + 1, 0, h - 1);

				// sample_count += (maxx - minx + 1) * (maxy - miny + 1);

				// if (sample_count > max_samples) {
				// 	visible = true;
				// 	break;
				// }

				visible = false;
				for (int y = miny; y <= maxy; y++) {
					for (int x = minx; x <= maxx; x++) {
						float t_depth = pn[0].z + (pn[0].x * x / w) + (pn[0].y * y / h);
						t_depth = MAX(t_depth, pn[1].z + (pn[1].x * x / w) + (pn[1].y * y / h));
						t_depth = MAX(t_depth, pn[2].z + (pn[2].x * x / w) + (pn[2].y * y / h));

						float depth = mips[lod][y * w + x];
						if (depth > t_depth) {
							visible = true;
							break;
						}
					}
					if (visible) {
						break;
					}
				}

				if (!visible) {
					break;
				}
			}

			{
				int w = sizes[0].x * 2;
				int h = sizes[0].y * 2;

				int minx = CLAMP(rect_min.x * w - 1, 0, w - 1);
				int maxx = CLAMP(rect_max.x * w + 1, 0, w - 1);

				int miny = CLAMP(rect_min.y * h - 1, 0, h - 1);
				int maxy = CLAMP(rect_max.y * h + 1, 0, h - 1);

				for (int y = miny; y <= maxy; y++) {
					for (int x = minx; x <= maxx; x++) {
						float t_depth = pn[0].z + (pn[0].x * x / w) + (pn[0].y * y / h);
						t_depth = MAX(t_depth, pn[1].z + (pn[1].x * x / w) + (pn[1].y * y / h));
						t_depth = MAX(t_depth, pn[2].z + (pn[2].x * x / w) + (pn[2].y * y / h));

						debug_data[y * w + x] = MAX(min_depth, t_depth);

						if (x == minx || x == maxx || y == miny || y == maxy) {
							if (visible || (x + y) % 2 == 0) {
								debug_data[y * w + x] = 0;
							}
						}
					}
				}

				int x = CLAMP(cs_proj[mi].x * w - 1, 0, w - 1);
				int y = CLAMP(cs_proj[mi].y * h - 1, 0, h - 1);
				debug_data[y * w + x] = 0;
			}

			return !visible;
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
		_FORCE_INLINE_ bool is_occluded(const real_t p_bounds[6], const Vector3 &p_cam_position, const Transform3D &p_cam_inv_transform, const Projection &p_cam_projection, real_t p_near, uint64_t &r_occlusion_timeout) {
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
