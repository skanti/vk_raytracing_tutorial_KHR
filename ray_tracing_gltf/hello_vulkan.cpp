/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


#include <iostream>
#include <sstream>
#include <vulkan/vulkan.hpp>

extern std::vector<std::string> defaultSearchPaths;

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION


#include "hello_vulkan.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvh/gltfscene.hpp"
#include "nvh/nvprint.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvk/renderpasses_vk.hpp"
#include "nvvk/shaders_vk.hpp"

#include "nvh/alignment.hpp"
#include "shaders/binding.glsl"
#include "shaders/gltf.glsl"


// Holding the camera matrices
struct CameraMatrices
{
  nvmath::mat4f view;
  nvmath::mat4f proj;
  nvmath::mat4f viewInverse;
  // #VKRay
  nvmath::mat4f projInverse;
};

//--------------------------------------------------------------------------------------------------
// Keep the handle on the device
// Initialize the tool to do all our allocations: buffers, images
//
void HelloVulkan::setup(const vk::Instance&       instance,
                        const vk::Device&         device,
                        const vk::PhysicalDevice& physicalDevice,
                        uint32_t                  queueFamily)
{
  AppBase::setup(instance, device, physicalDevice, queueFamily);
  m_alloc.init(device, physicalDevice);
  m_debug.setup(m_device);
  m_offscreenDepthFormat = nvvk::findDepthFormat(physicalDevice);
}

//--------------------------------------------------------------------------------------------------
// Called at each frame to update the camera matrix
//
void HelloVulkan::updateUniformBuffer(const vk::CommandBuffer& cmdBuf)
{
  // Prepare new UBO contents on host.
  const float    aspectRatio = m_size.width / static_cast<float>(m_size.height);
  CameraMatrices hostUBO     = {};
  hostUBO.view               = CameraManip.getMatrix();
  hostUBO.proj = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.1f, 1000.0f);
  // hostUBO.proj[1][1] *= -1;  // Inverting Y for Vulkan (not needed with perspectiveVK).
  hostUBO.viewInverse = nvmath::invert(hostUBO.view);
  // #VKRay
  hostUBO.projInverse = nvmath::invert(hostUBO.proj);

  // UBO on the device, and what stages access it.
  vk::Buffer deviceUBO = m_cameraMat.buffer;
  auto       uboUsageStages =
      vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eRayTracingShaderKHR;

  // Ensure that the modified UBO is not visible to previous frames.
  vk::BufferMemoryBarrier beforeBarrier;
  beforeBarrier.setSrcAccessMask(vk::AccessFlagBits::eShaderRead);
  beforeBarrier.setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
  beforeBarrier.setBuffer(deviceUBO);
  beforeBarrier.setOffset(0);
  beforeBarrier.setSize(sizeof hostUBO);
  cmdBuf.pipelineBarrier(uboUsageStages, vk::PipelineStageFlagBits::eTransfer,
                         vk::DependencyFlagBits::eDeviceGroup, {}, {beforeBarrier}, {});

  // Schedule the host-to-device upload. (hostUBO is copied into the cmd
  // buffer so it is okay to deallocate when the function returns).
  cmdBuf.updateBuffer<CameraMatrices>(m_cameraMat.buffer, 0, hostUBO);

  // Making sure the updated UBO will be visible.
  vk::BufferMemoryBarrier afterBarrier;
  afterBarrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite);
  afterBarrier.setDstAccessMask(vk::AccessFlagBits::eShaderRead);
  afterBarrier.setBuffer(deviceUBO);
  afterBarrier.setOffset(0);
  afterBarrier.setSize(sizeof hostUBO);
  cmdBuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, uboUsageStages,
                         vk::DependencyFlagBits::eDeviceGroup, {}, {afterBarrier}, {});
}

