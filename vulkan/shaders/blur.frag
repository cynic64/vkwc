#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(std140, push_constant) uniform UBO {
	mat4 proj;
        vec4 color;
        vec2 surface_id;
        vec2 surface_dims;
        vec2 screen_dims;
        float radius;
} data;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
        float r = data.radius;
        vec3 sum = vec3(0);
        sum += texture(tex, uv + vec2(r, r) / data.screen_dims).rgb;
        sum += texture(tex, uv + vec2(-r, r) / data.screen_dims).rgb;
        sum += texture(tex, uv + vec2(r, -r) / data.screen_dims).rgb;
        sum += texture(tex, uv + vec2(-r, -r) / data.screen_dims).rgb;
        sum /= 4;

        out_color = vec4(sum, 1);
}

