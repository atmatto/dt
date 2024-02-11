// requires:
// #include <stdlib.h>
// #include <stdio.h>

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