//--------------------------------------------------------------------------------------------------
// Describing the layout pushed when rendering
//
void HelloVulkan::createDescriptorSetLayout()
{
  using vkDS = vk::DescriptorSetLayoutBinding;
  using vkDT = vk::DescriptorType;
  using vkSS = vk::ShaderStageFlagBits;

  auto& bind = m_descSetLayoutBind;
  // Camera matrices (binding = 0)
  bind.addBinding(vkDS(B_CAMERA, vkDT::eUniformBuffer, 1, vkSS::eVertex | vkSS::eRaygenKHR));
  bind.addBinding(
      vkDS(B_VERTICES, vkDT::eStorageBuffer, 1, vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  bind.addBinding(
      vkDS(B_INDICES, vkDT::eStorageBuffer, 1, vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  bind.addBinding(vkDS(B_NORMALS, vkDT::eStorageBuffer, 1, vkSS::eClosestHitKHR));
  bind.addBinding(vkDS(B_TEXCOORDS, vkDT::eStorageBuffer, 1, vkSS::eClosestHitKHR));
  bind.addBinding(vkDS(B_MATERIALS, vkDT::eStorageBuffer, 1,
                       vkSS::eFragment | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  bind.addBinding(vkDS(B_MATRICES, vkDT::eStorageBuffer, 1,
                       vkSS::eVertex | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));
  auto nbTextures = static_cast<uint32_t>(m_textures.size());
  bind.addBinding(vkDS(B_TEXTURES, vkDT::eCombinedImageSampler, nbTextures,
                       vkSS::eFragment | vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));


  m_descSetLayout = m_descSetLayoutBind.createLayout(m_device);
  m_descPool      = m_descSetLayoutBind.createPool(m_device, 1);
  m_descSet       = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Setting up the buffers in the descriptor set
//
void HelloVulkan::updateDescriptorSet()
{
  std::vector<vk::WriteDescriptorSet> writes;

  // Camera matrices and scene description
  vk::DescriptorBufferInfo dbiUnif{m_cameraMat.buffer, 0, VK_WHOLE_SIZE};
  vk::DescriptorBufferInfo vertexDesc{m_vertexBuffer.buffer, 0, VK_WHOLE_SIZE};
  vk::DescriptorBufferInfo indexDesc{m_indexBuffer.buffer, 0, VK_WHOLE_SIZE};
  vk::DescriptorBufferInfo normalDesc{m_normalBuffer.buffer, 0, VK_WHOLE_SIZE};
  vk::DescriptorBufferInfo uvDesc{m_uvBuffer.buffer, 0, VK_WHOLE_SIZE};
  vk::DescriptorBufferInfo materialDesc{m_materialBuffer.buffer, 0, VK_WHOLE_SIZE};
  vk::DescriptorBufferInfo matrixDesc{m_matrixBuffer.buffer, 0, VK_WHOLE_SIZE};

  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_CAMERA, &dbiUnif));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_VERTICES, &vertexDesc));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_INDICES, &indexDesc));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_NORMALS, &normalDesc));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_TEXCOORDS, &uvDesc));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_MATERIALS, &materialDesc));
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, B_MATRICES, &matrixDesc));

  // All texture samplers
  std::vector<vk::DescriptorImageInfo> diit;
  for(auto& texture : m_textures)
    diit.emplace_back(texture.descriptor);
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, B_TEXTURES, diit.data()));

  // Writing the information
  m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Creating the pipeline layout
//
void HelloVulkan::createGraphicsPipeline()
{
  using vkSS = vk::ShaderStageFlagBits;

  vk::PushConstantRange pushConstantRanges = {vkSS::eVertex | vkSS::eFragment, 0,
                                              sizeof(ObjPushConstant)};

  // Creating the Pipeline Layout
  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
  vk::DescriptorSetLayout      descSetLayout(m_descSetLayout);
  pipelineLayoutCreateInfo.setSetLayoutCount(1);
  pipelineLayoutCreateInfo.setPSetLayouts(&descSetLayout);
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstantRanges);
  m_pipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Creating the Pipeline
  std::vector<std::string>                paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_pipelineLayout, m_offscreenRenderPass);
  gpb.depthStencilState.depthTestEnable = true;
  gpb.addShader(nvh::loadFile("spv/vert_shader.vert.spv", true, paths, true), vkSS::eVertex);
  gpb.addShader(nvh::loadFile("spv/frag_shader.frag.spv", true, paths, true), vkSS::eFragment);
  gpb.addBindingDescriptions(
      {{0, sizeof(nvmath::vec3)}, {1, sizeof(nvmath::vec3)}, {2, sizeof(nvmath::vec2)}});
  gpb.addAttributeDescriptions({
      {0, 0, vk::Format::eR32G32B32Sfloat, 0},  // Position
      {1, 1, vk::Format::eR32G32B32Sfloat, 0},  // Normal
      {2, 2, vk::Format::eR32G32Sfloat, 0},     // Texcoord0
  });
  m_graphicsPipeline = gpb.createPipeline();
  m_debug.setObjectName(m_graphicsPipeline, "Graphics");
}

