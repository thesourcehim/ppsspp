// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cassert>

#include "base/logging.h"
#include "base/timeutil.h"
#include "math/dataconv.h"

#include "Common/MemoryUtil.h"
#include "Core/MemMap.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "Core/Reporting.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"

#include "GPU/Math3D.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"

#include "Common/Vulkan/VulkanContext.h"
#include "Common/Vulkan/VulkanMemory.h"

#include "GPU/Common/TextureDecoder.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Vulkan/DrawEngineVulkan.h"
#include "GPU/Vulkan/TextureCacheVulkan.h"
#include "GPU/Vulkan/ShaderManagerVulkan.h"
#include "GPU/Vulkan/PipelineManagerVulkan.h"
#include "GPU/Vulkan/FramebufferVulkan.h"
#include "GPU/Vulkan/GPU_Vulkan.h"


enum {
	VERTEX_CACHE_SIZE = 4096 * 1024
};

#define VERTEXCACHE_DECIMATION_INTERVAL 17

enum { VAI_KILL_AGE = 120, VAI_UNRELIABLE_KILL_AGE = 240, VAI_UNRELIABLE_KILL_MAX = 4 };

enum {
	DRAW_BINDING_TEXTURE = 0,
	DRAW_BINDING_2ND_TEXTURE = 1,
	DRAW_BINDING_DYNUBO_BASE = 2,
	DRAW_BINDING_DYNUBO_LIGHT = 3,
	DRAW_BINDING_DYNUBO_BONE = 4,
	DRAW_BINDING_TESS_POS_TEXTURE = 5,
	DRAW_BINDING_TESS_TEX_TEXTURE = 6,
	DRAW_BINDING_TESS_COL_TEXTURE = 7,
};

