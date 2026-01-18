#include "ImGuiFileDialog.h"
#include "ImGuiNotify.hpp"
#include "imgui.h"

#include "editor.hpp"

const char *
Editor::dataTypeToStr(const DataType type)
{
        switch (type) {
                case DataType::F32:
                        return "f32";
                case DataType::U32:
                        return "u32";
                case DataType::I32:
                        return "i32";
                case DataType::ARRAY:
                        return "array";
                default:
                        return "unknown";
        }
}

Editor::DataType
Editor::strToDataType(const std::string &str)
{
        auto type = DataType::UNKNOWN;
        if (str == "f32")
                type = DataType::F32;
        else if (str == "u32")
                type = DataType::U32;
        else if (str == "i32")
                type = DataType::I32;
        else if (str == "array")
                type = DataType::ARRAY;

        return type;
}

bool
Editor::readFile(const std::string &filename, std::string &outContent)
{
        std::ifstream ifs(filename);
        if (!ifs.is_open())
                return false;

        std::stringstream ss;
        ss << ifs.rdbuf();
        outContent = ss.str();
        return true;
}

void
Editor::parseCfg(const cJSON *jsonNode, DataTree &node)
{
        const cJSON *nameItem = cJSON_GetObjectItem(jsonNode, "name");
        const cJSON *typeItem = cJSON_GetObjectItem(jsonNode, "type");

        if (!nameItem || !typeItem)
                return;

        node.name = nameItem->valuestring;

        if (const std::string typeStr = typeItem->valuestring; typeStr == "f32")
                node.type = DataType::F32;
        else if (typeStr == "u32")
                node.type = DataType::U32;
        else if (typeStr == "i32")
                node.type = DataType::I32;
        else if (typeStr == "array") {
                node.type = DataType::ARRAY;
                if (const cJSON *childrenJson = cJSON_GetObjectItem(jsonNode, "array")) {
                        for (const cJSON *child = childrenJson->child; child; child = child->next) {
                                DataTree childNode;
                                parseCfg(child, childNode);
                                node.children.push_back(std::move(childNode));
                        }
                }
        }
}

bool
Editor::loadCfg(const std::string &cfgPath)
{
        std::string content;
        if (!readFile(cfgPath, content))
                return false;

        cJSON *root = cJSON_Parse(content.c_str());
        if (!root)
                return false;

        const cJSON *cfgJson = cJSON_GetObjectItem(root, "CFG");
        if (!cfgJson) {
                cJSON_Delete(root);
                return false;
        }

        dataTree_.children.clear();
        for (const cJSON *f = cfgJson->child; f; f = f->next) {
                DataTree node;
                parseCfg(f, node);
                dataTree_.children.push_back(std::move(node));
        }

        cJSON_Delete(root);
        return true;
}

bool
Editor::storeCfg(const std::string &cfgPath) const
{
        cJSON *root = cJSON_CreateObject();
        cJSON *cfg  = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "CFG", cfg);

        std::function<void(const DataTree &, cJSON *)> serialize = [&](const DataTree &node, cJSON *parent) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", node.name.c_str());
                cJSON_AddStringToObject(obj, "type", dataTypeToStr(node.type));

                if (node.type == DataType::ARRAY) {
                        cJSON *arr = cJSON_CreateArray();
                        for (auto &child : node.children)
                                serialize(child, arr);
                        cJSON_AddItemToObject(obj, "array", arr);
                }

                if (parent->type == cJSON_Array)
                        cJSON_AddItemToArray(parent, obj);
                else
                        cJSON_AddItemToObject(parent, node.name.c_str(), obj);
        };

        for (auto &n : dataTree_.children)
                serialize(n, cfg);

        char         *out = cJSON_Print(root);
        std::ofstream ofs(cfgPath);
        ofs << out;
        cJSON_free(out);
        cJSON_Delete(root);

        return static_cast<bool>(ofs);
}

bool
Editor::parseBin(std::ifstream &ifs, DataTree &node)
{
        switch (node.type) {
                case DataType::F32: {
                        f32 val = 0.0f;
                        ifs.read(reinterpret_cast<char *>(&val), sizeof(f32));
                        if (!ifs)
                                return false;

                        node.val = val;
                        break;
                }
                case DataType::U32: {
                        u32 val = 0;
                        ifs.read(reinterpret_cast<char *>(&val), sizeof(u32));
                        if (!ifs)
                                return false;

                        node.val = val;
                        break;
                }
                case DataType::I32: {
                        i32 val = 0;
                        ifs.read(reinterpret_cast<char *>(&val), sizeof(i32));
                        if (!ifs)
                                return false;

                        node.val = val;
                        break;
                }
                case DataType::ARRAY: {
                        for (auto &child : node.children) {
                                if (!parseBin(ifs, child))
                                        return false;
                        }
                        break;
                }
                default:
                        return false;
        }
        return true;
}

