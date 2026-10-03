#version 450
#extension GL_EXT_multiview : require

// Snapdragon Game Super Resolution 1 integration.
// Copyright (c) 2025, Qualcomm Innovation Center, Inc.
// SPDX-License-Identifier: BSD-3-Clause
// Adapted from the official mobile GLSL shader for sampler2DArray multiview
// and fused into reVC's existing colour-filter resolve pass.

// A sampled array is required because FXAA follows edges into neighbouring
// texels. gl_ViewIndex selects the matching eye in the multiview image.
layout(set = 0, binding = 0) uniform sampler2DArray sceneColour;
layout(set = 0, binding = 1) uniform sampler2DArray sceneMotion;
layout(set = 0, binding = 2) uniform sampler2DArray sceneDepth;
layout(std140, set = 0, binding = 3) uniform MotionData {
	mat4 clipToPrevClip[2];
	// xy = scene size, z = history valid, w = temporal mode.
	vec4 temporalParams;
	vec4 sgsrParams;
} temporal;
layout(set = 0, binding = 4) uniform sampler2DArray sceneHistory;

layout(push_constant) uniform PostFxConstants
{
	vec4 blurColour;
	uvec4 mode;
	uvec4 upscale;
} postFx;

layout(location = 0) out vec4 outColour;
// Present only in V3's two-attachment resolve pass. Older one-attachment
// pipelines discard this output and therefore retain their exact behaviour.
layout(location = 1) out vec4 outResolvedHistory;

void StoreBoth(vec4 colour)
{
	outColour = colour;
	outResolvedHistory = colour;
}

vec2 ImageSize()
{
	return max(vec2(postFx.mode.zw), vec2(1.0));
}

vec2 OutputSize()
{
	return max(vec2(postFx.upscale.xy), vec2(1.0));
}

vec3 SampleColour(vec2 uv)
{
	vec2 halfTexel = 0.5 / ImageSize();
	return texture(sceneColour,
	               vec3(clamp(uv, halfTexel, vec2(1.0) - halfTexel),
	                    float(gl_ViewIndex))).rgb;
}

vec2 SampleCombinedMotion(vec2 uv)
{
	vec2 dynamicMotion = texture(sceneMotion,
		vec3(uv, float(gl_ViewIndex))).rg/8.0;
	if(length(dynamicMotion) > 1.0e-7)
		return dynamicMotion;
	if(temporal.temporalParams.z < 0.5)
		return vec2(0.0);

	vec2 size = max(temporal.temporalParams.xy, vec2(1.0));
	ivec2 pixel = clamp(ivec2(uv*size), ivec2(0), ivec2(size)-ivec2(1));
	float depth = texelFetch(sceneDepth,
		ivec3(pixel, int(gl_ViewIndex)), 0).r;
	if(depth >= 1.0-1.0e-6)
		return vec2(0.0);

	// reVC flips Y in the OpenXR projection and uses a positive-height Vulkan
	// viewport, so framebuffer UV and clip NDC Y grow in the same direction.
	vec2 currentNdc = uv*2.0-1.0;
	vec4 previousClip = temporal.clipToPrevClip[gl_ViewIndex] *
		vec4(currentNdc, depth, 1.0);
	if(abs(previousClip.w) <= 1.0e-6)
		return vec2(0.0);
	vec2 motion = currentNdc-previousClip.xy/previousClip.w;
	return any(isnan(motion)) || any(isinf(motion)) ? vec2(0.0) : motion;
}

