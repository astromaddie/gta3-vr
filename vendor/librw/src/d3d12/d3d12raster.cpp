#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef RW_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwd3d12.h"
#include "rwd3d12impl.h"

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace d3d12 {

int32 nativeRasterOffset;

#ifdef RW_D3D12

enum { MAX_MIP_LEVELS = 16 };

static TextureUploadProfile textureUploadProfile;

static double
textureProfileNowMs(void)
{
	static LARGE_INTEGER frequency = {};
	if(frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart * 1000.0 / frequency.QuadPart;
}

void
resetTextureUploadProfile(void)
{
	memset(&textureUploadProfile, 0, sizeof(textureUploadProfile));
}

void
getTextureUploadProfile(TextureUploadProfile *profile)
{
	if(profile)
		*profile = textureUploadProfile;
}

struct D3D12Raster
{
	ID3D12Resource *resource;
	D3D12_RESOURCE_STATES state;
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu;
	D3D12_GPU_DESCRIPTOR_HANDLE srvGpu;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv;
	uint8 *backingStore[MAX_MIP_LEVELS];
	uint32 levelSize[MAX_MIP_LEVELS];
	uint32 levelStride[MAX_MIP_LEVELS];
	uint32 numLevels;
	uint32 lockedLevel;
	uint32 srvIndex;
	uint32 rtvIndex;
	uint32 dsvIndex;
	bool32 hasAlpha;
	uint32 dirtyLevels;
	bool32 uploaded;
	bool32 directWriteLock;
	DXGI_FORMAT gpuFormat;
	uint32 heapPage;
	uint64 heapOffset;
	uint64 heapSize;
};

#define GETD3D12RASTEREXT(raster) \
	PLUGINOFFSET(D3D12Raster, raster, nativeRasterOffset)

template<class T>
static void
releaseCom(T *&object)
{
	if(object){
		object->Release();
		object = nil;
	}
}

static D3D12_HEAP_PROPERTIES
heapProperties(D3D12_HEAP_TYPE type)
{
	D3D12_HEAP_PROPERTIES props;
	memset(&props, 0, sizeof(props));
	props.Type = type;
	props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	props.CreationNodeMask = 1;
	props.VisibleNodeMask = 1;
	return props;
}

static D3D12_RESOURCE_DESC
textureDesc(uint32 width, uint32 height, uint16 levels, DXGI_FORMAT format,
	        D3D12_RESOURCE_FLAGS flags)
{
	D3D12_RESOURCE_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = levels;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = flags;
	return desc;
}

static void
logTextureCreateFailure(const char *stage, Raster *raster, uint32 levels,
	HRESULT result)
{
	static uint32 failures;
	if(failures++ >= 64)
		return;
	FILE *file = fopen("d3d12_texture_fail.log", failures == 1 ? "w" : "a");
	if(file == nil)
		return;
	fprintf(file, "%s hr=%08lX size=%dx%d depth=%d format=%08X type=%d levels=%u\n",
		stage, (unsigned long)result, raster ? raster->width : 0,
		raster ? raster->height : 0, raster ? raster->depth : 0,
		raster ? raster->format : 0, raster ? raster->type : 0,
		(unsigned)levels);
	fclose(file);
}

static D3D12_RESOURCE_BARRIER
transitionBarrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
	              D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier;
	memset(&barrier, 0, sizeof(barrier));
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

static bool32
createTextureResource(Raster *raster, D3D12Raster *nativeRaster)
{
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COPY_DEST;
	if(raster->type == Raster::CAMERATEXTURE){
		flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	DXGI_FORMAT format = nativeRaster->gpuFormat;
	if(format == DXGI_FORMAT_UNKNOWN)
		format = raster->type == Raster::CAMERATEXTURE ?
			DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
	D3D12_RESOURCE_DESC desc = textureDesc(
		raster->width, raster->height, (uint16)nativeRaster->numLevels,
		format, flags);
	double profileStart = textureProfileNowMs();
	HRESULT result = S_OK;
	if(raster->type != Raster::CAMERATEXTURE){
		if(!allocatePlacedTextureResource(&desc, initialState,
		       &nativeRaster->resource, &nativeRaster->heapPage,
		       &nativeRaster->heapOffset, &nativeRaster->heapSize))
			result = E_OUTOFMEMORY;
	}else{
		D3D12_HEAP_PROPERTIES props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
		result = device->CreateCommittedResource(
		       &props, D3D12_HEAP_FLAG_NONE, &desc, initialState, nil,
		       IID_PPV_ARGS(&nativeRaster->resource));
	}
	textureUploadProfile.defaultResourceMs +=
		(float32)(textureProfileNowMs() - profileStart);
	textureUploadProfile.textureResources++;
	if(FAILED(result)){
		logTextureCreateFailure("CreateCommittedResource", raster,
			nativeRaster->numLevels, result);
		return 0;
	}
	nativeRaster->state = initialState;

	profileStart = textureProfileNowMs();
	if(!allocateShaderResourceDescriptor(&nativeRaster->srvCpu,
	                                     &nativeRaster->srvGpu,
	                                     &nativeRaster->srvIndex)){
		textureUploadProfile.descriptorMs +=
			(float32)(textureProfileNowMs() - profileStart);
		logTextureCreateFailure("AllocateSRV", raster,
			nativeRaster->numLevels, E_OUTOFMEMORY);
		return 0;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv;
	memset(&srv, 0, sizeof(srv));
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = nativeRaster->numLevels;
	device->CreateShaderResourceView(nativeRaster->resource, &srv,
	                                 nativeRaster->srvCpu);

	if(raster->type == Raster::CAMERATEXTURE){
		if(!allocateRenderTargetDescriptor(&nativeRaster->rtv,
		                                  &nativeRaster->rtvIndex))
			return 0;
		device->CreateRenderTargetView(nativeRaster->resource, nil,
		                               nativeRaster->rtv);
	}
	textureUploadProfile.descriptorMs +=
		(float32)(textureProfileNowMs() - profileStart);
	return 1;
}

static bool32
createDepthResource(Raster *raster, D3D12Raster *nativeRaster)
{
	ID3D12Device *device = getDevice();
	if(device == nil)
		return 0;

	// DLAA/TAA needs to sample the same depth that the legacy RenderWare
	// passes write.  Allocate a typeless resource and expose compatible DSV
	// and SRV views instead of maintaining a second, potentially divergent,
	// depth copy.
	D3D12_RESOURCE_DESC desc = textureDesc(
		raster->width, raster->height, 1, DXGI_FORMAT_R24G8_TYPELESS,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	D3D12_CLEAR_VALUE clearValue;
	memset(&clearValue, 0, sizeof(clearValue));
	clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	clearValue.DepthStencil.Depth = 1.0f;
	clearValue.DepthStencil.Stencil = 0;
	D3D12_HEAP_PROPERTIES props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if(FAILED(device->CreateCommittedResource(
	       &props, D3D12_HEAP_FLAG_NONE, &desc,
	       D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
	       IID_PPV_ARGS(&nativeRaster->resource))))
		return 0;
	nativeRaster->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	if(!allocateDepthDescriptor(&nativeRaster->dsv,
	                          &nativeRaster->dsvIndex))
		return 0;
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	memset(&dsvDesc, 0, sizeof(dsvDesc));
	dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	device->CreateDepthStencilView(nativeRaster->resource, &dsvDesc,
	                               nativeRaster->dsv);
	if(!allocateShaderResourceDescriptor(&nativeRaster->srvCpu,
	                                     &nativeRaster->srvGpu,
	                                     &nativeRaster->srvIndex))
		return 0;
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
	memset(&srvDesc, 0, sizeof(srvDesc));
	srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(nativeRaster->resource, &srvDesc,
	                                 nativeRaster->srvCpu);
	nativeRaster->gpuFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	raster->format = Raster::D24;
	raster->depth = 24;
	return 1;
}

static bool32
uploadLevels(Raster *raster, D3D12Raster *nativeRaster,
	          uint32 firstLevel, uint32 levelCount)
{
	ID3D12Device *device = getDevice();
	if(device == nil || nativeRaster->resource == nil ||
	   levelCount == 0 || firstLevel >= nativeRaster->numLevels ||
	   firstLevel + levelCount > nativeRaster->numLevels)
		return 0;

	D3D12_RESOURCE_DESC texture = nativeRaster->resource->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[MAX_MIP_LEVELS];
	UINT numRows[MAX_MIP_LEVELS] = {};
	UINT64 rowSizes[MAX_MIP_LEVELS] = {};
	UINT64 uploadSize = 0;
	double profileStart = textureProfileNowMs();
	device->GetCopyableFootprints(&texture, firstLevel, levelCount, 0,
	                             footprints, numRows, rowSizes, &uploadSize);
	textureUploadProfile.footprintMs +=
		(float32)(textureProfileNowMs() - profileStart);

	D3D12_RESOURCE_DESC buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = uploadSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource *upload = nil;
	D3D12_HEAP_PROPERTIES uploadProps = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
	profileStart = textureProfileNowMs();
	HRESULT uploadResult = device->CreateCommittedResource(
	       &uploadProps, D3D12_HEAP_FLAG_NONE, &buffer,
	       D3D12_RESOURCE_STATE_GENERIC_READ, nil, IID_PPV_ARGS(&upload));
	textureUploadProfile.uploadResourceMs +=
		(float32)(textureProfileNowMs() - profileStart);
	textureUploadProfile.uploadBytes += uploadSize;
	textureUploadProfile.uploads++;
	if(FAILED(uploadResult))
		return 0;

	profileStart = textureProfileNowMs();
	uint8 *mapped = nil;
	D3D12_RANGE readRange = { 0, 0 };
	if(FAILED(upload->Map(0, &readRange, (void**)&mapped))){
		releaseCom(upload);
		return 0;
	}
	for(uint32 i = 0; i < levelCount; i++){
		const uint32 level = firstLevel + i;
		for(UINT row = 0; row < numRows[i]; row++)
			memcpy(mapped + footprints[i].Offset +
			       row*footprints[i].Footprint.RowPitch,
			       nativeRaster->backingStore[level] +
			       row*nativeRaster->levelStride[level],
			       nativeRaster->levelStride[level]);
	}
	upload->Unmap(0, nil);
	textureUploadProfile.cpuCopyMs +=
		(float32)(textureProfileNowMs() - profileStart);

	profileStart = textureProfileNowMs();
	bool32 ok = 0;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list){
		if(nativeRaster->state != D3D12_RESOURCE_STATE_COPY_DEST){
			D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
				nativeRaster->resource, nativeRaster->state,
				D3D12_RESOURCE_STATE_COPY_DEST);
			list->ResourceBarrier(1, &barrier);
		}
		for(uint32 i = 0; i < levelCount; i++){
			D3D12_TEXTURE_COPY_LOCATION dst;
			memset(&dst, 0, sizeof(dst));
			dst.pResource = nativeRaster->resource;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = firstLevel + i;
			D3D12_TEXTURE_COPY_LOCATION src;
			memset(&src, 0, sizeof(src));
			src.pResource = upload;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = footprints[i];
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nil);
		}
		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			nativeRaster->resource, D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		list->ResourceBarrier(1, &barrier);
		deferRelease(upload);
		upload = nil;
		ok = 1;
	}else if(queueTextureUpload(nativeRaster->resource, nativeRaster->state,
	         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, upload,
	         firstLevel, levelCount)){
		upload = nil;
		ok = 1;
	}
	if(ok)
		nativeRaster->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	releaseCom(upload);
	textureUploadProfile.queueMs +=
		(float32)(textureProfileNowMs() - profileStart);
	return ok;
}

static bool32
readbackCameraTextureLevel(D3D12Raster *nativeRaster, uint32 level,
	                       uint8 *pixels, uint32 stride)
{
	ID3D12Device *device = getDevice();
	ID3D12CommandQueue *queue = getCommandQueue();
	if(device == nil || queue == nil || nativeRaster->resource == nil ||
	   pixels == nil || level >= nativeRaster->numLevels ||
	   !prepareForReadback())
		return 0;

	D3D12_RESOURCE_DESC texture = nativeRaster->resource->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows = 0;
	UINT64 rowSize = 0;
	UINT64 bufferSize = 0;
	device->GetCopyableFootprints(&texture, level, 1, 0, &footprint,
	                             &rows, &rowSize, &bufferSize);
	D3D12_RESOURCE_DESC buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = bufferSize;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES readbackProps =
		heapProperties(D3D12_HEAP_TYPE_READBACK);
	ID3D12Resource *readback = nil;
	ID3D12CommandAllocator *allocator = nil;
	ID3D12GraphicsCommandList *list = nil;
	bool32 ok = SUCCEEDED(device->CreateCommittedResource(
		&readbackProps, D3D12_HEAP_FLAG_NONE, &buffer,
		D3D12_RESOURCE_STATE_COPY_DEST, nil, IID_PPV_ARGS(&readback))) &&
		SUCCEEDED(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nil,
			IID_PPV_ARGS(&list)));
	D3D12_RESOURCE_STATES previousState = nativeRaster->state;
	if(ok && previousState != D3D12_RESOURCE_STATE_COPY_SOURCE){
		D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
			nativeRaster->resource, previousState,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->ResourceBarrier(1, &barrier);
	}
	if(ok){
		D3D12_TEXTURE_COPY_LOCATION dst;
		memset(&dst, 0, sizeof(dst));
		dst.pResource = readback;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION src;
		memset(&src, 0, sizeof(src));
		src.pResource = nativeRaster->resource;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = level;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nil);
		if(previousState != D3D12_RESOURCE_STATE_COPY_SOURCE){
			D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
				nativeRaster->resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
				previousState);
			list->ResourceBarrier(1, &barrier);
		}
		ok = SUCCEEDED(list->Close());
	}
	if(ok){
		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		ok = waitForGpu();
	}
	if(ok){
		uint8 *mapped = nil;
		D3D12_RANGE range = {
			(SIZE_T)footprint.Offset,
			(SIZE_T)(footprint.Offset + footprint.Footprint.RowPitch*rows)
		};
		ok = SUCCEEDED(readback->Map(0, &range, (void**)&mapped));
		if(ok){
			uint32 width = nativeRaster->levelStride[level]/4;
			for(UINT row = 0; row < rows; row++){
				uint8 *src = mapped + footprint.Offset +
					row*footprint.Footprint.RowPitch;
				uint8 *dst = pixels + row*stride;
				// Camera textures use R8G8B8A8 on the GPU; the librw raster
				// lock contract for C8888 exposes BGRA bytes.
				for(uint32 x = 0; x < width; x++){
					dst[x*4] = src[x*4+2];
					dst[x*4+1] = src[x*4+1];
					dst[x*4+2] = src[x*4];
					dst[x*4+3] = src[x*4+3];
				}
			}
			D3D12_RANGE written = { 0, 0 };
			readback->Unmap(0, &written);
		}
	}
	releaseCom(list);
	releaseCom(allocator);
	releaseCom(readback);
	return ok;
}

