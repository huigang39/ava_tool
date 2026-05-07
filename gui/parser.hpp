#ifndef PARSER_HPP
#define PARSER_HPP

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "module.h"

#include "cJSON.h"
#include "dwarf_parser.hpp"
#include "elf_parser.hpp"
#include <chrono>
#include <filesystem>

class Parser
{
      public:
        enum class DataType {
                UNKNOWN,
                F32,
                U32,
                I32,
                ARRAY,
        };

        struct DataTree {
                std::string                 name;
                DataType                    type;
                std::variant<f32, u32, i32> val;
                std::vector<DataTree>       children;
        };

      private:
        std::string name_{};
        std::string cfgPath_{}, binPath_{}, elfPath_{};
        DataTree    dataTree_{
               .name = "CFG",
               .type = DataType::ARRAY,
        };
        int toastDismissTime_{2000};

        ElfInfo        elfInfo_{};
        dwarf::Info    dwarfInfo_{};
        char           elfFilter_[128]{};
        bool           elfFilterObjectsOnly_{true};
        int            elfArrayMaxElems_{64};
        std::filesystem::file_time_type elfLastWriteTime_{};
        bool                            elfReloaded_{false};

        enum class ParserState {
                None,
                LoadCfg,
                LoadBin,
                LoadElf,
        };
        ParserState state_ = ParserState::None;
        bool open_ = true;

        void menu();
        void draw();

        static bool readFile(const std::string &filename, std::string &outContent);

        static void        parseCfg(const cJSON *jsonNode, DataTree &node);
        static bool        parseBin(std::ifstream &ifs, DataTree &node);
        void               drawElfSymbols();
        void               drawVarRow(const std::string &displayName, const std::string &fullPath, u64 addr, u64 typeOff,
                                      int depth);

        void handleDroppedFile(const std::string &path);

        static const char *dataTypeToStr(DataType type);
        static DataType    strToDataType(const std::string &str);

        static void drawDataTree(DataTree &node, int indentLevel = 0);

      public:
        explicit Parser(std::string parserName) : name_(std::move(parserName)) { print_info(true, "Parser()"); }

        Parser() { print_info(true, "Parser()"); };
        ~Parser() { print_info(true, "~Parser()"); };

        bool loadCfg(const std::string &cfgPath);
        bool loadBin(const std::string &binPath);
        bool loadElf(const std::string &elfPath);

        void updateDisplay();

        const std::string &getName() const { return name_; }
        const std::string &getCfgPath() const { return cfgPath_; }
        const std::string &getBinPath() const { return binPath_; }
        const std::string &getElfPath() const { return elfPath_; }
        bool               consumeElfReloaded() { bool r = elfReloaded_; elfReloaded_ = false; return r; }
        const ElfInfo     &getElfInfo() const { return elfInfo_; }
        bool               isPendingDelete() const { return !open_; }
};

#endif // !PARSER_HPP
