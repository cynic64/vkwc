#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform UBO {
	layout(offset = 80) float surface_id;
} data;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_uv;

void main() {
	vec4 tex_color = textureLod(tex, uv, 0);

	if (tex_color.a == 0) discard;

	out_color = vec4(tex_color.xyz, 1);
	out_uv = vec4(uv, data.surface_id, 1);
}

