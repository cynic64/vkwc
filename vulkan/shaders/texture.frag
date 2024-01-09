#version 450

// This is what's already been drawn already, so all windows below us.
layout(set = 0, binding = 0) uniform sampler2D current_frame;
// This is the window texture
layout(set = 1, binding = 0) uniform sampler2D tex;

layout(std140, push_constant) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
        vec2 surface_dims;
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

vec2 get_local_uv() {
        // Get the local UV from the global UV and inverted projection matrix.
        // Explanation at bottom of file.
        float r0 = data.proj[0][2], r1 = data.proj[1][2], r2 = data.proj[2][2], r3 = data.proj[3][2];
        float x = global_uv.x * 2 - 1, y = global_uv.y * 2 - 1;
        float z = -(r3 + x*r0 + y*r1) / r2;

        vec4 inverted = data.proj * vec4(x, y, z, 1);
        return inverted.xy / inverted.w;
}

vec4 get_outside_color(vec2 uv) {
        float x_dist, y_dist;
        if (uv.x > 1) x_dist = data.surface_dims.x * (uv.x - 1);
        if (uv.x < 0) x_dist = data.surface_dims.x * -uv.x;
        if (uv.y > 1) y_dist = data.surface_dims.y * (uv.y - 1);
        if (uv.y < 0) y_dist = data.surface_dims.y * -uv.y;

        if (x_dist < 0) x_dist = 0;
        if (y_dist < 0) y_dist = 0;

        float dist_sq = x_dist * x_dist + y_dist * y_dist;
        if (dist_sq > 15*15 && dist_sq < 16*16) {
                return vec4(1);
        } else {
                return vec4(0);
        }
}

void main() {
        vec2 uv = get_local_uv();

        float thing = 0;
        if (uv.x > 0 && uv.x < 1 && uv.y > 0 && uv.y < 1) {
                // We're in the window
                out_color = vec4(texture(tex, uv).rgb, 0.9);
                out_uv = vec4(uv, data.surface_id.x, 1);
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
