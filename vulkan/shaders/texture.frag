#version 450

// This is the window texture
layout(set = 0, binding = 0) uniform sampler2D tex;
// This is what's already been drawn already, so all windows below us.
layout(set = 0, binding = 1) uniform sampler2D current_frame;


layout(std140, push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
} data;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

void main() {
	vec4 tex_color = textureLod(tex, uv, 0);

	out_color = vec4(tex_color.rgb, 0.8);
        // First component of surface_id is the actual surface ID, second
        // component is alpha. Alpha should be 1 unless we want to make the
        // texture we're rendering not absorb clicks.
	out_uv = vec4(uv, data.surface_id.xy);
}

