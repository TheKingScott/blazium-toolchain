#version 460
// MIT. The vertex shader: the CPU has transformed and clipped the vertices
// (clip space, as the PS2's EE does), so they pass through; the colour, the
// fog (rgb its colour, a how much of the surface is left) and the uv go on
// to the fragment shader.

layout (location = 0) in vec4 inPos;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec4 inFog;
layout (location = 3) in vec2 inUv;

layout (location = 0) out vec4 vColor;
layout (location = 1) out vec4 vFog;
layout (location = 2) out vec2 vUv;

void main()
{
	gl_Position = inPos;
	vColor = inColor;
	vFog = inFog;
	vUv = inUv;
}
