#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D sheet;

void main() {
    vec4 t = texture(sheet, in_uv);
    vec4 c = vec4(t.rgb * t.a, t.a) * in_color;
    if (c.a < 0.004) discard;
    out_color = c;
}
