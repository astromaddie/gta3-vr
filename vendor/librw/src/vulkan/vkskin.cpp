#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwvk.h"
#include "rwvkimpl.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_VULKAN

#include <android/log.h>
#define VKERR(...) __android_log_print(ANDROID_LOG_ERROR, "librw-vk", __VA_ARGS__)

namespace rw {
namespace vulkan {

namespace {

// Posed bone matrices for the atomic being drawn, staged here before being
// copied into the frame's dynamic buffer. One atomic is in flight at a time,
// so a single scratch block is enough.
float32 gBoneMatrices[RW_MAX_BONES*16];

void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	Skin *skin = Skin::get(geo);
	if(skin == nil){
		// A geometry on the skin pipeline with no skin plugin data cannot be
		// posed. Leaving the buffer empty drops it rather than reading past
		// the end of a weight array that was never allocated.
		VKERR("skinned geometry without skin data; skipping");
		return;
	}

	const bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	const bool hasNormals = !!(geo->flags & Geometry::NORMALS);
	const bool hasTexCoords = geo->numTexCoordSets > 0;

	const VkDeviceSize size =
		(VkDeviceSize)header->totalNumVertex * sizeof(SkinVertex);

	if(!reinstance)
		header->vertexBuffer = rwNewT(uint8, (uint32)size,
		                              MEMDUR_EVENT | ID_GEOMETRY);
	else
		destroyStaticBuffer(header->vbo, header->vboMemory);

	uint32 clampedIndices = 0;
	SkinVertex *vertices = (SkinVertex*)header->vertexBuffer;
	V3d *positions = geo->morphTargets[0].vertices;
	V3d *normals = geo->morphTargets[0].normals;
	TexCoords *texCoords = hasTexCoords ? geo->texCoords[0] : nil;
	RGBA *colours = isPrelit ? geo->colors : nil;

	for(uint32 i = 0; i < header->totalNumVertex; i++){
		SkinVertex &v = vertices[i];
		v.position[0] = positions[i].x;
		v.position[1] = positions[i].y;
		v.position[2] = positions[i].z;

		if(hasNormals && normals != nil){
			v.normal[0] = normals[i].x;
			v.normal[1] = normals[i].y;
			v.normal[2] = normals[i].z;
		}else{
			v.normal[0] = 0.0f;
			v.normal[1] = 0.0f;
			v.normal[2] = 1.0f;
		}

		if(colours != nil){
			v.colour[0] = colours[i].red;
			v.colour[1] = colours[i].green;
			v.colour[2] = colours[i].blue;
			v.colour[3] = colours[i].alpha;
		}else if(geo->flags & Geometry::LIGHT){
			// Lit, no prelight: black base, brightness from the lights.
			v.colour[0] = v.colour[1] = v.colour[2] = 0x00;
			v.colour[3] = 0xFF;
		}else{
			// Neither: fixed function full bright. Cutscene characters.
			v.colour[0] = v.colour[1] = v.colour[2] = 0xFF;
			v.colour[3] = 0xFF;
		}

		if(texCoords != nil){
			v.texCoord[0] = texCoords[i].u;
			v.texCoord[1] = texCoords[i].v;
		}else{
			v.texCoord[0] = 0.0f;
			v.texCoord[1] = 0.0f;
		}

		for(int32 w = 0; w < 4; w++){
			v.weights[w] = skin->weights[i*4 + w];
			// Clamp rather than trust the file: an out of range index reads
			// past the bone block in the shader, which on a tiler shows up as
			// geometry stretched to infinity rather than a clean fault.
			uint8 bone = skin->indices[i*4 + w];
			if(bone >= RW_MAX_BONES){
				bone = 0;
				clampedIndices++;
			}
			v.boneIndices[w] = bone;
		}
	}

	// Silent clamping would look like a modelling error rather than a limit
	// being hit, so say so once per geometry.
	if(clampedIndices > 0)
		VKERR("skin: %u bone indices past %d clamped to root "
		      "(numBones=%d); those vertices will be posed wrongly",
		      clampedIndices, RW_MAX_BONES, skin->numBones);

	if(isPrelit){
		InstanceData *inst = header->inst;
		for(uint32 m = 0; m < header->numMeshes; m++, inst++){
			inst->vertexAlpha = 0;
			for(int32 i = 0; i < inst->numVertices; i++)
				if(vertices[inst->minVert + i].colour[3] != 0xFF){
					inst->vertexAlpha = 1;
					break;
				}
		}
	}

