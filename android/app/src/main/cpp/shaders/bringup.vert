#version 450
// Multiview is the whole point of the Vulkan path on Quest: one draw call
// rasterises both eyes into the two layers of an array target, and gl_ViewIndex
// selects the per-eye matrix. This mirrors what the D3D12 backend achieves with
// instanced double-wide rendering, but without the manual clip-distance work.
#extension GL_EXT_multiview : require

layout(set = 0, binding = 0) uniform ViewData {
	mat4 viewProj[2];
} view;

layout(push_constant) uniform PushConstants {
	mat4 model;
	vec4 tint;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec4 fragTint;

void main()
{
	vec4 world = pc.model * vec4(inPosition, 1.0);
	gl_Position = view.viewProj[gl_ViewIndex] * world;
	fragNormal = mat3(pc.model) * inNormal;
	fragTint = pc.tint;
}