#endif

Raster*
rasterCreate(Raster *raster)
{
#ifdef RW_D3D12
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	nativeRaster->numLevels = 1;
	nativeRaster->hasAlpha = 0;
	nativeRaster->gpuFormat = raster->type == Raster::CAMERATEXTURE ?
		DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;

	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		goto done;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		goto done;

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		{
		int32 mipFlags = raster->format &
			(Raster::MIPMAP | Raster::AUTOMIPMAP);
		raster->format = Raster::C8888 | mipFlags;
		raster->depth = 32;
		raster->stride = raster->width*4;
		if((raster->format & Raster::MIPMAP) &&
		   !(raster->format & Raster::AUTOMIPMAP))
			nativeRaster->numLevels = Raster::calculateNumLevels(
				raster->width, raster->height);
		if(nativeRaster->numLevels > MAX_MIP_LEVELS)
			nativeRaster->numLevels = MAX_MIP_LEVELS;
		if(!createTextureResource(raster, nativeRaster))
			return nil;
		}
		break;
	case Raster::ZBUFFER:
		raster->stride = 0;
		if(!createDepthResource(raster, nativeRaster))
			return nil;
		break;
	case Raster::CAMERA:
		raster->format = Raster::C8888;
		raster->depth = 32;
		raster->stride = raster->width*4;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}

	if(raster->type == Raster::NORMAL || raster->type == Raster::TEXTURE ||
	   raster->type == Raster::CAMERATEXTURE){
		uint32 width = raster->width;
		uint32 height = raster->height;
		for(uint32 i = 0; i < nativeRaster->numLevels; i++){
			nativeRaster->levelStride[i] = width*4;
			nativeRaster->levelSize[i] = nativeRaster->levelStride[i]*height;
			nativeRaster->backingStore[i] = (uint8*)rwMalloc(
				nativeRaster->levelSize[i], MEMDUR_EVENT | ID_DRIVER);
			if(nativeRaster->backingStore[i] == nil)
				return nil;
			memset(nativeRaster->backingStore[i], 0,
			       nativeRaster->levelSize[i]);
			if(width > 1) width /= 2;
			if(height > 1) height /= 2;
		}
	}

