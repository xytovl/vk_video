#include "video_encoder.h"

#include <iostream>
#include <memory>
#include <stdexcept>

#include "memory_allocator.h"

vk::VideoFormatPropertiesKHR video_encoder::select_video_format(
        vk::PhysicalDevice physical_device,
        const vk::PhysicalDeviceVideoFormatInfoKHR & format_info)
{
	for (const auto & video_fmt_prop: physical_device.getVideoFormatPropertiesKHR(format_info))
	{
		// TODO: do something smart if there is more than one
		return video_fmt_prop;
	}
	throw std::runtime_error("No suitable image format found");
}

void video_encoder::init(vk::PhysicalDevice physical_device,
                         const vk::VideoCapabilitiesKHR & video_caps,
                         const vk::VideoEncodeCapabilitiesKHR & encode_caps,
                         const vk::VideoProfileInfoKHR & video_profile,
                         void * video_session_create_next,
                         void * session_params_next)
{
	static const uint32_t num_dpb_slots = 4;

	fence = device.createFence({});

	mini_vma mem_allocator;

	vk::VideoProfileListInfoKHR video_profile_list{
	        .profileCount = 1,
	        .pProfiles = &video_profile,
	};

	// Input image
	vk::VideoFormatPropertiesKHR picture_format;
	{
		vk::PhysicalDeviceVideoFormatInfoKHR video_fmt{
		        .pNext = &video_profile_list,
		        .imageUsage = vk::ImageUsageFlagBits::eVideoEncodeSrcKHR,
		};

		picture_format = select_video_format(physical_device, video_fmt);

		if (picture_format.format != vk::Format::eG8B8R82Plane420Unorm)
		{
			throw std::runtime_error("Unsupported format " +
			                         vk::to_string(picture_format.format) +
			                         " for encoder input image");
		}

		vk::Extent3D aligned_extent{
		        .width = align(extent.width, encode_caps.encodeInputPictureGranularity.width),
		        .height = align(extent.height, encode_caps.encodeInputPictureGranularity.height),
		        .depth = 1,
		};

		// TODO: check format capabilities
		//
		vk::ImageCreateInfo img_create_info{
		        .pNext = &video_profile_list,
		        .flags = picture_format.imageCreateFlags,
		        .imageType = picture_format.imageType,
		        .format = picture_format.format,
		        .extent = aligned_extent,
		        .mipLevels = 1,
		        .arrayLayers = 1,
		        .samples = vk::SampleCountFlagBits::e1,
		        .tiling = picture_format.imageTiling,
		        .usage = vk::ImageUsageFlagBits::eTransferDst |
		                 picture_format.imageUsageFlags,
		        .sharingMode = vk::SharingMode::eExclusive,
		};

		input_image = device.createImage(img_create_info);

		mem_allocator.request(
		        device.getImageMemoryRequirements(input_image),
		        [this](vk::DeviceMemory memory, size_t offset) {
			        device.bindImageMemory(input_image, memory, offset);
		        },
		        vk::MemoryPropertyFlagBits::eDeviceLocal);
	}

	// Decode picture buffer (DPB) images
	vk::VideoFormatPropertiesKHR reference_picture_format;
	{
		vk::PhysicalDeviceVideoFormatInfoKHR video_fmt{
		        .pNext = &video_profile_list,
		        .imageUsage = vk::ImageUsageFlagBits::eVideoEncodeDpbKHR,
		};

		reference_picture_format = select_video_format(physical_device, video_fmt);

		// TODO: check format capabilities
		// TODO: use multiple images if array levels are not supported

		vk::Extent3D aligned_extent{
		        .width = align(extent.width, video_caps.pictureAccessGranularity.width),
		        .height = align(extent.height, video_caps.pictureAccessGranularity.height),
		        .depth = 1,
		};

		vk::ImageCreateInfo img_create_info{
		        .pNext = &video_profile_list,
		        .flags = reference_picture_format.imageCreateFlags,
		        .imageType = reference_picture_format.imageType,
		        .format = reference_picture_format.format,
		        .extent = aligned_extent,
		        .mipLevels = 1,
		        .arrayLayers = num_dpb_slots,
		        .samples = vk::SampleCountFlagBits::e1,
		        .tiling = reference_picture_format.imageTiling,
		        .usage = reference_picture_format.imageUsageFlags,
		        .sharingMode = vk::SharingMode::eExclusive,
		};

		dpb_image = device.createImage(img_create_info);
		mem_allocator.request(
		        device.getImageMemoryRequirements(dpb_image),
		        [this](vk::DeviceMemory memory, size_t offset) {
			        device.bindImageMemory(dpb_image, memory, offset);
		        },
		        vk::MemoryPropertyFlagBits::eDeviceLocal);
	}

	// video session
	{
		vk::ExtensionProperties std_header_version = this->std_header_version();

		video_session =
		        device.createVideoSessionKHR(vk::VideoSessionCreateInfoKHR{
		                .pNext = video_session_create_next,
		                .queueFamilyIndex = encode_queue_family_index,
		                //.flags = vk::VideoSessionCreateFlagBitsKHR::eAllowEncodeParameterOptimizations,
		                .pVideoProfile = &video_profile,
		                .pictureFormat = picture_format.format,
		                .maxCodedExtent = extent,
		                .referencePictureFormat = reference_picture_format.format,
		                .maxDpbSlots = num_dpb_slots,
		                .maxActiveReferencePictures = 1,
		                .pStdHeaderVersion = &std_header_version,
		        });

		auto video_req = device.getVideoSessionMemoryRequirementsKHR(video_session);
		// FIXME: allocating on a single device memory seems to fail
#if 0
		auto video_session_bind = std::make_shared<std::vector<vk::BindVideoSessionMemoryInfoKHR>>();
		for (const auto & req: video_req)
		{
			mem_allocator.request(
			        req.memoryRequirements,
			        [this, video_session_bind, index = req.memoryBindIndex, size = req.memoryRequirements.size, bind_size = video_req.size()](vk::DeviceMemory mem,
			                                                                                                                                  size_t offset) {
				        video_session_bind->push_back({
				                .memoryBindIndex = index,
				                .memory = mem,
				                .memoryOffset = offset,
				                .memorySize = size,
				        });
				        if (video_session_bind->size() == bind_size)
				        {
					        device.bindVideoSessionMemoryKHR(video_session,
					                                         *video_session_bind);
				        }
			        },
			        vk::MemoryPropertyFlagBits::eDeviceLocal);
		}
#else
		std::vector<vk::BindVideoSessionMemoryInfoKHR> video_session_bind;
		for (const auto & req: video_req)
		{
			vk::MemoryAllocateInfo alloc_info{
			        .allocationSize = req.memoryRequirements.size,
			        .memoryTypeIndex = get_memory_type(
			                physical_device, req.memoryRequirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal)};

			const auto & mem_item = mem.emplace_back(device.allocateMemory(alloc_info));
			video_session_bind.push_back({
			        .memoryBindIndex = req.memoryBindIndex,
			        .memory = mem_item,
			        .memoryOffset = 0,
			        .memorySize = alloc_info.allocationSize,
			});
		}
		device.bindVideoSessionMemoryKHR(video_session, video_session_bind);
#endif

		// Output buffer
		{
			// very conservative bound
			output_buffer_size = extent.width * extent.height * 3;
			output_buffer_size = align(output_buffer_size, video_caps.minBitstreamBufferSizeAlignment);
			output_buffer = device.createBuffer(
			        {.pNext = &video_profile_list,
			         .size = output_buffer_size,
			         .usage = vk::BufferUsageFlagBits::eVideoEncodeDstKHR,
			         .sharingMode = vk::SharingMode::eExclusive});

			mem_allocator.request(
			        device.getBufferMemoryRequirements(output_buffer),
			        [this](vk::DeviceMemory memory, size_t offset) {
				        device.bindBufferMemory(output_buffer, memory, offset);
				        mapped_buffer = device.mapMemory(memory, offset, output_buffer_size);
			        },
			        vk::MemoryPropertyFlagBits::eHostVisible |
			                vk::MemoryPropertyFlagBits::eHostCoherent);
		}
	}

	mem = mem_allocator.alloc_and_bind(physical_device, device);

	// input image view
	{
		vk::ImageViewCreateInfo img_view_create_info{
		        .image = input_image,
		        .viewType = vk::ImageViewType::e2D,
		        .format = picture_format.format,
		        .components = picture_format.componentMapping,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .baseArrayLayer = 0,
		                             .layerCount = 1},
		};
		input_image_view = device.createImageView(img_view_create_info);
	}

	// DPB image views
	{
		vk::ImageViewCreateInfo img_view_create_info{
		        .image = dpb_image,
		        .viewType = vk::ImageViewType::e2D,
		        .format = reference_picture_format.format,
		        .components = reference_picture_format.componentMapping,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .layerCount = 1},
		};
		for (size_t i = 0; i < num_dpb_slots; ++i)
		{
			img_view_create_info.subresourceRange.baseArrayLayer = i;
			dpb_image_views.push_back(device.createImageView(img_view_create_info));
		}
	}

	// DPB video picture resource info
	{
		for (auto & dpb_image_view: dpb_image_views)
		{
			dpb_resource.push_back(
			        {
			                .codedExtent = extent,
			                .imageViewBinding = dpb_image_view,
			        });
		}
	}

	// DPB slot info
	{
		auto std_slots = setup_slot_info(num_dpb_slots);
		assert(std_slots.size() == num_dpb_slots);
		for (size_t i = 0; i < num_dpb_slots; ++i)
		{
			dpb_slots.push_back({
			        .pNext = std_slots[i],
			        .slotIndex = -1,
			        .pPictureResource = nullptr,
			});
		}

		dpb_status = slot_info(num_dpb_slots);
	}

	// video session parameters
	{
		video_session_parameters = device.createVideoSessionParametersKHR({
		        .pNext = session_params_next,
		        .videoSession = video_session,
		});
	}

	// query pool
	{
		vk::StructureChain query_pool_create = {
		        vk::QueryPoolCreateInfo{
		                .queryType = vk::QueryType::eVideoEncodeFeedbackKHR,
		                .queryCount = 1,

		        },
		        vk::QueryPoolVideoEncodeFeedbackCreateInfoKHR{
		                .pNext = &video_profile,
		                .encodeFeedbackFlags =
		                        vk::VideoEncodeFeedbackFlagBitsKHR::estreamBufferOffsetBit |
		                        vk::VideoEncodeFeedbackFlagBitsKHR::estreamBytesWrittenBit,
		        },
		};

		query_pool = device.createQueryPool(query_pool_create.get());
	}

	// command pool and buffer
	{
		command_pool = device.createCommandPool({
		        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		        .queueFamilyIndex = encode_queue_family_index,
		});

		command_buffer = device.allocateCommandBuffers({.commandPool = command_pool,
		                                                .commandBufferCount = 1})[0];
	}
}

