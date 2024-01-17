#version 450

layout(push_constant, row_major) uniform UBO {
	int mode;		// 0 = color, 1 = depth, 2 = uv, anything else = pink
} data;

layout(set = 0, binding = 0) uniform sampler2D screen_tex;
layout(set = 1, binding = 0) uniform sampler2D uv_tex;
layout(set = 2, binding = 0) uniform sampler2D blur_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
        if (data.mode == 0) {
                out_color = texture(screen_tex, uv);
        } else if (data.mode == 1) {
                out_color = texture(uv_tex, uv);
        } else if (data.mode == 2) {
                out_color = texture(blur_tex, uv);
        } else if (data.mode == 3) {
                // CRT-esque
                vec3 bloom = texture(blur_tex, uv).rgb;
                vec3 screen = texture(screen_tex, uv).rgb;
                float brightness = screen.r + screen.y + screen.z;

                if (brightness > 0.15 * 3) {
                        screen = vec3(1, 0.4, 0);
                        bloom = vec3(0);
                } else {
                        screen = vec3(0);
                        float bloom_brightness = bloom.r + bloom.g + bloom.b;
                        bloom = vec3(1, 0.2, 0) * bloom_brightness;
                }

                out_color = vec4(screen + bloom * 0.15, 1);
        } else {
                out_color = vec4(1, 0, 1, 1);
        }
}

