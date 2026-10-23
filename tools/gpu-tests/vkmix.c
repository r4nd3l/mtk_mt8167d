// vkmix: vkscene's render (depth + texture), then bursts of transfer clears on
// the same colour image, repeated MIX times (reproducer for the sporadic TQ lockup
// seen with vkmark cube -> clear). Env: MIX, RENDERN, CLEARN.
// Headless Vulkan correctness scene: depth test, a sampled texture (uploaded
// through a staging buffer), push constants, varyings and a descriptor set.
// Writes the frame as PPM so it can be diffed against lavapipe:
//   glslangValidator -V scene.vert -o scene.vert.spv; glslangValidator -V scene.frag -o scene.frag.spv
//   gcc -O1 -o vkscene vkscene.c -lvulkan
//   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vkscene gpu.ppm
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./vkscene ref.ppm   (on the PC)
// ponytail: nearest sampling + flat-ish colours so the GPU and lavapipe frames
// should match almost exactly; compare with cmp.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifndef W
#define W 320
#endif
#ifndef H
#define H 200
#endif
#define TS 64
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s -> %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(1);
}

static VkDeviceMemory bind_mem(VkMemoryRequirements mr, VkMemoryPropertyFlags want)
{
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = memtype(mr.memoryTypeBits, want) };
	VkDeviceMemory m;
	CK(vkAllocateMemory(dev, &ai, NULL, &m));
	return m;
}

static VkImage make_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImageView *view)
{
	VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
		.extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img;
	CK(vkCreateImage(dev, &ci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	CK(vkBindImageMemory(dev, img, bind_mem(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
	VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = fmt, .subresourceRange = { aspect, 0, 1, 0, 1 } };
	CK(vkCreateImageView(dev, &vci, NULL, view));
	return img;
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
	VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
	VkBuffer b;
	CK(vkCreateBuffer(dev, &ci, NULL, &b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, b, &mr);
	VkDeviceMemory m = bind_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CK(vkBindBufferMemory(dev, b, m, 0));
	CK(vkMapMemory(dev, m, 0, size, 0, map));
	return b;
}

static VkShaderModule load_spv(const char *path)
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
	CK(vkCreateShaderModule(dev, &ci, NULL, &m));
	free(code);
	return m;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
		    VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
	VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
		.oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "out.ppm";
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkscene", .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s\n", props.deviceName);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkFormat dfmt = getenv("DFMT") ? (VkFormat)atoi(getenv("DFMT")) : getenv("D16") ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(pd, dfmt, &fp);
	if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		dfmt = VK_FORMAT_D16_UNORM;
	printf("depth format %d\n", dfmt);

	const VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageView cview, dview, tview;
	VkImage cimg = make_image(cfmt, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &cview);
	make_image(dfmt, W, H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &dview);
	VkImage timg = make_image(cfmt, TS, TS, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &tview);

	/* texture: 8x8 px checker of white / grey with a gradient so orientation errors show */
	uint32_t *tpx;
	VkBuffer tbuf = make_buffer(TS * TS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, (void **)&tpx);
	for (int y = 0; y < TS; y++)
		for (int x = 0; x < TS; x++) {
			unsigned v = ((x / 8) ^ (y / 8)) & 1 ? 255 : 96;
			tpx[y * TS + x] = 0xff000000u | (v << 16) | ((unsigned)(y * 4) << 8) | v;
		}
	uint32_t *px;
	VkBuffer rbuf = make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&px);
	memset(px, 0xAA, W * H * 4);

	VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0 };
	VkSampler smp;
	CK(vkCreateSampler(dev, &sci, NULL, &smp));

	VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL } };
	VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorImageInfo dii = { smp, tview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
	float *ud;
	VkBuffer ubuf = make_buffer(13 * 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, (void **)&ud);
	const float U[13][4] = {
		{ -0.9, 0.9, 0.6, 1 }, { 0.5, 0.9, 0.6, 1 }, { -0.2, -0.7, 0.6, 1 },
		{ -0.5, 0.8, 0.3, 1 }, { 0.9, 0.8, 0.3, 1 }, { 0.2, -0.9, 0.3, 1 },
		{ -1.0, -1.0, 0.9, 1 }, { 1.0, -1.0, 0.9, 1 }, { 0.0, 1.0, 0.9, 1 },
		{ 1, 0.2, 0.2, 1 }, { 0.2, 1, 0.2, 1 }, { 0.3, 0.3, 1, 1 }, { 1, 1, 1, 1 } };
	memcpy(ud, U, sizeof(U));
	VkDescriptorBufferInfo dbi = { ubuf, 0, sizeof(U) };
	VkWriteDescriptorSet wds2[2] = { wds, { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi } };
	vkUpdateDescriptorSets(dev, 2, wds2, 0, NULL);

	VkAttachmentDescription att[2] = {
		{ .format = cfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
		{ .format = dfmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
	};
	VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cref,
		.pDepthStencilAttachment = &dref };
	#ifdef NODEPTH
	sub.pDepthStencilAttachment = NULL;
	const uint32_t natt = 1;
#else
	const uint32_t natt = 2;
#endif
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = natt, .pAttachments = att, .subpassCount = 1, .pSubpasses = &sub };
	VkRenderPass rp;
	CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkImageView views[2] = { cview, dview };
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = natt, .pAttachments = views,
		.width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = load_spv(getenv("VERT") ? getenv("VERT") : "scene.vert.spv"), .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = load_spv(getenv("FRAG") ? getenv("FRAG") : "scene.frag.spv"), .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = getenv("CULL") ? atoi(getenv("CULL")) : VK_CULL_MODE_NONE, .frontFace = getenv("FRONT") ? atoi(getenv("FRONT")) : VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
		.pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		
#ifndef NODEPTH
		.pDepthStencilState = &dss,
#endif
		 .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
	VkPipeline pipe;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cmd, &bi));

