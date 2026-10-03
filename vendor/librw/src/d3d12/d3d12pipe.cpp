#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

namespace rw {
namespace d3d12 {

#ifdef RW_D3D12

struct Vertex
{
	V3d position;
	V3d normal;
	RGBA color;
	TexCoords texCoords;
	float32 weights[4];
	uint8 boneIndices[4];
};

struct MeshDraw
{
	uint32 numIndices;
	uint32 startIndex;
	Material *material;
	bool32 vertexAlpha;
};

enum { DYNAMIC_VERTEX_FRAME_COUNT = 3 };

struct D3D12InstanceDataHeader : InstanceDataHeader
{
	uint32 serialNumber;
	uint32 numMeshes;
	uint32 numVertices;
	uint32 numIndices;
	D3D12_PRIMITIVE_TOPOLOGY topology;
	ID3D12Resource *vertexBuffer;
	ID3D12Resource *vertexBuffers[DYNAMIC_VERTEX_FRAME_COUNT];
	ID3D12Resource *indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	MeshDraw *meshes;
};

static ID3D12RootSignature *rootSignature;
static ID3D12RootSignature *stereoRootSignature;
enum WorldBlendMode {
	WORLD_BLEND_OPAQUE,
	WORLD_BLEND_ALPHA,
	WORLD_BLEND_ADD_ONE,
	WORLD_BLEND_ADD_ALPHA,
	WORLD_BLEND_SHADOW,
	WORLD_BLEND_INVERSE_DEST,
	WORLD_BLEND_REPLACE,
	WORLD_BLEND_KEEP_DESTINATION,
	WORLD_BLEND_MODULATE_DESTINATION,
	WORLD_BLEND_ALPHA_INVERSE_DEST_ALPHA,
	WORLD_BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA,
	WORLD_BLEND_COUNT
};
enum WorldDepthMode {
	WORLD_DEPTH_DISABLED,
	WORLD_DEPTH_WRITE_ONLY,
	WORLD_DEPTH_TEST_ONLY,
	WORLD_DEPTH_TEST_WRITE,
	WORLD_DEPTH_COUNT
};
enum WorldCullMode {
	WORLD_CULL_NONE,
	WORLD_CULL_BACK,
	WORLD_CULL_FRONT,
	WORLD_CULL_COUNT
};
static ID3D12PipelineState *worldPipelines[WORLD_BLEND_COUNT][WORLD_DEPTH_COUNT][WORLD_CULL_COUNT];
static ID3D12PipelineState *stereoWorldPipelines[WORLD_BLEND_COUNT][WORLD_DEPTH_COUNT][WORLD_CULL_COUNT];
static Raster *whiteRaster;
static bool32 pipelineReady;

enum {
	BONE_FRAME_COUNT = 3,
	MAX_SKIN_BONES = 64,
	MAX_WORLD_LIGHTS = 8,
	// A VR frame records the complete world twice before submission. The old
	// one-view arena could run out midway through the right eye, silently making
	// the remaining atomics (often vehicles and buildings) disappear.
	BONE_UPLOAD_SIZE = 16*1024*1024
};

struct BoneArena
{
	ID3D12Resource *resource;
	uint8 *mapped;
	uint32 offset;
};

struct LightingConstants
{
	float ambient[4];
	float surface[4];
	float colorRadius[MAX_WORLD_LIGHTS][4];
	float positionCos[MAX_WORLD_LIGHTS][4];
	float directionClamp[MAX_WORLD_LIGHTS][4];
};

static BoneArena boneArenas[BONE_FRAME_COUNT];
static uint32 activeBoneArena = UINT32_MAX;
static WorldRenderProfile worldRenderProfile;

enum { STEREO_ATOMIC_CACHE_COUNT = 8192 };

struct StereoAtomicCacheEntry
{
	Atomic *atomic;
	uint32 generation;
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	bool32 isSkinned;
	LightingConstants lighting;
};

static StereoAtomicCacheEntry stereoAtomicCache[STEREO_ATOMIC_CACHE_COUNT];
static uint32 stereoCacheGeneration = 1;
static int32 stereoWorldEye = -1;
static bool32 stereoSinglePassActive;
static uint32 stereoRightCameraFrame = UINT32_MAX;
static uint32 stereoRightCameraUploadFrame = UINT32_MAX;
static float stereoRightCameraConstants[32];
static D3D12_GPU_VIRTUAL_ADDRESS stereoRightCameraAddress;
static float stereoTemporalCameraConstants[2][32];
static bool32 stereoTemporalCameraValid[2];

enum { STEREO_WORLD_DRAW_CAPACITY = 16384 };

struct StereoWorldDraw
{
	D3D12_PRIMITIVE_TOPOLOGY topology;
	D3D12_VERTEX_BUFFER_VIEW vertexView;
	D3D12_INDEX_BUFFER_VIEW indexView;
	ID3D12PipelineState *pipeline;
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	D3D12_GPU_VIRTUAL_ADDRESS lightingAddress;
	D3D12_GPU_DESCRIPTOR_HANDLE texture;
	D3D12_GPU_DESCRIPTOR_HANDLE sampler;
	float constants[58];
	uint32 numIndices;
	uint32 startIndex;
};

struct StereoWorldRange
{
	uint32 first;
	uint32 count;
	bool32 complete;
};

static StereoWorldDraw stereoWorldDraws[STEREO_WORLD_DRAW_CAPACITY];
static StereoWorldRange stereoWorldRanges[STEREO_WORLD_SEGMENT_COUNT];
static uint32 stereoWorldDrawCount;
static int32 stereoCaptureSegment = -1;
static bool32 stereoWorldPacketValid;
static uint32 stereoWorldPacketGeneration;

struct StereoBundleFrame
{
	ID3D12CommandAllocator *allocator;
	ID3D12GraphicsCommandList *segments[STEREO_WORLD_SEGMENT_COUNT];
};

static StereoBundleFrame stereoBundleFrames[BONE_FRAME_COUNT];
static HANDLE stereoBundleWorkerThread;
static HANDLE stereoBundleWorkEvent;
static HANDLE stereoBundleDoneEvent;
static HANDLE stereoBundleStopEvent;
static volatile LONG stereoBundlePending;
static volatile LONG stereoBundleSucceeded;
static uint32 stereoBundleTaskFrame;
static uint32 stereoBundleTaskGeneration;
static float stereoBundleTaskView[16];
static float stereoBundleTaskProjection[16];
static uint32 stereoBundleCompletedFrame;
static uint32 stereoBundleCompletedGeneration;
static bool32 stereoBundleResourcesReady;

static bool32 waitForStereoWorldBundleBuild(void);
static bool32 createStereoBundleResources(void);
static void shutdownStereoBundleResources(void);

static double
profileNowMs(void)
{
	static LARGE_INTEGER frequency = {};
	if(frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart*1000.0/frequency.QuadPart;
}

void
resetWorldRenderProfile(void)
{
	memset(&worldRenderProfile, 0, sizeof(worldRenderProfile));
}

void
getWorldRenderProfile(WorldRenderProfile *profile)
{
	if(profile)
		*profile = worldRenderProfile;
}

void
setStereoWorldEye(int32 eye)
{
	if(eye < 0 && stereoSinglePassActive)
		endStereoSinglePass();
	if(eye == 0){
		// A cancelled OpenXR frame may never reach the right-eye replay. Do not
		// overwrite its packet while the worker still owns it.
		waitForStereoWorldBundleBuild();
		stereoCacheGeneration++;
		if(stereoCacheGeneration == 0){
			memset(stereoAtomicCache, 0, sizeof(stereoAtomicCache));
			stereoCacheGeneration = 1;
		}
		stereoWorldDrawCount = 0;
		stereoCaptureSegment = -1;
		stereoWorldPacketValid = 1;
		stereoWorldPacketGeneration++;
		if(stereoWorldPacketGeneration == 0)
			stereoWorldPacketGeneration = 1;
		memset(stereoWorldRanges, 0, sizeof(stereoWorldRanges));
		stereoTemporalCameraValid[0] = 0;
		stereoTemporalCameraValid[1] = 0;
	}
	stereoWorldEye = eye;
}

void
captureStereoWorldCamera(int32 eye, float32 jitterClipX, float32 jitterClipY)
{
	if(eye < 0 || eye > 1 || engine == nil || engine->currentCamera == nil)
		return;
	// Streamline consumes non-jittered camera matrices. Capture them before
	// applying the sub-pixel offset used by the actual DLAA render.
	memcpy(stereoTemporalCameraConstants[eye],
	       &engine->currentCamera->devView, 16*sizeof(float));
	memcpy(stereoTemporalCameraConstants[eye] + 16,
	       &engine->currentCamera->devProj, 16*sizeof(float));
	stereoTemporalCameraValid[eye] = 1;
	float *projection = (float*)&engine->currentCamera->devProj;
	projection[8] += jitterClipX;
	projection[9] += jitterClipY;
	if(eye == 1){
		memcpy(stereoRightCameraConstants,
		       &engine->currentCamera->devView, 16*sizeof(float));
		memcpy(stereoRightCameraConstants + 16,
		       &engine->currentCamera->devProj, 16*sizeof(float));
		stereoRightCameraFrame = getFrameIndex() % BONE_FRAME_COUNT;
		stereoRightCameraUploadFrame = UINT32_MAX;
		stereoRightCameraAddress = 0;
	}
}

bool32
getStereoWorldCamera(int32 eye, float32 view[16], float32 projection[16])
{
	if(eye < 0 || eye > 1 || view == nil || projection == nil ||
	   !stereoTemporalCameraValid[eye])
		return 0;
	memcpy(view, stereoTemporalCameraConstants[eye], 16*sizeof(float));
	memcpy(projection, stereoTemporalCameraConstants[eye] + 16, 16*sizeof(float));
	return 1;
}

void
beginStereoWorldCapture(uint32 segment)
{
	if(stereoWorldEye != 0 || !stereoWorldPacketValid ||
	   segment >= STEREO_WORLD_SEGMENT_COUNT){
		stereoCaptureSegment = -1;
		return;
	}
	StereoWorldRange &range = stereoWorldRanges[segment];
	range.first = stereoWorldDrawCount;
	range.count = 0;
	range.complete = 0;
	stereoCaptureSegment = (int32)segment;
}

void
endStereoWorldCapture(uint32 segment)
{
	if(stereoCaptureSegment != (int32)segment ||
	   segment >= STEREO_WORLD_SEGMENT_COUNT){
		stereoCaptureSegment = -1;
		return;
	}
	StereoWorldRange &range = stereoWorldRanges[segment];
	range.count = stereoWorldDrawCount - range.first;
	range.complete = stereoWorldPacketValid;
	stereoCaptureSegment = -1;
}

void
cancelStereoWorldPacket(void)
{
	waitForStereoWorldBundleBuild();
	stereoCaptureSegment = -1;
	stereoWorldPacketValid = 0;
	memset(stereoWorldRanges, 0, sizeof(stereoWorldRanges));
}

void
queueStereoWorldBundleBuild(const float32 *view, const float32 *projection)
{
	if(!stereoBundleResourcesReady || stereoWorldEye != 0 ||
	   view == nil || projection == nil ||
	   !stereoWorldPacketValid || stereoBundleWorkEvent == nil)
		return;
	for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++)
		if(!stereoWorldRanges[segment].complete)
			return;
	waitForStereoWorldBundleBuild();
	stereoBundleTaskFrame = getFrameIndex() % BONE_FRAME_COUNT;
	stereoBundleTaskGeneration = stereoWorldPacketGeneration;
	memcpy(stereoBundleTaskView, view,
	       sizeof(stereoBundleTaskView));
	memcpy(stereoBundleTaskProjection, projection,
	       sizeof(stereoBundleTaskProjection));
	InterlockedExchange(&stereoBundleSucceeded, 0);
	InterlockedExchange(&stereoBundlePending, 1);
	ResetEvent(stereoBundleDoneEvent);
	if(!SetEvent(stereoBundleWorkEvent)){
		InterlockedExchange(&stereoBundlePending, 0);
		SetEvent(stereoBundleDoneEvent);
	}
}

static StereoAtomicCacheEntry*
findStereoAtomicCache(Atomic *atomic, bool32 create)
{
	if(atomic == nil || stereoWorldEye < 0)
		return nil;
	const uintptr_t key = (uintptr_t)atomic;
	uint32 slot = (uint32)(((key >> 4) * 2654435761u) &
	                       (STEREO_ATOMIC_CACHE_COUNT - 1));
	for(uint32 probe = 0; probe < STEREO_ATOMIC_CACHE_COUNT; probe++){
		StereoAtomicCacheEntry *entry =
			&stereoAtomicCache[(slot + probe) & (STEREO_ATOMIC_CACHE_COUNT - 1)];
		if(entry->generation != stereoCacheGeneration){
			if(!create)
				return nil;
			entry->generation = stereoCacheGeneration;
			entry->atomic = atomic;
			return entry;
		}
		if(entry->atomic == atomic)
			return entry;
	}
	return nil;
}

template<class T>
static void
releaseCom(T *&object)
{
	if(object){
		object->Release();
		object = nil;
	}
}

static bool32
recordStereoWorldBundles(uint32 frame)
{
	if(frame >= BONE_FRAME_COUNT || !stereoWorldPacketValid)
		return 0;
	StereoBundleFrame &bundleFrame = stereoBundleFrames[frame];
	if(bundleFrame.allocator == nil ||
	   FAILED(bundleFrame.allocator->Reset()))
		return 0;
	ID3D12DescriptorHeap *heaps[2] = {
		getShaderResourceHeap(), getSamplerHeap()
	};
	if(heaps[0] == nil || heaps[1] == nil)
		return 0;
	for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++){
		const StereoWorldRange &range = stereoWorldRanges[segment];
		ID3D12GraphicsCommandList *list = bundleFrame.segments[segment];
		if(list == nil || !range.complete ||
		   range.first + range.count > stereoWorldDrawCount ||
		   FAILED(list->Reset(bundleFrame.allocator, nil)))
			return 0;
		list->SetDescriptorHeaps(2, heaps);
		list->SetGraphicsRootSignature(rootSignature);
		for(uint32 i = 0; i < range.count; i++){
			const StereoWorldDraw &draw = stereoWorldDraws[range.first + i];
			float constants[58];
			memcpy(constants, draw.constants, sizeof(constants));
			memcpy(constants + 16, stereoBundleTaskView,
			       sizeof(stereoBundleTaskView));
			memcpy(constants + 32, stereoBundleTaskProjection,
			       sizeof(stereoBundleTaskProjection));
			list->IASetPrimitiveTopology(draw.topology);
			list->IASetVertexBuffers(0, 1, &draw.vertexView);
			list->IASetIndexBuffer(&draw.indexView);
			list->SetPipelineState(draw.pipeline);
			list->SetGraphicsRoot32BitConstants(0, 58, constants, 0);
			list->SetGraphicsRootDescriptorTable(1, draw.texture);
			list->SetGraphicsRootConstantBufferView(2, draw.boneAddress);
			list->SetGraphicsRootConstantBufferView(3, draw.lightingAddress);
			list->SetGraphicsRootDescriptorTable(4, draw.sampler);
			list->DrawIndexedInstanced(draw.numIndices, 1,
			                           draw.startIndex, 0, 0);
		}
		if(FAILED(list->Close()))
			return 0;
	}
	return 1;
}

static DWORD WINAPI
stereoBundleWorkerProc(void*)
{
	HANDLE events[2] = { stereoBundleStopEvent, stereoBundleWorkEvent };
	for(;;){
		DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if(result == WAIT_OBJECT_0)
			break;
		if(result != WAIT_OBJECT_0 + 1)
			continue;
		const uint32 frame = stereoBundleTaskFrame;
		const uint32 generation = stereoBundleTaskGeneration;
		const double start = profileNowMs();
		const bool32 ok = recordStereoWorldBundles(frame);
		worldRenderProfile.stereoBundleBuildMs +=
			(float32)(profileNowMs() - start);
		stereoBundleCompletedFrame = frame;
		stereoBundleCompletedGeneration = generation;
		InterlockedExchange(&stereoBundleSucceeded, ok ? 1 : 0);
		SetEvent(stereoBundleDoneEvent);
	}
	return 0;
}

static bool32
waitForStereoWorldBundleBuild(void)
{
	if(InterlockedCompareExchange(&stereoBundlePending, 0, 0) == 0)
		return 0;
	if(stereoBundleDoneEvent == nil){
		InterlockedExchange(&stereoBundlePending, 0);
		return 0;
	}
	const double start = profileNowMs();
	const DWORD result = WaitForSingleObject(stereoBundleDoneEvent, INFINITE);
	worldRenderProfile.stereoBundleWaitMs +=
		(float32)(profileNowMs() - start);
	InterlockedExchange(&stereoBundlePending, 0);
	return result == WAIT_OBJECT_0 &&
	       InterlockedCompareExchange(&stereoBundleSucceeded, 0, 0) != 0;
}

static void
shutdownStereoBundleResources(void)
{
	if(stereoBundleWorkerThread){
		if(stereoBundleStopEvent)
			SetEvent(stereoBundleStopEvent);
		WaitForSingleObject(stereoBundleWorkerThread, INFINITE);
		CloseHandle(stereoBundleWorkerThread);
		stereoBundleWorkerThread = nil;
	}
	if(stereoBundleWorkEvent){ CloseHandle(stereoBundleWorkEvent); stereoBundleWorkEvent = nil; }
	if(stereoBundleDoneEvent){ CloseHandle(stereoBundleDoneEvent); stereoBundleDoneEvent = nil; }
	if(stereoBundleStopEvent){ CloseHandle(stereoBundleStopEvent); stereoBundleStopEvent = nil; }
	for(uint32 frame = 0; frame < BONE_FRAME_COUNT; frame++){
		for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++)
			releaseCom(stereoBundleFrames[frame].segments[segment]);
		releaseCom(stereoBundleFrames[frame].allocator);
	}
	InterlockedExchange(&stereoBundlePending, 0);
	InterlockedExchange(&stereoBundleSucceeded, 0);
	stereoBundleResourcesReady = 0;
}

