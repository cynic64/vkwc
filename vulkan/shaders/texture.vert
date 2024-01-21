#version 450

// we use a mat4 since it uses the same size as mat3 due to
// alignment. Easier to deal with (tighly-packed) mat4 though.
layout(std140, push_constant) uniform UBO {
	mat4 proj;
        vec4 color;
        vec2 surface_id;
        vec2 surface_dims;
} data;

layout(location = 0) out vec2 uv;
layout(location = 1) out vec2 global_uv;

void main() {

	vec2 pos = vec2(float((gl_VertexIndex + 1) & 2) / 2,
		float(gl_VertexIndex & 2) / 2);
        // have to map 0..1 -> -padding / surface_dims, 1 + padding / surface_dims
        vec2 padding_pixels = vec2(128);
        vec2 padding = padding_pixels / data.surface_dims;
        uv = pos * (1 + 2*padding) - padding;

	gl_Position = data.proj * vec4(pos, 0, 1.0);

        global_uv = (gl_Position.xy / gl_Position.w) * 0.5 + 0.5;
}
