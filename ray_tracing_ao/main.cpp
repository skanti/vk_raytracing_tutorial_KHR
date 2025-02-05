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


// ImGui - standalone example application for Glfw + Vulkan, using programmable
// pipeline If you are new to ImGui, see examples/README.txt and documentation
// at the top of imgui.cpp.

#include <array>
#include <iostream>

#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "backends/imgui_impl_glfw.h"
#include "imgui.h"

#include "hello_vulkan.h"
#include "imgui/imgui_camera_widget.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvpsystem.hpp"
#include "nvvk/appbase_vkpp.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/context_vk.hpp"


//////////////////////////////////////////////////////////////////////////
#define UNUSED(x) (void)(x)
//////////////////////////////////////////////////////////////////////////

// Default search path for shaders
std::vector<std::string> defaultSearchPaths;

// GLFW Callback functions
static void onErrorCallback(int error, const char* description)
{
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Extra UI
void renderUI(HelloVulkan& helloVk)
{
  ImGuiH::CameraWidget();
  if(ImGui::CollapsingHeader("Light"))
  {
    ImGui::RadioButton("Point", &helloVk.m_pushConstant.lightType, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Infinite", &helloVk.m_pushConstant.lightType, 1);

    ImGui::SliderFloat3("Position", &helloVk.m_pushConstant.lightPosition.x, -20.f, 20.f);
    ImGui::SliderFloat("Intensity", &helloVk.m_pushConstant.lightIntensity, 0.f, 150.f);
  }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
static int const SAMPLE_WIDTH  = 1280;
static int const SAMPLE_HEIGHT = 720;

//--------------------------------------------------------------------------------------------------
// Application Entry
//
int main(int argc, char** argv)
{
  UNUSED(argc);

  // Setup GLFW window
  glfwSetErrorCallback(onErrorCallback);
  if(!glfwInit())
  {
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window =
      glfwCreateWindow(SAMPLE_WIDTH, SAMPLE_HEIGHT, PROJECT_NAME, nullptr, nullptr);

  // Setup camera
  CameraManip.setWindowSize(SAMPLE_WIDTH, SAMPLE_HEIGHT);
  CameraManip.setLookat(nvmath::vec3f(5, 4, -4), nvmath::vec3f(0, 1, 0), nvmath::vec3f(0, 1, 0));

  // Setup Vulkan
  if(!glfwVulkanSupported())
  {
    printf("GLFW: Vulkan Not Supported\n");
    return 1;
  }

  // setup some basic things for the sample, logging file for example
  NVPSystem system(PROJECT_NAME);

  // Search path for shaders and other media
  defaultSearchPaths = {
      NVPSystem::exePath() + PROJECT_RELDIRECTORY,
      NVPSystem::exePath() + PROJECT_RELDIRECTORY "..",
      std::string(PROJECT_NAME),
  };

  // Requesting Vulkan extensions and layers
  nvvk::ContextCreateInfo contextInfo(true);
  contextInfo.setVersion(1, 2);
  contextInfo.addInstanceLayer("VK_LAYER_LUNARG_monitor", true);
  contextInfo.addInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME);
  contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, true);
#ifdef WIN32
  contextInfo.addInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
  contextInfo.addInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
  contextInfo.addInstanceExtension(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif
  contextInfo.addInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
  // #VKRay: Activate the ray tracing extension
  contextInfo.addDeviceExtension(VK_KHR_MAINTENANCE3_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  contextInfo.addDeviceExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
  // #VKRay: Activate the ray tracing extension
  vk::PhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures;
  contextInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false,
                                 &accelFeatures);
  vk::PhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures;
  contextInfo.addDeviceExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME, false, &rayQueryFeatures);


  // Creating Vulkan base application
  nvvk::Context vkctx{};
  vkctx.initInstance(contextInfo);
  // Find all compatible devices
  auto compatibleDevices = vkctx.getCompatibleDevices(contextInfo);
  assert(!compatibleDevices.empty());
  // Use a compatible device
  vkctx.initDevice(compatibleDevices[0], contextInfo);


  // Create example
  HelloVulkan helloVk;

  // Window need to be opened to get the surface on which to draw
  const vk::SurfaceKHR surface = helloVk.getVkSurface(vkctx.m_instance, window);
  vkctx.setGCTQueueWithPresent(surface);

  helloVk.setup(vkctx.m_instance, vkctx.m_device, vkctx.m_physicalDevice,
                vkctx.m_queueGCT.familyIndex);
  helloVk.createSwapchain(surface, SAMPLE_WIDTH, SAMPLE_HEIGHT);
  helloVk.createDepthBuffer();
  helloVk.createRenderPass();
  helloVk.createFrameBuffers();

  // Setup Imgui
  helloVk.initGUI(0);  // Using sub-pass 0

  // Creation of the example
  nvmath::mat4f t = nvmath::translation_mat4(nvmath::vec3f{0, 0.0, 0});
  helloVk.loadModel(nvh::findFile("media/scenes/plane.obj", defaultSearchPaths, true), t);
  //helloVk.loadModel(nvh::findFile("media/scenes/wuson.obj", defaultSearchPaths, true));
  helloVk.loadModel(nvh::findFile("media/scenes/Medieval_building.obj", defaultSearchPaths, true));

  helloVk.createOffscreenRender();
  helloVk.createDescriptorSetLayout();
  helloVk.createGraphicsPipeline();
  helloVk.createUniformBuffer();
  helloVk.createSceneDescriptionBuffer();

  // #VKRay
  helloVk.initRayTracing();
  helloVk.createBottomLevelAS();
  helloVk.createTopLevelAS();

  // Need the Top level AS
  helloVk.updateDescriptorSet();

  helloVk.createPostDescriptor();
  helloVk.createPostPipeline();
  helloVk.updatePostDescriptorSet();


  helloVk.createCompDescriptors();
  helloVk.updateCompDescriptors();
  helloVk.createCompPipelines();


  nvmath::vec4f clearColor = nvmath::vec4f(0, 0, 0, 0);

  helloVk.setupGlfwCallbacks(window);
  ImGui_ImplGlfw_InitForVulkan(window, true);


  AoControl aoControl;


  // Main loop
  while(!glfwWindowShouldClose(window))
  {
    try
    {
      glfwPollEvents();
      if(helloVk.isMinimized())
        continue;

      // Start the Dear ImGui frame
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();

      // Show UI window.
      if(helloVk.showGui())
      {
        ImGuiH::Panel::Begin();
        ImGui::ColorEdit3("Clear color", reinterpret_cast<float*>(&clearColor));

        renderUI(helloVk);
        ImGui::SetNextTreeNodeOpen(true, ImGuiCond_Once);
        if(ImGui::CollapsingHeader("Ambient Occlusion"))
        {
          bool changed{false};
          changed |= ImGui::SliderFloat("Radius", &aoControl.rtao_radius, 0, 5);
          changed |= ImGui::SliderInt("Rays per Pixel", &aoControl.rtao_samples, 1, 64);
          changed |= ImGui::SliderFloat("Power", &aoControl.rtao_power, 1, 5);
          changed |= ImGui::InputInt("Max Samples", &aoControl.max_samples);
          changed |= ImGui::Checkbox("Distanced Based", (bool*)&aoControl.rtao_distance_based);
          if(changed)
            helloVk.resetFrame();
        }

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                    1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGuiH::Control::Info("", "", "(F10) Toggle Pane", ImGuiH::Control::Flags::Disabled);
        ImGuiH::Panel::End();
      }

      // Start rendering the scene
      helloVk.prepareFrame();

      // Start command buffer of this frame
      auto                     curFrame = helloVk.getCurFrame();
      const vk::CommandBuffer& cmdBuf   = helloVk.getCommandBuffers()[curFrame];

      cmdBuf.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

      // Updating camera buffer
      helloVk.updateUniformBuffer(cmdBuf);

      // Clearing screen
      std::array<vk::ClearValue, 3> clearValues;
      clearValues[0].setColor(
          std::array<float, 4>({clearColor[0], clearColor[1], clearColor[2], clearColor[3]}));


      // Offscreen render pass
      {
        clearValues[1].setColor(std::array<float, 4>{0, 0, 0, 0});
        clearValues[2].setDepthStencil({1.0f, 0});
        vk::RenderPassBeginInfo offscreenRenderPassBeginInfo;
        offscreenRenderPassBeginInfo.setClearValueCount(3);
        offscreenRenderPassBeginInfo.setPClearValues(clearValues.data());
        offscreenRenderPassBeginInfo.setRenderPass(helloVk.m_offscreenRenderPass);
        offscreenRenderPassBeginInfo.setFramebuffer(helloVk.m_offscreenFramebuffer);
        offscreenRenderPassBeginInfo.setRenderArea({{}, helloVk.getSize()});

        // Rendering Scene
        {
          cmdBuf.beginRenderPass(offscreenRenderPassBeginInfo, vk::SubpassContents::eInline);
          helloVk.rasterize(cmdBuf);
          cmdBuf.endRenderPass();
          helloVk.runCompute(cmdBuf, aoControl);
        }
      }

      // 2nd rendering pass: tone mapper, UI
      {
        clearValues[1].setDepthStencil({1.0f, 0});
        vk::RenderPassBeginInfo postRenderPassBeginInfo;
        postRenderPassBeginInfo.setClearValueCount(2);
        postRenderPassBeginInfo.setPClearValues(clearValues.data());
        postRenderPassBeginInfo.setRenderPass(helloVk.getRenderPass());
        postRenderPassBeginInfo.setFramebuffer(helloVk.getFramebuffers()[curFrame]);
        postRenderPassBeginInfo.setRenderArea({{}, helloVk.getSize()});

        cmdBuf.beginRenderPass(postRenderPassBeginInfo, vk::SubpassContents::eInline);
        // Rendering tonemapper
        helloVk.drawPost(cmdBuf);
        // Rendering UI
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdBuf);
        cmdBuf.endRenderPass();
      }

      // Submit for display
      cmdBuf.end();
      helloVk.submitFrame();
    }
    catch(const std::system_error& e)
    {
      if(e.code() == vk::Result::eErrorDeviceLost)
      {
#if _WIN32
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_ICONERROR | MB_OK | MB_DEFBUTTON1);
#endif
      }
      std::cout << e.what() << std::endl;
      return e.code().value();
    }
  }

  // Cleanup
  helloVk.getDevice().waitIdle();
  helloVk.destroyResources();
  helloVk.destroy();

  vkctx.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
