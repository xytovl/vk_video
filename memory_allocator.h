#pragma once

#include <cstdint>
#include <functional>
#include <vulkan/vulkan.hpp>

inline uint32_t align(uint32_t value, uint32_t alignment)
{
	if (alignment == 0)
		return value;
	return alignment * ((value + alignment - 1) / alignment);
}

uint32_t get_memory_type(vk::PhysicalDevice phys_dev, uint32_t type_bits, vk::MemoryPropertyFlags memory_props);

class mini_vma
{
	struct alloc_request
	{
		vk::MemoryRequirements requirement;
		std::function<void(vk::DeviceMemory, size_t)> bind_cb;
		vk::MemoryPropertyFlags props;
		uint32_t mem_index = 0;
		size_t offset = 0;
	};

	std::vector<alloc_request> requests;

public:
	void request(vk::MemoryRequirements requirement,
	             decltype(alloc_request::bind_cb) cb,
	             vk::MemoryPropertyFlags props);

	std::vector<vk::DeviceMemory> alloc_and_bind(
	        vk::PhysicalDevice physical_device,
	        vk::Device device);
};
