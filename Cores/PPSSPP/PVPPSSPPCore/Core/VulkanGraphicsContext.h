//
//  VulkanGraphicsContext.h
//  PVPPSSPP
//  Copyright (c) 2012- PPSSPP Project.
//

#import "PVPPSSPPCore.h"
#import "PVPPSSPPCore+Controls.h"
#import "PVPPSSPPCore+Audio.h"
#import "PVPPSSPPCore+Video.h"
#import <PVPPSSPP/PVPPSSPP-Swift.h>
#import <Foundation/Foundation.h>
#import <PVSupport/PVSupport.h>
#import <PVSupport/PVLogging.h>

/* PPSSPP Includes */
#include <vector>
#include <string>
#include <cstring>

#include "Common/System/Display.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanDebug.h"
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/VulkanRenderManager.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/thin3d_create.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/VR/PPSSPPVR.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"
#if !PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(SWITCH)
#include <dlfcn.h>
#endif

#ifndef VulkanGraphicsContext_h
#define VulkanGraphicsContext_h

typedef void *VulkanLibraryHandle;
static VulkanLibraryHandle vulkanLibrary;
#define LOAD_GLOBAL_FUNC(x) x = (PFN_ ## x)dlsym(vulkanLibrary, #x); if (!x) { ELOG(@"Missing (global): %s", #x);}

class VulkanGraphicsContext : public GraphicsContext {
public:
	VulkanGraphicsContext() {
	}

	~VulkanGraphicsContext() {
        delete g_Vulkan;
        g_Vulkan = nullptr;
	}

