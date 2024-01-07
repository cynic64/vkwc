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

// This is the texture coordinate within the window texture, so it's (0,0) at
// the top-left corner of the window and (1, 1) at the bottom-right
layout(location = 0) out vec2 uv;
// This is the texture coordinate within the entire screen, so it's (0,0) at
// the top-left corner of the screen and (1,1) at the bottom right.
layout(location = 1) out vec2 global_uv;

void main() {
	vec2 pos = vec2(float((gl_VertexIndex + 1) & 2) * 0.5f,
		float(gl_VertexIndex & 2) * 0.5f);
	uv = data.uv_offset + pos * data.uv_size;
	gl_Position = data.proj * vec4(pos, 0.0, 1.0);

        global_uv = vec2(gl_Position.xy / gl_Position.w * 0.5 + 0.5);
}
