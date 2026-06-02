#ifndef SEQUENCE_EDITOR_HPP
#define SEQUENCE_EDITOR_HPP

#include "cJSON.h"
#include "dwarf_parser.hpp"
#include "jlink_port.hpp"
#include "module.h"
#include "monitor_types.hpp"
#include "parser.hpp"
#include "shm.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct SequenceAction {
        std::string name;
        u64         addr;
        DataType    type;
        std::string port; // "JLINK", "SHM", "LOCAL"
        std::string shmName;
        u64         typeOff;
        u32         bitOffset;
        u32         bitSize;

        std::string targetValue;

        bool                                     isEnum;
        std::vector<std::pair<i64, std::string>> enumDefs;
};

struct SequenceStep {
        std::string                 name;
        u32                         delayMs; // Wait time BEFORE executing this step
        std::vector<SequenceAction> actions;
};

class SequenceEditor
{
      public:
        SequenceEditor();
        ~SequenceEditor();

        void draw();

        bool isOpen() const { return show_; }
        void setOpen(bool o) { show_ = o; }

        bool isModified() const { return isModified_; }
        void clearModified() { isModified_ = false; }

        // Add getters/setters for JSON serialization
        std::vector<SequenceStep> &getSteps() { return steps_; }
        void                       setSteps(const std::vector<SequenceStep> &steps)
        {
                steps_      = steps;
                isModified_ = true;
        }

        void saveSession(cJSON *root);
        void loadSession(cJSON *root);

        void exportToFile(const std::string &path);
        void importFromFile(const std::string &path);

      private:
        void writeAction(const SequenceAction &action);
        void refreshSeqFiles();

        std::string              seqFolder_ = ".";
        std::vector<std::string> seqFiles_;
        int                      selectedSeqIdx_ = -1;

        std::vector<SequenceStep> steps_;
        int                       selectedStep_ = -1;

        enum class State { IDLE, RUNNING };
        State state_ = State::IDLE;

        int currentStepIdx_ = 0;
        u64 stepStartTime_  = 0;

        char savePathBuf_[256] = "";

        bool show_       = false;
        bool isModified_ = false;
};

#endif // SEQUENCE_EDITOR_HPP
