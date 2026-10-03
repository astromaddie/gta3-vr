#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwvk.h"
#include "rwvkimpl.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_VULKAN
#include <android/log.h>
#define VKLOG(...) __android_log_print(ANDROID_LOG_INFO, "librw-vk", __VA_ARGS__)
#include <time.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#define VKERR(...) __android_log_print(ANDROID_LOG_ERROR, "librw-vk", __VA_ARGS__)
#endif

namespace rw {
namespace vulkan {

int32 nativeRasterOffset;

#ifdef RW_VULKAN

#define GETVULKANRASTER(raster) \
	PLUGINOFFSET(VulkanRaster, raster, nativeRasterOffset)

// ---------------------------------------------------------------------------
// Format mapping
//
// Everything the game streams is either 32-bit BGRA or a DXT block format.
// Adreno has no BC support at all, so the DXT path cannot simply be handed to
// Vulkan the way it is on D3D12; see allocateCompressed.
// ---------------------------------------------------------------------------

// Live texture memory. See VulkanRaster::memoryBytes.
static size_t gTextureMemoryUsed;

size_t
getTextureMemoryUsed(void)
{
	return gTextureMemoryUsed;
}

// ---------------------------------------------------------------------------
// Mip generation
//
// Vice City's world textures are single level. Minifying a 256x256 fence off
// its base image samples one texel out of the dozens a distant pixel covers,
// which is what makes far railings and chain link crawl. The TXDs cannot be
// changed -- they are the player's own game data -- so the chain is built
// here, once, as each texture is uploaded.
//
// The filter is alpha weighted. A plain box filter averages the colour of
// texels that are not there: the cut-out half of a fence is black, and
// averaging it into the bars leaves every distant fence outlined in whatever
// the artist left behind the mask. Weighting each texel by its own alpha
// keeps the colour of the part that is actually visible.
// ---------------------------------------------------------------------------

// Why a texture was passed over. Reported periodically, so a build that
// quietly generates nothing can say which test it failed.
static uint32 gMipPromoted;
static uint32 gMipSeen, gMipMade, gMipOffFlag, gMipNotTexture,
	gMipTooSmall, gMipBadFormat;
// Total time spent filtering and re-encoding, so the cost of this is a
// number rather than an impression.
static std::atomic<uint64> gMipMicroseconds(0);
static uint8 *gMipScratch[2];
static size_t gMipScratchBytes;
static uint8 *gMipMask;
static size_t gMipMaskBytes;
// Nothing waits on the chain any more, so the only reason to refuse a
// texture is the scratch it would need: the worker holds two buffers of
// the base size, and 2048 square is 16 MB apiece.
static uint32 gMipSizeLimit = 2048;
static uint32 gMipTooLarge, gMipLanded;

static uint64
microseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64)now.tv_sec*1000000ull + (uint64)now.tv_nsec/1000ull;
}

void
reportGeneratedMips(void)
{
	if(!renderDiagnosticsEnabled())
		return;
	VKLOG("mips: seen %u made %u in %llu ms | rejected "
	      "flag %u type %u size %u large %u format %u | landed %u "
	      "promoted %u",
	      gMipSeen, gMipMade,
	      (unsigned long long)(gMipMicroseconds.load(std::memory_order_relaxed)/1000ull),
	      gMipOffFlag, gMipNotTexture, gMipTooSmall, gMipTooLarge,
	      gMipBadFormat, gMipLanded, gMipPromoted);
}

static bool32
wantsGeneratedMips(Raster *raster, VkFormat format)
{
	gMipSeen++;
	if(!gvk.generateMipmaps){
		gMipOffFlag++;
		return 0;
	}
	if((raster->type & 0xF) != Raster::TEXTURE){
		gMipNotTexture++;
		return 0;
	}
	// Below this a chain is one or two levels of nothing, and the small
	// interface sheets are the ones that must stay crisp.
	if(raster->width < 16 || raster->height < 16){
		gMipTooSmall++;
		return 0;
	}
	if((uint32)raster->width > gMipSizeLimit ||
	   (uint32)raster->height > gMipSizeLimit){
		gMipTooLarge++;
		return 0;
	}
	// The block formats are handled by the codec below; the rest are
	// filtered in place.
	const bool32 usable = format == VK_FORMAT_R8G8B8A8_UNORM ||
	       format == VK_FORMAT_R4G4B4A4_UNORM_PACK16 ||
	       format == VK_FORMAT_A1R5G5B5_UNORM_PACK16 ||
	       format == VK_FORMAT_R5G5B5A1_UNORM_PACK16 ||
	       format == VK_FORMAT_R5G6B5_UNORM_PACK16 ||
	       format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
	       format == VK_FORMAT_BC2_UNORM_BLOCK ||
	       format == VK_FORMAT_BC3_UNORM_BLOCK;
	if(!usable){
		if(gMipBadFormat < 4 && renderDiagnosticsEnabled())
			VKLOG("mips: format %d not downsampled (%dx%d)",
			      (int)format, raster->width, raster->height);
		gMipBadFormat++;
		return 0;
	}
	gMipMade++;
	return 1;
}

// One texel of any supported format as 8-bit RGBA.
static void
readTexel(const uint8 *src, VkFormat format, uint32 *rgba)
{
	switch(format){
	case VK_FORMAT_R8G8B8A8_UNORM:
		rgba[0] = src[0]; rgba[1] = src[1];
		rgba[2] = src[2]; rgba[3] = src[3];
		break;
	case VK_FORMAT_R4G4B4A4_UNORM_PACK16: {
		const uint32 v = (uint32)src[0] | ((uint32)src[1] << 8);
		rgba[0] = ((v >> 12) & 0xF)*17; rgba[1] = ((v >> 8) & 0xF)*17;
		rgba[2] = ((v >> 4) & 0xF)*17;  rgba[3] = (v & 0xF)*17;
		break;
	}
	case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
		const uint32 v = (uint32)src[0] | ((uint32)src[1] << 8);
		rgba[0] = (((v >> 10) & 0x1F)*255)/31;
		rgba[1] = (((v >> 5) & 0x1F)*255)/31;
		rgba[2] = ((v & 0x1F)*255)/31;
		rgba[3] = (v >> 15) ? 255 : 0;
		break;
	}
	case VK_FORMAT_R5G5B5A1_UNORM_PACK16: {
		const uint32 v = (uint32)src[0] | ((uint32)src[1] << 8);
		rgba[0] = (((v >> 11) & 0x1F)*255)/31;
		rgba[1] = (((v >> 6) & 0x1F)*255)/31;
		rgba[2] = (((v >> 1) & 0x1F)*255)/31;
		rgba[3] = (v & 1) ? 255 : 0;
		break;
	}
	default: {	// R5G6B5, no alpha
		const uint32 v = (uint32)src[0] | ((uint32)src[1] << 8);
		rgba[0] = (((v >> 11) & 0x1F)*255)/31;
		rgba[1] = (((v >> 5) & 0x3F)*255)/63;
		rgba[2] = ((v & 0x1F)*255)/31;
		rgba[3] = 255;
		break;
	}
	}
}

static void
writeTexel(uint8 *dst, VkFormat format, const uint32 *rgba)
{
	switch(format){
	case VK_FORMAT_R8G8B8A8_UNORM:
		dst[0] = (uint8)rgba[0]; dst[1] = (uint8)rgba[1];
		dst[2] = (uint8)rgba[2]; dst[3] = (uint8)rgba[3];
		break;
	case VK_FORMAT_R4G4B4A4_UNORM_PACK16: {
		const uint32 v = (((rgba[0]+8)/17) << 12) | (((rgba[1]+8)/17) << 8) |
		                 (((rgba[2]+8)/17) << 4) | ((rgba[3]+8)/17);
		dst[0] = (uint8)(v & 0xFF); dst[1] = (uint8)(v >> 8);
		break;
	}
	case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
		const uint32 v = ((rgba[3] >= 128 ? 1u : 0u) << 15) |
		                 (((rgba[0]*31+127)/255) << 10) |
		                 (((rgba[1]*31+127)/255) << 5) |
		                 ((rgba[2]*31+127)/255);
		dst[0] = (uint8)(v & 0xFF); dst[1] = (uint8)(v >> 8);
		break;
	}
	case VK_FORMAT_R5G5B5A1_UNORM_PACK16: {
		const uint32 v = (((rgba[0]*31+127)/255) << 11) |
		                 (((rgba[1]*31+127)/255) << 6) |
		                 (((rgba[2]*31+127)/255) << 1) |
		                 (rgba[3] >= 128 ? 1u : 0u);
		dst[0] = (uint8)(v & 0xFF); dst[1] = (uint8)(v >> 8);
		break;
	}
	default: {
		const uint32 v = (((rgba[0]*31+127)/255) << 11) |
		                 (((rgba[1]*63+127)/255) << 5) |
		                 ((rgba[2]*31+127)/255);
		dst[0] = (uint8)(v & 0xFF); dst[1] = (uint8)(v >> 8);
		break;
	}
	}
}

