// Fill-rate test (derived from vktri): NDRAW full-screen blended triangles per
// frame, FRAMES frames, prints Mpixel/s. gcc -O1 -DW=1024 -DH=616 -o vkfill vkfill.c -lvulkan
// Headless Vulkan triangle: render pass + graphics pipeline + draw, then read
// the image back and write it as PPM. Proves the full geometry/fragment path
// (TA + 3D), not just the transfer queue that vkclear exercises.
// Build on the tablet:
//   glslangValidator -V tri.vert -o tri.vert.spv
//   glslangValidator -V tri.frag -o tri.frag.spv
//   gcc -O1 -o vktri vktri.c -lvulkan
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vktri out.ppm
// ponytail: fixed 256x256 R8G8B8A8, no depth, no vertex buffers (positions
// from gl_VertexIndex), shaders loaded from tri.vert.spv / tri.frag.spv.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#ifndef W
#define W 256
#endif
#ifndef H
#define H 256
#endif
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); return 1; } } while (0)

static uint32_t memtype(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

static VkShaderModule load_spv(VkDevice dev, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *code = malloc(n);
	if (fread(code, 1, n, f) != (size_t)n) { perror(path); exit(1); }
	fclose(f);
	VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
	VkShaderModule m;
	if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) { fprintf(stderr, "shader %s failed\n", path); exit(1); }
	free(code);
	return m;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "out.ppm";
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vktri", .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CK(vkCreateInstance(&ici, NULL, &inst));

	uint32_t n = 1;
	VkPhysicalDevice pd;
	CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s\n", props.deviceName);

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
	VkQueueFamilyProperties *qf = calloc(qn, sizeof(*qf));
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t qi = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
	if (qi == UINT32_MAX) { fprintf(stderr, "no graphics queue\n"); return 1; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, qi, 0, &q);

	/* colour attachment */
	const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo imci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		#ifdef LINEAR
		.tiling = VK_IMAGE_TILING_LINEAR,
#else
		.tiling = VK_IMAGE_TILING_OPTIMAL,
#endif
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImage img;
	CK(vkCreateImage(dev, &imci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = memtype(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CK(vkAllocateMemory(dev, &mai, NULL, &imem));
	CK(vkBindImageMemory(dev, img, imem, 0));
	VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	CK(vkCreateImageView(dev, &vci, NULL, &view));

	/* readback buffer */
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	CK(vkCreateBuffer(dev, &bci, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = memtype(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	VkDeviceMemory bmem;
	CK(vkAllocateMemory(dev, &bai, NULL, &bmem));
	CK(vkBindBufferMemory(dev, buf, bmem, 0));
	uint32_t *px;
	CK(vkMapMemory(dev, bmem, 0, W * H * 4, 0, (void **)&px));
	memset(px, 0xAA, W * H * 4);

	/* render pass: clear to dark blue, store */
	VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
	VkRenderPass rp;
	CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	/* pipeline */
	VkShaderModule vs = load_spv(dev, getenv("VS") ? getenv("VS") : "fill.vert.spv"), fs = load_spv(dev, "fill.frag.spv");
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	#ifndef VPW
#define VPW W
#endif
#ifndef SCW
#define SCW W
#endif
	VkViewport vp = { 0, 0, VPW, VPW, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { SCW, SCW } };
#ifndef RAW
#define RAW W
#endif
	VkRect2D ra = { { 0, 0 }, { RAW, RAW } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf, .blendEnable = getenv("NOBLEND") ? VK_FALSE : VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout pl;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
		.pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0 };
	VkPipeline pipe;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
	puts("pipeline created");

	/* record */
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi };
	VkCommandPool pool;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cmd, &bi));
	VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.25f, 1.0f } } };
	VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = ra, .clearValueCount = 1, .pClearValues = &clear };
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	const int ndraw = getenv("NDRAW") ? atoi(getenv("NDRAW")) : 10;
	for (int d = 0; d < ndraw; d++)
		vkCmdDraw(cmd, getenv("NV") ? atoi(getenv("NV")) : 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	/* order the readback after the fragment job (no external subpass dependency) */
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
	CK(vkEndCommandBuffer(cmd));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };

	const int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 20;
	VkResult wr = VK_SUCCESS;
	struct timespec t0, t1;
	for (int fr = 0; fr <= frames; fr++) {
		if (fr == 1) clock_gettime(CLOCK_MONOTONIC, &t0); /* frame 0 = warm-up */
		CK(vkQueueSubmit(q, 1, &si, fence));
		wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
		vkResetFences(dev, 1, &fence);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("%d frames x %d draws of %dx%d: %.2f ms/frame, %.1f Mpix/s\n", frames, ndraw, W, H, sec * 1000 / frames,
	       (double)frames * ndraw * W * H / sec / 1e6);
	printf("wait: %d\n", wr);
	if (wr != VK_SUCCESS) return 2;

	/* check: centre inside the triangle, corner outside */
	uint32_t centre = px[(H / 2) * W + W / 2], corner = px[0];
	uint32_t inside = 0;
	for (uint32_t i = 0; i < W * H; i++)
		if ((px[i] & 0x00ffffff) == 0x000000ff) inside++; /* pure red (ABGR in memory) */
	printf("centre=0x%08x corner=0x%08x red_pixels=%u (expect centre red 0xff0000ff, corner 0xff400000, ~%u red)\n",
	       centre, corner, inside, W * H / 2);

	FILE *f = fopen(out, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (uint32_t i = 0; i < W * H; i++) {
			unsigned char rgb[3] = { px[i] & 0xff, (px[i] >> 8) & 0xff, (px[i] >> 16) & 0xff };
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
	}
	/* tear down so the driver frees (and can debug-dump) the render target */
	vkDeviceWaitIdle(dev);
	vkDestroyFramebuffer(dev, fb, NULL);
	vkDestroyRenderPass(dev, rp, NULL);
	vkDestroyDevice(dev, NULL);

	int ok = (centre & 0x00ffffff) == 0x000000ff && (corner & 0x00ffffff) == 0x00400000 && inside > W * H / 4;
	puts(ok ? "PASS: triangle rendered" : "FAIL");
	return ok ? 0 : 3;
}
