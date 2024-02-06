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
	VkDevice vdev;
	VkSwapchainKHR vsch;
	uint32_t vschimgc; // number of images in swapchain
	VkImage *vschimg; // array of swapchain images
	VkQueue queue;
	VkRenderPass rp;
	VkPipeline pl;
	VkCommandPool cmdpl;
	VkCommandBuffer *cmdb;
	VkSemaphore *imgsem; // image is ready to be drawn to
	VkSemaphore *prsem; // ready to present
	VkFence *cmdbfen; // ready to reuse cmd buffer
	VkFramebuffer *fb;
	VkExtent2D imgext;
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

// initialize vulkan
void beginVulkan(State *s) {
	// create instance
	
	VkApplicationInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	ai.apiVersion = VK_VERSION_1_3;

	const char *layers[] = {
		"VK_LAYER_KHRONOS_validation"
	};
	const char *iextensions[16] = { // remember to update the count in iextc
		"VK_KHR_surface",
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
	VkPhysicalDevice pd = pdevs[physdi];

	// check required device extensions

	const char *dextensions[] = {
		// "VK_KHR_synchronization2" TODO this was promotted and thus not needed?
		"VK_KHR_swapchain",
	};

	char extOk = checkDevExtensions(pd, LENGTH(dextensions), dextensions);
	if (!extOk) {
		panicf("gpu doesn't support required device extensions");
	}

	// create queues

	uint32_t qfamc;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfamc, NULL);
	if (qfamc == 0) {
		panicf("no queue families available");
	}
	VkQueueFamilyProperties *qfamp = calloc(qfamc, sizeof(VkQueueFamilyProperties));
	mustPtr(qfamp, "queue family properties array, len = %"PRIu32, qfamc);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfamc, qfamp);
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

	VkDeviceCreateInfo di = {};
	di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	di.queueCreateInfoCount = 1;
	di.pQueueCreateInfos = (VkDeviceQueueCreateInfo[]){qci};
	di.enabledExtensionCount = LENGTH(dextensions);
	di.ppEnabledExtensionNames = dextensions;
	
	VkDevice dev;
	must(vkCreateDevice(pd, &di, NULL, &dev));
	s->vdev = dev;

	infof("vulkan device created");

	// get the queue handle

	vkGetDeviceQueue(dev, qfi, 0, &s->queue);

	// create vulkan rendering surface

	VkSurfaceKHR surface;
	if (SDL_Vulkan_CreateSurface(s->window, instance, &surface) != SDL_TRUE) {
		panicf("failed to create a vulkan surface using sdl2");
	}

	// initialize swapchain

	uint32_t srffc;
	must(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &srffc, NULL));
	VkSurfaceFormatKHR *srff = calloc(srffc, sizeof(VkSurfaceFormatKHR));
	mustPtr(srff, "physical device surface formats array, len = %"PRIu32, srffc);
	must(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &srffc, srff));
	VkSurfaceFormatKHR *srffchosen = &srff[0];
	for (uint32_t i = 0; i < srffc; i++) {
		if (srff[i].format == VK_FORMAT_B8G8R8A8_SRGB && srff[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			srffchosen = &srff[i];
	}

	VkSurfaceCapabilitiesKHR srfcap = {};
	must(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &srfcap));

	uint32_t imgc = 3;
	if (imgc < srfcap.minImageCount)
		imgc = srfcap.minImageCount;
	if (imgc > srfcap.maxImageCount)
		imgc = srfcap.maxImageCount;

	VkExtent2D imgext = {1920, 1080}; // TODO: Make this configurable
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
	infof("chosen swapchain image extents: %"PRIu32"x%"PRIu32, imgext.width, imgext.height);

	VkSwapchainCreateInfoKHR schci = {};
	schci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	schci.surface = surface;
	schci.minImageCount = 3; // TODO: Think about choosing the right number
	schci.imageFormat = srffchosen->format;
	schci.imageColorSpace = srffchosen->colorSpace;
	schci.imageExtent = imgext;
	schci.imageArrayLayers = 1;
	schci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	schci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	schci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	schci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; // TODO: either FIFO Relaxed or Mailbox, with FIFO as fallback maybe

	VkSwapchainKHR sch;
	must(vkCreateSwapchainKHR(dev, &schci, NULL, &sch));
	infof("vulkan swapchain created");

	// get swapchain image handles

	uint32_t schimgc;
	must(vkGetSwapchainImagesKHR(dev, sch, &schimgc, NULL));
	VkImage *schimg = calloc(schimgc, sizeof(VkImage));
	mustPtr(schimg, "swapchain images array, len = %"PRIu32, schimgc);
	must(vkGetSwapchainImagesKHR(dev, sch, &schimgc, schimg));
	infof("obtained %"PRIu32" swapchain images", schimgc);

	s->vsch = sch;
	s->vschimgc = schimgc;
	s->vschimg = schimg;

	// create swapchain image views

	VkImageView *schimgv = calloc(schimgc, sizeof(VkImageView));
	mustPtr(schimgv, "swapchain image views array, len = %"PRIu32, schimgc);
	for (uint32_t i = 0; i < schimgc; i++) {
		VkImageViewCreateInfo ivci = {};
		ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image = s->vschimg[i];
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = srffchosen->format;
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 1;
		must(vkCreateImageView(dev, &ivci, NULL, &schimgv[i]));
	}

	// create attachments

	VkAttachmentDescription atd = {};
	atd.format = VK_FORMAT_B8G8R8A8_SRGB;
	atd.samples = VK_SAMPLE_COUNT_1_BIT;
	atd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	atd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	atd.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	atd.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	atd.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	atd.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference atr = {};
	atr.attachment = 0;
	atr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	// create subpasses

	VkSubpassDescription spd = {};
	spd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	spd.colorAttachmentCount = 1;
	spd.pColorAttachments = &atr;

	// create render pass

	VkSubpassDependency dep = {};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.srcAccessMask = 0;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo rpci = {};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 1;
	rpci.pAttachments = &atd;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &spd;
	rpci.dependencyCount = 1;
	rpci.pDependencies = &dep;
	vkCreateRenderPass(dev, &rpci, NULL, &s->rp);

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

	// create graphics pipeline

	VkGraphicsPipelineCreateInfo plci = {};
	plci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
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
	// TODO const VkPipelineDepthStencilStateCreateInfo*     pDepthStencilState;
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
	plci.renderPass = s->rp;
	plci.subpass = 0;
	plci.basePipelineHandle = VK_NULL_HANDLE;
	plci.basePipelineIndex = 0;
	must(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &plci, NULL, &s->pl));

	// create framebuffers

	s->fb = calloc(schimgc, sizeof(VkFramebuffer));
	mustPtr(s->fb, "framebuffers array, len = %"PRIu32, schimgc);
	for (uint32_t i = 0; i < schimgc; i++) {
		VkFramebufferCreateInfo fbci = {};
		fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbci.renderPass = s->rp;
		fbci.attachmentCount = 1;
		fbci.pAttachments = &schimgv[i];
		fbci.width = imgext.width;
		fbci.height = imgext.height;
		fbci.layers = 1;
		must(vkCreateFramebuffer(dev, &fbci, NULL, &s->fb[i]));
	}

	// create command pool

	VkCommandPoolCreateInfo cmdplci = {};
	cmdplci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdplci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // TODO: Transient?
	cmdplci.queueFamilyIndex = qfi;
	must(vkCreateCommandPool(dev, &cmdplci, NULL, &s->cmdpl));

	// allocate command buffers

	s->cmdb = calloc(schimgc, sizeof(VkCommandBuffer));
	mustPtr(s->cmdb, "command buffers array, len = %"PRIu32, schimgc);
	VkCommandBufferAllocateInfo cmdbai = {};
	cmdbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmdbai.commandPool = s->cmdpl;
	cmdbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmdbai.commandBufferCount = schimgc;
	must(vkAllocateCommandBuffers(dev, &cmdbai, s->cmdb));

	// create semaphores and fences

	s->imgsem = calloc(schimgc, sizeof(VkSemaphore));
	mustPtr(s->imgsem, "image semaphores array, len = %"PRIu32, schimgc);
	for (uint32_t i = 0; i < schimgc; i++)
		must(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &s->imgsem[i]));
	s->prsem = calloc(schimgc, sizeof(VkSemaphore));
	mustPtr(s->prsem, "present semaphores array, len = %"PRIu32, schimgc);
	for (uint32_t i = 0; i < schimgc; i++)
		must(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &s->prsem[i]));
	s->cmdbfen = calloc(schimgc, sizeof(VkFence));
	mustPtr(s->prsem, "command buffer fences array, len = %"PRIu32, schimgc);
	VkFenceCreateInfo cmdbfenci = {};
	cmdbfenci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	cmdbfenci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i = 0; i < schimgc; i++)
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
	uint32_t schimgi = 0;
	uint32_t schi = 0;
	while (!quit) {
		while (SDL_PollEvent(&e) != 0) {
			if (e.type == SDL_QUIT) quit = 1;
		}

		// quit = 1; // TODO: Delete when ready

		// acquire image from swap chain
		must(vkAcquireNextImageKHR(s->vdev, s->vsch, 3000000000, s->imgsem[schi], VK_NULL_HANDLE, &schimgi));

		// record command buffer

		vkWaitForFences(s->vdev, 1, &s->cmdbfen[schi], VK_TRUE, UINT64_MAX);
		vkResetFences(s->vdev, 1, &s->cmdbfen[schi]);
		printFramerate();
		// must(vkResetCommandBuffer(s->cmdb[schi], 0)); TODO this is not needed?
		must(vkBeginCommandBuffer(s->cmdb[schi], &(VkCommandBufferBeginInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}));

		VkRenderPassBeginInfo rpbi = {};
		rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		rpbi.renderPass = s->rp;
		rpbi.framebuffer = s->fb[schimgi];
		rpbi.renderArea.extent = s->imgext;
		rpbi.clearValueCount = 1;
		rpbi.pClearValues = &(VkClearValue){
			.color = (VkClearColorValue){.float32 = {1, 1, 1, 1}},
		};
		vkCmdBeginRenderPass(s->cmdb[schi], &rpbi, VK_SUBPASS_CONTENTS_INLINE);

		vkCmdBindPipeline(s->cmdb[schi], VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl);

		VkViewport vp = {};
		vp.width = s->imgext.width;
		vp.height = s->imgext.height;
		vp.minDepth = 0;
		vp.maxDepth = 1;
		vkCmdSetViewport(s->cmdb[schi], 0, 1, &vp);

		VkRect2D sc = {};
		sc.extent = s->imgext;
		vkCmdSetScissor(s->cmdb[schi], 0, 1, &sc);

		vkCmdDraw(s->cmdb[schi], 3, 1, 0, 0);
		vkCmdEndRenderPass(s->cmdb[schi]);
		must(vkEndCommandBuffer(s->cmdb[schi]));

		// submit command buffer

		VkSubmitInfo si = {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &s->imgsem[schi];
		si.pWaitDstStageMask = &(VkPipelineStageFlags){VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &s->cmdb[schi];
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &s->prsem[schi];

		must(vkQueueSubmit(s->queue, 1, &si, s->cmdbfen[schi]));

		// present swap chain image
		// BUG: we assume that the queue supports present
		VkPresentInfoKHR pi = {};
		pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &s->prsem[schi];
		pi.swapchainCount = 1;
		pi.pSwapchains = &s->vsch;
		pi.pImageIndices = &schimgi;
		must(vkQueuePresentKHR(s->queue, &pi));

		schi = (schi + 1) % s->vschimgc;
	}
}

int main() {
	State s;
	if (beginSdl(&s) != VK_SUCCESS) {
		return 1;
	}

	beginVulkan(&s);

	eventLoop(&s);

	endVulkan(&s);

	endSdl(&s);
	return 0;
}
