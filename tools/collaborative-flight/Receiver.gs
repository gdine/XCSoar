// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

var RETENTION_SECONDS = 24 * 60 * 60;
var MAX_STORED_CLIMBS = 4096;
var MAX_BATCH_CLIMBS = 64;

function validId(value) {
  return typeof value === "string" &&
    /^[A-Za-z0-9_-]{1,128}$/.test(value);
}

function finiteNumber(value) {
  return typeof value === "number" && isFinite(value);
}

function validPublicIdentity(value) {
  if (typeof value !== "string" || /^ *$/.test(value) ||
      /[\u0000-\u001f\u007f]/.test(value))
    return false;
  var count = 0;
  for (var i = 0; i < value.length; ++i) {
    var code = value.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff) {
      var next = value.charCodeAt(++i);
      if (!(next >= 0xdc00 && next <= 0xdfff))
        return false;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      return false;
    }
    ++count;
  }
  return count <= 10;
}

function validateClimb(value, now) {
  if (!value || !validId(value.id) ||
      !finiteNumber(value.start) || Math.floor(value.start) !== value.start ||
      !finiteNumber(value.end) || Math.floor(value.end) !== value.end ||
      value.start <= 0 || value.end <= value.start ||
      value.end - value.start > 7200 ||
      value.end < now - RETENTION_SECONDS || value.end > now + 300 ||
      !finiteNumber(value.lat) || Math.abs(value.lat) > 90 ||
      !finiteNumber(value.lon) || Math.abs(value.lon) > 180 ||
      !finiteNumber(value.base) || value.base < -1000 ||
      !finiteNumber(value.top) || value.top > 20000 ||
      value.top <= value.base ||
      !finiteNumber(value.rate) || value.rate <= 0 || value.rate > 50 ||
      Math.abs(value.rate -
               (value.top - value.base) / (value.end - value.start)) > 0.01)
    throw new Error("Invalid climb");

  var result = {
    id: value.id, start: value.start, end: value.end,
    lat: value.lat, lon: value.lon,
    base: value.base, top: value.top, rate: value.rate
  };
  // Missing identity is supported for older clients and stored observations.
  if (value.public_identity !== undefined) {
    if (!validPublicIdentity(value.public_identity))
      throw new Error("Invalid public identity");
    result.public_identity = value.public_identity;
  }
  return result;
}

function validateRequest(request, token, folderId, now) {
  if (typeof token !== "string" || token.length < 32 || token.length > 256 ||
      !validId(folderId))
    throw new Error("Receiver is not configured");
  if (!request || request.token !== token)
    throw new Error("Authentication failed");
  if (request.version !== 1 || request.folder_id !== folderId ||
      !validId(request.batch_id) || !Array.isArray(request.climbs) ||
      request.climbs.length < 1 || request.climbs.length > MAX_BATCH_CLIMBS)
    throw new Error("Invalid request");

  var ids = Object.create(null);
  return request.climbs.map(function(value) {
    var climb = validateClimb(value, now);
    if (ids[climb.id])
      throw new Error("Duplicate climb in batch");
    ids[climb.id] = true;
    return climb;
  });
}

function mergeClimbs(stored, incoming, now) {
  if (!Array.isArray(stored) || stored.length > MAX_STORED_CLIMBS)
    throw new Error("Invalid stored climb index");
  var byId = Object.create(null);
  stored.forEach(function(climb) {
    if (!climb || !finiteNumber(climb.end))
      throw new Error("Invalid stored climb");
    if (climb.end >= now - RETENTION_SECONDS) {
      var valid = validateClimb(climb, now);
      if (byId[valid.id])
        throw new Error("Duplicate stored climb");
      byId[valid.id] = valid;
    }
  });
  incoming.forEach(function(climb) {
    var previous = byId[climb.id];
    if (previous) {
      if (previous.start !== climb.start || previous.end !== climb.end ||
          ["lat", "lon", "base", "top", "rate"].some(function(key) {
            return Math.abs(previous[key] - climb[key]) > 1e-8;
          }))
        throw new Error("Conflicting climb ID");
      if (previous.public_identity === undefined &&
          climb.public_identity !== undefined)
        previous.public_identity = climb.public_identity;
    } else {
      byId[climb.id] = climb;
    }
  });
  var result = Object.keys(byId).map(function(id) { return byId[id]; });
  if (result.length > MAX_STORED_CLIMBS)
    throw new Error("Receiver is full; retry after old climbs expire");
  result.sort(function(a, b) {
    return a.end - b.end || a.id.localeCompare(b.id);
  });
  return result;
}

