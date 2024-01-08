#version 450

layout(push_constant, row_major) uniform UBO {
	int mode;		// 0 = color, 1 = depth, 2 = uv, anything else = pink
} data;

layout(set = 0, binding = 0) uniform sampler2D screen_tex;
layout(set = 1, binding = 0) uniform sampler2D uv_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
        if (data.mode == 0) {
                out_color = texture(screen_tex, uv);
        } else if (data.mode == 1) {
                out_color = texture(uv_tex, uv);
        } else {
                out_color = vec4(1, 0, 1, 1);
        }
}

