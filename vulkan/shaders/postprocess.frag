#version 450

layout(push_constant, row_major, std140) uniform UBO {
        // 0 = color, 1 = depth, 2 = uv, anything else = pink
	int mode;		
        // I want to be able to smoothly from one colorscheme to the other.
        // This says how much of the previous colorscheme we should have vs the
        // next (0 to 1).
        float colorscheme_ratio;
        // Indices of the two colorschemes to mix
        int src_colorscheme_idx;
        int dst_colorscheme_idx;
} data;

layout(set = 0, binding = 0) uniform sampler2D screen_tex;
layout(set = 1, binding = 0) uniform sampler2D uv_tex;
layout(set = 2, binding = 0) uniform sampler2D blur_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

// Gotham scheme
vec3 colors1[8] = {
        // black
        vec3(0.006, 0.007, 0.012),
        // white
        vec3(.847, 0.456, 0.056),
        // red
        vec3(.539, 0.031, 0.02),
        // yellow
        vec3(.644, 0.141, 0.038),
        // green
        vec3(.246, 0.262, 0.381),
        // aqua
        vec3(.01, 0.089, 0.133),
        // blue
        vec3(.033, 0.235, 0.342),
        // purple
        vec3(.023, 0.392, 0.25),
};

// Gruvbox scheme
vec3 colors2[8] = {
        vec3(0.021, 0.021, 0.021),
        vec3(0.831, 0.708, 0.445),
        vec3(0.965, 0.067, 0.034),
        vec3(0.956, 0.509, 0.028),
        vec3(0.479, 0.497, 0.019),
        vec3(0.27, 0.527, 0.202),
        vec3(0.227, 0.376, 0.314),
        vec3(0.651, 0.238, 0.328),
};

// Nord
vec3 colors3[8] = {
        vec3(0.044, 0.054, 0.084),
        vec3(0.687, 0.73, 0.815),
        vec3(0.22, 0.356, 0.533),
        vec3(0.112, 0.22, 0.413),
        vec3(0.521, 0.12, 0.144),
        vec3(0.631, 0.242, 0.162),
        vec3(0.831, 0.597, 0.258),
        vec3(0.456, 0.27, 0.418),
};

// Solarized light
vec3 colors4[8] = {
        vec3(0.855, 0.807, 0.665),
        vec3(0.098, 0.156, 0.178),
        vec3(0.597, 0.07, 0.008),
        vec3(0.716, 0.032, 0.028),
        vec3(0.651, 0.037, 0.223),
        vec3(0.15, 0.165, 0.552),
        vec3(0.019, 0.258, 0.644),
        vec3(0.235, 0.319, 0.0),
};

vec3 all_colors[4][8] = {colors1, colors2, colors3, colors4};

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
                // Select colorscheme
                vec3 colors[8];
                if (uv.x > data.colorscheme_ratio) colors = all_colors[data.src_colorscheme_idx];
                else colors = all_colors[data.dst_colorscheme_idx];

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
                // Select colorscheme
                vec3 colors[8];
                if (uv.x > data.colorscheme_ratio) colors = all_colors[data.src_colorscheme_idx];
                else colors = all_colors[data.dst_colorscheme_idx];

                // Colorscheme demo
                out_color = vec4(colors[int(uv.x * 7.999)], 1);
        } else {
                out_color = vec4(1, 0, 1, 1);
        }
}

