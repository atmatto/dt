#include <vulkan.h>

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "util.h"
#include "swapchain.h"

void swapchainSemsInit(SwapchainSems *s, VkDevice dev, uint32_t imageCount) {
	s->count = imageCount + 1;
	mustCondition(s->count <= SWAPCHAIN_SEMS_MAX,
		"swapchain semaphores count is less than maximum %"PRIu32" < %"PRIu32, s->count, SWAPCHAIN_SEMS_MAX);
	s->av_top = 0;
	for (uint32_t i = 0; i < s->count; i++) {
		must(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &s->sem[i]));
		s->av_stack[s->av_top++] = i;
	}
	for (uint32_t i = 0; i < imageCount; i++)
		s->re_map[i] = -1;
}

void swapchainSemsDestroy(SwapchainSems *s, VkDevice dev) {
	for (uint32_t i = 0; i < s->count; i++)
		vkDestroySemaphore(dev, s->sem[i], NULL);
}

uint32_t swapchainSemsReserve(SwapchainSems *s) {
	mustCondition(s->av_top > 0, "swapchain semaphores stack is not empty");
	return s->av_stack[--s->av_top];
}

void swapchainSemsRelease(SwapchainSems *s, uint32_t semIndex) {
	mustCondition(s->av_top < SWAPCHAIN_SEMS_MAX, "swapchain semaphores stack is not full");
	s->av_stack[s->av_top++] = semIndex;
} 

void swapchainsSemsAssociate(SwapchainSems *s, uint32_t semIndex, uint32_t imageIndex) {
	if (s->re_map[imageIndex] >= 0)
		swapchainSemsRelease(s, s->re_map[imageIndex]);
	s->re_map[imageIndex] = semIndex;
}

VkSurfaceFormatKHR swapchainGetFormat(VkPhysicalDevice pd, VkSurfaceKHR surf) {
	uint32_t n;
	must(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, NULL));
	VkSurfaceFormatKHR *formats = calloc(n, sizeof(VkSurfaceFormatKHR));
	mustPtr(formats, "physical device surface formats array, len = %"PRIu32, n);
	must(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, formats));
	VkSurfaceFormatKHR chosen = formats[0];
	for (uint32_t i = 0; i < n; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			chosen = formats[i];
	}
	free(formats);
	return chosen;
}

void swapchainConfigure(Swapchain *sc, VkPhysicalDevice pd, VkSurfaceKHR surf, uint32_t minCount, VkExtent2D targetExtent) {
	VkSurfaceCapabilitiesKHR caps = {};
	must(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));

	if (minCount < caps.minImageCount)
		minCount = caps.minImageCount;
	if (minCount > caps.maxImageCount)
		minCount = caps.maxImageCount;
	sc->count = minCount;

	if ((targetExtent.width == 0 || targetExtent.height == 0)
			&& (caps.currentExtent.width != 0xFFFFFFFF || caps.currentExtent.height != 0xFFFFFFFF)) {
		targetExtent = caps.currentExtent;
	} else {
		if (targetExtent.width > caps.maxImageExtent.width)
			targetExtent.width = caps.maxImageExtent.width;
		if (targetExtent.height > caps.maxImageExtent.height)
			targetExtent.height = caps.maxImageExtent.height;
		if (targetExtent.width < caps.minImageExtent.width)
			targetExtent.width = caps.minImageExtent.width;
		if (targetExtent.height < caps.minImageExtent.height)
			targetExtent.height = caps.minImageExtent.height;
	}
	sc->extent = targetExtent;
}

void swapchainInit(Swapchain *sc, VkDevice dev, VkSurfaceKHR surf, VkSurfaceFormatKHR surffmt) {
	// initialize swapchain

	VkSwapchainCreateInfoKHR schci = {};
	schci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	schci.surface = surf;
	schci.minImageCount = sc->count;
	schci.imageFormat = surffmt.format;
	schci.imageColorSpace = surffmt.colorSpace;
	schci.imageExtent = sc->extent;
	schci.imageArrayLayers = 1;
	schci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	schci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	schci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	schci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR; // TODO: use FIFO Relaxed, with FIFO as fallback

	must(vkCreateSwapchainKHR(dev, &schci, NULL, &sc->chain));

	// get swapchain image handles

	must(vkGetSwapchainImagesKHR(dev, sc->chain, &sc->count, NULL));
	sc->img = calloc(sc->count, sizeof(VkImage));
	mustPtr(sc->img, "swapchain images array, len = %"PRIu32, sc->count);
	must(vkGetSwapchainImagesKHR(dev, sc->chain, &sc->count, sc->img));

	// create swapchain image views

	sc->imgv = calloc(sc->count, sizeof(VkImageView));
	mustPtr(sc->imgv, "swapchain image views array, len = %"PRIu32, sc->count);
	for (uint32_t i = 0; i < sc->count; i++) {
		VkImageViewCreateInfo ivci = {};
		ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image = sc->img[i];
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = surffmt.format;
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 1;
		must(vkCreateImageView(dev, &ivci, NULL, &sc->imgv[i]));
	}

	// create semaphores

	sc->presReady = calloc(sc->count, sizeof(VkSemaphore));
	mustPtr(sc->presReady, "present semaphores array, len = %"PRIu32, sc->count);
	for (uint32_t i = 0; i < sc->count; i++)
		must(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}, NULL, &sc->presReady[i]));
	swapchainSemsInit(&sc->drawReady, dev, sc->count);

	infof("swapchain created (%"PRIu32" images, %"PRIu32"x%"PRIu32")", sc->count, sc->extent.width, sc->extent.height);
}

void swapchainDestroy(Swapchain *sc, VkDevice dev) {
	// semaphores
	for (uint32_t i = 0; i < sc->count; i++)
		vkDestroySemaphore(dev, sc->presReady[i], NULL);
	free(sc->presReady);
	swapchainSemsDestroy(&sc->drawReady, dev);
	// image views
	for (uint32_t i = 0; i < sc->count; i++)
		vkDestroyImageView(dev, sc->imgv[i], NULL);
	free(sc->imgv);
	// swapchain
	vkDestroySwapchainKHR(dev, sc->chain, NULL);
	// images - were destroyed by the swapchain
	free(sc->img);
	infof("swapchain destroyed");
}

void swapchainResize(Swapchain *sc, VkDevice dev, VkPhysicalDevice pd, VkSurfaceKHR surf) {
	swapchainDestroy(sc, dev);
	swapchainConfigure(sc, pd, surf, sc->count, sc->extent);
	VkSurfaceFormatKHR fmt = swapchainGetFormat(pd, surf); // TODO: This could possibly return a different format and e.g. the pipeline should be recreated then
	swapchainInit(sc, dev, surf, fmt);
}
