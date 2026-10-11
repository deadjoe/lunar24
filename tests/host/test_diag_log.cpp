// Copyright (c) 2026 Lunar 24 contributors
// SPDX-License-Identifier: Apache-2.0
#include "mini_test.h"
#include <host/diag_log.h>

#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace lunar24::host::diag;
namespace fs = std::filesystem;

static std::string read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static std::string readAll(const fs::path& dir) {
  std::vector<fs::path> paths;
  for (const auto& e : fs::directory_iterator(dir)) paths.push_back(e.path());
  std::sort(paths.begin(), paths.end());
  std::string all;
  for (const auto& p : paths) all += read(p);
  return all;
}
static std::size_t count(const std::string& text, const std::string& what) {
  std::size_t n = 0;
  for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}
static fs::path freshDir(const char* tag) {
  // A non-ASCII folder, like a Chinese user name on Windows.
  const auto dir = fs::temp_directory_path() /
      fs::u8path(std::string(u8"lunar-diag-日志-") + tag + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  return dir;
}

int main() {
  // ---- log.conf ----
  {
    std::vector<std::string> problems;
    const Config c = parseConfig("# x\nmidi = trace\nlevel = info\nsegment_kib = 128\ntotal_mib=8\nbogus = 1\nui = loud\n",
                                 &problems);
    CHECK(c.threshold[static_cast<int>(Cat::midi)] == Level::trace);  // overrides 'level' wherever it is
    CHECK(c.threshold[static_cast<int>(Cat::ui)] == Level::info);
    CHECK(c.threshold[static_cast<int>(Cat::audio)] == Level::info);
    CHECK_EQ(c.segmentBytes, std::uint64_t(128) << 10);
    CHECK_EQ(c.totalBytes, std::uint64_t(8) << 20);
    CHECK_EQ(problems.size(), std::size_t(2));
    const Config d = parseConfig(kConfigTemplate, &problems);
    CHECK_EQ(problems.size(), std::size_t(2));  // the template itself parses cleanly
    CHECK(d.threshold[0] == Level::debug);
    CHECK_EQ(parseConfig("segment_kib = 1\n").segmentBytes, std::uint64_t(64) << 10);  // clamped
  }

  // ---- file names ----
  {
    files::Name n;
    CHECK(files::parseName("app-20261011-155440-p4242-001.log", &n));
    CHECK_EQ(n.kind, std::string("app"));
    CHECK_EQ(n.session, std::string("app-20261011-155440-p4242"));
    CHECK_EQ(n.part, 1);
    CHECK(files::parseName("vst3-20261011-155440-p1-012.log", &n) && n.part == 12);
    CHECK(!files::parseName("audio.log", &n));
    CHECK(!files::parseName("app-2026-155440-p1-001.log", &n));
    CHECK(!files::parseName("notes.txt", &n));
  }

  // ---- a session: threads, header, config file, end line ----
  {
    const auto dir = freshDir("session");
    auto logger = std::make_unique<Logger>();
    logger->attach();
    CHECK(logger->write(Cat::app, Level::info, "queued before the folder is known"));
    CHECK(logger->open(dir.u8string(), "app", "Lunar 24 1.0.0 test build"));
    CHECK(!logger->open(dir.u8string(), "app", "second open is ignored"));
    CHECK(fs::exists(dir / "log.conf"));  // the template, so the settings are discoverable
    CHECK(!logger->write(Cat::midi, Level::trace, "below the default level"));
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
      threads.emplace_back([&, t] {
        for (int i = 0; i < 500; ++i) logger->writef(Cat::ui, Level::debug, std::uint16_t(t + 1), "thread %d line %d", t, i);
      });
    for (auto& t : threads) t.join();
    CHECK(logger->write(Cat::state, Level::info, "snapshot begin\nparam a=1\nsnapshot end"));
    CHECK(logger->flush());
    const std::string file = logger->currentFile();
    CHECK(!file.empty());
    logger->detach();
    CHECK(!logger->write(Cat::app, Level::info, "after the last detach"));
    const std::string text = readAll(dir / "logs");
    CHECK(text.find("queued before the folder is known") != std::string::npos);
    CHECK(text.find("Lunar 24 1.0.0 test build") != std::string::npos);
    CHECK(text.find("log.conf created with defaults") != std::string::npos);
    CHECK(text.find("second open") == std::string::npos);
    CHECK(text.find("below the default level") == std::string::npos);
    CHECK_EQ(count(text, " D ui     #"), std::size_t(2000));  // nothing lost under contention
    CHECK(text.find("thread 3 line 499") != std::string::npos);
    CHECK(text.find("    | param a=1\n") != std::string::npos);  // continuation lines stay with the record
    CHECK(text.find("session end (clean shutdown)") != std::string::npos);
    CHECK(text.find("after the last detach") == std::string::npos);

    // The next session reports how the previous one ended, and reads the edited log.conf.
    { std::ofstream conf(dir / "log.conf"); conf << "level = info\nnot a setting\n"; }
    logger->attach();
    CHECK(logger->open(dir.u8string(), "app", ""));
    CHECK(!logger->write(Cat::ui, Level::debug, "debug is off now"));
    CHECK(logger->write(Cat::ui, Level::info, "second session"));
    CHECK(logger->flush());
    const std::string second = read(fs::u8path(logger->currentFile()));
    logger->detach();
    CHECK(second.find("previous app session ended cleanly") != std::string::npos);
    CHECK(second.find("log.conf problem (ignored): no '='") != std::string::npos);
    CHECK(readAll(dir / "logs").find("debug is off now") == std::string::npos);
    fs::remove_all(dir);
  }

  // ---- a crashed session is reported ----
  {
    const auto dir = freshDir("crash");
    fs::create_directories(dir / "logs");
    { std::ofstream f(dir / "logs" / "app-20200101-000000-p7-001.log"); f << "last words, no end line\n"; }
    Logger logger;
    logger.attach();
    CHECK(logger.open(dir.u8string(), "app", ""));
    CHECK(logger.flush());
    const std::string text = read(fs::u8path(logger.currentFile()));
    logger.detach();
    CHECK(text.find("session has no end line") != std::string::npos);
    CHECK(text.find("app-20200101-000000-p7-001.log") != std::string::npos);
    fs::remove_all(dir);
  }

  // ---- a full queue drops and counts, never blocks ----
  {
    const auto dir = freshDir("overflow");
    Logger logger(4096);
    logger.attach();  // no folder yet: nothing drains
    int accepted = 0, refused = 0;
    for (int i = 0; i < 200; ++i) (logger.write(Cat::ui, Level::info, std::string(100, 'x')) ? accepted : refused)++;
    CHECK(accepted > 0 && refused > 0);
    CHECK(!logger.write(Cat::ui, Level::info, std::string(Logger::kMaxRecordBytes + 1, 'y')));
    CHECK(logger.open(dir.u8string(), "app", ""));
    CHECK(logger.flush());
    const std::string text = read(fs::u8path(logger.currentFile()));
    logger.detach();
    CHECK(text.find(std::to_string(refused + 1) + " log records dropped") != std::string::npos);
    CHECK_EQ(count(text, std::string(100, 'x')), std::size_t(accepted));
    fs::remove_all(dir);
  }

  // ---- parts roll over whole records; the folder stays within its budget ----
  {
    const auto dir = freshDir("rotate");
    { std::ofstream conf(dir / "log.conf"); conf << "segment_kib = 64\ntotal_mib = 4\n"; }
    fs::create_directories(dir / "logs");
    // Old sessions from earlier runs, 1 MiB each, older than the live-session grace.
    for (int i = 0; i < 5; ++i) {
      const auto p = dir / "logs" / ("app-2020010" + std::to_string(i + 1) + "-000000-p1-001.log");
      { std::ofstream f(p, std::ios::binary); f << std::string(1u << 20, 'o'); }
      fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(24 * (10 - i)));
    }
    { std::ofstream f(dir / "logs" / "keep-me.txt"); f << "not ours"; }
    Logger logger;
    logger.attach();
    CHECK(logger.open(dir.u8string(), "app", "header in every part"));
    const std::string record = std::string(1000, 'r');
    for (int i = 0; i < 300; ++i) {
      logger.write(Cat::ui, Level::info, record);
      if (i % 50 == 0) logger.flush();
    }
    CHECK(logger.flush());
    logger.detach();
    std::uint64_t total = 0;
    int parts = 0;
    bool oldest = false;
    for (const auto& e : fs::directory_iterator(dir / "logs")) {
      const std::string name = e.path().filename().string();
      if (name.rfind("app-2020", 0) != 0 && name.find(".log") != std::string::npos) {
        ++parts;
        const std::string part = read(e.path());
        CHECK(part.size() <= (64u << 10) + 2048);
        CHECK(part.find("header in every part") != std::string::npos);
        CHECK(part.back() == '\n');
      }
      if (name == "app-20200101-000000-p1-001.log") oldest = true;
      total += fs::file_size(e.path());
    }
    CHECK(parts >= 5);
    CHECK(total <= (4u << 20) + 4096);
    CHECK(!oldest);  // the oldest session went first
    CHECK(fs::exists(dir / "logs" / "keep-me.txt"));
    CHECK_EQ(count(readAll(dir / "logs"), record), std::size_t(300));
    fs::remove_all(dir);
  }

  // ---- an unwritable folder is a counted loss, not a crash ----
  {
    const auto dir = freshDir("blocked");
    { std::ofstream f(dir / "logs"); f << "a file where the folder should be"; }
    Logger logger;
    logger.attach();
    CHECK(logger.open(dir.u8string(), "app", ""));
    CHECK(logger.write(Cat::app, Level::error, "cannot land"));
    CHECK(logger.flush());
    CHECK(logger.currentFile().empty());
    CHECK(logger.stats().failed >= 1);
    logger.detach();
    fs::remove_all(dir);
  }
  return test::finish("test_diag_log");
}