done:
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = raster->pixels;
	return raster;
#else
	return nil;
#endif
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
#ifdef RW_D3D12
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(raster->privateFlags != 0 || level < 0 ||
	   (uint32)level >= nativeRaster->numLevels)
		return nil;
	if(raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE &&
	   raster->type != Raster::CAMERATEXTURE)
		return nil;

	uint32 width = raster->originalWidth;
	uint32 height = raster->originalHeight;
	for(int32 i = 0; i < level; i++){
		if(width > 1) width /= 2;
		if(height > 1) height /= 2;
	}
	raster->width = width;
	raster->height = height;
	raster->stride = nativeRaster->levelStride[level];
	// Native texture conversion fills a complete mip and does not need the old
	// contents. Write directly into the persistent CPU backing store in that
	// case; the old path allocated a temporary buffer and copied every mip a
	// second time before uploading it.
	nativeRaster->directWriteLock =
		(lockMode & Raster::LOCKWRITE) != 0 &&
		(lockMode & Raster::LOCKNOFETCH) != 0 &&
		(lockMode & Raster::LOCKREAD) == 0 &&
		raster->type != Raster::CAMERATEXTURE;
	if(nativeRaster->directWriteLock)
		raster->pixels = nativeRaster->backingStore[level];
	else{
		raster->pixels = (uint8*)rwMalloc(nativeRaster->levelSize[level],
		                                  MEMDUR_EVENT | ID_DRIVER);
		if(raster->pixels == nil)
			return nil;
	}
	if((lockMode & Raster::LOCKNOFETCH) == 0 ||
	   (lockMode & Raster::LOCKREAD)){
		if(raster->type == Raster::CAMERATEXTURE &&
		   (lockMode & Raster::LOCKREAD)){
			if(!readbackCameraTextureLevel(nativeRaster, level,
			                                  raster->pixels, raster->stride)){
				rwFree(raster->pixels);
				raster->pixels = raster->originalPixels;
				raster->width = raster->originalWidth;
				raster->height = raster->originalHeight;
				raster->stride = raster->originalStride;
				return nil;
			}
			memcpy(nativeRaster->backingStore[level], raster->pixels,
			       nativeRaster->levelSize[level]);
		}else
			memcpy(raster->pixels, nativeRaster->backingStore[level],
			       nativeRaster->levelSize[level]);
	}
	nativeRaster->lockedLevel = level;
	raster->privateFlags = lockMode;
	return raster->pixels;
