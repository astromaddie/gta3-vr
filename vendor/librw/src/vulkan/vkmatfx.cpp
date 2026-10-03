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

#define PLUGIN_ID ID_MATFX

#ifdef RW_VULKAN

namespace rw {
namespace vulkan {

// MatFX covers the environment-map materials: in Vice City that is every
// vehicle body, plus assorted shiny world pieces. Without a pipeline on this
// platform librw parks those atomics on a dummy pipe that draws nothing --
// which on a vehicle looks like wheels driving themselves around.
//
// This first version renders the base pass only, through the same path as the
// default pipeline. The reflection pass needs a second draw with the env
// raster and a texture matrix; until it lands, cars are painted but not shiny.

namespace {

void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	if(!gvk.inFrame || header->vbo == VK_NULL_HANDLE ||
	   header->ibo == VK_NULL_HANDLE)
		return;
	drawAtomicMeshes(atomic, header, SHADER_WORLD, nil);
}

void *
matfxOpen(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_VULKAN] = makeMatFXPipeline();
	return o;
}

void *
matfxClose(void *o, int32, int32)
{
	if(matFXGlobals.pipelines[PLATFORM_VULKAN]){
		((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_VULKAN])->destroy();
		matFXGlobals.pipelines[PLATFORM_VULKAN] = nil;
	}
	return o;
}

} // namespace

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_VULKAN, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

ObjPipeline *
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

}
}

#else

namespace rw {
namespace vulkan {
void initMatFX(void) { }
}
}

#endif
