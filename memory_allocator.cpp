#include "memory_allocator.h"

#include <map>
#include <stdexcept>

uint32_t get_memory_type(vk::PhysicalDevice phys_dev, uint32_t type_bits, vk::MemoryPropertyFlags memory_props)
{
	auto mem_prop = phys_dev.getMemoryProperties();

	for (uint32_t i = 0; i < mem_prop.memoryTypeCount; ++i)
	{
		if ((type_bits >> i) & 1)
		{
			if ((mem_prop.memoryTypes[i].propertyFlags & memory_props) ==
			    memory_props)
				return i;
		}
	}
	throw std::runtime_error("Failed to get memory type");
}

void mini_vma::request(vk::MemoryRequirements requirement,
                       decltype(alloc_request::bind_cb) cb,
                       vk::MemoryPropertyFlags props)
{
	requests.emplace_back(requirement, cb, props);
};

std::vector<vk::DeviceMemory> mini_vma::alloc_and_bind(
        vk::PhysicalDevice physical_device,
        vk::Device device)
{
	struct mem_status
	{
		vk::DeviceMemory mem;
		size_t alloc_size = 0;
	};
	std::map<uint32_t, mem_status> statuses;

	for (auto & request: requests)
	{
		request.mem_index = get_memory_type(
		        physical_device, request.requirement.memoryTypeBits, request.props);
		auto & status = statuses[request.mem_index];
		request.offset = align(status.alloc_size, request.requirement.alignment);
		status.alloc_size = request.offset + request.requirement.size;
	}

	std::vector<vk::DeviceMemory> memories;
	try
	{
		for (auto & [index, status]: statuses)
		{
			memories.push_back(device.allocateMemory({
			        .allocationSize = status.alloc_size,
			        .memoryTypeIndex = index,
			}));
			status.mem = memories.back();
		}
		for (auto & request: requests)
		{
			request.bind_cb(statuses[request.mem_index].mem,
			                request.offset);
		}
		requests.clear();
		return memories;
	}
	catch (...)
	{
		for (auto & mem: memories)
		{
			device.freeMemory(mem);
		}
		throw;
	}
}
