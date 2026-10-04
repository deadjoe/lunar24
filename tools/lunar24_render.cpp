// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
//
// lunar24_render — render the Lunar 24 engine offline to a WAV file, so the sound can be
// judged by ear without the app. Examples:
//
//   lunar24_render --seconds 30 --out drone.wav
//   lunar24_render --program-l "Shimmer" --program-r "Space reverb" --set effector.z=0.8
//   lunar24_render --note 0:0:4 --note 4:7:4 --out notes.wav      # time:semitone:length (s)
//   lunar24_render --cable lfo_a.cv_out=vcf.cv_l_in --set vcf.l_mod=0.6
//   lunar24_render --set keyboard.mode=2 --step 2:7 --step 3:12:0 --note 0:0:8   # 16-step sequencer
//   lunar24_render --drone 1 --set drone_1.mod_1=1 --shade 4:1:1 --shade 8:1:0   # hand on the eye
//   lunar24_render --drone 1 --at 2:drone_1.tune_1=0.6 --at 4:drone_1.tune_1=0.4   # turn a knob
//   lunar24_render --list params|jacks|programs

#include <host/standalone_audio_engine.h>
#include <lunar24/core/state_default.h>
#include <lunar24/core/state_edit.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace lunar24;

namespace {

struct Note {
  double at = 0.0, semis = 0.0, length = 1.0;
};
struct KnobMove {  // a panel knob set to `value` at `at` seconds
  double at = 0.0;
  core::ParameterId id{};
  double value = 0.0;
};
struct Shade {  // the hand over a classic drone's photo sensor from `at` on
  double at = 0.0;
  int group = 0;  // 0..3 = drone 1/2/4/5
  double value = 0.0;
};

void usage() {
  std::puts(
      "usage: lunar24_render [options]\n"
      "  --seconds N          length (default 20)\n"
      "  --out FILE           WET L/R output WAV (default lunar24.wav)\n"
      "  --dry FILE           also write DRY A/B to a WAV\n"
      "  --sr HZ              sample rate (default 48000)\n"
      "  --seed N             machine seed (default: the app's startup seed)\n"
      "  --set ID=VALUE       set a panel parameter by stable id (e.g. vcf.l_freq=0.4)\n"
      "  --cable OUT=IN       plug a patch cable (e.g. lfo_a.cv_out=vcf.cv_l_in)\n"
      "  --program-l NAME     left effector program (e.g. \"Shimmer\" or cathedral.1)\n"
      "  --program-r NAME     right effector program\n"
      "  --note T:S:L         play a keyboard note at T seconds, S semitones, L seconds long\n"
      "  --step I:N[:G]       16-step sequencer step I (1-16): note N semitones, gate G (1 = on)\n"
      "  --drone N            open DRONE VOICES key N (1-6) at the start\n"
      "  --at T:ID=VALUE      set a panel parameter at T seconds (a knob turned while playing)\n"
      "  --shade T:D:V        from T seconds, hold a hand over drone D's photo sensor (D = 1, 2, 4, 5;\n"
      "                       V = 0 away .. 1 covering); its MOD buttons decide which generators bend\n"
      "  --list params|jacks|programs");
}

bool writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r,
              int sr) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const std::uint32_t n = static_cast<std::uint32_t>(l.size());
  const std::uint32_t data = n * 4, riff = 36 + data, fmtLen = 16, rate = sr, bytesPerSec = sr * 4;
  const std::uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&bytesPerSec, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int16_t a = static_cast<std::int16_t>(std::lrint(std::fmax(-1.0, std::fmin(1.0, l[i])) * 32767.0));
    const std::int16_t b = static_cast<std::int16_t>(std::lrint(std::fmax(-1.0, std::fmin(1.0, r[i])) * 32767.0));
    std::fwrite(&a, 2, 1, f);
    std::fwrite(&b, 2, 1, f);
  }
  std::fclose(f);
  return true;
}

bool splitAt(const std::string& s, char c, std::string& a, std::string& b) {
  const auto p = s.find(c);
  if (p == std::string::npos) return false;
  a = s.substr(0, p);
  b = s.substr(p + 1);
  return true;
}

core::ControlEvent noteEvent(core::ControlEventKind kind, double value, core::NoteId id) {
  core::ControlEvent e{};
  e.kind = kind;
  e.value = static_cast<core::SignalSample>(value);
  e.source = 7;
  e.noteId = id;
  e.side = core::KeyboardSide::Left;
  return e;
}

void printStats(const char* name, const std::vector<float>& x) {
  double peak = 0.0, ss = 0.0;
  for (float v : x) { peak = std::fmax(peak, std::fabs(v)); ss += double(v) * v; }
  std::printf("  %-6s peak %.3f  rms %.3f (%.1f dBFS)\n", name, peak,
              std::sqrt(ss / std::max<std::size_t>(1, x.size())),
              20.0 * std::log10(std::sqrt(ss / std::max<std::size_t>(1, x.size())) + 1e-12));
}

}  // namespace

