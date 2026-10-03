#version 450

#include "rw_common.glsl"

// Alpha test is a specialisation constant rather than a uniform branch. On a
// tiler a discard in the shader disables early-Z for the whole draw, so the
// opaque world pass must compile to a variant that contains no discard at all.
layout(constant_id = 0) const int RW_ALPHA_TEST = 0;

layout(set = 1, binding = 0) uniform sampler2D diffuseTexture;
// The shared vehicle env-map art -- the streak texture the DFF materials
// reference -- bound once per frame; white when no vehicle has drawn yet.
layout(set = 3, binding = 0) uniform sampler2D envTexture;
// The previous frame's scene colour. The reflection block reprojects the
// reflected ray into it, so a car body shows the buildings, lamps and neon
// that actually stand around it -- the part of "looks like metal" a canned
// texture cannot provide.
layout(set = 3, binding = 1) uniform sampler2DArray envScene;
// The generated water wave normal map; see createWaterNormalTexture.
layout(set = 3, binding = 2) uniform sampler2D waterNormals;

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in float fragFog;
layout(location = 3) flat in vec2 fragDynamicMotion;
layout(location = 4) in vec3 fragWorldPos;
layout(location = 5) in vec3 fragNormal;

layout(location = 0) out vec4 outColour;
layout(location = 1) out vec2 outDynamicMotion;

// How much of the previous-frame lookup at this clip position survives:
// nothing behind or hugging the camera, and a soft ramp toward the frame
// edge so a turning head sweeps a fade instead of a hard border.
float ReflectionFade(vec4 clip)
{
	if(clip.w <= 0.1)
		return 0.0;
	vec2 ndc = clip.xy/clip.w;
	return clamp((1.0 - max(abs(ndc.x), abs(ndc.y)))*3.0, 0.0, 1.0)*
	       clamp(clip.w*0.5, 0.0, 1.0);
}

// True when the interface drew into this cell of the sampled frame. A help
// box is head-locked; mirrored, it would slide across the body with every
// head turn and read as the reflection wobbling.
bool CoveredByInterface(vec2 uv, int eye)
{
	ivec2 cell = ivec2(clamp(uv*16.0, vec2(0.0), vec2(15.99)));
	int bit = cell.y*16 + cell.x;
	int word = (eye == 0 ? 0 : 8) + (bit >> 5);
	return (scene.im2dCoverage[word >> 2][word & 3] &
	        (1u << (bit & 31))) != 0u;
}