//--------------------------------------------------------------------------------------------------
// Loading the OBJ file and setting up all buffers
//
void HelloVulkan::loadScene(const std::string& filename)
{
  using vkBU = vk::BufferUsageFlagBits;
  tinygltf::Model    tmodel;
  tinygltf::TinyGLTF tcontext;
  std::string        warn, error;

  LOGI("Loading file: %s", filename.c_str());
  if(!tcontext.LoadASCIIFromFile(&tmodel, &error, &warn, filename))
  {
    assert(!"Error while loading scene");
  }
  LOGW(warn.c_str());
  LOGE(error.c_str());


  m_gltfScene.importMaterials(tmodel);
  m_gltfScene.importDrawableNodes(tmodel,
                                  nvh::GltfAttributes::Normal | nvh::GltfAttributes::Texcoord_0);

  // Create the buffers on Device and copy vertices, indices and materials
  nvvk::CommandPool cmdBufGet(m_device, m_graphicsQueueIndex);
  vk::CommandBuffer cmdBuf = cmdBufGet.createCommandBuffer();

  m_vertexBuffer =
      m_alloc.createBuffer(cmdBuf, m_gltfScene.m_positions,
                           vkBU::eVertexBuffer | vkBU::eStorageBuffer | vkBU::eShaderDeviceAddress
                               | vkBU::eAccelerationStructureBuildInputReadOnlyKHR);
  m_indexBuffer =
      m_alloc.createBuffer(cmdBuf, m_gltfScene.m_indices,
                           vkBU::eIndexBuffer | vkBU::eStorageBuffer | vkBU::eShaderDeviceAddress
                               | vkBU::eAccelerationStructureBuildInputReadOnlyKHR);
  m_normalBuffer = m_alloc.createBuffer(cmdBuf, m_gltfScene.m_normals,
                                        vkBU::eVertexBuffer | vkBU::eStorageBuffer);
  m_uvBuffer     = m_alloc.createBuffer(cmdBuf, m_gltfScene.m_texcoords0,
                                    vkBU::eVertexBuffer | vkBU::eStorageBuffer);

  // Copying all materials, only the elements we need
  std::vector<GltfShadeMaterial> shadeMaterials;
  for(auto& m : m_gltfScene.m_materials)
  {
    shadeMaterials.emplace_back(
        GltfShadeMaterial{m.baseColorFactor, m.baseColorTexture, m.emissiveFactor});
  }
  m_materialBuffer = m_alloc.createBuffer(cmdBuf, shadeMaterials, vkBU::eStorageBuffer);

  // Instance Matrices used by rasterizer
  std::vector<nvmath::mat4f> nodeMatrices;
  for(auto& node : m_gltfScene.m_nodes)
  {
    nodeMatrices.emplace_back(node.worldMatrix);
  }
  m_matrixBuffer = m_alloc.createBuffer(cmdBuf, nodeMatrices, vkBU::eStorageBuffer);

  // The following is used to find the primitive mesh information in the CHIT
  std::vector<RtPrimitiveLookup> primLookup;
  for(auto& primMesh : m_gltfScene.m_primMeshes)
  {
    primLookup.push_back({primMesh.firstIndex, primMesh.vertexOffset, primMesh.materialIndex});
  }
  m_rtPrimLookup =
      m_alloc.createBuffer(cmdBuf, primLookup, vk::BufferUsageFlagBits::eStorageBuffer);


  // Creates all textures found
  createTextureImages(cmdBuf, tmodel);
  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();

  m_debug.setObjectName(m_vertexBuffer.buffer, "Vertex");
  m_debug.setObjectName(m_indexBuffer.buffer, "Index");
  m_debug.setObjectName(m_normalBuffer.buffer, "Normal");
  m_debug.setObjectName(m_uvBuffer.buffer, "TexCoord");
  m_debug.setObjectName(m_materialBuffer.buffer, "Material");
  m_debug.setObjectName(m_matrixBuffer.buffer, "Matrix");
}


//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the camera matrices
// - Buffer is host visible
//
void HelloVulkan::createUniformBuffer()
{
  using vkBU = vk::BufferUsageFlagBits;
  using vkMP = vk::MemoryPropertyFlagBits;

  m_cameraMat = m_alloc.createBuffer(sizeof(CameraMatrices),
                                     vkBU::eUniformBuffer | vkBU::eTransferDst, vkMP::eDeviceLocal);
  m_debug.setObjectName(m_cameraMat.buffer, "cameraMat");
}

