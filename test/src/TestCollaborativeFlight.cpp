// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "CollaborativeFlight/Core.hpp"
#include "CollaborativeFlight/Settings.hpp"
#include "TestUtil.hpp"

#include <boost/json.hpp>
#include <cmath>
#include <limits>

using namespace CollaborativeFlight;
using namespace std::chrono_literals;

static constexpr auto EPOCH = Clock::time_point{1750000000s};

template<typename F>
static bool
Throws(F f)
{
  try {
    f();
    return false;
  } catch (const std::exception &) {
    return true;
  }
}

static Sample
Fix(unsigned second, bool circle, double altitude = 1000)
{
  const double phase = second * 12 * M_PI / 180;
  const double lat = circle ? 45 + 0.001 * std::sin(phase) : 45;
  const double lon = circle ? 6 + 0.0014 * std::cos(phase)
    : 6 + second * 0.0002;
  return {EPOCH + std::chrono::seconds{second},
          {Angle::Degrees(lon), Angle::Degrees(lat)}, altitude};
}

static void
TestDetector()
{
  Detector detector;
  for (unsigned i = 0; i < 120; ++i)
    detector.Add(Fix(i, false));
  ok1(detector.Detect(EPOCH + 1000s).empty());
  detector.Reset();

  for (unsigned i = 0; i < 180; ++i)
    detector.Add(Fix(i, true, 1000 + i * 2));
  ok1(detector.Detect(EPOCH + 180s).empty());
  /* Continue straight from the final circle fix. */
  const auto end = Fix(179, true, 1358);
  for (unsigned i = 180; i < 240; ++i) {
    auto p = end;
    p.time = EPOCH + std::chrono::seconds{i};
    p.location.longitude += Angle::Degrees((i - 179) * 0.0002);
    detector.Add(p);
  }
  ok1(detector.Detect(EPOCH + 240s).empty());
  auto boundary = detector;
  const auto climbs = detector.Detect(EPOCH + 600s);
  ok1(climbs.size() == 1);
  if (!climbs.empty()) {
    const auto &c = climbs.front();
    ok1(c.end - c.start > 60s);
    ok1(c.rate > 1.5 && c.rate <= 2.01);
    ok1(std::abs(c.location.latitude.Degrees() - 45) < 0.0002);
    ok1(std::abs(c.location.longitude.Degrees() - 6) < 0.0003);
    ok1(boundary.Detect(c.end + Detector::LOOKAHEAD - 1s).empty());
    ok1(boundary.Detect(c.end + Detector::LOOKAHEAD).size() == 1);
  } else {
    for (unsigned i = 0; i < 6; ++i)
      ok1(false);
  }
  ok1(detector.Detect(EPOCH + 700s).empty());
  detector.Reset();
  for (unsigned i = 0; i < 180; ++i)
    detector.Add(Fix(i, true, 1000 - i));
  for (unsigned i = 180; i < 240; ++i)
    detector.Add(Fix(i, false));
  ok1(detector.Detect(EPOCH + 700s).empty());

  detector.Reset();
  for (unsigned i = 0; i < 45; ++i)
    detector.Add(Fix(i, true, 1000 + i));
  for (unsigned i = 46; i < 100; ++i)
    detector.Add(Fix(i, false));
  ok1(detector.Detect(EPOCH + 700s).empty());

  detector.Reset();
  for (unsigned i = 0; i < 45; ++i)
    detector.Add(Fix(i, true, 1000 + i));
  for (unsigned i = 80; i < 125; ++i)
    detector.Add(Fix(i, true, 1000 + i));
  for (unsigned i = 126; i < 150; ++i)
    detector.Add(Fix(i, false));
  ok1(detector.Detect(EPOCH + 700s).empty());

  Detector segmented;
  for (unsigned i = 0; i < 180; ++i)
    segmented.Add(Fix(i, true, 1000 + i * 2));
  for (unsigned i = 180; i < 240; ++i) {
    auto p = end;
    p.time = EPOCH + std::chrono::seconds{i};
    p.location.longitude += Angle::Degrees((i - 179) * 0.0002);
    segmented.Add(p);
  }
  segmented.Add(Fix(300, false));
  ok1(segmented.Detect(EPOCH + 700s).size() == 1);

  Detector bounded;
  for (unsigned i = 0; i < Detector::MAX_SAMPLES + 200; ++i)
    bounded.Add(Fix(i, true, 1000 + i * 0.5));
  const unsigned exit = Detector::MAX_SAMPLES + 199;
  const auto final = Fix(exit, true, 1000 + exit * 0.5);
  for (unsigned i = exit + 1; i < exit + 120; ++i) {
    auto p = final;
    p.time = EPOCH + std::chrono::seconds{i};
    p.location.longitude += Angle::Degrees((i - exit) * 0.0002);
    bounded.Add(p);
  }
  ok1(bounded.Detect(EPOCH + 8000s).empty());

  auto invalid = Fix(900, false);
  invalid.altitude = std::numeric_limits<double>::quiet_NaN();
  ok1(Throws([&]{ detector.Add(invalid); }));
}

