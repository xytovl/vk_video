#include "slot_info.h"

#include <algorithm>

#ifdef DPB_CHAOS_MODE
#include <random>

static std::mt19937 rnd;
#endif

size_t slot_info::get_slot()
{
	auto i = std::ranges::min_element(frames);
	return i - std::begin(frames);
}

std::optional<size_t> slot_info::get_ref()
{
#ifdef DPB_CHAOS_MODE
	size_t i = rnd() % frames.size();
	if (frames[i] >= 0 and i != get_slot())
		return i;
#else
	auto i = std::ranges::max_element(frames);
	if (*i >= 0)
		return i - std::begin(frames);
#endif
	return {};
}
