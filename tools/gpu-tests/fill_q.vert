#version 450
// triangle whose bounding box is the top-left quarter of the viewport
void main() {
    const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(-1.0, 0.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