#ifndef NOUPLOAD
	barrier(cmd, timg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy up = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TS, TS, 1 } };
#if defined(UPCLEAR)
	{
		VkClearColorValue cc = { .float32 = { 1, 1, 1, 1 } };
		VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
	}
#elif defined(UPNONE)
	(void)up;
#else
	vkCmdCopyBufferToImage(cmd, tbuf, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
#endif
	barrier(cmd, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

#ifdef SPLIT
	{
		CK(vkEndCommandBuffer(cmd));
		VkSubmitInfo usi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
		CK(vkQueueSubmit(q, 1, &usi, VK_NULL_HANDLE));
		CK(vkQueueWaitIdle(q));
		CK(vkResetCommandBuffer(cmd, 0));
		CK(vkBeginCommandBuffer(cmd, &bi));
	}
#endif
#endif
	VkClearValue clear[2] = { { .color = { .float32 = { 0.1f, 0.1f, 0.15f, 1.0f } } }, { .depthStencil = { 1.0f, 0 } } };
	VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = natt, .pClearValues = clear };
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
	const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, tint);
	vkCmdDraw(cmd, 9, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
#ifndef NOBARRIER
	/* the render pass has no external dependency, so order the readback explicitly */
	{
		VkImageMemoryBarrier rb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = cimg, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &rb);
	}
#endif
	VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cmd, cimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
	CK(vkEndCommandBuffer(cmd));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
#ifdef TWICE
	CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(q));
	memset(px, 0xAA, W * H * 4);
#endif
	CK(vkQueueSubmit(q, 1, &si, fence));
	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
	printf("wait: %d\n", wr);
	if (wr != VK_SUCCESS) return 2;

	{
		const int mix = getenv("MIX") ? atoi(getenv("MIX")) : 20;
		const int rendern = getenv("RENDERN") ? atoi(getenv("RENDERN")) : 300;
		const int clearn = getenv("CLEARN") ? atoi(getenv("CLEARN")) : 1000;
		VkCommandBuffer ccmd[3];
		VkFence cf[3];
		VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3 };
		CK(vkAllocateCommandBuffers(dev, &cai, ccmd));
		for (int k = 0; k < 3; k++) {
			VkFenceCreateInfo sf = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
			CK(vkCreateFence(dev, &sf, NULL, &cf[k]));
		}
		VkImageSubresourceRange crr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		for (int m = 0; m < mix; m++) {
			/* render burst: same command buffer, waited per frame (it is not simultaneous-use) */
			for (int r = 0; r < rendern; r++) {
				CK(vkResetFences(dev, 1, &fence));
				CK(vkQueueSubmit(q, 1, &si, fence));
				CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
			}
			/* clear burst, like vkmark's clear scene: 3 cmd buffers, re-recorded */
			for (int c = 0; c < clearn; c++) {
				int k = c % 3;
				CK(vkWaitForFences(dev, 1, &cf[k], VK_TRUE, 10ull * 1000 * 1000 * 1000));
				CK(vkResetFences(dev, 1, &cf[k]));
				CK(vkResetCommandBuffer(ccmd[k], 0));
				VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT };
				CK(vkBeginCommandBuffer(ccmd[k], &cbi));
				VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = cimg, .subresourceRange = crr };
				vkCmdPipelineBarrier(ccmd[k], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
				VkClearColorValue cc = { .float32 = { (c % 7) / 7.0f, (c % 5) / 5.0f, 0.3f, 1.0f } };
				vkCmdClearColorImage(ccmd[k], cimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &crr);
				b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
				vkCmdPipelineBarrier(ccmd[k], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
				CK(vkEndCommandBuffer(ccmd[k]));
				VkSubmitInfo csi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &ccmd[k] };
				CK(vkQueueSubmit(q, 1, &csi, cf[k]));
			}
			CK(vkQueueWaitIdle(q));
		}
		puts("mix done");
	}
	FILE *f = fopen(out, "wb");
	if (!f) { perror(out); return 1; }
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (uint32_t i = 0; i < W * H; i++) {
		unsigned char rgb[3] = { px[i] & 0xff, (px[i] >> 8) & 0xff, (px[i] >> 16) & 0xff };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	puts("written");
	return 0;
}
