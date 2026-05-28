#ifndef REGISTER_VIEWER_HPP
#define REGISTER_VIEWER_HPP

#include <string>

class RegisterViewer
{
      public:
        RegisterViewer() = default;

        void draw();

        bool show_{false};

      private:
        struct RegDesc {
                const char *name;
                int         jlinkIndex;
        };

        static const RegDesc kRegs[];
};

#endif // !REGISTER_VIEWER_HPP