#else
	return nil;
#endif
}

void
rasterUnlock(Raster *raster, int32 level)
{
#ifdef RW_D3D12
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(raster->pixels && (raster->privateFlags & Raster::LOCKWRITE) &&
	   level >= 0 && (uint32)level < nativeRaster->numLevels){
		if(!nativeRaster->directWriteLock)
			memcpy(nativeRaster->backingStore[level], raster->pixels,
			       nativeRaster->levelSize[level]);
		nativeRaster->dirtyLevels |= 1u << level;
		if(nativeRaster->uploaded){
			if(!uploadLevels(raster, nativeRaster, level, 1))
				fprintf(stderr, "librw D3D12: texture upload failed\n");
			nativeRaster->dirtyLevels &= ~(1u << level);
		}else{
			const uint32 allLevels = nativeRaster->numLevels >= 32 ? UINT32_MAX :
				((1u << nativeRaster->numLevels) - 1u);
			if((nativeRaster->dirtyLevels & allLevels) == allLevels){
				if(uploadLevels(raster, nativeRaster, 0, nativeRaster->numLevels)){
					nativeRaster->uploaded = 1;
					nativeRaster->dirtyLevels = 0;
				}else
					fprintf(stderr, "librw D3D12: texture upload failed\n");
			}
		}
	}
	if(raster->pixels && !nativeRaster->directWriteLock)
		rwFree(raster->pixels);
	nativeRaster->directWriteLock = 0;
	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
	raster->privateFlags = 0;
#endif
}