enum {
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

DrawEngineVulkan::DrawEngineVulkan(VulkanContext *vulkan, Draw::DrawContext *draw)
	:	vulkan_(vulkan),
		draw_(draw),
		curFrame_(0),
		stats_{} {
	decOptions_.expandAllWeightsToFloat = false;
	decOptions_.expand8BitNormalsToFloat = false;

	// Allocate nicely aligned memory. Maybe graphics drivers will appreciate it.
	// All this is a LOT of memory, need to see if we can cut down somehow.
	decoded = (u8 *)AllocateMemoryPages(DECODED_VERTEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	decIndex = (u16 *)AllocateMemoryPages(DECODED_INDEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	splineBuffer = (u8 *)AllocateMemoryPages(SPLINE_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);

	indexGen.Setup(decIndex);

	InitDeviceObjects();

	tessDataTransfer = new TessellationDataTransferVulkan(vulkan);
}

void DrawEngineVulkan::InitDeviceObjects() {
	// All resources we need for PSP drawing. Usually only bindings 0 and 2-4 are populated.
	VkDescriptorSetLayoutBinding bindings[8];
	bindings[0].descriptorCount = 1;
	bindings[0].pImmutableSamplers = nullptr;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[0].binding = DRAW_BINDING_TEXTURE;
	bindings[1].descriptorCount = 1;
	bindings[1].pImmutableSamplers = nullptr;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = DRAW_BINDING_2ND_TEXTURE;
	bindings[2].descriptorCount = 1;
	bindings[2].pImmutableSamplers = nullptr;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[2].binding = DRAW_BINDING_DYNUBO_BASE;
	bindings[3].descriptorCount = 1;
	bindings[3].pImmutableSamplers = nullptr;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	bindings[3].binding = DRAW_BINDING_DYNUBO_LIGHT;
	bindings[4].descriptorCount = 1;
	bindings[4].pImmutableSamplers = nullptr;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	bindings[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	bindings[4].binding = DRAW_BINDING_DYNUBO_BONE;
	// Hardware tessellation
	bindings[5].descriptorCount = 1;
	bindings[5].pImmutableSamplers = nullptr;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[5].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	bindings[5].binding = DRAW_BINDING_TESS_POS_TEXTURE;
	bindings[6].descriptorCount = 1;
	bindings[6].pImmutableSamplers = nullptr;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[6].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	bindings[6].binding = DRAW_BINDING_TESS_TEX_TEXTURE;
	bindings[7].descriptorCount = 1;
	bindings[7].pImmutableSamplers = nullptr;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[7].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	bindings[7].binding = DRAW_BINDING_TESS_COL_TEXTURE;

	VkDevice device = vulkan_->GetDevice();

	VkDescriptorSetLayoutCreateInfo dsl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsl.bindingCount = 8;
	dsl.pBindings = bindings;
	VkResult res = vkCreateDescriptorSetLayout(device, &dsl, nullptr, &descriptorSetLayout_);
	assert(VK_SUCCESS == res);

	VkDescriptorPoolSize dpTypes[2];
	dpTypes[0].descriptorCount = 2048;
	dpTypes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	dpTypes[1].descriptorCount = 4096;
	dpTypes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dp.pNext = nullptr;
	dp.flags = 0;   // Don't want to mess around with individually freeing these, let's go fixed each frame and zap the whole array. Might try the dynamic approach later.
	dp.maxSets = 2048;
	dp.pPoolSizes = dpTypes;
	dp.poolSizeCount = ARRAY_SIZE(dpTypes);

	// We are going to use one-shot descriptors in the initial implementation. Might look into caching them
	// if creating and updating them turns out to be expensive.
	for (int i = 0; i < VulkanContext::MAX_INFLIGHT_FRAMES; i++) {
		// If we run out of memory, try with less descriptors.
		for (int tries = 0; tries < 3; ++tries) {
			VkResult res = vkCreateDescriptorPool(vulkan_->GetDevice(), &dp, nullptr, &frame_[i].descPool);
			if (res == VK_SUCCESS) {
				break;
			}
			// Let's try to reduce the counts.
			assert(res == VK_ERROR_OUT_OF_HOST_MEMORY || res == VK_ERROR_OUT_OF_DEVICE_MEMORY);
			dpTypes[0].descriptorCount /= 2;
			dpTypes[1].descriptorCount /= 2;
		}
		frame_[i].pushUBO = new VulkanPushBuffer(vulkan_, 8 * 1024 * 1024);
		frame_[i].pushVertex = new VulkanPushBuffer(vulkan_, 2 * 1024 * 1024);
		frame_[i].pushIndex = new VulkanPushBuffer(vulkan_, 1 * 1024 * 1024);
	}

	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.pPushConstantRanges = nullptr;
	pl.pushConstantRangeCount = 0;
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &descriptorSetLayout_;
	pl.flags = 0;
	res = vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout_);
	assert(VK_SUCCESS == res);

	VkSamplerCreateInfo samp = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samp.flags = 0;
	samp.magFilter = VK_FILTER_NEAREST;
	samp.minFilter = VK_FILTER_NEAREST;
	res = vkCreateSampler(device, &samp, nullptr, &depalSampler_);
	res = vkCreateSampler(device, &samp, nullptr, &nullSampler_);
	assert(VK_SUCCESS == res);

	vertexCache_ = new VulkanPushBuffer(vulkan_, VERTEX_CACHE_SIZE);
}

DrawEngineVulkan::~DrawEngineVulkan() {
	FreeMemoryPages(decoded, DECODED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(decIndex, DECODED_INDEX_BUFFER_SIZE);
	FreeMemoryPages(splineBuffer, SPLINE_BUFFER_SIZE);

	DestroyDeviceObjects();

	delete tessDataTransfer;
}

void DrawEngineVulkan::FrameData::Destroy(VulkanContext *vulkan) {
	if (descPool != VK_NULL_HANDLE) {
		vulkan->Delete().QueueDeleteDescriptorPool(descPool);
	}

	if (pushUBO) {
		pushUBO->Destroy(vulkan);
		delete pushUBO;
		pushUBO = nullptr;
	}
	if (pushVertex) {
		pushVertex->Destroy(vulkan);
		delete pushVertex;
		pushVertex = nullptr;
	}
	if (pushIndex) {
		pushIndex->Destroy(vulkan);
		delete pushIndex;
		pushIndex = nullptr;
	}
}

void DrawEngineVulkan::DestroyDeviceObjects() {
	for (int i = 0; i < 2; i++) {
		frame_[i].Destroy(vulkan_);
	}
	if (depalSampler_ != VK_NULL_HANDLE)
		vulkan_->Delete().QueueDeleteSampler(depalSampler_);
	if (nullSampler_ != VK_NULL_HANDLE)
		vulkan_->Delete().QueueDeleteSampler(nullSampler_);
	if (pipelineLayout_ != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(vulkan_->GetDevice(), pipelineLayout_, nullptr);
	pipelineLayout_ = VK_NULL_HANDLE;
	if (descriptorSetLayout_ != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(vulkan_->GetDevice(), descriptorSetLayout_, nullptr);
	descriptorSetLayout_ = VK_NULL_HANDLE;
	if (nullTexture_) {
		nullTexture_->Destroy();
		delete nullTexture_;
		nullTexture_ = nullptr;
	}
	vertexCache_->Destroy(vulkan_);
	delete vertexCache_;
	vertexCache_ = nullptr;
}

void DrawEngineVulkan::DeviceLost() {
	DestroyDeviceObjects();
	DirtyAllUBOs();
}

void DrawEngineVulkan::DeviceRestore(VulkanContext *vulkan) {
	vulkan_ = vulkan;

	InitDeviceObjects();
}

void DrawEngineVulkan::BeginFrame() {
	lastCmd_ = VK_NULL_HANDLE;
	lastPipeline_ = nullptr;

	FrameData *frame = &frame_[curFrame_];
	vkResetDescriptorPool(vulkan_->GetDevice(), frame->descPool, 0);
	frame->descSets.clear();

	// First reset all buffers, then begin. This is so that Reset can free memory and Begin can allocate it,
	// if growing the buffer is needed. Doing it this way will reduce fragmentation if more than one buffer
	// needs to grow in the same frame. The state where many buffers are reset can also be used to 
	// defragment memory.
	frame->pushUBO->Reset();
	frame->pushVertex->Reset();
	frame->pushIndex->Reset();

	frame->pushUBO->Begin(vulkan_);
	frame->pushVertex->Begin(vulkan_);
	frame->pushIndex->Begin(vulkan_);

	// TODO : Find a better place to do this.
	if (!nullTexture_) {
		nullTexture_ = new VulkanTexture(vulkan_);
		int w = 8;
		int h = 8;
		nullTexture_->CreateDirect(w, h, 1, VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		uint32_t bindOffset;
		VkBuffer bindBuf;
		uint32_t *data = (uint32_t *)frame->pushUBO->Push(w * h * 4, &bindOffset, &bindBuf);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				// data[y*w + x] = ((x ^ y) & 1) ? 0xFF808080 : 0xFF000000;   // gray/black checkerboard
				data[y*w + x] = 0;  // black
			}
		}
		nullTexture_->UploadMip(0, w, h, bindBuf, bindOffset, w);
		nullTexture_->EndCreate();
	}

	DirtyAllUBOs();

	// Wipe the vertex cache if it's grown too large.
	if (vertexCache_->GetTotalSize() > VERTEX_CACHE_SIZE) {
		vertexCache_->Destroy(vulkan_);
		delete vertexCache_;  // orphans the buffers, they'll get deleted once no longer used by an in-flight frame.
		vertexCache_ = new VulkanPushBuffer(vulkan_, VERTEX_CACHE_SIZE);
		vai_.clear();
	}

	vertexCache_->BeginNoReset();

	if (--decimationCounter_ <= 0) {
		decimationCounter_ = VERTEXCACHE_DECIMATION_INTERVAL;

		const int threshold = gpuStats.numFlips - VAI_KILL_AGE;
		const int unreliableThreshold = gpuStats.numFlips - VAI_UNRELIABLE_KILL_AGE;
		int unreliableLeft = VAI_UNRELIABLE_KILL_MAX;
		for (auto iter = vai_.begin(); iter != vai_.end(); ) {
			bool kill;
			if (iter->second->status == VertexArrayInfoVulkan::VAI_UNRELIABLE) {
				// We limit killing unreliable so we don't rehash too often.
				kill = iter->second->lastFrame < unreliableThreshold && --unreliableLeft >= 0;
			} else {
				kill = iter->second->lastFrame < threshold;
			}
			if (kill) {
				delete iter->second;
				vai_.erase(iter++);
			} else {
				++iter;
			}
		}
	}
}

void DrawEngineVulkan::EndFrame() {
	FrameData *frame = &frame_[curFrame_];
	stats_.pushUBOSpaceUsed = (int)frame->pushUBO->GetOffset();
	stats_.pushVertexSpaceUsed = (int)frame->pushVertex->GetOffset();
	stats_.pushIndexSpaceUsed = (int)frame->pushIndex->GetOffset();
	frame->pushUBO->End();
	frame->pushVertex->End();
	frame->pushIndex->End();
	curFrame_++;
	if (curFrame_ >= vulkan_->GetInflightFrames())
		curFrame_ = 0;
	vertexCache_->End();
}

void DrawEngineVulkan::SetupVertexDecoder(u32 vertType) {
	SetupVertexDecoderInternal(vertType);
}

inline void DrawEngineVulkan::SetupVertexDecoderInternal(u32 vertType) {
	// As the decoder depends on the UVGenMode when we use UV prescale, we simply mash it
	// into the top of the verttype where there are unused bits.
	const u32 vertTypeID = (vertType & 0xFFFFFF) | (gstate.getUVGenMode() << 24);

	// If vtype has changed, setup the vertex decoder.
	if (vertTypeID != lastVType_) {
		dec_ = GetVertexDecoder(vertTypeID);
		lastVType_ = vertTypeID;
	}
	if (!dec_)
		Crash();
}

void DrawEngineVulkan::SubmitPrim(void *verts, void *inds, GEPrimitiveType prim, int vertexCount, u32 vertType, int *bytesRead) {
	if (!indexGen.PrimCompatible(prevPrim_, prim) || numDrawCalls >= MAX_DEFERRED_DRAW_CALLS || vertexCountInDrawCalls_ + vertexCount > VERTEX_BUFFER_MAX)
		Flush();

	// TODO: Is this the right thing to do?
	if (prim == GE_PRIM_KEEP_PREVIOUS) {
		prim = prevPrim_ != GE_PRIM_INVALID ? prevPrim_ : GE_PRIM_POINTS;
	} else {
		prevPrim_ = prim;
	}

	SetupVertexDecoderInternal(vertType);

	*bytesRead = vertexCount * dec_->VertexSize();
	if ((vertexCount < 2 && prim > 0) || (vertexCount < 3 && prim > 2 && prim != GE_PRIM_RECTANGLES))
		return;

	DeferredDrawCall &dc = drawCalls[numDrawCalls];
	dc.verts = verts;
	dc.inds = inds;
	dc.vertType = vertType;
	dc.indexType = (vertType & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT;
	dc.prim = prim;
	dc.vertexCount = vertexCount;

	if (g_Config.bVertexCache) {
		u32 dhash = dcid_;
		dhash ^= (u32)(uintptr_t)verts;
		dhash = __rotl(dhash, 13);
		dhash ^= (u32)(uintptr_t)inds;
		dhash = __rotl(dhash, 13);
		dhash ^= (u32)vertType;
		dhash = __rotl(dhash, 13);
		dhash ^= (u32)vertexCount;
		dhash = __rotl(dhash, 13);
		dhash ^= (u32)prim;
		dcid_ = dhash;
	}

	if (inds) {
		GetIndexBounds(inds, vertexCount, vertType, &dc.indexLowerBound, &dc.indexUpperBound);
	} else {
		dc.indexLowerBound = 0;
		dc.indexUpperBound = vertexCount - 1;
	}

	uvScale[numDrawCalls] = gstate_c.uv;

	numDrawCalls++;
	vertexCountInDrawCalls_ += vertexCount;

	if (g_Config.bSoftwareSkinning && (vertType & GE_VTYPE_WEIGHT_MASK)) {
		DecodeVertsStep(decoded, decodeCounter_, decodedVerts_);
		decodeCounter_++;
	}

	if (prim == GE_PRIM_RECTANGLES && (gstate.getTextureAddress(0) & 0x3FFFFFFF) == (gstate.getFrameBufAddress() & 0x3FFFFFFF)) {
		// Rendertarget == texture?
		if (!g_Config.bDisableSlowFramebufEffects) {
			gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
			Flush();
		}
	}
}

int DrawEngineVulkan::ComputeNumVertsToDecode() const {
	int vertsToDecode = 0;
	if (drawCalls[0].indexType == GE_VTYPE_IDX_NONE >> GE_VTYPE_IDX_SHIFT) {
		for (int i = 0; i < numDrawCalls; i++) {
			const DeferredDrawCall &dc = drawCalls[i];
			vertsToDecode += dc.vertexCount;
		}
	} else {
		// TODO: Share this computation with DecodeVertsStep?
		for (int i = 0; i < numDrawCalls; i++) {
			const DeferredDrawCall &dc = drawCalls[i];
			int lastMatch = i;
			const int total = numDrawCalls;
			int indexLowerBound = dc.indexLowerBound;
			int indexUpperBound = dc.indexUpperBound;
			for (int j = i + 1; j < total; ++j) {
				if (drawCalls[j].verts != dc.verts)
					break;

				indexLowerBound = std::min(indexLowerBound, (int)drawCalls[j].indexLowerBound);
				indexUpperBound = std::max(indexUpperBound, (int)drawCalls[j].indexUpperBound);
				lastMatch = j;
			}
			vertsToDecode += indexUpperBound - indexLowerBound + 1;
			i = lastMatch;
		}
	}
	return vertsToDecode;
}

void DrawEngineVulkan::DecodeVerts(VulkanPushBuffer *push, uint32_t *bindOffset, VkBuffer *vkbuf) {
	u8 *dest = decoded;

	// Figure out how much pushbuffer space we need to allocate.
	if (push) {
		int vertsToDecode = ComputeNumVertsToDecode();
		dest = (u8 *)push->Push(vertsToDecode * dec_->GetDecVtxFmt().stride, bindOffset, vkbuf);
	}

	const UVScale origUV = gstate_c.uv;
	for (; decodeCounter_ < numDrawCalls; decodeCounter_++) {
		gstate_c.uv = uvScale[decodeCounter_];
		DecodeVertsStep(dest, decodeCounter_, decodedVerts_);  // NOTE! DecodeVertsStep can modify decodeCounter_!
	}
	gstate_c.uv = origUV;

	// Sanity check
	if (indexGen.Prim() < 0) {
		ERROR_LOG_REPORT(G3D, "DecodeVerts: Failed to deduce prim: %i", indexGen.Prim());
		// Force to points (0)
		indexGen.AddPrim(GE_PRIM_POINTS, 0);
	}
}


VkDescriptorSet DrawEngineVulkan::GetDescriptorSet(VkImageView imageView, VkSampler sampler, VkBuffer base, VkBuffer light, VkBuffer bone) {
	DescriptorSetKey key;
	key.imageView_ = imageView;
	key.sampler_ = sampler;
	key.secondaryImageView_ = VK_NULL_HANDLE;
	key.base_ = base;
	key.light_ = light;
	key.bone_ = bone;
	assert(base != VK_NULL_HANDLE);
	assert(light != VK_NULL_HANDLE);
	assert(bone != VK_NULL_HANDLE);

	FrameData *frame = &frame_[curFrame_];
	if (!(gstate_c.bezier || gstate_c.spline)) { // Has no cache when HW tessellation.
		auto iter = frame->descSets.find(key);
		if (iter != frame->descSets.end()) {
			return iter->second;
		}
	}

	// Didn't find one in the frame descriptor set cache, let's make a new one.
	// We wipe the cache on every frame.

	VkDescriptorSet desc;
	VkDescriptorSetAllocateInfo descAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	descAlloc.pNext = nullptr;
	descAlloc.pSetLayouts = &descriptorSetLayout_;
	descAlloc.descriptorPool = frame->descPool;
	descAlloc.descriptorSetCount = 1;
	VkResult result = vkAllocateDescriptorSets(vulkan_->GetDevice(), &descAlloc, &desc);
	assert(result == VK_SUCCESS);

	// We just don't write to the slots we don't care about.
	VkWriteDescriptorSet writes[7];
	memset(writes, 0, sizeof(writes));
	// Main texture
	int n = 0;
	VkDescriptorImageInfo tex{};
	if (imageView) {
		// TODO: Also support LAYOUT_GENERAL to be able to texture from framebuffers without transitioning them?
		tex.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		tex.imageView = imageView;
		tex.sampler = sampler;
		writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[n].pNext = nullptr;
		writes[n].dstBinding = DRAW_BINDING_TEXTURE;
		writes[n].pImageInfo = &tex;
		writes[n].descriptorCount = 1;
		writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[n].dstSet = desc;
		n++;
	}

  // Skipping 2nd texture for now.

	// Tessellation data textures
	if (gstate_c.bezier || gstate_c.spline) {
		VkDescriptorImageInfo tess_tex[3]{};
		VkSampler sampler = ((TessellationDataTransferVulkan *)tessDataTransfer)->GetSampler();
		for (int i = 0; i < 3; i++) {
			VulkanTexture *texture = ((TessellationDataTransferVulkan *)tessDataTransfer)->GetTexture(i);
			VkImageView imageView = texture->GetImageView();
			if (i == 0 || imageView) {
				tess_tex[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				tess_tex[i].imageView = imageView;
				tess_tex[i].sampler = sampler;
				writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[n].pNext = nullptr;
				writes[n].dstBinding = DRAW_BINDING_TESS_POS_TEXTURE + i;
				writes[n].pImageInfo = &tess_tex[i];
				writes[n].descriptorCount = 1;
				writes[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[n].dstSet = desc;
				n++;
			}
		}
	}

	// Uniform buffer objects
	VkDescriptorBufferInfo buf[3]{};
	int count = 0;
	buf[count].buffer = base;
	buf[count].offset = 0;
	buf[count].range = sizeof(UB_VS_FS_Base);
	count++;
	buf[count].buffer = light;
	buf[count].offset = 0;
	buf[count].range = sizeof(UB_VS_Lights);
	count++;
	buf[count].buffer = bone;
	buf[count].offset = 0;
	buf[count].range = sizeof(UB_VS_Bones);
	count++;
	for (int i = 0; i < count; i++) {
		writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[n].pNext = nullptr;
		writes[n].dstBinding = DRAW_BINDING_DYNUBO_BASE + i;
		writes[n].dstArrayElement = 0;
		writes[n].pBufferInfo = &buf[i];
		writes[n].dstSet = desc;
		writes[n].descriptorCount = 1;
		writes[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		n++;
	}

	vkUpdateDescriptorSets(vulkan_->GetDevice(), n, writes, 0, nullptr);

	if (!(gstate_c.bezier || gstate_c.spline)) // Avoid caching when HW tessellation.
		frame->descSets[key] = desc;
	return desc;
}

void DrawEngineVulkan::DirtyAllUBOs() {
	baseUBOOffset = 0;
	lightUBOOffset = 0;
	boneUBOOffset = 0;
	baseBuf = VK_NULL_HANDLE;
	lightBuf = VK_NULL_HANDLE;
	boneBuf = VK_NULL_HANDLE;
	dirtyUniforms_ = DIRTY_BASE_UNIFORMS | DIRTY_LIGHT_UNIFORMS | DIRTY_BONE_UNIFORMS;
	imageView = VK_NULL_HANDLE;
	sampler = VK_NULL_HANDLE;
	gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
}

void MarkUnreliable(VertexArrayInfoVulkan *vai) {
	vai->status = VertexArrayInfoVulkan::VAI_UNRELIABLE;
	// TODO: If we change to a real allocator, free the data here.
	// For now we just leave it in the pushbuffer.
}

// The inline wrapper in the header checks for numDrawCalls == 0d
void DrawEngineVulkan::DoFlush() {
	gpuStats.numFlushes++;
	// TODO: Should be enough to update this once per frame?
	gpuStats.numTrackedVertexArrays = (int)vai_.size();

	VkCommandBuffer cmd = (VkCommandBuffer)draw_->GetNativeObject(Draw::NativeObject::RENDERPASS_COMMANDBUFFER);
	if (cmd != lastCmd_) {
		lastPipeline_ = nullptr;
		lastCmd_ = cmd;
		// Since we have a new cmdbuf, dirty our dynamic state so it gets re-set.
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE|DIRTY_DEPTHSTENCIL_STATE);
	}

	VkRenderPass rp = (VkRenderPass)draw_->GetNativeObject(Draw::NativeObject::CURRENT_RENDERPASS);
	if (!rp)
		Crash();

	FrameData *frame = &frame_[curFrame_];

	bool textureNeedsApply = false;
	if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
		textureCache_->SetTexture();
		gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
		textureNeedsApply = true;
		if (gstate_c.needShaderTexClamp) {
			// We will rarely need to set this, so let's do it every time on use rather than in runloop.
			// Most of the time non-framebuffer textures will be used which can be clamped themselves.
			gstate_c.Dirty(DIRTY_TEXCLAMP);
		}
	}

	GEPrimitiveType prim = prevPrim_;

	bool useHWTransform = CanUseHardwareTransform(prim);

	VulkanVertexShader *vshader = nullptr;
	VulkanFragmentShader *fshader = nullptr;

	uint32_t ibOffset = 0;
	uint32_t vbOffset = 0;
	
	VkRenderPass renderPass = (VkRenderPass)draw_->GetNativeObject(Draw::NativeObject::COMPATIBLE_RENDERPASS);

	if (useHWTransform) {
		// We don't detect clears in this path, so here we can switch framebuffers if necessary.

		int vertexCount = 0;
		int maxIndex;
		bool useElements = true;

		// Cannot cache vertex data with morph enabled.
		bool useCache = g_Config.bVertexCache && !(lastVType_ & GE_VTYPE_MORPHCOUNT_MASK);
		// Also avoid caching when software skinning.
		VkBuffer vbuf = VK_NULL_HANDLE;
		VkBuffer ibuf = VK_NULL_HANDLE;
		if (g_Config.bSoftwareSkinning && (lastVType_ & GE_VTYPE_WEIGHT_MASK)) {
			useCache = false;
		}

		if (useCache) {
			u32 id = dcid_ ^ gstate.getUVGenMode();  // This can have an effect on which UV decoder we need to use! And hence what the decoded data will look like. See #9263
			auto iter = vai_.find(id);
			VertexArrayInfoVulkan *vai;
			if (iter != vai_.end()) {
				// We've seen this before. Could have been a cached draw.
				vai = iter->second;
			} else {
				vai = new VertexArrayInfoVulkan();
				vai_[id] = vai;
			}

			switch (vai->status) {
			case VertexArrayInfoVulkan::VAI_NEW:
			{
				// Haven't seen this one before. We don't actually upload the vertex data yet.
				ReliableHashType dataHash = ComputeHash();
				vai->hash = dataHash;
				vai->minihash = ComputeMiniHash();
				vai->status = VertexArrayInfoVulkan::VAI_HASHING;
				vai->drawsUntilNextFullHash = 0;
				DecodeVerts(frame->pushVertex, &vbOffset, &vbuf);  // writes to indexGen
				vai->numVerts = indexGen.VertexCount();
				vai->prim = indexGen.Prim();
				vai->maxIndex = indexGen.MaxIndex();
				vai->flags = gstate_c.vertexFullAlpha ? VAIVULKAN_FLAG_VERTEXFULLALPHA : 0;
				goto rotateVBO;
			}

			// Hashing - still gaining confidence about the buffer.
			// But if we get this far it's likely to be worth uploading the data.
			case VertexArrayInfoVulkan::VAI_HASHING:
			{
				vai->numDraws++;
				if (vai->lastFrame != gpuStats.numFlips) {
					vai->numFrames++;
				}
				if (vai->drawsUntilNextFullHash == 0) {
					// Let's try to skip a full hash if mini would fail.
					const u32 newMiniHash = ComputeMiniHash();
					ReliableHashType newHash = vai->hash;
					if (newMiniHash == vai->minihash) {
						newHash = ComputeHash();
					}
					if (newMiniHash != vai->minihash || newHash != vai->hash) {
						MarkUnreliable(vai);
						DecodeVerts(frame->pushVertex, &vbOffset, &vbuf);
						goto rotateVBO;
					}
					if (vai->numVerts > 64) {
						// exponential backoff up to 16 draws, then every 24
						vai->drawsUntilNextFullHash = std::min(24, vai->numFrames);
					} else {
						// Lower numbers seem much more likely to change.
						vai->drawsUntilNextFullHash = 0;
					}
					// TODO: tweak
					//if (vai->numFrames > 1000) {
					//	vai->status = VertexArrayInfo::VAI_RELIABLE;
					//}
				} else {
					vai->drawsUntilNextFullHash--;
					u32 newMiniHash = ComputeMiniHash();
					if (newMiniHash != vai->minihash) {
						MarkUnreliable(vai);
						DecodeVerts(frame->pushVertex, &vbOffset, &vbuf);
						goto rotateVBO;
					}
				}

				if (!vai->vb) {
					// Directly push to the vertex cache.
					DecodeVerts(vertexCache_, &vai->vbOffset, &vai->vb);
					_dbg_assert_msg_(G3D, gstate_c.vertBounds.minV >= gstate_c.vertBounds.maxV, "Should not have checked UVs when caching.");
					vai->numVerts = indexGen.VertexCount();
					vai->prim = indexGen.Prim();
					vai->maxIndex = indexGen.MaxIndex();
					vai->flags = gstate_c.vertexFullAlpha ? VAIVULKAN_FLAG_VERTEXFULLALPHA : 0;
					useElements = !indexGen.SeenOnlyPurePrims();
					if (!useElements && indexGen.PureCount()) {
						vai->numVerts = indexGen.PureCount();
					}
					if (useElements) {
						u32 size = sizeof(uint16_t) * indexGen.VertexCount();
						void *dest = vertexCache_->Push(size, &vai->ibOffset, &vai->ib);
						memcpy(dest, decIndex, size);
					} else {
						vai->ib = VK_NULL_HANDLE;
						vai->ibOffset = 0;
					}
				} else {
					gpuStats.numCachedDrawCalls++;
					useElements = vai->ib ? true : false;
					gpuStats.numCachedVertsDrawn += vai->numVerts;
					gstate_c.vertexFullAlpha = vai->flags & VAIVULKAN_FLAG_VERTEXFULLALPHA;
				}
				vbuf = vai->vb;
				ibuf = vai->ib;
				vbOffset = vai->vbOffset;
				ibOffset = vai->ibOffset;
				vertexCount = vai->numVerts;
				maxIndex = vai->maxIndex;
				prim = static_cast<GEPrimitiveType>(vai->prim);
				break;
			}

			// Reliable - we don't even bother hashing anymore. Right now we don't go here until after a very long time.
			case VertexArrayInfoVulkan::VAI_RELIABLE:
			{
				vai->numDraws++;
				if (vai->lastFrame != gpuStats.numFlips) {
					vai->numFrames++;
				}
				gpuStats.numCachedDrawCalls++;
				gpuStats.numCachedVertsDrawn += vai->numVerts;
				vbuf = vai->vb;
				ibuf = vai->ib;
				vbOffset = vai->vbOffset;
				ibOffset = vai->ibOffset;
				vertexCount = vai->numVerts;
				maxIndex = vai->maxIndex;
				prim = static_cast<GEPrimitiveType>(vai->prim);

				gstate_c.vertexFullAlpha = vai->flags & VAIVULKAN_FLAG_VERTEXFULLALPHA;
				break;
			}

			case VertexArrayInfoVulkan::VAI_UNRELIABLE:
			{
				vai->numDraws++;
				if (vai->lastFrame != gpuStats.numFlips) {
					vai->numFrames++;
				}
				DecodeVerts(frame->pushVertex, &vbOffset, &vbuf);
				goto rotateVBO;
			}
			default:
				break;
			}
		} else {
			if (g_Config.bSoftwareSkinning && (lastVType_ & GE_VTYPE_WEIGHT_MASK)) {
				// If software skinning, we've already predecoded into "decoded". So push that content.
				VkDeviceSize size = decodedVerts_ * dec_->GetDecVtxFmt().stride;
				u8 *dest = (u8 *)frame->pushVertex->Push(size, &vbOffset, &vbuf);
				memcpy(dest, decoded, size);
			} else {
				// Decode directly into the pushbuffer
				DecodeVerts(frame->pushVertex, &vbOffset, &vbuf);
			}

	rotateVBO:
			gpuStats.numUncachedVertsDrawn += indexGen.VertexCount();
			useElements = !indexGen.SeenOnlyPurePrims();
			vertexCount = indexGen.VertexCount();
			if (!useElements && indexGen.PureCount()) {
				vertexCount = indexGen.PureCount();
			}
			prim = indexGen.Prim();
		}

		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		if (textureNeedsApply) {
			textureCache_->ApplyTexture();
			textureCache_->GetVulkanHandles(imageView, sampler);
			if (imageView == VK_NULL_HANDLE)
				imageView = nullTexture_->GetImageView();
			if (sampler == VK_NULL_HANDLE)
				sampler = nullSampler_;
		}

		if (!lastPipeline_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE) || prim != lastPrim_) {
			shaderManager_->GetShaders(prim, lastVType_, &vshader, &fshader, useHWTransform);
			if (prim != lastPrim_ || gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
				ConvertStateToVulkanKey(*framebufferManager_, shaderManager_, prim, pipelineKey_, dynState_);
			}
			VulkanPipeline *pipeline = pipelineManager_->GetOrCreatePipeline(pipelineLayout_, renderPass, pipelineKey_, dec_, vshader, fshader, true);
			if (!pipeline) {
				// Already logged, let's bail out.
				return;
			}
			if (pipeline != lastPipeline_) {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);  // TODO: Avoid if same as last draw.
				lastPipeline_ = pipeline;
			}
			ApplyDrawStateLate(cmd, false, 0);
			gstate_c.Clean(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
		}
		lastPrim_ = prim;

		dirtyUniforms_ |= shaderManager_->UpdateUniforms();

		UpdateUBOs(frame);

		VkDescriptorSet ds = GetDescriptorSet(imageView, sampler, baseBuf, lightBuf, boneBuf);

		const uint32_t dynamicUBOOffsets[3] = {
			baseUBOOffset, lightUBOOffset, boneUBOOffset,
		};
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &ds, 3, dynamicUBOOffsets);

		int stride = dec_->GetDecVtxFmt().stride;

		VkDeviceSize offsets[1] = { vbOffset };
		if (useElements) {
			if (!ibuf)
				ibOffset = (uint32_t)frame->pushIndex->Push(decIndex, sizeof(uint16_t) * indexGen.VertexCount(), &ibuf);
			// TODO (maybe): Avoid rebinding vertex/index buffers if the vertex size stays the same by using the offset arguments.
			// Not sure if actually worth it, binding buffers should be fast.
			vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, offsets);
			vkCmdBindIndexBuffer(cmd, ibuf, ibOffset, VK_INDEX_TYPE_UINT16);
			int numInstances = (gstate_c.bezier || gstate_c.spline) ? numPatches : 1;
			vkCmdDrawIndexed(cmd, vertexCount, numInstances, 0, 0, 0);
		} else {
			vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, offsets);
			vkCmdDraw(cmd, vertexCount, 1, 0, 0);
		}
	} else {
		// Decode to "decoded"
		DecodeVerts(nullptr, nullptr, nullptr);
		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) && (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}

		gpuStats.numUncachedVertsDrawn += indexGen.VertexCount();
		prim = indexGen.Prim();
		// Undo the strip optimization, not supported by the SW code yet.
		if (prim == GE_PRIM_TRIANGLE_STRIP)
			prim = GE_PRIM_TRIANGLES;
		VERBOSE_LOG(G3D, "Flush prim %i SW! %i verts in one go", prim, indexGen.VertexCount());

		lastPrim_ = prim;
		int numTrans = 0;
		bool drawIndexed = false;
		u16 *inds = decIndex;
		TransformedVertex *drawBuffer = NULL;
		SoftwareTransformResult result;
		memset(&result, 0, sizeof(result));

		SoftwareTransformParams params;
		memset(&params, 0, sizeof(params));
		params.decoded = decoded;
		params.transformed = transformed;
		params.transformedExpanded = transformedExpanded;
		params.fbman = framebufferManager_;
		params.texCache = textureCache_;
		params.allowSeparateAlphaClear = false;

		int maxIndex = indexGen.MaxIndex();
		SoftwareTransform(
			prim, indexGen.VertexCount(),
			dec_->VertexType(), inds, GE_VTYPE_IDX_16BIT, dec_->GetDecVtxFmt(),
			maxIndex, drawBuffer, numTrans, drawIndexed, &params, &result);

		// Only here, where we know whether to clear or to draw primitives, should we actually set the current framebuffer! Because that gives use the opportunity
		// to use a "pre-clear" render pass, for high efficiency on tilers.
		if (result.action == SW_DRAW_PRIMITIVES) {
			if (textureNeedsApply) {
				textureCache_->ApplyTexture();
				textureCache_->GetVulkanHandles(imageView, sampler);
				if (imageView == VK_NULL_HANDLE)
					imageView = nullTexture_->GetImageView();
				if (sampler == VK_NULL_HANDLE)
					sampler = nullSampler_;
			}

			ConvertStateToVulkanKey(*framebufferManager_, shaderManager_, prim, pipelineKey_, dynState_);
			ApplyDrawStateLate(cmd, result.setStencil, result.stencilValue);

			shaderManager_->GetShaders(prim, lastVType_, &vshader, &fshader, useHWTransform);
			VulkanPipeline *pipeline = pipelineManager_->GetOrCreatePipeline(pipelineLayout_, renderPass, pipelineKey_, dec_, vshader, fshader, false);
			if (!pipeline) {
				// Already logged, let's bail out.
				return;
			}
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);  // TODO: Avoid if same as last draw.

			dirtyUniforms_ |= shaderManager_->UpdateUniforms();

			// Even if the first draw is through-mode, make sure we at least have one copy of these uniforms buffered
			UpdateUBOs(frame);

			VkDescriptorSet ds = GetDescriptorSet(imageView, sampler, baseBuf, lightBuf, boneBuf);
			const uint32_t dynamicUBOOffsets[3] = {
				baseUBOOffset, lightUBOOffset, boneUBOOffset,
			};
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &ds, 3, dynamicUBOOffsets);

			if (drawIndexed) {
				VkBuffer vbuf, ibuf;
				vbOffset = (uint32_t)frame->pushVertex->Push(drawBuffer, maxIndex * sizeof(TransformedVertex), &vbuf);
				ibOffset = (uint32_t)frame->pushIndex->Push(inds, sizeof(short) * numTrans, &ibuf);
				VkDeviceSize offsets[1] = { vbOffset };
				// TODO: Avoid rebinding if the vertex size stays the same by using the offset arguments
				vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, offsets);
				vkCmdBindIndexBuffer(cmd, ibuf, ibOffset, VK_INDEX_TYPE_UINT16);
				vkCmdDrawIndexed(cmd, numTrans, 1, 0, 0, 0);
			} else {
				VkBuffer vbuf;
				vbOffset = (uint32_t)frame->pushVertex->Push(drawBuffer, numTrans * sizeof(TransformedVertex), &vbuf);
				VkDeviceSize offsets[1] = { vbOffset };
				// TODO: Avoid rebinding if the vertex size stays the same by using the offset arguments
				vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, offsets);
				vkCmdDraw(cmd, numTrans, 1, 0, 0);
			}
		} else if (result.action == SW_CLEAR) {
			// Note: we won't get here if the clear is alpha but not color, or color but not alpha.

			// We let the framebuffer manager handle the clear. It can use renderpasses to optimize on tilers.
			// If non-buffered though, it'll just do a plain clear.
			framebufferManager_->NotifyClear(gstate.isClearModeColorMask(), gstate.isClearModeAlphaMask(), gstate.isClearModeDepthMask(), result.color, result.depth);

			int scissorX1 = gstate.getScissorX1();
			int scissorY1 = gstate.getScissorY1();
			int scissorX2 = gstate.getScissorX2() + 1;
			int scissorY2 = gstate.getScissorY2() + 1;
			framebufferManager_->SetSafeSize(scissorX2, scissorY2);

			if (g_Config.bBlockTransferGPU && (gstate_c.featureFlags & GPU_USE_CLEAR_RAM_HACK) && gstate.isClearModeColorMask() && (gstate.isClearModeAlphaMask() || gstate.FrameBufFormat() == GE_FORMAT_565)) {
				framebufferManager_->ApplyClearToMemory(scissorX1, scissorY1, scissorX2, scissorY2, result.color);
			}
		}
	}

	gpuStats.numDrawCalls += numDrawCalls;
	gpuStats.numVertsSubmitted += vertexCountInDrawCalls_;

	indexGen.Reset();
	decodedVerts_ = 0;
	numDrawCalls = 0;
	vertexCountInDrawCalls_ = 0;
	decodeCounter_ = 0;
	dcid_ = 0;
	prevPrim_ = GE_PRIM_INVALID;
	gstate_c.vertexFullAlpha = true;
	framebufferManager_->SetColorUpdated(gstate_c.skipDrawReason);

	// Now seems as good a time as any to reset the min/max coords, which we may examine later.
	gstate_c.vertBounds.minU = 512;
	gstate_c.vertBounds.minV = 512;
	gstate_c.vertBounds.maxU = 0;
	gstate_c.vertBounds.maxV = 0;

	host->GPUNotifyDraw();
}

