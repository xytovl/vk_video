#include <fstream>
#include <iostream>
#include <vector>
#include <vulkan/vulkan.hpp>

#include "test_pattern.h"
#include "video_encoder_h264.h"

// Use random frame as a reference, randomly insert references
// #define DPB_CHAOS_MODE

struct queue
{
	vk::Queue queue;
	uint32_t familyIndex;
};

auto make_device(vk::Instance & instance)
{
	for (auto d: instance.enumeratePhysicalDevices())
	{
		auto props = d.enumerateDeviceExtensionProperties();
		auto [feat, feat_11, feat_12, feat_13] =
		        d.getFeatures2<vk::PhysicalDeviceFeatures2,
		                       vk::PhysicalDeviceVulkan11Features,
		                       vk::PhysicalDeviceVulkan12Features,
		                       vk::PhysicalDeviceVulkan13Features>();
		if (not feat_13.synchronization2)
			continue;
		vk::DeviceCreateInfo create_info{.pNext = &feat};
		std::vector<const char *> required_extensions = {
		        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
		        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
		        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
		        // VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
		};
		for (const auto & ext: required_extensions)
		{
			if (std::ranges::find_if(props, [ext](auto el) {
				    return ext == std::string(el.extensionName);
			    }) == props.end())
			{
				throw std::runtime_error("Missing device extension " +
				                         std::string(ext));
			}
		}
		create_info.setPEnabledExtensionNames(required_extensions);

		queue encode_queue{nullptr, 0};
		queue gfx_queue{nullptr, 0};

		std::vector<vk::DeviceQueueCreateInfo> queue_info{};
		auto queues = d.getQueueFamilyProperties();
		uint32_t i = 0;
		float prio = 0.5;
		for (const auto & q: queues)
		{
			if (q.queueFlags & vk::QueueFlagBits::eVideoEncodeKHR)
			{
				encode_queue.familyIndex = i;
				queue_info.push_back({
				        .queueFamilyIndex = i,
				        .queueCount = 1,
				});
				queue_info.back().setQueuePriorities(prio);
			}
			if (q.queueFlags & vk::QueueFlagBits::eGraphics)
			{
				gfx_queue.familyIndex = i;
				queue_info.push_back({
				        .queueFamilyIndex = i,
				        .queueCount = 1,
				});
				queue_info.back().setQueuePriorities(prio);
			}
			if (queue_info.size() == 2)
				break;
			++i;
		}
		if (queue_info.size() != 2)
		{
			throw std::runtime_error("No suitable queue for video encode");
		}
		create_info.setQueueCreateInfos(queue_info);

		auto dev = d.createDevice(create_info);
		encode_queue.queue = dev.getQueue(encode_queue.familyIndex, 0);
		gfx_queue.queue = dev.getQueue(gfx_queue.familyIndex, 0);
		return std::make_tuple(d, dev, encode_queue, gfx_queue);
	}
	throw std::runtime_error("No vulkan device available");
}

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

