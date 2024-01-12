#pragma once

#include "video_encoder.h"

#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

class video_encoder_h264 : public video_encoder
{
	uint16_t idr_id = 0;
	StdVideoH264SequenceParameterSet sps;
	StdVideoH264PictureParameterSet pps;

	StdVideoEncodeH264SliceHeader slice_header;
	vk::VideoEncodeH264NaluSliceInfoKHR nalu_slice_info;

	StdVideoEncodeH264PictureInfo std_picture_info;
	vk::VideoEncodeH264PictureInfoKHR picture_info;

	StdVideoEncodeH264ReferenceListsInfo reference_lists_info;

	std::vector<StdVideoEncodeH264ReferenceInfo> dpb_std_info;
	std::vector<vk::VideoEncodeH264DpbSlotInfoKHR> dpb_std_slots;

	video_encoder_h264(vk::Device device, vk::Queue encode_queue, uint32_t encode_queue_family_index, vk::Extent2D extent);

protected:
	std::vector<void *> setup_slot_info(size_t dpb_size) override;

	void * encode_info_next(uint32_t frame_num, size_t slot, std::optional<size_t> ref) override;
	virtual vk::ExtensionProperties std_header_version() override;

public:
	static std::unique_ptr<video_encoder_h264> create(vk::PhysicalDevice physical_device,
	                                 vk::Device device,
	                                 vk::Queue encode_queue,
	                                 uint32_t encode_queue_family_index,
	                                 const vk::Extent2D & extent);

	std::vector<uint8_t> get_sps_pps();
};