vec3 ApplyTemporalStabilizer(vec2 uv, vec3 current)
{
	if(temporal.temporalParams.z < 0.5)
		return current;

	vec2 motion = SampleCombinedMotion(uv);
	if(any(greaterThanEqual(abs(motion), vec2(0.124))))
		return current;
	// Stored motion is currentNDC-previousNDC. Both clip axes have the same
	// direction as framebuffer UV in this renderer.
	vec2 previousUv = uv-motion*0.5;
	if(any(lessThanEqual(previousUv, vec2(0.0))) ||
	   any(greaterThanEqual(previousUv, vec2(1.0))) ||
	   length(motion) > 0.35)
		return current;

	// Clamp reprojected history to the current 3x3 neighbourhood. This is the
	// main disocclusion guard: a colour that no longer exists around the current
	// pixel cannot leave a long trail behind a car, hand or pedestrian.
	vec2 texel = 1.0/ImageSize();
	vec3 localMin = current;
	vec3 localMax = current;
	for(int y = -1; y <= 1; y++)
		for(int x = -1; x <= 1; x++){
			vec3 sampleColour = SampleColour(uv+vec2(x, y)*texel);
			localMin = min(localMin, sampleColour);
			localMax = max(localMax, sampleColour);
		}
	vec3 history = texture(sceneHistory,
		vec3(previousUv, gl_ViewIndex)).rgb;
	history = clamp(history, localMin-vec3(1.0/255.0),
		localMax+vec3(1.0/255.0));

	float motionPixels = length(motion*0.5*OutputSize());
	float lumaDifference = abs(dot(history-current,
		vec3(0.299, 0.587, 0.114)));
	// Native 100% benefits from strong temporal stability. At 125% and above
	// the current frame already contains substantially more real detail; the
	// old 90% history blend averaged that detail back down to roughly the 100%
	// appearance. Preserve the high-resolution current sample instead of
	// pretending that non-jittered history is a DLSS-style reconstruction.
	float historyWeight;
	if(postFx.upscale.z == 4u){
		// Stable V3 samples the preceding already-resolved history without
		// perturbing the headset projection. This is a temporal stability test,
		// not a claim of four reconstructed spatial samples.
		historyWeight = mix(0.62, 0.08,
			clamp(motionPixels/5.0, 0.0, 1.0));
		historyWeight *= 1.0-clamp(lumaDifference*14.0, 0.0, 0.94);
	}else if(postFx.upscale.z == 3u){
		// This history is the complementary raw quarter-pixel sample, not an
		// accumulated blurry frame. A static surface receives a true two-sample
		// resolve; motion and colour disagreement quickly return ownership to the
		// current frame.
		historyWeight = mix(0.50, 0.12,
			clamp(motionPixels/6.0, 0.0, 1.0));
		historyWeight *= 1.0-clamp(lumaDifference*12.0, 0.0, 0.90);
	}else{
		float renderScale = max(float(postFx.upscale.w), 100.0)/100.0;
		float highResolution = clamp((renderScale-1.0)/0.25, 0.0, 1.0);
		float stillWeight = mix(0.90, 0.48, highResolution);
		float movingWeight = mix(0.55, 0.20, highResolution);
		historyWeight = mix(stillWeight, movingWeight,
			clamp(motionPixels/10.0, 0.0, 1.0));
		historyWeight *= 1.0-clamp(lumaDifference*8.0, 0.0, 0.80);
	}
	return mix(current, history, historyWeight);
}

float Luma(vec3 colour)
{
	return dot(colour, vec3(0.299, 0.587, 0.114));
}

vec3 ApplyFxaa(vec2 uv, vec3 middle);

float SgsrFastLanczos2(float x)
{
	float wA = x - 4.0;
	float wB = x*wA - wA;
	wA *= wA;
	return wB*wA;
}

vec2 SgsrEdgeDirection(vec4 left, vec4 right)
{
	float rxLz = right.x-left.z;
	float rwLy = right.w-left.y;
	vec2 delta = vec2(rxLz+rwLy, rxLz-rwLy);
	float inverseLength = inversesqrt(dot(delta, delta)+3.075740e-05);
	return delta*inverseLength;
}

vec2 SgsrWeightY(float dx, float dy, float c, vec3 data)
{
	float edgeDistance = dx*data.z+dy*data.y;
	float x = dx*dx+dy*dy + edgeDistance*edgeDistance*
		(clamp(c*c*data.x, 0.0, 1.0)*0.7-1.0);
	float weight = SgsrFastLanczos2(x);
	return vec2(weight, weight*c);
}

