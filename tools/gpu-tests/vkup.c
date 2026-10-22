// Round trip: buffer -> optimal image (vkCmdCopyBufferToImage) -> buffer.
// Checks the upload path of the transfer queue in isolation.
//   gcc -O1 -o vkup vkup.c -lvulkan && PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vkup [size]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)
static VkPhysicalDevice pd;
static VkDevice dev;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	exit(1);
}
static VkBuffer mkbuf(VkDeviceSize sz, VkBufferUsageFlags u, uint32_t **map)
{
	VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = u };
	VkBuffer b; CK(vkCreateBuffer(dev, &ci, NULL, &b));
	VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m)); CK(vkBindBufferMemory(dev, b, m, 0));
	CK(vkMapMemory(dev, m, 0, sz, 0, (void **)map));
	return b;
}
int main(int argc, char **argv)
{
	uint32_t S = argc > 1 ? atoi(argv[1]) : 64;
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
	VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { S, S, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
	VkImage img; CK(vkCreateImage(dev, &ci, NULL, &img));
	VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory im; CK(vkAllocateMemory(dev, &ai, NULL, &im)); CK(vkBindImageMemory(dev, img, im, 0));
	uint32_t *src, *dst;
	VkBuffer sb = mkbuf(S * S * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src), db = mkbuf(S * S * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst);
	for (uint32_t i = 0; i < S * S; i++) src[i] = 0xff000000u | i * 2654435761u >> 8;
	memset(dst, 0xAA, S * S * 4);
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
	VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cmd, &bi));
	VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { S, S, 1 } };
	vkCmdCopyBufferToImage(cmd, sb, img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
	b.oldLayout = b.newLayout; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_GENERAL, db, 1, &cp);
	CK(vkEndCommandBuffer(cmd));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(q));
	uint32_t bad = 0, first = UINT32_MAX;
	for (uint32_t i = 0; i < S * S; i++)
		if (dst[i] != src[i]) { bad++; if (first == UINT32_MAX) first = i; }
	printf("%ux%u round trip: %u/%u wrong", S, S, bad, S * S);
	if (bad) printf(", first at %u: got %08x want %08x", first, dst[first], src[first]);
	puts(bad ? "  FAIL" : "  PASS");
	return bad != 0;
}
