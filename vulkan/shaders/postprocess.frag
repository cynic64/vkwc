#version 450

layout(push_constant, row_major) uniform UBO {
	int mode;		// 0 = color, 1 = depth, 2 = uv, anything else = pink
} data;

layout(set = 0, binding = 0) uniform sampler2D screen_tex;
layout(set = 1, binding = 0) uniform sampler2D uv_tex;
layout(set = 2, binding = 0) uniform sampler2D blur_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

vec3 colors[] = {
        // black
        vec3(0.006, 0.007, 0.012),
        // white
        vec3(.847, 0.456, 0.056),
        // red
        vec3(.539, 0.031, 0.02),
        // orange
        vec3(.644, 0.141, 0.038),
        // purple
        vec3(.246, 0.262, 0.381),
        // blue
        vec3(.01, 0.089, 0.133),
        // aqua
        vec3(.033, 0.235, 0.342),
        // green
        vec3(.023, 0.392, 0.25),
};

// From https://stackoverflow.com/questions/15095909/from-rgb-to-hsv-in-opengl-glsl
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

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
        } else if (data.mode == 4) {
                // Color remap
                vec3 screen = texture(screen_tex, uv).rgb;
                vec3 hsv = rgb2hsv(screen);
                float h = hsv.x, s = hsv.y, v = hsv.z;
                float sixth = 1.0/6;

                if (v < 0.1) {
                        // black
                        out_color = vec4(colors[0], 1);
                } else if (s < 0.4) {
                        // white
                        out_color = vec4(colors[1], 1);
                } else if (h < 0.5 * sixth || h >= 5.5 * sixth) {
                        // red
                        out_color = vec4(colors[2], 1);
                } else if (h > 0.5*sixth && h <= 1.5*sixth) {
                        // yellow
                        out_color = vec4(colors[3], 1);
                } else if (h > 1.5*sixth && h <= 2.5*sixth) {
                        // green
                        out_color = vec4(colors[4], 1);
                } else if (h > 2.5*sixth && h <= 3.5*sixth) {
                        // aqua
                        out_color = vec4(colors[5], 1);
                } else if (h > 3.5*sixth && h <= 4.5*sixth) {
                        // blue
                        out_color = vec4(colors[6], 1);
                } else if (h > 4.5*sixth/2 && h <= 5.5*sixth) {
                        // pink
                        out_color = vec4(colors[7], 1);
                } else {
                        // ???
                        out_color = vec4(1, 0, 1, 1);
                }
        } else if (data.mode == 5) {
                // Colorscheme demo
                out_color = vec4(colors[int(uv.x * 7.999)], 1);
        } else {
                out_color = vec4(1, 0, 1, 1);
        }
}