int main(int argc, char** argv) {
  double seconds = 20.0, sr = 48000.0;
  std::uint64_t seed = host::kLunarStartupSeed;
  std::string out = "lunar24.wav", dryOut;
  std::vector<std::string> sets, cables, progL, progR, steps;
  std::vector<Note> notes;
  std::vector<Shade> shades;
  std::vector<KnobMove> moves;
  std::vector<int> drones;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& v) {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      v = argv[++i];
    };
    std::string v;
    if (a == "--seconds") { next(v); seconds = std::atof(v.c_str()); }
    else if (a == "--out") next(out);
    else if (a == "--dry") next(dryOut);
    else if (a == "--sr") { next(v); sr = std::atof(v.c_str()); }
    else if (a == "--seed") { next(v); seed = std::strtoull(v.c_str(), nullptr, 0); }
    else if (a == "--set") { next(v); sets.push_back(v); }
    else if (a == "--step") { next(v); steps.push_back(v); }
    else if (a == "--cable") { next(v); cables.push_back(v); }
    else if (a == "--program-l") { next(v); progL.push_back(v); }
    else if (a == "--program-r") { next(v); progR.push_back(v); }
    else if (a == "--note") {
      next(v);
      Note n;
      if (std::sscanf(v.c_str(), "%lf:%lf:%lf", &n.at, &n.semis, &n.length) < 2) { usage(); return 2; }
      notes.push_back(n);
    } else if (a == "--at") {
      next(v);
      KnobMove mv;
      std::string head, assign, id, val;
      if (!splitAt(v, ':', head, assign) || !splitAt(assign, '=', id, val)) { usage(); return 2; }
      const core::ParameterDescriptor* d = core::find_parameter_by_name(id);
      if (d == nullptr) { std::fprintf(stderr, "unknown parameter: %s\n", id.c_str()); return 2; }
      mv.at = std::atof(head.c_str());
      mv.id = d->id;
      mv.value = std::atof(val.c_str());
      moves.push_back(mv);
    } else if (a == "--drone") {
      next(v);
      const int d = std::atoi(v.c_str());
      if (d < 1 || d > 6) { usage(); return 2; }
      drones.push_back(d - 1);
    } else if (a == "--shade") {
      next(v);
      Shade sh;
      int d = 0;
      if (std::sscanf(v.c_str(), "%lf:%d:%lf", &sh.at, &d, &sh.value) != 3) { usage(); return 2; }
      static constexpr int kGroupOfDrone[7] = {-1, 0, 1, -1, 2, 3, -1};
      sh.group = d >= 1 && d <= 6 ? kGroupOfDrone[d] : -1;
      if (sh.group < 0) { std::fprintf(stderr, "--shade: drones 1, 2, 4 and 5 have photo sensors\n"); return 2; }
      shades.push_back(sh);
    } else if (a == "--list") {
      next(v);
      if (v == "params")
        for (const auto& p : registry::kParameters)
          std::printf("%-34s %-8s %g..%g (default %g)\n", std::string(p.stable_id).c_str(),
                      std::string(p.unit).c_str(), p.min, p.max, p.initial);
      else if (v == "jacks")
        for (const auto& j : registry::kJacks)
          std::printf("%-34s %s\n", std::string(j.stable_id).c_str(),
                      j.direction == core::PinDirection::output ? "out" : "in");
      else
        for (const auto& p : registry::kPrograms)
          std::printf("%-26s %-14s %u  %s\n", std::string(p.stable_id).c_str(),
                      std::string(p.cartridge).c_str(), p.slot, std::string(p.name).c_str());
      return 0;
    } else { usage(); return a == "--help" || a == "-h" ? 0 : 2; }
  }

  core::DeviceStateV1 st = core::make_default_device_state(seed);
  for (const auto& s : steps) {  // I:N[:G], I = 1..16
    int i = 0, n = 0, g = 1;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &i, &n, &g) < 2 || i < 1 || i > 16) {
      std::fprintf(stderr, "bad --step %s\n", s.c_str());
      return 1;
    }
    auto& step = st.keyboardSeqCurrent.steps[static_cast<std::size_t>(i - 1)];
    step.note = static_cast<std::uint8_t>(n < 0 ? 0 : n);
    step.gate = g != 0 ? 1 : 0;
    st.keyboardSeqCurrentR.steps[static_cast<std::size_t>(i - 1)] = step;
  }
  for (const auto& s : sets) {
    std::string id, val;
    const core::ParameterDescriptor* d = splitAt(s, '=', id, val) ? core::find_parameter_by_name(id) : nullptr;
    if (d == nullptr) { std::fprintf(stderr, "unknown parameter: %s\n", s.c_str()); return 2; }
    core::state_set_param(st, d->id, std::atof(val.c_str()));
  }
  for (const auto& c : cables) {
    std::string src, dst;
    const core::JackDescriptor* s = splitAt(c, '=', src, dst) ? core::find_jack_by_name(src) : nullptr;
    const core::JackDescriptor* k = s ? core::find_jack_by_name(dst) : nullptr;
    if (k == nullptr || !core::state_connect(st, s->id, k->id)) {
      std::fprintf(stderr, "bad cable: %s\n", c.c_str());
      return 2;
    }
  }
  auto pickProgram = [&](const std::vector<std::string>& names, core::EffectorSelection& sel,
                         core::ParameterId selectParam) {
    if (names.empty()) return true;
    const core::ProgramDescriptor* p = core::find_program_by_name(names.back());
    if (p == nullptr) { std::fprintf(stderr, "unknown program: %s\n", names.back().c_str()); return false; }
    sel.program = p->id;                                    // inserts the cartridge
    core::state_set_param(st, selectParam, p->slot - 1.0);  // and flips the 1-2-3 switch
    return true;
  };
  if (!pickProgram(progL, st.leftEffector, core::ParameterId::effector_select_l)) return 2;
  if (!pickProgram(progR, st.rightEffector, core::ParameterId::effector_select_r)) return 2;

  const int block = 256;
  host::StandaloneAudioEngine engine;
  if (!engine.prepare(seed, sr, block, 2, 4)) { std::fprintf(stderr, "prepare failed\n"); return 1; }
  const auto status = engine.applyDeviceState(st, sr, block, 2, 4);
  if (status != host::StandaloneAudioEngine::StateApplyStatus::Accepted) {
    std::fprintf(stderr, "the machine state was rejected (status %d)\n", static_cast<int>(status));
    return 1;
  }

  const std::size_t total = static_cast<std::size_t>(seconds * sr);
  std::vector<double> in0(block, 0.0), in1(block, 0.0), o[4];
  for (auto& b : o) b.assign(block, 0.0);
  const double* ins[2] = {in0.data(), in1.data()};
  double* outs[4] = {o[0].data(), o[1].data(), o[2].data(), o[3].data()};
  std::vector<float> wl, wr, da, db;
  wl.reserve(total); wr.reserve(total); da.reserve(total); db.reserve(total);

  for (int d : drones) engine.postDroneKey(d, true);
  std::vector<bool> on(notes.size(), false), off(notes.size(), false), shaded(shades.size(), false),
      moved(moves.size(), false);
  for (std::size_t pos = 0; pos < total; pos += block) {
    const double t = pos / sr;
    for (std::size_t n = 0; n < notes.size(); ++n) {
      const auto id = static_cast<core::NoteId>(n + 1);
      if (!on[n] && t >= notes[n].at) {
        on[n] = true;
        engine.postEvent(noteEvent(core::ControlEventKind::pitch, notes[n].semis / 12.0, id));
        engine.postEvent(noteEvent(core::ControlEventKind::pressure, 0.8, id));
        engine.postEvent(noteEvent(core::ControlEventKind::gate_on, 1.0, id));
      }
      if (on[n] && !off[n] && t >= notes[n].at + notes[n].length) {
        off[n] = true;
        engine.postEvent(noteEvent(core::ControlEventKind::gate_off, 0.0, id));
      }
    }
    for (std::size_t k = 0; k < moves.size(); ++k)
      if (!moved[k] && t >= moves[k].at) {
        moved[k] = true;
        engine.postParameter(moves[k].id, moves[k].value);
      }
    for (std::size_t k = 0; k < shades.size(); ++k)
      if (!shaded[k] && t >= shades[k].at) {
        shaded[k] = true;
        engine.postPhotoShade(shades[k].group, shades[k].value);
      }
    engine.processBlock(ins, outs, 2, 4, block);
    for (int i = 0; i < block && pos + i < total; ++i) {
      wl.push_back(static_cast<float>(o[0][i]));
      wr.push_back(static_cast<float>(o[1][i]));
      da.push_back(static_cast<float>(o[2][i]));
      db.push_back(static_cast<float>(o[3][i]));
    }
  }

  if (!writeWav(out, wl, wr, static_cast<int>(sr))) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
  std::printf("wrote %s (%.1f s)\n", out.c_str(), seconds);
  printStats("WET L", wl);
  printStats("WET R", wr);
  if (!dryOut.empty() && writeWav(dryOut, da, db, static_cast<int>(sr))) {
    std::printf("wrote %s\n", dryOut.c_str());
    printStats("DRY A", da);
    printStats("DRY B", db);
  }
  if (engine.nonFiniteSamples() != 0)
    std::printf("  WARNING: %llu non-finite samples\n",
                static_cast<unsigned long long>(engine.nonFiniteSamples()));
  return 0;
}