static bool32
createStereoBundleResources(void)
{
	if(stereoBundleResourcesReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;
	for(uint32 frame = 0; frame < BONE_FRAME_COUNT; frame++){
		if(FAILED(device->CreateCommandAllocator(
		       D3D12_COMMAND_LIST_TYPE_BUNDLE,
		       IID_PPV_ARGS(&stereoBundleFrames[frame].allocator)))){
			shutdownStereoBundleResources();
			return 0;
		}
		for(uint32 segment = 0; segment < STEREO_WORLD_SEGMENT_COUNT; segment++){
			ID3D12GraphicsCommandList *&list =
				stereoBundleFrames[frame].segments[segment];
			if(FAILED(device->CreateCommandList(
			       0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
			       stereoBundleFrames[frame].allocator, nil,
			       IID_PPV_ARGS(&list))) || FAILED(list->Close())){
				shutdownStereoBundleResources();
				return 0;
			}
		}
	}
	stereoBundleWorkEvent = CreateEventA(nil, FALSE, FALSE, nil);
	stereoBundleDoneEvent = CreateEventA(nil, TRUE, TRUE, nil);
	stereoBundleStopEvent = CreateEventA(nil, TRUE, FALSE, nil);
	if(stereoBundleWorkEvent == nil || stereoBundleDoneEvent == nil ||
	   stereoBundleStopEvent == nil){
		shutdownStereoBundleResources();
		return 0;
	}
	stereoBundleWorkerThread = CreateThread(
		nil, 0, stereoBundleWorkerProc, nil, 0, nil);
	if(stereoBundleWorkerThread == nil){
		shutdownStereoBundleResources();
		return 0;
	}
	stereoBundleResourcesReady = 1;
	return 1;
}

static D3D12_HEAP_PROPERTIES
uploadHeapProperties(void)
{
	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.Type = D3D12_HEAP_TYPE_UPLOAD;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

static D3D12_RESOURCE_DESC
bufferDesc(uint64 size)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return desc;
}

static bool32
createUploadBuffer(const void *data, uint64 size, ID3D12Resource **resource)
{
	ID3D12Device *device = getDevice();
	if(device == nil || data == nil || size == 0 || resource == nil)
		return 0;
	const double startedMs = profileNowMs();
	D3D12_HEAP_PROPERTIES props = uploadHeapProperties();
	D3D12_RESOURCE_DESC desc = bufferDesc(size);
	if(FAILED(device->CreateCommittedResource(
	       &props, D3D12_HEAP_FLAG_NONE, &desc,
	       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
	       IID_PPV_ARGS(resource))))
		return 0;
	void *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED((*resource)->Map(0, &readRange, &mapped))){
		releaseCom(*resource);
		return 0;
	}
	memcpy(mapped, data, (size_t)size);
	(*resource)->Unmap(0, nil);
	worldRenderProfile.bufferUploadMs += (float32)(profileNowMs()-startedMs);
	worldRenderProfile.bufferBytes += size;
	return 1;
}

