#pragma once
#include "gui/mujoco_actuator.hpp"
#include "gui/mujoco_control.hpp"
#include <memory>
#include <string>

class MuJoCoView
{
        struct Impl;
        std::unique_ptr<Impl> impl_;

      public:
        MuJoCoView();
        ~MuJoCoView();
        bool                       loadModel(const std::string &path);
        bool                       createActuator(const std::string &path, const ava_actuator::Config &config);
        bool                       start(const std::string &region);
        void                       stop();
        void                       setPaused(bool paused);
        void                       reset();
        void                       tick();
        void                       draw(const std::string &region, bool live, double position);
        unsigned                   drawControls();
        const ava_mujoco::Control &control() const;
        bool                       setControl(const ava_mujoco::Control &config);
        bool                       running() const;
        const std::string         &modelPath() const;
        const std::string         &error() const;
        static std::string         defaultModelPath();
};
