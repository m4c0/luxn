#pragma leco app
#pragma leco add_impl "uxn/src/uxn.c"
#pragma leco add_impl "uxn/src/devices/system.c"
#pragma leco add_impl "uxn/src/devices/console.c"
#pragma leco add_impl "uxn/src/devices/file.c"
#pragma leco add_impl "uxn/src/devices/datetime.c"
#pragma leco add_impl "uxn/src/devices/mouse.c"
#pragma leco add_impl "uxn/src/devices/controller.c"
#pragma leco add_impl "uxn/src/devices/screen.c"
#pragma leco add_impl "uxn/src/devices/audio.c"
#pragma leco add_resource "uxn/boot.rom"

#include <string.h>

extern "C" {
#include "uxn/src/uxn.h"

#include "uxn/src/devices/console.h"
#include "uxn/src/devices/datetime.h"
#include "uxn/src/devices/screen.h"
#include "uxn/src/devices/system.h"
}

#define WIDTH 64 * 8
#define HEIGHT 40 * 8

import casein;
import hai;
import quack;
import silog;
import sires;
import vee;
import voo;

class emu {
  Uint8 m_dev[0x100]{};
  Uxn m_u{};
  hai::array<Uint8> m_ram{0x10000 * RAM_PAGES};

public:
  emu() {
    m_u.dev = m_dev;
    m_u.ram = m_ram.begin();

    // TODO: "slurp" into RAM directly
    sires::slurp("boot.rom")
        .map([this](auto &&buf) {
          unsigned sz = m_ram.size() - PAGE_PROGRAM;
          sz = sz < buf.size() ? sz : buf.size();
          memcpy(&m_ram[PAGE_PROGRAM], buf.begin(), sz);
        })
        .take([](auto msg) {
          silog::log(silog::error, "Failed to read [boot.rom]");
          throw 0;
        });

    screen_resize(WIDTH, HEIGHT);

    if (!uxn_eval(&m_u, PAGE_PROGRAM)) {
      silog::log(silog::error, "uxn_eval failed for main program");
      throw 0;
    }
  }
};

class thread : public voo::casein_thread {
  void run() override {
    emu e{};

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

    { voo::mapmem m{a.host_memory()}; }

    quack::upc rpc{
        .grid_pos = {0.5f, 0.5f},
        .grid_size = {1.0f, 1.0f},
    };

    while (!interrupted()) {
      voo::swapchain_and_stuff sw{dq};
      extent_loop(dq.queue(), sw, [&] {
        auto upc = quack::adjust_aspect(rpc, sw.aspect());
        sw.queue_one_time_submit(dq.queue(), [&](auto pcb) {
          a.setup_copy(*pcb);
          ib.setup_copy(*pcb);

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

extern "C" Uint8 emu_dei(Uxn *u, Uint8 addr) {
  Uint8 p = addr & 0x0f, d = addr & 0xf0;
  switch (d) {
  case 0x00:
    return system_dei(u, addr);
  case 0x20:
    return screen_dei(u, addr);
  case 0x30:
    // return audio_dei(0, &u->dev[d], p);
  case 0x40:
    // return audio_dei(1, &u->dev[d], p);
  case 0x50:
    // return audio_dei(2, &u->dev[d], p);
  case 0x60:
    // return audio_dei(3, &u->dev[d], p);
    return 0;
  case 0xc0:
    return datetime_dei(u, addr);
  }
  return u->dev[addr];
}
extern "C" void emu_deo(Uxn *u, Uint8 addr, Uint8 value) {
  Uint8 p = addr & 0x0f, d = addr & 0xf0;
  u->dev[addr] = value;
  switch (d) {
  case 0x00:
    system_deo(u, &u->dev[d], p);
    if (p > 0x7 && p < 0xe)
      screen_palette(&u->dev[0x8]);
    break;
  case 0x10:
    console_deo(&u->dev[d], p);
    break;
  case 0x20:
    screen_deo(u->ram, &u->dev[0x20], p);
    break;
  case 0x30:
    // audio_deo(0, &u->dev[d], p, u);
    break;
  case 0x40:
    // audio_deo(1, &u->dev[d], p, u);
    break;
  case 0x50:
    // audio_deo(2, &u->dev[d], p, u);
    break;
  case 0x60:
    // audio_deo(3, &u->dev[d], p, u);
    break;
  case 0xa0:
    // file_deo(0, u->ram, &u->dev[d], p);
    break;
  case 0xb0:
    // file_deo(1, u->ram, &u->dev[d], p);
    break;
  }
}
extern "C" int emu_resize(int width, int height) {
  silog::log(silog::debug, "resize: %d %d", width, height);
  return 0;
}

extern "C" void casein_handle(const casein::event &e) {
  static thread t{};
  t.handle(e);
}
