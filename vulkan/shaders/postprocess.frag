#version 450

layout(push_constant, row_major) uniform UBO {
	int mode;		// 0 = color, 1 = depth, 2 = uv, anything else = pink
} data;

layout(set = 0, binding = 0) uniform sampler2D color_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
	out_color = texture(color_tex, uv);
}

