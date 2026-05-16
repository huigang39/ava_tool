#ifndef PARSER_HPP
#define PARSER_HPP

#include "module.h"
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
        virtual void               setTemplate(const DataTree               &/*tree*/) {}

        static const char *dataTypeToStr(DataType type)
        {
                switch (type) {
                        case DataType::F32:
                                return "F32";
                        case DataType::F64:
                                return "F64";
                        case DataType::U8:
                                return "U8";
                        case DataType::U16:
                                return "U16";
                        case DataType::U32:
                                return "U32";
                        case DataType::U64:
                                return "U64";
                        case DataType::I8:
                                return "I8";
                        case DataType::I16:
                                return "I16";
                        case DataType::I32:
                                return "I32";
                        case DataType::I64:
                                return "I64";
                        case DataType::ARRAY:
                                return "ARRAY";
                        default:
                                return "UNKNOWN";
                }
        }

        static DataType strToDataType(const std::string &str)
        {
                std::string s = str;
                std::transform(s.begin(), s.end(), s.begin(), ::toupper);
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
