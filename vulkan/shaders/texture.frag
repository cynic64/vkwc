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

layout(location = 0) in vec2 global_uv;

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

vec2 get_local_uv() {
        // Get the local UV from the global UV and inverted projection matrix.
        // Explanation at bottom of file.
        float r0 = data.proj[0][2];
        float r1 = data.proj[1][2];
        float r2 = data.proj[2][2];
        float r3 = data.proj[3][2];
        float x = global_uv.x * 2 - 1, y = global_uv.y * 2 - 1;
        float z = -(r3 + x*r0 + y*r1) / r2;

        vec4 inverted = data.proj * vec4(x, y, z, 1);
        // The rounding makes sure we always get the same values even if the
        // window is moved. If it was 0.1, 0.2, 0.3, 0.4 before it will still be
        // 0.1, 0.2, 0.3, 0.4 even if you move the window.
        vec2 coords = round(inverted.xy / inverted.w * data.surface_dims);

        return (coords + vec2(0.5)) / data.surface_dims;
}

vec4 get_outside_color(vec2 uv) {
        float x_dist, y_dist;
        if (uv.x > 1) x_dist = data.surface_dims.x * (uv.x - 1);
        if (uv.x < 0) x_dist = data.surface_dims.x * -uv.x;
        if (uv.y > 1) y_dist = data.surface_dims.y * (uv.y - 1);
        if (uv.y < 0) y_dist = data.surface_dims.y * -uv.y;

        if (x_dist < 0) x_dist = 0;
        if (y_dist < 0) y_dist = 0;

        float dist = max(x_dist, y_dist);

        vec4 bright = vec4(0.5, 0.5, 0.5, 1);
        vec4 dark = vec4(0.05, 0.05, 0.05, 1);
        vec4 background = vec4(0.012, 0.012, 0.012, 1);

        if (data.is_focused == 0) bright = dark;

        if (dist < 1) {
                return bright;
        } else if (uv.y < 0 && y_dist < 48 && x_dist < 16) {
                // "Titlebar" thing at top
                if (x_dist <= 0 && y_dist > 16 && y_dist < 32) {
                        // Little box in titlebar
                        return dark;
                } else {
                        return background;
                }
        } else if (dist < 16) {
                return background;
        }

        // Shadow stuff
        if (uv.y < 0) y_dist -= 32;
        y_dist -= 16;
        x_dist -= 16;

        if (x_dist < 0) x_dist = 0;
        if (y_dist < 0) y_dist = 0;

        dist = sqrt(x_dist*x_dist + y_dist*y_dist);

        if (dist < 32) {
                float shadow_opacity = 1 - dist / 32;
                // Make it fade fast
                shadow_opacity *= shadow_opacity;
                shadow_opacity *= shadow_opacity;
                vec3 shadow_color = vec3(0);
                return vec4(shadow_color, 0.5 * shadow_opacity);
        } else {
                return vec4(0);
        }
}

vec3 get_blurred_background() {
        vec2 uv = global_uv;

        return texture(blur, uv).rgb;
}

void main() {
        vec2 uv = get_local_uv();

        float thing = 0;
        if (uv.x > 0 && uv.x < 1 && uv.y > 0 && uv.y < 1) {
                // We're in the window
                vec4 window = texture(tex, uv);
                vec3 background = get_blurred_background();
                float opacity = data.is_focused == 1 ? 0.9 : 0.7;

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

// We want to be able to set any pixel on the screen with this fragment shader,
// so the vertex shader is set to always output a fullscreen quad. This means
// we have to do the opposite of what the transform matrix usually does -
// instead of figuring out which pixels the corners of the window land on, we
// need to figure out where we are in the window based off the pixel.
//
// So we invert the matrix, which is already done for us by render_texture.
// However, we have to correctly guess the *the Z coordinate that would be
// under this pixel if we were rendering normally*, otherwise the inversion
// won't work properly.
//
// Usually we take normal_proj * vec4(uv, 0, 1). So we have to figure out what
// Z value to use so that inverse_proj * vec4(uv, z, 1) has a Z of 0, otherwise
// it's all messed up. The matrix multiplication looks like this:
//
// [ . . . . ]
// [ . . . . ]
// [ . . . . ] * [x, y, z, 1] = [_, _, 0, _]
// [ . . . . ]
//
// I marked the values we don't care about with _. Anyway, result's Z will be
// the 3rd row of the matrix * [x, y, z, 1], so if we number the elements of
// the third row r0..r3:
//
// x*r0 + y*r1 + z*r2 + 1*r3 = 0
// z = -(r3 + x*r0 + y*r1) / r2
//
// And then we can invert and everyone is happy :D
