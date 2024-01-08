#version 450

layout(push_constant, row_major) uniform UBO {
	int mode;		// 0 = color, 1 = depth, 2 = uv, anything else = pink
} data;

layout(location = 0) out vec4 out_color;

void main() {
	out_color = vec4(1, 0, 1, 1);
}

