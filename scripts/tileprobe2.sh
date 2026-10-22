#!/bin/sh
# For each tile (tx,ty): draw a tiny triangle inside that 16x16 tile, dump the
# region headers and print which header indices are non-empty (flag 0x10).
cd /home/user/vk
for t in "0 0" "1 0" "0 1" "1 1" "2 0" "0 2" "3 0" "4 0" "0 4" "7 0" "8 0" "0 8" "15 0" "0 15" "15 15"; do
	set -- $t
	cx=$(( $1 * 16 + 8 )); cy=$(( $2 * 16 + 8 ))
	cat > tp.vert <<EOV
#version 450
void main() {
    const vec2 p[3] = vec2[3](vec2($cx-4, $cy-4), vec2($cx+4, $cy-4), vec2($cx, $cy+4));
    gl_Position = vec4(p[gl_VertexIndex] / 128.0 - 1.0, 0.0, 1.0);
}
EOV
	glslangValidator -V tp.vert -o tri.vert.spv >/dev/null
	PVR_DUMP_RGN=6000 PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 timeout 60 ./vktri /tmp/tp.ppm > /tmp/tp.txt 2>&1
	python3 - "$1" "$2" <<'EOP'
import sys
t=open("/tmp/tp.txt").read().split("\n")
i=[k for k,l in enumerate(t) if l.startswith("DUMP rgn size")][0]
hx=""
for l in t[i+1:]:
    if l.startswith("DUMP"): break
    hx+=l.strip()
b=bytes.fromhex(hx)
ne=[k for k in range(len(b)//5) if b[k*5] & 0x10]
print("tile", sys.argv[1], sys.argv[2], "nonempty", ne, "flags", [hex(b[k*5]) for k in ne])
EOP
done
glslangValidator -V tri.vert -o tri.vert.spv >/dev/null