bool
Editor::loadBin(const std::string &binPath)
{
        std::ifstream ifs(binPath, std::ios::binary);
        if (!ifs.is_open())
                return false;

        for (auto &child : dataTree_.children) {
                if (!parseBin(ifs, child))
                        return false;
        }
        return true;
}

bool
Editor::storeBin(const std::string &binPath) const
{
        std::ofstream ofs(binPath, std::ios::binary);
        if (!ofs.is_open())
                return false;

        std::function<void(const DataTree &)> writeNode = [&](const DataTree &node) {
                switch (node.type) {
                        case DataType::F32: {
                                f32 val = std::get<f32>(node.val);
                                ofs.write(reinterpret_cast<char *>(&val), sizeof(val));
                                break;
                        }
                        case DataType::U32: {
                                u32 val = std::get<u32>(node.val);
                                ofs.write(reinterpret_cast<char *>(&val), sizeof(val));
                                break;
                        }
                        case DataType::I32: {
                                i32 val = std::get<i32>(node.val);
                                ofs.write(reinterpret_cast<char *>(&val), sizeof(val));
                                break;
                        }
                        case DataType::ARRAY: {
                                for (auto &child : node.children)
                                        writeNode(child);
                                break;
                        }
                        default:
                                break;
                }
        };

        for (auto &n : dataTree_.children)
                writeNode(n);

        return static_cast<bool>(ofs);
}

