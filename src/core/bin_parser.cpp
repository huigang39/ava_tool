#include "core/bin_parser.hpp"

bool BinParser::parseRecursive(std::ifstream &ifs, DataTree &node)
{
    u32 sz = Parser::typeBytes(node.type);
    if (sz > 0) {
        u8 buf[8] = {0};
        ifs.read(reinterpret_cast<char*>(buf), sz);
        if (!ifs) return false;
        
        switch (node.type) {
            case DataType::F32: node.val = *(f32*)buf; break;
            case DataType::F64: node.val = *(f64*)buf; break;
            case DataType::U8:  node.val = *(u8*)buf;  break;
            case DataType::U16: node.val = *(u16*)buf; break;
            case DataType::U32: node.val = *(u32*)buf; break;
            case DataType::U64: node.val = *(u64*)buf; break;
            case DataType::I8:  node.val = *(i8*)buf;  break;
            case DataType::I16: node.val = *(i16*)buf; break;
            case DataType::I32: node.val = *(i32*)buf; break;
            case DataType::I64: node.val = *(i64*)buf; break;
            default: break;
        }
        return true;
    }

    if (node.type == DataType::ARRAY) {
        for (auto &child : node.children) {
            if (!parseRecursive(ifs, child))
                return false;
        }
        return true;
    }

    return false;
}

bool BinParser::parse(const std::string &path)
{
    path_ = path;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open())
        return false;

    for (auto &child : dataTree_.children) {
        if (!parseRecursive(ifs, child))
            return false;
    }
    return true;
}

void BinParser::setTemplate(const DataTree& tree) {
    dataTree_ = tree;
}