static Climb
ExampleClimb()
{
  return {EPOCH, EPOCH + 120s,
          {Angle::Degrees(6), Angle::Degrees(45)}, 1000, 1240, 2};
}

static void
TestQueueAndProtocol()
{
  Queue queue;
  queue.client = "test-client";
  queue.Add(ExampleClimb());
  queue.Add(ExampleClimb());
  ok1(queue.climbs.size() == 1);
  const auto serialized = queue.Serialize();
  auto restored = Queue::Parse(serialized);
  ok1(restored.client == queue.client);
  ok1(restored.climbs.size() == 1);
  ok1(restored.climbs.front().climb.start == queue.climbs.front().climb.start);
  ok1(restored.climbs.front().climb.location.Distance(
        queue.climbs.front().climb.location) < 0.001);
  ok1(restored.climbs.front().climb.rate == 2);
  const std::string token(32, 's');
  const auto batch = MakeBatch(restored, "folder-id", token, EPOCH + 600s);
  ok1(batch.climb_ids.size() == 1);
  const auto body = boost::json::parse(batch.body).as_object();
  ok1(body.at("token").as_string() == token);
  ok1(body.at("folder_id").as_string() == "folder-id");
  ok1(body.at("climbs").as_array().front().as_object().at("rate") == 2.);
  ok1(!body.contains("track"));
  const auto ack = "{\"ok\":true,\"batch_id\":\"" + batch.id +
    "\",\"accepted\":1}";
  ok1(!Throws([&]{ ValidateAcknowledgement(ack, batch); }));
  ok1(Throws([&]{ ValidateAcknowledgement("{}", batch); }));
  ok1(Throws([&]{ ValidateAcknowledgement(
    R"({"ok":true,"batch_id":"wrong","accepted":1})", batch); }));
  ok1(Throws([&]{ ValidateAcknowledgement(
    R"({"ok":false,"batch_id":"wrong","accepted":1})", batch); }));
  ok1(Throws([&]{ ValidateAcknowledgement("<html>Sign in</html>", batch); }));

  restored.Acknowledge({"unrelated"});
  ok1(!restored.climbs.front().delivered);
  restored.Acknowledge(batch.climb_ids);
  ok1(restored.climbs.front().delivered);
  ok1(MakeBatch(restored, "folder-id", token, EPOCH + 700s).climb_ids.empty());
  ok1(Queue::Parse(restored.Serialize()).climbs.front().delivered);
  ok1(restored.Prune(EPOCH + 120s + 24h) == 0);
  ok1(restored.Prune(EPOCH + 121s + 24h) == 1);
  ok1(restored.climbs.empty());
  queue.Acknowledge({});
  ok1(!queue.climbs.front().delivered);
  ok1(MakeBatch(queue, "folder-id", token, EPOCH + 121s + 24h).
      climb_ids.empty());
  ok1(Throws([]{ Queue::Parse("not json"); }));
  ok1(Throws([]{
    Queue::Parse(R"({"version":2,"client":"x","climbs":[]})");
  }));
  ok1(Throws([]{
    Queue::Parse(R"({"version":1,"client":"../x","climbs":[]})");
  }));
  ok1(Throws([&] {
    Queue::Parse(
      R"({"version":1,"client":"x","climbs":[{"start":1750000000,)"
      R"("end":1750000120,"lat":45,"lon":6,"base":1000,"top":1240,)"
      R"("rate":3,"delivered":false}]})");
  }));
  ok1(Throws([] {
    Queue::Parse(
      R"({"version":1,"client":"x","climbs":[{"start":9223372036854775000,)"
      R"("end":9223372036854775120,"lat":45,"lon":6,"base":1000,"top":1240,)"
      R"("rate":2,"delivered":false}]})");
  }));
  ok1(Throws([&]{ MakeBatch(queue, "folder-id", "short", EPOCH); }));
  for (unsigned i = 1; i < Queue::MAX_CLIMBS; ++i) {
    auto c = ExampleClimb();
    c.start += std::chrono::seconds{i};
    c.end += std::chrono::seconds{i};
    queue.Add(c);
  }
  ok1(queue.climbs.size() == Queue::MAX_CLIMBS);
  ok1(MakeBatch(queue, "folder-id", token, EPOCH + 1000s).
      climb_ids.size() == 64);
  auto extra = ExampleClimb();
  extra.start += 1h;
  extra.end += 1h;
  ok1(Throws([&]{ queue.Add(extra); }));
  ok1(Queue::Parse(queue.Serialize()).climbs.size() == Queue::MAX_CLIMBS);
}