void
setRasterHasAlpha(Raster *raster, bool32 hasAlpha)
{
#ifdef RW_D3D12
	if(raster && raster->platform == PLATFORM_D3D12)
		GETD3D12RASTEREXT(raster)->hasAlpha = hasAlpha;
#else
	(void)raster;
	(void)hasAlpha;
#endif
}

bool32
allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha)
{
#ifdef RW_D3D12
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::TEXTURE || dxt < 1 || dxt > 5)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource != nil)
		return 0;

	if((raster->format & Raster::MIPMAP) == 0)
		numLevels = 1;
	if(numLevels < 1)
		numLevels = 1;
	if(numLevels > MAX_MIP_LEVELS)
		numLevels = MAX_MIP_LEVELS;
	nativeRaster->numLevels = (uint32)numLevels;
	nativeRaster->hasAlpha = hasAlpha;
	if(dxt == 1)
		nativeRaster->gpuFormat = DXGI_FORMAT_BC1_UNORM;
	else if(dxt == 2 || dxt == 3)
		nativeRaster->gpuFormat = DXGI_FORMAT_BC2_UNORM;
	else
		nativeRaster->gpuFormat = DXGI_FORMAT_BC3_UNORM;

	if(!createTextureResource(raster, nativeRaster))
		return 0;
	const uint32 blockBytes = dxt == 1 ? 8u : 16u;
	uint32 width = (uint32)raster->width;
	uint32 height = (uint32)raster->height;
	for(uint32 level = 0; level < nativeRaster->numLevels; level++){
		const uint32 blocksWide = width > 4 ? (width + 3)/4 : 1;
		const uint32 blocksHigh = height > 4 ? (height + 3)/4 : 1;
		nativeRaster->levelStride[level] = blocksWide*blockBytes;
		nativeRaster->levelSize[level] =
			nativeRaster->levelStride[level]*blocksHigh;
		nativeRaster->backingStore[level] = (uint8*)rwMalloc(
			nativeRaster->levelSize[level], MEMDUR_EVENT | ID_DRIVER);
		if(nativeRaster->backingStore[level] == nil)
			return 0;
		memset(nativeRaster->backingStore[level], 0,
		       nativeRaster->levelSize[level]);
		if(width > 1) width /= 2;
		if(height > 1) height /= 2;
	}
	raster->flags &= ~Raster::DONTALLOCATE;
	raster->stride = nativeRaster->levelStride[0];
	raster->originalStride = raster->stride;
	return 1;