//--------------------------------------------------------------------------------------------------
// Creating all textures and samplers
//
void HelloVulkan::createTextureImages(const vk::CommandBuffer& cmdBuf, tinygltf::Model& gltfModel)
{
  using vkIU = vk::ImageUsageFlagBits;

  vk::SamplerCreateInfo samplerCreateInfo{
      {}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear};
  samplerCreateInfo.setMaxLod(FLT_MAX);
  vk::Format format = vk::Format::eR8G8B8A8Srgb;

  auto addDefaultTexture = [this]() {
    // Make dummy image(1,1), needed as we cannot have an empty array
    nvvk::ScopeCommandBuffer cmdBuf(m_device, m_graphicsQueueIndex);
    std::array<uint8_t, 4>   white = {255, 255, 255, 255};
    m_textures.emplace_back(m_alloc.createTexture(
        cmdBuf, 4, white.data(), nvvk::makeImage2DCreateInfo(vk::Extent2D{1, 1}), {}));
    m_debug.setObjectName(m_textures.back().image, "dummy");
  };

  if(gltfModel.images.empty())
  {
    addDefaultTexture();
    return;
  }

  m_textures.reserve(gltfModel.images.size());
  for(size_t i = 0; i < gltfModel.images.size(); i++)
  {
    auto&        gltfimage  = gltfModel.images[i];
    void*        buffer     = &gltfimage.image[0];
    VkDeviceSize bufferSize = gltfimage.image.size();
    auto         imgSize    = vk::Extent2D(gltfimage.width, gltfimage.height);

    if(bufferSize == 0 || gltfimage.width == -1 || gltfimage.height == -1)
    {
      addDefaultTexture();
      continue;
    }

    vk::ImageCreateInfo imageCreateInfo =
        nvvk::makeImage2DCreateInfo(imgSize, format, vkIU::eSampled, true);

    nvvk::Image image = m_alloc.createImage(cmdBuf, bufferSize, buffer, imageCreateInfo);
    nvvk::cmdGenerateMipmaps(cmdBuf, image.image, format, imgSize, imageCreateInfo.mipLevels);
    vk::ImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
    m_textures.emplace_back(m_alloc.createTexture(image, ivInfo, samplerCreateInfo));

    m_debug.setObjectName(m_textures[i].image, std::string("Txt" + std::to_string(i)).c_str());
  }
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void HelloVulkan::destroyResources()
{
  m_device.destroy(m_graphicsPipeline);
  m_device.destroy(m_pipelineLayout);
  m_device.destroy(m_descPool);
  m_device.destroy(m_descSetLayout);
  m_alloc.destroy(m_cameraMat);

  m_alloc.destroy(m_vertexBuffer);
  m_alloc.destroy(m_normalBuffer);
  m_alloc.destroy(m_uvBuffer);
  m_alloc.destroy(m_indexBuffer);
  m_alloc.destroy(m_materialBuffer);
  m_alloc.destroy(m_matrixBuffer);
  m_alloc.destroy(m_rtPrimLookup);

  for(auto& t : m_textures)
  {
    m_alloc.destroy(t);
  }

  //#Post
  m_device.destroy(m_postPipeline);
  m_device.destroy(m_postPipelineLayout);
  m_device.destroy(m_postDescPool);
  m_device.destroy(m_postDescSetLayout);
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_image_copy);
  m_alloc.destroy(m_offscreenDepth);
  m_device.destroy(m_offscreenRenderPass);
  m_device.destroy(m_offscreenFramebuffer);

  // #VKRay
  m_rtBuilder.destroy();
  m_sbtWrapper.destroy();
  m_device.destroy(m_rtDescPool);
  m_device.destroy(m_rtDescSetLayout);
  m_device.destroy(m_rtPipeline);
  m_device.destroy(m_rtPipelineLayout);

  m_alloc.deinit();
}

//--------------------------------------------------------------------------------------------------
// Drawing the scene in raster mode
//
void HelloVulkan::rasterize(const vk::CommandBuffer& cmdBuf)
{
  using vkPBP = vk::PipelineBindPoint;

  std::vector<vk::DeviceSize> offsets = {0, 0, 0};

  m_debug.beginLabel(cmdBuf, "Rasterize");

  // Dynamic Viewport
  cmdBuf.setViewport(0, {vk::Viewport(0, 0, (float)m_size.width, (float)m_size.height, 0, 1)});
  cmdBuf.setScissor(0, {{{0, 0}, {m_size.width, m_size.height}}});

  // Drawing all triangles
  cmdBuf.bindPipeline(vkPBP::eGraphics, m_graphicsPipeline);
  cmdBuf.bindDescriptorSets(vkPBP::eGraphics, m_pipelineLayout, 0, {m_descSet}, {});
  std::vector<vk::Buffer> vertexBuffers = {m_vertexBuffer.buffer, m_normalBuffer.buffer,
                                           m_uvBuffer.buffer};
  cmdBuf.bindVertexBuffers(0, static_cast<uint32_t>(vertexBuffers.size()), vertexBuffers.data(),
                           offsets.data());
  cmdBuf.bindIndexBuffer(m_indexBuffer.buffer, 0, vk::IndexType::eUint32);

  uint32_t idxNode = 0;
  for(auto& node : m_gltfScene.m_nodes)
  {
    auto& primitive = m_gltfScene.m_primMeshes[node.primMesh];

    m_pushConstant.instanceId = idxNode++;
    m_pushConstant.materialId = primitive.materialIndex;
    cmdBuf.pushConstants<ObjPushConstant>(
        m_pipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
        m_pushConstant);
    cmdBuf.drawIndexed(primitive.indexCount, 1, primitive.firstIndex, primitive.vertexOffset, 0);
  }

  m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void HelloVulkan::onResize(int /*w*/, int /*h*/)
{
  createOffscreenRender();
  updatePostDescriptorSet();
  updateRtDescriptorSet();
  resetFrame();
}

//////////////////////////////////////////////////////////////////////////
// Post-processing
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Creating an offscreen frame buffer and the associated render pass
//
void HelloVulkan::createOffscreenRender()
{
  m_alloc.destroy(m_offscreenColor);
  m_alloc.destroy(m_image_copy);
  m_alloc.destroy(m_offscreenDepth);

  // Creating the color image
  {
    auto colorCreateInfo = nvvk::makeImage2DCreateInfo(m_size, m_offscreenColorFormat,
                                                       vk::ImageUsageFlagBits::eColorAttachment
                                                           | vk::ImageUsageFlagBits::eSampled
                                                           | vk::ImageUsageFlagBits::eStorage);


    nvvk::Image             image  = m_alloc.createImage(colorCreateInfo);
    vk::ImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, colorCreateInfo);
    m_offscreenColor               = m_alloc.createTexture(image, ivInfo, vk::SamplerCreateInfo());
    m_offscreenColor.descriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // linear image for copying
    VkImageCreateInfo imageCreateInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCreateInfo.imageType         = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageCreateInfo.extent = {m_size.width, m_size.height, 1};
    imageCreateInfo.mipLevels   = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.tiling            = VK_IMAGE_TILING_LINEAR;
    imageCreateInfo.usage             = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.usage             = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    m_image_copy = m_alloc.createImage(imageCreateInfo, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                            | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                            | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  }

  // Creating the depth buffer
  auto depthCreateInfo =
      nvvk::makeImage2DCreateInfo(m_size, m_offscreenDepthFormat,
                                  vk::ImageUsageFlagBits::eDepthStencilAttachment);
  {
    nvvk::Image image = m_alloc.createImage(depthCreateInfo);

    vk::ImageViewCreateInfo depthStencilView;
    depthStencilView.setViewType(vk::ImageViewType::e2D);
    depthStencilView.setFormat(m_offscreenDepthFormat);
    depthStencilView.setSubresourceRange({vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1});
    depthStencilView.setImage(image.image);

    m_offscreenDepth = m_alloc.createTexture(image, depthStencilView);
  }

  // Setting the image layout for both color and depth
  {
    nvvk::CommandPool genCmdBuf(m_device, m_graphicsQueueIndex);
    auto              cmdBuf = genCmdBuf.createCommandBuffer();
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenColor.image, vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eGeneral);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_offscreenDepth.image, vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eDepthStencilAttachmentOptimal,
                                vk::ImageAspectFlagBits::eDepth);
    nvvk::cmdBarrierImageLayout(cmdBuf, m_image_copy.image, vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eTransferDstOptimal);

    genCmdBuf.submitAndWait(cmdBuf);
  }

  // Creating a renderpass for the offscreen
  if(!m_offscreenRenderPass)
  {
    m_offscreenRenderPass =
        nvvk::createRenderPass(m_device, {m_offscreenColorFormat}, m_offscreenDepthFormat, 1, true,
                               true, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral);
  }

  // Creating the frame buffer for offscreen
  std::vector<vk::ImageView> attachments = {m_offscreenColor.descriptor.imageView,
                                            m_offscreenDepth.descriptor.imageView};

  m_device.destroy(m_offscreenFramebuffer);
  vk::FramebufferCreateInfo info;
  info.setRenderPass(m_offscreenRenderPass);
  info.setAttachmentCount(2);
  info.setPAttachments(attachments.data());
  info.setWidth(m_size.width);
  info.setHeight(m_size.height);
  info.setLayers(1);
  m_offscreenFramebuffer = m_device.createFramebuffer(info);
}

