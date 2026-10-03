#pragma once

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

namespace rw {
struct Raster;

namespace d3d12 {

// CPU-side cost accumulated while the current streaming item creates and
// uploads D3D12 textures. The OpenXR profiler snapshots this alongside the
// slowest streamed item so texture stalls can be split into allocator, copy
// and command-recording costs without doing synchronous per-texture logging.
struct TextureUploadProfile
{
	float32 defaultResourceMs;
	float32 descriptorMs;
	float32 footprintMs;
	float32 uploadResourceMs;
	float32 cpuCopyMs;
	float32 queueMs;
	uint64 uploadBytes;
	uint32 textureResources;
	uint32 uploads;
};

void resetTextureUploadProfile(void);
void getTextureUploadProfile(TextureUploadProfile *profile);

struct FrameSyncProfile
{
	float32 frameFenceWaitMs;
	float32 fullGpuWaitMs;
};

void resetFrameSyncProfile(void);
void getFrameSyncProfile(FrameSyncProfile *profile);

// Work that is normally hidden inside the legacy RenderWare atomic callback.
// In particular, geometry is converted and two upload buffers are allocated
// the first time a newly streamed model is drawn. That can look like an
// unexplained eye-render spike unless it is reported separately.
struct WorldRenderProfile
{
	float32 geometryInstanceMs;
	float32 bufferUploadMs;
	uint64 bufferBytes;
	uint64 submittedIndices;
	uint32 geometryInstances;
	uint32 drawCalls;
	float32 stereoBundleBuildMs;
	float32 stereoBundleWaitMs;
	uint32 stereoBundleDrawCalls;
	uint32 stereoBundleFallbacks;
	uint32 stereoSinglePassBegins;
	uint32 stereoSinglePassDrawCalls;
	uint64 stereoSinglePassIndices;
	uint32 stereoSinglePassFallbacks;
	uint32 fixedFoveatedBegins;
	uint32 fixedFoveatedFailures;
};

void resetWorldRenderProfile(void);
void getWorldRenderProfile(WorldRenderProfile *profile);

// The two legacy RenderScene passes see the same animated pose and the same
// world lights.  Let the D3D12 pipeline retain those eye-independent constants
// after the left eye and reuse them while recording the right eye.
void setStereoWorldEye(int32 eye);
// Capture the active eye matrices and temporarily address the complete
// double-wide OpenXR target.  World geometry can then use one instanced draw
// to rasterize both eye views while immediate effects keep their sub-viewports.
void captureStereoWorldCamera(int32 eye, float32 jitterClipX, float32 jitterClipY);
bool32 getStereoWorldCamera(int32 eye, float32 view[16], float32 projection[16]);
bool32 beginStereoSinglePass(void);
void endStereoSinglePass(void);

// Tier 2 D3D12 variable-rate shading profile used by the double-wide OpenXR
// world pass.  Profile zero disables it; the other profiles progressively
// reduce the full-rate central region while retaining an automatic 1x1
// fallback on GPUs without a shading-rate image.
enum FixedFoveatedProfile {
	FIXED_FOVEATED_OFF,
	FIXED_FOVEATED_QUALITY,
	FIXED_FOVEATED_BALANCED,
	FIXED_FOVEATED_PERFORMANCE,
	FIXED_FOVEATED_PROFILE_COUNT
};

struct FixedFoveatedRenderingInfo
{
	bool32 supported;
	bool32 enabled;
	bool32 active;
	bool32 additionalRates;
	uint32 tier;
	uint32 tileSize;
	uint32 profile;
	uint32 imageWidth;
	uint32 imageHeight;
};

void setFixedFoveatedRenderingProfile(uint32 profile);
void getFixedFoveatedRenderingInfo(FixedFoveatedRenderingInfo *info);
bool32 beginFixedFoveatedRendering(void);
void endFixedFoveatedRendering(void);

// Stage 6 stereo frame packet.  The legacy game builds these heavy world
// passes while rendering the left eye.  D3D12 stores the fully resolved draw
// commands (including skinning, lighting and material state), then replays the
// same immutable commands with the right-eye camera matrices.  Immediate-mode
// weather, water and screen effects deliberately stay on the original path.
enum StereoWorldSegment {
	STEREO_WORLD_ROADS,
	STEREO_WORLD_ENTITIES,
	STEREO_WORLD_BOATS,
	STEREO_WORLD_FADING_UNDERWATER,
	STEREO_WORLD_FADING,
	STEREO_WORLD_SEGMENT_COUNT
};
void beginStereoWorldCapture(uint32 segment);
void endStereoWorldCapture(uint32 segment);
void queueStereoWorldBundleBuild(const float32 *view,
                                 const float32 *projection);
bool32 replayStereoWorldSegment(uint32 segment);
void cancelStereoWorldPacket(void);

// Suballocate ordinary sampled textures from large default heaps. Render
// targets and depth buffers keep their dedicated committed-resource path.
bool32 allocatePlacedTextureResource(const D3D12_RESOURCE_DESC *desc,
                                     D3D12_RESOURCE_STATES initialState,
                                     ID3D12Resource **resource,
                                     uint32 *heapPage, uint64 *heapOffset,
                                     uint64 *heapSize);
void deferTextureAllocationRelease(uint32 heapPage, uint64 heapOffset,
                                   uint64 heapSize);

ID3D12Device *getDevice(void);
// Streamline has to receive the final D3D12 device before librw creates the
// command queue and swap chain.  The game registers this callback before RW
// initialization so renderer-independent builds keep no Streamline dependency.
typedef void (*DeviceCreatedCallback)(void);
void setDeviceCreatedCallback(DeviceCreatedCallback callback);
ID3D12CommandQueue *getCommandQueue(void);
ID3D12GraphicsCommandList *getCommandList(void);
ID3D12DescriptorHeap *getShaderResourceHeap(void);
ID3D12DescriptorHeap *getSamplerHeap(void);
uint32 getFrameIndex(void);
void getPresentSize(int32 *width, int32 *height);
void setPresentInterval(uint32 interval);
// Size of the camera raster currently bound by beginUpdate.  Im2D vertices
// are expressed in pixels of that raster, which can differ from the desktop
// swapchain for VR eyes, HUD layers and other camera textures.
void getCurrentRenderTargetSize(int32 *width, int32 *height);
bool32 readPresentedFrame(uint8 *pixels, uint32 stride,
                          int32 width, int32 height);
bool32 prepareForReadback(void);
// Submit the active command list to the graphics queue without stalling the
// CPU. OpenXR owns completion synchronization after xrReleaseSwapchainImage.
bool32 submitForExternal(void);
// Submit the currently open graphics list and wait until resources shared with
// an external compositor (OpenXR) are safe to release.
bool32 submitAndWaitForExternal(void);
bool32 copyCurrentBackBufferToExternal(ID3D12Resource *destination);
bool32 uploadRgbaToExternal(ID3D12Resource *destination, const uint8 *pixels,
                            uint32 stride, int32 width, int32 height);
void deferRelease(IUnknown *object);
// Upload command allocators, lists and buffers are submitted before the next
// presented frame. Keep them alive until that frame's fence has completed.
void deferReleaseAfterNextSubmit(IUnknown *object);
// Queue a texture copy for the next regular frame command list. Ownership of
// upload transfers to the queue on success.
bool32 queueTextureUpload(ID3D12Resource *destination,
                          D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after,
                          ID3D12Resource *upload,
                          uint32 firstLevel, uint32 levelCount);

bool32 allocateShaderResourceDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
                                        uint32 *index);
bool32 getSamplerView(uint32 filter, uint32 addressU, uint32 addressV,
                      D3D12_GPU_DESCRIPTOR_HANDLE *gpu);
bool32 allocateDepthDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                               uint32 *index);
bool32 allocateRenderTargetDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE *cpu,
                                      uint32 *index);