#else
	(void)raster;
	(void)dxt;
	(void)numLevels;
	(void)hasAlpha;
	return 0;
#endif
}

int32
rasterNumLevels(Raster *raster)
{
#ifdef RW_D3D12
	return GETD3D12RASTEREXT(raster)->numLevels;
#else
	return 1;
#endif
}

bool32
imageFindRasterFormat(Image *image, int32 type, int32 *width, int32 *height,
	                  int32 *depth, int32 *format)
{
	if((type & 0xF) != Raster::TEXTURE)
		return 0;
	*width = image->width;
	*height = image->height;
	*depth = 32;
	*format = Raster::C8888 | type;
	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
#ifdef RW_D3D12
	if((raster->type & 0xF) != Raster::TEXTURE)
		return 0;
	Image *trueColor = nil;
	if(image->depth <= 8){
		trueColor = Image::create(image->width, image->height, image->depth);
		trueColor->pixels = image->pixels;
		trueColor->stride = image->stride;
		trueColor->palette = image->palette;
		trueColor->unpalettize();
		image = trueColor;
	}
	if(image->width != raster->width || image->height != raster->height ||
	   (image->depth != 24 && image->depth != 32 && image->depth != 16)){
		if(trueColor) trueColor->destroy();
		return 0;
	}

	bool32 wasLocked = raster->pixels != nil &&
		(raster->privateFlags & Raster::LOCKWRITE) != 0;
	uint8 *pixels = wasLocked ? raster->pixels :
		raster->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(pixels == nil){
		if(trueColor) trueColor->destroy();
		return 0;
	}
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	nativeRaster->hasAlpha = nativeRaster->hasAlpha || image->hasAlpha();
	for(int32 y = 0; y < image->height; y++){
		uint8 *src = image->pixels + y*image->stride;
		uint8 *dst = pixels + y*raster->stride;
		for(int32 x = 0; x < image->width; x++){
			if(image->depth == 32)
				conv_BGRA8888_from_RGBA8888(dst, src);
			else if(image->depth == 24)
				conv_BGRA8888_from_RGB888(dst, src);
			else{
				uint8 rgba[4];
				conv_RGBA8888_from_ARGB1555(rgba, src);
				conv_BGRA8888_from_RGBA8888(dst, rgba);
			}
			src += image->bpp;
			dst += 4;
		}
	}
	if(!wasLocked)
		raster->unlock(0);
	if(trueColor) trueColor->destroy();
	return 1;
#else
	return 0;
#endif
}

