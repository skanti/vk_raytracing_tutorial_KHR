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


#include "raytrace.hpp"
#include "nvh/fileoperations.hpp"
#include "nvvk/descriptorsets_vk.hpp"

#include "nvh/alignment.hpp"
#include "nvvk/shaders_vk.hpp"
#include "obj_loader.h"

extern std::vector<std::string> defaultSearchPaths;


void Raytracer::setup(const vk::Device&         device,
                      const vk::PhysicalDevice& physicalDevice,
                      nvvk::ResourceAllocator*  allocator,
                      uint32_t                  queueFamily)
{
  m_device             = device;
  m_physicalDevice     = physicalDevice;
  m_alloc              = allocator;
  m_graphicsQueueIndex = queueFamily;

  // Requesting ray tracing properties
  auto properties =
      m_physicalDevice.getProperties2<vk::PhysicalDeviceProperties2,
                                      vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  m_rtProperties = properties.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
  m_rtBuilder.setup(m_device, allocator, m_graphicsQueueIndex);

  m_sbtWrapper.setup(device, queueFamily, allocator, m_rtProperties);
  m_debug.setup(device);
}


void Raytracer::destroy()
{
  m_sbtWrapper.destroy();
  m_rtBuilder.destroy();
  m_device.destroy(m_rtDescPool);
  m_device.destroy(m_rtDescSetLayout);
  m_device.destroy(m_rtPipeline);
  m_device.destroy(m_rtPipelineLayout);
  m_alloc->destroy(m_rtSBTBuffer);
}

//--------------------------------------------------------------------------------------------------
// Converting a OBJ primitive to the ray tracing geometry used for the BLAS
//
auto Raytracer::objectToVkGeometryKHR(const ObjModel& model)
{
  // Building part
  vk::DeviceAddress vertexAddress = m_device.getBufferAddress({model.vertexBuffer.buffer});
  vk::DeviceAddress indexAddress  = m_device.getBufferAddress({model.indexBuffer.buffer});

  vk::AccelerationStructureGeometryTrianglesDataKHR triangles;
  triangles.setVertexFormat(vk::Format::eR32G32B32Sfloat);
  triangles.setVertexData(vertexAddress);
  triangles.setVertexStride(sizeof(VertexObj));
  triangles.setIndexType(vk::IndexType::eUint32);
  triangles.setIndexData(indexAddress);
  triangles.setTransformData({});
  triangles.setMaxVertex(model.nbVertices);

  // Setting up the build info of the acceleration
  vk::AccelerationStructureGeometryKHR asGeom;
  asGeom.setGeometryType(vk::GeometryTypeKHR::eTriangles);
  asGeom.setFlags(vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation);  // For AnyHit
  asGeom.geometry.setTriangles(triangles);


  vk::AccelerationStructureBuildRangeInfoKHR offset;
  offset.setFirstVertex(0);
  offset.setPrimitiveCount(model.nbIndices / 3);  // Nb triangles
  offset.setPrimitiveOffset(0);
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);
  return input;
}


//--------------------------------------------------------------------------------------------------
// Returning the ray tracing geometry used for the BLAS, containing all spheres
//
auto Raytracer::implicitToVkGeometryKHR(const ImplInst& implicitObj)
{
  vk::DeviceAddress dataAddress = m_device.getBufferAddress({implicitObj.implBuf.buffer});

  vk::AccelerationStructureGeometryAabbsDataKHR aabbs;
  aabbs.setData(dataAddress);
  aabbs.setStride(sizeof(ObjImplicit));

  // Setting up the build info of the acceleration
  VkAccelerationStructureGeometryKHR asGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  asGeom.geometryType   = VK_GEOMETRY_TYPE_AABBS_KHR;
  asGeom.flags          = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;  // For AnyHit
  asGeom.geometry.aabbs = aabbs;


  vk::AccelerationStructureBuildRangeInfoKHR offset;
  offset.setFirstVertex(0);
  offset.setPrimitiveCount(static_cast<uint32_t>(implicitObj.objImpl.size()));  // Nb aabb
  offset.setPrimitiveOffset(0);
  offset.setTransformOffset(0);

  nvvk::RaytracingBuilderKHR::BlasInput input;
  input.asGeometry.emplace_back(asGeom);
  input.asBuildOffsetInfo.emplace_back(offset);
  return input;
}