//--------------------------------------------------------------------------------------------------
// The pipeline is how things are rendered, which shaders, type of primitives, depth test and more
//
void HelloVulkan::createPostPipeline()
{
  // Push constants in the fragment shader
  vk::PushConstantRange pushConstantRanges = {vk::ShaderStageFlagBits::eFragment, 0, sizeof(float)};

  // Creating the pipeline layout
  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
  pipelineLayoutCreateInfo.setSetLayoutCount(1);
  pipelineLayoutCreateInfo.setPSetLayouts(&m_postDescSetLayout);
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstantRanges);
  m_postPipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Pipeline: completely generic, no vertices
  nvvk::GraphicsPipelineGeneratorCombined pipelineGenerator(m_device, m_postPipelineLayout,
                                                            m_renderPass);
  pipelineGenerator.addShader(nvh::loadFile("spv/passthrough.vert.spv", true, defaultSearchPaths,
                                            true),
                              vk::ShaderStageFlagBits::eVertex);
  pipelineGenerator.addShader(nvh::loadFile("spv/post.frag.spv", true, defaultSearchPaths, true),
                              vk::ShaderStageFlagBits::eFragment);
  pipelineGenerator.rasterizationState.setCullMode(vk::CullModeFlagBits::eNone);
  m_postPipeline = pipelineGenerator.createPipeline();
  m_debug.setObjectName(m_postPipeline, "post");
}

