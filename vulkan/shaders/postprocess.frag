#version 450

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput in_color;
layout (input_attachment_index = 0, set = 0, binding = 1) uniform subpassInput in_depth;
layout (input_attachment_index = 0, set = 0, binding = 2) uniform subpassInput in_uv;

layout(push_constant, row_major) uniform UBO {
	int mode;		// 0 = color, 1 = depth, 2 = uv, anything else = pink
} data;


layout(location = 0) out vec4 out_color;

void main() {
	if (data.mode == 0) {
		out_color = subpassLoad(in_color);
	} else if (data.mode == 1) {
		out_color = subpassLoad(in_depth);
	} else if (data.mode == 2) {
		out_color = subpassLoad(in_uv);
	} else {
		out_color = vec4(1.0, 0.0, 1.0, 1.0);
	}
}

