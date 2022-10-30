#version 450

void main() {
	vec2 pos = vec2(float((gl_VertexIndex + 1) & 2) - 1,
		float(gl_VertexIndex & 2) - 1);
	gl_Position = vec4(pos, 0.0, 1.0);
}
