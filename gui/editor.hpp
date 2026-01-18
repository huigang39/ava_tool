#ifndef EDITOR_HPP
#define EDITOR_HPP

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "module.h"

#include "cJSON.h"

class Editor
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
        std::string cfgPath_{}, binPath_{};
        DataTree    dataTree_{
               .name = "CFG",
               .type = DataType::ARRAY,
        };
        int toastDismissTime_{2000};

        enum class EditorState {
                None,
                LoadCfg,
                LoadBin,
                StoreCfg,
                StoreBin,
        };

        EditorState state_ = EditorState::None;

        void menu();
        void draw();

        static bool readFile(const std::string &filename, std::string &outContent);

        static void        parseCfg(const cJSON *jsonNode, DataTree &node);
        bool               loadCfg(const std::string &cfgPath);
        [[nodiscard]] bool storeCfg(const std::string &cfgPath) const;

        static bool        parseBin(std::ifstream &ifs, DataTree &node);
        bool               loadBin(const std::string &binPath);
        [[nodiscard]] bool storeBin(const std::string &binPath) const;

        static const char *dataTypeToStr(DataType type);
        static DataType    strToDataType(const std::string &str);

        static void drawDataTree(DataTree &node, int indentLevel = 0);

      public:
        explicit Editor(std::string eidtorName) : name_(std::move(eidtorName)) { print_info(true, "Editor()"); }

        Editor() { print_info(true, "Editor()"); };
        ~Editor() { print_info(true, "~Editor()"); };

        void updateDisplay();
};

#endif // !EDITOR_HPP
