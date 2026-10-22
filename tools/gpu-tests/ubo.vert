#version 450
// Same scene as scene.vert, but positions + colours come from a uniform buffer
// (like vkcube: big UBO indexed by gl_VertexIndex) instead of shader constants.
layout(std140, set = 0, binding = 1) uniform U { vec4 pos[9]; vec4 col[3]; vec4 tint; } u;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 col;
void main() {
    vec4 p = u.pos[gl_VertexIndex];
    gl_Position = vec4(p.xyz, 1.0);
    uv = p.xy * 0.5 + 0.5;
    col = u.col[gl_VertexIndex / 3] * u.tint;
}
