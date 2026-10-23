// Clear stress: NIMG optimal images, each submit clears one of them with
// vkCmdClearColorImage (transfer queue path), REPEAT submits without waiting
// in between (like vkmark's "clear" scene).
//   gcc -O1 -o vkclr vkclr.c -lvulkan && REPEAT=20000 ./vkclr 640 400
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)
#define NIMG 3
int main(int argc, char **argv)
{
	uint32_t W = argc > 1 ? atoi(argv[1]) : 640, H = argc > 2 ? atoi(argv[2]) : 400;
	int repeat = getenv("REPEAT") ? atoi(getenv("REPEAT")) : 20000;
	VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
	VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	VkImage img[NIMG];
	VkCommandBuffer cmd[NIMG];
	VkFence fence[NIMG];
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = NIMG };
	CK(vkAllocateCommandBuffers(dev, &cbai, cmd));
	for (int i = 0; i < NIMG; i++) {
		VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = getenv("FMT") ? (VkFormat)atoi(getenv("FMT")) : VK_FORMAT_B8G8R8A8_UNORM,
			.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
		CK(vkCreateImage(dev, &ci, NULL, &img[i]));
		VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img[i], &mr);
		uint32_t t = 0; while (!(mr.memoryTypeBits & (1u << t))) t++;
		VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
		VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m)); CK(vkBindImageMemory(dev, img[i], m, 0));
		VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
		CK(vkCreateFence(dev, &fci, NULL, &fence[i]));
	}
	VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	const int churn = getenv("CHURN") ? atoi(getenv("CHURN")) : 0; /* GPU allocs freed per frame */
	for (int f = 0; f < repeat; f++) {
		int i = f % NIMG;
		/* churn GPU virtual addresses: allocate + free buffers so VAs get reused */
		for (int c = 0; c < churn; c++) {
			VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 65536 * (1 + c % 4),
				.memoryTypeIndex = 0 };
			VkDeviceMemory m;
			CK(vkAllocateMemory(dev, &ai, NULL, &m));
			vkFreeMemory(dev, m, NULL);
		}
		/* like a swapchain: wait for this image's previous frame, re-record, submit */
		CK(vkWaitForFences(dev, 1, &fence[i], VK_TRUE, 10ull * 1000 * 1000 * 1000));
		CK(vkResetFences(dev, 1, &fence[i]));
		CK(vkResetCommandBuffer(cmd[i], 0));
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CK(vkBeginCommandBuffer(cmd[i], &bi));
		VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i], .subresourceRange = rr };
		vkCmdPipelineBarrier(cmd[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
		VkClearColorValue cc = { .float32 = { (f % 7) / 7.0f, (f % 11) / 11.0f, 0.5f, 1.0f } };
		vkCmdClearColorImage(cmd[i], img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
		CK(vkEndCommandBuffer(cmd[i]));
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[i] };
		CK(vkQueueSubmit(q, 1, &si, fence[i]));
	}
	CK(vkQueueWaitIdle(q));
	puts("done");
	return 0;
}
