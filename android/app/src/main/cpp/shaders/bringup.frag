#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragTint;

layout(location = 0) out vec4 outColour;

void main()
{
	// Fixed key light. Enough shading to make depth, culling and the stereo
	// separation readable by eye without any texture or material system.
	const vec3 lightDir = normalize(vec3(0.4, 0.85, 0.35));
	float lambert = max(dot(normalize(fragNormal), lightDir), 0.0);
	float shade = lambert * 0.75 + 0.25;
	outColour = vec4(fragTint.rgb * shade, 1.0);
}