// Qualcomm SGSR1 RGBA mode, using green as the edge channel just like the
// reference mobile shader. The VR-recommended threshold is 4/255.
vec3 ApplySgsr1(vec2 uv)
{
	const int edgeChannel = 1;
	const float edgeThreshold = 4.0/255.0;
	const float edgeSharpness = 2.0;
	vec3 colour = SampleColour(uv);
	// SGSR1 is reconstruction/sharpening, not temporal anti-aliasing. Feed it
	// the available spatial AA result when FXAA is enabled; bypassing FXAA here
	// made the lower-resolution geometry edges sharper but visibly more jagged.
	if(postFx.mode.y != 0u)
		colour = ApplyFxaa(uv, colour);
	vec2 inputSize = ImageSize();
	vec2 invInputSize = 1.0/inputSize;
	vec2 imageCoord = uv*inputSize + vec2(-0.5, 0.5);
	vec2 imagePixel = floor(imageCoord);
	vec2 coord = imagePixel*invInputSize;
	vec2 phase = imageCoord-imagePixel;
	vec3 layerCoord = vec3(coord, float(gl_ViewIndex));
	vec4 left = textureGather(sceneColour, layerCoord, edgeChannel);
	float centreEdge = colour[edgeChannel];
	float edgeVote = abs(left.z-left.y) + abs(centreEdge-left.y) +
	                 abs(centreEdge-left.z);
	if(edgeVote <= edgeThreshold)
		return colour;

	coord.x += invInputSize.x;
	vec4 right = textureGather(sceneColour,
		vec3(coord + vec2(invInputSize.x, 0.0), float(gl_ViewIndex)),
		edgeChannel);
	vec4 upDown;
	upDown.xy = textureGather(sceneColour,
		vec3(coord + vec2(0.0, -invInputSize.y), float(gl_ViewIndex)),
		edgeChannel).wz;
	upDown.zw = textureGather(sceneColour,
		vec3(coord + vec2(0.0, invInputSize.y), float(gl_ViewIndex)),
		edgeChannel).yx;
	float mean = (left.y+left.z+right.x+right.w)*0.25;
	left -= vec4(mean);
	right -= vec4(mean);
	upDown -= vec4(mean);
	float centreDelta = centreEdge-mean;
	float sum = dot(abs(left), vec4(1.0)) +
	            dot(abs(right), vec4(1.0)) +
	            dot(abs(upDown), vec4(1.0));
	float sumMean = 10.14185/max(sum, 1.0e-5);
	vec3 filterData = vec3(sumMean*sumMean,
		SgsrEdgeDirection(left, right));
	vec2 accumulated = SgsrWeightY(phase.x, phase.y+1.0,
		upDown.x, filterData);
	accumulated += SgsrWeightY(phase.x-1.0, phase.y+1.0,
		upDown.y, filterData);
	accumulated += SgsrWeightY(phase.x-1.0, phase.y-2.0,
		upDown.z, filterData);
	accumulated += SgsrWeightY(phase.x, phase.y-2.0,
		upDown.w, filterData);
	accumulated += SgsrWeightY(phase.x+1.0, phase.y-1.0,
		left.x, filterData);
	accumulated += SgsrWeightY(phase.x, phase.y-1.0,
		left.y, filterData);
	accumulated += SgsrWeightY(phase.x, phase.y,
		left.z, filterData);
	accumulated += SgsrWeightY(phase.x+1.0, phase.y,
		left.w, filterData);
	accumulated += SgsrWeightY(phase.x-1.0, phase.y-1.0,
		right.x, filterData);
	accumulated += SgsrWeightY(phase.x-2.0, phase.y-1.0,
		right.y, filterData);
	accumulated += SgsrWeightY(phase.x-2.0, phase.y,
		right.z, filterData);
	accumulated += SgsrWeightY(phase.x-1.0, phase.y,
		right.w, filterData);
	float filtered = accumulated.y/max(accumulated.x, 1.0e-5);
	float maximum = max(max(left.y, left.z), max(right.x, right.w));
	float minimum = min(min(left.y, left.z), min(right.x, right.w));
	filtered = clamp(edgeSharpness*filtered, minimum, maximum);
	float delta = clamp(filtered-centreDelta,
	                   -23.0/255.0, 23.0/255.0);
	return clamp(colour+vec3(delta), vec3(0.0), vec3(1.0));
}

