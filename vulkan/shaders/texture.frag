#version 450

// This is what's already been drawn already, so all windows below us.
layout(set = 0, binding = 0) uniform sampler2D current_frame;
// This is the window texture
layout(set = 1, binding = 0) uniform sampler2D tex;


layout(std140, push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
} data;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec2 global_uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

vec3 get_blurred_background() {
	// Taken from: https://www.shadertoy.com/view/Xltfzj
	float tau = 6.28318530718;

	// GAUSSIAN BLUR SETTINGS {{{
	float directions = 8.0; // BLUR DIRECTIONS (Default 16.0 - More is better but slower)
	float quality = 4.0; // BLUR QUALITY (Default 4.0 - More is better but slower)
	float size = 8.0; // BLUR SIZE (Radius)
	// GAUSSIAN BLUR SETTINGS }}}

	// TODO: reimplement
	//vec2 radius = Size/iResolution.xy;
	vec2 radius = size / vec2(1000, 1000);

	// Normalized pixel coordinates (from 0 to 1)
	// Pixel colour
	vec4 color = texture(current_frame, global_uv);

	// Blur calculations
	for( float d = 0.0; d < tau; d += tau / directions) {
		for(float i = 1.0 / quality; i <= 1.001; i += 1.0 / quality) {
			color += texture(current_frame,
				global_uv + vec2(cos(d), sin(d)) * radius * i);
		}
	}

	// Output to screen
	color /= quality * directions + 1;

	return color.rgb;
}

void main() {
	vec3 window_color = texture(tex, uv + vec2(0.5 / 700, 0.5 / 500)).rgb;

        vec3 prev_color = get_blurred_background();

        vec3 final_color = window_color * 0.8 + prev_color * 0.2;
	out_color = vec4(final_color, 1.0);
        // First component of surface_id is the actual surface ID, second
        // component is alpha. Alpha should be 1 unless we want to make the
        // texture we're rendering not absorb clicks.
	out_uv = vec4(uv, data.surface_id.xy);
}