video_encoder::~video_encoder()
{
	// TODO: delete stuff
}

std::vector<uint8_t> video_encoder::get_encoded_parameters(void * next)
{
	auto [feedback, encoded] = device.getEncodedVideoSessionParametersKHR({
	        .pNext = next,
	        .videoSessionParameters = video_session_parameters,
	});
	return encoded;
}

std::span<uint8_t> video_encoder::encode_frame(vk::Semaphore wait_semaphore, uint32_t src_queue)
{
	command_buffer.reset();
	command_buffer.begin(vk::CommandBufferBeginInfo{});
	vk::ImageMemoryBarrier2 barrier{
	        .srcStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
	        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
	        .dstStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
	        .dstAccessMask = vk::AccessFlagBits2::eVideoEncodeReadKHR,
	        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	        .newLayout = vk::ImageLayout::eVideoEncodeSrcKHR,
	        .srcQueueFamilyIndex = src_queue,
	        .dstQueueFamilyIndex = encode_queue_family_index,
	        .image = input_image,
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
	command_buffer.resetQueryPool(query_pool, 0, 1);

	// slot: where the encoded picture will be stored in DPB
	size_t slot = dpb_status.get_slot();
	// ref_slot: which image to use as reference
	auto ref_slot = dpb_status.get_ref();
	assert(not(ref_slot and (*ref_slot == slot)));
	dpb_status[slot] = frame_num;

	dpb_slots[slot].slotIndex = -1;
	dpb_slots[slot].pPictureResource = &dpb_resource[slot];

	{
		vk::VideoBeginCodingInfoKHR video_coding_begin_info{
		        .videoSession = video_session,
		        .videoSessionParameters = video_session_parameters,
		};
		video_coding_begin_info.setReferenceSlots(dpb_slots);
		command_buffer.beginVideoCodingKHR(video_coding_begin_info);
	}

	if (frame_num == 0)
	{
		command_buffer.controlVideoCodingKHR({.flags = vk::VideoCodingControlFlagBitsKHR::eReset});
		vk::ImageMemoryBarrier2 dpb_barrier{
		        .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
		        .srcAccessMask = vk::AccessFlagBits2::eNone,
		        .dstStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
		        .dstAccessMask = vk::AccessFlagBits2::eVideoEncodeReadKHR | vk::AccessFlagBits2::eVideoEncodeWriteKHR,
		        .oldLayout = vk::ImageLayout::eUndefined,
		        .newLayout = vk::ImageLayout::eVideoEncodeDpbKHR,
		        .image = dpb_image,
		        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
		                             .baseMipLevel = 0,
		                             .levelCount = 1,
		                             .baseArrayLayer = 0,
		                             .layerCount = 1},
		};
		command_buffer.pipelineBarrier2({
		        .imageMemoryBarrierCount = 1,
		        .pImageMemoryBarriers = &dpb_barrier,
		});
	}

	dpb_slots[slot].slotIndex = slot;
	vk::VideoEncodeInfoKHR encode_info{
	        .pNext = encode_info_next(frame_num, slot, ref_slot),
	        .dstBuffer = output_buffer,
	        .dstBufferOffset = 0,
	        .dstBufferRange = output_buffer_size,
	        .srcPictureResource = {.codedExtent = extent,
	                               .baseArrayLayer = 0,
	                               .imageViewBinding = input_image_view},
	        .pSetupReferenceSlot = &dpb_slots[slot],
	};
	if (ref_slot)
		encode_info.setReferenceSlots(dpb_slots[*ref_slot]);

	command_buffer.beginQuery(query_pool, 0, {});
	command_buffer.encodeVideoKHR(encode_info);
	command_buffer.endQuery(query_pool, 0);
	command_buffer.endVideoCodingKHR(vk::VideoEndCodingInfoKHR{});
	command_buffer.end();

	vk::SubmitInfo2 submit{};
	vk::CommandBufferSubmitInfo cmd_info{
	        .commandBuffer = command_buffer,
	};
	submit.setCommandBufferInfos(cmd_info);
	vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = wait_semaphore,
	        .stageMask = vk::PipelineStageFlagBits2::eVideoEncodeKHR,
	};
	submit.setWaitSemaphoreInfos(sem_info);
	encode_queue.submit2(submit, fence);

	if (auto res = device.waitForFences(fence, true, 1'000'000'000);
	    res != vk::Result::eSuccess)
	{
		throw std::runtime_error("wait for fences: " + vk::to_string(res));
	}

	auto [res, feedback] = device.getQueryPoolResults<uint32_t>(query_pool,
	                                                            0,
	                                                            1,
	                                                            3 * sizeof(uint32_t),
	                                                            0,
	                                                            vk::QueryResultFlagBits::eWait);
	if (res != vk::Result::eSuccess)
	{
		std::cerr << "device.getQueryPoolResults: " << vk::to_string(res) << std::endl;
	}

	device.resetFences(fence);

	++frame_num;

	return {((uint8_t *)mapped_buffer) + feedback[0], feedback[1]};
}
