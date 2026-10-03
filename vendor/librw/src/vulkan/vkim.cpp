#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwvk.h"
#include "rwvkimpl.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_VULKAN

#include <android/log.h>

namespace rw {
namespace vulkan {

namespace {

// im3DTransform hands over a vertex array and a world matrix; the following
// RenderPrimitive / RenderIndexedPrimitive calls consume them, and im3DEnd
// drops them. The state has to survive between those calls.
struct Im3DState
{
	Im3DVertex *vertices;
	int32 numVertices;
	float32 model[16];
} gim3d;

static bool32 gImmediate2DStrictDepth;

// Triangle fans never reach the device.
//
// Creating a pipeline with VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN does not return
// on this Adreno driver -- not slowly, at all: vkCreateGraphicsPipelines was
// still inside the compiler after 80 seconds at full CPU. Fans are a weak spot
// across mobile GPUs generally, and the game leans on them: CSprite2d::DrawRect
// draws every rectangle as one, so the very first menu backdrop hit it.
//
// They are expanded into triangle lists on the CPU instead. A fan of n vertices
// is n-2 triangles sharing vertex 0, which costs one small index buffer out of
// the per-frame allocator and removes the dependency entirely.
VkPrimitiveTopology
topologyFromPrimitiveType(PrimitiveType type)
{
	switch(type){
	case PRIMTYPEPOINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case PRIMTYPELINELIST:  return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case PRIMTYPEPOLYLINE:  return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case PRIMTYPETRILIST:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	case PRIMTYPETRISTRIP:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	case PRIMTYPETRIFAN:    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	default:                return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}
}

// Expands a fan into a triangle list. sourceIndices may be nil, in which case
// the fan is over vertices 0..vertexCount-1. Returns the number of indices
// written, or 0 when there is no triangle to draw.
int32
expandTriangleFan(const uint16 *sourceIndices, int32 sourceCount,
                  uint16 *out, int32 outCapacity)
{
	if(sourceCount < 3)
		return 0;
	const int32 triangles = sourceCount - 2;
	if(triangles*3 > outCapacity)
		return 0;

	int32 written = 0;
	for(int32 i = 1; i < sourceCount - 1; i++){
		out[written++] = sourceIndices ? sourceIndices[0] : 0;
		out[written++] = sourceIndices ? sourceIndices[i] : (uint16)i;
		out[written++] = sourceIndices ? sourceIndices[i+1] : (uint16)(i+1);
	}
	return written;
}

void
identity(float32 *m)
{
	memset(m, 0, sizeof(float32)*16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Returns false when no pipeline could be built. Callers must not draw in that
// case: issuing vkCmdDraw with nothing bound faults inside the driver rather
// than being caught as a validation error.
bool32
setupCommonState(VkCommandBuffer commandBuffer, uint32 shader,
                 VkPrimitiveTopology topology, const float32 *model)
{
	VkPipeline pipeline = getPipeline(shader, topology);
	if(pipeline == VK_NULL_HANDLE)
		return 0;
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	Raster *raster = (Raster*)GetRenderStatePtr(TEXTURERASTER);
	// The masked-geometry mip bias belongs to the world pipe and is
	// part of the sampler key; left at whatever the last atomic set,
	// a particle with a mip chain would soften by however much the
	// foliage before it asked for.
	gstate.mipLodBias = 0;
	VkDescriptorSet textureSet = getTextureDescriptor(raster);
	vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                        getPipelineLayout(), 1, 1, &textureSet, 0, nil);

	// Zero-initialised: immediate geometry never uses the lighting fields, and
	// garbage in them would still reach the shader.
	PushConstants push = {};
	// Same per-draw fog switch the atomic pipeline uses; see vkpipe.cpp.
	push.ambientLight[3] = gstate.fogEnabled ? 1.0f : 0.0f;
	// Im3D is world space, so it needs the game camera folded in like the
	// world pipeline does. Im2D is not: it anchors its panel in play space
	// through scene.im2dTransform and must stay clear of the game camera.
	if(shader == SHADER_IM2D)
		// The plane the interface lands on travels per draw, so the radar
		// can be moved onto the wrist without disturbing the rest of it.
		memcpy(push.model, gvk.im2dTransformActive, sizeof(push.model));
	else
		multiplyMatrix(push.model, gvk.worldToPlay, model);
	if(gvk.sgsrMode != SGSR_OFF && shader != SHADER_IM2D){
		// Immediate geometry has no persistent object identity/history. Keep its
		// packed root vector at zero so the post pass uses depth reconstruction.
		// Im2D is exempt: its model slot now carries the interface plane, and
		// clearing that column would flatten the matrix.
		push.model[3] = push.model[7] = push.model[11] = push.model[15] = 0.0f;
	}
	push.materialColour[0] = push.materialColour[1] =
	push.materialColour[2] = push.materialColour[3] = 1.0f;
	// Doubles as the Im2D target select. The wrist minimap renders in a pass
	// of its own, where the model slot carries a straight screen-pixels-to-
	// clip matrix rather than a plane in the world; nothing else in the
	// immediate shaders reads this field.
	push.surfaceProps[0] =
		shader == SHADER_IM2D && gvk.wristPanelOffscreen ? 2.0f : 1.0f;
	push.surfaceProps[1] = 0.0f;	// immediate geometry is never lit
	// The strict radar pass is pinned to the near depth plane. This keeps the
	// map visible through a vehicle cockpit while preserving the original
	// equal-depth circular mask. For Im3D the slot instead carries the
	// modern-water bit the fragment shader's water block keys off.
	if(shader == SHADER_IM2D)
		push.surfaceProps[2] = gImmediate2DStrictDepth ? 1.0f : 0.0f;
	else
		push.surfaceProps[2] = gvk.modernWater && gvk.im3dWaterDraw ?
			(float32)0x1000000u : 0.0f;
	push.surfaceProps[3] = gstate.alphaTestRef / 255.0f;
	vkCmdPushConstants(commandBuffer, getPipelineLayout(),
	                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	                   0, sizeof(push), &push);
	return 1;
}

// Builds the screen-space-to-clip remap the Im2D vertex shader expects.
// Vulkan's NDC has y = -1 at the top, which matches screen space directly, so
// both axes use the same sign, unlike the OpenGL backend.
void
makeIm2DTransform(float32 *model)
{
	identity(model);
	const float32 width = (float32)gvk.width;
	const float32 height = (float32)gvk.height;
	model[0] = width > 0.0f ? 2.0f/width : 0.0f;
	model[1] = height > 0.0f ? 2.0f/height : 0.0f;
	model[2] = -1.0f;
	model[3] = -1.0f;
}

bool32
uploadVertices(const void *data, VkDeviceSize size, VkBuffer *bufferOut,
               VkDeviceSize *offsetOut)
{
	void *mapped = nil;
	if(!allocateDynamic(size, 16, bufferOut, offsetOut, &mapped))
		return 0;
	memcpy(mapped, data, (size_t)size);
	return 1;
}

bool32
uploadIndices(const void *data, VkDeviceSize size, VkBuffer *bufferOut,
              VkDeviceSize *offsetOut)
{
	void *mapped = nil;
	if(!allocateDynamic(size, 4, bufferOut, offsetOut, &mapped))
		return 0;
	memcpy(mapped, data, (size_t)size);
	return 1;
}

// Produces the index buffer to draw with, expanding a triangle fan when the
// game asks for one. countOut is 0 for a plain non-indexed draw.
bool32
resolveIndices(PrimitiveType primType, const uint16 *indices, int32 numIndices,
               int32 numVertices, VkBuffer *bufferOut, VkDeviceSize *offsetOut,
               int32 *countOut)
{
	*countOut = 0;

	if(primType == PRIMTYPETRIFAN){
		const int32 source = indices != nil && numIndices > 0 ?
			numIndices : numVertices;
		if(source < 3)
			return 0;
		const int32 count = (source - 2) * 3;
		void *mapped = nil;
		if(!allocateDynamic((VkDeviceSize)count*sizeof(uint16), 4,
		                    bufferOut, offsetOut, &mapped))
			return 0;
		*countOut = expandTriangleFan(indices, source, (uint16*)mapped, count);
		return *countOut > 0;
	}

	if(indices != nil && numIndices > 0){
		void *mapped = nil;
		if(!allocateDynamic((VkDeviceSize)numIndices*sizeof(uint16), 4,
		                    bufferOut, offsetOut, &mapped))
			return 0;
		memcpy(mapped, indices, (size_t)numIndices*sizeof(uint16));
		*countOut = numIndices;
		return 1;
	}
	return 1;
}

// Draw accounting. Distinguishes "the game never asked" from "it asked and we
// dropped it", which look identical from outside: both leave the GPU idle.
static uint32 gIm2DCalls, gIm2DDropped, gIm2DDrawn;
static float32 gIm2DSafeAreaScaleX = 1.0f;
static float32 gIm2DSafeAreaScaleY = 1.0f;
static float32 gIm2DSafeAreaOffsetX;
static float32 gIm2DSafeAreaOffsetY;
static bool32 gIm2DSafeAreaSuspended;

bool32
uploadIm2DVertices(const Im2DVertex *source, int32 count,
                   VkBuffer *bufferOut, VkDeviceSize *offsetOut)
{
	const VkDeviceSize size = (VkDeviceSize)count*sizeof(Im2DVertex);
	void *mapped = nil;
	if(!allocateDynamic(size, 16, bufferOut, offsetOut, &mapped))
		return 0;

	// The wrist minimap is exempt in both directions: its own target has no
	// safe area, and the quad on the arm carries a transform of its own that
	// a remap around the panel centre would drag off the wrist.
	if(gvk.wristPanelOffscreen || gIm2DSafeAreaSuspended ||
	   (fabsf(gIm2DSafeAreaScaleX-1.0f) < 0.0001f &&
	    fabsf(gIm2DSafeAreaScaleY-1.0f) < 0.0001f &&
	    fabsf(gIm2DSafeAreaOffsetX) < 0.0001f &&
	    fabsf(gIm2DSafeAreaOffsetY) < 0.0001f)){
		memcpy(mapped, source, (size_t)size);
		return 1;
	}

	Im2DVertex *destination = (Im2DVertex*)mapped;
	const float32 centreX = (float32)gvk.width*0.5f;
	const float32 centreY = (float32)gvk.height*0.5f;
	for(int32 i = 0; i < count; i++){
		destination[i] = source[i];
		destination[i].x =
			centreX+(source[i].x-centreX)*gIm2DSafeAreaScaleX+
			gIm2DSafeAreaOffsetX;
		destination[i].y =
			centreY+(source[i].y-centreY)*gIm2DSafeAreaScaleY+
			gIm2DSafeAreaOffsetY;
	}
	return 1;
}

void
logImStats(void)
{
	// This used to print every frame because calls always increases. Android
	// logcat then did formatted I/O at headset refresh rate for a diagnostic
	// whose useful event is a dropped draw. Report the initial state and only
	// report again when that event count changes.
	static uint32 lastDropped = 0xFFFFFFFF;
	if(gIm2DDropped == lastDropped)
		return;
	lastDropped = gIm2DDropped;
	__android_log_print(ANDROID_LOG_INFO, "librw-vk",
	                    "im2d: %u calls, %u drawn, %u dropped",
	                    gIm2DCalls, gIm2DDrawn, gIm2DDropped);
}

void
drawIm2D(PrimitiveType primType, void *vertices, int32 numVertices,
         void *indices, int32 numIndices)
{
	gIm2DCalls++;
	if(!gvk.inFrame || numVertices <= 0){
		gIm2DDropped++;
		return;
	}

	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceSize vertexOffset = 0;
	if(!uploadIm2DVertices((const Im2DVertex*)vertices, numVertices,
	                      &vertexBuffer, &vertexOffset))
		return;

	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceSize indexOffset = 0;
	int32 indexCount = 0;
	if(!resolveIndices(primType, (const uint16*)indices, numIndices,
	                   numVertices, &indexBuffer, &indexOffset, &indexCount))
		return;

	VkCommandBuffer commandBuffer = gvk.frameCommands;
	float32 model[16];
	makeIm2DTransform(model);
	if(!setupCommonState(commandBuffer, SHADER_IM2D,
	                     topologyFromPrimitiveType(primType), model)){
		gIm2DDropped++;
		return;
	}
	gIm2DDrawn++;

	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);

	if(indexCount > 0){
		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, indexOffset,
		                     VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(commandBuffer, (uint32)indexCount, 1, 0, 0, 0);
	}else{
		vkCmdDraw(commandBuffer, (uint32)numVertices, 1, 0, 0);
	}
}

void
drawIm3D(PrimitiveType primType, void *indices, int32 numIndices)
{
	if(!gvk.inFrame || gim3d.vertices == nil || gim3d.numVertices <= 0)
		return;

	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceSize vertexOffset = 0;
	if(!uploadVertices(gim3d.vertices,
	                   (VkDeviceSize)gim3d.numVertices*sizeof(Im3DVertex),
	                   &vertexBuffer, &vertexOffset))
		return;

	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceSize indexOffset = 0;
	int32 indexCount = 0;
	if(!resolveIndices(primType, (const uint16*)indices, numIndices,
	                   gim3d.numVertices, &indexBuffer, &indexOffset, &indexCount))
		return;

	VkCommandBuffer commandBuffer = gvk.frameCommands;
	if(!setupCommonState(commandBuffer, SHADER_IM3D,
	                     topologyFromPrimitiveType(primType), gim3d.model))
		return;

	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexOffset);