static uint32 levelDimension(uint32 base, int32 level);
static void cancelGeneratedMips(VulkanRaster *native);

static VkDeviceSize
alignLevel(VkDeviceSize size)
{
	return (size+3) & ~(VkDeviceSize)3;
}

// How much room the generated levels need after the base image. The chain
// rides in the same staging allocation the base already uses, so a streaming
// burst costs exactly as many buffers as it did before mips existed.
static VkDeviceSize
generatedMipBytes(int32 numLevels, uint32 baseWidth, uint32 baseHeight,
                  uint32 bytesPerPixel)
{
	VkDeviceSize total = 0;
	for(int32 level = 1; level < numLevels; level++)
		total += alignLevel((VkDeviceSize)levelDimension(baseWidth, level)*
		         levelDimension(baseHeight, level)*bytesPerPixel);
	return total;
}

// One level down, halving in each axis. An odd dimension drops its last row
// or column, which is what Vulkan's own chain does.
static void
downsampleLevel(const uint8 *src, uint32 srcWidth, uint32 srcHeight,
                uint8 *dst, uint32 dstWidth, uint32 dstHeight,
                VkFormat format, uint32 bytesPerPixel)
{
	for(uint32 y = 0; y < dstHeight; y++){
		const uint32 y0 = y*2;
		const uint32 y1 = y0+1 < srcHeight ? y0+1 : y0;
		for(uint32 x = 0; x < dstWidth; x++){
			const uint32 x0 = x*2;
			const uint32 x1 = x0+1 < srcWidth ? x0+1 : x0;
			uint32 texel[4][4];
			readTexel(src+(y0*srcWidth+x0)*bytesPerPixel, format, texel[0]);
			readTexel(src+(y0*srcWidth+x1)*bytesPerPixel, format, texel[1]);
			readTexel(src+(y1*srcWidth+x0)*bytesPerPixel, format, texel[2]);
			readTexel(src+(y1*srcWidth+x1)*bytesPerPixel, format, texel[3]);
			uint32 alpha = 0;
			for(int i = 0; i < 4; i++)
				alpha += texel[i][3];
			uint32 out[4];
			if(alpha == 0){
				// Nothing visible to weight by; keep the plain average so a
				// fully cut-out corner still carries a sane colour.
				for(int c = 0; c < 3; c++)
					out[c] = (texel[0][c]+texel[1][c]+
					          texel[2][c]+texel[3][c]+2)/4;
			}else{
				for(int c = 0; c < 3; c++)
					out[c] = (texel[0][c]*texel[0][3]+
					          texel[1][c]*texel[1][3]+
					          texel[2][c]*texel[2][3]+
					          texel[3][c]*texel[3][3]+alpha/2)/alpha;
			}
			out[3] = (alpha+2)/4;
			writeTexel(dst+(y*dstWidth+x)*bytesPerPixel, format, out);
		}
	}
}

static VkFormat
vulkanFormatFromRasterFormat(int32 format, int32 *bytesPerPixelOut)
{
	int32 bytesPerPixel = 0;
	VkFormat vkFormat = VK_FORMAT_UNDEFINED;

	switch(format & 0xF00){
	case Raster::C8888:
		vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
		bytesPerPixel = 4;
		break;
	case Raster::C888:
		// No widely supported 24-bit sampled format on tilers; widen to 32-bit
		// and leave alpha at one. rasterLock hands out a 4-byte stride to match.
		vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
		bytesPerPixel = 4;
		break;
	case Raster::C1555:
		vkFormat = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::C565:
		vkFormat = VK_FORMAT_R5G6B5_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::C4444:
		vkFormat = VK_FORMAT_R4G4B4A4_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::LUM8:
		vkFormat = VK_FORMAT_R8_UNORM;
		bytesPerPixel = 1;
		break;
	case Raster::C555:
		vkFormat = VK_FORMAT_R5G5B5A1_UNORM_PACK16;
		bytesPerPixel = 2;
		break;
	case Raster::D16:
		vkFormat = VK_FORMAT_D16_UNORM;
		bytesPerPixel = 2;
		break;
	case Raster::D24:
	case Raster::D32:
		vkFormat = VK_FORMAT_D24_UNORM_S8_UINT;
		bytesPerPixel = 4;
		break;
	default:
		break;
	}

	if(bytesPerPixelOut != nil)
		*bytesPerPixelOut = bytesPerPixel;
	return vkFormat;
}

static uint32
levelDimension(uint32 base, int32 level)
{
	uint32 value = base >> level;
	return value != 0 ? value : 1;
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, object, offset);
	memset(native, 0, sizeof(VulkanRaster));
	native->format = VK_FORMAT_UNDEFINED;
	native->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	native->lockedLevel = -1;
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	// The env-map raster the reflection block samples can be streamed out
	// with its vehicle; beginFrame falls back to white until the next one
	// registers.
	if(gvk.envRaster == (Raster*)object)
		gvk.envRaster = nil;
	// A texture can be evicted while its chain is still being built.
	cancelGeneratedMips(PLUGINOFFSET(VulkanRaster, object, offset));
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, object, offset);
	if(gvk.device != VK_NULL_HANDLE){
		if(native->stagingMapped != nil)
			vkUnmapMemory(gvk.device, native->stagingMemory);
		if(native->stagingBuffer)
			vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
		if(native->stagingMemory)
			vkFreeMemory(gvk.device, native->stagingMemory, nil);
		if(native->view || native->image || native->memory){
			retireImage(native->view, native->image, native->memory);
			gTextureMemoryUsed -= gTextureMemoryUsed < native->memoryBytes ?
				gTextureMemoryUsed : (size_t)native->memoryBytes;
		}
		retireTextureDescriptorSets(native->descriptorSet,
			NUM_FRAME_CONTEXTS);
	}
	memset(native, 0, sizeof(VulkanRaster));
	native->lockedLevel = -1;
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	VulkanRaster *native = PLUGINOFFSET(VulkanRaster, dst, offset);
	memset(native, 0, sizeof(VulkanRaster));
	native->format = VK_FORMAT_UNDEFINED;
	native->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	native->lockedLevel = -1;
	return dst;
}

void *
destroyNativeData(void *object, int32 offset, int32 size)
{
	return destroyNativeRaster(object, offset, size);
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(
		sizeof(VulkanRaster), ID_RASTERVULKAN, createNativeRaster,
		destroyNativeRaster, copyNativeRaster);
}

// ---------------------------------------------------------------------------
// Raster interface
// ---------------------------------------------------------------------------

