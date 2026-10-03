#version 450
#extension GL_EXT_multiview : require

#include "rw_common.glsl"

// Immediate-mode 3D: water, glass, rain, skidmarks, coronas, tyre marks and the
// VR hands all arrive through this path. Vertices are in world space already
// (im3DTransform applies the object matrix on the CPU), so only the per-eye
// view-projection is applied here.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColour;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec4 fragColour;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out float fragFog;
layout(location = 3) flat out vec2 fragDynamicMotion;
// Present only because this stage shares rw_world.frag with the atomic
// pipelines. Immediate geometry pushes a zero diffuse coefficient, which is
// what makes the fragment stage skip the dynamic lights entirely.
layout(location = 4) out vec3 fragWorldPos;
layout(location = 5) out vec3 fragNormal;

void main()
{
	mat4 model = RwModelMatrix();
	vec4 world = model * vec4(inPosition, 1.0);
	gl_Position = scene.viewProj[gl_ViewIndex] * world;
	vec4 rootMotion = RwRootScreenMotion();
	fragDynamicMotion = gl_ViewIndex == 0 ? rootMotion.xy : rootMotion.zw;
	fragColour = inColour * push.materialColour;
	fragTexCoord = inTexCoord;
	if(RW_WORLD_EFFECTS != 0){
		fragWorldPos = world.xyz;
		fragNormal = vec3(0.0, 0.0, 1.0);
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
