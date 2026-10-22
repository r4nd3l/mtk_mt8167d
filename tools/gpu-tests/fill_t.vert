#version 450
// tiny triangle inside the first 16x16 tile
void main() {
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(-0.99, -1.0), vec2(-1.0, -0.99));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
