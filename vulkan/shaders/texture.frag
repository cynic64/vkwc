#version 450

// This is what's been drawn so far, but blurred heavily
layout(set = 0, binding = 0) uniform sampler2D blur;
// This is the window texture
layout(set = 1, binding = 0) uniform sampler2D tex;

layout(std140, push_constant) uniform UBO {
	mat4 proj;
        vec4 color;
        vec2 surface_id;
        vec2 surface_dims;
        vec2 screen_dims;
        float is_focused;
} data;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec2 global_uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

vec3 colors[8] = {
        vec3(0, 0, 0),
        vec3(1, 1, 1),
        vec3(1, 0, 0),
        vec3(1, 1, 0),
        vec3(0, 1, 0),
        vec3(0, 1, 1),
        vec3(0, 0, 1),
        vec3(1, 0, 1),
};

uint hash(uint x) {
        x += ( x << 10u );
        x ^= ( x >>  6u );
        x += ( x <<  3u );
        x ^= ( x >> 11u );
        x += ( x << 15u );

        return x;
}

// Adapted from https://stackoverflow.com/questions/4200224/random-noise-functions-for-glsl
float random_float(vec2 uv) {
        uint h = hash(floatBitsToUint(uv.x) ^ hash(floatBitsToUint(uv.y)));

	const uint ieeeMantissa = 0x007FFFFFu; // binary32 mantissa bitmask
	const uint ieeeOne      = 0x3F800000u; // 1.0 in IEEE binary32

	h &= ieeeMantissa;                     // Keep only mantissa bits (fractional part)
	h |= ieeeOne;                          // Add fractional part to 1.0

	float  f = uintBitsToFloat( h );       // Range [1:2]
	return f - 1.0;                        // Range [0:1]
}

vec4 neon(vec3 color, float dist, float size) {
        if (dist > -1 && dist < 0) {
                // Line
                return vec4(1, 1, 1, 1);
        } else if (dist > -size && dist < size) {
                // Fade away from the border
                float opacity = 1 - (abs(dist) / size);
                opacity *= opacity * opacity;
                opacity *= opacity;
                return vec4(color, opacity);
        } else {
                return vec4(0);
        }
}

vec4 mix(vec4 x, vec4 y) {
        // Mix with alpha
        if (y.a > x.a) {
                vec4 tmp = x;
                x = y;
                y = tmp;
        }

        if (y.a == 0) return x;

        return vec4(x.rgb + y.rgb / (x.a / y.a), x.a);
}

vec4 get_outside_color(vec2 uv) {
        float x_dist, y_dist;
        if (uv.x > 1) x_dist = data.surface_dims.x * (uv.x - 1);
        if (uv.x < 0) x_dist = data.surface_dims.x * -uv.x;
        if (uv.y > 1) y_dist = data.surface_dims.y * (uv.y - 1);
        if (uv.y < 0) y_dist = data.surface_dims.y * -uv.y;

        if (x_dist < 0) x_dist = 0;
        if (y_dist < 0) y_dist = 0;

        //float dist = sqrt(x_dist * x_dist + y_dist * y_dist);
        float dist = max(x_dist, y_dist);

        vec4 bright = vec4(0.5, 0.5, 0.5, 1);
        vec4 dark = vec4(0.05, 0.05, 0.05, 1);
        vec4 background = vec4(0.012, 0.012, 0.012, 1);

        if (data.is_focused == 0) bright = dark;

        vec4 sum = vec4(0);

        // One-pixel border around the window
        if (dist < 1) sum += bright;

        sum = mix(sum, neon(colors[0], dist, 32));
        sum = mix(sum, neon(colors[1], dist - 16, 32));
        sum = mix(sum, neon(colors[2], dist - 32, 32));
        sum = mix(sum, neon(colors[3], dist - 48, 32));
        sum = mix(sum, neon(colors[4], dist - 64, 32));
        sum = mix(sum, neon(colors[5], dist - 80, 32));
        sum = mix(sum, neon(colors[6], dist - 96, 32));

        return sum;
}

vec3 get_blurred_background() {
        return texture(blur, global_uv).rgb;
}

void main() {
        if (uv.x > 0 && uv.x < 1 && uv.y > 0 && uv.y < 1) {
                // We're in the window
                vec4 window = texture(tex, uv);
                vec3 background = get_blurred_background();
                float opacity = data.is_focused == 1 ? 0.5 : 0.5;

                float alpha = window.a;
                window *= opacity;
                background *= (1 - opacity);

                out_color = vec4(window.rgb + background, alpha);
                out_uv = vec4(uv, data.surface_id.x, 1);

                // Overlay noise
                float noise = random_float(uv);
                noise = sqrt(noise);
                noise = sqrt(noise);
                float noise_factor = 0.5;
                noise = noise * noise_factor + (1 -  noise_factor);
                vec3 upper = vec3(noise);
                vec3 lower = out_color.rgb;
                // This is the "overlay" mode from GIMP
                out_color.rgb = ((1 - lower) * 2 * upper + lower) * lower;
        } else {
                // We're outside the window
                out_color = get_outside_color(uv);
                out_uv = vec4(0);
        }
}