//--------------------------------------------------------------------------------------------------
// The descriptor layout is the description of the data that is passed to the vertex or the
// fragment program.
//
void HelloVulkan::createPostDescriptor()
{
  using vkDS = vk::DescriptorSetLayoutBinding;
  using vkDT = vk::DescriptorType;
  using vkSS = vk::ShaderStageFlagBits;

  m_postDescSetLayoutBind.addBinding(vkDS(0, vkDT::eCombinedImageSampler, 1, vkSS::eFragment));
  m_postDescSetLayout = m_postDescSetLayoutBind.createLayout(m_device);
  m_postDescPool      = m_postDescSetLayoutBind.createPool(m_device);
  m_postDescSet       = nvvk::allocateDescriptorSet(m_device, m_postDescPool, m_postDescSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Update the output
//
void HelloVulkan::updatePostDescriptorSet()
{
  vk::WriteDescriptorSet writeDescriptorSets =
      m_postDescSetLayoutBind.makeWrite(m_postDescSet, 0, &m_offscreenColor.descriptor);
  m_device.updateDescriptorSets(writeDescriptorSets, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Draw a full screen quad with the attached image
//
void HelloVulkan::drawPost(vk::CommandBuffer cmdBuf)
{
  m_debug.beginLabel(cmdBuf, "Post");

  cmdBuf.setViewport(0, {vk::Viewport(0, 0, (float)m_size.width, (float)m_size.height, 0, 1)});
  cmdBuf.setScissor(0, {{{0, 0}, {m_size.width, m_size.height}}});

  auto aspectRatio = static_cast<float>(m_size.width) / static_cast<float>(m_size.height);
  cmdBuf.pushConstants<float>(m_postPipelineLayout, vk::ShaderStageFlagBits::eFragment, 0,
                              aspectRatio);
  cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, m_postPipeline);
  cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_postPipelineLayout, 0,
                            m_postDescSet, {});
  cmdBuf.draw(3, 1, 0, 0);

  m_debug.endLabel(cmdBuf);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------------------------------------
// Initialize Vulkan ray tracing
// #VKRay
void HelloVulkan::initRayTracing()
{
  // Requesting ray tracing properties
  auto properties =
      m_physicalDevice.getProperties2<vk::PhysicalDeviceProperties2,
                                      vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  m_rtProperties = properties.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  m_rtBuilder.setup(m_device, &m_alloc, m_graphicsQueueIndex);
  m_sbtWrapper.setup(m_device, m_graphicsQueueIndex, &m_alloc, m_rtProperties);
}

//--------------------------------------------------------------------------------------------------
// Converting a GLTF primitive in the Raytracing Geometry used for the BLAS
//
auto HelloVulkan::primitiveToGeometry(const nvh::GltfPrimMesh& prim)
{
  // Building part
  vk::DeviceAddress vertexAddress = m_device.getBufferAddress({m_vertexBuffer.buffer});
  vk::DeviceAddress indexAddress  = m_device.getBufferAddress({m_indexBuffer.buffer});

  vk::AccelerationStructureGeometryTrianglesDataKHR triangles;
  triangles.setVertexFormat(vk::Format::eR32G32B32Sfloat);
  triangles.setVertexData(vertexAddress);
  triangles.setVertexStride(sizeof(nvmath::vec3f));
  triangles.setIndexType(vk::IndexType::eUint32);
  triangles.setIndexData(indexAddress);
  triangles.setTransformData({});
  triangles.setMaxVertex(prim.vertexCount);

  // Setting up the build info of the acceleration
  vk::AccelerationStructureGeometryKHR asGeom;
  asGeom.setGeometryType(vk::GeometryTypeKHR::eTriangles);
  asGeom.setFlags(vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);  // For AnyHit
  asGeom.geometry.setTriangles(triangles);

  vk::AccelerationStructureBuildRangeInfoKHR offset;
  offset.setFirstVertex(prim.vertexOffset);
  offset.setPrimitiveCount(prim.indexCount / 3);
  offset.setPrimitiveOffset(prim.firstIndex * sizeof(uint32_t));
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);
  return input;
}

//--------------------------------------------------------------------------------------------------
//
//
void HelloVulkan::createBottomLevelAS()
{
  // BLAS - Storing each primitive in a geometry
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
  allBlas.reserve(m_gltfScene.m_primMeshes.size());
  for(auto& primMesh : m_gltfScene.m_primMeshes)
  {
    auto geo = primitiveToGeometry(primMesh);
    allBlas.push_back({geo});
  }
  m_rtBuilder.buildBlas(allBlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace);
}

void HelloVulkan::createTopLevelAS()
{
  std::vector<nvvk::RaytracingBuilderKHR::Instance> tlas;
  tlas.reserve(m_gltfScene.m_nodes.size());
  for(auto& node : m_gltfScene.m_nodes)
  {
    nvvk::RaytracingBuilderKHR::Instance rayInst;
    rayInst.transform        = node.worldMatrix;
    rayInst.instanceCustomId = node.primMesh;  // gl_InstanceCustomIndexEXT: to find which primitive
    rayInst.blasId           = node.primMesh;
    rayInst.flags            = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    rayInst.hitGroupId       = 0;  // We will use the same hit group for all objects
    tlas.emplace_back(rayInst);
  }
  m_rtBuilder.buildTlas(tlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace);
}

//--------------------------------------------------------------------------------------------------
// This descriptor set holds the Acceleration structure and the output image
//
void HelloVulkan::createRtDescriptorSet()
{
  using vkDT   = vk::DescriptorType;
  using vkSS   = vk::ShaderStageFlagBits;
  using vkDSLB = vk::DescriptorSetLayoutBinding;

  m_rtDescSetLayoutBind.addBinding(vkDSLB(0, vkDT::eAccelerationStructureKHR, 1,
                                          vkSS::eRaygenKHR | vkSS::eClosestHitKHR));  // TLAS
  m_rtDescSetLayoutBind.addBinding(
      vkDSLB(1, vkDT::eStorageImage, 1, vkSS::eRaygenKHR));  // Output image
  m_rtDescSetLayoutBind.addBinding(vkDSLB(
      2, vkDT::eStorageBuffer, 1, vkSS::eClosestHitKHR | vkSS::eAnyHitKHR));  // Primitive info

  m_rtDescPool      = m_rtDescSetLayoutBind.createPool(m_device);
  m_rtDescSetLayout = m_rtDescSetLayoutBind.createLayout(m_device);
  m_rtDescSet       = m_device.allocateDescriptorSets({m_rtDescPool, 1, &m_rtDescSetLayout})[0];

  vk::AccelerationStructureKHR                   tlas = m_rtBuilder.getAccelerationStructure();
  vk::WriteDescriptorSetAccelerationStructureKHR descASInfo;
  descASInfo.setAccelerationStructureCount(1);
  descASInfo.setPAccelerationStructures(&tlas);
  vk::DescriptorImageInfo imageInfo{
      {}, m_offscreenColor.descriptor.imageView, vk::ImageLayout::eGeneral};
  vk::DescriptorBufferInfo primitiveInfoDesc{m_rtPrimLookup.buffer, 0, VK_WHOLE_SIZE};

  std::vector<vk::WriteDescriptorSet> writes;
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 0, &descASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 1, &imageInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 2, &primitiveInfoDesc));
  m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Writes the output image to the descriptor set
// - Required when changing resolution
//
void HelloVulkan::updateRtDescriptorSet()
{
  using vkDT = vk::DescriptorType;

  // (1) Output buffer
  vk::DescriptorImageInfo imageInfo{
      {}, m_offscreenColor.descriptor.imageView, vk::ImageLayout::eGeneral};
  vk::WriteDescriptorSet wds{m_rtDescSet, 1, 0, 1, vkDT::eStorageImage, &imageInfo};
  m_device.updateDescriptorSets(wds, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Pipeline for the ray tracer: all shaders, raygen, chit, miss
//
void HelloVulkan::createRtPipeline()
{
  vk::ShaderModule raygenSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/pathtrace.rgen.spv", true, defaultSearchPaths, true));
  vk::ShaderModule missSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/pathtrace.rmiss.spv", true, defaultSearchPaths, true));

  // The second miss shader is invoked when a shadow ray misses the geometry. It
  // simply indicates that no occlusion has been found
  vk::ShaderModule shadowmissSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytraceShadow.rmiss.spv", true, defaultSearchPaths, true));


  std::vector<vk::PipelineShaderStageCreateInfo> stages;

  // Raygen
  vk::RayTracingShaderGroupCreateInfoKHR rg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  rg.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eRaygenKHR, raygenSM, "main"});
  m_rtShaderGroups.push_back(rg);
  // Miss
  vk::RayTracingShaderGroupCreateInfoKHR mg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  mg.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, missSM, "main"});
  m_rtShaderGroups.push_back(mg);
  // Shadow Miss
  mg.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, shadowmissSM, "main"});
  m_rtShaderGroups.push_back(mg);

  // Hit Group - Closest Hit + AnyHit
  vk::ShaderModule chitSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/pathtrace.rchit.spv", true, defaultSearchPaths, true));

  vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  hg.setClosestHitShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chitSM, "main"});
  m_rtShaderGroups.push_back(hg);

  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;

  // Push constant: we want to be able to update constants used by the shaders
  vk::PushConstantRange pushConstant{vk::ShaderStageFlagBits::eRaygenKHR
                                         | vk::ShaderStageFlagBits::eClosestHitKHR
                                         | vk::ShaderStageFlagBits::eMissKHR,
                                     0, sizeof(RtPushConstant)};
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstant);

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<vk::DescriptorSetLayout> rtDescSetLayouts = {m_rtDescSetLayout, m_descSetLayout};
  pipelineLayoutCreateInfo.setSetLayoutCount(static_cast<uint32_t>(rtDescSetLayouts.size()));
  pipelineLayoutCreateInfo.setPSetLayouts(rtDescSetLayouts.data());

  m_rtPipelineLayout = m_device.createPipelineLayout(pipelineLayoutCreateInfo);

  // Assemble the shader stages and recursion depth info into the ray tracing pipeline

  vk::RayTracingPipelineCreateInfoKHR rayPipelineInfo;
  rayPipelineInfo.setStageCount(static_cast<uint32_t>(stages.size()));  // Stages are shaders
  rayPipelineInfo.setPStages(stages.data());

  rayPipelineInfo.setGroupCount(static_cast<uint32_t>(
      m_rtShaderGroups.size()));  // 1-raygen, n-miss, n-(hit[+anyhit+intersect])
  rayPipelineInfo.setPGroups(m_rtShaderGroups.data());

  rayPipelineInfo.setMaxPipelineRayRecursionDepth(2);  // Ray depth
  rayPipelineInfo.setLayout(m_rtPipelineLayout);

  m_rtPipeline = m_device.createRayTracingPipelineKHR({}, {}, rayPipelineInfo).value;



  // Creating the SBT
  m_sbtWrapper.create(m_rtPipeline, rayPipelineInfo);


  m_device.destroy(raygenSM);
  m_device.destroy(missSM);
  m_device.destroy(shadowmissSM);
  m_device.destroy(chitSM);
}

