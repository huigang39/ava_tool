#ifndef ASSEMBLY_VIEWER_HPP
#define ASSEMBLY_VIEWER_HPP

#include <string>
#include <vector>

class Gui;

class AssemblyViewer
{
      public:
        AssemblyViewer() = default;

        void draw(Gui *gui);

        bool show_{false};

      private:
        std::string disassembly_{};
        std::string currentElfPath_{};
        bool        isLoading_{false};

        void disassemble(const std::string &elfPath);
};

#endif // !ASSEMBLY_VIEWER_HPP