vec3 ApplyFxaa(vec2 uv, vec3 middle)
{
	vec2 texel = 1.0 / ImageSize();
	vec3 nw = SampleColour(uv + vec2(-1.0, -1.0) * texel);
	vec3 ne = SampleColour(uv + vec2( 1.0, -1.0) * texel);
	vec3 sw = SampleColour(uv + vec2(-1.0,  1.0) * texel);
	vec3 se = SampleColour(uv + vec2( 1.0,  1.0) * texel);
	float lm = Luma(middle);
	float lnw = Luma(nw);
	float lne = Luma(ne);
	float lsw = Luma(sw);
	float lse = Luma(se);
	float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
	float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));

	// Most of a Vice City frame is low contrast. Avoid the four directional
	// samples there, saving bandwidth on the Quest tiler.
	if(lmax - lmin < max(0.0312, lmax * 0.125))
		return middle;

	vec2 direction = vec2(-((lnw + lne) - (lsw + lse)),
	                       ((lnw + lsw) - (lne + lse)));
	float reduce = max((lnw + lne + lsw + lse) * 0.03125,
	                   0.0078125);
	direction = clamp(direction /
	                  (min(abs(direction.x), abs(direction.y)) + reduce),
	                  vec2(-8.0), vec2(8.0)) * texel;
	vec3 a = 0.5 * (
		SampleColour(uv + direction * (1.0 / 3.0 - 0.5)) +
		SampleColour(uv + direction * (2.0 / 3.0 - 0.5)));
	vec3 b = a * 0.5 + 0.25 * (
		SampleColour(uv + direction * -0.5) +
		SampleColour(uv + direction *  0.5));
	float lb = Luma(b);
	return (lb < lmin || lb > lmax) ? a : b;
}

vec3 FetchScene(ivec2 pixel)
{
	ivec2 size = ivec2(ImageSize());
	return texelFetch(sceneColour,
		ivec3(clamp(pixel, ivec2(0), size-ivec2(1)),
		      int(gl_ViewIndex)), 0).rgb;
}

float Sgsr2FastLanczos(float base)
{
	float y = base-1.0;
	float y2 = y*y;
	return (0.75*y+y2)*y2;
}