//--------------------------------------------------------------------------------------------------
// Ray Tracing the scene
//
void HelloVulkan::raytrace(const vk::CommandBuffer& cmdBuf, const nvmath::vec4f& clearColor)
{
  updateFrame();

  m_debug.beginLabel(cmdBuf, "Ray trace");
  // Initializing push constant values
  m_rtPushConstants.clearColor     = clearColor;
  m_rtPushConstants.lightPosition  = m_pushConstant.lightPosition;
  m_rtPushConstants.lightIntensity = m_pushConstant.lightIntensity;
  m_rtPushConstants.lightType      = m_pushConstant.lightType;

  cmdBuf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipeline);
  cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipelineLayout, 0,
                            {m_rtDescSet, m_descSet}, {});
  cmdBuf.pushConstants<RtPushConstant>(m_rtPipelineLayout,
                                       vk::ShaderStageFlagBits::eRaygenKHR
                                           | vk::ShaderStageFlagBits::eClosestHitKHR
                                           | vk::ShaderStageFlagBits::eMissKHR,
                                       0, m_rtPushConstants);


  auto regions = m_sbtWrapper.getRegions();
  cmdBuf.traceRaysKHR(regions[0], regions[1], regions[2], regions[3],  //
                      m_size.width, m_size.height, 1);


  m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// If the camera matrix has changed, resets the frame.