function cupCoordinate(value, latitude) {
  var thousandths = Math.round(Math.abs(value) * 60 * 1000);
  var degrees = Math.floor(thousandths / 60000);
  var minutes = (thousandths % 60000 / 1000).toFixed(3);
  var degreeText = String(degrees);
  while (degreeText.length < (latitude ? 2 : 3))
    degreeText = "0" + degreeText;
  if (Number(minutes) < 10)
    minutes = "0" + minutes;
  return degreeText + minutes +
    (latitude ? (value < 0 ? "S" : "N") : (value < 0 ? "W" : "E"));
}

function climbName(c) {
  var fields = JSON.stringify([
    c.id, c.start, c.end, c.lat.toFixed(6), c.lon.toFixed(6),
    c.base.toFixed(1), c.top.toFixed(1), c.rate.toFixed(2)
  ]);
  var digest = Utilities.computeDigest(Utilities.DigestAlgorithm.SHA_256,
                                      fields, Utilities.Charset.UTF_8);
  var hash = 0;
  for (var i = 0; i < digest.length; ++i)
    hash = (hash * 256 + (digest[i] & 255)) % 60466176; // 36^5
  var suffix = hash.toString(36).toUpperCase();
  while (suffix.length < 5)
    suffix = "0" + suffix;
  return (c.public_identity === undefined ? "CF" : c.public_identity) +
    "-" + suffix;
}

function cupQuote(value) {
  return '"' + value.replace(/"/g, '""') + '"';
}

function makeCup(climbs) {
  var lines = ["name,code,country,lat,lon,elev,style,rwdir,rwlen,freq,desc"];
  climbs.forEach(function(c) {
    var observed = new Date(c.end * 1000).toISOString()
      .replace("T", " ").replace(".000Z", " UTC");
    var description = "Observed: " + observed +
      "; Avg vario: +" + c.rate.toFixed(2) + " m/s" +
      "; Gain: " + (c.top - c.base).toFixed(0) + " m" +
      "; Duration: " + (c.end - c.start) + " s";
    var name = cupQuote(climbName(c));
    lines.push(name + "," + name + ",," +
               cupCoordinate(c.lat, true) + "," +
               cupCoordinate(c.lon, false) + "," +
               c.top.toFixed(1) + 'm,1,,,,"' + description + '"');
  });
  return lines.join("\r\n") + "\r\n";
}

function findFile(folder, name) {
  var matches = folder.getFilesByName(name);
  var file = matches.hasNext() ? matches.next() : null;
  if (matches.hasNext())
    throw new Error("Duplicate receiver file: " + name);
  return file;
}

function writeFile(folder, name, content, type) {
  var file = findFile(folder, name);
  if (file)
    file.setContent(content);
  else
    folder.createFile(name, content, type);
}

function jsonResponse(value) {
  return ContentService.createTextOutput(JSON.stringify(value))
    .setMimeType(ContentService.MimeType.JSON);
}

function doPost(event) {
  var lock = null;
  var stage = "validation";
  try {
    if (!event || !event.postData || !event.postData.contents ||
        event.postData.contents.length > 131072)
      throw new Error("Invalid request body");
    var request = JSON.parse(event.postData.contents);
    var properties = PropertiesService.getScriptProperties();
    var folderId = properties.getProperty("FOLDER_ID");
    var now = Math.floor(Date.now() / 1000);
    var incoming = validateRequest(request,
      properties.getProperty("UPLOAD_TOKEN"), folderId, now);

    stage = "locking";
    lock = LockService.getScriptLock();
    if (!lock.tryLock(5000))
      throw new Error("Receiver is busy");

    stage = "reading Drive index";
    var folder = DriveApp.getFolderById(folderId);
    var index = findFile(folder, "collaborative-flight.json");
    var stored = index ? JSON.parse(index.getBlob().getDataAsString()) : [];
    var merged = mergeClimbs(stored, incoming, now);
    // Store deduplication state first; retries repair an interrupted CUP write.
    stage = "writing Drive index";
    writeFile(folder, "collaborative-flight.json", JSON.stringify(merged),
              "application/json");
    stage = "writing CUP";
    writeFile(folder, "climbs.cup", makeCup(merged), "text/plain");
    return jsonResponse({
      ok: true, batch_id: request.batch_id, accepted: incoming.length
    });
  } catch (error) {
    // Do not log the request, token, locations, or a Drive exception message.
    console.error("Collaborative flight receiver failed at " + stage);
    return jsonResponse({ok: false, error: "Request rejected; check receiver setup"});
  } finally {
    if (lock && lock.hasLock())
      lock.releaseLock();
  }
}