// Official Qualcomm SGSR2 two-pass fragment Upscale stage, adapted to the
// two-layer OpenXR target. Binding 1 contains the Convert pass result for this
// mode; binding 4 is the previous resolved output.
vec3 ApplyOfficialSgsr2(vec2 uv)
{
	vec2 renderSize = ImageSize();
	vec2 outputSize = OutputSize();
	vec2 renderRcp = 1.0/renderSize;
	vec2 jitter = temporal.sgsrParams.xy;
	vec2 jitterUv = clamp(uv+jitter*renderRcp, vec2(0.0), vec2(1.0));
	ivec2 inputPos = ivec2(jitterUv*renderSize);
	vec4 mda = textureLod(sceneMotion,
		vec3(jitterUv, float(gl_ViewIndex)), 0.0);
	vec2 motion = mda.xy;
	vec2 previousUv = clamp(uv-0.5*motion,
	                            vec2(0.0), vec2(1.0));
	mediump vec3 history = textureLod(sceneHistory,
		vec3(previousUv, float(gl_ViewIndex)), 0.0).rgb;

	float viewportScale = outputSize.x/renderSize.x;
	float areaScale = (outputSize.x*outputSize.y)/
	                  max(renderSize.x*renderSize.y, 1.0);
	float varianceScale = min(20.0, pow(areaScale, 3.0));
	float depthFactor = mda.z;
	float biasMin = max(1.0, 0.3+0.3*viewportScale);
	float kernelBias = mix(viewportScale, biasMin, 0.25*depthFactor)*0.5;
	float kernelBias2 = kernelBias*kernelBias;
	float motionViewportLength = length(motion*outputSize);
	// SGSR2's reference rejection is deliberately very aggressive while the
	// camera moves. In VR that means normal head rotation continually falls
	// back to the low-resolution current frame, so the accumulated AA appears
	// only while the head is still. Camera-only motion is already reprojected
	// by the exact per-eye matrices; cap only its rejection pressure. Preserve
	// the stock response for pixels carrying real object/root velocity.
	float rejectionMotionLength = mix(
		min(motionViewportLength, 10.0), motionViewportLength, mda.w);
	float curveBias = mix(-2.0, -3.0,
		clamp(rejectionMotionLength*0.02, 0.0, 1.0));
	vec2 sourcePos = vec2(inputPos)+vec2(0.5)-jitter;
	vec2 sourceDelta = sourcePos-uv*renderSize;

	// Qualcomm's reference upscaler performs colour, kernel and variance work
	// at mediump on Adreno. Keep coordinates/motion highp, but do not pay FP32
	// for the five-tap colour box at every output pixel in both eyes.
	mediump vec4 upsample = vec4(0.0);
	mediump vec3 boxCenter = vec3(0.0);
	mediump vec3 boxVariance = vec3(0.0);
	mediump float boxWeight = 0.0;
	mediump vec3 boxMin = vec3(65504.0);
	mediump vec3 boxMax = vec3(-65504.0);
	const ivec2 taps[5] = ivec2[5](
		ivec2(0, 1), ivec2(1, 0), ivec2(-1, 0),
		ivec2(0, 0), ivec2(0, -1));
	for(int i = 0; i < 5; i++){
		mediump vec3 colour = FetchScene(inputPos+taps[i]);
		vec2 offset = sourceDelta+vec2(taps[i]);
		mediump float distance2 = dot(offset, offset);
		mediump float base = clamp(distance2*kernelBias2, 0.0, 1.0);
		mediump float weight = Sgsr2FastLanczos(base);
		upsample += vec4(colour*weight, weight);
		mediump float varianceWeight = exp(distance2*curveBias);
		boxMin = min(boxMin, colour);
		boxMax = max(boxMax, colour);
		mediump vec3 weighted = colour*varianceWeight;
		boxCenter += weighted;
		boxVariance += colour*weighted;
		boxWeight += varianceWeight;
	}
	boxWeight = 1.0/max(boxWeight, 1.0e-7);
	boxCenter *= boxWeight;
	boxVariance = sqrt(abs(boxVariance*boxWeight-boxCenter*boxCenter));
	upsample.rgb = clamp(upsample.rgb/max(upsample.a, 1.0e-7),
	                     boxMin-vec3(0.075), boxMax+vec3(0.075));
	upsample.a /= 3.0;

	float baseUpdate = 1.0-depthFactor;
	baseUpdate = min(baseUpdate, mix(baseUpdate, upsample.a*10.0,
		clamp(10.0*rejectionMotionLength, 0.0, 1.0)));
	baseUpdate = min(baseUpdate, mix(baseUpdate, upsample.a,
		clamp(rejectionMotionLength*0.05, 0.0, 1.0)));
	float boxScale = max(depthFactor,
		clamp(rejectionMotionLength*0.05, 0.0, 1.0));
	mediump vec3 scaledVariance =
		boxVariance*mix(varianceScale, 1.0, boxScale);
	mediump vec3 constrainedMin = max(boxMin, boxCenter-scaledVariance);
	mediump vec3 constrainedMax = min(boxMax, boxCenter+scaledVariance);
	mediump vec3 clampedHistory =
		clamp(history, constrainedMin, constrainedMax);
	float contribution =
		(any(greaterThan(constrainedMin, history)) ||
		 any(greaterThan(history, constrainedMax))) ? 0.0 : 1.0;
	history = mix(clampedHistory, history, contribution);
	// Only relax a rejected static-history sample while the HMD is genuinely
	// moving. The old unconditional floor retained stale history through tiny
	// tracking motion and could feel like temporal drag while standing still.
	float cameraMotionPressure = (1.0-mda.w)*smoothstep(
		1.5, 12.0, motionViewportLength);
	float blendContribution = mda.w > 0.5 ? contribution :
		max(contribution, 0.12*cameraMotionPressure);
	float baseAlpha = mix(
		min(baseUpdate, 0.1), baseUpdate, blendContribution);
	float alpha = clamp(upsample.a/max(1.192e-7, baseAlpha+upsample.a) +
	                    temporal.sgsrParams.w, 0.0, 1.0);
	// Bound recursive history age in VR. With synthetic jitter removed, natural
	// HMD motion supplies new sub-pixel coverage; keeping at least 38% of the
	// current frame while still and 55% during a turn prevents the world from
	// subtly lagging behind the head without switching temporal AA fully off.
	if(mda.w < 0.5)
		alpha = max(alpha, mix(0.38, 0.55, cameraMotionPressure));
	if(temporal.temporalParams.z < 0.5)
		alpha = 1.0;
	return mix(history, upsample.rgb, alpha);
}