static void
TestDisplaySettings()
{
  CollaborativeFlightSettings collaborative;
  RecentClimbsSettings recent;
  ok1(!collaborative.GetDisplaySettings().enabled);
  ok1(collaborative.display.interval == 3min);
  ok1(collaborative.display.max_age == 60);
  ok1(collaborative.display.min_climb_rate == 0);
  ok1(collaborative.display.url.empty());

  collaborative.enabled = true;
  ok1(collaborative.GetDisplaySettings().enabled && !recent.enabled);
  collaborative.display.url = "https://example.com/group.cup";
  collaborative.display.max_age = 120;
  collaborative.display.min_climb_rate = 1.5;
  collaborative.display.interval = 5min;
  const auto display = collaborative.GetDisplaySettings();
  ok1(display.url == collaborative.display.url);
  ok1(display.max_age == 120 && display.min_climb_rate == 1.5);
  ok1(display.interval == 5min && collaborative.interval == 3min);
  ok1(recent.url.empty() && recent.max_age == 60 &&
      recent.min_climb_rate == 0);

  auto other = collaborative;
  other.display.max_age = 240;
  ok1(other != collaborative);
  ok1(other.HasSameUploadConfiguration(collaborative));
  other.token = "different";
  ok1(!other.HasSameUploadConfiguration(collaborative));
  collaborative.enabled = false;
  recent.enabled = true;
  ok1(!collaborative.GetDisplaySettings().enabled && recent.enabled);
  ok1(collaborative.display.url == display.url);
  collaborative.SetDefaults();
  ok1(!collaborative.GetDisplaySettings().enabled &&
      collaborative.display.url.empty() && collaborative.display.max_age == 60);
}

