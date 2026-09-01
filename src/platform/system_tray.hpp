#pragma once
#include <memory>

class SystemTray
{
      public:
        enum class Action { None, Restore, Exit };
        SystemTray();
        ~SystemTray();
        bool   show();
        void   remove();
        Action takeAction();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
};
