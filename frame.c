// handling multiple frames in flight

#include <vulkan.h>

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "util.h"
#include "frame.h"

void frameInit(Frame *f, VkDevice dev, VkCommandPool cmdpl) {
	// f->cmdbuf
	VkCommandBufferAllocateInfo cmdbai = {};
	cmdbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmdbai.commandPool = cmdpl;
	cmdbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmdbai.commandBufferCount = 1;
	must(vkAllocateCommandBuffers(dev, &cmdbai, &f->cmdbuf));
	// f->ready
	VkFenceCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	must(vkCreateFence(dev, &fci, NULL, &f->ready));
}

// caller has to ensure that the resources are no longer in use
void frameDestroy(Frame *f, VkDevice dev, VkCommandPool cmdpl) {
	// f->cmdbuf
	vkFreeCommandBuffers(dev, cmdpl, 1, &f->cmdbuf);
	// f->ready
	vkDestroyFence(dev, f->ready, NULL);
}

// f->count must be set to the expected value
// queueFamilyIndex is used for creating a command pool
void framesInit(Frames *f, VkDevice dev, uint32_t queueFamilyIndex) {
	f->current = 0;
	// f->cmdpl
	VkCommandPoolCreateInfo cmdplci = {};
	cmdplci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmdplci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cmdplci.queueFamilyIndex = queueFamilyIndex;
	must(vkCreateCommandPool(dev, &cmdplci, NULL, &f->cmdpl));
	// f->frames
	f->frames = calloc(f->count, sizeof(Frame));
	mustPtr(f->frames, "framesInit: f->frames, len = %"PRIu32, f->count);
	for (uint32_t i = 0; i < f->count; i++) {
		frameInit(&f->frames[i], dev, f->cmdpl);
	}
}

// caller has to ensure that the resources are no longer in use
void framesDestroy(Frames *f, VkDevice dev) {
	// f->frames
	for (uint32_t i = 0; i < f->count; i++) {
		frameDestroy(&f->frames[i], dev, f->cmdpl);
	}
	free(f->frames);
	f->frames = NULL;
	// f->cmdpl
	vkDestroyCommandPool(dev, f->cmdpl, NULL);
}

Frame *framesNext(Frames *f) {
	f->current = (f->current + 1) % f->count;
	return &f->frames[f->current];
}
