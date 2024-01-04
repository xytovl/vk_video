#include <fstream>
#include <iostream>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

struct queue {
    vk::raii::Queue queue;
    uint32_t familyIndex;
};

auto make_device(vk::raii::Instance& instance) {
    vk::raii::PhysicalDevices physicalDevices(instance);
    for (const auto& d : physicalDevices) {
        auto props = d.enumerateDeviceExtensionProperties();
        auto [feat, feat_13] =
            d.getFeatures2<vk::PhysicalDeviceFeatures2,
                           vk::PhysicalDeviceVulkan13Features>();
        if (not feat_13.synchronization2) continue;
        vk::DeviceCreateInfo create_info{.pNext = &feat};
        std::vector<const char*> required_extensions = {
            VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
            VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
            VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
            VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
            // VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
        };
        for (const auto& ext : required_extensions) {
            if (std::ranges::find_if(props, [ext](auto el) {
                    return ext == std::string(el.extensionName);
                }) == props.end()) {
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
        for (const auto& q : queues) {
            if (q.queueFlags & vk::QueueFlagBits::eVideoEncodeKHR) {
                encode_queue.familyIndex = i;
                queue_info.push_back({
                    .queueFamilyIndex = i,
                    .queueCount = 1,
                });
                queue_info.back().setQueuePriorities(prio);
            }
            if (q.queueFlags & vk::QueueFlagBits::eGraphics) {
                gfx_queue.familyIndex = i;
                queue_info.push_back({
                    .queueFamilyIndex = i,
                    .queueCount = 1,
                });
                queue_info.back().setQueuePriorities(prio);
            }
            if (queue_info.size() == 2) break;
            ++i;
        }
        if (queue_info.size() != 2) {
            throw std::runtime_error("No suitable queue for video encode");
        }
        create_info.setQueueCreateInfos(queue_info);

        auto dev = d.createDevice(create_info);
        encode_queue.queue = vk::raii::Queue(dev, encode_queue.familyIndex, 0);
        gfx_queue.queue = vk::raii::Queue(dev, gfx_queue.familyIndex, 0);
        return std::make_tuple(std::move(d), std::move(dev),
                               std::move(encode_queue), std::move(gfx_queue));
    }
    throw std::runtime_error("No vulkan device available");
}

uint32_t get_memory_type(vk::raii::PhysicalDevice& phys_dev, uint32_t type_bits,
                         vk::MemoryPropertyFlags memory_props) {
    auto mem_prop = phys_dev.getMemoryProperties();

    for (uint32_t i = 0; i < mem_prop.memoryTypeCount; ++i) {
        if ((type_bits >> i) & 1) {
            if ((mem_prop.memoryTypes[i].propertyFlags & memory_props) ==
                memory_props)
                return i;
        }
    }
    throw std::runtime_error("Failed to get memory type");
}

auto create_src_image(const vk::VideoProfileListInfoKHR& prof,
                      const vk::Extent2D& extent,
                      vk::raii::PhysicalDevice& phys_dev,
                      vk::raii::Device& dev) {
    vk::PhysicalDeviceVideoFormatInfoKHR video_fmt{
        .pNext = &prof,
        .imageUsage = vk::ImageUsageFlagBits::eVideoEncodeSrcKHR,
    };

    vk::ImageCreateInfo img_create_info{
        .pNext = &prof,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    vk::ImageViewCreateInfo img_view_create_info{
        .viewType = vk::ImageViewType::e2D,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
    };
    auto video_fmt_props = phys_dev.getVideoFormatPropertiesKHR(video_fmt);
    for (const auto& video_fmt_prop : video_fmt_props) {
        std::cerr << "format: " << vk::to_string(video_fmt_prop.format)
                  << std::endl;
        std::cerr << "tiling: " << vk::to_string(video_fmt_prop.imageTiling)
                  << std::endl;
        std::cerr << "usage: " << vk::to_string(video_fmt_prop.imageUsageFlags)
                  << std::endl;
        std::cerr << "components: "
                  << vk::to_string(video_fmt_prop.componentMapping.r)
                  << std::endl
                  << "            "
                  << vk::to_string(video_fmt_prop.componentMapping.g)
                  << std::endl
                  << "            "
                  << vk::to_string(video_fmt_prop.componentMapping.b)
                  << std::endl
                  << "            "
                  << vk::to_string(video_fmt_prop.componentMapping.a)
                  << std::endl;
    }
    for (const auto& video_fmt_prop : video_fmt_props) {
        // TODO: select a good format if multiple ones are available
        img_view_create_info.format = img_create_info.format =
            video_fmt_prop.format;
        img_create_info.flags |= video_fmt_prop.imageCreateFlags;
        img_create_info.imageType = video_fmt_prop.imageType;
        img_create_info.tiling = video_fmt_prop.imageTiling;
        img_create_info.usage |= video_fmt_prop.imageUsageFlags;
        img_view_create_info.components = video_fmt_prop.componentMapping;
        break;
    }

    vk::raii::Image img_in(dev, img_create_info);

    auto mem_req = img_in.getMemoryRequirements();

    vk::MemoryAllocateInfo alloc_info{
        .allocationSize = mem_req.size,
        .memoryTypeIndex =
            get_memory_type(phys_dev, mem_req.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible &
                                vk::MemoryPropertyFlagBits::eHostCoherent),
    };

    vk::raii::DeviceMemory img_mem(dev, alloc_info);
    img_in.bindMemory(*img_mem, 0);

    img_view_create_info.image = *img_in;
    vk::raii::ImageView img_view(dev, img_view_create_info);

    return std::make_tuple(std::move(img_in), std::move(img_mem),
                           std::move(img_view), img_create_info.format);
}

auto create_dpb_images(const vk::VideoProfileListInfoKHR& prof,
                       const vk::Extent2D& extent,
                       vk::raii::PhysicalDevice& phys_dev,
                       vk::raii::Device& dev, int slots) {
    vk::PhysicalDeviceVideoFormatInfoKHR video_fmt{
        .pNext = &prof,
        .imageUsage = vk::ImageUsageFlagBits::eVideoEncodeDpbKHR,
    };

    vk::ImageCreateInfo img_create_info{
        .pNext = &prof,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    auto video_fmt_props = phys_dev.getVideoFormatPropertiesKHR(video_fmt);
    for (const auto& video_fmt_prop : video_fmt_props) {
        // TODO: select a good format if multiple ones are available
        img_create_info.format = video_fmt_prop.format;
        img_create_info.flags |= video_fmt_prop.imageCreateFlags;
        img_create_info.imageType = video_fmt_prop.imageType;
        img_create_info.tiling = video_fmt_prop.imageTiling;
        img_create_info.usage |= video_fmt_prop.imageUsageFlags;
        break;
    }

    std::vector<std::pair<vk::raii::Image, vk::raii::ImageView>> images;

    size_t alloc_size = 0;
    std::vector<size_t> offsets;

    std::optional<vk::MemoryRequirements> mem_req;

    for (int i = 0; i < slots; ++i) {
        vk::raii::Image img(dev, img_create_info);

        auto req = img.getMemoryRequirements();
        if (mem_req) {
            assert(req.memoryTypeBits == mem_req->memoryTypeBits);
        } else {
            mem_req = req;
        }
        size_t offset = ((alloc_size - 1) / req.alignment + 1) * req.alignment;
        offsets.push_back(offset);
        alloc_size = offset + req.size;

        images.emplace_back(std::move(img), nullptr);
    }

    vk::MemoryAllocateInfo alloc_info{
        .allocationSize = alloc_size,
        .memoryTypeIndex =
            get_memory_type(phys_dev, mem_req->memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eDeviceLocal),
    };

    vk::raii::DeviceMemory img_mem(dev, alloc_info);

    for (int i = 0; i < slots; ++i) {
        images[i].first.bindMemory(*img_mem, offsets[i]);

        images[i].second = vk::raii::ImageView(
            dev, {
                     .image = *images[i].first,
                     .viewType = vk::ImageViewType::e2D,
                     .format = img_create_info.format,
                     .subresourceRange = {.aspectMask =
                                              vk::ImageAspectFlagBits::eColor,
                                          .baseMipLevel = 0,
                                          .levelCount = 1,
                                          .baseArrayLayer = 0,
                                          .layerCount = 1},
                 });
    }

    return std::make_tuple(std::move(images), std::move(img_mem),
                           img_create_info.format);
}

auto create_buffer(const vk::VideoProfileListInfoKHR& prof,
                   vk::raii::Device& dev, vk::raii::PhysicalDevice& phys_dev,
                   size_t size) {
    vk::raii::Buffer buffer(
        dev, vk::BufferCreateInfo{
                 .pNext = &prof,
                 .size = size,
                 .usage = vk::BufferUsageFlagBits::eVideoEncodeDstKHR,
                 .sharingMode = vk::SharingMode::eExclusive});

    auto mem_req = buffer.getMemoryRequirements();

    vk::MemoryAllocateInfo alloc_info{
        .allocationSize = mem_req.size,
        .memoryTypeIndex =
            get_memory_type(phys_dev, mem_req.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible |
                                vk::MemoryPropertyFlagBits::eHostCoherent),
    };

    vk::raii::DeviceMemory mem(dev, alloc_info);
    buffer.bindMemory(*mem, 0);
    return std::make_tuple(std::move(buffer), std::move(mem));
}

class slot_info {
    std::vector<int32_t> frames;

   public:
    slot_info(size_t size) : frames(size, -1) {}

    size_t get_slot() {
        auto i = std::ranges::min_element(frames);
        return i - std::begin(frames);
    }

    int32_t& operator[](size_t slot) { return frames[slot]; }

    std::optional<size_t> get_ref() {
        auto i = std::ranges::max_element(frames);
        if (*i >= 0) return i - std::begin(frames);
        return {};
    }
};

class test_pattern {
    vk::raii::Image& target_img;
    vk::Extent2D extent;
    vk::raii::Buffer buf = nullptr;
    vk::raii::DeviceMemory mem = nullptr;
    size_t counter = 0;

    inline static const std::array y = {940, 877, 754, 691, 313, 250, 127, 64};
    inline static const std::array cb = {512, 64, 615, 167, 857, 409, 960, 512};
    inline static const std::array cr = {512, 553, 64, 105, 919, 960, 471, 512};

   public:
    test_pattern(vk::raii::PhysicalDevice& phys_dev, vk::raii::Device& dev,
                 vk::raii::Image& target_img, vk::Format fmt,
                 vk::Extent2D extent)
        : target_img(target_img), extent(extent) {
        assert(fmt == vk::Format::eG8B8R82Plane420Unorm);

        buf = vk::raii::Buffer(
            dev, {
                     .size = extent.width * extent.height,
                     .usage = vk::BufferUsageFlagBits::eTransferSrc |
                              vk::BufferUsageFlagBits::eTransferDst,
                     .sharingMode = vk::SharingMode::eExclusive,
                 });

        auto mem_req = buf.getMemoryRequirements();

        vk::MemoryAllocateInfo alloc_info{
            .allocationSize = mem_req.size,
            .memoryTypeIndex =
                get_memory_type(phys_dev, mem_req.memoryTypeBits,
                                vk::MemoryPropertyFlagBits::eDeviceLocal),
        };

        mem = vk::raii::DeviceMemory(dev, alloc_info);
        buf.bindMemory(*mem, 0);
    }
    void draw(vk::raii::CommandBuffer& cmd_buf, uint32_t src_queue,
              uint32_t dst_queue) {
        static const int freq = 200;
        size_t shift = (counter / freq) % y.size();
        size_t offset = counter % freq;
        ++counter;

        uint32_t bar_y_w = extent.width / y.size();

        vk::ImageMemoryBarrier2 im_barrier{
            .srcStageMask = vk::PipelineStageFlagBits2KHR::eNone,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = *target_img,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
        };

        vk::BufferMemoryBarrier2 buf_barrier{
            .srcStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite |
                             vk::AccessFlagBits2::eTransferRead,
            .dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite |
                             vk::AccessFlagBits2::eTransferRead,
            .buffer = *buf,
            .size = vk::WholeSize,
        };
        vk::DependencyInfo dep_info{};
        dep_info.setImageMemoryBarriers(im_barrier);
        cmd_buf.pipelineBarrier2(dep_info);

        for (size_t i = 0; i < y.size(); ++i) {
            size_t j = (i + shift) % y.size();

            im_barrier.srcStageMask = vk::PipelineStageFlagBits2KHR::eTransfer;
            im_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
            im_barrier.dstStageMask = vk::PipelineStageFlagBits2KHR::eTransfer;
            im_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
            im_barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            im_barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            dep_info.setBufferMemoryBarriers(buf_barrier);

            uint32_t data;
            std::array<uint8_t, 4> val;
            val.fill((y[j] * 256) / 1024);
            std::memcpy(&data, val.data(), sizeof(data));

            cmd_buf.fillBuffer(*buf, 0, vk::WholeSize, data);
            cmd_buf.pipelineBarrier2(dep_info);

            vk::BufferImageCopy region{
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::ePlane0,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {.x = int32_t(offset + (i - 1) * bar_y_w),
                                .y = 0,
                                .z = 0},
                .imageExtent = {.width = bar_y_w,
                                .height = extent.height,
                                .depth = 1},
            };
            if (i == 0) {
                region.imageOffset.x = 0;
                cmd_buf.copyBufferToImage(*buf, *target_img,
                                          vk::ImageLayout::eTransferDstOptimal,
                                          region);
                region.imageOffset.x = extent.width - bar_y_w;
                cmd_buf.pipelineBarrier2(dep_info);
            }
            cmd_buf.copyBufferToImage(*buf, *target_img,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      region);
            cmd_buf.pipelineBarrier2(dep_info);

            val[2] = val[0] = (cb[j] * 256) / 1024;
            val[3] = val[1] = (cr[j] * 256) / 1024;
            std::memcpy(&data, val.data(), sizeof(data));
            cmd_buf.fillBuffer(*buf, 0, vk::WholeSize, data);
            cmd_buf.pipelineBarrier2(dep_info);

            region.imageSubresource.aspectMask =
                vk::ImageAspectFlagBits::ePlane1;
            region.imageOffset.x /= 2;
            region.imageExtent.width /= 2;
            region.imageExtent.height /= 2;
            if (i == 0) {
                cmd_buf.copyBufferToImage(*buf, *target_img,
                                          vk::ImageLayout::eTransferDstOptimal,
                                          region);
                region.imageOffset.x = 0;
                cmd_buf.pipelineBarrier2(dep_info);
            }
            cmd_buf.copyBufferToImage(*buf, *target_img,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      region);
            cmd_buf.pipelineBarrier2(dep_info);
        }
        im_barrier.srcStageMask = vk::PipelineStageFlagBits2KHR::eTransfer;
        im_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        im_barrier.dstStageMask = vk::PipelineStageFlagBits2KHR::eTopOfPipe;
        im_barrier.dstAccessMask = vk::AccessFlagBits2::eNone;
        im_barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        im_barrier.newLayout = vk::ImageLayout::eVideoEncodeSrcKHR;
        im_barrier.setSrcQueueFamilyIndex(src_queue);
        im_barrier.setDstQueueFamilyIndex(dst_queue);
        cmd_buf.pipelineBarrier2(dep_info);
    }
};

int main(int /*argc*/, char** /*argv*/) {
    try {
        vk::raii::Context ctx;
        std::ofstream out("out.h264", std::ios::trunc);

        vk::Extent2D extent{1920, 1080};

        vk::ApplicationInfo app_info{
            .pApplicationName = "vk_video test",
            .applicationVersion = 1,
            .pEngineName = "test engine",
            .engineVersion = 1,
            .apiVersion = VK_API_VERSION_1_3,
        };
        vk::raii::Instance instance(ctx, {
                                             .pApplicationInfo = &app_info,
                                         });

        auto [phys_dev, dev, encode_queue, gfx_queue] = make_device(instance);

        vk::VideoProfileListInfoKHR prof;
        vk::StructureChain prof_info{
            vk::VideoProfileInfoKHR{
                .videoCodecOperation =
                    vk::VideoCodecOperationFlagBitsKHR::eEncodeH264,
                .chromaSubsampling =
                    vk::VideoChromaSubsamplingFlagBitsKHR::e420,
                .lumaBitDepth = vk::VideoComponentBitDepthFlagBitsKHR::e8,
                .chromaBitDepth = vk::VideoComponentBitDepthFlagBitsKHR::e8,
            },
            vk::VideoEncodeH264ProfileInfoKHR{
                .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN,
            }};
        prof.setProfiles(prof_info.get());

        auto [video_caps, encode_caps, encode_h264_caps] =
            phys_dev.getVideoCapabilitiesKHR<
                vk::VideoCapabilitiesKHR, vk::VideoEncodeCapabilitiesKHR,
                vk::VideoEncodeH264CapabilitiesKHR>(prof_info.get());
        std::cerr << "maxActiveReferencePictures "
                  << video_caps.maxActiveReferencePictures << std::endl;
        std::cerr << "maxDpbSlots " << video_caps.maxDpbSlots << std::endl;
        std::cerr << "minCodedExtent " << video_caps.minCodedExtent.width << "x"
                  << video_caps.minCodedExtent.height << std::endl;
        std::cerr << "maxCodedExtent " << video_caps.maxCodedExtent.width << "x"
                  << video_caps.maxCodedExtent.height << std::endl;
        std::cerr << "std header " << video_caps.stdHeaderVersion.extensionName
                  << std::endl;
        std::cerr << "std header " << video_caps.stdHeaderVersion.specVersion
                  << std::endl;

        auto [img_in, img_in_mem, img_in_view, img_fmt] =
            create_src_image(prof, extent, phys_dev, dev);

        test_pattern pattern(phys_dev, dev, img_in, img_fmt, extent);

        auto [dpb, dpb_mem, dpb_fmt] =
            create_dpb_images(prof, extent, phys_dev, dev, 2);

        vk::raii::VideoSessionKHR video_session(
            dev,
            vk::VideoSessionCreateInfoKHR{
                .queueFamilyIndex = encode_queue.familyIndex,
                // .flags =
                // vk::VideoSessionParametersCreateFlagBitsKHR::eAllowEncodeParameterOptimization,
                .pVideoProfile = &prof_info.get(),
                .pictureFormat = img_fmt,
                .maxCodedExtent = extent,
                .referencePictureFormat = dpb_fmt,
                .maxDpbSlots = 2,
                .maxActiveReferencePictures = 1,
                .pStdHeaderVersion = &video_caps.stdHeaderVersion,
            });

        auto video_session_mem_req = video_session.getMemoryRequirements();

        std::vector<vk::raii::DeviceMemory> video_session_mem;
        std::vector<vk::BindVideoSessionMemoryInfoKHR> video_session_bind;
        for (const auto& req : video_session_mem_req) {
            vk::MemoryAllocateInfo alloc_info{
                .allocationSize = req.memoryRequirements.size,
                .memoryTypeIndex = get_memory_type(
                    phys_dev, req.memoryRequirements.memoryTypeBits,
                    vk::MemoryPropertyFlagBits::eDeviceLocal)};

            const auto& mem = video_session_mem.emplace_back(dev, alloc_info);
            video_session_bind.push_back({
                .memoryBindIndex = req.memoryBindIndex,
                .memory = *mem,
                .memoryOffset = 0,
                .memorySize = alloc_info.allocationSize,
            });
        }
        video_session.bindMemory(video_session_bind);

        size_t buffer_size = extent.width * extent.height * 3;
        buffer_size =
            ((buffer_size - 1) / video_caps.minBitstreamBufferSizeAlignment +
             1) *
            video_caps.minBitstreamBufferSizeAlignment;
        auto [out_buffer, out_buffer_mem] =
            create_buffer(prof, dev, phys_dev, buffer_size);
        void* mapped_buffer = out_buffer_mem.mapMemory(0, buffer_size);
        assert(mapped_buffer);

        StdVideoH264SequenceParameterSet sps{
            .flags =
                {
                    .constraint_set0_flag = 0,
                    .constraint_set1_flag = 0,
                    .constraint_set2_flag = 0,
                    .constraint_set3_flag = 0,
                    .constraint_set4_flag = 0,
                    .constraint_set5_flag = 0,
                    .direct_8x8_inference_flag = 0,
                    .mb_adaptive_frame_field_flag = 0,
                    .frame_mbs_only_flag = 1,
                    .delta_pic_order_always_zero_flag = 1,
                    .separate_colour_plane_flag = 0,
                    .gaps_in_frame_num_value_allowed_flag = 1,
                    .qpprime_y_zero_transform_bypass_flag = 0,
                    .frame_cropping_flag = 0,
                    .seq_scaling_matrix_present_flag = 0,
                    .vui_parameters_present_flag = 0,
                },
            .profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN,
            .level_idc = STD_VIDEO_H264_LEVEL_IDC_5_0,
            .chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420,
            .seq_parameter_set_id = 0,
            .bit_depth_luma_minus8 = 0,
            .bit_depth_chroma_minus8 = 0,
            .log2_max_frame_num_minus4 = 12,
            .pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2,
            .offset_for_non_ref_pic = 0,
            .offset_for_top_to_bottom_field = 0,
            .log2_max_pic_order_cnt_lsb_minus4 = 0,
            .num_ref_frames_in_pic_order_cnt_cycle = 0,
            .max_num_ref_frames = 1,
            .reserved1 = 0,
            .pic_width_in_mbs_minus1 = (extent.width - 1) / 16,
            .pic_height_in_map_units_minus1 = (extent.height - 1) / 16,
            .frame_crop_left_offset = 0,
            .frame_crop_right_offset = 0,
            .frame_crop_top_offset = 0,
            .frame_crop_bottom_offset = 0,
            .reserved2 = 0,
            .pOffsetForRefFrame = nullptr,
            .pScalingLists = nullptr,
            .pSequenceParameterSetVui = nullptr,
        };
        StdVideoH264PictureParameterSet pps{
            .flags =
                {
                    .transform_8x8_mode_flag = 0,
                    .redundant_pic_cnt_present_flag = 1,
                    .constrained_intra_pred_flag = 0,
                    .deblocking_filter_control_present_flag = 0,
                    .weighted_pred_flag = 0,
                    .bottom_field_pic_order_in_frame_present_flag = 0,
                    .entropy_coding_mode_flag = 1,
                    .pic_scaling_matrix_present_flag = 0,
                },
            .seq_parameter_set_id = 0,
            .pic_parameter_set_id = 0,
            .num_ref_idx_l0_default_active_minus1 = 0,
            .num_ref_idx_l1_default_active_minus1 = 0,
            .weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT,
            .pic_init_qp_minus26 = 0,
            .pic_init_qs_minus26 = 0,
            .chroma_qp_index_offset = 0,
            .second_chroma_qp_index_offset = 0,
            .pScalingLists = nullptr,
        };

        vk::VideoEncodeH264SessionParametersAddInfoKHR h264_add_info{};
        h264_add_info.setStdSPSs(sps);
        h264_add_info.setStdPPSs(pps);

        vk::VideoEncodeH264SessionParametersCreateInfoKHR h264_session_params{
            .maxStdSPSCount = 1,
            .maxStdPPSCount = 1,
            .pParametersAddInfo = &h264_add_info,
        };
        vk::raii::VideoSessionParametersKHR video_session_params(
            dev,
            {.pNext = &h264_session_params, .videoSession = *video_session});

        vk::StructureChain get_info = {
            vk::VideoEncodeSessionParametersGetInfoKHR{
                .videoSessionParameters = *video_session_params,
            },
            vk::VideoEncodeH264SessionParametersGetInfoKHR{
                .writeStdSPS = true,
                .writeStdPPS = true,
            },
        };

        auto [feedback, sps_pps] =
            dev.getEncodedVideoSessionParametersKHR(get_info.get());
        out.write((const char*)sps_pps.data(), sps_pps.size());
        out.flush();

        vk::raii::CommandPool gfx_cmd_pool(
            dev,
            {
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                .queueFamilyIndex = gfx_queue.familyIndex,
            });

        auto gfx_cmd_buffer = std::move(dev.allocateCommandBuffers(
            {.commandPool = *gfx_cmd_pool, .commandBufferCount = 1})[0]);

        vk::raii::CommandPool cmd_pool(
            dev,
            {
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                .queueFamilyIndex = encode_queue.familyIndex,
            });

        auto cmd_buffer = std::move(dev.allocateCommandBuffers(
            {.commandPool = *cmd_pool, .commandBufferCount = 1})[0]);

        vk::StructureChain query_pool_create = {
            vk::QueryPoolCreateInfo{
                .queryType = vk::QueryType::eVideoEncodeFeedbackKHR,
                .queryCount = 1,

            },
            vk::QueryPoolVideoEncodeFeedbackCreateInfoKHR{
                .pNext = &prof_info.get(),
                .encodeFeedbackFlags =
                    vk::VideoEncodeFeedbackFlagBitsKHR::estreamBytesWrittenBit,
            },
        };
        vk::raii::QueryPool query_pool(dev, query_pool_create.get());

        std::vector<vk::VideoPictureResourceInfoKHR> ref_pics(dpb.size());
        std::vector<StdVideoEncodeH264ReferenceInfo> h264_ref(dpb.size());
        std::vector<vk::VideoEncodeH264DpbSlotInfoKHR> ref_h264_slots(
            dpb.size());
        std::vector<vk::VideoReferenceSlotInfoKHR> ref_slots(dpb.size());
        for (size_t i = 0; i < dpb.size(); ++i) {
            ref_pics[i] = vk::VideoPictureResourceInfoKHR{
                .codedExtent = extent,
                .baseArrayLayer = 0,
                .imageViewBinding = *dpb[i].second};

            h264_ref[i] = StdVideoEncodeH264ReferenceInfo{
                // TODO: set data
            };
            ref_h264_slots[i] = vk::VideoEncodeH264DpbSlotInfoKHR{
                .pStdReferenceInfo = &h264_ref[i],
            };
            ref_slots[i] = vk::VideoReferenceSlotInfoKHR{
                .pNext = &ref_h264_slots[i],
                .slotIndex = -1,
                .pPictureResource = nullptr,
            };
        }

        vk::raii::Fence fence(dev, vk::FenceCreateInfo{});
        vk::raii::Semaphore sem(dev, vk::SemaphoreCreateInfo{});

        slot_info frames(dpb.size());
        uint16_t idr_id = 0;
        for (int32_t frame = 0;; ++frame) {
            std::cerr << "encoding frame " << frame << std::endl;
            size_t slot = frames.get_slot();
            auto ref = frames.get_ref();
            assert(not(ref and (*ref == slot)));

            ref_slots[slot].slotIndex = -1;
            ref_slots[slot].pPictureResource = &ref_pics[slot];

            {
                gfx_cmd_buffer.reset();
                gfx_cmd_buffer.begin({});
                pattern.draw(gfx_cmd_buffer, gfx_queue.familyIndex,
                             encode_queue.familyIndex);
                gfx_cmd_buffer.end();

                vk::SubmitInfo submit{};
                submit.setCommandBuffers(*gfx_cmd_buffer);
                submit.setSignalSemaphores(*sem);
                gfx_queue.queue.submit(submit);
            }

            cmd_buffer.reset();
            cmd_buffer.begin({});

            vk::ImageMemoryBarrier2 im_barrier{
                .srcStageMask = vk::PipelineStageFlagBits2KHR::eBottomOfPipe,
                .srcAccessMask = vk::AccessFlagBits2::eNone,
                .dstStageMask = vk::PipelineStageFlagBits2KHR::eVideoEncodeKHR,
                .dstAccessMask = vk::AccessFlagBits2::eVideoEncodeReadKHR,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eVideoEncodeSrcKHR,
                .srcQueueFamilyIndex = gfx_queue.familyIndex,
                .dstQueueFamilyIndex = encode_queue.familyIndex,
                .image = *img_in,
                .subresourceRange = {.aspectMask =
                                         vk::ImageAspectFlagBits::eColor,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
            };
            vk::DependencyInfo dep_info{};
            dep_info.setImageMemoryBarriers(im_barrier);
            cmd_buffer.pipelineBarrier2(dep_info);
            cmd_buffer.resetQueryPool(*query_pool, 0, 1);
            vk::VideoBeginCodingInfoKHR video_coding_begin_info{
                .videoSession = *video_session,
                .videoSessionParameters = *video_session_params,
            };
            video_coding_begin_info.setReferenceSlots(ref_slots);
            cmd_buffer.beginVideoCodingKHR(video_coding_begin_info);

            if (frame == 0)
                cmd_buffer.controlVideoCodingKHR(
                    {.flags = vk::VideoCodingControlFlagBitsKHR::eReset});

            StdVideoEncodeH264SliceHeader std_slice{
                .flags =
                    {
                        .direct_spatial_mv_pred_flag = 0,  //?
                        .num_ref_idx_active_override_flag = 0,
                        .reserved = 0,
                    },
                .first_mb_in_slice = 0,
                .slice_type = ref ? STD_VIDEO_H264_SLICE_TYPE_P
                                  : STD_VIDEO_H264_SLICE_TYPE_I,
                .slice_alpha_c0_offset_div2 = 0,
                .slice_beta_offset_div2 = 0,
                .slice_qp_delta = 0,
                .reserved1 = 0,
                .cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0,
                .disable_deblocking_filter_idc =
                    STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED,
                .pWeightTable = nullptr,
            };
            vk::VideoEncodeH264NaluSliceInfoKHR nalu{
                .pStdSliceHeader = &std_slice,
            };
            std::vector<StdVideoEncodeH264ReferenceListsInfo> refs;
            if (ref) {
                auto& i =
                    refs.emplace_back(StdVideoEncodeH264ReferenceListsInfo{
                        .flags =
                            {
                                .ref_pic_list_modification_flag_l0 = 0,
                                .ref_pic_list_modification_flag_l1 = 0,
                                .reserved = 0,
                            },
                        .num_ref_idx_l0_active_minus1 = 0,
                        .num_ref_idx_l1_active_minus1 = 0,
                        .RefPicList0 = {},
                        .RefPicList1 = {},
                        .refList0ModOpCount = 0,
                        .refList1ModOpCount = 0,
                        .refPicMarkingOpCount = 0,
                        .reserved1 = {},
                        .pRefList0ModOperations = nullptr,
                        .pRefList1ModOperations = nullptr,
                        .pRefPicMarkingOperations = nullptr,
                    });
                std::fill(i.RefPicList0, i.RefPicList0 + sizeof(i.RefPicList0),
                          STD_VIDEO_H264_NO_REFERENCE_PICTURE);
                std::fill(i.RefPicList1, i.RefPicList1 + sizeof(i.RefPicList1),
                          STD_VIDEO_H264_NO_REFERENCE_PICTURE);
                i.RefPicList0[0] = *ref;
            }
            StdVideoEncodeH264PictureInfo std_pic_info{
                .flags =
                    {
                        .IdrPicFlag = uint32_t(ref ? 0 : 1),
                        .is_reference = 1,
                        .no_output_of_prior_pics_flag = 0,
                        .long_term_reference_flag = 0,
                        .adaptive_ref_pic_marking_mode_flag = 0,
                        .reserved = 0,
                    },
                .seq_parameter_set_id = 0,
                .pic_parameter_set_id = 0,
                .idr_pic_id = idr_id,
                .primary_pic_type = ref ? STD_VIDEO_H264_PICTURE_TYPE_P
                                        : STD_VIDEO_H264_PICTURE_TYPE_IDR,
                .frame_num = uint32_t(frame),
                .PicOrderCnt = 0,
                .temporal_id = 0,
                .reserved1 = {},
                .pRefLists = ref ? refs.data() : nullptr,
            };
            vk::VideoEncodeH264PictureInfoKHR pic_info{
                .pStdPictureInfo = &std_pic_info,
                .generatePrefixNalu =
                    false,  // check if useful, check if supported
            };
            pic_info.setNaluSliceEntries(nalu);

            ref_slots[slot].slotIndex = slot;
            h264_ref[slot].primary_pic_type = std_pic_info.primary_pic_type;
            h264_ref[slot].FrameNum = std_pic_info.frame_num;
            vk::VideoEncodeInfoKHR encode_info{
                .pNext = &pic_info,
                .dstBuffer = *out_buffer,
                .dstBufferOffset = 0,
                .dstBufferRange = buffer_size,
                .srcPictureResource = {.codedExtent = extent,
                                       .baseArrayLayer = 0,
                                       .imageViewBinding = *img_in_view},
                .pSetupReferenceSlot = &ref_slots[slot],
            };
            if (ref) encode_info.setReferenceSlots(ref_slots[*ref]);

            cmd_buffer.beginQuery(*query_pool, 0);
            cmd_buffer.encodeVideoKHR(encode_info);
            cmd_buffer.endQuery(*query_pool, 0);
            cmd_buffer.endVideoCodingKHR({});
            cmd_buffer.end();

            if (ref) {
            } else {
                ++idr_id;
            }

            frames[slot] = frame;

            vk::PipelineStageFlags stage =
                vk::PipelineStageFlagBits::eTopOfPipe;
            vk::SubmitInfo submit{};
            submit.setCommandBuffers(*cmd_buffer);
            submit.setWaitSemaphores(*sem);
            submit.setWaitDstStageMask(stage);
            encode_queue.queue.submit(submit, *fence);
            auto res = dev.waitForFences(*fence, true, 1'000'000'000);
            if (res != vk::Result::eSuccess) {
                throw std::runtime_error("wait for fences: " +
                                         vk::to_string(res));
            }
            {
                auto [res, size] = query_pool.getResults<uint32_t>(
                    0, 1, 3 * sizeof(uint32_t), 0,
                    vk::QueryResultFlagBits::eWait);
                if (res != vk::Result::eSuccess) {
                    throw std::runtime_error("query_pool.getResult: " +
                                             vk::to_string(res));
                }
                std::cerr << "query result:";
                for (const auto& i : size) std::cerr << " " << i;
                std::cerr << std::endl;
                out.write((char*)mapped_buffer, size[1]);
                out.flush();
            }
            dev.resetFences(*fence);
        }

    } catch (std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
