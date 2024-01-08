#version 450

// This is what's already been drawn already, so all windows below us.
layout(set = 0, binding = 0) uniform sampler2D current_frame;
// This is the window texture
layout(set = 1, binding = 0) uniform sampler2D tex;


layout(std140, push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
        vec2 screen_dims;
} data;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec2 global_uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

uint hash(uint x) {
        x += ( x << 10u );
        x ^= ( x >>  6u );
        x += ( x <<  3u );
        x ^= ( x >> 11u );
        x += ( x << 15u );

        return x;
}

vec3 get_blurred_background() {
        // Blurring is hard, man...

        // Sample a pixel in a random direction
        uint random = hash(uint(uv.x * 16777216 + uv.y * 1024));
        // A one pixel offset in screen coordinates
        float x_off = 1 / data.screen_dims.x, y_off = 1 / data.screen_dims.y;

        vec2 directions[] = {
                vec2(x_off, 0),
                vec2(0, y_off),
                vec2(-x_off, 0),
                vec2(0, -y_off),
        };

        vec3 color = vec3(0);
        for (int i = 0; i < 8; i++) {
                int max_distance = 32;
                color += texture(current_frame, global_uv
                        + 4 * (max_distance / (random % max_distance + 1)) * directions[random % 4]).rgb;
                random = hash(random);
        };
        return color / 8;
}

void main() {
	vec3 window_color = texture(tex, uv).rgb;

        vec3 prev_color = get_blurred_background();

        vec3 final_color = window_color * 0.9 + prev_color * 0.1;
	out_color = vec4(final_color, 1.0);
        // First component of surface_id is the actual surface ID, second
        // component is alpha. Alpha should be 1 unless we want to make the
        // texture we're rendering not absorb clicks.
	out_uv = vec4(uv, data.surface_id.xy);
}