Image*
rasterToImage(Raster *raster)
{
#ifdef RW_D3D12
	if(raster->type == Raster::CAMERA){
		Image *image = Image::create(raster->width, raster->height, 32);
		if(image == nil)
			return nil;
		image->allocate();
		if(image->pixels == nil ||
		   !readPresentedFrame(image->pixels, image->stride,
		                       image->width, image->height)){
			image->destroy();
			return nil;
		}
		return image;
	}
	if(raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE &&
	   raster->type != Raster::CAMERATEXTURE)
		return nil;
	uint8 *pixels = raster->lock(0, Raster::LOCKREAD);
	if(pixels == nil)
		return nil;
	Image *image = Image::create(raster->width, raster->height, 32);
	image->allocate();
	for(int32 y = 0; y < raster->height; y++){
		uint8 *src = pixels + y*raster->stride;
		uint8 *dst = image->pixels + y*image->stride;
		for(int32 x = 0; x < raster->width; x++){
			conv_RGBA8888_from_BGRA8888(dst, src);
			src += 4;
			dst += 4;
		}
	}
	raster->unlock(0);
	return image;
#else
	return nil;
#endif
}

#ifdef RW_D3D12
bool32
transitionDepthRaster(Raster *raster, D3D12_RESOURCE_STATES state)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil)
		return 0;
	if(nativeRaster->state == state)
		return 1;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return 0;
	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
		nativeRaster->resource, nativeRaster->state, state);
	list->ResourceBarrier(1, &barrier);
	nativeRaster->state = state;
	return 1;
}

bool32
getDepthTarget(Raster *raster, ID3D12Resource **resource,
	           D3D12_CPU_DESCRIPTOR_HANDLE *view)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->dsv.ptr == 0)
		return 0;
	if(!transitionDepthRaster(raster, D3D12_RESOURCE_STATE_DEPTH_WRITE))
		return 0;
	if(resource) *resource = nativeRaster->resource;
	if(view) *view = nativeRaster->dsv;
	return 1;
}

bool32
getDepthTextureView(Raster *raster, ID3D12Resource **resource,
	                D3D12_GPU_DESCRIPTOR_HANDLE *view)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->srvGpu.ptr == 0)
		return 0;
	const D3D12_RESOURCE_STATES shaderRead =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	if(!transitionDepthRaster(raster, shaderRead))
		return 0;
	if(resource) *resource = nativeRaster->resource;
	if(view) *view = nativeRaster->srvGpu;
	return 1;
}

bool32
getColorTarget(Raster *raster, ID3D12Resource **resource,
	           D3D12_CPU_DESCRIPTOR_HANDLE *view)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type != Raster::CAMERATEXTURE)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->rtv.ptr == 0)
		return 0;
	if(resource) *resource = nativeRaster->resource;
	if(view) *view = nativeRaster->rtv;
	return 1;
}

bool32
getRasterResource(Raster *raster, ID3D12Resource **resource)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type == Raster::CAMERA || raster->type == Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil)
		return 0;
	if(resource) *resource = nativeRaster->resource;
	return 1;
}