void deferDescriptorRelease(uint32 srvIndex, uint32 rtvIndex,
                            uint32 dsvIndex);

bool32 getDepthTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
// The D24S8 depth allocation is typeless internally so temporal passes can
// sample its depth plane through an R24_UNORM_X8_TYPELESS SRV.
bool32 transitionDepthRaster(Raster *raster, D3D12_RESOURCE_STATES state);
bool32 getDepthTextureView(Raster *raster, ID3D12Resource **resource,
                           D3D12_GPU_DESCRIPTOR_HANDLE *view);
bool32 getColorTarget(Raster *raster, ID3D12Resource **resource,
                      D3D12_CPU_DESCRIPTOR_HANDLE *view);
bool32 getRasterResource(Raster *raster, ID3D12Resource **resource);
bool32 transitionRaster(Raster *raster, D3D12_RESOURCE_STATES state);
bool32 setStereoWideViewport(bool32 wide);
bool32 getTextureView(Raster *raster, D3D12_GPU_DESCRIPTOR_HANDLE *view,
                      bool32 *hasAlpha);
// Draw a camera texture into an external typeless RGBA8 target while remapping
// its UV rectangle. Used by OpenXR to convert the stable symmetric game view
// into each runtime-provided asymmetric eye frustum.
bool32 resolveRasterToExternal(Raster *source, ID3D12Resource *destination,
                               int32 width, int32 height,
                               float32 uvScaleX, float32 uvScaleY,
                               float32 uvOffsetX, float32 uvOffsetY,
                               bool32 fxaaEnabled, uint32 colorMode,
                               const float32 blurColor[4],
                               const float32 contrastMult[3],
                               const float32 contrastAdd[3]);
// Resolve a native shader-readable texture into an OpenXR swapchain image.
// DLAA owns the source resource and descriptor, so unlike the raster version
// this function does not perform a source state transition.
bool32 resolveTextureToExternal(ID3D12Resource *source,
                                D3D12_GPU_DESCRIPTOR_HANDLE sourceView,
                                ID3D12Resource *destination,
                                int32 sourceWidth, int32 sourceHeight,
                                int32 width, int32 height,
                                float32 uvScaleX, float32 uvScaleY,
                                float32 uvOffsetX, float32 uvOffsetY,
                                bool32 fxaaEnabled, uint32 colorMode,
                                const float32 blurColor[4],
                                const float32 contrastMult[3],
                                const float32 contrastAdd[3]);

bool32 initializeImmediate(void);
void shutdownImmediate(void);
void setRenderState(int32 state, void *value);
void *getRenderState(int32 state);
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1,
                       int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType type, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType type, void *vertices,
                               int32 numVertices, void *indices,
                               int32 numIndices);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world,
                   uint32 flags);
void im3DRenderPrimitive(PrimitiveType type);
void im3DRenderIndexedPrimitive(PrimitiveType type, void *indices,
                               int32 numIndices);
void im3DEnd(void);

}
}
#endif
