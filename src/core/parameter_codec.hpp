#pragma once

#include "../../ckit/include/param_list.h"
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace parameter_codec
{
struct Field {
        param_list_descriptor descriptor;
        std::string           path;
};

// 描述表的位置来自当前 AXF; 记录的校验和字段写入由 module 完成
class Layout
{
        size_t                             configSize_;
        std::vector<Field>                 fields_;
        std::vector<param_list_descriptor> descriptors_;
        std::vector<param_list_descriptor> packedDescriptors_;

        param_list schema(bool packed = false) const
        {
                const auto &descriptors = packed ? packedDescriptors_ : descriptors_;
                return {descriptors.data(),
                        static_cast<uint32_t>(descriptors.size()),
                        packed ? fields_.size() * sizeof(uint32_t) : configSize_,
                        nullptr};
        }

        static void check(int result)
        {
                if (result == PARAM_LIST_MISMATCH)
                        throw std::runtime_error("Parameter type or revision differs from the current layout");
                if (result < 0)
                        throw std::runtime_error("Invalid parameter layout, record or value");
        }

      public:
        Layout(size_t configSize, std::vector<Field> fields) : configSize_(configSize), fields_(std::move(fields))
        {
                if (!configSize_ || fields_.size() > (size_t{UINT16_MAX} + 1) / sizeof(uint32_t))
                        throw std::runtime_error("Parameter layout exceeds module limits");
                for (size_t i = 0; i < fields_.size(); ++i) {
                        const auto &descriptor = fields_[i].descriptor;
                        descriptors_.push_back(descriptor);
                        auto packed   = descriptor;
                        packed.offset = static_cast<uint16_t>(i * sizeof(uint32_t));
                        packed.shift  = 0;
                        packed.bits   = 32;
                        packedDescriptors_.push_back(packed);
                }
        }

        bool                      is_empty() const { return fields_.empty(); }
        const std::vector<Field> &fields() const { return fields_; }

        std::vector<uint8_t> encode(std::span<const uint8_t> config) const
        {
                if (config.size() != configSize_)
                        throw std::runtime_error("Configuration size differs from parameter layout");
                if (is_empty())
                        return {};
                const auto           list = schema();
                std::vector<uint8_t> records(fields_.size() * PARAM_LIST_RECORD_SIZE);
                check(param_list_encode(&list, config.data(), records.data(), records.size()));
                return records;
        }

        std::vector<uint8_t> encode_values(std::span<const uint32_t> values) const
        {
                if (values.size() != fields_.size())
                        throw std::runtime_error("Parameter value count differs from layout");
                if (is_empty())
                        return {};
                const auto           list = schema(true);
                std::vector<uint8_t> records(fields_.size() * PARAM_LIST_RECORD_SIZE);
                const int            result = param_list_encode(&list, values.data(), records.data(), records.size());
                if (result < 0) {
                        param_list_invalid_field invalid{};
                        if (param_list_first_invalid(&list, values.data(), &invalid))
                                for (const auto &field : fields_)
                                        if (field.descriptor.id == invalid.id)
                                                throw std::runtime_error(field.path + ": invalid parameter value");
                        check(result);
                }
                return records;
        }

        std::vector<uint32_t> read_values(std::span<const uint8_t> config) const
        {
                const auto            records = encode(config);
                std::vector<uint32_t> values(fields_.size());
                if (!is_empty()) {
                        const auto list = schema(true);
                        check(param_list_decode(&list, values.data(), records.data(), records.size()));
                }
                return values;
        }

        void decode(std::span<uint8_t> config, std::span<const uint8_t> records) const
        {
                if (config.size() != configSize_)
                        throw std::runtime_error("Configuration size differs from parameter layout");
                const auto list = schema();
                check(param_list_decode(&list, config.data(), records.data(), records.size()));
        }
};
} // namespace parameter_codec