void Raytracer::createBottomLevelAS(std::vector<ObjModel>& models, ImplInst& implicitObj)
{
  // BLAS - Storing each primitive in a geometry
  std::vector<nvvk::RaytracingBuilderKHR::BlasInput> allBlas;
  allBlas.reserve(models.size());
  for(const auto& obj : models)
  {
    auto blas = objectToVkGeometryKHR(obj);

    // We could add more geometry in each BLAS, but we add only one for now
    allBlas.emplace_back(blas);
  }

  // Adding implicit
  if(!implicitObj.objImpl.empty())
  {
    auto blas = implicitToVkGeometryKHR(implicitObj);
    allBlas.emplace_back(blas);
    implicitObj.blasId = static_cast<int>(allBlas.size() - 1);  // remember blas ID for tlas
  }


  m_rtBuilder.buildBlas(allBlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace
                                     | vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction);
}

void Raytracer::createTopLevelAS(std::vector<ObjInstance>& instances, ImplInst& implicitObj)
{
  std::vector<nvvk::RaytracingBuilderKHR::Instance> tlas;
  tlas.reserve(instances.size());
  for(int i = 0; i < static_cast<int>(instances.size()); i++)
  {
    nvvk::RaytracingBuilderKHR::Instance rayInst;
    rayInst.transform        = instances[i].transform;  // Position of the instance
    rayInst.instanceCustomId = i;                       // gl_InstanceCustomIndexEXT
    rayInst.blasId           = instances[i].objIndex;
    rayInst.hitGroupId       = 0;  // We will use the same hit group for all objects
    rayInst.flags            = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    tlas.emplace_back(rayInst);
  }

  // Add the blas containing all implicit
  if(!implicitObj.objImpl.empty())
  {
    nvvk::RaytracingBuilderKHR::Instance rayInst;
    rayInst.transform = implicitObj.transform;  // Position of the instance
    rayInst.instanceCustomId =
        static_cast<uint32_t>(implicitObj.blasId);  // Same for material index
    rayInst.blasId     = static_cast<uint32_t>(implicitObj.blasId);
    rayInst.hitGroupId = 1;  // We will use the same hit group for all objects (the second one)
    rayInst.flags      = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    tlas.emplace_back(rayInst);
  }

  m_rtBuilder.buildTlas(tlas, vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace);
}

//--------------------------------------------------------------------------------------------------
// This descriptor set holds the Acceleration structure and the output image
//
void Raytracer::createRtDescriptorSet(const vk::ImageView& outputImage)
{
  using vkDT   = vk::DescriptorType;
  using vkSS   = vk::ShaderStageFlagBits;
  using vkDSLB = vk::DescriptorSetLayoutBinding;

  m_rtDescSetLayoutBind.addBinding(vkDSLB(0, vkDT::eAccelerationStructureKHR, 1,
                                          vkSS::eRaygenKHR | vkSS::eClosestHitKHR));  // TLAS
  m_rtDescSetLayoutBind.addBinding(
      vkDSLB(1, vkDT::eStorageImage, 1, vkSS::eRaygenKHR));  // Output image

  m_rtDescPool      = m_rtDescSetLayoutBind.createPool(m_device);
  m_rtDescSetLayout = m_rtDescSetLayoutBind.createLayout(m_device);
  m_rtDescSet       = m_device.allocateDescriptorSets({m_rtDescPool, 1, &m_rtDescSetLayout})[0];

  vk::AccelerationStructureKHR                   tlas = m_rtBuilder.getAccelerationStructure();
  vk::WriteDescriptorSetAccelerationStructureKHR descASInfo;
  descASInfo.setAccelerationStructureCount(1);
  descASInfo.setPAccelerationStructures(&tlas);
  vk::DescriptorImageInfo imageInfo{{}, outputImage, vk::ImageLayout::eGeneral};

  std::vector<vk::WriteDescriptorSet> writes;
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 0, &descASInfo));
  writes.emplace_back(m_rtDescSetLayoutBind.makeWrite(m_rtDescSet, 1, &imageInfo));
  m_device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Writes the output image to the descriptor set
