#pragma once

#include <span>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "slot_info.h"

class video_encoder
{
	vk::Device device;
	vk::Queue encode_queue;
	uint32_t encode_queue_family_index;

	vk::Fence fence;

	vk::VideoSessionKHR video_session;
	vk::VideoSessionParametersKHR video_session_parameters;

	vk::QueryPool query_pool;
	vk::CommandPool command_pool;

	vk::CommandBuffer command_buffer;

	vk::Buffer output_buffer;
	size_t output_buffer_size;
	void * mapped_buffer = nullptr;

public:
	vk::Image input_image;

private:
	vk::ImageView input_image_view;

	slot_info dpb_status = slot_info(0);

	vk::Image dpb_image;
	std::vector<vk::ImageView> dpb_image_views;
	std::vector<vk::VideoPictureResourceInfoKHR> dpb_resource;
	std::vector<vk::VideoReferenceSlotInfoKHR> dpb_slots;

	std::vector<vk::DeviceMemory> mem;

	vk::VideoFormatPropertiesKHR select_video_format(
	        vk::PhysicalDevice physical_device,
	        const vk::PhysicalDeviceVideoFormatInfoKHR &);

	uint32_t frame_num = 0;
	const vk::Extent2D extent;

protected:
	video_encoder(vk::Device device, vk::Queue encode_queue, uint32_t encode_queue_family_index, vk::Extent2D extent) :
	        device(device), encode_queue(encode_queue), encode_queue_family_index(encode_queue_family_index), extent(extent) {}

	void init(vk::PhysicalDevice physical_device,
	          const vk::VideoCapabilitiesKHR & video_caps,
	          const vk::VideoEncodeCapabilitiesKHR & encode_caps,
	          const vk::VideoProfileInfoKHR & video_profile,
		  void *video_session_create_next,
	          void * session_params_next);

	virtual ~video_encoder();

	std::vector<uint8_t> get_encoded_parameters(void * next);

	virtual std::vector<void *> setup_slot_info(size_t dpb_size) = 0;
	virtual void * encode_info_next(uint32_t frame_num, size_t slot, std::optional<size_t> ref) = 0;
	virtual vk::ExtensionProperties std_header_version() = 0;

public:
	std::span<uint8_t> encode_frame(vk::Semaphore wait_semaphore, uint32_t src_queue);
};

