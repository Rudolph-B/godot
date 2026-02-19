#[compute]

#version 450

#VERSION_DEFINES

#include "../light_data_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_hiz;
layout(set = 0, binding = 1, std140) uniform SceneData {
	mat4 projection[2];
	mat4 inv_projection[2];
	mat4 reprojection[2];
	vec4 eye_offset[2];
}
scene_data;

layout(r8, set = 0, binding = 2) uniform restrict writeonly image2D output_shadow;
layout(r8, set = 0, binding = 3) uniform restrict writeonly image2D output_mip_level;

layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D output_debug;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int mipmaps;
	int num_steps;
	vec4 light_coordinate;
	float distance_fade;
	float curve_fade_in;
	float depth_tolerance;
	bool orthogonal;
	int view_index;
	int debug_enabled;
	int debug_mode;
}
params;

#define M_PI 3.14159265359

float linearize_depth(float depth) {
	vec4 pos = vec4(0.0, 0.0, depth, 1.0);
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.z / pos.w;
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	vec4 light = params.light_coordinate;

	vec3 screen_pos;
	screen_pos.xy = pixel_pos;
	screen_pos.z = texelFetch(source_hiz, pixel_pos, 0).x; // TEST Linearize??

	vec3 screen_dir = normalize(light.xyz - screen_pos.xyz) * light.w;

	vec2 t0 = (vec2(0.0) - screen_pos.xy) / screen_dir.xy;
	vec2 t1 = (vec2(1.0) - screen_pos.xy) / screen_dir.xy;
	vec2 t2 = max(t0, t1);
	float t_max = min(t2.x, t2.y);

	float ref_depth = screen_pos.z;
	float shadow = 1.0;

	for (int i = 1; i < 64; i++) {
		ivec2 xy_offset = ivec2(screen_dir.xy * i);
		float depth_offset = screen_dir.z * i;

		float expected_depth = linearize_depth(ref_depth + depth_offset);
		float measured_depth = linearize_depth(texelFetch(source_hiz, pixel_pos + xy_offset, 0).x);

		float diff = measured_depth - expected_depth;

		// Break if diff is half of depth_tolerance
		if (diff > 0.1 && diff < params.depth_tolerance) {
			shadow = 0.0;
			break;
		}
	}

	vec4 depth_color = vec4(screen_pos.z, screen_pos.z, screen_pos.z, 0.0);
	imageStore(output_shadow, pixel_pos, vec4(shadow, 0.0, 0.0, 0.0));
	imageStore(output_mip_level, pixel_pos, depth_color);

	vec4 line_color = vec4(0.0, 0.0, 0.0, 0.0);
	switch (params.debug_mode) {
		case 2: {
			ivec2 i = params.screen_size.xy / 2;
			vec2 p = pixel_pos - i;

			vec2 d = normalize(light.xy - i) * light.w;
			//				d.y = -d.y;
			bool p_sign = (sign(p.x) == sign(d.x) && sign(p.y) == sign(d.y));
			if (floor(p.x * d.y) == floor(p.y * d.x) && p_sign) {
				line_color.g = 1.0;
				line_color.a = 1.0;
			}
		}

			{
				ivec2 i = ivec2(500, 400);
				vec2 p = pixel_pos - i;

				vec2 d = normalize(light.xy - i) * light.w;

				bool p_sign = (sign(p.x) == sign(d.x) && sign(p.y) == sign(d.y));
				if (floor(p.x * d.y) == floor(p.y * d.x) && p_sign) {
					line_color.g = 1.0;
					line_color.a = 1.0;
				}
			}

			{
				ivec2 i = ivec2(800, 400);
				vec2 p = pixel_pos - i;
				vec2 d = normalize(light.xy - i) * light.w;

				bool p_sign = (sign(p.x) == sign(d.x) && sign(p.y) == sign(d.y));
				if (floor(p.x * d.y) == floor(p.y * d.x) && p_sign) {
					line_color.g = 1.0;
					line_color.a = 1.0;
				}
			}

			if (ivec2(light.xy) == pixel_pos) {
				line_color.b = 1.0;
				line_color.a = 1.0;
			}
			imageStore(output_debug, pixel_pos, line_color);
			break;
		case 1:
			imageStore(output_debug, pixel_pos, vec4(shadow, shadow, shadow, shadow));
			break;
		case 0:
		default:
			imageStore(output_debug, pixel_pos, depth_color);
			break;
	}
}