void DrawEngineVulkan::UpdateUBOs(FrameData *frame) {
	if ((dirtyUniforms_ & DIRTY_BASE_UNIFORMS) || baseBuf == VK_NULL_HANDLE) {
		baseUBOOffset = shaderManager_->PushBaseBuffer(frame->pushUBO, &baseBuf);
		dirtyUniforms_ &= ~DIRTY_BASE_UNIFORMS;
	}
	if ((dirtyUniforms_ & DIRTY_LIGHT_UNIFORMS) || lightBuf == VK_NULL_HANDLE) {
		lightUBOOffset = shaderManager_->PushLightBuffer(frame->pushUBO, &lightBuf);
		dirtyUniforms_ &= ~DIRTY_LIGHT_UNIFORMS;
	}
	if ((dirtyUniforms_ & DIRTY_BONE_UNIFORMS) || boneBuf == VK_NULL_HANDLE) {
		boneUBOOffset = shaderManager_->PushBoneBuffer(frame->pushUBO, &boneBuf);
		dirtyUniforms_ &= ~DIRTY_BONE_UNIFORMS;
	}
}

bool DrawEngineVulkan::IsCodePtrVertexDecoder(const u8 *ptr) const {
	return decJitCache_->IsInSpace(ptr);
}

void DrawEngineVulkan::TessellationDataTransferVulkan::PrepareBuffers(float *&pos, float *&tex, float *&col, int size, bool hasColor, bool hasTexCoords) {
	int rowPitch;

	// Position
	if (prevSize < size) {
		prevSize = size;

		data_tex[0]->CreateDirect(size, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}
	pos = (float *)data_tex[0]->Lock(0, &rowPitch);

	// Texcoords
	if (hasTexCoords) {
		if (prevSizeTex < size) {
			prevSizeTex = size;

			data_tex[1]->CreateDirect(size, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		}
		tex = (float *)data_tex[1]->Lock(0, &rowPitch);
	}

	// Color
	int sizeColor = hasColor ? size : 1;
	if (prevSizeCol < sizeColor) {
		prevSizeCol = sizeColor;

		data_tex[2]->CreateDirect(sizeColor, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}
	col = (float *)data_tex[2]->Lock(0, &rowPitch);
}

void DrawEngineVulkan::TessellationDataTransferVulkan::SendDataToShader(const float *pos, const float *tex, const float *col, int size, bool hasColor, bool hasTexCoords) {
	// Position
	data_tex[0]->Unlock();

	// Texcoords
	if (hasTexCoords)
		data_tex[1]->Unlock();

	// Color
	data_tex[2]->Unlock();
}