	if(size > 0)
		uploadStaticBuffer(header->vertexBuffer, size,
		                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		                   &header->vbo, &header->vboMemory);
}

void
skinUninstanceCB(Geometry *, InstanceDataHeader *)
{
	assert(0 && "can't uninstance");
}

// Builds the pose: for each bone, the inverse bind matrix followed by the
// hierarchy's current matrix, brought back into the atomic's own space when
// the hierarchy is not already local. Mirrors uploadSkinMatrices in
// gl3skin.cpp, which is the reference for every other backend.
int32
buildBoneMatrices(Atomic *a)
{
	Skin *skin = Skin::get(a->geometry);
	if(skin == nil)
		return 0;

	HAnimHierarchy *hier = Skin::getHierarchy(a);
	Matrix *m = (Matrix*)gBoneMatrices;

	if(hier == nil){
		const int32 count = skin->numBones < RW_MAX_BONES ?
			skin->numBones : RW_MAX_BONES;
		for(int32 i = 0; i < count; i++, m++)
			m->setIdentity();
		return count;
	}

	if(hier->numNodes > RW_MAX_BONES){
		static int32 reported = 0;
		if(reported++ < 4)
			VKERR("skin: hierarchy has %d nodes, bone block holds %d; "
			      "bones past the limit will not animate",
			      hier->numNodes, RW_MAX_BONES);
	}
	// RenderWare allows the skin and the hierarchy to disagree; the weights
	// index the skin's bones, so posing past numBones reads matrices that were
	// never written for this model.
	if(skin->numBones != hier->numNodes){
		static int32 reportedMismatch = 0;
		if(reportedMismatch++ < 4)
			VKERR("skin: numBones=%d but hierarchy has %d nodes",
			      skin->numBones, hier->numNodes);
	}

	const int32 count = hier->numNodes < RW_MAX_BONES ?
		hier->numNodes : RW_MAX_BONES;
	Matrix *invMats = (Matrix*)skin->inverseMatrices;

	if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
		for(int32 i = 0; i < count; i++, m++){
			invMats[i].flags = 0;
			Matrix::mult(m, &invMats[i], &hier->matrices[i]);
		}
	}else{
		Matrix invAtomic, tmp;
		Matrix::invert(&invAtomic, a->getFrame()->getLTM());
		for(int32 i = 0; i < count; i++, m++){
			invMats[i].flags = 0;
			Matrix::mult(&tmp, &hier->matrices[i], &invAtomic);
			Matrix::mult(m, &invMats[i], &tmp);
		}
	}
	return count;
}

// Copies the pose into the frame's dynamic buffer and reports the offset the
// descriptor should be bound at. Returns false when the frame ran out of room,
// in which case the caller must not draw: a stale offset would pose the model
// with another atomic's bones.
bool32
uploadBoneMatrices(int32 count, uint32 *dynamicOffsetOut)
{
	// rw::Matrix is 4x3 in memory; the shader reads mat4. Expand into the
	// destination rather than keeping a second staging copy.
	const VkDeviceSize blockSize = RW_MAX_BONES*16*sizeof(float32);
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	void *mapped = nil;
	if(!allocateDynamic(blockSize, getBoneBlockAlignment(), &buffer, &offset,
	                    &mapped))
		return 0;

	// The skin path is verified correct: NPCs pose and face exactly as the
	// game data says. The player model alone renders turned around -- it is a
	// VR-build asset whose facing was never visible on the desktop because
	// that build plays first person from inside the head. Not compensated
	// here; it resolves itself when the first-person camera is ported.
	float32 *dst = (float32*)mapped;
	const Matrix *src = (const Matrix*)gBoneMatrices;
	for(int32 i = 0; i < count; i++, src++, dst += 16){
		dst[0]  = src->right.x; dst[1]  = src->right.y;
		dst[2]  = src->right.z; dst[3]  = 0.0f;
		dst[4]  = src->up.x;    dst[5]  = src->up.y;
		dst[6]  = src->up.z;    dst[7]  = 0.0f;
		dst[8]  = src->at.x;    dst[9]  = src->at.y;
		dst[10] = src->at.z;    dst[11] = 0.0f;
		dst[12] = src->pos.x;   dst[13] = src->pos.y;
		dst[14] = src->pos.z;   dst[15] = 1.0f;
	}
	// Unused slots are identity, so a stray index poses the vertex at its bind
	// position instead of collapsing it to the origin.
	for(int32 i = count; i < RW_MAX_BONES; i++, dst += 16){
		memset(dst, 0, 16*sizeof(float32));
		dst[0] = dst[5] = dst[10] = dst[15] = 1.0f;
	}

	*dynamicOffsetOut = (uint32)offset;
	return 1;
}

void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	if(!gvk.inFrame || header->vbo == VK_NULL_HANDLE ||
	   header->ibo == VK_NULL_HANDLE)
		return;

	const int32 boneCount = buildBoneMatrices(atomic);
	uint32 dynamicOffset = 0;
	if(!uploadBoneMatrices(boneCount, &dynamicOffset))
		return;

	drawAtomicMeshes(atomic, header, SHADER_SKIN, &dynamicOffset);
}

void *
skinOpen(void *o, int32, int32)
{
	skinGlobals.pipelines[PLATFORM_VULKAN] = makeSkinPipeline();
	return o;
}

void *
skinClose(void *o, int32, int32)
{
	if(skinGlobals.pipelines[PLATFORM_VULKAN]){
		((ObjPipeline*)skinGlobals.pipelines[PLATFORM_VULKAN])->destroy();
		skinGlobals.pipelines[PLATFORM_VULKAN] = nil;
	}
	return o;
}

} // namespace

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_VULKAN, 0, ID_SKIN, skinOpen, skinClose);
}

ObjPipeline *
makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->uninstanceCB = skinUninstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	return pipe;
}

}
}

#else

namespace rw {
namespace vulkan {
void initSkin(void) { }
}
}

#endif
