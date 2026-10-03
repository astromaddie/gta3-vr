#version 450
#ifndef RW_IM2D_COVERAGE
#define RW_IM2D_COVERAGE 1
#endif
// For gl_ViewIndex: the coverage mask is kept per eye, because the interface
// plane projects to slightly different screen cells in each.
#extension GL_EXT_multiview : enable

#include "rw_common.glsl"

layout(constant_id = 0) const int RW_ALPHA_TEST = 0;

layout(set = 1, binding = 0) uniform sampler2D diffuseTexture;

// Which coarse screen cells the interface touched this frame. The reflection
// block reads it back two frames later and refuses to mirror those cells, so
// a head-locked help box never slides across a car body. Written with
// atomics from a sparse subset of fragments -- a cell is a sixteenth of
// the screen wide, so every fourth pixel in each axis still marks them all.
#if RW_IM2D_COVERAGE
layout(std430, set = 0, binding = 1) buffer CoverageBuffer {
	uint bits[16];
} coverage;
#endif

layout(location = 0) in vec4 fragColour;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) flat in float fragWorldSprite;

layout(location = 0) out vec4 outColour;
layout(location = 1) out vec2 outDynamicMotion;

void main()
{
	vec4 colour = fragColour * texture(diffuseTexture, fragTexCoord);
	if(RW_ALPHA_TEST == 1){
		if(colour.a < push.surfaceProps.w)
			discard;
	}
	// No fog on 2D: the HUD, fonts and menus are composited, not in the world.
	outColour = colour;
	// The Im2D pipeline disables attachment-1 writes. Keep a conventional zero
	// output for interface compatibility; it never reaches the motion image.
	outDynamicMotion = vec2(0.0);

	// Mark the coverage cell. Skip the wrist-panel offscreen target -- its
	// coordinates are panel pixels, not scene pixels -- and every WORLD
	// sprite: a corona or a headlight glow is scenery, its light belongs in
	// a reflection, and masking it carved a travelling square around every
	// street lamp. Everything else that leaves a visible trace masks: a
	// bright-glyphs-only threshold was tried here and the translucent grey
	// help box still read clearly as a grey slab on a car body, so the
	// cut-off only spares pixels too faint to tint anything.
#if RW_IM2D_COVERAGE
	const float ghost = colour.a*max(colour.r, max(colour.g, colour.b));
	if(push.surfaceProps.x < 1.5 && fragWorldSprite < 0.5 &&
	   ghost > 0.04 &&
	   ((uint(gl_FragCoord.x) | uint(gl_FragCoord.y)) & 3u) == 0u){
		vec2 cellUv = gl_FragCoord.xy /
			max(scene.lightCount.zw, vec2(1.0));
		ivec2 cell = ivec2(clamp(cellUv*16.0, vec2(0.0), vec2(15.99)));
		int bit = cell.y*16 + cell.x;
		int word = (gl_ViewIndex == 0 ? 0 : 8) + (bit >> 5);
		atomicOr(coverage.bits[word], 1u << (bit & 31));
	}
#endif
}
