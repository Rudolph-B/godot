#[compute]

#version 450

#VERSION_DEFINES

#include "../light_data_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_hiz;
layout(set = 0, binding = 1, std140) uniform DirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
directional_lights;
layout(set = 0, binding = 2, std140) uniform SceneData {
	mat4 projection[2];
	mat4 inv_projection[2];
	mat4 reprojection[2];
	vec4 eye_offset[2];
	int directional_light_count;
}
scene_data;

layout(r8, set = 0, binding = 3) uniform restrict writeonly image2D output_shadow;
layout(r8, set = 0, binding = 4) uniform restrict writeonly image2D output_mip_level;

layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D output_debug;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int mipmaps;
	int num_steps;
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

vec3 compute_view_pos(vec3 screen_pos) {
	vec4 pos;
	pos.xy = screen_pos.xy * 2.0 - 1.0;
	pos.z = screen_pos.z;
	pos.w = 1.0;
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.xyz / pos.w;
}

vec3 compute_screen_pos(vec3 pos) {
	vec4 screen_pos = scene_data.projection[params.view_index] * vec4(pos, 1.0);
	screen_pos.xyz /= screen_pos.w;
	screen_pos.xy = screen_pos.xy * 0.5 + 0.5;
	return screen_pos.xyz;
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	//	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
	//		return;
	//	}
	vec4 projection = vec4(directional_lights.data[0].direction, 0) * scene_data.projection[0];
	vec3 light_dir = projection.xyz / projection.w;

	vec3 screen_pos;
	screen_pos.xy = vec2(pixel_pos + 0.5) / params.screen_size;
	screen_pos.z = texelFetch(source_hiz, pixel_pos, 0).x;

	vec4 depth_color = vec4(screen_pos.z, screen_pos.z, screen_pos.z, 0.0);

	float ref_depth = screen_pos.z;
	float shadow = 1.0;

	for (int i = 1; i < 64; i++) {
		ivec2 xy_offset = ivec2(light_dir.xy * i);
		float depth_offset = light_dir.z * i;

		float expected_depth = linearize_depth(ref_depth + depth_offset);
		float measured_depth = linearize_depth(texelFetch(source_hiz, pixel_pos + xy_offset, 0).x);

		float diff = measured_depth - expected_depth;
		if (diff > 0 && diff < params.depth_tolerance) {
			shadow = 0.0;
			break;
		}
	}

	imageStore(output_shadow, pixel_pos, vec4(shadow, 0.0, 0.0, 0.0));
	imageStore(output_mip_level, pixel_pos, depth_color);

	switch (params.debug_mode) {
		case 2:
			imageStore(output_debug, pixel_pos, vec4(0.0, 0.0, 0.0, 0.0));
			break;
		case 1:
			imageStore(output_debug, pixel_pos, vec4(shadow, shadow, shadow, 0.0));
			break;
		case 0:
		default:
			imageStore(output_debug, pixel_pos, depth_color);
			break;
	}
}
