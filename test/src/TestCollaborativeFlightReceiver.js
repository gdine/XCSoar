// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project
// Run: node test/src/TestCollaborativeFlightReceiver.js

if (typeof require === "function") {
  var fs = require("fs");
  eval(fs.readFileSync("tools/collaborative-flight/Receiver.gs", "utf8"));
  var Utilities = {
    DigestAlgorithm: {SHA_256: "sha256"}, Charset: {UTF_8: "utf8"},
    computeDigest: function(algorithm, text, charset) {
      return Array.prototype.slice.call(
        require("crypto").createHash(algorithm).update(text, charset).digest());
    }
  };
}

var count = 0;
function assert(value, message) {
  ++count;
  if (!value)
    throw new Error(message);
}

function rejects(action, message) {
  var failed = false;
  try { action(); } catch (error) { failed = true; }
  assert(failed, message);
}

var now = Math.floor(Date.now() / 1000);
var token = new Array(65).join("a");
var climb = {
  id: "client-123", start: now - 600, end: now - 480,
  lat: 45, lon: 6, base: 1000, top: 1240, rate: 2
};
var request = {
  version: 1, token: token, folder_id: "folder-id",
  batch_id: "client-999", climbs: [climb]
};
function clone(value) { return JSON.parse(JSON.stringify(value)); }

var incoming = validateRequest(request, token, "folder-id", now);
assert(incoming.length === 1, "valid batch");
var wrong = clone(request);
wrong.token = "wrong";
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "wrong token");
wrong = clone(request);
wrong.folder_id = "other-folder";
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "wrong repository");
wrong = clone(request);
wrong.climbs[0].lat = 91;
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "invalid latitude");
wrong = clone(request);
wrong.climbs[0].rate = 3;
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "inconsistent rate");
wrong = clone(request);
wrong.climbs[0].end = now + 301;
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "future time");
wrong = clone(request);
wrong.climbs[0].start -= RETENTION_SECONDS;
wrong.climbs[0].end -= RETENTION_SECONDS;
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "expired time");
wrong = clone(request);
wrong.climbs.push(clone(climb));
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "duplicate event in batch");
wrong = clone(request);
while (wrong.climbs.length <= MAX_BATCH_CLIMBS)
  wrong.climbs.push(clone(climb));
rejects(function() { validateRequest(wrong, token, "folder-id", now); },
        "oversize batch");
assert(mergeClimbs(incoming, incoming, now).length === 1,
       "idempotent retry");
var roundTrip = clone(incoming);
roundTrip[0].lon += 1e-14;
assert(mergeClimbs(incoming, roundTrip, now)[0].lon === incoming[0].lon,
       "coordinate floating-point round trip keeps original event");
var conflict = clone(incoming);
conflict[0].lat = 44;
rejects(function() { mergeClimbs(incoming, conflict, now); },
        "conflicting ID");
var expired = clone(climb);
expired.start -= RETENTION_SECONDS;
expired.end -= RETENTION_SECONDS;
assert(mergeClimbs([expired], incoming, now).length === 1, "expire old entries");
var cup = makeCup(incoming);
assert(cup.indexOf("4530.000") === -1, "coordinate sanity");
assert(cup.indexOf("4500.000N,00600.000E,1240.0m,1") >= 0, "CUP location");
assert(cup.indexOf("Observed: ") >= 0 && cup.indexOf(" UTC; Avg vario: +2.00 m/s") >= 0,
       "CUP recent-climb metadata");
assert(cupCoordinate(-45.5, true) === "4530.000S", "southern latitude");
assert(cupCoordinate(-6.5, false) === "00630.000W", "western longitude");
assert(cupCoordinate(5.999999999, false) === "00600.000E", "minute rollover");

var files = Object.create(null), writes = 0, accesses = 0;
var failCup = false, locked = false;
function file(name) {
  return {
    getBlob: function() {
      return {getDataAsString: function() { return files[name]; }};
    },
    setContent: function(body) {
      ++writes;
      if (name === "climbs.cup" && failCup)
        throw new Error("Simulated disk error");
      files[name] = body;
    }
  };
}
var folder = {
  getFilesByName: function(name) {
    var available = Object.prototype.hasOwnProperty.call(files, name);
    return {
      hasNext: function() { return available; },
      next: function() { available = false; return file(name); }
    };
  },
  createFile: function(name, body) { file(name).setContent(body); }
};
var DriveApp = {
  getFolderById: function(id) {
    ++accesses;
    assert(id === "folder-id", "configured folder");
    return folder;
  }
};
var PropertiesService = {
  getScriptProperties: function() {
    return {getProperty: function(name) {
      return name === "FOLDER_ID" ? "folder-id" : token;
    }};
  }
};
var LockService = {
  getScriptLock: function() {
    return {
      tryLock: function() { locked = true; return true; },
      hasLock: function() { return locked; },
      releaseLock: function() { locked = false; }
    };
  }
};
var ContentService = {
  MimeType: {JSON: "application/json"},
  createTextOutput: function(body) {
    return {setMimeType: function() { return JSON.parse(body); }};
  }
};
var console = typeof console === "undefined" ? {error: function() {}} : console;
function post(value) {
  return doPost({postData: {contents: JSON.stringify(value)}});
}

