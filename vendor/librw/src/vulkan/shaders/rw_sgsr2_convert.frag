#version 450
#extension GL_EXT_multiview : require

// Qualcomm SGSR2 2-pass fragment Convert stage, adapted from the official
// BSD-3-Clause shader for sampler2DArray OpenXR multiview and reVC's native
// currentNDC-previousNDC velocity convention.

layout(set = 0, binding = 0) uniform sampler2DArray sceneMotion;
layout(set = 0, binding = 1) uniform sampler2DArray sceneDepth;
layout(std140, set = 0, binding = 2) uniform MotionData {
	mat4 clipToPrevClip[2];
	vec4 temporalParams;
	vec4 sgsrParams;
} temporal;

layout(location = 0) out vec4 outMotionDepthClipAlpha;

void main()
{
	vec2 renderSize = max(temporal.temporalParams.xy, vec2(1.0));
	vec2 renderSizeRcp = 1.0/renderSize;
	vec2 uv = gl_FragCoord.xy*renderSizeRcp;
	vec2 gatherCoord = uv-vec2(0.5)*renderSizeRcp;
	float layer = float(gl_ViewIndex);

	// Official SGSR2 depth-clip neighbourhood. Vice City uses ordinary
	// 0-near/1-far depth, so the nearest sample is the minimum.
	vec4 bottomLeft = textureGather(sceneDepth,
		vec3(gatherCoord, layer), 0);
	vec4 bottomRight = textureGather(sceneDepth,
		vec3(gatherCoord+vec2(renderSizeRcp.x*2.0, 0.0), layer), 0);
	vec4 topLeft = textureGather(sceneDepth,
		vec3(gatherCoord+vec2(0.0, renderSizeRcp.y*2.0), layer), 0);
	vec4 topRight = textureGather(sceneDepth,
		vec3(gatherCoord+renderSizeRcp*2.0, layer), 0);

	float nearestFour = min(min(min(bottomLeft.z, bottomRight.w),
	                            topLeft.y), topRight.x);
	float bottomLeftMin = min(min(bottomLeft.x, bottomLeft.y),
	                          min(bottomLeft.z, bottomLeft.w));
	float bottomRightMin = min(min(bottomRight.x, bottomRight.y),
	                           min(bottomRight.z, bottomRight.w));
	float topLeftMin = min(min(topLeft.x, topLeft.y),
	                       min(topLeft.z, topLeft.w));
	float topRightMin = min(min(topRight.x, topRight.y),
	                        min(topRight.z, topRight.w));
	float nearestDepth = min(topLeft.x,
		min(nearestFour, min(bottomLeftMin, bottomRight.x)));

	float depthClip = 0.0;
	if(nearestFour < 1.0-1.0e-5){
		float separation = 1.37e-5*temporal.sgsrParams.z*
			length(renderSize)*(1.0-nearestFour);
		float weight = 0.0;
		weight += clamp(separation/(abs(nearestFour-bottomLeftMin)+1.19e-7), 0.0, 1.0);
		weight += clamp(separation/(abs(nearestFour-bottomRightMin)+1.19e-7), 0.0, 1.0);
		weight += clamp(separation/(abs(nearestFour-topLeftMin)+1.19e-7), 0.0, 1.0);
		weight += clamp(separation/(abs(nearestFour-topRightMin)+1.19e-7), 0.0, 1.0);
		depthClip = clamp(1.0-weight*0.25, 0.0, 1.0);
	}

	ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0),
		ivec2(renderSize)-ivec2(1));
	vec2 encodedDynamicMotion = texelFetch(sceneMotion,
		ivec3(pixel, int(gl_ViewIndex)), 0).rg;
	const bool hasDynamicMotion =
		length(encodedDynamicMotion) > 1.0e-7;
	vec2 motion = encodedDynamicMotion/8.0;
	if(length(motion) <= 1.0e-7){
		// reVC's OpenXR projection already flips clip Y (m[5] < 0) and the
		// Vulkan viewport has a positive height. Framebuffer UV and clip NDC Y
		// therefore grow in the same direction here, unlike the reference
		// REQUEST_NDC_Y_UP sample.
		// Depth was rasterised with the current sub-pixel projection offset,
		// while clipToPrevClip is deliberately built from unjittered matrices.
		// Reconstruct the unjittered current clip coordinate first. Omitting this
		// subtraction injected the projection jitter into camera motion a second
		// time, so each eye sampled history at a slightly wandering position.
		vec2 currentPixel = gl_FragCoord.xy-temporal.sgsrParams.xy;
		vec2 currentNdc = currentPixel*2.0*renderSizeRcp-1.0;
		vec4 previousClip = temporal.clipToPrevClip[gl_ViewIndex]*
			vec4(currentNdc, nearestDepth, 1.0);
		if(temporal.temporalParams.z > 0.5 &&
		   nearestDepth < 1.0-1.0e-6 &&
		   abs(previousClip.w) > 1.0e-6)
			motion = currentNdc-previousClip.xy/previousClip.w;
		else
			motion = vec2(0.0);
	}
	if(any(isnan(motion)) || any(isinf(motion)))
		motion = vec2(0.0);
	// Alpha is free in Qualcomm's Convert payload. Keep whether the scene MRT
	// supplied object/root velocity so the Upscale pass can distinguish a
	// moving car or ped from the much larger, perfectly reprojectable HMD
	// camera motion. Treating both alike made temporal AA visibly switch off
	// whenever the player turned their head.
	outMotionDepthClipAlpha = vec4(
		motion, depthClip, hasDynamicMotion ? 1.0 : 0.0);
}