void
Editor::drawDataTree(DataTree &node, const int indentLevel)
{
        // 本节点行
        ImGui::TableNextRow();

        // ---------------- Column 0: Name (tree node or simple name) ----------------
        ImGui::TableSetColumnIndex(0);

        // 生成稳定唯一 ID 后缀（用节点地址）
        const auto        addr     = reinterpret_cast<uintptr_t>(&node);
        const std::string idSuffix = std::to_string(addr);

        // 缩进显示层级（仅视觉效果）
        ImGui::Indent(indentLevel * 10.0f);

        // 如果是 ARRAY，用一个可展开的 tree 小箭头；否则正常显示/编辑 name
        const bool isArray = (node.type == DataType::ARRAY);
        bool       opened  = false;
        if (isArray) {
                // 注意：不要使用 SpanFullWidth，否则会跨列并导致错位
                opened = ImGui::TreeNodeEx((std::string("##tree_") + idSuffix).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
                // 在 tree 前显示节点名称（TreeNodeEx 本身不打印文本，故也显示 name）
                ImGui::SameLine();
                // 可编辑名字：按 Enter 确认（使用 unique id）
                char nameBuf[128];
                snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
                if (ImGui::InputText((std::string("##name_") + idSuffix).c_str(),
                                     nameBuf,
                                     sizeof(nameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                        node.name = nameBuf;
                }
        } else {
                // 非 array：直接显示可编辑 name（按 Enter 确认）
                char nameBuf[128];
                snprintf(nameBuf, sizeof(nameBuf), "%s", node.name.c_str());
                if (ImGui::InputText((std::string("##name_") + idSuffix).c_str(),
                                     nameBuf,
                                     sizeof(nameBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                        node.name = nameBuf;
                }
        }

        ImGui::Unindent(static_cast<f32>(indentLevel) * 10.0f);

        // ---------------- Column 1: Type (Combo) ----------------
        ImGui::TableSetColumnIndex(1);
        // Combo 显示需要确保每次使用唯一 id，否则同名会冲突
        const char *types[]     = {"f32", "u32", "i32", "array"};
        int         currentType = static_cast<int>(node.type);
        if (ImGui::Combo((std::string("##type_") + idSuffix).c_str(), &currentType, types, IM_ARRAYSIZE(types))) {
                node.type = static_cast<DataType>(currentType);
                // 当类型变化时，可能需要初始化 node.val 或 children（根据你的需求处理）
        }

        // ---------------- Column 2: Value (editable / disabled) ----------------
        ImGui::TableSetColumnIndex(2);
        switch (node.type) {
                case DataType::F32: {
                        // 确保 variant 中已有 f32（如果没有，可先初始化为 0）
                        if (!std::holds_alternative<f32>(node.val))
                                node.val = 0.0f;
                        f32 val = std::get<f32>(node.val);
                        if (ImGui::InputFloat((std::string("##val_") + idSuffix).c_str(), &val, 0.0f, 0.0f, "%.6f"))
                                node.val = val;
                        break;
                }
                case DataType::U32: {
                        if (!std::holds_alternative<u32>(node.val))
                                node.val = static_cast<u32>(0);
                        const u32 val = std::get<u32>(node.val);
                        int       tmp = static_cast<int>(val);
                        if (ImGui::InputInt((std::string("##val_") + idSuffix).c_str(), &tmp))
                                node.val = static_cast<u32>(tmp);
                        break;
                }
                case DataType::I32: {
                        if (!std::holds_alternative<i32>(node.val))
                                node.val = static_cast<i32>(0);
                        i32 val = std::get<i32>(node.val);
                        if (ImGui::InputInt((std::string("##val_") + idSuffix).c_str(), &val))
                                node.val = val;
                        break;
                }
                case DataType::ARRAY: {
                        ImGui::TextDisabled("(array)");
                        break;
                }
                default:
                        ImGui::TextDisabled("(unknown)");
                        break;
        }

        // ---------------- 子节点（如果是 array 并且展开） ----------------
        if (isArray && opened) {
                // 子节点应该从新的一行开始绘制，每个子节点都会调用 TableNextRow()
                for (auto &child : node.children) {
                        // drawDataTree(child, indentLevel + 1) 内部会调用 TableNextRow()
                        drawDataTree(child, indentLevel + 1);
                }
                ImGui::TreePop();
        }
}

void
Editor::menu()
{
        if (ImGui::BeginPopupContextWindow("EditorMenu")) {
                if (ImGui::MenuItem("Load CFG")) {
                        state_ = EditorState::LoadCfg;

                        const IGFD::FileDialogConfig cfg = {
                            .fileName     = "CFG",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("ChooseCfgFile", "choose CFG file", ".json", cfg);
                }

                if (ImGui::MenuItem("Load BIN")) {
                        state_ = EditorState::LoadBin;

                        const IGFD::FileDialogConfig bin = {
                            .fileName     = "BIN",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("ChooseBinFile", "choose BIN file", ".bin", bin);
                }

                if (ImGui::MenuItem("Store CFG")) {
                        state_ = EditorState::StoreCfg;

                        const IGFD::FileDialogConfig cfg = {
                            .fileName     = "CFG",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("StoreCfgFile", "save CFG file", ".json", cfg);
                }

                if (ImGui::MenuItem("Store BIN")) {
                        state_ = EditorState::StoreBin;

                        const IGFD::FileDialogConfig bin = {
                            .fileName     = "BIN",
                            .filePathName = ".",
                        };

                        ImGuiFileDialog::Instance()->OpenDialog("StoreBinFile", "save BIN file", ".bin", bin);
                }

                ImGui::EndPopup();
        }

        switch (state_) {
                case EditorState::LoadCfg: {
                        if (ImGuiFileDialog::Instance()->Display("ChooseCfgFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        cfgPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (loadCfg(cfgPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "load CFG success: %s",
                                                                           cfgPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "load CFG failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::LoadBin: {
                        if (ImGuiFileDialog::Instance()->Display("ChooseBinFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        binPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (loadBin(binPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "load BIN success: %s",
                                                                           binPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "load BIN failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::StoreCfg: {
                        if (ImGuiFileDialog::Instance()->Display("StoreCfgFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        cfgPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (storeCfg(cfgPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "store CFG success: %s",
                                                                           cfgPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "store CFG failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                case EditorState::StoreBin: {
                        if (ImGuiFileDialog::Instance()->Display("StoreBinFile")) {
                                if (ImGuiFileDialog::Instance()->IsOk()) {
                                        binPath_ = ImGuiFileDialog::Instance()->GetFilePathName();

                                        if (storeBin(binPath_))
                                                ImGui::InsertNotification({ImGuiToastType::Success,
                                                                           toastDismissTime_,
                                                                           "store BIN success: %s",
                                                                           binPath_.c_str()});
                                        else
                                                ImGui::InsertNotification(
                                                    {ImGuiToastType::Error, toastDismissTime_, "store BIN failure"});
                                }
                                state_ = EditorState::None;
                                ImGuiFileDialog::Instance()->Close();
                        }
                        break;
                }
                default:
                        break;
        }
}

void
Editor::draw()
{
        if (ImGui::BeginTable(
                "DataEditorTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();

                for (auto &child : dataTree_.children)
                        drawDataTree(child);

                ImGui::EndTable();
        }
}

void
Editor::updateDisplay()
{
        if (ImGui::Begin(name_.c_str())) {
                menu();
                draw();
                ImGui::End();
        }
}
