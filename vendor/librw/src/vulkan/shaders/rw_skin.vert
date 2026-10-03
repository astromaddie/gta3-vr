#version 450
#extension GL_EXT_multiview : require

#include "rw_common.glsl"

// Skinned geometry: peds, and anything else driven by an HAnim hierarchy.
//
// Identical to rw_world.vert once the vertex has been posed, so it shares
// rw_world.frag. The bone matrices arrive in their own descriptor set rather
// than push constants -- 64 mat4 is 4 KB, far past the 128 byte push budget --
// and the set is a dynamic uniform buffer so a draw only costs an offset.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColour;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in uvec4 inBoneIndices;

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out float fragFog;
layout(location = 3) flat out vec2 fragDynamicMotion;
layout(location = 4) out vec3 fragWorldPos;
layout(location = 5) out vec3 fragNormal;

layout(set = 2, binding = 0) uniform BoneData {
	mat4 bone[RW_MAX_BONES];
} bones;

void main()
{
	// RenderWare normalises the four weights, and unused slots carry weight 0,
	// so accumulating all four unconditionally is correct and branch free.
	mat4 pose =
		bones.bone[inBoneIndices.x] * inWeights.x +
		bones.bone[inBoneIndices.y] * inWeights.y +
		bones.bone[inBoneIndices.z] * inWeights.z +
		bones.bone[inBoneIndices.w] * inWeights.w;

	vec4 skinned = pose * vec4(inPosition, 1.0);
	mat4 model = RwModelMatrix();
	vec4 world = model * skinned;
	gl_Position = scene.viewProj[gl_ViewIndex] * world;
	vec4 rootMotion = RwRootScreenMotion();
	fragDynamicMotion = gl_ViewIndex == 0 ? rootMotion.xy : rootMotion.zw;

	// Same additive fixed-function sum as rw_world.vert; see the comment there.
	vec3 normal = normalize(mat3(model) * mat3(pose) * inNormal);
	vec3 colour = inColour.rgb + push.ambientLight.rgb * push.surfaceProps.x;
	vec4 dirColour = unpackUnorm4x8(floatBitsToUint(push.lightDirColour.w));
	colour += dirColour.rgb *
	          max(dot(normal, -push.lightDirColour.xyz), 0.0) *
	          push.surfaceProps.y;

	fragColour = vec4(clamp(colour, 0.0, 1.0), inColour.a) * push.materialColour;
	fragTexCoord = inTexCoord;
	// For the per-pixel dynamic lights; skinned meshes always carry real
	// normals, so the fragment stage uses this one as it is.
	if(RW_WORLD_EFFECTS != 0){
		fragWorldPos = world.xyz;
		fragNormal = normal;
	}

	// Fog by distance from the eye rather than by depth along the view axis.
	// Planar depth makes the fog a wall perpendicular to the gaze, and in a
	// headset that wall turns with the head: the mist appears to follow the
	// player instead of sitting still in the world.
	float fogDistance = length(world.xyz - scene.im2dParams.yzw);
	fragFog = (scene.fogParams.w * push.ambientLight.w) > 0.5 ?
		clamp((scene.fogParams.y - fogDistance) * scene.fogParams.z, 0.0, 1.0) :
		1.0;
}