// otherwise, increments frame.
//
void HelloVulkan::updateFrame()
{
  static nvmath::mat4f refCamMatrix;
  static float         refFov{CameraManip.getFov()};

  const auto& m   = CameraManip.getMatrix();
  const auto  fov = CameraManip.getFov();

  if(memcmp(&refCamMatrix.a00, &m.a00, sizeof(nvmath::mat4f)) != 0 || refFov != fov)
  {
    resetFrame();
    refCamMatrix = m;
    refFov       = fov;
  }
  m_rtPushConstants.frame++;
}

void HelloVulkan::resetFrame()
{
  m_rtPushConstants.frame = -1;
}

void HelloVulkan::snapshot(const vk::CommandBuffer& cmdBuf)
{
  {
    VkImageCopy region;
    region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount     = 1;
    region.srcSubresource.mipLevel       = 0;
    region.srcOffset      = {0, 0, 0};
    region.dstSubresource = region.srcSubresource;
    region.dstOffset      = {0, 0, 0};
    region.extent = {m_size.width, m_size.height, 1};
    vkCmdCopyImage(cmdBuf, m_offscreenColor.image, VK_IMAGE_LAYOUT_GENERAL, m_image_copy.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }

  const VkAccessFlags        srcAccesses = VK_ACCESS_TRANSFER_WRITE_BIT;
  const VkAccessFlags        dstAccesses = VK_ACCESS_HOST_READ_BIT;
  const VkPipelineStageFlags srcStages   = nvvk::makeAccessMaskPipelineStageFlags(srcAccesses);
  const VkPipelineStageFlags dstStages   = nvvk::makeAccessMaskPipelineStageFlags(dstAccesses);
  const VkImageMemoryBarrier barrier =
      nvvk::makeImageMemoryBarrier(m_image_copy.image, srcAccesses,
                                   dstAccesses,  // Src and dst access masks
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,  // Src and dst layouts
                                   VK_IMAGE_ASPECT_COLOR_BIT);
  vkCmdPipelineBarrier(cmdBuf,                // Command buffer
                       srcStages, dstStages,  // Src and dst pipeline stages
                       0,                     // Dependency flags
                       0, nullptr,            // Global memory barriers
                       0, nullptr,            // Buffer memory barriers
                       1, &barrier);          // Image memory barriers
}

std::vector<uint8_t> HelloVulkan::upload_image()
{
  // Get layout of the image (including row pitch)
  VkImageSubresource  subResource{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
  VkSubresourceLayout subResourceLayout;
  vkGetImageSubresourceLayout(m_device, m_image_copy.image, &subResource, &subResourceLayout);

  using dtype = float;
  // Map image memory so we can start copying from it
  const dtype* imagedata = (dtype*)m_alloc.map(m_image_copy);
  imagedata += subResourceLayout.offset;

  // copy from memory pointer
  std::vector<uint8_t> img(m_size.height * m_size.width * 4);

  const int pitch = subResourceLayout.rowPitch / sizeof(dtype);
  std::cout << "pitch: " << pitch << std::endl;
  dtype* row = (dtype*)imagedata;
  for(uint32_t y = 0; y < m_size.height; y++)
  {
    const int offset = y * pitch;
    std::transform(row, row + pitch, img.begin() + offset,
                   [](dtype v) { return static_cast<uint8_t>(v * 255.0f); });
    //std::memcpy(img.data() + y * subResourceLayout.rowPitch, row, subResourceLayout.rowPitch);
    row += pitch;
  }
  m_alloc.unmap(m_image_copy);

  return img;
}