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
#include "uxn/src/devices/file.h"
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

static volatile bool emu_resized = true;

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

  void eval() {
    Uint8 *vector_addr = &m_u.dev[0x20];
    auto screen_vector = PEEK2(vector_addr);
    uxn_eval(&m_u, screen_vector);
    screen_redraw(&m_u);
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
      ms[0] = {1, 1, 1, 1};
    });

    voo::h2l_image a{dq.physical_device(), 1024, 1024};
    auto smp = vee::create_sampler(vee::nearest_sampler);
    auto dset = ps.allocate_descriptor_set(a.iv(), *smp);

    quack::upc rpc{
        .grid_pos = {0.5f, 0.5f},
        .grid_size = {1.0f, 1.0f},
    };

    while (!interrupted()) {
      voo::swapchain_and_stuff sw{dq};
      extent_loop(dq.queue(), sw, [&] {
        e.eval();

        if (emu_resized) {
          float sw = uxn_screen.width;
          float sh = uxn_screen.height;
          rpc.grid_size = {sw, sh};
          rpc.grid_pos = rpc.grid_size / 2.0;
          ib.map_positions([sw, sh](auto *ps) { ps[0] = {{0, 0}, {sw, sh}}; });
          ib.map_uvs([sw, sh](auto *uvs) {
            float u = sw / 1024.0;
            float v = sh / 1024.0;
            uvs[0] = {{0, 0}, {u, v}};
          });
          emu_resized = false;
        }

        {
          auto w = uxn_screen.width;
          auto h = uxn_screen.height;
          voo::mapmem m{a.host_memory()};
          auto mp = static_cast<quack::u8_rgba *>(*m);
          auto sp = static_cast<quack::u8_rgba *>(
              static_cast<void *>(uxn_screen.pixels));
          for (auto y = 0; y < h; y++) {
            for (auto x = 0; x < w; x++) {
              auto [b, g, r, a] = sp[x];
              mp[x] = {r, g, b, a};
            }
            sp += w;
            mp += 1024;
          }
        }

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
  // Uint8 p = addr & 0x0f;
  Uint8 d = addr & 0xf0;
  switch (d) {
  case 0x00:
    return system_dei(u, addr);
  case 0x20:
    return screen_dei(u, addr);
  case 0x30:
  case 0x40:
  case 0x50:
  case 0x60:
    silog::log(silog::debug, "DEI: %02x", addr);
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
  case 0x40:
  case 0x50:
  case 0x60:
    silog::log(silog::debug, "DEO: %02x %02x", addr, value);
    break;
  case 0xa0:
    file_deo(0, u->ram, &u->dev[d], p);
    break;
  case 0xb0:
    file_deo(1, u->ram, &u->dev[d], p);
    break;
  }
}
extern "C" int emu_resize(int width, int height) {
  silog::log(silog::debug, "resize: %d %d", width, height);
  emu_resized = true;
  return 0;
}

extern "C" void casein_handle(const casein::event &e) {
  static thread t{};
  t.handle(e);
}