	if(indexCount > 0){
		vkCmdBindIndexBuffer(commandBuffer, indexBuffer, indexOffset,
		                     VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(commandBuffer, (uint32)indexCount, 1, 0, 0, 0);
	}else{
		vkCmdDraw(commandBuffer, (uint32)gim3d.numVertices, 1, 0, 0);
	}
}

} // namespace

void
setIm2DSafeAreaScale(float32 scale)
{
	setIm2DSafeAreaTransform(scale, scale, 0.0f, 0.0f);
}

void
setIm2DSafeAreaTransform(float32 scaleX, float32 scaleY,
                         float32 offsetX, float32 offsetY)
{
	gIm2DSafeAreaScaleX = scaleX < 0.1f ? 0.1f :
		(scaleX > 2.0f ? 2.0f : scaleX);
	gIm2DSafeAreaScaleY = scaleY < 0.1f ? 0.1f :
		(scaleY > 2.0f ? 2.0f : scaleY);
	gIm2DSafeAreaOffsetX = offsetX;
	gIm2DSafeAreaOffsetY = offsetY;
}

void
setIm2DSafeAreaSuspended(bool32 suspended)
{
	gIm2DSafeAreaSuspended = suspended;
}

void
setImmediate2DStrictDepth(bool32 enabled)
{
	gImmediate2DStrictDepth = enabled;
}

bool32
getImmediate2DStrictDepth(void)
{
	return gImmediate2DStrictDepth;
}

void
reportImStats(void)
{
	logImStats();
}

// ---------------------------------------------------------------------------
// Device entry points
// ---------------------------------------------------------------------------

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	uint16 indices[2] = { (uint16)vert1, (uint16)vert2 };
	drawIm2D(PRIMTYPELINELIST, vertices, numVertices, indices, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2,
                   int32 vert3)
{
	uint16 indices[3] = { (uint16)vert1, (uint16)vert2, (uint16)vert3 };
	drawIm2D(PRIMTYPETRILIST, vertices, numVertices, indices, 3);
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	drawIm2D(primType, vertices, numVertices, nil, 0);
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices,
                           int32 numVertices, void *indices, int32 numIndices)
{
	drawIm2D(primType, vertices, numVertices,
	         indices, numIndices);
}

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	gim3d.vertices = (Im3DVertex*)vertices;
	gim3d.numVertices = numVertices;

	if(world == nil){
		identity(gim3d.model);
	}else{
		gim3d.model[0]  = world->right.x; gim3d.model[1]  = world->right.y;
		gim3d.model[2]  = world->right.z; gim3d.model[3]  = 0.0f;
		gim3d.model[4]  = world->up.x;    gim3d.model[5]  = world->up.y;
		gim3d.model[6]  = world->up.z;    gim3d.model[7]  = 0.0f;
		gim3d.model[8]  = world->at.x;    gim3d.model[9]  = world->at.y;
		gim3d.model[10] = world->at.z;    gim3d.model[11] = 0.0f;
		gim3d.model[12] = world->pos.x;   gim3d.model[13] = world->pos.y;
		gim3d.model[14] = world->pos.z;   gim3d.model[15] = 1.0f;
	}
	(void)flags;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	drawIm3D(primType, nil, 0);
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	drawIm3D(primType, indices, numIndices);
}

void
im3DEnd(void)
{
	gim3d.vertices = nil;
	gim3d.numVertices = 0;
}

}
}

#endif