void main()
{
	// Untextured materials bind a 1x1 white texture, so there is no separate
	// untextured variant to compile or select.
	vec4 texColour = texture(diffuseTexture, fragTexCoord);
	vec4 colour = fragColour * texColour;

	if(RW_ALPHA_TEST == 1){
		// The PS2 rule splits a blended mesh in two: the solid half is
		// drawn with depth writes, the faint half without. A negative
		// reference selects that second half, so both come out of one
		// shader without a pipeline variant of their own.
		const float reference = push.surfaceProps.w;
		if(reference >= 0.0){
			if(colour.a < reference)
				discard;
		}else if(colour.a >= -reference)
			discard;
	}

	// The game's dynamic lights, per pixel. The original fixed function turned
	// these into extra directionals and only for peds and vehicles; evaluating
	// them here means the road lights up under the headlights and a muzzle
	// flash reaches the walls. Which lights can touch this draw was decided on
	// the CPU: surfaceProps.z packs the surface mask in bits 0-7, the air-glow
	// mask in bits 8-15 and a real-normals flag in bit 16, so the draws away
	// from every light -- most of a frame -- skip everything here.
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	const bool vizActive = scene.vizParams.x > 0.5;
#endif
	if(RW_WORLD_EFFECTS != 0){
	const int lightBits = int(push.surfaceProps.z);
	const int surfaceMask = lightBits & 0xFF;
	const int glowMask = (lightBits >> 8) & 0xFF;
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	if(!vizActive && (surfaceMask | glowMask) != 0 &&
#else
	if((surfaceMask | glowMask) != 0 &&
#endif
	   push.surfaceProps.y > 0.0){
		const int lightTotal = int(scene.lightCount.x);
		const vec3 eye = scene.im2dParams.yzw;
		vec3 normal = vec3(0.0);
		if(surfaceMask != 0){
			if((lightBits & 0x10000) != 0){
				normal = normalize(fragNormal);
			}else{
				// Vice City's prelit world ships no normals -- the instancer
				// fills a dummy in -- so those draws take a facet normal
				// from the position derivatives instead.
				normal = cross(dFdx(fragWorldPos), dFdy(fragWorldPos));
				float len = length(normal);
				normal = len > 0.0 ? normal/len : vec3(0.0);
				// The winding is not trustworthy either way -- the fold
				// mirrors X -- so face the facet toward the viewer instead.
				if(dot(normal, eye - fragWorldPos) < 0.0)
					normal = -normal;
			}
		}
		vec3 rayDir = vec3(0.0);
		float rayLen = 0.0;
		if(glowMask != 0){
			rayDir = fragWorldPos - eye;
			rayLen = length(rayDir);
			rayDir = rayLen > 0.0 ? rayDir/rayLen : vec3(0.0);
		}
		vec3 lit = vec3(0.0);
		vec3 airGlow = vec3(0.0);
		for(int i = 0; i < lightTotal; i++){
			const int bit = 1 << i;
			if(((surfaceMask | glowMask) & bit) == 0)
				continue;
			const float radius = scene.lightPosRad[i].w;
			if((surfaceMask & bit) != 0){
				vec3 toLight = scene.lightPosRad[i].xyz - fragWorldPos;
				float distSq = dot(toLight, toLight);
				if(distSq < radius*radius && distSq > 0.0){
					float dist = sqrt(distSq);
					// CPointLights' own falloff: full strength inside half
					// the radius, linear to nothing at the edge.
					float intensity =
						clamp((1.0 - dist/radius)*2.0, 0.0, 1.0);
					vec3 dir = toLight/dist;
					if(scene.lightDir[i].w > 0.5)
						// And its cone rule for headlights: cos 60 degrees
						// wide, fading from the axis.
						intensity *= max(
							(dot(-dir, scene.lightDir[i].xyz)-0.5)*2.0,
							0.0);
					// Wrapped rather than clamped: a headlight grazes the
					// road at nearly ninety degrees, and a hard N.L erases
					// exactly the pool the light exists to draw.
					intensity *= clamp((dot(normal, dir)+0.5)/1.5,
					                   0.0, 1.0);
					lit += scene.lightColour[i].rgb*intensity;
				}
			}
			if((glowMask & bit) != 0){
				// Light in the air: how close the view ray passes to the
				// source. A lamp then reads as a soft glow against whatever
				// stands behind it, not only as its pool on the ground, and
				// geometry still occludes it correctly because the term only
				// exists on pixels whose ray gets near the light.
				vec3 toLight = scene.lightPosRad[i].xyz - eye;
				float along = clamp(dot(toLight, rayDir), 0.0, rayLen);
				vec3 offAxis = toLight - rayDir*along;
				float glowRadius = radius*0.4;
				float glow = 1.0 -
					dot(offAxis, offAxis)/(glowRadius*glowRadius);
				if(glow > 0.0){
					if(scene.lightDir[i].w > 0.5){
						// Shape a headlight's glow to its beam instead of a
						// ball around the bumper.
						vec3 fromLight = normalize(eye + rayDir*along -
							scene.lightPosRad[i].xyz);
						glow *= max(
							(dot(fromLight, scene.lightDir[i].xyz)-0.5)*2.0,
							0.0);
					}
					airGlow += scene.lightColour[i].rgb*
						(glow*glow*scene.lightCount.y);
				}
			}
		}
		// Same modulation chain as the vertex sum for the surface term; the
		// glow is light in the air and lands on top untinted.
		colour.rgb += lit*push.surfaceProps.y*push.materialColour.rgb*
		              texColour.rgb;
		colour.rgb += airGlow;
	}

	// Reflections for the MatFX env-map materials -- vehicle bodywork and
	// chrome. The original drew those as a second textured pass this backend
	// never picked up; here the same streak art the DFF references is
	// wrapped as a panorama around the reflected ray, so structure slides
	// across the body as the car or the head turns -- which is what reads as
	// a reflection where a plain gradient only reads as brighter paint. The
	// timecycle tints it (fog at the horizon, sky above) and the sun adds a
	// glint.
	const int envBits = (lightBits >> 17) & 0x3F;
	// From the driver's seat the car's own body and glass wrap the entire
	// view, so without the distance gate the reflection block became a
	// full-screen pass the moment the player sat down -- the cockpit frame
	// drops -- and what it drew there was mostly smears of the car itself.
	// Fading in between 1.1m and 1.9m from the eye turns the dashboard and
	// windscreen off outright, keeps the bonnet, and leaves every other car
	// untouched.
	// The water mask atomic carries MatFX; the water block below is its
	// reflection, so it must not run this one too.
	if(envBits != 0 && scene.reflectionParams.x > 0.0 &&
	   (lightBits & 0x1000000) == 0){
		vec3 fromEye = fragWorldPos - scene.im2dParams.yzw;
		const float eyeDistSq = dot(fromEye, fromEye);
		if(eyeDistSq > 1.21){
		const float eyeDist = sqrt(eyeDistSq);
		const float envFade = clamp(eyeDist*1.25 - 1.375, 0.0, 1.0);
		vec3 reflNormal = normalize(fragNormal);
		vec3 viewDir = fromEye/eyeDist;
		vec3 reflected = reflect(viewDir, reflNormal);

		// Fallback layer: the DFF's streak art wrapped as a panorama around
		// the reflected ray, tinted by the timecycle. The epsilon keeps
		// atan defined when the ray points straight up.
		vec2 envUV = vec2(atan(reflected.x, reflected.z + 1e-5)*0.159155
		                  + 0.5,
		                  0.5 - reflected.y*0.5);
		// An explicit level, not the derivative of that coordinate. The
		// block sits inside a branch only part of a quad takes, where
		// derivatives are not defined to begin with, and the atan above
		// wraps once per turn around the car, so the implicit level swung
		// from pixel to pixel across a curved panel. The art is a small
		// streak sheet; its base level costs nothing to keep resident.
		vec3 streaks = textureLod(envTexture, envUV, 0.0).rgb;
		float up = clamp(reflected.y*0.5 + 0.5, 0.0, 1.0);
		vec3 tint = mix(scene.fogColour.rgb, scene.skyColour.rgb, up*up);
		vec3 env = streaks*(tint*1.6);

		// The real surroundings: walk a short way along the reflected ray
		// and look that point up in the previous frame's image, through the
		// reprojection that already carries the camera-fold delta -- without
		// it the reflection swims by one frame of motion and snaps back.
		// Wrong by one frame of content and by the fixed march distance,
		// but it puts the actual lamp post, wall and neon onto the body,
		// and that movement is what reads as metal.
		// The march distance is a guess at how far the reflected thing
		// stands. Downward rays hit the road right under the car and
		// upward ones leave for buildings and sky; one fixed middle value
		// made both swim against head movement, since the parallax error
		// grows with the gap between the guess and the truth.
		// The player's own vehicle stays on the panorama: from the driver's
		// seat the screen-space lookup mostly finds the car itself, and the
		// cockpit magnifies every artefact of the technique. Bit 23 comes
		// from the CPU, which knows which car is occupied.
		if((lightBits & 0x800000) == 0 && scene.reflectionParams.y > 0.0){
		float march = 10.0 + reflected.y*(reflected.y > 0.0 ? 18.0 : 7.0);
		vec4 probe = vec4(fragWorldPos + reflected*march, 1.0);
		// The lookup is MONO: both eyes project through the left eye's
		// matrix and fetch the left layer, so the reflected content is
		// identical by construction and binocular rivalry is impossible --
		// per-eye lookups fetched genuinely different pictures whenever
		// the march guess missed the true depth. The reflection reads as
		// laid on the body, exactly like the panorama layer, and one
		// fetch instead of two halves the scattered samples. Worth
		// having, but not where the cost is: quartering the sampled
		// image changed nothing measurable, and a strength of zero --
		// which skips the fetch outright -- costs about what full
		// strength does. What this block spends goes on the arithmetic
		// around it, per pixel of bodywork on screen.
		vec4 clip = scene.reflectionReproject[0]*probe;
		float fade = ReflectionFade(clip);
		// The player-set SSR range: past it the car eases onto the
		// panorama over four metres instead of cutting.
		fade *= clamp((scene.reflectionParams.z - eyeDist)*0.25,
		              0.0, 1.0);
		if(fade > 0.0){
			vec2 uv = clip.xy/clip.w*0.5 + 0.5;
			if(CoveredByInterface(uv, 0))
				fade = 0.0;
			else{
				vec3 seen =
					textureLod(envScene, vec3(uv, 0.0), 0.0).rgb;
				env = mix(env, seen*1.2,
					clamp(fade*scene.reflectionParams.y, 0.0, 1.0));
			}
		}

		}
		vec4 sun = unpackUnorm4x8(floatBitsToUint(push.lightDirColour.w));
		float glint = max(dot(reflected, -push.lightDirColour.xyz), 0.0);
		env += sun.rgb*pow(glint, 24.0);
		colour.rgb += env*(envFade*scene.reflectionParams.x*
		                   float(envBits)*(1.0/63.0));
		}
	}

	// MODERN WATER, on the draws the game marks as water -- both the far
	// immediate-mode sectors and the near wavy/mask atomics. The wave
	// shape comes from the generated normal map, scrolled as two layers in
	// different directions -- the Half-Life recipe, wired to the previous
	// frame instead of a second render pass: at grazing angles the water
	// becomes a Fresnel-weighted mirror of what stood on screen, looking
	// down it stays Rockstar's water art. Only the flat sky fallback is
	// kept additive and small -- mixing that one in washed the authored
	// texture out once already. One integer test when off.
	if((lightBits & 0x1000000) != 0){
		const float t = scene.reflectionParams.w*scene.waterParams1.y;
		// True world coordinates: play space moves with the camera and
		// waves built from it would race across the sea.
		const vec2 wp =
			(scene.playToWorld*vec4(fragWorldPos, 1.0)).xy;
		// Two layers at different scale, heading and speed; their
		// disagreement hides the tiling.
		vec3 layerA = texture(waterNormals,
			wp*0.061 + t*vec2(0.023, 0.012)).rgb*2.0 - 1.0;
		vec3 layerB = texture(waterNormals,
			wp*0.017 - t*vec2(0.008, 0.015)).rgb*2.0 - 1.0;
		vec2 slope = (layerA.xy + layerB.xy*0.7)*scene.waterParams1.x;
		// Play space is Y up; the horizontal swap only turns the wave
		// anisotropy, which the noise hides.
		vec3 normal = normalize(vec3(slope.x*0.38, 1.0, slope.y*0.38));

		vec3 fromEye = fragWorldPos - scene.im2dParams.yzw;
		float eyeDist = max(length(fromEye), 0.001);
		vec3 viewDir = fromEye/eyeDist;
		vec3 reflected = reflect(viewDir, normal);
		// A ripple can fold the ray under the surface; a mirror cannot.
		reflected.y = abs(reflected.y);
		// The lookup ray leaves the FLAT plane and the waves distort the
		// fetched picture instead of steering the ray: a building in the
		// water stays a building with a wobble, where a wave-bent ray
		// smeared it across whatever stood near the horizon.
		vec3 mirror = vec3(viewDir.x, max(-viewDir.y, 0.02), viewDir.z);

		float facing = clamp(dot(normal, -viewDir), 0.0, 1.0);
		float fresnel = pow(1.0 - facing, 5.0);

		// The probe rises a fixed eight metres off the plane whatever
		// the ray's pitch. A march that grew with distance kept the
		// probe ON the water across a bay, and the previous frame's
		// water there holds this block's own output -- the feedback
		// loop read as marching stripes. Above the waterline it lands
		// on buildings and sky, which cannot feed back. The lookup
		// stays MONO -- left matrix, left layer -- like the car block
		// above, so the eyes cannot disagree.
		float seenWeight = 0.0;
		if(scene.waterParams1.w > 0.0){
		float march = clamp(8.0/max(mirror.y, 0.04), 12.0, 260.0);
		vec4 clip = scene.reflectionReproject[0]*
			vec4(fragWorldPos + mirror*march, 1.0);
		float fade = ReflectionFade(clip);
		if(fade > 0.0){
			vec2 uv = clip.xy/clip.w*0.5 + 0.5 +
				slope*(0.015*scene.waterParams1.z);
			if(!CoveredByInterface(uv, 0)){
				// One-texel tent: the reduced copy comes from a
				// single blit and aliases, and a flat mirror
				// shows that moire as shimmering stripes.
				vec2 px = 0.6/vec2(textureSize(envScene, 0).xy);
				vec3 seen = (textureLod(envScene,
					vec3(uv + px, 0.0), 0.0).rgb +
					textureLod(envScene,
					vec3(uv - px, 0.0), 0.0).rgb +
					textureLod(envScene, vec3(
					uv + vec2(px.x, -px.y), 0.0), 0.0).rgb +
					textureLod(envScene, vec3(
					uv - vec2(px.x, -px.y), 0.0), 0.0).rgb)*
					0.25;
				seenWeight = fade*clamp((fresnel*1.5 + 0.12)*
					scene.waterParams1.w, 0.0, 1.0);
				colour.rgb = mix(colour.rgb, seen, seenWeight);
			}
		}
		}
		// Where the lookup has no answer -- screen edges, cells the
		// interface drew over -- a little sky lands additively instead,
		// so the mirror does not cut off at the frame border.
		vec3 sheen = mix(scene.fogColour.rgb, scene.skyColour.rgb,
		                 clamp(mirror.y*2.0, 0.0, 1.0));
		colour.rgb += sheen*((1.0 - seenWeight)*(fresnel*0.30 + 0.03)*
		                     scene.waterParams2.x);

		if(scene.waterParams2.y > 0.0){
		vec4 sunColour =
			unpackUnorm4x8(floatBitsToUint(scene.waterSun.w));
		float glint = pow(max(dot(reflected, scene.waterSun.xyz), 0.0),
		                  96.0);
		colour.rgb += sunColour.rgb*(glint*1.4*scene.waterParams2.y);
		}

		// The dynamic lights lay their sparks across the waves: a street
		// lamp or a headlight beside the seafront is what carries the
		// water at night, when the sky has nothing to offer. Skipped
		// outright at zero: the slider only scaled the sum, so turning
		// it off still walked every light on every water pixel.
		const int waterLights = scene.waterParams2.z > 0.0 ?
			int(scene.lightCount.x) : 0;
		vec3 sparks = vec3(0.0);
		for(int i = 0; i < waterLights; i++){
			vec3 toLight = scene.lightPosRad[i].xyz - fragWorldPos;
			float lightDist = length(toLight);
			float reach = scene.lightPosRad[i].w*2.0;
			if(lightDist >= reach || lightDist <= 0.0)
				continue;
			float spark = pow(max(dot(reflected, toLight/lightDist), 0.0),
			                  64.0);
			sparks += scene.lightColour[i].rgb*
				(spark*(1.0 - lightDist/reach)*1.5);
		}
		colour.rgb += sparks*scene.waterParams2.z;
	}

	}
#if defined(MIAMIVR_DEV_TOOLS) && MIAMIVR_DEV_TOOLS
	// Culling visualizer: paint the draw its debug colour. The index is the
	// whole of surfaceProps.z here (the pack was suppressed on the CPU), so
	// the texture still reads through underneath at 40 percent.
	if(vizActive){
		const vec3 pal[5] = vec3[5](vec3(0.0),
			vec3(0.20, 0.45, 1.00),   // visible
			vec3(1.00, 0.14, 0.10),   // frustum-culled
			vec3(1.00, 0.55, 0.10),   // occluder-culled
			vec3(1.00, 0.90, 0.15));  // distance / LOD
		int idx = clamp(int(push.surfaceProps.z), 0, 4);
		if(idx != 0)
			colour.rgb = mix(colour.rgb, pal[idx], 0.6);
	}
#endif

	colour.rgb = mix(scene.fogColour.rgb, colour.rgb, fragFog);
	outColour = colour;
	// Compact dynamic-vector transport. The post pass divides by the same gain.
	outDynamicMotion = clamp(fragDynamicMotion*8.0, vec2(-1.0), vec2(1.0));
}