static bool32
compileShader(const char *source, const char *entry, const char *target,
	          ID3DBlob **shader, const D3D_SHADER_MACRO *defines = nil)
{
	ID3DBlob *errors = nil;
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
	HRESULT hr = D3DCompile(source, strlen(source), "librw_d3d12_default",
	                        defines, nil, entry, target, flags, 0, shader, &errors);
	if(FAILED(hr) && errors)
		fprintf(stderr, "librw D3D12 shader: %s\n",
		        (const char*)errors->GetBufferPointer());
	releaseCom(errors);
	return SUCCEEDED(hr);
}

static void
getWorldPipelineBlend(uint32 mode, D3D12_BLEND *source,
                      D3D12_BLEND *destination,
                      D3D12_BLEND *sourceAlpha,
                      D3D12_BLEND *destinationAlpha)
{
	switch(mode){
	case WORLD_BLEND_ADD_ONE:
		*source = D3D12_BLEND_ONE;
		*destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_ONE;
		*destinationAlpha = D3D12_BLEND_ONE;
		break;
	case WORLD_BLEND_ADD_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA;
		*destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA;
		*destinationAlpha = D3D12_BLEND_ONE;
		break;
	case WORLD_BLEND_SHADOW:
		*source = D3D12_BLEND_ZERO;
		*destination = D3D12_BLEND_INV_SRC_COLOR;
		*sourceAlpha = D3D12_BLEND_ZERO;
		*destinationAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		break;
	case WORLD_BLEND_INVERSE_DEST:
		*source = D3D12_BLEND_INV_DEST_COLOR;
		*destination = D3D12_BLEND_ZERO;
		*sourceAlpha = D3D12_BLEND_INV_DEST_ALPHA;
		*destinationAlpha = D3D12_BLEND_ZERO;
		break;
	case WORLD_BLEND_REPLACE:
		*source = D3D12_BLEND_ONE;
		*destination = D3D12_BLEND_ZERO;
		*sourceAlpha = D3D12_BLEND_ONE;
		*destinationAlpha = D3D12_BLEND_ZERO;
		break;
	case WORLD_BLEND_KEEP_DESTINATION:
		*source = D3D12_BLEND_ZERO;
		*destination = D3D12_BLEND_ONE;
		*sourceAlpha = D3D12_BLEND_ZERO;
		*destinationAlpha = D3D12_BLEND_ONE;
		break;
	case WORLD_BLEND_MODULATE_DESTINATION:
		*source = D3D12_BLEND_ZERO;
		*destination = D3D12_BLEND_SRC_COLOR;
		*sourceAlpha = D3D12_BLEND_ZERO;
		*destinationAlpha = D3D12_BLEND_SRC_ALPHA;
		break;
	case WORLD_BLEND_ALPHA_INVERSE_DEST_ALPHA:
		*source = D3D12_BLEND_SRC_ALPHA;
		*destination = D3D12_BLEND_INV_DEST_ALPHA;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA;
		*destinationAlpha = D3D12_BLEND_INV_DEST_ALPHA;
		break;
	case WORLD_BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA:
		*source = D3D12_BLEND_DEST_ALPHA;
		*destination = D3D12_BLEND_INV_DEST_ALPHA;
		*sourceAlpha = D3D12_BLEND_DEST_ALPHA;
		*destinationAlpha = D3D12_BLEND_INV_DEST_ALPHA;
		break;
	case WORLD_BLEND_ALPHA:
	default:
		*source = D3D12_BLEND_SRC_ALPHA;
		*destination = D3D12_BLEND_INV_SRC_ALPHA;
		*sourceAlpha = D3D12_BLEND_SRC_ALPHA;
		*destinationAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		break;
	}
}

static uint32
getWorldBlendMode(void)
{
	void *source = getRenderState(SRCBLEND);
	void *destination = getRenderState(DESTBLEND);
	if(source == (void*)BLENDONE && destination == (void*)BLENDONE)
		return WORLD_BLEND_ADD_ONE;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDONE)
		return WORLD_BLEND_ADD_ALPHA;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDINVSRCCOLOR)
		return WORLD_BLEND_SHADOW;
	if(source == (void*)BLENDINVDESTCOLOR && destination == (void*)BLENDZERO)
		return WORLD_BLEND_INVERSE_DEST;
	if(source == (void*)BLENDONE && destination == (void*)BLENDZERO)
		return WORLD_BLEND_REPLACE;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDONE)
		return WORLD_BLEND_KEEP_DESTINATION;
	if(source == (void*)BLENDZERO && destination == (void*)BLENDSRCCOLOR)
		return WORLD_BLEND_MODULATE_DESTINATION;
	if(source == (void*)BLENDSRCALPHA && destination == (void*)BLENDINVDESTALPHA)
		return WORLD_BLEND_ALPHA_INVERSE_DEST_ALPHA;
	if(source == (void*)BLENDDESTALPHA && destination == (void*)BLENDINVDESTALPHA)
		return WORLD_BLEND_DEST_ALPHA_INVERSE_DEST_ALPHA;
	return WORLD_BLEND_ALPHA;
}

