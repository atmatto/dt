#include "SDL_video.h"
#include "vulkan_core.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan.h>
#include <vk_mem_alloc.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "shaders_out/shader.vert.h"
#include "shaders_out/shader.frag.h"

// macro for printing informational messages
#define infof(fmt, args...) printf("[info] " fmt "\n", ##args)

// macro for printing errors
#define errorf(fmt, args...) printf("[error] " fmt "\n", ##args)

// like errorf, but exits
#define panicf(fmt, args...) do {errorf(fmt, ##args); exit(1);} while(0)

// checks if the result is VK_SUCCESS and exits otherwise
#define must(result) do { \
		VkResult r = result; \
		if (r != VK_SUCCESS) { \
			panicf(__FILE__":%d: function returned VkResult \"%d\", but VK_SUCCESS was expected.", __LINE__, r); \
		} \
	} while (0)

// if pointer is null, prints error and exits
#define mustPtr(ptr, fmt...) do { \
		if (ptr == NULL) { \
			panicf("pointer was null: " fmt); \
		} \
	} while (0);

#define LENGTH(X) (sizeof X / sizeof X[0])

typedef struct State { // TODO: Some members are probably unneeded
	SDL_Window *window;
	VkPhysicalDevice vpd;
	VkDevice vdev;
	VmaAllocator vma;
	VkSwapchainKHR vsch;
	uint32_t vschimgc; // number of images in swapchain
	VkImage *vschimg; // array of swapchain images
	VkImageView *vschimgv;
	VkImage dbi; // depth buffer
	VmaAllocation dba;
	VkImageView dbiv;
	VkQueue queue;
	VkPipeline pl;
	VkCommandPool cmdpl;
	VkCommandBuffer *cmdb;
	VkSemaphore *imgsem; // image is ready to be drawn to
	VkSemaphore *prsem; // ready to present
	VkFence *cmdbfen; // ready to reuse cmd buffer
	VkExtent2D imgext;
	VkSurfaceKHR vsurface;
	VkSurfaceFormatKHR vsurff;
} State;

// initialize sdl and setup window
VkResult beginSdl(State *s) {
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		errorf("SDL_Init: %s", SDL_GetError());
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	int x = 640, y = 480;
	s->window = SDL_CreateWindow("Vulkano", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, x, y, SDL_WINDOW_VULKAN|SDL_WINDOW_RESIZABLE);
	if (s->window == NULL) {
		errorf("SDL_CreateWindow: %s", SDL_GetError());
		return VK_ERROR_INITIALIZATION_FAILED;
	}
  
	return VK_SUCCESS;
}

// cleanup sdl
void endSdl(State *s) {
	SDL_DestroyWindow(s->window);
	SDL_Quit();
}