var unauthenticated = clone(request);
unauthenticated.token = "no";
assert(!post(unauthenticated).ok, "unauthorized POST");
assert(accesses === 0 && writes === 0, "authenticate before Drive access");
var reply = post(request);
assert(reply.ok && reply.batch_id === request.batch_id && reply.accepted === 1,
       "exact acknowledgement");
assert(!locked, "release receiver lock");
assert(JSON.parse(files["collaborative-flight.json"]).length === 1, "stored event");
assert(post(request).ok, "repeated POST accepted");
assert(JSON.parse(files["collaborative-flight.json"]).length === 1, "no duplicate");
var second = clone(request);
second.climbs[0].id = "other-client-123";
failCup = true;
assert(!post(second).ok, "CUP write error is not acknowledged");
assert(!locked, "release lock after error");
assert(JSON.parse(files["collaborative-flight.json"]).length === 2,
       "index survives interrupted export");
failCup = false;
assert(post(second).ok, "retry repairs failed export");
assert(JSON.parse(files["collaborative-flight.json"]).length === 2,
       "repair remains idempotent");
assert(files["climbs.cup"].indexOf(climbName(second.climbs[0])) >= 0,
       "repaired CUP");

var named = clone(climb);
named.public_identity = "Pilot F-AB";
assert(validPublicIdentity(named.public_identity), "ten-character identity");
assert(validPublicIdentity("\u00c9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9"),
       "ten non-ASCII characters");
assert(validPublicIdentity("\ud83d\ude80\ud83d\ude80\ud83d\ude80\ud83d\ude80\ud83d\ude80" +
                           "\ud83d\ude80\ud83d\ude80\ud83d\ude80\ud83d\ude80\ud83d\ude80"),
       "ten supplementary Unicode characters");
assert(!validPublicIdentity("12345678901"), "eleven characters rejected");
assert(!validPublicIdentity(""), "empty identity rejected");
assert(!validPublicIdentity("   "), "blank identity rejected");
assert(!validPublicIdentity("A\nB"), "control characters rejected");
assert(!validPublicIdentity("\ud800"), "unpaired surrogate rejected");
assert(validateClimb(named, now).public_identity === named.public_identity,
       "identity survives validation");
var invalidName = clone(named);
invalidName.public_identity = "too-long-name";
rejects(function() { validateClimb(invalidName, now); }, "invalid identity");
var shortName = climbName(named);
assert(/^Pilot F-AB-[A-Z0-9]{5}$/.test(shortName), "readable name shape");
assert(shortName === climbName(clone(named)), "stable hash");
var renamed = clone(named);
renamed.public_identity = "Other";
assert(climbName(renamed).slice(-5) === shortName.slice(-5),
       "hash derives from stable event fields, not editable identity");
renamed = clone(named);
renamed.start -= 1;
assert(climbName(renamed) !== shortName, "UTC field affects hash");
renamed = clone(named);
renamed.id = "different-client-123";
assert(climbName(renamed) !== shortName, "client ID affects hash");
assert(/^CF-[A-Z0-9]{5}$/.test(climbName(climb)), "legacy compact name");
var legacyUpgrade = mergeClimbs([climb], [named], now);
assert(legacyUpgrade[0].public_identity === named.public_identity,
       "retry enriches legacy identity");
renamed = clone(named);
renamed.public_identity = "Other";
assert(mergeClimbs([named], [renamed], now)[0].public_identity ===
       named.public_identity, "retry preserves first recorded identity");
named.public_identity = 'A,"B';
assert(makeCup([named]).indexOf('"A,""B-') >= 0, "CSV escaping");
assert(makeCup([named]).indexOf(named.id) === -1,
       "long internal ID is not a visible CUP name or code");
var fixed = {
  id: "stable-123", start: 1750000000, end: 1750000120,
  lat: 45, lon: 6, base: 1000, top: 1240, rate: 2,
  public_identity: "Pilot"
};
assert(climbName(fixed) === "Pilot-5NN8K", "SHA-256/base36 fixed vector");
var namedRequest = clone(request);
namedRequest.climbs[0].public_identity = "Pilot F-AB";
assert(post(namedRequest).ok, "identity-bearing POST accepted");
assert(files["climbs.cup"].indexOf('"Pilot F-AB-') >= 0,
       "identity reaches published CUP");
assert(JSON.parse(files["collaborative-flight.json"])[0].public_identity ===
       "Pilot F-AB", "identity persists in Drive index");
assert(post(request).ok, "legacy retry still accepted after upgrade");
assert(files["climbs.cup"].indexOf('"Pilot F-AB-') >= 0,
       "legacy retry does not erase identity");

var full = [];
for (var i = 0; i < MAX_STORED_CLIMBS; ++i) {
  var item = clone(climb);
  item.id = "full-" + i;
  full.push(item);
}
rejects(function() { mergeClimbs(full, incoming, now); },
        "capacity never silently evicts");
assert(mergeClimbs(full, [full[0]], now).length === MAX_STORED_CLIMBS,
       "retry works at capacity");
var receiverTestResult = "PASS: " + count + " receiver assertions";
if (typeof process !== "undefined")
  process.stdout.write(receiverTestResult + "\n");
