#pragma once

#include <vector>
#include <vulkan/vulkan.hpp>

class test_pattern
{
	vk::Device device;
	vk::Extent2D extent;

public:
	vk::Image img_y;
	vk::Image img_uv;
private:
	std::vector<vk::DeviceMemory> mem;

	vk::ImageView view_y;
	vk::ImageView view_uv;

	vk::DescriptorSetLayout ds_layout = nullptr;
	vk::PipelineLayout layout = nullptr;

	vk::Pipeline pipeline;
	vk::DescriptorPool dp;
	vk::DescriptorSet ds;

	using push_constant = uint32_t;
	uint32_t counter = 0;

public:
	test_pattern(vk::PhysicalDevice phys_dev, vk::Device dev, vk::Extent2D extent);
	void record_draw_commands(vk::CommandBuffer cmd_buf);
};