// - Required when changing resolution
//
void Raytracer::updateRtDescriptorSet(const vk::ImageView& outputImage)
{
  using vkDT = vk::DescriptorType;

  // (1) Output buffer
  vk::DescriptorImageInfo imageInfo{{}, outputImage, vk::ImageLayout::eGeneral};
  vk::WriteDescriptorSet  wds{m_rtDescSet, 1, 0, 1, vkDT::eStorageImage, &imageInfo};
  m_device.updateDescriptorSets(wds, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Pipeline for the ray tracer: all shaders, raygen, chit, miss
//
void Raytracer::createRtPipeline(vk::DescriptorSetLayout& sceneDescLayout)
{
  vk::ShaderModule raygenSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace.rgen.spv", true, defaultSearchPaths, true));
  vk::ShaderModule missSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace.rmiss.spv", true, defaultSearchPaths, true));

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
  m_rtShaderGroups.push_back(rg);  // 0

  // Miss
  vk::RayTracingShaderGroupCreateInfoKHR mg{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  mg.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, missSM, "main"});
  m_rtShaderGroups.push_back(mg);  // 1
  // Shadow Miss
  mg.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eMissKHR, shadowmissSM, "main"});
  m_rtShaderGroups.push_back(mg);  // 2

  // Hit Group0 - Closest Hit + AnyHit
  vk::ShaderModule chitSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace.rchit.spv", true, defaultSearchPaths, true));
  vk::ShaderModule ahitSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace.rahit.spv", true, defaultSearchPaths, true));

  vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                            VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
  hg.setClosestHitShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chitSM, "main"});
  hg.setAnyHitShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eAnyHitKHR, ahitSM, "main"});
  m_rtShaderGroups.push_back(hg);  // 3


  // Hit Group1 - Closest Hit + Intersection (procedural)
  vk::ShaderModule chit2SM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace2.rchit.spv", true, defaultSearchPaths, true));
  vk::ShaderModule ahit2SM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace2.rahit.spv", true, defaultSearchPaths, true));
  vk::ShaderModule rintSM = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/raytrace.rint.spv", true, defaultSearchPaths, true));
  {
    vk::RayTracingShaderGroupCreateInfoKHR hg{vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                              VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    hg.setClosestHitShader(static_cast<uint32_t>(stages.size()));
    stages.push_back({{}, vk::ShaderStageFlagBits::eClosestHitKHR, chit2SM, "main"});
    hg.setAnyHitShader(static_cast<uint32_t>(stages.size()));
    stages.push_back({{}, vk::ShaderStageFlagBits::eAnyHitKHR, ahit2SM, "main"});
    hg.setIntersectionShader(static_cast<uint32_t>(stages.size()));
    stages.push_back({{}, vk::ShaderStageFlagBits::eIntersectionKHR, rintSM, "main"});
    m_rtShaderGroups.push_back(hg);  // 4
  }

  // Callable shaders
  vk::RayTracingShaderGroupCreateInfoKHR callGroup{vk::RayTracingShaderGroupTypeKHR::eGeneral,
                                                   VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR,
                                                   VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};

  vk::ShaderModule call0 = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/light_point.rcall.spv", true, defaultSearchPaths, true));
  vk::ShaderModule call1 = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/light_spot.rcall.spv", true, defaultSearchPaths, true));
  vk::ShaderModule call2 = nvvk::createShaderModule(
      m_device, nvh::loadFile("spv/light_inf.rcall.spv", true, defaultSearchPaths, true));

  callGroup.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eCallableKHR, call0, "main"});
  m_rtShaderGroups.push_back(callGroup);  // 5
  callGroup.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eCallableKHR, call1, "main"});
  m_rtShaderGroups.push_back(callGroup);  // 6
  callGroup.setGeneralShader(static_cast<uint32_t>(stages.size()));
  stages.push_back({{}, vk::ShaderStageFlagBits::eCallableKHR, call2, "main"});
  m_rtShaderGroups.push_back(callGroup);  //7


  vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;

  // Push constant: we want to be able to update constants used by the shaders
  vk::PushConstantRange pushConstant{vk::ShaderStageFlagBits::eRaygenKHR
                                         | vk::ShaderStageFlagBits::eClosestHitKHR
                                         | vk::ShaderStageFlagBits::eMissKHR
                                         | vk::ShaderStageFlagBits::eCallableKHR,
                                     0, sizeof(RtPushConstants)};
  pipelineLayoutCreateInfo.setPushConstantRangeCount(1);
  pipelineLayoutCreateInfo.setPPushConstantRanges(&pushConstant);

  // Descriptor sets: one specific to ray tracing, and one shared with the rasterization pipeline
  std::vector<vk::DescriptorSetLayout> rtDescSetLayouts = {m_rtDescSetLayout, sceneDescLayout};
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

  m_sbtWrapper.create(m_rtPipeline, rayPipelineInfo);

  m_device.destroy(raygenSM);
  m_device.destroy(missSM);
  m_device.destroy(shadowmissSM);
  m_device.destroy(chitSM);
  m_device.destroy(ahitSM);
  m_device.destroy(chit2SM);
  m_device.destroy(ahit2SM);
  m_device.destroy(rintSM);
  m_device.destroy(call0);
  m_device.destroy(call1);
  m_device.destroy(call2);
}