int main(int /*argc*/, char ** /*argv*/)
{
	try
	{
		VULKAN_HPP_DEFAULT_DISPATCHER.init();
		std::ofstream out("out.h264", std::ios::trunc);

		vk::Extent2D extent{1920, 1080};

		vk::ApplicationInfo app_info{
		        .pApplicationName = "vk_video test",
		        .applicationVersion = 1,
		        .pEngineName = "test engine",
		        .engineVersion = 1,
		        .apiVersion = VK_API_VERSION_1_3,
		};
		auto instance = vk::createInstance({
		        .pApplicationInfo = &app_info,
		});
		VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

		auto [phys_dev, dev, encode_queue, gfx_queue] = make_device(instance);
		VULKAN_HPP_DEFAULT_DISPATCHER.init(dev);

		auto command_pool = dev.createCommandPool({
		        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		        .queueFamilyIndex = gfx_queue.familyIndex,
		});

		auto command_buffer = 
		        dev.allocateCommandBuffers({.commandPool = command_pool,
		                                    .commandBufferCount = 1})[0];

		test_pattern pattern(phys_dev, dev, extent);
		auto sem = dev.createSemaphore(vk::SemaphoreCreateInfo{});
		auto fence = dev.createFence(vk::FenceCreateInfo{});

		auto encoder = video_encoder_h264::create(phys_dev, dev, encode_queue.queue, encode_queue.familyIndex, extent);

		{
			auto sps_pps = encoder->get_sps_pps();
			out.write((char *)sps_pps.data(), sps_pps.size());
		}

		for (int frame = 0; frame < 120; ++frame)
		{
			std::cerr << "frame " << frame << std::endl;
			// test pattern
			{
				command_buffer.reset();
				command_buffer.begin(vk::CommandBufferBeginInfo{});
				pattern.record_draw_commands(command_buffer);

				vk::ImageMemoryBarrier2 barrier{
				        .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
				        .srcAccessMask = vk::AccessFlagBits2::eNone,
				        .dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
				        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
				        .oldLayout = vk::ImageLayout::eUndefined,
				        .newLayout = vk::ImageLayout::eTransferDstOptimal,
				        .image = encoder->input_image,
				        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
				                             .baseMipLevel = 0,
				                             .levelCount = 1,
				                             .baseArrayLayer = 0,
				                             .layerCount = 1},
				};
				vk::DependencyInfo dep_info{
				        .imageMemoryBarrierCount = 1,
				        .pImageMemoryBarriers = &barrier,
				};
				command_buffer.pipelineBarrier2(dep_info);

				command_buffer.copyImage(
				        pattern.img_y,
				        vk::ImageLayout::eTransferSrcOptimal,
				        encoder->input_image,
				        vk::ImageLayout::eTransferDstOptimal,
				        vk::ImageCopy{.srcSubresource = {
				                              .aspectMask = vk::ImageAspectFlagBits::eColor,
				                              .layerCount = 1,
				                      },
				                      .dstSubresource = {
				                              .aspectMask = vk::ImageAspectFlagBits::ePlane0,
				                              .layerCount = 1,
				                      },
				                      .extent = {extent.width, extent.height, 1}});
				command_buffer.copyImage(
				        pattern.img_uv,
				        vk::ImageLayout::eTransferSrcOptimal,
				        encoder->input_image,
				        vk::ImageLayout::eTransferDstOptimal,
				        vk::ImageCopy{.srcSubresource = {
				                              .aspectMask = vk::ImageAspectFlagBits::eColor,
				                              .layerCount = 1,
				                      },
				                      .dstSubresource = {
				                              .aspectMask = vk::ImageAspectFlagBits::ePlane1,
				                              .layerCount = 1,
				                      },
				                      .extent = {extent.width / 2, extent.height / 2, 1}});

				barrier.srcStageMask = vk::PipelineStageFlagBits2KHR::eTransfer;
				barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
				barrier.dstStageMask = vk::PipelineStageFlagBits2KHR::eTopOfPipe;
				barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
				barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal,
				barrier.newLayout = vk::ImageLayout::eVideoEncodeSrcKHR,
				barrier.srcQueueFamilyIndex = gfx_queue.familyIndex,
				barrier.dstQueueFamilyIndex = encode_queue.familyIndex,
				command_buffer.pipelineBarrier2(dep_info);

				command_buffer.end();

				vk::SubmitInfo submit{};
				submit.setCommandBuffers(command_buffer);
				submit.setSignalSemaphores(sem);
				gfx_queue.queue.submit(submit, fence);
			}

			auto encoded_data = encoder->encode_frame(sem, gfx_queue.familyIndex);
			out.write((char *)encoded_data.data(), encoded_data.size_bytes());
			if (auto res = dev.waitForFences(fence, true, 1'000'000'000);
			    res != vk::Result::eSuccess)
			{
				throw std::runtime_error("wait for fences: " + vk::to_string(res));
			}
			dev.resetFences(fence);
		}
		// FIXME: normal exit
		out.flush();
		std::quick_exit(0);
	}
	catch (std::exception & e)
	{
		std::cerr << "error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
