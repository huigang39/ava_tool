#include <memory>
#include <ranges>
#include <thread>

#include "module.h"

#include "gui.hpp"

fft_t fft;

u64 cnt;

void threadFunc(Gui::MonitorMapType *monitors) {
  while (true) {
    for (const auto &monitor : *monitors | std::views::values) {
      if (MonitorChannel *ch = monitor->findChannel("scope_0", "ch_0");
          ch != nullptr) {
        u64 val;
        shm_read(&ch->getShm(), &val, sizeof(val));
        ch->setRVal(static_cast<f32>(val));
        print_info(true, "val: %llu", val);
      }
      delay_us(1000);
    }
  }
}

static int module_init() {
  print_info(true, "module init");

  return 0;
}

int main(int argc, char **argv) {
  module_init();

  Gui gui;

  std::thread t(threadFunc, &gui.getMonitors());
  t.detach();

  gui.loop();

  return 0;
}
