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

layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D output_color;
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

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	//	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
	//		return;
	//	}
	vec3 direction = normalize(directional_lights.data[0].direction);
	vec4 color = vec4(0.0, 0.0, 0.0, 0.0);
	float mip_level = 0.0;

	vec3 screen_pos;
	screen_pos.xy = vec2(pixel_pos + 0.5) / params.screen_size;
	screen_pos.z = texelFetch(source_hiz, pixel_pos, 0).x;
	vec4 depth_color = vec4(0.8, 0.8, direction.z, 0.0);

	imageStore(output_color, pixel_pos, depth_color);
	imageStore(output_debug, pixel_pos, depth_color);
	imageStore(output_mip_level, pixel_pos, depth_color);
}