static void
TestSettings()
{
  CollaborativeFlightSettings settings;
  settings.SetDefaults();
  ok1(!settings.enabled);
  ok1(!settings.debug);
  ok1(settings.interval == 3min);
  ok1(settings.folder_url.empty() && settings.upload_url.empty());
  ok1(CollaborativeFlightSettings::ClampInterval(0s) == 1min);
  ok1(CollaborativeFlightSettings::ClampInterval(120min) == 60min);
  ok1(FolderId("https://drive.google.com/drive/folders/abc-DEF_123?usp=sharing")
      == "abc-DEF_123");
  ok1(Throws([]{ FolderId("http://drive.google.com/drive/folders/abc"); }));
  ok1(Throws([]{ FolderId("https://drive.google.com/drive/folders/../../"); }));
  ok1(Throws([]{ FolderId("https://evil.test/drive/folders/abc"); }));
  ok1(!Throws([]{
    ValidateUploadUrl("https://script.google.com/macros/s/abc-DEF_123/exec");
  }));
  ok1(Throws([]{
    ValidateUploadUrl("http://script.google.com/macros/s/abc/exec");
  }));
  ok1(Throws([]{
    ValidateUploadUrl("https://script.google.com/macros/s/abc/exec?token=x");
  }));
  ok1(Throws([]{
    ValidateUploadUrl("https://script.google.com.evil.test/macros/s/abc/exec");
  }));
}

static void
TestPublicIdentity()
{
  ok1(DefaultPublicIdentity("Pilot", "F-AB") == "Pilot F-AB");
  ok1(DefaultPublicIdentity("Long pilot name", "F-AB") == "Long pilot");
  ok1(DefaultPublicIdentity("", "F-AB") == "F-AB");
  ok1(DefaultPublicIdentity("", "") == "CF");
  const std::string unicode =
    "\xc3\x89\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
    "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
  ok1(DefaultPublicIdentity(unicode + "X", "") == unicode);
  ok1(!Throws([&]{ ValidatePublicIdentity(unicode); }));
  ok1(Throws([&]{ ValidatePublicIdentity(unicode + "X"); }));
  ok1(Throws([]{ ValidatePublicIdentity(""); }));
  ok1(Throws([]{ ValidatePublicIdentity("   "); }));
  ok1(Throws([]{ ValidatePublicIdentity("A\nB"); }));
  ok1(Throws([]{ ValidatePublicIdentity("\xc3"); }));
  ok1(Throws([]{ ValidatePublicIdentity("12345678901"); }));
  ok1(!Throws([]{ ValidatePublicIdentity("A,\"B"); }));
  const std::string supplementary =
    "\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80"
    "\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80"
    "\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80\xf0\x9f\x9a\x80"
    "\xf0\x9f\x9a\x80";
  ok1(!Throws([&]{ ValidatePublicIdentity(supplementary); }));
  ok1(DefaultPublicIdentity(supplementary + "X", "") == supplementary);
  Queue queue;
  queue.client = "identity-test";
  queue.Add(ExampleClimb(), unicode);
  auto restored = Queue::Parse(queue.Serialize());
  ok1(restored.climbs.front().public_identity == unicode);
  const auto batch = MakeBatch(restored, "folder-id", std::string(32, 's'),
                               EPOCH + 600s);
  auto body = boost::json::parse(batch.body).as_object();
  ok1(body.at("climbs").as_array().front().as_object().
      at("public_identity").as_string() == unicode);
  ok1(batch.climb_ids.front() == ExampleClimb().Id(queue.client));
  queue.Add(ExampleClimb(), "Changed");
  ok1(queue.climbs.front().public_identity == unicode);
  ok1(Throws([&]{ queue.Add(ExampleClimb(), "12345678901"); }));
  Queue legacy;
  legacy.client = queue.client;
  legacy.Add(ExampleClimb());
  ok1(Queue::Parse(legacy.Serialize()).climbs.front().public_identity.empty());
  auto legacyBody = boost::json::parse(
    MakeBatch(legacy, "folder-id", std::string(32, 's'),
              EPOCH + 600s).body).as_object();
  ok1(legacyBody.at("climbs").as_array().front().as_object().
      at("public_identity").as_string() == "CF");
  CollaborativeFlightSettings settings;
  auto other = settings;
  other.public_identity = "Changed";
  ok1(!settings.HasSameUploadConfiguration(other));
}

int
main()
{
  plan_tests(66 + 16 + 23);
  TestDetector();
  TestQueueAndProtocol();
  TestSettings();
  TestDisplaySettings();
  TestPublicIdentity();
  return exit_status();
}