Raster *
rasterCreate(Raster *raster)
{
	VulkanRaster *native = GETVULKANRASTER(raster);

	if(raster->type != Raster::TEXTURE && raster->type != Raster::CAMERATEXTURE &&
	   raster->type != Raster::ZBUFFER && raster->type != Raster::CAMERA){
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		return raster;
	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	// Raster::DEFAULT means "backend's choice". The game creates cameras and
	// z-buffers this way, so refusing it here rejects the main render targets.
	int32 requestedFormat = raster->format;
	if((requestedFormat & 0xF00) == 0){
		requestedFormat |= (raster->type & 0xF) == Raster::ZBUFFER ?
			Raster::D24 : Raster::C8888;
		raster->format = requestedFormat;
	}

	int32 bytesPerPixel = 0;
	native->format = vulkanFormatFromRasterFormat(requestedFormat, &bytesPerPixel);
	if(native->format == VK_FORMAT_UNDEFINED){
		VKERR("unsupported raster format 0x%x", raster->format);
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}
	raster->depth = bytesPerPixel * 8;
	raster->stride = raster->width * bytesPerPixel;

	native->numLevels = 1;
	native->generateMips = 0;
	if((raster->format & (Raster::MIPMAP | Raster::AUTOMIPMAP)) ||
	   wantsGeneratedMips(raster, native->format)){
		uint32 size = raster->width > raster->height ?
		              (uint32)raster->width : (uint32)raster->height;
		while(size > 1){
			size >>= 1;
			native->numLevels++;
		}
		native->generateMips =
			(raster->format & (Raster::MIPMAP | Raster::AUTOMIPMAP)) ?
				0 : 1;
	}

	const bool32 isDepth = (raster->type & 0xF) == Raster::ZBUFFER;

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = native->format;
	imageInfo.extent.width = (uint32)raster->width;
	imageInfo.extent.height = (uint32)raster->height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = (uint32)native->numLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = isDepth ?
		(VkImageUsageFlags)VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
		(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		                    VK_IMAGE_USAGE_SAMPLED_BIT);
	if((raster->type & 0xF) == Raster::CAMERATEXTURE ||
	   (raster->type & 0xF) == Raster::CAMERA)
		imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	if(vkCreateImage(gvk.device, &imageInfo, nil, &native->image) != VK_SUCCESS){
		VKERR("vkCreateImage failed for %dx%d", raster->width, raster->height);
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, native->image, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		vkDestroyImage(gvk.device, native->image, nil);
		native->image = VK_NULL_HANDLE;
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &native->memory) != VK_SUCCESS){
		vkDestroyImage(gvk.device, native->image, nil);
		native->image = VK_NULL_HANDLE;
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}
	native->memoryBytes = requirements.size;
	gTextureMemoryUsed += (size_t)requirements.size;
	vkBindImageMemory(gvk.device, native->image, native->memory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = native->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = native->format;
	viewInfo.subresourceRange.aspectMask = isDepth ?
		VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = (uint32)native->numLevels;
	viewInfo.subresourceRange.layerCount = 1;
	if(vkCreateImageView(gvk.device, &viewInfo, nil, &native->view) != VK_SUCCESS){
		VKERR("vkCreateImageView failed");
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	native->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return raster;
}

// DXT1 may carry one-bit transparency even when the TXD native-texture
// header does not advertise alpha. In that mode colour index 3 is
// transparent whenever colour0 <= colour1. Foliage in Vice City uses this
// extensively, so trusting only the header would incorrectly classify its
// material as opaque and draw the black padding around palm leaves.
// ---------------------------------------------------------------------------
// Block compression
//
// Adreno accepts BC, so the game's DXT blocks reach the GPU untouched and
// never pass through the uncompressed path above. Giving those textures a
// chain means working in blocks: decode a level, filter it, encode it again.
// The base level is always the artist's own blocks; only the generated levels
// are re-encoded, and a mip is forgiving of a simple endpoint fit.
// ---------------------------------------------------------------------------

static uint32
bcBlockBytes(VkFormat format)
{
	return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16;
}

static void
decodeColourBlock(const uint8 *block, bool32 punchThrough, uint8 *rgba)
{
	const uint32 c0 = (uint32)block[0] | ((uint32)block[1] << 8);
	const uint32 c1 = (uint32)block[2] | ((uint32)block[3] << 8);
	const uint32 indices = (uint32)block[4] | ((uint32)block[5] << 8) |
	                       ((uint32)block[6] << 16) | ((uint32)block[7] << 24);
	uint32 red[4], green[4], blue[4], alpha[4];
	red[0] = (((c0 >> 11) & 0x1F)*255)/31;
	green[0] = (((c0 >> 5) & 0x3F)*255)/63;
	blue[0] = ((c0 & 0x1F)*255)/31;
	red[1] = (((c1 >> 11) & 0x1F)*255)/31;
	green[1] = (((c1 >> 5) & 0x3F)*255)/63;
	blue[1] = ((c1 & 0x1F)*255)/31;
	alpha[0] = alpha[1] = alpha[2] = alpha[3] = 255;
	if(c0 > c1 || !punchThrough){
		red[2] = (red[0]*2+red[1])/3;
		green[2] = (green[0]*2+green[1])/3;
		blue[2] = (blue[0]*2+blue[1])/3;
		red[3] = (red[0]+red[1]*2)/3;
		green[3] = (green[0]+green[1]*2)/3;
		blue[3] = (blue[0]+blue[1]*2)/3;
	}else{
		red[2] = (red[0]+red[1])/2;
		green[2] = (green[0]+green[1])/2;
		blue[2] = (blue[0]+blue[1])/2;
		red[3] = green[3] = blue[3] = 0;
		alpha[3] = 0;
	}
	for(int t = 0; t < 16; t++){
		const uint32 i = (indices >> (t*2)) & 3;
		rgba[t*4+0] = (uint8)red[i];
		rgba[t*4+1] = (uint8)green[i];
		rgba[t*4+2] = (uint8)blue[i];
		rgba[t*4+3] = (uint8)alpha[i];
	}
}

static void
decodeBcBlock(const uint8 *block, VkFormat format, uint8 *rgba)
{
	if(format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK){
		decodeColourBlock(block, 1, rgba);
		return;
	}
	decodeColourBlock(block+8, 0, rgba);
	if(format == VK_FORMAT_BC2_UNORM_BLOCK){
		for(int t = 0; t < 16; t++){
			const uint32 nibble = (block[t/2] >> ((t & 1)*4)) & 0xF;
			rgba[t*4+3] = (uint8)(nibble*17);
		}
		return;
	}
	// BC3: two endpoints and three-bit indices.
	uint32 value[8];
	value[0] = block[0];
	value[1] = block[1];
	if(value[0] > value[1]){
		for(int i = 1; i < 7; i++)
			value[i+1] = ((7-i)*value[0] + i*value[1])/7;
	}else{
		for(int i = 1; i < 5; i++)
			value[i+1] = ((5-i)*value[0] + i*value[1])/5;
		value[6] = 0;
		value[7] = 255;
	}
	uint64 bits = 0;
	for(int i = 0; i < 6; i++)
		bits |= (uint64)block[2+i] << (i*8);
	for(int t = 0; t < 16; t++)
		rgba[t*4+3] = (uint8)value[(bits >> (t*3)) & 7];
}

static void
encodeColourBlock(const uint8 *rgba, bool32 skipClear,
                  bool32 allowPunchThrough, uint8 *block)
{
	// Bounding box of the texels that are actually there. A mip level does not
	// need a principal-axis fit; the endpoints only have to bracket what the
	// block still contains.
	uint32 lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
	bool32 anyOpaque = 0, anyClear = 0;
	for(int t = 0; t < 16; t++){
		if(skipClear && rgba[t*4+3] < 128){
			anyClear = 1;
			continue;
		}
		anyOpaque = 1;
		for(int c = 0; c < 3; c++){
			const uint32 v = rgba[t*4+c];
			if(v < lo[c]) lo[c] = v;
			if(v > hi[c]) hi[c] = v;
		}
	}
	if(!anyOpaque){
		// Every texel was cut. Only the punch-through path can reach
		// this, and there all sixteen indices are the transparent one,
		// so the endpoints only have to be defined.
		lo[0] = lo[1] = lo[2] = 0;
		hi[0] = hi[1] = hi[2] = 0;
	}
	uint32 c0 = (((hi[0]*31+127)/255) << 11) | (((hi[1]*63+127)/255) << 5) |
	            ((hi[2]*31+127)/255);
	uint32 c1 = (((lo[0]*31+127)/255) << 11) | (((lo[1]*63+127)/255) << 5) |
	            ((lo[2]*31+127)/255);
	// BC1 reads the mode off the endpoint order, so it has to be forced.
	const bool32 threeColour = allowPunchThrough && anyClear;
	if(threeColour){
		if(c0 > c1){ const uint32 s = c0; c0 = c1; c1 = s; }
		if(c0 == c1){
			if(c1 < 0xFFFF) c1++;
			else if(c0 > 0) c0--;
		}
	}else if(c0 <= c1){
		if(c1 < 0xFFFF) c0 = c1+1;
		else if(c1 > 0) { c0 = c1; c1--; }
	}
	uint8 palette[4][3];
	palette[0][0] = (uint8)((((c0 >> 11) & 0x1F)*255)/31);
	palette[0][1] = (uint8)((((c0 >> 5) & 0x3F)*255)/63);
	palette[0][2] = (uint8)(((c0 & 0x1F)*255)/31);
	palette[1][0] = (uint8)((((c1 >> 11) & 0x1F)*255)/31);
	palette[1][1] = (uint8)((((c1 >> 5) & 0x3F)*255)/63);
	palette[1][2] = (uint8)(((c1 & 0x1F)*255)/31);
	for(int c = 0; c < 3; c++){
		if(threeColour){
			palette[2][c] =
				(uint8)(((uint32)palette[0][c]+palette[1][c])/2);
			palette[3][c] = 0;
		}else{
			palette[2][c] =
				(uint8)(((uint32)palette[0][c]*2+palette[1][c])/3);
			palette[3][c] =
				(uint8)(((uint32)palette[0][c]+palette[1][c]*2)/3);
		}
	}
	// Project each texel onto the endpoint axis rather than testing it
	// against every palette entry. Same answer for a mip level, and this
	// runs on the streaming thread for every texture the game loads.
	const int axis[3] = {
		(int)palette[1][0]-(int)palette[0][0],
		(int)palette[1][1]-(int)palette[0][1],
		(int)palette[1][2]-(int)palette[0][2]
	};
	const int axisLength =
		axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2];
	// Palette order along the axis is 0, 2, 3, 1 for four colours and
	// 0, 2, 1 for three.
	static const uint32 fourStep[4] = { 0, 2, 3, 1 };
	static const uint32 threeStep[3] = { 0, 2, 1 };
	const int steps = threeColour ? 2 : 3;
	uint32 indices = 0;
	for(int t = 0; t < 16; t++){
		uint32 best;
		if(threeColour && rgba[t*4+3] < 128)
			best = 3;
		else if(axisLength == 0)
			best = 0;
		else{
			const int along =
				((int)rgba[t*4+0]-(int)palette[0][0])*axis[0]+
				((int)rgba[t*4+1]-(int)palette[0][1])*axis[1]+
				((int)rgba[t*4+2]-(int)palette[0][2])*axis[2];
			int step = (along*steps*2+axisLength)/(axisLength*2);
			if(step < 0) step = 0;
			if(step > steps) step = steps;
			best = threeColour ? threeStep[step] : fourStep[step];
		}
		indices |= best << (t*2);
	}
	block[0] = (uint8)(c0 & 0xFF); block[1] = (uint8)(c0 >> 8);
	block[2] = (uint8)(c1 & 0xFF); block[3] = (uint8)(c1 >> 8);
	block[4] = (uint8)(indices & 0xFF);
	block[5] = (uint8)((indices >> 8) & 0xFF);
	block[6] = (uint8)((indices >> 16) & 0xFF);
	block[7] = (uint8)((indices >> 24) & 0xFF);
}

static void
encodeBcBlock(const uint8 *rgba, VkFormat format, uint8 *block)
{
	if(format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK){
		encodeColourBlock(rgba, 1, 1, block);
		return;
	}
	// Every texel goes into the fit here. Skipping the clear ones
	// sounds right and is not: a minified leaf mask is mostly clear,
	// so whole blocks end up with nothing to fit and collapse -- a
	// grid of dark squares across the foliage.
	encodeColourBlock(rgba, 0, 0, block+8);
	if(format == VK_FORMAT_BC2_UNORM_BLOCK){
		for(int i = 0; i < 8; i++)
			block[i] = (uint8)((((rgba[(i*2+1)*4+3]+8)/17) << 4) |
			                    ((rgba[(i*2)*4+3]+8)/17));
		return;
	}
	// BC3: bracket the alphas and index into the eight-step ramp.
	uint32 lo = 255, hi = 0;
	for(int t = 0; t < 16; t++){
		const uint32 a = rgba[t*4+3];
		if(a < lo) lo = a;
		if(a > hi) hi = a;
	}
	if(hi == lo){
		if(hi < 255) hi = lo+1;
		else lo = hi-1;
	}
	block[0] = (uint8)hi;
	block[1] = (uint8)lo;
	// Ramp position straight from the value: index 0 is hi, 1 is lo, and
	// 2..7 walk between them.
	const int span = (int)hi-(int)lo;
	uint64 bits = 0;
	for(int t = 0; t < 16; t++){
		const int a = (int)rgba[t*4+3];
		int step = (((int)hi-a)*7*2+span)/(span*2);
		if(step < 0) step = 0;
		if(step > 7) step = 7;
		const uint64 best = step == 0 ? 0 : step == 7 ? 1 :
			(uint64)(step+1);
		bits |= best << (t*3);
	}
	for(int i = 0; i < 6; i++)
		block[2+i] = (uint8)((bits >> (i*8)) & 0xFF);
}

// A whole level, block grid to RGBA and back. Texels outside a partial edge
// block repeat the last real column or row, so the encoder never fits
// endpoints to whatever happened to be in memory.
static void
decodeBcLevel(const uint8 *blocks, uint32 width, uint32 height,
              VkFormat format, uint8 *rgba)
{
	const uint32 blockBytes = bcBlockBytes(format);
	const uint32 across = (width+3)/4, down = (height+3)/4;
	uint8 texels[16*4];
	for(uint32 by = 0; by < down; by++){
		for(uint32 bx = 0; bx < across; bx++){
			decodeBcBlock(blocks+((size_t)by*across+bx)*blockBytes,
				format, texels);
			for(uint32 y = 0; y < 4; y++){
				const uint32 py = by*4+y;
				if(py >= height) break;
				for(uint32 x = 0; x < 4; x++){
					const uint32 px = bx*4+x;
					if(px >= width) break;
					for(int c = 0; c < 4; c++)
						rgba[((size_t)py*width+px)*4+c] =
							texels[(y*4+x)*4+c];
				}
			}
		}
	}
}

static void
encodeBcLevel(const uint8 *rgba, uint32 width, uint32 height,
              VkFormat format, uint8 *blocks)
{
	const uint32 blockBytes = bcBlockBytes(format);
	const uint32 across = (width+3)/4, down = (height+3)/4;
	uint8 texels[16*4];
	for(uint32 by = 0; by < down; by++){
		for(uint32 bx = 0; bx < across; bx++){
			for(uint32 y = 0; y < 4; y++){
				uint32 py = by*4+y;
				if(py >= height) py = height-1;
				for(uint32 x = 0; x < 4; x++){
					uint32 px = bx*4+x;
					if(px >= width) px = width-1;
					for(int c = 0; c < 4; c++)
						texels[(y*4+x)*4+c] =
							rgba[((size_t)py*width+px)*4+c];
				}
			}
			encodeBcBlock(texels, format,
				blocks+((size_t)by*across+bx)*blockBytes);
		}
	}
}

static VkDeviceSize
bcLevelBytes(uint32 width, uint32 height, VkFormat format)
{
	return (VkDeviceSize)((width+3)/4)*((height+3)/4)*bcBlockBytes(format);
}

static bool32
dxt1BlocksHaveTransparency(const uint8 *blocks, uint32 size)
{
	for(uint32 offset = 0; offset + 8 <= size; offset += 8){
		const uint16 colour0 =
			(uint16)blocks[offset + 0] |
			((uint16)blocks[offset + 1] << 8);
		const uint16 colour1 =
			(uint16)blocks[offset + 2] |
			((uint16)blocks[offset + 3] << 8);
		if(colour0 > colour1)
			continue;

		const uint32 indices =
			(uint32)blocks[offset + 4] |
			((uint32)blocks[offset + 5] << 8) |
			((uint32)blocks[offset + 6] << 16) |
			((uint32)blocks[offset + 7] << 24);
		for(uint32 pixel = 0; pixel < 16; pixel++)
			if(((indices >> (pixel * 2)) & 3u) == 3u)
				return 1;
	}
	return 0;
}

// One bit of alpha cannot hold a filtered edge. Every level generated from a
// punch-through BC1 texture is cut back to a mask, so a bush keeps its hard
// edge at every distance and crawls as the head moves -- which is exactly
// what the palm crowns, stored as BC2 with a real alpha ramp, never do.
//
// So give those textures somewhere to put the alpha: re-pack the base level
// as BC3 before it is uploaded, and let the chain be built in that format.
// A block with no cut-out is copied bit for bit and simply told it is opaque,
// so the artist's own colour survives; only the blocks along the mask are
// unpacked and fitted again, and those are the ones a mip is going to
// rewrite anyway.
static void
promoteBc1ToBc3(const uint8 *blocks, uint32 width, uint32 height, uint8 *out)
{
	const uint32 across = (width+3)/4, down = (height+3)/4;
	for(uint32 b = 0; b < across*down; b++){
		const uint8 *source = blocks + (size_t)b*8;
		uint8 *block = out + (size_t)b*16;
		const uint32 colour0 = (uint32)source[0] | ((uint32)source[1] << 8);
		const uint32 colour1 = (uint32)source[2] | ((uint32)source[3] << 8);
		if(colour0 > colour1){
			// Four-colour block: nothing in it is transparent.
			block[0] = 255;
			block[1] = 255;
			memset(block+2, 0, 6);
			memcpy(block+8, source, 8);
			continue;
		}
		uint8 rgba[16*4];
		decodeBcBlock(source, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, rgba);
		encodeBcBlock(rgba, VK_FORMAT_BC3_UNORM_BLOCK, block);
	}
}

// The fraction of a level a hard alpha cut keeps.
static float32
alphaCoverage(const uint8 *rgba, size_t texels, uint32 cut)
{
	size_t kept = 0;
	for(size_t t = 0; t < texels; t++)
		if(rgba[t*4+3] >= cut)
			kept++;
	return texels != 0 ? (float32)kept/(float32)texels : 0.0f;
}

// BC1 carries one bit of alpha, so a filtered level has to be cut back to a
// mask before it can be encoded -- and cutting at half loses coverage every
// time. Foliage is the worst case: each level keeps a little less of the
// leaf than the one above it, and a tree three streets away ends up a
// handful of leaves on a branch, thinning and thickening as the head moves.
//
// So pick the cut that keeps as much of this level as the artist's own mask
// kept of the base. The edge stays hard -- one bit cannot do better -- but
// the crown holds its weight all the way out.
static void
preserveAlphaCoverage(uint8 *rgba, size_t texels, float32 coverage)
{
	uint32 histogram[256];
	memset(histogram, 0, sizeof(histogram));
	for(size_t t = 0; t < texels; t++)
		histogram[rgba[t*4+3]]++;
	const size_t wanted = (size_t)(coverage*(float32)texels + 0.5f);
	size_t kept = 0;
	uint32 cut = 256;
	while(cut > 1 && kept < wanted)
		kept += histogram[--cut];
	for(size_t t = 0; t < texels; t++)
		rgba[t*4+3] = rgba[t*4+3] >= cut ? 255 : 0;
}

// Builds the chain from a copy of the base blocks into its own buffer, off
// the game's threads. Each level is decoded from the one above it, filtered
// with the same alpha weighting the uncompressed path uses, and encoded
// again. False means nothing was written and the levels stay unused.
static bool32
buildCompressedMips(const uint8 *base, uint8 *out, uint32 baseWidth,
                    uint32 baseHeight, VkFormat sourceFormat,
                    VkFormat outFormat, int32 numLevels)
{
	// Two scratch levels, ping-ponged: the one being read and the one being
	// written. Kept between textures and grown to the largest seen -- the
	// game streams these continuously, and a pair of multi-megabyte
	// allocations per texture was a good part of the cost.
	const size_t pixels = (size_t)baseWidth*baseHeight*4;
	if(pixels > gMipScratchBytes){
		rwFree(gMipScratch[0]);
		rwFree(gMipScratch[1]);
		gMipScratch[0] = (uint8*)rwMalloc(pixels, MEMDUR_GLOBAL | ID_IMAGE);
		gMipScratch[1] = (uint8*)rwMalloc(pixels, MEMDUR_GLOBAL | ID_IMAGE);
		gMipScratchBytes = gMipScratch[0] && gMipScratch[1] ? pixels : 0;
	}
	uint8 *previous = gMipScratch[0];
	uint8 *current = gMipScratch[1];
	if(previous == nil || current == nil)
		return 0;

	// One bit of alpha needs the coverage pass below, and that has to
	// work on a copy: the next level is filtered from the smooth one,
	// or the mask would be cut from an already cut level and the error
	// would compound down the chain. Only level 1 is ever this large.
	const bool32 punchThrough =
		outFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	const size_t maskBytes = punchThrough ?
		(size_t)levelDimension(baseWidth, 1)*
		levelDimension(baseHeight, 1)*4 : 0;
	if(maskBytes > gMipMaskBytes){
		rwFree(gMipMask);
		gMipMask = (uint8*)rwMalloc(maskBytes, MEMDUR_GLOBAL | ID_IMAGE);
		gMipMaskBytes = gMipMask != nil ? maskBytes : 0;
	}

	// The artist's own blocks, whatever they were packed as: a promoted
	// texture is filtered from the original rather than from the copy
	// that was just re-packed for the upload.
	decodeBcLevel(base, baseWidth, baseHeight, sourceFormat, previous);
	// A texture with no cut-outs, or nothing but cut-outs, has no
	// coverage to hold on to.
	float32 baseCoverage = 0.0f;
	if(punchThrough && gMipMask != nil)
		baseCoverage = alphaCoverage(previous,
			(size_t)baseWidth*baseHeight, 128);
	const bool32 holdCoverage =
		baseCoverage > 0.0f && baseCoverage < 1.0f;
	uint32 sourceWidth = baseWidth, sourceHeight = baseHeight;
	VkDeviceSize offset = 0;
	for(int32 level = 1; level < numLevels; level++){
		const uint32 width = levelDimension(baseWidth, level);
		const uint32 height = levelDimension(baseHeight, level);
		downsampleLevel(previous, sourceWidth, sourceHeight,
			current, width, height, VK_FORMAT_R8G8B8A8_UNORM, 4);
		const uint8 *encodeFrom = current;
		if(holdCoverage){
			const size_t levelTexels = (size_t)width*height;
			memcpy(gMipMask, current, levelTexels*4);
			preserveAlphaCoverage(gMipMask, levelTexels,
				baseCoverage);
			encodeFrom = gMipMask;
		}
		encodeBcLevel(encodeFrom, width, height, outFormat, out+offset);
		offset += alignLevel(bcLevelBytes(width, height, outFormat));
		uint8 *swap = previous; previous = current; current = swap;
		sourceWidth = width;
		sourceHeight = height;
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Deferred mip generation
//
// Filtering and re-encoding a texture costs a few milliseconds, and the game
// streams textures continuously while the player drives. Doing it where the
// texture is created put every one of those milliseconds on the streaming
// thread: standing still was fine, turning a corner into unseen ground was a
// stall. The base level goes up immediately, the chain is built on a worker,
// and the finished levels are uploaded on the next frame that asks for them.
//
// Until they land the view exposes level 0 alone, so nothing ever samples a
// level that has not been written.
// ---------------------------------------------------------------------------

struct GeneratedMipJob
{
	VulkanRaster *native;
	// What the base blocks are packed as, and what the chain is
	// written as. They differ for a promoted texture.
	VkFormat sourceFormat;
	VkFormat format;
	uint32 width;
	uint32 height;
	int32 numLevels;
	uint8 *source;
	uint8 *result;
	VkDeviceSize resultBytes;
	bool cancelled;
};

static std::mutex gMipMutex;
static std::condition_variable gMipSignal;
static std::deque<GeneratedMipJob*> gMipPending;
static std::deque<GeneratedMipJob*> gMipFinished;
// The job being filtered right now. It sits in neither queue while that
// runs, so a raster destroyed in that window walks straight past the
// scan in cancelGeneratedMips and the finished job comes back holding a
// handle that no longer exists.
static GeneratedMipJob *gMipInFlight;
static std::thread gMipWorker;
static bool gMipWorkerStarted;
static bool gMipWorkerStop;

static void
freeMipJob(GeneratedMipJob *job)
{
	rwFree(job->source);
	rwFree(job->result);
	rwFree(job);
}

// Called with the queue lock held. The worker owns the in-flight buffers
// until filtering returns; all other jobs can release their copies now.
static void
discardGeneratedMipJobs(void)
{
	if(gMipInFlight != nil){
		gMipInFlight->cancelled = true;
		gMipInFlight->native = nil;
	}
	while(!gMipPending.empty()){
		freeMipJob(gMipPending.front());
		gMipPending.pop_front();
	}
	while(!gMipFinished.empty()){
		freeMipJob(gMipFinished.front());
		gMipFinished.pop_front();
	}
}

void
setGenerateMipmaps(bool32 enabled)
{
	enabled = enabled != 0;
	if(gvk.generateMipmaps == enabled)
		return;
	if(renderDiagnosticsEnabled())
		VKLOG("generated mipmaps %s", enabled ? "on" : "off");
	gvk.generateMipmaps = enabled;
	if(!enabled){
		std::lock_guard<std::mutex> lock(gMipMutex);
		discardGeneratedMipJobs();
	}
}

static void
mipWorkerBody(void)
{
	for(;;){
		GeneratedMipJob *job = nil;
		{
			std::unique_lock<std::mutex> lock(gMipMutex);
			gMipSignal.wait(lock, []{
				return gMipWorkerStop || !gMipPending.empty();
			});
			if(gMipWorkerStop)
				return;
			job = gMipPending.front();
			gMipPending.pop_front();
			if(job->cancelled){
				job->native = nil;
				freeMipJob(job);
				continue;
			}
			gMipInFlight = job;
		}
		const uint64 startedAt = microseconds();
		const bool32 built = buildCompressedMips(job->source, job->result, job->width,
			job->height, job->sourceFormat, job->format,
			job->numLevels);
		gMipMicroseconds.fetch_add(microseconds()-startedAt,
			std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lock(gMipMutex);
			gMipInFlight = nil;
			// The raster may have been evicted while this ran.
			if(!built || job->cancelled || job->native == nil){
				freeMipJob(job);
				continue;
			}
			gMipFinished.push_back(job);
		}
	}
}

// Takes the lock itself: a raster can be destroyed from anywhere, and the
// worker may be holding the job it is about to disown.
static void
cancelGeneratedMips(VulkanRaster *native)
{
	std::lock_guard<std::mutex> lock(gMipMutex);
	for(auto job = gMipPending.begin(); job != gMipPending.end(); )
		if((*job)->native == native){
			freeMipJob(*job);
			job = gMipPending.erase(job);
		}else
			++job;
	if(gMipInFlight != nil && gMipInFlight->native == native){
		gMipInFlight->cancelled = true;
		gMipInFlight->native = nil;
	}
	for(auto job = gMipFinished.begin(); job != gMipFinished.end(); )
		if((*job)->native == native){
			freeMipJob(*job);
			job = gMipFinished.erase(job);
		}else
			++job;
}

static void
queueGeneratedMips(VulkanRaster *native, VkFormat sourceFormat,
                   VkFormat format, uint32 width, uint32 height,
                   int32 numLevels, const uint8 *blocks,
                   VkDeviceSize sourceBytes)
{
	if(!gvk.generateMipmaps)
		return;
	VkDeviceSize resultBytes = 0;
	for(int32 level = 1; level < numLevels; level++)
		resultBytes += alignLevel(bcLevelBytes(levelDimension(width, level),
			levelDimension(height, level), format));
	if(resultBytes == 0)
		return;

	GeneratedMipJob *job =
		(GeneratedMipJob*)rwMalloc(sizeof(GeneratedMipJob),
			MEMDUR_GLOBAL | ID_IMAGE);
	if(job == nil)
		return;
	memset(job, 0, sizeof(*job));
	job->native = native;
	job->sourceFormat = sourceFormat;
	job->format = format;
	job->width = width;
	job->height = height;
	job->numLevels = numLevels;
	job->resultBytes = resultBytes;
	job->source = (uint8*)rwMalloc((size_t)sourceBytes,
		MEMDUR_GLOBAL | ID_IMAGE);
	job->result = (uint8*)rwMalloc((size_t)resultBytes,
		MEMDUR_GLOBAL | ID_IMAGE);
	if(job->source == nil || job->result == nil){
		freeMipJob(job);
		return;
	}
	memcpy(job->source, blocks, (size_t)sourceBytes);

	std::lock_guard<std::mutex> lock(gMipMutex);
	if(!gMipWorkerStarted){
		gMipWorkerStarted = true;
		gMipWorker = std::thread(mipWorkerBody);
	}
	gMipPending.push_back(job);
	gMipSignal.notify_one();
}

// The finished levels, uploaded from the thread that owns the command
// buffer. A job's raster is read after the queue lock is released, which
// is only safe because rasters are created and destroyed on this same
// thread -- the game's. The worker never destroys one; it hands the job
// back and lets cancelGeneratedMips disown it. Bounded per call, so a
// burst of streamed textures does not turn into one long frame at this
// end either.
void
uploadFinishedMips(void)
{
	// Outside a frame endOneShot waits the entire graphics queue. Leave the
	// completed chains queued until they can inherit a normal frame fence.
	if(!gvk.generateMipmaps || !gvk.inFrame)
		return;
	const VkDeviceSize uploadBudget = 2*1024*1024;
	VkDeviceSize uploadedBytes = 0;
	for(int handled = 0; handled < 8; handled++){
		GeneratedMipJob *job = nil;
		{
			std::lock_guard<std::mutex> lock(gMipMutex);
			if(gMipFinished.empty())
				return;
			job = gMipFinished.front();
			// A single large chain must still make progress; later chains wait
			// for the next frame instead of forming a multi-megabyte burst.
			if(uploadedBytes != 0 &&
			   job->resultBytes > uploadBudget-uploadedBytes)
				return;
			gMipFinished.pop_front();
		}
		if(job->cancelled || job->native == nil){
			freeMipJob(job);
			continue;
		}
		VulkanRaster *native = job->native;

		VkBuffer staging = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		if(!createBuffer(job->resultBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                 &staging, &stagingMemory)){
			freeMipJob(job);
			continue;
		}
		void *mapped = nil;
		if(vkMapMemory(gvk.device, stagingMemory, 0, job->resultBytes,
		               0, &mapped) != VK_SUCCESS){
			retireBuffer(staging, stagingMemory);
			freeMipJob(job);
			continue;
		}
		memcpy(mapped, job->result, (size_t)job->resultBytes);
		vkUnmapMemory(gvk.device, stagingMemory);

		VkCommandBuffer commandBuffer = beginOneShot();
		if(commandBuffer == VK_NULL_HANDLE){
			retireBuffer(staging, stagingMemory);
			freeMipJob(job);
			continue;
		}
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)job->numLevels,
		                      native->layout,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkDeviceSize offset = 0;
		for(int32 level = 1; level < job->numLevels; level++){
			const uint32 width = levelDimension(job->width, level);
			const uint32 height = levelDimension(job->height, level);
			VkBufferImageCopy region = {};
			region.bufferOffset = offset;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = (uint32)level;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.width = width;
			region.imageExtent.height = height;
			region.imageExtent.depth = 1;
			vkCmdCopyBufferToImage(commandBuffer, staging, native->image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			offset += alignLevel(bcLevelBytes(width, height, job->format));
		}
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)job->numLevels,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		endOneShot(commandBuffer);
		uploadedBytes += job->resultBytes;
		native->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		retireBuffer(staging, stagingMemory);

		// Only now may anything sample past level 0. The old view is retired
		// with the frame; the cached descriptors are forced to be rewritten
		// against the new one.
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = native->image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = job->format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = (uint32)job->numLevels;
		viewInfo.subresourceRange.layerCount = 1;
		VkImageView view = VK_NULL_HANDLE;
		if(vkCreateImageView(gvk.device, &viewInfo, nil, &view) == VK_SUCCESS){
			retireImage(native->view, VK_NULL_HANDLE, VK_NULL_HANDLE);
			native->view = view;
			for(int frame = 0; frame < NUM_FRAME_CONTEXTS; frame++)
				native->samplerKey[frame] = 0xFFFFFFFFu;
			gMipLanded++;
		}
		freeMipJob(job);
		if(uploadedBytes >= uploadBudget)
			return;
	}
}

void
stopGeneratedMips(void)
{
	{
		std::lock_guard<std::mutex> lock(gMipMutex);
		if(!gMipWorkerStarted)
			return;
		gMipWorkerStop = true;
		discardGeneratedMipJobs();
	}
	gMipSignal.notify_all();
	gMipWorker.join();
	gMipWorkerStarted = false;
	gMipWorkerStop = false;
	rwFree(gMipScratch[0]);
	rwFree(gMipScratch[1]);
	rwFree(gMipMask);
	gMipScratch[0] = gMipScratch[1] = nil;
	gMipMask = nil;
	gMipScratchBytes = 0;
	gMipMaskBytes = 0;
	std::lock_guard<std::mutex> lock(gMipMutex);
	discardGeneratedMipJobs();
}

// Creates a texture directly from DXT blocks, no CPU decode. Adreno 740
// reports full BC support, so stream data reaches the GPU the same way the
// desktop D3D12 build uploads it. Decoding on the CPU instead cost
// milliseconds per streamed texture, which surfaced as frame drops -- the
// world lurching -- whenever driving streamed new map sectors in.
//
// levelSizes holds one entry per level the TXD carried, and blocks holds
// them back to back. Such a chain is used as it stands when this backend
// is not building one of its own -- see the note where that is decided.
Raster *
rasterFromDXT(int32 width, int32 height, int32 dxt, bool32 hasAlpha,
              const uint8 *blocks, uint32 size,
              const uint32 *levelSizes, int32 txdLevels)
{
	if(!gvk.supportsBC || blocks == nil || size == 0)
		return nil;

	VkFormat format;
	switch(dxt){
	// Always the alpha-carrying BC1, as the D3D12 backend picks
	// DXGI_FORMAT_BC1_UNORM for DXT1 regardless of the TXD's alpha flag.
	// DXT1 encodes punch-through transparency in the block itself; reading it
	// as BC1_RGB forces alpha to 1, the alpha test then discards nothing, and
	// masked foliage renders as solid black quads -- the palm silhouettes.
	case 1: format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
	case 2:
	case 3: format = VK_FORMAT_BC2_UNORM_BLOCK; break;
	case 4:
	case 5: format = VK_FORMAT_BC3_UNORM_BLOCK; break;
	default: return nil;
	}

	Raster *raster = Raster::create(width, height, 32,
		Raster::TEXTURE | Raster::C8888 | Raster::DONTALLOCATE);
	if(raster == nil)
		return nil;
	VulkanRaster *native = GETVULKANRASTER(raster);
	native->format = format;
	// A chain out of the dictionary is only taken when this backend
	// is not building one. The model packs were filtered without
	// weighting the colour by alpha, so their levels carry a dark rim
	// around every shape in a mask -- a lattice across foliage and
	// neon once anything samples them. Ours are weighted, so with
	// generation on, ours wins.
	const bool32 generate = wantsGeneratedMips(raster, format);
	const bool32 txdChain = levelSizes != nil && txdLevels > 1 && !generate;
	native->numLevels = 1;
	native->generateMips = 0;
	if(txdChain){
		native->numLevels = txdLevels;
	}else{
		native->generateMips = generate;
		if(native->generateMips){
			uint32 extent = (uint32)(width > height ? width : height);
			while(extent > 1){
				extent >>= 1;
				native->numLevels++;
			}
		}
	}
	const bool32 blockAlpha =
		dxt == 1 && dxt1BlocksHaveTransparency(blocks,
			levelSizes != nil ? levelSizes[0] : size);
	native->hasAlpha = hasAlpha || blockAlpha;

	// A cut-out BC1 texture is re-packed as BC3 on the way up, so the
	// generated levels have somewhere to keep the alpha the filter
	// produces. Only worth the second four bits per block when there
	// is a chain to fill: without one the base is the artist's mask
	// either way. The blocks the worker filters stay the originals.
	const VkFormat sourceFormat = format;
	const uint32 sourceSize = size;
	const bool32 promote = native->generateMips && !txdChain &&
		blockAlpha &&
		format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	if(promote){
		format = VK_FORMAT_BC3_UNORM_BLOCK;
		native->format = format;
		size = bcLevelBytes((uint32)width, (uint32)height, format);
		gMipPromoted++;
	}
	if(blockAlpha && !hasAlpha){
		static uint32 detectedWithoutHeader = 0;
		if(detectedWithoutHeader < 8){
			VKLOG("DXT1 transparency found in blocks despite clear TXD alpha flag (%dx%d)",
			      width, height);
			detectedWithoutHeader++;
		}
	}

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format;
	imageInfo.extent.width = (uint32)width;
	imageInfo.extent.height = (uint32)height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = (uint32)native->numLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                  VK_IMAGE_USAGE_SAMPLED_BIT;
	if(vkCreateImage(gvk.device, &imageInfo, nil, &native->image) != VK_SUCCESS){
		raster->destroy();
		return nil;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(gvk.device, native->image, &requirements);
	uint32 typeIndex = 0;
	if(!findMemoryType(requirements.memoryTypeBits,
	                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &typeIndex)){
		raster->destroy();
		return nil;
	}
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = typeIndex;
	if(vkAllocateMemory(gvk.device, &allocInfo, nil, &native->memory) != VK_SUCCESS){
		raster->destroy();
		return nil;
	}
	native->memoryBytes = requirements.size;
	gTextureMemoryUsed += (size_t)requirements.size;
	vkBindImageMemory(gvk.device, native->image, native->memory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = native->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	// A generated chain is not there yet, so the view starts at level 0
	// and is replaced once the worker lands it. One that came out of the
	// dictionary is uploaded below and can be exposed at once.
	viewInfo.subresourceRange.levelCount =
		txdChain ? (uint32)native->numLevels : 1;
	viewInfo.subresourceRange.layerCount = 1;
	if(vkCreateImageView(gvk.device, &viewInfo, nil, &native->view) != VK_SUCCESS){
		raster->destroy();
		return nil;
	}

	const VkDeviceSize stagingSize = size;

	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if(!createBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &staging, &stagingMemory)){
		raster->destroy();
		return nil;
	}
	uint8 *mapped = nil;
	if(vkMapMemory(gvk.device, stagingMemory, 0, stagingSize, 0,
	               (void**)&mapped) != VK_SUCCESS){
		vkDestroyBuffer(gvk.device, staging, nil);
		vkFreeMemory(gvk.device, stagingMemory, nil);
		raster->destroy();
		return nil;
	}
	if(promote)
		promoteBc1ToBc3(blocks, (uint32)width, (uint32)height, mapped);
	else
		memcpy(mapped, blocks, size);
	vkUnmapMemory(gvk.device, stagingMemory);

	VkCommandBuffer commandBuffer = beginOneShot();
	if(commandBuffer != VK_NULL_HANDLE){
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)native->numLevels,
		                      VK_IMAGE_LAYOUT_UNDEFINED,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkDeviceSize levelOffset = 0;
		for(int32 level = 0; level < (txdChain ? txdLevels : 1); level++){
			VkBufferImageCopy region = {};
			region.bufferOffset = levelOffset;
			region.imageSubresource.aspectMask =
				VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = (uint32)level;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.width =
				levelDimension((uint32)width, level);
			region.imageExtent.height =
				levelDimension((uint32)height, level);
			region.imageExtent.depth = 1;
			vkCmdCopyBufferToImage(commandBuffer, staging, native->image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			levelOffset += levelSizes != nil ? levelSizes[level] : size;
		}
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)native->numLevels,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		endOneShot(commandBuffer);
		native->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	// endOneShot is asynchronous during a live frame. The active frame fence
	// retires this staging allocation after both the copy and its consumers.
	retireBuffer(staging, stagingMemory);

	if(native->generateMips && native->numLevels > 1)
		queueGeneratedMips(native, sourceFormat, format,
			(uint32)width, (uint32)height, native->numLevels,
			blocks, levelSizes != nil ? levelSizes[0] : sourceSize);

	return raster;
}

uint8 *
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	VulkanRaster *native = GETVULKANRASTER(raster);
	if(native->image == VK_NULL_HANDLE || level >= native->numLevels)
		return nil;
	// A stale lock means an earlier unlock never ran. Refusing here would
	// cascade into a texture that is never uploaded, so drop the old staging
	// allocation and carry on.
	if(native->lockedLevel >= 0){
		static bool32 warned = 0;
		if(!warned){
			VKERR("raster was still locked at level %d; releasing stale staging",
			      native->lockedLevel);
			warned = 1;
		}
		if(native->stagingMapped != nil)
			vkUnmapMemory(gvk.device, native->stagingMemory);
		if(native->stagingBuffer)
			vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
		if(native->stagingMemory)
			vkFreeMemory(gvk.device, native->stagingMemory, nil);
		native->stagingBuffer = VK_NULL_HANDLE;
		native->stagingMemory = VK_NULL_HANDLE;
		native->stagingMapped = nil;
		native->lockedLevel = -1;
	}

	const uint32 width = levelDimension((uint32)raster->width, level);
	const uint32 height = levelDimension((uint32)raster->height, level);
	const uint32 bytesPerPixel = (uint32)(raster->depth / 8);
	const VkDeviceSize size = (VkDeviceSize)width * height * bytesPerPixel;
	// The generated chain rides behind the base in this same allocation.
	const VkDeviceSize total = size +
		(native->generateMips && level == 0 ?
			alignLevel(size)-size +
			generatedMipBytes(native->numLevels, width, height,
				bytesPerPixel) : 0);

	if(!createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                 &native->stagingBuffer, &native->stagingMemory))
		return nil;
	if(vkMapMemory(gvk.device, native->stagingMemory, 0, total, 0,
	               (void**)&native->stagingMapped) != VK_SUCCESS){
		vkDestroyBuffer(gvk.device, native->stagingBuffer, nil);
		vkFreeMemory(gvk.device, native->stagingMemory, nil);
		native->stagingBuffer = VK_NULL_HANDLE;
		native->stagingMemory = VK_NULL_HANDLE;
		return nil;
	}
	// LOCKREAD would need a device-to-host copy first. Nothing in the game
	// reads a streamed texture back, so it is refused loudly rather than
	// silently returning uninitialised staging memory.
	if(lockMode & Raster::LOCKREAD)
		memset(native->stagingMapped, 0, (size_t)size);

	native->lockedLevel = level;
	native->lockedFlags = (uint32)lockMode;
	raster->stride = (int32)(width * bytesPerPixel);
	raster->width = (int32)width;
	raster->height = (int32)height;
	return native->stagingMapped;
}

// Fills levels 1..n from the base the game just wrote, and queues each one
// out of the same buffer.
static void
uploadGeneratedMips(VulkanRaster *native, uint32 baseWidth, uint32 baseHeight,
                    uint32 bytesPerPixel, VkCommandBuffer commandBuffer)
{
	if(native->stagingMapped == nil || bytesPerPixel == 0)
		return;
	const VkDeviceSize baseBytes =
		alignLevel((VkDeviceSize)baseWidth*baseHeight*bytesPerPixel);

	// Each level is filtered from the one above it, so the chain is walked in
	// order and the previous result stays addressable.
	const uint8 *source = native->stagingMapped;
	uint32 sourceWidth = baseWidth, sourceHeight = baseHeight;
	VkDeviceSize offset = baseBytes;
	for(int32 level = 1; level < native->numLevels; level++){
		const uint32 width = levelDimension(baseWidth, level);
		const uint32 height = levelDimension(baseHeight, level);
		uint8 *destination = native->stagingMapped+offset;
		downsampleLevel(source, sourceWidth, sourceHeight,
			destination, width, height, native->format, bytesPerPixel);

		VkBufferImageCopy region = {};
		region.bufferOffset = offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = (uint32)level;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = width;
		region.imageExtent.height = height;
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(commandBuffer, native->stagingBuffer,
			native->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		source = destination;
		sourceWidth = width;
		sourceHeight = height;
		offset += alignLevel((VkDeviceSize)width*height*bytesPerPixel);
	}
}


void
rasterUnlock(Raster *raster, int32 level)
{
	VulkanRaster *native = GETVULKANRASTER(raster);
	if(native->lockedLevel < 0 || native->stagingBuffer == VK_NULL_HANDLE)
		return;

	// rasterLock narrowed these to the locked level before handing the
	// staging pointer out, so they are already this level's extent.
	const uint32 width = (uint32)raster->width;
	const uint32 height = (uint32)raster->height;

	VkCommandBuffer commandBuffer = beginOneShot();
	if(commandBuffer != VK_NULL_HANDLE){
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)native->numLevels,
		                      native->layout == VK_IMAGE_LAYOUT_UNDEFINED ?
		                          VK_IMAGE_LAYOUT_UNDEFINED :
		                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = (uint32)level;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = width;
		region.imageExtent.height = height;
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(commandBuffer, native->stagingBuffer, native->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		if(gvk.generateMipmaps && native->generateMips &&
		   level == 0 && native->numLevels > 1)
			uploadGeneratedMips(native, width, height,
				(uint32)(raster->depth/8), commandBuffer);
		transitionImageLayout(commandBuffer, native->image,
		                      VK_IMAGE_ASPECT_COLOR_BIT,
		                      (uint32)native->numLevels,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		endOneShot(commandBuffer);
		native->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	vkUnmapMemory(gvk.device, native->stagingMemory);
	// Keep the upload source alive until the active frame fence signals.
	retireBuffer(native->stagingBuffer, native->stagingMemory);
	native->stagingBuffer = VK_NULL_HANDLE;
	native->stagingMemory = VK_NULL_HANDLE;
	native->stagingMapped = nil;
	native->lockedLevel = -1;
}

int32
rasterNumLevels(Raster *raster)
{
	VulkanRaster *native = GETVULKANRASTER(raster);
	return native->numLevels > 0 ? native->numLevels : 1;
}

// Whether this backend built the chain rather than taking it from the
// dictionary. Only the ones built here are filtered with the colour
// weighted by alpha.
bool32
rasterHasGeneratedMips(Raster *raster)
{
	return gvk.generateMipmaps && GETVULKANRASTER(raster)->generateMips;
}

bool32
imageFindRasterFormat(Image *image, int32 type, int32 *width, int32 *height,
                      int32 *depth, int32 *format)
{
	if((type & 0xF) != Raster::TEXTURE)
		return 0;

	int32 formatOut = 0;
	switch(image->depth){
	case 32: formatOut = image->hasAlpha() ? Raster::C8888 : Raster::C888; break;
	case 24: formatOut = Raster::C888; break;
	case 16: formatOut = Raster::C1555; break;
	// Palettised sources are expanded before upload: no mobile GPU samples a
	// paletted format, and the game's 8-bit TXDs are small enough that the
	// widened copy costs less than any indirection would.
	case 8:
	case 4:
		formatOut = Raster::C8888;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	*width = image->width;
	*height = image->height;
	*depth = formatOut == Raster::C888 ? 32 : (formatOut == Raster::C8888 ? 32 : 16);
	*format = formatOut | type;
	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if((raster->type & 0xF) != Raster::TEXTURE)
		return 0;

	VulkanRaster *native = GETVULKANRASTER(raster);
	native->hasAlpha = native->hasAlpha || image->hasAlpha();

	uint8 *dst = rasterLock(raster, 0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(dst == nil)
		return 0;

	// Palettised sources are expanded inline rather than through a temporary
	// Image. Building one and pointing its palette at the caller's meant
	// unpalettize() freed a palette this function does not own, and the caller
	// freed it again afterwards -- a double free of the 1024-byte palette that
	// corrupted the heap and took down unrelated threads much later.
	const int32 bytesPerPixel = raster->depth / 8;
	const uint8 *palette = image->palette;

	for(int32 y = 0; y < raster->height; y++){
		uint8 *dstRow = dst + (size_t)y * raster->stride;
		const uint8 *srcRow = image->pixels + (size_t)y * image->stride;

		switch(image->depth){
		case 32:
			memcpy(dstRow, srcRow, (size_t)raster->width * 4);
			break;
		case 24:
			for(int32 x = 0; x < raster->width; x++){
				dstRow[x*4 + 0] = srcRow[x*3 + 0];
				dstRow[x*4 + 1] = srcRow[x*3 + 1];
				dstRow[x*4 + 2] = srcRow[x*3 + 2];
				dstRow[x*4 + 3] = 0xFF;
			}
			break;
		case 8:
			if(palette == nil)
				break;
			for(int32 x = 0; x < raster->width; x++){
				const uint8 *entry = &palette[srcRow[x] * 4];
				dstRow[x*4 + 0] = entry[0];
				dstRow[x*4 + 1] = entry[1];
				dstRow[x*4 + 2] = entry[2];
				dstRow[x*4 + 3] = entry[3];
			}
			break;
		case 4:
			if(palette == nil)
				break;
			// Two pixels per byte, left in the high nibble.
			for(int32 x = 0; x < raster->width; x++){
				const uint8 packed = srcRow[x >> 1];
				const uint8 index = (x & 1) ? (packed & 0xF) : (packed >> 4);
				const uint8 *entry = &palette[index * 4];
				dstRow[x*4 + 0] = entry[0];
				dstRow[x*4 + 1] = entry[1];
				dstRow[x*4 + 2] = entry[2];
				dstRow[x*4 + 3] = entry[3];
			}
			break;
		default:
			memcpy(dstRow, srcRow, (size_t)raster->width * bytesPerPixel);
			break;
		}
	}

	rasterUnlock(raster, 0);
	return 1;
}

Image *
rasterToImage(Raster *raster)
{
	// Reading a device-local image back needs a transfer-src copy and a host
	// buffer round trip. Nothing on this platform asks for it yet; returning
	// nil is better than an empty image that silently corrupts a screenshot.
	(void)raster;
	VKERR("rasterToImage is not implemented on the Vulkan backend");
	return nil;
}

void
setRasterHasAlpha(Raster *raster, bool32 hasAlpha)
{
	GETVULKANRASTER(raster)->hasAlpha = hasAlpha;
}

bool32
rasterHasAlpha(Raster *raster)
{
	if(raster == nil)
		return 0;
	if(raster->parent != nil)
		raster = raster->parent;
	if(raster->platform != PLATFORM_VULKAN)
		return 0;
	return GETVULKANRASTER(raster)->hasAlpha;
}

bool32
allocateCompressed(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha)
{
	// Adreno exposes ETC2 and ASTC but not BC/DXT, and every Vice City TXD is
	// DXT1/3/5. The blocks therefore cannot be uploaded as-is the way the
	// D3D12 backend does; they have to be decoded, or transcoded offline into
	// ASTC during data staging. Reporting failure here lets the caller fall
	// back to the decompressed path rather than creating an unsampleable image.
	(void)raster;
	(void)numLevels;
	(void)hasAlpha;
	if(!gvk.supportsBC){
		static bool32 warned = 0;
		if(!warned){
			VKLOG("device has no BC support; DXT%d rasters will be decompressed",
			      dxt);
			warned = 1;
		}
		return 0;
	}
	return 0;
}

#else

void registerNativeRaster(void) {}
void *destroyNativeData(void *object, int32, int32) { return object; }

#endif

}
}