	VulkanGraphicsContext(CAMetalLayer* layer, const char* vulkan_path) {
		bool success = false;
		ELOG(@"Init");
		init_glslang();
		ELOG(@"Creating Vulkan context");
		Version gitVer(PPSSPP_GIT_VERSION);
		if (!this->VulkanLoad(vulkan_path)) {
			ELOG(@"Failed to load Vulkan driver library");
			return;
		}
		if (!g_Vulkan) {
			g_Vulkan = new VulkanContext();
		}
		VulkanContext::CreateInfo info{};
		info.app_name = "PPSSPP";
		info.app_ver = gitVer.ToInteger();
		info.flags = this->flags;
		VkResult res = g_Vulkan->CreateInstance(info);
		if (res != VK_SUCCESS) {
			ELOG(@"Failed to create vulkan context: %s", g_Vulkan->InitError().c_str());
			VulkanSetAvailable(false);
			delete g_Vulkan;
			g_Vulkan = nullptr;
			return;
		}
		int physicalDevice = g_Vulkan->GetBestPhysicalDevice();
		if (physicalDevice < 0) {
			ELOG(@"No usable Vulkan device found.");
			g_Vulkan->DestroyInstance();
			delete g_Vulkan;
			g_Vulkan = nullptr;
			return;
		}
		g_Vulkan->ChooseDevice(physicalDevice);
		ELOG(@"Creating Vulkan device");
		if (g_Vulkan->CreateDevice() != VK_SUCCESS) {
			ELOG(@"Failed to create vulkan device: %s", g_Vulkan->InitError().c_str());
			g_Vulkan->DestroyInstance();
			delete g_Vulkan;
			g_Vulkan = nullptr;
			return;
		}
		ELOG(@"Vulkan device created!");
		g_Config.iGPUBackend = (int)GPUBackend::VULKAN;
		SetGPUBackend(GPUBackend::VULKAN);
		if (!g_Vulkan) {
			ELOG(@"InitFromRenderThread: No Vulkan context");
			return;
		}
		res = g_Vulkan->InitSurface(WINDOWSYSTEM_METAL_EXT, (__bridge void *)layer, nullptr);
		if (res != VK_SUCCESS) {
			ELOG(@"g_Vulkan->InitSurface failed: '%s'", VulkanResultToString(res));
			return;
		}
		if (g_Vulkan->InitSwapchain()) {
			draw_ = Draw::T3DCreateVulkanContext(g_Vulkan);
			SetGPUBackend(GPUBackend::VULKAN);
			success = draw_->CreatePresets();  // Doesn't fail, we ship the compiler.
			_assert_msg_(success, "Failed to compile preset shaders");
			draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, g_Vulkan->GetBackbufferWidth(), g_Vulkan->GetBackbufferHeight());
			VulkanRenderManager *renderManager = (VulkanRenderManager *)draw_->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
			renderManager->SetInflightFrames(g_Config.iInflightFrames);
			success = renderManager->HasBackbuffers();
		} else {
			success = false;
		}
		ELOG(@"Vulkan Init completed, %s", success ? "successfully" : "but failed");
		if (!success) {
			g_Vulkan->DestroySwapchain();
			g_Vulkan->DestroySurface();
			g_Vulkan->DestroyDevice();
			g_Vulkan->DestroyInstance();
		}
	}

	Draw::DrawContext *GetDrawContext() override {
		return draw_;
	}

	void Shutdown() override {
        ELOG(@"Shutdown");
        draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, g_Vulkan->GetBackbufferWidth(), g_Vulkan->GetBackbufferHeight());
        delete draw_;
        draw_ = nullptr;
        g_Vulkan->WaitUntilQueueIdle();
        g_Vulkan->PerformPendingDeletes();
        g_Vulkan->DestroySwapchain();
        g_Vulkan->DestroySurface();
        ELOG(@"Done with ShutdownFromRenderThread");
		ELOG(@"Calling NativeShutdownGraphics");
		g_Vulkan->DestroyDevice();
		g_Vulkan->DestroyInstance();
		// We keep the g_Vulkan context around to avoid invalidating a ton of pointers around the app.
		finalize_glslang();
		ELOG(@"Shutdown completed");
	}

	void SwapBuffers() override {
	}

	void Resize() override {
		ELOG(@"Resize begin (oldsize: %dx%d)", g_Vulkan->GetBackbufferWidth(), g_Vulkan->GetBackbufferHeight());
		draw_->HandleEvent(Draw::Event::LOST_BACKBUFFER, g_Vulkan->GetBackbufferWidth(), g_Vulkan->GetBackbufferHeight());
		g_Vulkan->DestroySwapchain();
		g_Vulkan->DestroySurface();
		g_Vulkan->UpdateFlags(this->flags);
		g_Vulkan->ReinitSurface();
		g_Vulkan->InitSwapchain();
		draw_->HandleEvent(Draw::Event::GOT_BACKBUFFER, g_Vulkan->GetBackbufferWidth(), g_Vulkan->GetBackbufferHeight());
		ELOG(@"Resize end (final size: %dx%d)", g_Vulkan->GetBackbufferWidth(), g_Vulkan->GetBackbufferHeight());
	}

	void SwapInterval(int interval) override {
	}

	void *GetAPIContext() override {
		return g_Vulkan;
	}

	bool Initialized() {
		return draw_ != nullptr;
	}
    
    bool VulkanLoad(const char* path) {
        void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            ELOG(@"%s: Library loaded\n", path);
        } else {
            ELOG(@"Faield path %s: Library not loaded\n", path);
        }
        vulkanLibrary = lib;
        LOAD_GLOBAL_FUNC(vkCreateInstance);
        LOAD_GLOBAL_FUNC(vkGetInstanceProcAddr);
        LOAD_GLOBAL_FUNC(vkGetDeviceProcAddr);
        LOAD_GLOBAL_FUNC(vkEnumerateInstanceVersion);
        LOAD_GLOBAL_FUNC(vkEnumerateInstanceExtensionProperties);
        LOAD_GLOBAL_FUNC(vkEnumerateInstanceLayerProperties);
        if (vkCreateInstance && vkGetInstanceProcAddr && vkGetDeviceProcAddr && vkEnumerateInstanceExtensionProperties && vkEnumerateInstanceLayerProperties) {
            ELOG(@"VulkanLoad: Base functions loaded.");
            return true;
        } else {
            ELOG(@"VulkanLoad: Failed to load Vulkan base functions.");
            return false;
        }
    }
private:
	VulkanContext *g_Vulkan = nullptr;
	Draw::DrawContext *draw_ = nullptr;
	uint32_t flags = VULKAN_FLAG_PRESENT_MAILBOX | VULKAN_FLAG_PRESENT_FIFO_RELAXED;
};
#endif /* VulkanGraphicsContext_h */
