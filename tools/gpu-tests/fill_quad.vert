#version 450
// two triangles exactly covering the viewport (6 vertices)
void main() {
    const vec2 p[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
