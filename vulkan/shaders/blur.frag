#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(std140, push_constant) uniform UBO {
	mat4 proj;
        vec4 color;
        vec2 surface_id;
        vec2 surface_dims;
        vec2 screen_dims;
        // 0 = downsampling, 1 = upsampling
        float upsample;
} data;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
        if (data.upsample == 0) {
                // Downsample
                out_color = texture(tex, uv) / 2
                        + texture(tex, uv + vec2(1, 0) / data.screen_dims) / 8
                        + texture(tex, uv + vec2(-1, 0 / data.screen_dims)) / 8
                        + texture(tex, uv + vec2(0.0,  1) / data.screen_dims) / 8
                        + texture(tex, uv + vec2(0.0, -1) / data.screen_dims) / 8;
                out_color.a = 1;
        } else {
                // Upsample
                out_color = texture(tex, uv + vec2(1, 1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(-1, 1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(1, -1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(-1, -1) / data.screen_dims) / 6
                        + texture(tex, uv + vec2(2, 0) / data.screen_dims) / 12
                        + texture(tex, uv + vec2(-2, 0) / data.screen_dims) / 12
                        + texture(tex, uv + vec2(0, 2) / data.screen_dims) / 12
                        + texture(tex, uv + vec2(0, -2) / data.screen_dims) / 12;
        }
}

