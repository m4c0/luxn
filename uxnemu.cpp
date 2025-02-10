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
#include "uxn/src/devices/mouse.h"
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
import vapp;
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

  void mouse_pos(int x, int y) {
    if (x < 0)
      x = 0;
    if (x > uxn_screen.width)
      x = uxn_screen.width;
    if (y < 0)
      y = 0;
    if (y > uxn_screen.height)
      y = uxn_screen.height;
    ::mouse_pos(&m_u, &m_u.dev[0x90], x, y);
  }
  void mouse_down(int b) { ::mouse_down(&m_u, &m_u.dev[0x90], b + 1); }
  void mouse_up(int b) { ::mouse_up(&m_u, &m_u.dev[0x90], b + 1); }
};
static emu g_e{};

class thread : public vapp {
  thread() {
    using namespace casein;
    handle(MOUSE_MOVE, [] { g_e.mouse_pos(casein::mouse_pos.x, casein::mouse_pos.y); });
    handle(MOUSE_UP,   M_LEFT,  [] { g_e.mouse_up(0); });
    handle(MOUSE_UP,   M_RIGHT, [] { g_e.mouse_up(1); });
    handle(MOUSE_DOWN, M_LEFT,  [] { g_e.mouse_down(0); });
    handle(MOUSE_DOWN, M_RIGHT, [] { g_e.mouse_down(1); });
  }

  void run() override {
    quack::instance inst {
      .position = {0, 0},
      .size = {1, 1},
      .uv0 = {0, 0},
      .uv1 = {1, 1},
      .colour = {0, 0, 0, 1},
      .multiplier = {1, 1, 1, 1},
    };

    voo::device_and_queue dq{"uxnemu", casein::native_ptr};
    quack::pipeline_stuff ps{dq, 1};
    quack::buffer_updater ib{&dq, 1, [&](auto p) { *p = inst; }};

    voo::h2l_image a{dq.physical_device(), 1024, 1024, VK_FORMAT_R8G8B8A8_SRGB};
    auto smp = vee::create_sampler(vee::nearest_sampler);
    auto dset = ps.allocate_descriptor_set(a.iv(), *smp);

    quack::upc rpc{
        .grid_pos = {0.5f, 0.5f},
        .grid_size = {1.0f, 1.0f},
    };

    while (!interrupted()) {
      voo::swapchain_and_stuff sw{dq};
      extent_loop(dq.queue(), sw, [&] {
        g_e.eval();

        if (emu_resized) {
          float sw = uxn_screen.width;
          float sh = uxn_screen.height;
          rpc.grid_size = {sw, sh};
          rpc.grid_pos = rpc.grid_size / 2.0;
          inst.size = { sw, sh };
          inst.uv1 = { sw / 1024.0f, sh / 1024.0f };
          ib.run_once();
          emu_resized = false;
        }

        {
          struct rgba { char r; char g; char b; char a; };
          auto w = uxn_screen.width;
          auto h = uxn_screen.height;
          voo::mapmem m{a.host_memory()};
          auto mp = static_cast<rgba *>(*m);
          auto sp = static_cast<rgba *>(static_cast<void *>(uxn_screen.pixels));
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

          auto scb = sw.cmd_render_pass({ *pcb });
          quack::run(&ps, {
            .sw = &sw,
            .scb = *scb,
            .pc = &upc,
            .inst_buffer = ib.data().local_buffer(),
            .atlas_dset = dset,
            .count = 1,
          });
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
