#ifndef PARSER_HPP
#define PARSER_HPP

#include "ckit.h"
#include <algorithm>
#include <string>
#include <variant>
#include <vector>

enum class DataType {
        UNKNOWN,
        F32,
        F64,
        U8,
        U16,
        U32,
        U64,
        I8,
        I16,
        I32,
        I64,
        ARRAY,
        F16,
};

struct DataTree {
        std::string                                                  name;
        DataType                                                     type;
        std::variant<f32, f64, u8, u16, u32, u64, i8, i16, i32, i64> val;
        std::vector<DataTree>                                        children;
};

class Parser
{
      public:
        virtual ~Parser()                                         = default;
        virtual bool               parse(const std::string &path) = 0;
        virtual const DataTree    &getDataTree() const            = 0;
        virtual const std::string &getPath() const                = 0;
        /**
         * @brief tree
         */
        virtual void setTemplate(const DataTree &) {}

        static const char *dataTypeToStr(DataType type)
        {
                switch (type) {
                        case DataType::F16:
                                return "f16";
                        case DataType::F32:
                                return "f32";
                        case DataType::F64:
                                return "f64";
                        case DataType::U8:
                                return "u8";
                        case DataType::U16:
                                return "u16";
                        case DataType::U32:
                                return "u32";
                        case DataType::U64:
                                return "u64";
                        case DataType::I8:
                                return "i8";
                        case DataType::I16:
                                return "i16";
                        case DataType::I32:
                                return "i32";
                        case DataType::I64:
                                return "i64";
                        case DataType::ARRAY:
                                return "array";
                        default:
                                return "unknown";
                }
        }

        static DataType strToDataType(const std::string &str)
        {
                std::string s = str;
                std::transform(s.begin(), s.end(), s.begin(), ::toupper);
                if (s == "F16")
                        return DataType::F16;
                if (s == "F32")
                        return DataType::F32;
                if (s == "F64")
                        return DataType::F64;
                if (s == "U8")
                        return DataType::U8;
                if (s == "U16")
                        return DataType::U16;
                if (s == "U32")
                        return DataType::U32;
                if (s == "U64")
                        return DataType::U64;
                if (s == "I8")
                        return DataType::I8;
                if (s == "I16")
                        return DataType::I16;
                if (s == "I32")
                        return DataType::I32;
                if (s == "I64")
                        return DataType::I64;
                if (s == "ARRAY")
                        return DataType::ARRAY;
                return DataType::UNKNOWN;
        }

        static u32 typeBytes(DataType type)
        {
                switch (type) {
                        case DataType::U8:
                        case DataType::I8:
                                return 1;
                        case DataType::U16:
                        case DataType::I16:
                        case DataType::F16:
                                return 2;
                        case DataType::F32:
                        case DataType::U32:
                        case DataType::I32:
                                return 4;
                        case DataType::F64:
                        case DataType::U64:
                        case DataType::I64:
                                return 8;
                        default:
                                return 0;
                }
        }
};

#endif // !PARSER_HPP
