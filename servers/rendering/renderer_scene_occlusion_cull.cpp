/**************************************************************************/
/*  renderer_scene_occlusion_cull.cpp                                     */
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

#include "renderer_scene_occlusion_cull.h"

RendererSceneOcclusionCull *RendererSceneOcclusionCull::singleton = nullptr;

const Vector3 RendererSceneOcclusionCull::HZBuffer::corners[8] = {
	Vector3(0, 0, 0),
	Vector3(0, 0, 1),
	Vector3(0, 1, 0),
	Vector3(0, 1, 1),
	Vector3(1, 0, 0),
	Vector3(1, 0, 1),
	Vector3(1, 1, 0),
	Vector3(1, 1, 1)
};

const Vector2i RendererSceneOcclusionCull::HZBuffer::children[9] = {
	Vector2i(0, 0),
	Vector2i(0, 1),
	Vector2i(1, 0),
	Vector2i(1, 1)
};

bool RendererSceneOcclusionCull::HZBuffer::occlusion_jitter_enabled = false;

bool RendererSceneOcclusionCull::HZBuffer::is_empty() const {
	return sizes.is_empty();
}

void RendererSceneOcclusionCull::HZBuffer::clear() {
	if (sizes.is_empty()) {
		return; // Already cleared
	}

	data.clear();
	sizes.clear();
	min_mips.clear();
	max_mips.clear();

	debug_data.clear();
	if (debug_image.is_valid()) {
		debug_image.unref();
	}

	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free(debug_texture);
}

void RendererSceneOcclusionCull::HZBuffer::resize(const Size2i &p_size) {
	occlusion_buffer_size = p_size;

	if (p_size == Size2i()) {
		clear();
		return;
	}

	if (!sizes.is_empty() && p_size == sizes[0]) {
		return; // Size didn't change
	}

	int mip_count = 0;
	int w = p_size.x;
	int h = p_size.y;

	// Calculate datasize and mip_count
	int data_size = 0;
	while (true) {
		data_size += 2 * h * w;

		w = MAX(1, (w + 1) >> 1);
		h = MAX(1, (h + 1) >> 1);

		mip_count++;

		if (w == 1U && h == 1U) {
			data_size += 1U;
			mip_count++;
			break;
		}
	}

	// Since max_mip[0] = min_mip[0] we can subtract the first instance of h * w from data_size
	data_size -= p_size.x * p_size.y;

	// Resize the datastructures
	data.resize(data_size);
	max_mips.resize(mip_count);
	min_mips.resize(mip_count);
	sizes.resize(mip_count);

	// Populate the max_mips pointers
	w = p_size.x;
	h = p_size.y;
	float *ptr = data.ptr();
	for (int i = 0; i < mip_count; i++) {
		sizes[i] = Size2i(w, h);
		max_mips[i] = ptr;

		ptr = &ptr[w * h];
		w = MAX(1, (w + 1) >> 1);
		h = MAX(1, (h + 1) >> 1);
	}

	// Populate the min_mips pointers
	// Start at one since max_mip[0] = min_mip[0]
	min_mips[0] = max_mips[0];
	for (int i = 1; i < mip_count; i++) {
		min_mips[i] = ptr;
		ptr = &ptr[sizes[i].x * sizes[i].y];
	}

	for (int i = 0; i < data_size; i++) {
		data[i] = FLT_MAX;
	}

	debug_data.resize(sizes[0].x * sizes[0].y);
	if (debug_texture.is_valid()) {
		RS::get_singleton()->free(debug_texture);
		debug_texture = RID();
	}
}

void RendererSceneOcclusionCull::HZBuffer::update_mips() {
	// Keep this up to date as a local to be used for occlusion timers.
	occlusion_frame = Engine::get_singleton()->get_frames_drawn();

	if (sizes.is_empty()) {
		return;
	}

	for (uint32_t mip = 1; mip < sizes.size(); mip++) {
		for (int y = 0; y < sizes[mip].y; y++) {
			for (int x = 0; x < sizes[mip].x; x++) {
				int prev_x = x * 2;
				int prev_y = y * 2;

				int prev_w = sizes[mip - 1].width;
				int prev_h = sizes[mip - 1].height;

#define CHECK_MAX_OFFSET(xx, yy) max_depth = MAX(max_depth, max_mips[mip - 1][MIN(prev_h - 1, prev_y + (yy)) * prev_w + MIN(prev_w - 1, prev_x + (xx))])

				float max_depth = max_mips[mip - 1][prev_y * sizes[mip - 1].x + prev_x];
				CHECK_MAX_OFFSET(0, 1);
				CHECK_MAX_OFFSET(1, 0);
				CHECK_MAX_OFFSET(1, 1);

				max_mips[mip][y * sizes[mip].x + x] = max_depth;
#undef CHECK_MAX_OFFSET

#define CHECK_MIN_OFFSET(xx, yy) min_depth = MIN(min_depth, min_mips[mip - 1][MIN(prev_h - 1, prev_y + (yy)) * prev_w + MIN(prev_w - 1, prev_x + (xx))])

				float min_depth = min_mips[mip - 1][prev_y * sizes[mip - 1].x + prev_x];
				CHECK_MIN_OFFSET(0, 1);
				CHECK_MIN_OFFSET(1, 0);
				CHECK_MIN_OFFSET(1, 1);

				min_mips[mip][y * sizes[mip].x + x] = min_depth;
#undef CHECK_MAX_OFFSET

			}
		}
	}
}

RID RendererSceneOcclusionCull::HZBuffer::get_debug_texture() {
	if (sizes.is_empty() || sizes[0] == Size2i()) {
		return RID();
	}

	if (debug_image.is_null()) {
		debug_image.instantiate();
	}

	unsigned char *ptrw = debug_data.ptrw();
	for (int i = 0; i < debug_data.size(); i++) {
		ptrw[i] = MIN(max_mips[0][i] / debug_tex_range, 1.0) * 255;
	}

	debug_image->set_data(sizes[0].x, sizes[0].y, false, Image::FORMAT_L8, debug_data);

	if (debug_texture.is_null()) {
		debug_texture = RS::get_singleton()->texture_2d_create(debug_image);
	} else {
		RenderingServer::get_singleton()->texture_2d_update(debug_texture, debug_image);
	}

	return debug_texture;
}
