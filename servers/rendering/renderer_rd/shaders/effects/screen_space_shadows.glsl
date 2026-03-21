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

	if (any(greaterThanEqual(group_start, params.screen_size)) &&
			any(greaterThanEqual(group_end, params.screen_size))) {
		return;
	}

	// swap
	if (reverse_direction) {
		vec2 temp = group_start;
		group_start = group_end;
		group_end = temp;
	}

	// Flip direction if light rays are diverging
	vec2 group_delta = group_end - group_start;
	vec2 pixel_pos = mix(group_start, group_end, float(thread_id) / WAVE_SIZE);

	// Should I keep the minux 1?
	float pixel_distance = !reverse_direction ? axis_start - thread_id : axis_start - (WAVE_SIZE - 1) + thread_id;

	const float direction = reverse_direction ? 1.0 : -1.0;

	float sampling_depth[READ_COUNT];
	float shadowing_depth[READ_COUNT];
	float sample_distance[READ_COUNT];
	float depth_thickness_scale[READ_COUNT];
	const float z_sign = -1.0;
	const float near = 1.0;
	const float far = 0.0;
	const float surface_thickness = 0.005;

	vec2 xy_offset = vec2(0, 0);
	for (int i = 0; i < READ_COUNT; i++) {
		float depth = texelFetch(depth_buffer, ivec2(pixel_pos + xy_offset), 0).x;
		shadowing_depth[i] = depth;
		sampling_depth[i] = depth;
		sample_distance[i] = pixel_distance + (WAVE_SIZE * i) * direction;
		depth_thickness_scale[i] = abs(far - depth);

		float stored_depth = (shadowing_depth[i] - light.z) / sample_distance[i];
		if (i != 0) {
			stored_depth = sample_distance[i] > 0.0 ? stored_depth : 1e10;
		}

		int idx = (i * WAVE_SIZE) + thread_id;
		DepthData[idx] = stored_depth;
		xy_offset += group_delta;
	}

	memoryBarrierShared();
	barrier();

	float shadow = 1.0;
	float depth_scale = min(sample_distance[0] + direction, 1.0 / surface_thickness) * sample_distance[0] / depth_thickness_scale[0];

	float start_depth = sampling_depth[0];
	start_depth = (start_depth - light.z) / sample_distance[0];
	start_depth = start_depth * depth_scale - z_sign;
	int sample_index = thread_id + 1;

	for (int i = 0; i < SAMPLE_COUNT; i++) {
		float depth_delta = abs(start_depth - DepthData[sample_index + i] * depth_scale);

		// We want to find the distance of the sample that is closest to the reference depth
		shadow = min(shadow, depth_delta);
	}

	ivec2 ipixel_pos = ivec2(pixel_pos);
	imageStore(output_shadow, ipixel_pos, vec4(shadow, 0.0, 0.0, 0.0));

	vec4 line_color = vec4(0.0, 0.0, 0.0, 0.0);
	switch (params.debug_mode) {
		case 5:

			float dista = clamp(sample_distance[params.max_steps % SAMPLE_COUNT] / 100, 0.0, 1.0);
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
			float test = 0.2;
			if (light.w < 0.0f) {
				test = 0.8;
			}
			imageStore(output_debug, ipixel_pos, vec4(test, test, test, 1.0));
			break;
		case 1:
		case 0:
		default:
			float test_depth = DepthData[thread_id + params.max_steps];
			vec4 depth_debug = vec4(test_depth, test_depth, test_depth, 1.0);
			imageStore(output_debug, ipixel_pos, depth_debug);
			break;
	}
}