// returns the index of the chosen device
// TODO: let the user override this; require necessary capabilities
// TODO: what if device score calculation overflows?
uint32_t chooseDevice(uint32_t count, const VkPhysicalDevice *devs) {
	// choose the gpu with the highest score
	uint64_t maxScore = 0;
	int bestDev = 0;
	VkPhysicalDeviceProperties props;
	for (uint32_t i = 0; i < count; i++) {
		vkGetPhysicalDeviceProperties(*devs, &props);
	
		// score multiplier based on the GPU type
		int multiplier;
		switch (props.deviceType) {
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
				multiplier = 10;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
				multiplier = 2;
				break;
			default:
				multiplier = 1;
				break;
		}

		VkPhysicalDeviceMemoryProperties mem;
		vkGetPhysicalDeviceMemoryProperties(*devs, &mem);

		uint64_t score = 0;
		// choose one with more memory if there are multiple of the same type
		for (uint32_t j = 0; j < mem.memoryHeapCount; j++)
			if (mem.memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
				score += mem.memoryHeaps[j].size; // VkDeviceSize = uint64_t

		score *= multiplier;
		if (score > maxScore) {
			maxScore = score;
			bestDev = i;
		}
	}

	vkGetPhysicalDeviceProperties(devs[bestDev], &props);
	infof("vulkan physical device chosen: (%d) %s (score %" PRIu64 ")", bestDev, props.deviceName, maxScore);
	return 0;
}

// returns 1 if all specified device extensions are available, otherwise returns 0
char checkDevExtensions(VkPhysicalDevice dev, uint32_t count, const char *exts[]) {
	uint32_t pc;
	vkEnumerateDeviceExtensionProperties(dev, NULL, &pc, NULL);
	VkExtensionProperties *eps = calloc(pc, sizeof(VkExtensionProperties));
	mustPtr(eps, "device extensions array, len = %"PRIu32, pc);
	vkEnumerateDeviceExtensionProperties(dev, NULL, &pc, eps);

	uint32_t found = 0; // number of matched extensions
	for (uint32_t i = 0; i < pc; i++) {
		for (uint32_t j = 0; j < count; j++) {
			if (strncmp(exts[j], eps[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE) == 0) {
				found++;
			}
		}
	}
	return found == count;
}

void createSwapchain(State *s) {
	// initialize swapchain

	if (s->vschimgc == 0) { // BUG: We assume that the format won't change
		uint32_t srffc;
		must(vkGetPhysicalDeviceSurfaceFormatsKHR(s->vpd, s->vsurface, &srffc, NULL));
		VkSurfaceFormatKHR *srff = calloc(srffc, sizeof(VkSurfaceFormatKHR));
		mustPtr(srff, "physical device surface formats array, len = %"PRIu32, srffc);
		must(vkGetPhysicalDeviceSurfaceFormatsKHR(s->vpd, s->vsurface, &srffc, srff));
		s->vsurff = srff[0];
		for (uint32_t i = 0; i < srffc; i++) {
			if (srff[i].format == VK_FORMAT_B8G8R8A8_SRGB && srff[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				s->vsurff = srff[i];
		}
		free(srff);
	}

	VkSurfaceCapabilitiesKHR srfcap = {};
	must(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->vpd, s->vsurface, &srfcap));

	uint32_t imgc = 3;
	if (imgc < srfcap.minImageCount)
		imgc = srfcap.minImageCount;
	if (imgc > srfcap.maxImageCount)
		imgc = srfcap.maxImageCount;

	VkExtent2D imgext = {1920, 1080};
	if (srfcap.currentExtent.width != 0xFFFFFFFF || srfcap.currentExtent.height != 0xFFFFFFFF) {
		imgext = srfcap.currentExtent;
	} else {
		if (imgext.width > srfcap.maxImageExtent.width)
			imgext.width = srfcap.maxImageExtent.width;
		if (imgext.height > srfcap.maxImageExtent.height)
			imgext.height = srfcap.maxImageExtent.height;
		if (imgext.width < srfcap.minImageExtent.width)
			imgext.width = srfcap.minImageExtent.width;
		if (imgext.height < srfcap.minImageExtent.height)
			imgext.height = srfcap.minImageExtent.height;
	}
	s->imgext = imgext;

	VkSwapchainCreateInfoKHR schci = {};
	schci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	schci.surface = s->vsurface;
	schci.minImageCount = imgc;
	schci.imageFormat = s->vsurff.format;
	schci.imageColorSpace = s->vsurff.colorSpace;
	schci.imageExtent = imgext;
	schci.imageArrayLayers = 1;
	schci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	schci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	schci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	schci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; // TODO: use FIFO Relaxed, with FIFO as fallback

	must(vkCreateSwapchainKHR(s->vdev, &schci, NULL, &s->vsch));

	// get swapchain image handles

	must(vkGetSwapchainImagesKHR(s->vdev, s->vsch, &s->vschimgc, NULL));
	if (s->vschimg != NULL) {
		s->vschimg = reallocarray(s->vschimg, s->vschimgc, sizeof(VkImage));
	} else {
		s->vschimg = calloc(s->vschimgc, sizeof(VkImage));
	}
	mustPtr(s->vschimg, "swapchain images array, len = %"PRIu32, s->vschimgc);
	must(vkGetSwapchainImagesKHR(s->vdev, s->vsch, &s->vschimgc, s->vschimg));

	// create swapchain image views

	if (s->vschimgv != NULL) {
		s->vschimgv = reallocarray(s->vschimgv, s->vschimgc, sizeof(VkImageView));
	} else {
		s->vschimgv = calloc(s->vschimgc, sizeof(VkImageView));
	}
	mustPtr(s->vschimgv, "swapchain image views array, len = %"PRIu32, s->vschimgc);
	for (uint32_t i = 0; i < s->vschimgc; i++) {
		VkImageViewCreateInfo ivci = {};
		ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image = s->vschimg[i];
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = s->vsurff.format;
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 1;
		must(vkCreateImageView(s->vdev, &ivci, NULL, &s->vschimgv[i]));
	}

	infof("swapchain created (%"PRIu32" images, %"PRIu32"x%"PRIu32")", s->vschimgc, imgext.width, imgext.height);

	// create depth buffer

	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_D32_SFLOAT;
	ici.extent.width = s->imgext.width;
	ici.extent.height = s->imgext.height;
	ici.extent.depth = 1;
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo aci = {};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	must(vmaCreateImage(s->vma, &ici, &aci, &s->dbi, &s->dba, NULL));

	VkImageViewCreateInfo ivci = {};
	ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivci.image = s->dbi;
	ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivci.format = VK_FORMAT_D32_SFLOAT;
	ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	ivci.subresourceRange.baseMipLevel = 0;
	ivci.subresourceRange.levelCount = 1;
	ivci.subresourceRange.baseArrayLayer = 0;
	ivci.subresourceRange.layerCount = 1;
	must(vkCreateImageView(s->vdev, &ivci, NULL, &s->dbiv));

	infof("depth buffer created");
}

// initialize vulkan
void beginVulkan(State *s) {
	// create instance
	
	VkApplicationInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	ai.apiVersion = VK_API_VERSION_1_3;

	const char *layers[] = {
		"VK_LAYER_KHRONOS_validation",
	};
	const char *iextensions[16] = { // remember to update the count in iextc
		VK_KHR_SURFACE_EXTENSION_NAME,
	};

	uint32_t sdlextc = 0;
	SDL_Vulkan_GetInstanceExtensions(s->window, &sdlextc, NULL);
	uint32_t iextc = 1 + sdlextc;
	if (iextc > LENGTH(iextensions)) {
		panicf("too many instance extensions given by sdl2 (%"PRIu32", for a total %"PRIu32"/%lu)",
			sdlextc, iextc, LENGTH(iextensions));
	}
	SDL_Vulkan_GetInstanceExtensions(s->window, &sdlextc, &iextensions[iextc - sdlextc]);
	infof("instance extensions:");
	for (uint32_t i = 0; i < iextc; i++)
		infof("%d: %s", i, iextensions[i]);
	
	VkInstanceCreateInfo ii = {};
	ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ii.pApplicationInfo = &ai;
	ii.enabledLayerCount = LENGTH(layers);
	ii.ppEnabledLayerNames = layers;
	ii.enabledExtensionCount = iextc;
	ii.ppEnabledExtensionNames = iextensions;
	
	VkInstance instance;
	must(vkCreateInstance(&ii, NULL, &instance));
	infof("vulkan instance created");
	
	// choose physical device

	uint32_t physdc;
	must(vkEnumeratePhysicalDevices(instance, &physdc, NULL));
	infof("vulkan devices count: %"PRIu32, physdc);
	if (physdc == 0) {
		panicf("no gpu available");
	}
	VkPhysicalDevice *pdevs = calloc(physdc, sizeof(VkPhysicalDevice));
	mustPtr(pdevs, "physical devices array, len = %"PRIu32, physdc);
	must(vkEnumeratePhysicalDevices(instance, &physdc, pdevs));

	uint32_t physdi = chooseDevice(physdc, pdevs);
	s->vpd = pdevs[physdi];

	// check required device extensions

	const char *dextensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	char extOk = checkDevExtensions(s->vpd, LENGTH(dextensions), dextensions);
	if (!extOk) {
		panicf("gpu doesn't support required device extensions");
	}

	// create queues

	uint32_t qfamc;
	vkGetPhysicalDeviceQueueFamilyProperties(s->vpd, &qfamc, NULL);
	if (qfamc == 0) {
		panicf("no queue families available");
	}
	VkQueueFamilyProperties *qfamp = calloc(qfamc, sizeof(VkQueueFamilyProperties));
	mustPtr(qfamp, "queue family properties array, len = %"PRIu32, qfamc);
	vkGetPhysicalDeviceQueueFamilyProperties(s->vpd, &qfamc, qfamp);
	uint32_t qfi = qfamc;
	for (uint32_t i = 0; i < qfamc; i++)
		if ((qfamp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))
				== (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {
			qfi = i;
			break;
		}
	if (qfi == qfamc)
		panicf("no queue family supporting graphics");

	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = qfi;
	qci.queueCount = 1;
	qci.pQueuePriorities = (float[]){1.0f};

	// no device features are used

	// create device

	VkPhysicalDeviceSynchronization2Features s2f = {};
	s2f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
	s2f.synchronization2 = VK_TRUE;

	VkPhysicalDeviceDynamicRenderingFeatures drf = {};
	drf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
	drf.pNext = &s2f;
	drf.dynamicRendering = VK_TRUE;

	VkDeviceCreateInfo di = {};
	di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	di.pNext = &drf;
	di.queueCreateInfoCount = 1;
	di.pQueueCreateInfos = (VkDeviceQueueCreateInfo[]){qci};
	di.enabledExtensionCount = LENGTH(dextensions);
	di.ppEnabledExtensionNames = dextensions;
	
	VkDevice dev;
	must(vkCreateDevice(s->vpd, &di, NULL, &dev));
	s->vdev = dev;

	infof("vulkan device created");

	// create VMA allocator

	VmaAllocatorCreateInfo aci = {};
	aci.physicalDevice = s->vpd;
	aci.device = s->vdev;
	aci.instance = instance;
	aci.vulkanApiVersion = ai.apiVersion;
	must(vmaCreateAllocator(&aci, &s->vma));

	// get the queue handle

	vkGetDeviceQueue(dev, qfi, 0, &s->queue);

	// create vulkan rendering surface

	if (SDL_Vulkan_CreateSurface(s->window, instance, &s->vsurface) != SDL_TRUE) {
		panicf("failed to create a vulkan surface using sdl2");
	}

	// create shaders

	VkShaderModuleCreateInfo vsmci = {};
	vsmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vsmci.codeSize = shader_vert_len;
	vsmci.pCode = (uint32_t *)shader_vert; // TODO: Alignment?
	VkShaderModule vsm;
	must(vkCreateShaderModule(dev, &vsmci, NULL, &vsm));
	VkPipelineShaderStageCreateInfo vspsci = {};
	vspsci.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vspsci.stage = VK_SHADER_STAGE_VERTEX_BIT;
	vspsci.module = vsm;
	vspsci.pName = "main";

	VkShaderModuleCreateInfo fsmci = {};
	fsmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fsmci.codeSize = shader_frag_len;
	fsmci.pCode = (uint32_t *)shader_frag; // TODO: Alignment?
	VkShaderModule fsm;
	must(vkCreateShaderModule(dev, &fsmci, NULL, &fsm));
	VkPipelineShaderStageCreateInfo fspsci = {};
	fspsci.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	fspsci.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	fspsci.module = fsm;
	fspsci.pName = "main";

	VkPipelineShaderStageCreateInfo psci[] = {vspsci, fspsci};

	// create swapchain

	createSwapchain(s);

	// create graphics pipeline

	VkPipelineRenderingCreateInfo plrci = {};
	plrci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	plrci.colorAttachmentCount = 1;
	plrci.pColorAttachmentFormats = &s->vsurff.format;
	plrci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

	VkGraphicsPipelineCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	plci.pNext = &plrci;
	plci.stageCount = LENGTH(psci);
	plci.pStages = psci;
	plci.pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		// TODO: Vertices are currently hardcoded
	};
	plci.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,
	};
	// const VkPipelineTessellationStateCreateInfo*     pTessellationState;
	plci.pViewportState = &(VkPipelineViewportStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	plci.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.lineWidth = 1.0f,
	};
	plci.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	plci.pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};
	plci.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &(VkPipelineColorBlendAttachmentState){
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			.blendEnable = VK_TRUE,
			.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
			.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
			.colorBlendOp = VK_BLEND_OP_ADD,
			.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
			.alphaBlendOp = VK_BLEND_OP_ADD,
		},
		.blendConstants = {0, 0, 0, 0},
	};
	VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	plci.pDynamicState = &(VkPipelineDynamicStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = LENGTH(dyns),
		.pDynamicStates = (VkDynamicState *)&dyns,
	};
	VkPipelineLayoutCreateInfo pllyci = {};
	pllyci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	VkPipelineLayout plly;
	must(vkCreatePipelineLayout(dev, &pllyci, NULL, &plly));
	plci.layout = plly;
	// plci.renderPass = s->rp;
	// plci.subpass = 0;
	plci.basePipelineHandle = VK_NULL_HANDLE;
	plci.basePipelineIndex = 0;
	must(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &plci, NULL, &s->pl));

	// create command pool

	VkCommandPoolCreateInfo cmdplci = {};
	cmdplci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdplci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cmdplci.queueFamilyIndex = qfi;
	must(vkCreateCommandPool(dev, &cmdplci, NULL, &s->cmdpl));

	// allocate command buffers

	s->cmdb = calloc(s->vschimgc, sizeof(VkCommandBuffer));
	mustPtr(s->cmdb, "command buffers array, len = %"PRIu32, s->vschimgc);
	VkCommandBufferAllocateInfo cmdbai = {};
	cmdbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmdbai.commandPool = s->cmdpl;
	cmdbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmdbai.commandBufferCount = s->vschimgc;
	must(vkAllocateCommandBuffers(dev, &cmdbai, s->cmdb));

	// create semaphores and fences

	s->imgsem = calloc(s->vschimgc, sizeof(VkSemaphore));
	mustPtr(s->imgsem, "image semaphores array, len = %"PRIu32, s->vschimgc);
	for (uint32_t i = 0; i < s->vschimgc; i++)
		must(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &s->imgsem[i]));
	s->prsem = calloc(s->vschimgc, sizeof(VkSemaphore));
	mustPtr(s->prsem, "present semaphores array, len = %"PRIu32, s->vschimgc);
	for (uint32_t i = 0; i < s->vschimgc; i++)
		must(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &s->prsem[i]));
	s->cmdbfen = calloc(s->vschimgc, sizeof(VkFence));
	mustPtr(s->prsem, "command buffer fences array, len = %"PRIu32, s->vschimgc);
	VkFenceCreateInfo cmdbfenci = {};
	cmdbfenci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	cmdbfenci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i = 0; i < s->vschimgc; i++)
		must(vkCreateFence(dev, &cmdbfenci, NULL, &s->cmdbfen[i]));
}

