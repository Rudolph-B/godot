#[compute]

#version 450

#VERSION_DEFINES

#include "../light_data_inc.glsl"

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_buffer;
layout(set = 0, binding = 1, std140) uniform SceneData {
	mat4 projection[2];
	mat4 inv_projection[2];
	mat4 reprojection[2];
	vec4 eye_offset[2];
}
scene_data;

layout(r8, set = 0, binding = 2) uniform restrict writeonly image2D output_shadow;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D output_debug;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	ivec2 light_offset;
	vec4 light_coordinate;
	float min;
	float max;
	float depth_tolerance;
	int max_steps;
	int view_index;
	int debug_enabled;
	int debug_mode;
}
params;

#define M_PI 3.14159265359
#define WAVE_SIZE 64
#define SAMPLE_COUNT 60
#define READ_COUNT (SAMPLE_COUNT / WAVE_SIZE + 2) // 2

shared float DepthData[READ_COUNT * WAVE_SIZE];

float linearize_depth(float depth) {
	vec4 pos = vec4(0.0, 0.0, depth, 1.0);
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.z / pos.w;
}

void main() {
	ivec2 group_offset = ivec2(gl_WorkGroupID.yz);
	int group_id = int(gl_WorkGroupID.x);
	int thread_id = int(gl_LocalInvocationID.x);
	vec4 light = params.light_coordinate;

	vec2 light_xy = floor(light.xy) + 0.5;
	vec2 light_xy_fraction = light.xy - light_xy;
	bool reverse_direction = light.w < 0.0f;

	ivec2 grid_offset = group_offset * WAVE_SIZE + params.light_offset;
	vec2 group_dir;
	float axis_start = 0.0;
	bool x_major_axis = false;

	if (grid_offset.y <= grid_offset.x && grid_offset.x > -grid_offset.y) {
		// East of light -> major axis direction down
		group_dir = vec2(0.0, -1.0);
		axis_start = abs(grid_offset.x) + light_xy_fraction.x;
		x_major_axis = false;
	} else if (grid_offset.x <= -grid_offset.y && grid_offset.x > grid_offset.y) {
		// South of light -> major axis direction left
		group_dir = vec2(-1.0, 0.0);
		axis_start = abs(grid_offset.y) + light_xy_fraction.y;
		x_major_axis = true;
	} else if (grid_offset.x <= grid_offset.y && -grid_offset.x > grid_offset.y) {
		// West of light -> major axis direction up
		group_dir = vec2(0.0, 1.0);
		axis_start = abs(grid_offset.x) + light_xy_fraction.x;
		x_major_axis = false;
	} else if (-grid_offset.x <= grid_offset.y && grid_offset.x < grid_offset.y) {
		// North of light -> major axis direction right
		group_dir = vec2(1.0, 0.0);
		axis_start = abs(grid_offset.y) + light_xy_fraction.y;
		x_major_axis = true;
	} else {
		return;
	}

	vec2 group_start = grid_offset + light_xy + group_dir * group_id;
	vec2 group_end = mix(light.xy, group_start, max((axis_start - WAVE_SIZE), 0.0) / axis_start);

	// swap
	if (reverse_direction) {
		vec2 temp = group_start;
		group_start = group_end;
		group_end = temp;
	}

	// Flip direction if light rays are diverging
	vec2 group_delta = group_end - group_start;
	vec2 pixel_pos = mix(group_start, group_end, float(thread_id) / WAVE_SIZE);

	float pixel_distance = !reverse_direction ? axis_start - thread_id : axis_start + thread_id;
	//	shadow = mix(1.0, shadow, directional_lights.data[i].shadow_opacity);
	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	const float direction = reverse_direction ? 1.0 : -1.0;

	float shadowing_depth[READ_COUNT];
	float sample_distance[READ_COUNT];
	float depth_thickness_scale[READ_COUNT];

	vec2 xy_offset = vec2(0, 0);
	for (int i = 0; i < READ_COUNT; i++) {
		shadowing_depth[i] = texelFetch(depth_buffer, ivec2(pixel_pos + xy_offset), 0).x;
		sample_distance[i] = pixel_distance - (WAVE_SIZE * i) * direction;

		int idx = (i * WAVE_SIZE) + thread_id;
		DepthData[idx] = linearize_depth(shadowing_depth[i]);
		xy_offset += group_delta;
	}

	memoryBarrierShared();
	barrier();

	vec3 screen_pos;
	screen_pos.xy = pixel_pos;
	screen_pos.z = shadowing_depth[0];

	vec3 screen_dir = light.w * normalize(light.xyz - screen_pos.xyz);

	if (x_major_axis) {
		screen_dir = screen_dir / abs(screen_dir.y);
	} else {
		screen_dir = screen_dir / abs(screen_dir.x);
	}

	float ref_depth = screen_pos.z;
	float shadow = 1.0;

	for (int i = 1; i < 40; i++) {
		ivec2 xy_offset = ivec2(screen_dir.xy * i);
		float depth_offset = screen_dir.z * i;

		float expected_depth = linearize_depth(ref_depth + depth_offset);
		//		float measured_depth = linearize_depth(texelFetch(source_hiz, ivec2(pixel_pos + xy_offset), 0).x);
		float measured_depth = DepthData[thread_id + i];

		float diff = measured_depth - expected_depth;

		// Break if diff is half of depth_tolerance
		if (diff > 0.1 && diff < params.depth_tolerance) {
			shadow = 0.0;
			break;
		}
	}

	ivec2 ipixel_pos = ivec2(pixel_pos);
	imageStore(output_shadow, ipixel_pos, vec4(shadow, 0.0, 0.0, 0.0));

	vec4 line_color = vec4(0.0, 0.0, 0.0, 0.0);
	switch (params.debug_mode) {
		case 5:

			float dista = clamp(abs(1) / 100000.0, 0.0, 1.0);
			imageStore(output_debug, ipixel_pos, vec4(dista, dista, dista, 1.0));
			break;

		case 4:

			float group = gl_WorkGroupID.x / 63.0;
			imageStore(output_debug, ipixel_pos, vec4(group, group, group, 1.0));
			break;
		case 3:
			imageStore(output_debug, ipixel_pos, vec4(shadow, shadow, shadow, shadow));
			break;
		case 2:
			// Individual
			float test_depth2 = abs(linearize_depth(texelFetch(depth_buffer, ivec2(pixel_pos + screen_dir.xy * params.max_steps), 0).x));
			test_depth2 -= abs(DepthData[thread_id + params.max_steps]);
			test_depth2 = abs(test_depth2);
			if (test_depth2 < 0.001) {
				test_depth2 = 1.0;
			}
			//			test_depth2 = min(test_depth2, abs(linearize_depth(ref_depth) / 200.0));
			vec4 depth_debug2 = vec4(test_depth2, test_depth2, test_depth2, 1.0);
			imageStore(output_debug, ipixel_pos, depth_debug2);
			break;
		case 1:
			// Shared
			float test_depth1 = abs(linearize_depth(texelFetch(depth_buffer, ivec2(pixel_pos + screen_dir.xy * params.max_steps), 0).x)) / 200.0;
			vec4 depth_debug1 = vec4(test_depth1, test_depth1, test_depth1, 1.0);
			imageStore(output_debug, ipixel_pos, depth_debug1);
			break;
		case 0:
		default:
			float test_depth = abs(DepthData[thread_id + params.max_steps]) / 200.0;
			vec4 depth_debug = vec4(test_depth, test_depth, test_depth, 1.0);
			imageStore(output_debug, ipixel_pos, depth_debug);
			break;
	}
}
