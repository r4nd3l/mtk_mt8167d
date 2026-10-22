#version 450
#ifndef WSCALE
#define WSCALE 2.0
#endif
// Two overlapping triangles: A (far, z=0.6) drawn first, B (near, z=0.3) second,
// plus C (farthest, z=0.9) drawn last, which the depth test must hide where it overlaps.
layout(push_constant) uniform PC { vec4 tint; } pc;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 col;
const vec3 P[9] = vec3[9](
    vec3(-0.9, 0.9, 0.6), vec3(0.5, 0.9, 0.6), vec3(-0.2, -0.7, 0.6),
    vec3(-0.5, 0.8, 0.3), vec3(0.9, 0.8, 0.3), vec3(0.2, -0.9, 0.3),
    vec3(-1.0, -1.0, 0.9), vec3(1.0, -1.0, 0.9), vec3(0.0, 1.0, 0.9));
const vec4 C[3] = vec4[3](vec4(1, 0.2, 0.2, 1), vec4(0.2, 1, 0.2, 1), vec4(0.3, 0.3, 1, 1));
void main() {
    vec3 p = P[gl_VertexIndex];
    gl_Position = vec4(p, 1.0) * WSCALE;
    uv = p.xy * 0.5 + 0.5;
    col = C[gl_VertexIndex / 3] * pc.tint;
}