bool32
transitionRaster(Raster *raster, D3D12_RESOURCE_STATES state)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   raster->type == Raster::CAMERA || raster->type == Raster::ZBUFFER)
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil)
		return 0;
	if(nativeRaster->state == state)
		return 1;
	ID3D12GraphicsCommandList *list = getCommandList();
	if(list == nil)
		return 0;
	D3D12_RESOURCE_BARRIER barrier = transitionBarrier(
		nativeRaster->resource, nativeRaster->state, state);
	list->ResourceBarrier(1, &barrier);
	nativeRaster->state = state;
	return 1;
}

bool32
getTextureView(Raster *raster, D3D12_GPU_DESCRIPTOR_HANDLE *view,
	           bool32 *hasAlpha)
{
	if(raster)
		raster = raster->parent;
	if(raster == nil || raster->platform != PLATFORM_D3D12 ||
	   (raster->type != Raster::NORMAL && raster->type != Raster::TEXTURE &&
	    raster->type != Raster::CAMERATEXTURE))
		return 0;
	D3D12Raster *nativeRaster = GETD3D12RASTEREXT(raster);
	if(nativeRaster->resource == nil || nativeRaster->srvGpu.ptr == 0)
		return 0;
	// Native readers normally fill the complete mip chain before sampling. If
	// a caller supplied only part of it, upload the backing store on first use.
	if(raster->type != Raster::CAMERATEXTURE && !nativeRaster->uploaded){
		if(!uploadLevels(raster, nativeRaster, 0, nativeRaster->numLevels))
			return 0;
		nativeRaster->uploaded = 1;
		nativeRaster->dirtyLevels = 0;
	}
	if(nativeRaster->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
	   !transitionRaster(raster, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
		return 0;
	if(view) *view = nativeRaster->srvGpu;
	if(hasAlpha) *hasAlpha = nativeRaster->hasAlpha;
	return 1;
}
#endif

static void*
createNativeRaster(void *object, int32 offset, int32)
{
#ifdef RW_D3D12
	D3D12Raster *raster = PLUGINOFFSET(D3D12Raster, object, offset);
	memset(raster, 0, sizeof(*raster));
	raster->srvIndex = UINT32_MAX;
	raster->rtvIndex = UINT32_MAX;
	raster->dsvIndex = UINT32_MAX;
	raster->heapPage = UINT32_MAX;
#endif
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
#ifdef RW_D3D12
	D3D12Raster *raster = PLUGINOFFSET(D3D12Raster, object, offset);
	deferRelease(raster->resource);
	if(raster->heapPage != UINT32_MAX)
		deferTextureAllocationRelease(raster->heapPage, raster->heapOffset,
		                              raster->heapSize);
	deferDescriptorRelease(raster->srvIndex, raster->rtvIndex,
	                       raster->dsvIndex);
	raster->resource = nil;
	raster->srvIndex = raster->rtvIndex = raster->dsvIndex = UINT32_MAX;
	raster->heapPage = UINT32_MAX;
	raster->heapOffset = raster->heapSize = 0;
	raster->srvCpu.ptr = raster->srvGpu.ptr = 0;
	raster->rtv.ptr = raster->dsv.ptr = 0;
	for(uint32 i = 0; i < MAX_MIP_LEVELS; i++){
		if(raster->backingStore[i]){
			rwFree(raster->backingStore[i]);
			raster->backingStore[i] = nil;
		}
	}
#endif
	return object;
}

static void*
copyNativeRaster(void *dst, void*, int32 offset, int32)
{
#ifdef RW_D3D12
	D3D12Raster *raster = PLUGINOFFSET(D3D12Raster, dst, offset);
	memset(raster, 0, sizeof(*raster));
	raster->srvIndex = UINT32_MAX;
	raster->rtvIndex = UINT32_MAX;
	raster->dsvIndex = UINT32_MAX;
	raster->heapPage = UINT32_MAX;
#endif
	return dst;
}

void
registerNativeRaster(void)
{
#ifdef RW_D3D12
	nativeRasterOffset = Raster::registerPlugin(
		sizeof(D3D12Raster), ID_RASTERD3D12, createNativeRaster,
		destroyNativeRaster, copyNativeRaster);
#endif
}

}
}