void main()
{
	if(postFx.mode.x == 2u){
		StoreBoth(vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}
	vec2 uv = gl_FragCoord.xy / OutputSize();
	if(postFx.upscale.z == 1u){
		vec2 motion = SampleCombinedMotion(uv);
		vec3 source = SampleColour(uv);
		if(postFx.mode.y != 0u)
			source = ApplyFxaa(uv, source);
		// A normal 72 Hz head turn moves only a few hundredths of NDC per
		// frame. The old x4 display looked almost uniformly grey even when the
		// vectors were correct. Amplify magnitude for diagnosis and retain the
		// source image underneath so menus, geometry and the tested object stay
		// readable. Direction is encoded as red/cyan for X and green/magenta
		// for Y; stationary pixels keep a neutral grey tint.
		float rawMagnitude = length(motion);
		// Suppress floating-point/reprojection noise, then apply a strictly
		// linear diagnostic gain. The previous square-root curve amplified tiny
		// stationary errors into full rainbow flicker.
		float magnitude = rawMagnitude > 2.0e-5 ?
			clamp((rawMagnitude-2.0e-5)*64.0, 0.0, 1.0) : 0.0;
		vec2 direction = rawMagnitude > 1.0e-7 ?
			motion/rawMagnitude : vec2(0.0);
		vec3 directionColour = clamp(vec3(
			0.5+0.5*direction.x,
			0.5+0.5*direction.y,
			0.5-0.25*(direction.x+direction.y)), vec3(0.0), vec3(1.0));
		vec3 motionColour = mix(vec3(0.5), directionColour, magnitude);
		StoreBoth(vec4(mix(source, motionColour, 0.58), 1.0));
		return;
	}
	if(postFx.upscale.z == 5u){
		vec4 source = vec4(ApplyOfficialSgsr2(uv), 1.0);
		if(postFx.mode.x != 1u){
			StoreBoth(source);
			return;
		}
		float alpha = postFx.blurColour.a;
		vec4 doubled = clamp(postFx.blurColour*2.0, 0.0, 1.0);
		vec4 previous = source;
		vec4 destination = source;
		for(int i = 0; i < 5; i++){
			vec4 filtered = destination*(1.0-alpha)+
			                previous*doubled*alpha;
			filtered += previous*postFx.blurColour;
			filtered += previous*postFx.blurColour;
			previous = clamp(filtered, 0.0, 1.0);
		}
		outResolvedHistory = source;
		outColour = vec4(previous.rgb, 1.0);
		return;
	}
	const bool temporalEnabled =
		postFx.upscale.z == 2u || postFx.upscale.z == 3u ||
		postFx.upscale.z == 4u;
	vec4 source = vec4(SampleColour(uv), 1.0);
	if(postFx.mode.y != 0u)
		source.rgb = ApplyFxaa(uv, source.rgb);
	if(temporalEnabled)
		source.rgb = ApplyTemporalStabilizer(uv, source.rgb);
	if(postFx.mode.x != 1u){
		StoreBoth(vec4(source.rgb, 1.0));
		return;
	}

	// Exact POSTFX_NORMAL colour filter used by the desktop Vice City path.
	float alpha = postFx.blurColour.a;
	vec4 doubled = clamp(postFx.blurColour * 2.0, 0.0, 1.0);
	vec4 destination = source;
	vec4 previous = destination;
	for(int i = 0; i < 5; i++){
		vec4 filtered = destination * (1.0 - alpha) +
		                previous * doubled * alpha;
		filtered += previous * postFx.blurColour;
		filtered += previous * postFx.blurColour;
		previous = clamp(filtered, 0.0, 1.0);
	}
	// History remains in linear game colour so the Vice City colour filter is
	// never applied twice when the result comes back on the next frame.
	outResolvedHistory = vec4(source.rgb, 1.0);
	outColour = vec4(previous.rgb, 1.0);
}
