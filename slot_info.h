#pragma once

#include <cstdint>
#include <optional>
#include <vector>

class slot_info
{
	std::vector<int32_t> frames;

public:
	slot_info(size_t size) :
	        frames(size, -1) {}

    std::pair<size_t, std::optional<size_t>> add_frame(int32_t frame);
	size_t get_slot();

	int32_t & operator[](size_t slot)
	{
		return frames[slot];
	}

	std::optional<size_t> get_ref();
};
