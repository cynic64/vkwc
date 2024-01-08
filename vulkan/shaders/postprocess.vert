#version 450

layout(location = 0) out vec2 out_uv;

void main() {
	vec2 pos = vec2(float((gl_VertexIndex + 1) & 2) - 1,
		float(gl_VertexIndex & 2) - 1);
	gl_Position = vec4(pos, 0.0, 1.0);

        out_uv = pos * 0.5 + 0.5;
}
