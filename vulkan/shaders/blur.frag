#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(std140, push_constant) uniform UBO {
	mat4 proj;
        vec4 color;
        vec2 surface_id;
        vec2 surface_dims;
        vec2 screen_dims;
        // 0 = downsampling, 1 = upsampling, 2 = downsample but threshold first
        float mode;
} data;

// We use the same fragment shader as windows, which get their local UV and
// global UV through the second. So iwe ignore the first here and start at idx
// 1.
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 out_color;

vec3 threshold(vec3 x) {
        if (x.r + x.g + x.b > 0.3 * 3) {
                return x;
        } else {
                return vec3(0);
        }
}

void main() {
        if (data.mode == 0) {
                // Downsample
                out_color = texture(tex, uv) / 2
                        + texture(tex, uv + vec2(0.5, 0) / data.screen_dims) / 8
                        + texture(tex, uv + vec2(-0.5, 0) / data.screen_dims) / 8
                        + texture(tex, uv + vec2(0,  0.5) / data.screen_dims) / 8
                        + texture(tex, uv + vec2(0, -0.5) / data.screen_dims) / 8;
                out_color.a = 1;
        } else if (data.mode == 1) {
                // Upsample
                out_color = texture(tex, uv + vec2(1, 1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(-1, 1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(1, -1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(-1, -1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(2, 0) / data.screen_dims) / 12
                        + texture(tex, uv + vec2(-2, 0) / data.screen_dims) / 12
                        + texture(tex, uv + vec2(0, 2) / data.screen_dims) / 12
                        + texture(tex, uv + vec2(0, -2) / data.screen_dims) / 12;
                out_color.a = 1;
        } else if (data.mode == 2) {
                // Downsample with threshold
                vec3 s1 = texture(tex, uv).rgb;
                vec3 s2 = texture(tex, uv + vec2(0.5, 0) / data.screen_dims).rgb;
                vec3 s3 = texture(tex, uv + vec2(-0.5, 0) / data.screen_dims).rgb;
                vec3 s4 = texture(tex, uv + vec2(0, 0.5) / data.screen_dims).rgb;
                vec3 s5 = texture(tex, uv + vec2(0, -0.5) / data.screen_dims).rgb;

                s1 = threshold(s1);
                s2 = threshold(s2);
                s3 = threshold(s3);
                s4 = threshold(s4);
                s5 = threshold(s5);

                out_color.a = 1;
                out_color.rgb = s1 + s2 + s3 + s4 + s5;
        } else {
                // ???
                out_color = vec4(1, 0, 1, 1);
        }
}