//--------------------------------------------------------------------------------------------------
// Ray Tracing the scene
//
void Raytracer::raytrace(const vk::CommandBuffer& cmdBuf,
                         const nvmath::vec4f&     clearColor,
                         vk::DescriptorSet&       sceneDescSet,
                         vk::Extent2D&            size,
                         ObjPushConstants&        sceneConstants)
{
  m_debug.beginLabel(cmdBuf, "Ray trace");
  // Initializing push constant values
  m_rtPushConstants.clearColor           = clearColor;
  m_rtPushConstants.lightPosition        = sceneConstants.lightPosition;
  m_rtPushConstants.lightIntensity       = sceneConstants.lightIntensity;
  m_rtPushConstants.lightDirection       = sceneConstants.lightDirection;
  m_rtPushConstants.lightSpotCutoff      = sceneConstants.lightSpotCutoff;
  m_rtPushConstants.lightSpotOuterCutoff = sceneConstants.lightSpotOuterCutoff;
  m_rtPushConstants.lightType            = sceneConstants.lightType;
  m_rtPushConstants.frame                = sceneConstants.frame;

  cmdBuf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipeline);
  cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, m_rtPipelineLayout, 0,
                            {m_rtDescSet, sceneDescSet}, {});
  cmdBuf.pushConstants<RtPushConstants>(m_rtPipelineLayout,
                                        vk::ShaderStageFlagBits::eRaygenKHR
                                            | vk::ShaderStageFlagBits::eClosestHitKHR
                                            | vk::ShaderStageFlagBits::eMissKHR
                                            | vk::ShaderStageFlagBits::eCallableKHR,
                                        0, m_rtPushConstants);


  auto regions = m_sbtWrapper.getRegions();
  cmdBuf.traceRaysKHR(regions[0], regions[1], regions[2], regions[3], size.width, size.height, 1);

  m_debug.endLabel(cmdBuf);
}
