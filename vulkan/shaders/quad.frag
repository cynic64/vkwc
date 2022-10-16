#version 450

layout(location = 0) out vec4 out_color;
layout(location = 1) out uvec4 out_uv;

layout(push_constant) uniform UBO {
	layout(offset = 96) vec4 color;
} data;

void main() {
	out_color = data.color;
	out_uv = uvec4(0, 0, 0, 0);
}
