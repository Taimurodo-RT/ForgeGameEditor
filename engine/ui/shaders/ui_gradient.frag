#version 450

// CSS gradients: linear, radial and conic, plain or repeating.

#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;

layout(set = 3, binding = 0) uniform Gradient {
    int func;
    int num_stops;
    vec2 p;  // linear: start point, radial and conic: centre
    vec2 v;  // linear: vector to the end point, radial: inverse radius, conic: angle as a unit vector
    vec2 pad;
    vec4 colors[16];
    vec4 positions[4]; // 16 stop positions, 0 at the start point and 1 at the end
} g;

float stop_position(int i) {
    return g.positions[i / 4][i % 4];
}

vec4 mix_stop_colors(float t) {
    vec4 color = g.colors[0];
    for (int i = 1; i < g.num_stops; i++)
        color = mix(color, g.colors[i], smoothstep(stop_position(i - 1), stop_position(i), t));
    return color;
}

void main() {
    float t = 0.0;
    if (g.func == LINEAR || g.func == REPEATING_LINEAR) {
        t = dot(g.v, in_uv - g.p) / dot(g.v, g.v);
    } else if (g.func == RADIAL || g.func == REPEATING_RADIAL) {
        t = length(g.v * (in_uv - g.p));
    } else {
        mat2 r = mat2(g.v.x, -g.v.y, g.v.y, g.v.x);
        vec2 d = r * (in_uv - g.p);
        t = 0.5 + atan(-d.x, d.y) / (2.0 * PI);
    }
    if (g.func >= REPEATING_LINEAR) {
        float t0 = stop_position(0);
        float t1 = stop_position(g.num_stops - 1);
        t = t0 + mod(t - t0, t1 - t0);
    }
    out_color = in_color * mix_stop_colors(t);
}
