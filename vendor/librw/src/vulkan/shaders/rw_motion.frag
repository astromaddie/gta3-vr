#version 450
#extension GL_EXT_multiview : require

// Phase one of the SGSR2 integration: reconstruct per-eye camera motion from
// the current depth surface and the exact previous/current world-to-clip
// transforms. Dynamic-object velocity will later overwrite these values for
// moving geometry; zero application velocity is the fallback explicitly used
// by Qualcomm's SGSR2 Convert pass.

layout(std140, set = 0, binding = 0) uniform MotionData {
	mat4 clipToPrevClip[2];
	// xy = scene size, z = history valid, w = temporal mode.
	vec4 temporalParams;
} temporal;
layout(set = 0, binding = 1) uniform sampler2DArray sceneDepth;
layout(location = 0) out vec2 outMotion;

void main()
{
	vec2 size = max(temporal.temporalParams.xy, vec2(1.0));
	vec2 uv = gl_FragCoord.xy / size;
	ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0),
		ivec2(size)-ivec2(1));
	float depth = texelFetch(sceneDepth, ivec3(pixel, int(gl_ViewIndex)), 0).r;

	// reVC flips Y in the OpenXR projection and uses a positive-height Vulkan
	// viewport, so framebuffer UV and clip NDC Y grow in the same direction.
	vec2 currentNdc = uv*2.0-1.0;
	vec4 previousClip = temporal.clipToPrevClip[gl_ViewIndex] *
		vec4(currentNdc, depth, 1.0);
	vec2 previousNdc = abs(previousClip.w) > 1.0e-6 ?
		previousClip.xy/previousClip.w : currentNdc;
	vec2 motion = currentNdc-previousNdc;
	if(temporal.temporalParams.z < 0.5 || depth >= 1.0-1.0e-6 ||
	   any(isnan(motion)) || any(isinf(motion)))
		motion = vec2(0.0);
	outMotion = clamp(motion*8.0, vec2(-1.0), vec2(1.0));
}
