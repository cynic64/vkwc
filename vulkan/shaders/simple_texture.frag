#version 450

// This is the window texture
layout(set = 0, binding = 0) uniform sampler2D tex;

layout(std140, push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
        vec2 screen_dims;
} data;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

void main() {
	out_color = texture(tex, uv);
        out_uv = vec4(0, 0, 0, 0);
}