static bool32
createPipelineResources(void)
{
	if(pipelineReady)
		return 1;
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_DESCRIPTOR_RANGE ranges[2];
	memset(ranges, 0, sizeof(ranges));
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER params[5];
	memset(params, 0, sizeof(params));
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;
	params[0].Constants.Num32BitValues = 58;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 1;
	params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[2].Descriptor.ShaderRegister = 1;
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[3].Descriptor.ShaderRegister = 2;
	params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[4].DescriptorTable.NumDescriptorRanges = 1;
	params[4].DescriptorTable.pDescriptorRanges = &ranges[1];
	params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC signature;
	memset(&signature, 0, sizeof(signature));
	signature.NumParameters = 5;
	signature.pParameters = params;
	signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ID3DBlob *serialized = nil;
	ID3DBlob *errors = nil;
	HRESULT hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	// The stereo shader receives the ordinary per-draw block through a CBV
	// instead of 58 root constants. That leaves ample root-signature space for
	// the right-eye camera while preserving all material/bone/light bindings.
	D3D12_ROOT_PARAMETER stereoParams[6];
	memset(stereoParams, 0, sizeof(stereoParams));
	stereoParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	stereoParams[0].Descriptor.ShaderRegister = 0;
	stereoParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	for(uint32 i = 1; i < 5; i++)
		stereoParams[i] = params[i];
	stereoParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	stereoParams[5].Descriptor.ShaderRegister = 3;
	stereoParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	signature.NumParameters = 6;
	signature.pParameters = stereoParams;
	serialized = nil;
	errors = nil;
	hr = D3D12SerializeRootSignature(
		&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
	if(FAILED(hr)){
		if(errors)
			fprintf(stderr, "librw D3D12 stereo root signature: %s\n",
			        (const char*)errors->GetBufferPointer());
		releaseCom(errors);
		releaseCom(serialized);
		return 0;
	}
	releaseCom(errors);
	hr = device->CreateRootSignature(
		0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(&stereoRootSignature));
	releaseCom(serialized);
	if(FAILED(hr))
		return 0;

	static const char *shaderSource =
		"cbuffer DrawConstants : register(b0) {"
		" row_major float4x4 world; row_major float4x4 view;"
		" row_major float4x4 projection; float4 materialColor; float4 drawFlags;"
		" float fogEnd; float fogRange; };\n"
		"#ifdef STEREO_VERTEX\n"
		"cbuffer RightCameraConstants : register(b3) {"
		" row_major float4x4 rightView; row_major float4x4 rightProjection; };\n"
		"#endif\n"
		"cbuffer SkinConstants : register(b1) { row_major float4x4 bones[64]; };"
		"cbuffer LightingConstants : register(b2) { float4 ambientLight;"
		" float4 surfaceProps; float4 lightColorRadius[8];"
		" float4 lightPositionCos[8]; float4 lightDirectionClamp[8]; };"
		"Texture2D diffuseTexture : register(t0);"
		"SamplerState diffuseSampler : register(s0);"
		"struct VSIn { float3 position : POSITION; float3 normal : NORMAL;"
		" float4 color : COLOR0; float2 uv : TEXCOORD0;"
		" float4 weights : BLENDWEIGHT0; uint4 indices : BLENDINDICES0; };"
		"struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0;"
		" float2 uv : TEXCOORD0; float fogFactor : TEXCOORD1;\n"
		"#ifdef STEREO_VERTEX\n"
		" float2 clipDistance : SV_ClipDistance0;\n"
		"#endif\n"
		"};"
		"VSOut BuildVS(VSIn input, uint stereoEye) { VSOut output;"
		" float4 localPosition = float4(input.position, 1.0);"
		" float3 localNormal = input.normal;"
		" if(drawFlags.y > 0.5) { localPosition = float4(0.0, 0.0, 0.0, 0.0);"
		" localNormal = float3(0.0, 0.0, 0.0);"
		" [unroll] for(int i = 0; i < 4; i++)"
		" { localPosition += mul(float4(input.position, 1.0), bones[input.indices[i]]) * input.weights[i];"
		" localNormal += mul(float4(input.normal, 0.0), bones[input.indices[i]]).xyz * input.weights[i]; } }"
		" float4 worldPosition = mul(localPosition, world);"
		" float4 p = mul(worldPosition, view); output.position = mul(p, projection);\n"
		"#ifdef STEREO_VERTEX\n"
		" if(stereoEye != 0) { p = mul(worldPosition, rightView);"
		" output.position = mul(p, rightProjection); }\n"
		"#endif\n"
		" output.fogFactor = fogRange < 0.0 ?"
		" saturate((p.z - fogEnd) * fogRange) : 1.0;"
		" float3 normal = normalize(mul(float4(localNormal, 0.0), world).xyz);"
		" float3 litColor = input.color.rgb + ambientLight.rgb * surfaceProps.x;"
		" [unroll] for(int lightIndex = 0; lightIndex < 8; lightIndex++) {"
		"  float4 light = lightColorRadius[lightIndex]; if(light.w == 0.0) break;"
		"  float diffuse = 0.0; float attenuation = 1.0;"
		"  if(light.w < 0.0) {"
		"   diffuse = max(0.0, dot(normal, -lightDirectionClamp[lightIndex].xyz));"
		"  } else {"
		"   float3 toVertex = worldPosition.xyz - lightPositionCos[lightIndex].xyz;"
		"   float distanceToLight = length(toVertex);"
		"   float3 ray = distanceToLight > 0.0001 ? toVertex / distanceToLight : float3(0.0, 0.0, 0.0);"
		"   attenuation = max(0.0, 1.0 - distanceToLight / light.w);"
		"   diffuse = max(0.0, dot(normal, -ray));"
		"   float falloffClamp = lightDirectionClamp[lightIndex].w;"
		"   if(falloffClamp >= 0.0) {"
		"    float pointCos = dot(ray, lightDirectionClamp[lightIndex].xyz);"
		"    float coneCos = -lightPositionCos[lightIndex].w;"
		"    float falloff = (pointCos - coneCos) / max(0.0001, 1.0 - coneCos);"
		"    if(falloff < 0.0) diffuse = 0.0;"
		"    diffuse *= max(falloff, falloffClamp);"
		"   }"
		"  }"
		"  litColor += diffuse * light.rgb * attenuation * surfaceProps.z;"
		" }"
		" output.color = float4(saturate(litColor), input.color.a) * materialColor; output.uv = input.uv;"
		" return output; }"
		"VSOut VSMain(VSIn input) { return BuildVS(input, 0); }\n"
		"#ifdef STEREO_VERTEX\n"
		"VSOut VSMainStereo(VSIn input, uint instanceId : SV_InstanceID) {"
		" VSOut output = BuildVS(input, instanceId);"
		" output.clipDistance = float2(output.position.x + output.position.w,"
		"  output.position.w - output.position.x);"
		" output.position.x = output.position.x * 0.5 +"
		"  (instanceId == 0 ? -0.5 : 0.5) * output.position.w;"
		" return output; }\n"
		"#endif\n"
		"float4 PSMain(VSOut input) : SV_TARGET {"
		" float4 color = input.color;"
		" if(drawFlags.x > 0.5) color *= diffuseTexture.Sample(diffuseSampler, input.uv);"
		" if(drawFlags.w > 1.5) clip(drawFlags.z - color.a - 0.000001);"
		" else if(drawFlags.w > 0.5) clip(color.a - drawFlags.z);"
		" uint packedFogColor = asuint(surfaceProps.w);"
		" float3 fogColor = float3(packedFogColor & 255u,"
		" (packedFogColor >> 8) & 255u, (packedFogColor >> 16) & 255u) / 255.0;"
		" color.rgb = lerp(fogColor, color.rgb, input.fogFactor);"
		" return color; }";
	ID3DBlob *vertexShader = nil;
	ID3DBlob *stereoVertexShader = nil;
	ID3DBlob *pixelShader = nil;
	const D3D_SHADER_MACRO stereoDefines[] = {
		{ "STEREO_VERTEX", "1" }, { nil, nil }
	};
	if(!compileShader(shaderSource, "VSMain", "vs_5_0", &vertexShader) ||
	   !compileShader(shaderSource, "VSMainStereo", "vs_5_0", &stereoVertexShader,
	                  stereoDefines) ||
	   !compileShader(shaderSource, "PSMain", "ps_5_0", &pixelShader)){
		releaseCom(vertexShader);
		releaseCom(stereoVertexShader);
		releaseCom(pixelShader);
		return 0;
	}

	D3D12_INPUT_ELEMENT_DESC input[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
		  (UINT)offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
		  (UINT)offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
		  (UINT)offsetof(Vertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
		  (UINT)offsetof(Vertex, texCoords), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
		  (UINT)offsetof(Vertex, weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0,
		  (UINT)offsetof(Vertex, boneIndices), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso;
	memset(&pso, 0, sizeof(pso));
	pso.pRootSignature = rootSignature;
	pso.VS.pShaderBytecode = vertexShader->GetBufferPointer();
	pso.VS.BytecodeLength = vertexShader->GetBufferSize();
	pso.PS.pShaderBytecode = pixelShader->GetBufferPointer();
	pso.PS.BytecodeLength = pixelShader->GetBufferSize();
	pso.InputLayout.pInputElementDescs = input;
	pso.InputLayout.NumElements = (UINT)nelem(input);
	pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso.RasterizerState.FrontCounterClockwise = TRUE;
	pso.RasterizerState.DepthClipEnable = TRUE;
	pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso.DepthStencilState.DepthEnable = TRUE;
	pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	pso.SampleMask = UINT_MAX;
	pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso.NumRenderTargets = 1;
	pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pso.SampleDesc.Count = 1;
	for(uint32 blend = 0; blend < WORLD_BLEND_COUNT && SUCCEEDED(hr); blend++){
		pso.BlendState.RenderTarget[0].BlendEnable =
			blend != WORLD_BLEND_OPAQUE;
		getWorldPipelineBlend(blend,
			&pso.BlendState.RenderTarget[0].SrcBlend,
			&pso.BlendState.RenderTarget[0].DestBlend,
			&pso.BlendState.RenderTarget[0].SrcBlendAlpha,
			&pso.BlendState.RenderTarget[0].DestBlendAlpha);
		for(uint32 depth = 0; depth < WORLD_DEPTH_COUNT && SUCCEEDED(hr); depth++){
			const bool depthTest = depth == WORLD_DEPTH_TEST_ONLY ||
			                       depth == WORLD_DEPTH_TEST_WRITE;
			const bool depthWrite = depth == WORLD_DEPTH_WRITE_ONLY ||
			                        depth == WORLD_DEPTH_TEST_WRITE;
			pso.DepthStencilState.DepthEnable = depthTest || depthWrite;
			pso.DepthStencilState.DepthWriteMask =
				depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL :
				D3D12_DEPTH_WRITE_MASK_ZERO;
			pso.DepthStencilState.DepthFunc = depthTest ?
				D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
			for(uint32 cull = 0; cull < WORLD_CULL_COUNT && SUCCEEDED(hr); cull++){
				pso.RasterizerState.CullMode = cull == WORLD_CULL_BACK ?
					D3D12_CULL_MODE_BACK : cull == WORLD_CULL_FRONT ?
					D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE;
				pso.pRootSignature = rootSignature;
				pso.VS.pShaderBytecode = vertexShader->GetBufferPointer();
				pso.VS.BytecodeLength = vertexShader->GetBufferSize();
				hr = device->CreateGraphicsPipelineState(
					&pso, IID_PPV_ARGS(&worldPipelines[blend][depth][cull]));
				if(SUCCEEDED(hr)){
					pso.pRootSignature = stereoRootSignature;
					pso.VS.pShaderBytecode = stereoVertexShader->GetBufferPointer();
					pso.VS.BytecodeLength = stereoVertexShader->GetBufferSize();
					hr = device->CreateGraphicsPipelineState(
						&pso, IID_PPV_ARGS(&stereoWorldPipelines[blend][depth][cull]));
				}
			}
		}
	}
	releaseCom(vertexShader);
	releaseCom(stereoVertexShader);
	releaseCom(pixelShader);
	if(FAILED(hr)){
		for(uint32 blend = 0; blend < WORLD_BLEND_COUNT; blend++)
			for(uint32 depth = 0; depth < WORLD_DEPTH_COUNT; depth++)
				for(uint32 cull = 0; cull < WORLD_CULL_COUNT; cull++)
				{
					releaseCom(worldPipelines[blend][depth][cull]);
					releaseCom(stereoWorldPipelines[blend][depth][cull]);
				}
		releaseCom(stereoRootSignature);
		releaseCom(rootSignature);
		return 0;
	}

	D3D12_HEAP_PROPERTIES boneProps = uploadHeapProperties();
	D3D12_RESOURCE_DESC boneBuffer = bufferDesc(BONE_UPLOAD_SIZE);
	for(uint32 i = 0; i < BONE_FRAME_COUNT; i++){
		if(FAILED(device->CreateCommittedResource(
		       &boneProps, D3D12_HEAP_FLAG_NONE, &boneBuffer,
		       D3D12_RESOURCE_STATE_GENERIC_READ, nil,
		       IID_PPV_ARGS(&boneArenas[i].resource))))
			return 0;
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(boneArenas[i].resource->Map(
		       0, &readRange, (void**)&boneArenas[i].mapped)))
			return 0;
	}
	activeBoneArena = UINT32_MAX;

	Image *white = Image::create(1, 1, 32);
	white->allocate();
	white->pixels[0] = white->pixels[1] = white->pixels[2] =
		white->pixels[3] = 0xFF;
	whiteRaster = Raster::createFromImage(white, PLATFORM_D3D12);
	white->destroy();
	if(whiteRaster == nil)
		return 0;
	// The Stage 6 bundle worker experiment is intentionally disabled. Profiling
	// on the target Quest/D3D12 driver showed that command recording contended
	// with the main thread and added about one millisecond of synchronization.
	// Keep the proven direct immutable-packet replay until true single-pass
	// stereo replaces the second world submission.
	pipelineReady = 1;
	return 1;
}

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry == nil || geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D12)
		return;
	D3D12InstanceDataHeader *header =
		(D3D12InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	for(uint32 i = 0; i < DYNAMIC_VERTEX_FRAME_COUNT; i++){
		deferRelease(header->vertexBuffers[i]);
		header->vertexBuffers[i] = nil;
	}
	deferRelease(header->indexBuffer);
	header->vertexBuffer = nil;
	header->indexBuffer = nil;
	rwFree(header->meshes);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static void
fillInstanceVertices(Geometry *geometry, Vertex *vertices)
{
	Skin *skin = Skin::get(geometry);
	for(uint32 i = 0; i < (uint32)geometry->numVertices; i++){
		vertices[i].position = geometry->morphTargets[0].vertices[i];
		if((geometry->flags & Geometry::NORMALS) &&
		   geometry->morphTargets[0].normals)
			vertices[i].normal = geometry->morphTargets[0].normals[i];
		else
			vertices[i].normal.set(0.0f, 0.0f, 1.0f);
		if((geometry->flags & Geometry::PRELIT) && geometry->colors)
			vertices[i].color = geometry->colors[i];
		else if(geometry->flags & Geometry::LIGHT)
			// Lit non-prelit geometry has no emissive vertex contribution. White
			// saturated the shader before ambient/directional lighting, so the
			// original scorched-vehicle lighting could never darken wrecks.
			vertices[i].color = makeRGBA(0, 0, 0, 255);
		else
			vertices[i].color = makeRGBA(255, 255, 255, 255);
		if(geometry->numTexCoordSets > 0 && geometry->texCoords[0])
			vertices[i].texCoords = geometry->texCoords[0][i];
		else{
			vertices[i].texCoords.u = 0.0f;
			vertices[i].texCoords.v = 0.0f;
		}
		vertices[i].weights[0] = 1.0f;
		vertices[i].weights[1] = vertices[i].weights[2] =
			vertices[i].weights[3] = 0.0f;
		memset(vertices[i].boneIndices, 0, sizeof(vertices[i].boneIndices));
		if(skin && skin->weights && skin->indices){
			memcpy(vertices[i].weights, skin->weights + i*4,
			       sizeof(vertices[i].weights));
			memcpy(vertices[i].boneIndices, skin->indices + i*4,
			       sizeof(vertices[i].boneIndices));
		}
	}
}

static void
refreshInstanceMeshes(Geometry *geometry, D3D12InstanceDataHeader *header,
	                  uint16 *indices)
{
	Mesh *mesh = geometry->meshHeader->getMeshes();
	uint32 indexOffset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		header->meshes[i].numIndices = mesh[i].numIndices;
		header->meshes[i].startIndex = indexOffset;
		header->meshes[i].material = mesh[i].material;
		header->meshes[i].vertexAlpha = 0;
		for(uint32 j = 0; j < mesh[i].numIndices; j++){
			const uint16 vertex = mesh[i].indices[j];
			if(indices)
				indices[indexOffset + j] = vertex;
			if(geometry->colors && geometry->colors[vertex].alpha != 0xFF)
				header->meshes[i].vertexAlpha = 1;
		}
		indexOffset += mesh[i].numIndices;
	}
}

static bool32
updateDynamicVertices(Geometry *geometry, D3D12InstanceDataHeader *header,
	                  const Vertex *vertices)
{
	const uint32 frame = getFrameIndex() % DYNAMIC_VERTEX_FRAME_COUNT;
	const uint64 size = header->numVertices*sizeof(Vertex);
	ID3D12Resource *&buffer = header->vertexBuffers[frame];
	if(buffer == nil){
		if(!createUploadBuffer(vertices, size, &buffer))
			return 0;
	}else{
		void *mapped = nil;
		D3D12_RANGE readRange = { 0, 0 };
		if(FAILED(buffer->Map(0, &readRange, &mapped)))
			return 0;
		memcpy(mapped, vertices, (size_t)size);
		buffer->Unmap(0, nil);
	}
	header->vertexBuffer = buffer;
	header->vertexView.BufferLocation = buffer->GetGPUVirtualAddress();
	header->vertexView.SizeInBytes = (UINT)size;
	header->vertexView.StrideInBytes = sizeof(Vertex);
	return 1;
}

static bool32
instanceGeometry(Geometry *geometry)
{
	if(geometry == nil || geometry->meshHeader == nil ||
	   geometry->numVertices <= 0 || geometry->morphTargets == nil ||
	   geometry->morphTargets[0].vertices == nil)
		return 0;
	if(geometry->flags & Geometry::NATIVE)
		return 0;

	if(geometry->instData){
		D3D12InstanceDataHeader *existing =
			(D3D12InstanceDataHeader*)geometry->instData;
		if(existing->platform == PLATFORM_D3D12 &&
		   existing->serialNumber == geometry->meshHeader->serialNum){
			if(geometry->lockedSinceInst == 0)
				return 1;
			const double instanceStartedMs = profileNowMs();
			Vertex *vertices = rwNewT(Vertex, existing->numVertices,
			                              MEMDUR_EVENT | ID_GEOMETRY);
			fillInstanceVertices(geometry, vertices);
			const bool32 ok = updateDynamicVertices(geometry, existing, vertices);
			rwFree(vertices);
			if(!ok)
				return 0;
			refreshInstanceMeshes(geometry, existing, nil);
			geometry->lockedSinceInst = 0;
			worldRenderProfile.geometryInstanceMs +=
				(float32)(profileNowMs()-instanceStartedMs);
			worldRenderProfile.geometryInstances++;
			return 1;
		}
		freeInstanceData(geometry);
	}
	const double instanceStartedMs = profileNowMs();

	D3D12InstanceDataHeader *header = rwNewT(
		D3D12InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header, 0, sizeof(*header));
	header->platform = PLATFORM_D3D12;
	header->serialNumber = geometry->meshHeader->serialNum;
	header->numMeshes = geometry->meshHeader->numMeshes;
	header->numVertices = geometry->numVertices;
	header->numIndices = geometry->meshHeader->totalIndices;
	header->topology = geometry->meshHeader->flags == MeshHeader::TRISTRIP ?
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP :
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	header->meshes = rwNewT(MeshDraw, header->numMeshes,
	                            MEMDUR_EVENT | ID_GEOMETRY);

	Vertex *vertices = rwNewT(Vertex, header->numVertices,
	                          MEMDUR_EVENT | ID_GEOMETRY);
	fillInstanceVertices(geometry, vertices);
	uint16 *indices = rwNewT(uint16, header->numIndices,
	                         MEMDUR_EVENT | ID_GEOMETRY);
	refreshInstanceMeshes(geometry, header, indices);

	const uint32 frame = getFrameIndex() % DYNAMIC_VERTEX_FRAME_COUNT;
	bool32 ok = createUploadBuffer(vertices,
		header->numVertices*sizeof(Vertex), &header->vertexBuffers[frame]) &&
		createUploadBuffer(indices,
		header->numIndices*sizeof(uint16), &header->indexBuffer);
	rwFree(vertices);
	rwFree(indices);
	if(!ok){
		releaseCom(header->vertexBuffers[frame]);
		releaseCom(header->indexBuffer);
		rwFree(header->meshes);
		rwFree(header);
		return 0;
	}
	header->vertexBuffer = header->vertexBuffers[frame];
	header->vertexView.BufferLocation =
		header->vertexBuffer->GetGPUVirtualAddress();
	header->vertexView.SizeInBytes = header->numVertices*sizeof(Vertex);
	header->vertexView.StrideInBytes = sizeof(Vertex);
	header->indexView.BufferLocation =
		header->indexBuffer->GetGPUVirtualAddress();
	header->indexView.SizeInBytes = header->numIndices*sizeof(uint16);
	header->indexView.Format = DXGI_FORMAT_R16_UINT;
	geometry->instData = header;
	geometry->lockedSinceInst = 0;
	worldRenderProfile.geometryInstanceMs +=
		(float32)(profileNowMs()-instanceStartedMs);
	worldRenderProfile.geometryInstances++;
	return 1;
}

static void
fillMatrix(float *dst, const Matrix *matrix)
{
	dst[0] = matrix->right.x; dst[1] = matrix->right.y;
	dst[2] = matrix->right.z; dst[3] = 0.0f;
	dst[4] = matrix->up.x; dst[5] = matrix->up.y;
	dst[6] = matrix->up.z; dst[7] = 0.0f;
	dst[8] = matrix->at.x; dst[9] = matrix->at.y;
	dst[10] = matrix->at.z; dst[11] = 0.0f;
	dst[12] = matrix->pos.x; dst[13] = matrix->pos.y;
	dst[14] = matrix->pos.z; dst[15] = 1.0f;
}

static bool32
allocateArenaConstants(const void *data, uint32 dataSize,
	                  D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(activeBoneArena != frame){
		activeBoneArena = frame;
		boneArenas[frame].offset = 0;
	}
	BoneArena &arena = boneArenas[frame];
	uint32 offset = (arena.offset + 255u) & ~255u;
	uint32 size = (dataSize + 255u) & ~255u;
	if(data == nil || address == nil || arena.mapped == nil ||
	   offset + size > BONE_UPLOAD_SIZE)
		return 0;
	memset(arena.mapped + offset, 0, size);
	memcpy(arena.mapped + offset, data, dataSize);
	*address = arena.resource->GetGPUVirtualAddress() + offset;
	arena.offset = offset + size;
	return 1;
}

bool32
beginStereoSinglePass(void)
{
	const uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	if(!pipelineReady || stereoSinglePassActive || stereoWorldEye != 0 ||
	   stereoRightCameraFrame != frame){
		worldRenderProfile.stereoSinglePassFallbacks++;
		return 0;
	}
	if(stereoRightCameraUploadFrame != frame || stereoRightCameraAddress == 0){
		if(!allocateArenaConstants(stereoRightCameraConstants,
		       sizeof(stereoRightCameraConstants), &stereoRightCameraAddress)){
			worldRenderProfile.stereoSinglePassFallbacks++;
			return 0;
		}
		stereoRightCameraUploadFrame = frame;
	}
	if(!setStereoWideViewport(1)){
		worldRenderProfile.stereoSinglePassFallbacks++;
		return 0;
	}
	stereoSinglePassActive = 1;
	worldRenderProfile.stereoSinglePassBegins++;
	FixedFoveatedRenderingInfo foveatedInfo;
	getFixedFoveatedRenderingInfo(&foveatedInfo);
	if(foveatedInfo.supported && foveatedInfo.enabled){
		if(beginFixedFoveatedRendering())
			worldRenderProfile.fixedFoveatedBegins++;
		else
			worldRenderProfile.fixedFoveatedFailures++;
	}
	return 1;
}

void
endStereoSinglePass(void)
{
	if(!stereoSinglePassActive)
		return;
	endFixedFoveatedRendering();
	stereoSinglePassActive = 0;
	setStereoWideViewport(0);
}

static bool32
allocateBoneConstants(const float *matrices,
	                  D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	return allocateArenaConstants(matrices,
	       MAX_SKIN_BONES*16*sizeof(float), address);
}

static bool32
allocateLightingConstants(const LightingConstants *constants,
	                     D3D12_GPU_VIRTUAL_ADDRESS *address)
{
	return allocateArenaConstants(constants, sizeof(*constants), address);
}

static void
collectLighting(Atomic *atomic, LightingConstants *constants)
{
	memset(constants, 0, sizeof(*constants));
	constants->ambient[3] = 1.0f;
	if(atomic == nil || atomic->geometry == nil ||
	   (atomic->geometry->flags & Geometry::LIGHT) == 0 ||
	   engine->currentWorld == nil)
		return;

	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	memset(&lightData, 0, sizeof(lightData));
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;
	((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	constants->ambient[0] = lightData.ambient.red;
	constants->ambient[1] = lightData.ambient.green;
	constants->ambient[2] = lightData.ambient.blue;
	int32 count = 0;
	for(int32 i = 0; i < lightData.numDirectionals &&
	    count < MAX_WORLD_LIGHTS; i++){
		Light *light = lightData.directionals[i];
		if(light == nil || light->getFrame() == nil)
			continue;
		V3d direction = light->getFrame()->getLTM()->at;
		constants->colorRadius[count][0] = light->color.red;
		constants->colorRadius[count][1] = light->color.green;
		constants->colorRadius[count][2] = light->color.blue;
		constants->colorRadius[count][3] = -1.0f;
		constants->directionClamp[count][0] = direction.x;
		constants->directionClamp[count][1] = direction.y;
		constants->directionClamp[count][2] = direction.z;
		constants->directionClamp[count][3] = -1.0f;
		count++;
	}
	for(int32 i = 0; i < lightData.numLocals &&
	    count < MAX_WORLD_LIGHTS; i++){
		Light *light = lightData.locals[i];
		if(light == nil || light->getFrame() == nil || light->radius <= 0.0f)
			continue;
		Matrix *matrix = light->getFrame()->getLTM();
		constants->colorRadius[count][0] = light->color.red;
		constants->colorRadius[count][1] = light->color.green;
		constants->colorRadius[count][2] = light->color.blue;
		constants->colorRadius[count][3] = light->radius;
		constants->positionCos[count][0] = matrix->pos.x;
		constants->positionCos[count][1] = matrix->pos.y;
		constants->positionCos[count][2] = matrix->pos.z;
		if(light->getType() == Light::POINT){
			constants->directionClamp[count][3] = -1.0f;
		}else{
			constants->positionCos[count][3] = light->minusCosAngle;
			constants->directionClamp[count][0] = matrix->at.x;
			constants->directionClamp[count][1] = matrix->at.y;
			constants->directionClamp[count][2] = matrix->at.z;
			constants->directionClamp[count][3] =
				light->getType() == Light::SOFTSPOT ? 0.0f : 1.0f;
		}
		count++;
	}
}

static bool32
uploadSkinMatrices(Atomic *atomic, D3D12_GPU_VIRTUAL_ADDRESS *address,
	               bool32 *isSkinned)
{
	float matrices[MAX_SKIN_BONES*16];
	memset(matrices, 0, sizeof(matrices));
	for(int32 i = 0; i < MAX_SKIN_BONES; i++){
		matrices[i*16] = 1.0f;
		matrices[i*16+5] = 1.0f;
		matrices[i*16+10] = 1.0f;
		matrices[i*16+15] = 1.0f;
	}
	Skin *skin = atomic && atomic->geometry ? Skin::get(atomic->geometry) : nil;
	*isSkinned = skin != nil && skin->numBones > 0 &&
		skin->inverseMatrices != nil;
	if(!*isSkinned){
		uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
		if(boneArenas[frame].resource == nil)
			return 0;
		*address = boneArenas[frame].resource->GetGPUVirtualAddress();
		return 1;
	}

	HAnimHierarchy *hierarchy = Skin::getHierarchy(atomic);
	int32 count = skin->numBones < MAX_SKIN_BONES ?
		skin->numBones : MAX_SKIN_BONES;
	if(hierarchy && hierarchy->matrices && atomic->getFrame()){
		if(hierarchy->numNodes < count)
			count = hierarchy->numNodes;
		Matrix *inverseMatrices = (Matrix*)skin->inverseMatrices;
		if(hierarchy->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(int32 i = 0; i < count; i++){
				Matrix result;
				Matrix::mult(&result, &inverseMatrices[i],
				             &hierarchy->matrices[i]);
				fillMatrix(matrices + i*16, &result);
			}
		}else{
			Matrix inverseAtomic;
			Matrix::invert(&inverseAtomic, atomic->getFrame()->getLTM());
			for(int32 i = 0; i < count; i++){
				Matrix local, result;
				Matrix::mult(&local, &hierarchy->matrices[i],
				             &inverseAtomic);
				Matrix::mult(&result, &inverseMatrices[i], &local);
				fillMatrix(matrices + i*16, &result);
			}
		}
	}
	return allocateBoneConstants(matrices, address);
}

enum MeshSelection {
	MESH_ALL,
	MESH_OPAQUE_ONLY,
	MESH_TRANSPARENT_ONLY
};

static bool32
renderGeometry(Atomic *atomic, MeshSelection selection, uint8 fadeAlpha)
{
	if(!pipelineReady || atomic == nil || atomic->geometry == nil ||
	   !instanceGeometry(atomic->geometry))
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(list == nil || camera == nil || atomic->getFrame() == nil)
		return 0;
	D3D12InstanceDataHeader *header =
		(D3D12InstanceDataHeader*)atomic->geometry->instData;
	const bool32 stereoDraw = stereoSinglePassActive &&
		stereoRightCameraAddress != 0;
	list->SetGraphicsRootSignature(stereoDraw ? stereoRootSignature : rootSignature);
	if(stereoDraw)
		list->SetGraphicsRootConstantBufferView(5, stereoRightCameraAddress);
	list->IASetPrimitiveTopology(header->topology);
	list->IASetVertexBuffers(0, 1, &header->vertexView);
	list->IASetIndexBuffer(&header->indexView);

	float constants[58];
	memset(constants, 0, sizeof(constants));
	fillMatrix(constants, atomic->getFrame()->getLTM());
	memcpy(constants + 16, &camera->devView, 16*sizeof(float));
	memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
	D3D12_GPU_VIRTUAL_ADDRESS boneAddress;
	bool32 isSkinned;
	LightingConstants lighting;
	StereoAtomicCacheEntry *cached = findStereoAtomicCache(atomic, 0);
	if(cached){
		boneAddress = cached->boneAddress;
		isSkinned = cached->isSkinned;
		lighting = cached->lighting;
	}else{
		if(!uploadSkinMatrices(atomic, &boneAddress, &isSkinned))
			return 0;
		collectLighting(atomic, &lighting);
		// Only the left eye populates the cache.  A right-eye-only edge object
		// simply follows the normal path instead of contaminating this frame's
		// left-eye snapshot.
		if(stereoWorldEye == 0){
			StereoAtomicCacheEntry *entry = findStereoAtomicCache(atomic, 1);
			if(entry){
				entry->boneAddress = boneAddress;
				entry->isSkinned = isSkinned;
				entry->lighting = lighting;
			}
		}
	}
	constants[53] = isSkinned ? 1.0f : 0.0f;
	constants[54] = (uint32)(uintptr_t)getRenderState(ALPHATESTREF)/255.0f;
	constants[55] = (float)(uint32)(uintptr_t)getRenderState(ALPHATESTFUNC);
	// World atomics are submitted from the renderer's fog-enabled passes, but
	// later compatibility draws can change the global RW state. Use the camera
	// range directly here so the modern backend cannot inherit a stale FALSE.
	if(camera->fogPlane < camera->farPlane){
		constants[56] = camera->farPlane;
		constants[57] = 1.0f/(camera->fogPlane - camera->farPlane);
	}
	uint32 packedFog = (uint32)(uintptr_t)getRenderState(FOGCOLOR);
	list->SetGraphicsRootConstantBufferView(2, boneAddress);
	D3D12_GPU_DESCRIPTOR_HANDLE fallback;
	getTextureView(whiteRaster, &fallback, nil);
	bool32 hasTransparent = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *material = header->meshes[i].material;
		RGBA color = material ? material->color : makeRGBA(255, 255, 255, 255);
		color.alpha = (uint8)((color.alpha*fadeAlpha)/255);
		constants[48] = color.red/255.0f;
		constants[49] = color.green/255.0f;
		constants[50] = color.blue/255.0f;
		constants[51] = color.alpha/255.0f;
		D3D12_GPU_DESCRIPTOR_HANDLE texture = fallback;
		D3D12_GPU_DESCRIPTOR_HANDLE sampler;
		getSamplerView(Texture::LINEAR, Texture::WRAP, Texture::WRAP, &sampler);
		bool32 textured = 0;
		bool32 textureAlpha = 0;
		if(material && material->texture && material->texture->raster){
			textured = getTextureView(material->texture->raster,
			                          &texture, &textureAlpha);
			getSamplerView(material->texture->getFilter(),
			               material->texture->getAddressU(),
			               material->texture->getAddressV(), &sampler);
		}
		bool32 transparent = fadeAlpha != 0xFF ||
			header->meshes[i].vertexAlpha || color.alpha != 0xFF ||
			(textured && textureAlpha);
		hasTransparent = hasTransparent || transparent;
		if(selection == MESH_OPAQUE_ONLY && transparent)
			continue;
		if(selection == MESH_TRANSPARENT_ONLY && !transparent)
			continue;
		if(material){
			lighting.surface[0] = material->surfaceProps.ambient;
			lighting.surface[1] = material->surfaceProps.specular;
			lighting.surface[2] = material->surfaceProps.diffuse;
			lighting.surface[3] = 0.0f;
		}else{
			lighting.surface[0] = 1.0f;
			lighting.surface[1] = 0.0f;
			lighting.surface[2] = 1.0f;
			lighting.surface[3] = 0.0f;
		}
		memcpy(&lighting.surface[3], &packedFog, sizeof(packedFog));
		D3D12_GPU_VIRTUAL_ADDRESS lightingAddress;
		if(!allocateLightingConstants(&lighting, &lightingAddress))
			return hasTransparent;
		list->SetGraphicsRootConstantBufferView(3, lightingAddress);
		uint32 blend = WORLD_BLEND_OPAQUE;
		if(transparent)
			blend = getWorldBlendMode();
		const bool depthTest = getRenderState(ZTESTENABLE) != nil;
		const bool depthWrite = getRenderState(ZWRITEENABLE) != nil;
		uint32 depth = depthTest ?
			(depthWrite ? WORLD_DEPTH_TEST_WRITE : WORLD_DEPTH_TEST_ONLY) :
			(depthWrite ? WORLD_DEPTH_WRITE_ONLY : WORLD_DEPTH_DISABLED);
		uint32 cullState = (uint32)(uintptr_t)getRenderState(CULLMODE);
		uint32 cull = cullState == CULLBACK ? WORLD_CULL_BACK :
			cullState == CULLFRONT ? WORLD_CULL_FRONT : WORLD_CULL_NONE;
		ID3D12PipelineState *pipeline = stereoDraw ?
			stereoWorldPipelines[blend][depth][cull] :
			worldPipelines[blend][depth][cull];
		list->SetPipelineState(pipeline);
		constants[52] = textured ? 1.0f : 0.0f;
		if(stereoDraw){
			D3D12_GPU_VIRTUAL_ADDRESS drawAddress;
			if(!allocateArenaConstants(constants, sizeof(constants), &drawAddress))
				return hasTransparent;
			list->SetGraphicsRootConstantBufferView(0, drawAddress);
		}else
			list->SetGraphicsRoot32BitConstants(0, 58, constants, 0);
		list->SetGraphicsRootDescriptorTable(1, texture);
		list->SetGraphicsRootDescriptorTable(4, sampler);
		list->DrawIndexedInstanced(header->meshes[i].numIndices,
		                           stereoDraw ? 2 : 1,
		                           header->meshes[i].startIndex, 0, 0);
		if(stereoCaptureSegment >= 0 && stereoWorldPacketValid){
			if(stereoWorldDrawCount < STEREO_WORLD_DRAW_CAPACITY){
				StereoWorldDraw &draw = stereoWorldDraws[stereoWorldDrawCount++];
				draw.topology = header->topology;
				draw.vertexView = header->vertexView;
				draw.indexView = header->indexView;
				draw.pipeline = pipeline;
				draw.boneAddress = boneAddress;
				draw.lightingAddress = lightingAddress;
				draw.texture = texture;
				draw.sampler = sampler;
				memcpy(draw.constants, constants, sizeof(draw.constants));
				draw.numIndices = header->meshes[i].numIndices;
				draw.startIndex = header->meshes[i].startIndex;
			}else{
				// Never replay a truncated world. The right eye will use the
				// original renderer for every segment when this safety limit is hit.
				stereoWorldPacketValid = 0;
			}
		}
		worldRenderProfile.drawCalls++;
		worldRenderProfile.submittedIndices += header->meshes[i].numIndices *
			(stereoDraw ? 2 : 1);
		if(stereoDraw){
			worldRenderProfile.stereoSinglePassDrawCalls++;
			worldRenderProfile.stereoSinglePassIndices +=
				header->meshes[i].numIndices * 2;
		}
	}
	return hasTransparent;
}

bool32
replayStereoWorldSegment(uint32 segment)
{
	if(stereoWorldEye != 1 || !stereoWorldPacketValid ||
	   segment >= STEREO_WORLD_SEGMENT_COUNT || !pipelineReady)
		return 0;
	const StereoWorldRange &range = stereoWorldRanges[segment];
	if(!range.complete || range.first + range.count > stereoWorldDrawCount)
		return 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	Camera *camera = engine->currentCamera;
	if(list == nil || camera == nil)
		return 0;

	list->SetGraphicsRootSignature(rootSignature);
	if(InterlockedCompareExchange(&stereoBundlePending, 0, 0) != 0)
		waitForStereoWorldBundleBuild();
	const uint32 frame = getFrameIndex() % BONE_FRAME_COUNT;
	const bool32 bundleReady = stereoBundleResourcesReady &&
		InterlockedCompareExchange(&stereoBundleSucceeded, 0, 0) != 0 &&
		stereoBundleCompletedGeneration == stereoWorldPacketGeneration &&
		stereoBundleCompletedFrame == frame &&
		stereoBundleFrames[frame].segments[segment] != nil;
	if(bundleReady){
		list->ExecuteBundle(stereoBundleFrames[frame].segments[segment]);
		worldRenderProfile.drawCalls += range.count;
		worldRenderProfile.stereoBundleDrawCalls += range.count;
		for(uint32 i = 0; i < range.count; i++)
			worldRenderProfile.submittedIndices +=
				stereoWorldDraws[range.first + i].numIndices;
		return 1;
	}
	if(stereoBundleResourcesReady)
		worldRenderProfile.stereoBundleFallbacks++;
	for(uint32 i = 0; i < range.count; i++){
		const StereoWorldDraw &draw = stereoWorldDraws[range.first + i];
		float constants[58];
		memcpy(constants, draw.constants, sizeof(constants));
		memcpy(constants + 16, &camera->devView, 16*sizeof(float));
		memcpy(constants + 32, &camera->devProj, 16*sizeof(float));
		list->IASetPrimitiveTopology(draw.topology);
		list->IASetVertexBuffers(0, 1, &draw.vertexView);
		list->IASetIndexBuffer(&draw.indexView);
		list->SetPipelineState(draw.pipeline);
		list->SetGraphicsRoot32BitConstants(0, 58, constants, 0);
		list->SetGraphicsRootDescriptorTable(1, draw.texture);
		list->SetGraphicsRootConstantBufferView(2, draw.boneAddress);
		list->SetGraphicsRootConstantBufferView(3, draw.lightingAddress);
		list->SetGraphicsRootDescriptorTable(4, draw.sampler);
		list->DrawIndexedInstanced(draw.numIndices, 1,
		                           draw.startIndex, 0, 0);
		worldRenderProfile.drawCalls++;
		worldRenderProfile.submittedIndices += draw.numIndices;
	}
	return 1;
}

bool32
renderAtomicFirstPass(Atomic *atomic)
{
	return renderGeometry(atomic, MESH_OPAQUE_ONLY, 255);
}

void
renderAtomicBlendPass(Atomic *atomic, uint8 fadeAlpha)
{
	renderGeometry(atomic, MESH_TRANSPARENT_ONLY, fadeAlpha);
}

static void
pipelineInstance(ObjPipeline*, Atomic *atomic)
{
	if(atomic && atomic->geometry)
		instanceGeometry(atomic->geometry);
}

static void
pipelineUninstance(ObjPipeline*, Atomic *atomic)
{
	if(atomic)
		freeInstanceData(atomic->geometry);
}

static void
pipelineRender(ObjPipeline*, Atomic *atomic)
{
	renderGeometry(atomic, MESH_ALL, 255);
}

#else

void *destroyNativeData(void *object, int32, int32) { return object; }

#endif

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipeline = ObjPipeline::create();
#ifdef RW_D3D12
	pipeline->init(PLATFORM_D3D12);
	pipeline->impl.instance = pipelineInstance;
	pipeline->impl.uninstance = pipelineUninstance;
	pipeline->impl.render = pipelineRender;
	createPipelineResources();
#endif
	return pipeline;
}

void
shutdownDefaultPipeline(void)
{
#ifdef RW_D3D12
	pipelineReady = 0;
	shutdownStereoBundleResources();
	if(whiteRaster){
		whiteRaster->destroy();
		whiteRaster = nil;
	}
	for(uint32 i = 0; i < BONE_FRAME_COUNT; i++){
		if(boneArenas[i].resource && boneArenas[i].mapped)
			boneArenas[i].resource->Unmap(0, nil);
		boneArenas[i].mapped = nil;
		boneArenas[i].offset = 0;
		releaseCom(boneArenas[i].resource);
	}
	activeBoneArena = UINT32_MAX;
	for(uint32 blend = 0; blend < WORLD_BLEND_COUNT; blend++)
		for(uint32 depth = 0; depth < WORLD_DEPTH_COUNT; depth++)
			for(uint32 cull = 0; cull < WORLD_CULL_COUNT; cull++)
			{
				releaseCom(worldPipelines[blend][depth][cull]);
				releaseCom(stereoWorldPipelines[blend][depth][cull]);
			}
	stereoSinglePassActive = 0;
	stereoRightCameraFrame = UINT32_MAX;
	stereoRightCameraUploadFrame = UINT32_MAX;
	stereoRightCameraAddress = 0;
	stereoTemporalCameraValid[0] = 0;
	stereoTemporalCameraValid[1] = 0;
	releaseCom(stereoRootSignature);
	releaseCom(rootSignature);
#endif
}

}
}
