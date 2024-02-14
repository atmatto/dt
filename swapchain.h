// A semaphore is needed when acquiring an image, before its index is known.
// This structure should be used as follows:
// - Reserve a semaphore and use it when acquiring an image
// - Associate the semaphore with the returned image index
//   -> Release the semaphore previously associated with this index (automatic)
// If for some reason the semaphore index won't be associated with an image, it
// must be released manually.
#define SWAPCHAIN_SEMS_MAX 16
typedef struct SwapchainSems {
	uint32_t count;
	VkSemaphore sem[SWAPCHAIN_SEMS_MAX];
	// stack of available indices
	uint32_t av_stack[SWAPCHAIN_SEMS_MAX];
	uint32_t av_top; // index one past the top element
	// map of reserved indices (image index -> semaphore index or -1)
	int32_t re_map[SWAPCHAIN_SEMS_MAX];
} SwapchainSems;

void swapchainSemsInit(SwapchainSems *s, VkDevice dev, uint32_t imageCount);

void swapchainSemsDestroy(SwapchainSems *s, VkDevice dev);

uint32_t swapchainSemsReserve(SwapchainSems *s);

void swapchainSemsRelease(SwapchainSems *s, uint32_t semIndex);

void swapchainsSemsAssociate(SwapchainSems *s, uint32_t semIndex, uint32_t imageIndex);

typedef struct Swapchain {
	uint32_t count;
	VkSwapchainKHR chain;
	VkExtent2D extent;
	VkImage *img;
	VkImageView *imgv;
	SwapchainSems drawReady; // image is ready to be drawn to
	VkSemaphore *presReady; // image is ready to be presented
} Swapchain;

VkSurfaceFormatKHR swapchainGetFormat(VkPhysicalDevice pd, VkSurfaceKHR surf);

void swapchainConfigure(Swapchain *sc, VkPhysicalDevice pd, VkSurfaceKHR surf, uint32_t minCount, VkExtent2D targetExtent);

void swapchainInit(Swapchain *sc, VkDevice dev, VkSurfaceKHR surf, VkSurfaceFormatKHR surffmt);

// caller has to ensure that the resources are no longer in use
void swapchainDestroy(Swapchain *sc, VkDevice dev);

// caller has to ensure that the resources are no longer in use
void swapchainResize(Swapchain *sc, VkDevice dev, VkPhysicalDevice pd, VkSurfaceKHR surf);
