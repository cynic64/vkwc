#version 450

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

layout(std140, push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
} data;

void main() {
	out_color = data.color;
	out_uv = vec4(0, 0, 0, 0);
}
