#include "test_pattern.h"

#include "memory_allocator.h"

#include "spirv_pattern.h"

test_pattern::test_pattern(vk::PhysicalDevice phys_dev, vk::Device dev, vk::Extent2D extent) :
        device(dev), extent(extent)
{
	std::array formats = {vk::Format::eR8Unorm, vk::Format::eR8G8Unorm};

	mini_vma memory_allocator;

	for (int i = 0; i < 2; ++i)
	{
		auto & img = i == 0 ? img_y : img_uv;
		img = dev.createImage({
		        .imageType = vk::ImageType::e2D,
		        .format = formats[i],
		        .extent = {extent.width / (i + 1), extent.height / (i + 1), 1},
		        .mipLevels = 1,
		        .arrayLayers = 1,
		        .samples = vk::SampleCountFlagBits::e1,
		        .tiling = vk::ImageTiling::eOptimal,
		        .usage = vk::ImageUsageFlagBits::eStorage |
		                 vk::ImageUsageFlagBits::eTransferSrc,
		        .sharingMode = vk::SharingMode::eExclusive,
		});

		memory_allocator.request(
		        device.getImageMemoryRequirements(img), [this, img](vk::DeviceMemory mem, size_t offset) {
			        device.bindImageMemory(img, mem, offset);
		        },
		        vk::MemoryPropertyFlagBits::eDeviceLocal);
	}
	mem = memory_allocator.alloc_and_bind(phys_dev, dev);

	for (int i = 0; i < 2; ++i)
	{
		auto & view = i == 0 ? view_y : view_uv;

		view = dev.createImageView(
		        {
		                .image = i == 0 ? img_y : img_uv,
		                .viewType = vk::ImageViewType::e2D,
		                .format = formats[i],
		                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                                     .baseMipLevel = 0,
		                                     .levelCount = 1,
		                                     .baseArrayLayer = 0,
		                                     .layerCount = 1},
		        });
	}

	std::array ds_layout_binding{
	        vk::DescriptorSetLayoutBinding{
	                .binding = 0,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	        vk::DescriptorSetLayoutBinding{
	                .binding = 1,
	                .descriptorType = vk::DescriptorType::eStorageImage,
	                .descriptorCount = 1,
	                .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        },
	};

	ds_layout = dev.createDescriptorSetLayout({
	        .bindingCount = ds_layout_binding.size(),
	        .pBindings = ds_layout_binding.data(),
	});

	vk::PushConstantRange pc_range{
	        .stageFlags = vk::ShaderStageFlagBits::eCompute,
	        .offset = 0,
	        .size = sizeof(push_constant),
	};

	layout = device.createPipelineLayout({
	        .setLayoutCount = 1,
	        .pSetLayouts = &ds_layout,
	        .pushConstantRangeCount = 1,
	        .pPushConstantRanges = &pc_range,
	});

	{
		auto shader = device.createShaderModule({
		        .codeSize = sizeof(spirv_pattern),
		        .pCode = spirv_pattern,
		});

		vk::Result res;
		std::tie(res, pipeline) = device.createComputePipeline(
		        nullptr, vk::ComputePipelineCreateInfo{
		                         .stage = {
		                                 .stage = vk::ShaderStageFlagBits::eCompute,
		                                 .module = shader,
		                                 .pName = "main",
		                         },
		                         .layout = layout,
		                 });
		device.destroyShaderModule(shader);
	}

	vk::DescriptorPoolSize pool_size{
	        .type = vk::DescriptorType::eStorageImage,
	        .descriptorCount = 2,
	};

	dp = dev.createDescriptorPool({
	        .maxSets = 2,
	        .poolSizeCount = 1,
	        .pPoolSizes = &pool_size,
	});

	ds = dev.allocateDescriptorSets({
	        .descriptorPool = dp,
	        .descriptorSetCount = 1,
	        .pSetLayouts = &ds_layout,
	})[0];

	vk::DescriptorImageInfo desc_img_info_y{
	        .imageView = view_y,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};
	vk::DescriptorImageInfo desc_img_info_cb_cr{
	        .imageView = view_uv,
	        .imageLayout = vk::ImageLayout::eGeneral,
	};

	dev.updateDescriptorSets(
	        {
	                vk::WriteDescriptorSet{
	                        .dstSet = ds,
	                        .dstBinding = 0,
	                        .descriptorCount = 1,
	                        .descriptorType = vk::DescriptorType::eStorageImage,
	                        .pImageInfo = &desc_img_info_y,
	                },
	                vk::WriteDescriptorSet{
	                        .dstSet = ds,
	                        .dstBinding = 1,
	                        .descriptorCount = 1,
	                        .descriptorType = vk::DescriptorType::eStorageImage,
	                        .pImageInfo = &desc_img_info_cb_cr,
	                },
	        },
	        nullptr);
}

void test_pattern::record_draw_commands(vk::CommandBuffer cmd_buf)
{
	std::array im_barriers = {
	        vk::ImageMemoryBarrier2{
	                .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
	                .srcAccessMask = vk::AccessFlagBits2::eNone,
	                .dstStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = img_y,
	                .subresourceRange = {.aspectMask =
	                                             vk::ImageAspectFlagBits::eColor,
	                                     .baseMipLevel = 0,
	                                     .levelCount = 1,
	                                     .baseArrayLayer = 0,
	                                     .layerCount = 1},
	        },
	        vk::ImageMemoryBarrier2{
	                .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
	                .srcAccessMask = vk::AccessFlagBits2::eNone,
	                .dstStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = img_uv,
	                .subresourceRange = {.aspectMask =
	                                             vk::ImageAspectFlagBits::eColor,
	                                     .baseMipLevel = 0,
	                                     .levelCount = 1,
	                                     .baseArrayLayer = 0,
	                                     .layerCount = 1},
	        },
	};
	vk::DependencyInfo dep_info{};
	dep_info.setImageMemoryBarriers(im_barriers);
	cmd_buf.pipelineBarrier2(dep_info);

	cmd_buf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
	cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, ds, {});
	cmd_buf.pushConstants<push_constant>(layout, vk::ShaderStageFlagBits::eCompute, 0, counter);
	cmd_buf.dispatch(extent.width/16, extent.height/16, 1);

	for (auto & barrier: im_barriers)
	{
		barrier.srcStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader;
		barrier.srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite;
		barrier.dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer;
		barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
		barrier.oldLayout = vk::ImageLayout::eGeneral;
		barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
	}
	cmd_buf.pipelineBarrier2(dep_info);
	counter += 10;

}
