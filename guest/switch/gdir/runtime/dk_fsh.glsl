#version 460
// MIT. The fragment shader: the GS's MODULATE (texture times vertex colour,
// RGB only) and its FOG (fog.rgb the fog colour, fog.a how much of the
// surface is left: 1 no fog); alpha is the vertex's (the fade's blend).

layout (location = 0) in vec4 vColor;
layout (location = 1) in vec4 vFog;
layout (location = 2) in vec2 vUv;

layout (binding = 0) uniform sampler2D tex;

layout (location = 0) out vec4 outColor;

void main()
{
	vec3 c = texture(tex, vUv).rgb * vColor.rgb;
	outColor = vec4(mix(vFog.rgb, c, vFog.a), vColor.a);
}
