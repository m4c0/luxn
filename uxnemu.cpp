#pragma leco tool
#pragma leco add_impl "uxn/src/uxn.c"
#pragma leco add_impl "uxn/src/devices/system.c"
#pragma leco add_impl "uxn/src/devices/console.c"
#pragma leco add_impl "uxn/src/devices/file.c"
#pragma leco add_impl "uxn/src/devices/datetime.c"
#pragma leco add_impl "uxn/src/devices/mouse.c"
#pragma leco add_impl "uxn/src/devices/controller.c"
#pragma leco add_impl "uxn/src/devices/screen.c"
#pragma leco add_impl "uxn/src/devices/audio.c"

extern "C" {
#include "uxn/src/uxn.h"
}

import casein;
import quack;
import vee;
import voo;

class thread : public voo::casein_thread {
  void run() override {
    voo::device_and_queue dq{"uxnemu", native_ptr()};
    quack::pipeline_stuff ps{dq, 1};
    quack::instance_batch ib{ps.create_batch(1)};
    ib.map_all([](auto p) {
      auto &[cs, ms, ps, us] = p;
      ps[0] = {{0, 0}, {1, 1}};
      cs[0] = {0, 0, 0, 1};
      us[0] = {{0, 0}, {1, 1}};
      ms[1] = {1, 1, 1, 1};
    });

    voo::h2l_image a{dq.physical_device(), 32, 32};
    auto smp = vee::create_sampler(vee::nearest_sampler);
    auto dset = ps.allocate_descriptor_set(a.iv(), *smp);

    quack::upc rpc{
        .grid_pos = {0.5f, 0.5f},
        .grid_size = {1.0f, 1.0f},
    };

    while (!interrupted()) {
      voo::swapchain_and_stuff sw{dq};
      extent_loop(dq.queue(), sw, [&] {
        auto upc = quack::adjust_aspect(rpc, sw.aspect());
        sw.queue_one_time_submit(dq.queue(), [&](auto pcb) {
          auto scb = sw.cmd_render_pass(pcb);
          vee::cmd_set_viewport(*scb, sw.extent());
          vee::cmd_set_scissor(*scb, sw.extent());
          ib.build_commands(*pcb);
          ps.cmd_bind_descriptor_set(*scb, dset);
          ps.cmd_push_vert_frag_constants(*scb, upc);
          ps.run(*scb, 1);
        });
      });
    }
  }
};

extern "C" Uint8 emu_dei(Uxn *u, Uint8 addr) { return 0; }
extern "C" void emu_deo(Uxn *u, Uint8 addr, Uint8 value) {}
extern "C" int emu_resize(int width, int height) { return 0; }

extern "C" void casein_handle(const casein::event &e) {
  static thread t{};
  t.handle(e);
}
