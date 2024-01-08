#version 450

// we use a mat4 since it uses the same size as mat3 due to
// alignment. Easier to deal with (tighly-packed) mat4 though.
layout(std140, push_constant, row_major) uniform UBO {
	mat4 proj;
	vec2 uv_offset;
	vec2 uv_size;
        vec4 color;
        vec2 surface_id;
} data;

layout(location = 0) out vec2 global_uv;

void main() {
	vec2 pos = vec2(float((gl_VertexIndex + 1) & 2) - 1,
		float(gl_VertexIndex & 2) - 1);
	gl_Position = vec4(pos, 0.0, 1.0);

        global_uv = pos * 0.5 + 0.5;
}