// cleanup vulkan
void endVulkan(State *s) {
	vkDeviceWaitIdle(s->vdev);
}

void printFramerate() {
	static uint32_t frames = 0;
	static uint32_t lastCalculation = 0;
	uint32_t now = SDL_GetTicks(); // ms
	frames += 1;
	if (now - lastCalculation >= 2000) {
		infof("framerate: %"PRIu32, (1000 * frames)/(now - lastCalculation));
		frames = 0;
		lastCalculation = now;
	}
}

void eventLoop(State *s) {
	SDL_Event e;
	char quit = 0;
	char resize = 0;
	uint32_t schimgi = 0;
	uint32_t schi = 0;

	VkRenderingAttachmentInfo ati = {};
	ati.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	ati.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
	ati.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	ati.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ati.clearValue = (VkClearValue){
		.color = (VkClearColorValue){.float32 = {1, 1, 1, 1}},
	};

	VkRenderingAttachmentInfo dti = {};
	dti.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	dti.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
	dti.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	dti.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	dti.clearValue = (VkClearValue){
		.depthStencil = (VkClearDepthStencilValue){.depth = 1.0f}, // TODO: ?
	};

	VkRenderingInfo ri = {};
	ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	ri.renderArea.extent = s->imgext;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &ati;
	ri.pDepthAttachment = &dti;
	// TODO const VkRenderingAttachmentInfo*    pDepthAttachment;

	VkViewport vp = {};
	vp.width = s->imgext.width;
	vp.height = s->imgext.height;
	vp.minDepth = 0;
	vp.maxDepth = 1;

	VkRect2D sc = {};
	sc.extent = s->imgext;

	VkImageMemoryBarrier2 imb1 = {};
	imb1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	imb1.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
	imb1.srcAccessMask = VK_ACCESS_2_NONE;
	imb1.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	imb1.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	imb1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imb1.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
	imb1.image = VK_NULL_HANDLE;
	imb1.subresourceRange = (VkImageSubresourceRange){
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};
	VkImageMemoryBarrier2 imb2 = {};
	imb2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	imb2.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	imb2.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	imb2.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
	imb2.dstAccessMask = VK_ACCESS_2_NONE;
	imb2.oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
	imb2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	imb2.image = VK_NULL_HANDLE;
	imb2.subresourceRange = (VkImageSubresourceRange){
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};
	VkImageMemoryBarrier2 dmb1 = {};
	dmb1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	dmb1.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
	dmb1.srcAccessMask = VK_ACCESS_2_NONE;
	dmb1.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	dmb1.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dmb1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	dmb1.newLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
	dmb1.image = VK_NULL_HANDLE;
	dmb1.subresourceRange = (VkImageSubresourceRange){
		.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};
	// VkImageMemoryBarrier2 dmb2 = {};
	// dmb2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	// dmb2.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
	// dmb2.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	// dmb2.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
	// dmb2.dstAccessMask = VK_ACCESS_2_NONE;
	// dmb2.oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
	// dmb2.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	// dmb2.image = s->dbi;
	// dmb2.subresourceRange = (VkImageSubresourceRange){
	// 	.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	// 	.baseMipLevel = 0,
	// 	.levelCount = 1,
	// 	.baseArrayLayer = 0,
	// 	.layerCount = 1,
	// };
	VkImageMemoryBarrier2 imbs[] = {imb1, imb2, dmb1, /*dmb2*/};

	VkDependencyInfo di = {};
	di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	di.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	di.imageMemoryBarrierCount = LENGTH(imbs);
	di.pImageMemoryBarriers = imbs;

	while (!quit) {
		while (SDL_PollEvent(&e) != 0) {
			if (e.type == SDL_QUIT)
				quit = 1;
			else if (e.type == SDL_WINDOWEVENT) {
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					resize = 1;
				}
			}
		}

		schi = (schi + 1) % s->vschimgc;

		if (resize) {  // TODO: What if the size becomes 0x0?
			resize = 0;
			vkDeviceWaitIdle(s->vdev);
			// destroy depth buffer
			vkDestroyImageView(s->vdev, s->dbiv, NULL);
			vmaDestroyImage(s->vma, s->dbi, s->dba);
			// destroy image views
			for (uint32_t i = 0; i < s->vschimgc; i++)
				vkDestroyImageView(s->vdev, s->vschimgv[i], NULL);
			// destroy swap chain
			vkDestroySwapchainKHR(s->vdev, s->vsch, NULL);
			infof("swapchain and depth buffer destroyed");
			// recreate swap chain
			createSwapchain(s);
			// update variables
			ri.renderArea.extent = s->imgext;
			vp.width = s->imgext.width;
			vp.height = s->imgext.height;
			sc.extent = s->imgext;
		}

		// TODO: Analyze the swapchain usage, paying attention to synchronization regarding the depth buffer,
		//       Taking https://github.com/KhronosGroup/Vulkan-Samples/tree/main/samples/performance/swapchain_images into
		//       consideration.
		// acquire image from swap chain, wait for an available command buffer

		VkResult ar = vkAcquireNextImageKHR(s->vdev, s->vsch, 3000000000, s->imgsem[schi], VK_NULL_HANDLE, &schimgi);
		if (ar == VK_SUCCESS) {
		} else if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
			resize = 1;
			continue;
		} else if (ar != VK_SUBOPTIMAL_KHR) {
			panicf("failed to acquire swap chain image, VkResult=%d", ar);
		}

		must(vkWaitForFences(s->vdev, 1, &s->cmdbfen[schi], VK_TRUE, 3000000000));
		vkResetFences(s->vdev, 1, &s->cmdbfen[schi]);
		printFramerate();

		// record command buffer

		must(vkBeginCommandBuffer(s->cmdb[schi], &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));

		imbs[0].image = s->vschimg[schimgi];
		imbs[1].image = s->vschimg[schimgi];
		imbs[2].image = s->dbi;
		vkCmdPipelineBarrier2(s->cmdb[schi], &di);
		
		ati.imageView = s->vschimgv[schimgi];
		dti.imageView = s->dbiv;
		vkCmdBeginRendering(s->cmdb[schi], &ri);

		ri.renderArea.extent = s->imgext;
		vp.width = s->imgext.width;
		vp.height = s->imgext.height;
		vkCmdSetViewport(s->cmdb[schi], 0, 1, &vp);
		vkCmdSetScissor(s->cmdb[schi], 0, 1, &sc);

		vkCmdBindPipeline(s->cmdb[schi], VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl);

		vkCmdDraw(s->cmdb[schi], 3, 1, 0, 0);
		vkCmdEndRendering(s->cmdb[schi]);

		must(vkEndCommandBuffer(s->cmdb[schi]));

		// submit command buffer

		VkSubmitInfo2 si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
		si.waitSemaphoreInfoCount = 1;
		si.pWaitSemaphoreInfos = &(VkSemaphoreSubmitInfo){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = s->imgsem[schi],
			.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		};
		si.commandBufferInfoCount = 1;
		si.pCommandBufferInfos = &(VkCommandBufferSubmitInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = s->cmdb[schi],
		};
		si.signalSemaphoreInfoCount = 1;
		si.pSignalSemaphoreInfos = &(VkSemaphoreSubmitInfo){
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = s->prsem[schi],
			.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			
		};

		must(vkQueueSubmit2(s->queue, 1, &si, s->cmdbfen[schi]));

		// present swap chain image

		VkPresentInfoKHR pi = {};
		pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &s->prsem[schi];
		pi.swapchainCount = 1;
		pi.pSwapchains = &s->vsch;
		pi.pImageIndices = &schimgi;
		VkResult pr = vkQueuePresentKHR(s->queue, &pi);
		if (pr == VK_SUCCESS) {
		} else if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
			resize = 1;
			continue;
		} else {
			panicf("failed to present swap chain image, VkResult=%d", pr);
		}
	}
}

int main() {
	State s = {};
	if (beginSdl(&s) != VK_SUCCESS) {
		return 1;
	}

	beginVulkan(&s);

	eventLoop(&s);

	endVulkan(&s);

	endSdl(&s);
	return 0;
}
