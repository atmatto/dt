// handling multiple frames in flight

typedef struct Frame {
	VkCommandBuffer cmdbuf;
	VkFence ready;
} Frame;

typedef struct Frames {
	uint32_t count;
	uint32_t current;
	VkCommandPool cmdpl;
	Frame *frames;
} Frames;

void frameInit(Frame *f, VkDevice dev, VkCommandPool cmdpl);

// caller has to ensure that the resources are no longer in use
void frameDestroy(Frame *f, VkDevice dev, VkCommandPool cmdpl);

// f->count must be set to the expected value
// queueFamilyIndex is used for creating a command pool
void framesInit(Frames *f, VkDevice dev, uint32_t queueFamilyIndex);

// caller has to ensure that the resources are no longer in use
void framesDestroy(Frames *f, VkDevice dev);

Frame *framesNext(Frames *f);
