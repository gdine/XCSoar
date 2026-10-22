# Collaborative flight: Google Drive receiver

XCSoar's optional **Config → Services → Collaborative flight** service sends
summaries of locally detected climbs, not the full flight track. This receiver
writes them to a Drive folder and publishes `climbs.cup` for the service's
own group-climb display. The independent **Recent climbs** service can
continue downloading a different file, for example OGN climbs, at the same
time. A folder share link alone cannot accept
uploads: an authorized Apps Script web app is required.

## Deploy once for your group

1. Create a dedicated Google Drive folder. Copy its share link, in the form
   `https://drive.google.com/drive/folders/FOLDER_ID`.
   Everyone who can read this folder can see the shared climb positions,
   times, altitudes, public pilot identities and installation/event identifiers.
2. Go to <https://script.google.com/>, create a project, and paste
   [Receiver.gs](Receiver.gs) into its script editor.
3. In **Project settings → Script properties**, add:
   - `FOLDER_ID`: only the folder ID from the link, not the whole URL.
   - `UPLOAD_TOKEN`: a randomly generated secret of 32–256 characters.
     For example, generate 32 random bytes locally with
     `openssl rand -hex 32`. Do not reuse an account password.
4. Choose **Deploy → New deployment → Web app**.
   Set **Execute as** to yourself (the folder owner) and **Who has access**
   to **Anyone**. Authorize the requested Drive access in your own Google
   account. Workspace administrators may prohibit anonymous web apps.
   Do not grant authorization to an untrusted copy of this script.
5. Copy the deployed HTTPS URL, which must have the form
   `https://script.google.com/macros/s/DEPLOYMENT_ID/exec`.
   Do not use the editor URL or the `/dev` test URL.
6. In XCSoar, open **Config → Services → Collaborative flight**.
   Enter the shared folder link, upload service URL and upload secret.
   The upload interval defaults to three minutes (1–60 minutes).
   Check **Public identity** before enabling: it is initially your pilot name
   plus glider registration, truncated to 10 Unicode characters (or `CF` if
   both are empty). You can edit it; saved choices are not overwritten when
   your pilot or aircraft settings change.
   Optionally enable **Log transfers**, then enable the service.
   Enabling is explicit consent to sharing these summaries with the group.
7. After the first successful upload, `climbs.cup` appears in the folder.
   For unauthenticated downloads, share this file with **Anyone with the
   link**, then put its **file** share link in
   **Services → Collaborative flight → CUP file link** on participating devices.
   The folder link is not the CUP file link.

## Displaying collaborative climbs

The **Collaborative flight** switch controls both sharing and group-climb
display. Turning it off removes only collaborative waypoints, not those owned
by **Recent climbs**. Either service can run by itself or both can run together.

In **Services → Collaborative flight**, set:

- **CUP file link**: the public link of the group's `climbs.cup` file, not the
  folder or upload-service URL. Leave it blank to continue sharing without
  displaying group climbs. Existing installations need to set this new link;
  it is not copied from Recent climbs, which may contain a different source.
- **Update interval**: download frequency, 1–60 minutes, default 3 minutes.
  This is separate from the **Upload interval**.
- **Maximum age**: 15–1,440 minutes in 15-minute steps, default 60 minutes.
- **Minimum climb rate**: zero disables rate filtering. Units follow altitude:
  metres use m/s (0.1 steps), feet use ft/min (20 steps).

Collaborative climbs use the same temporary thermal waypoints, observation
timestamps and climb-rate colour scale as Recent climbs. No downloaded
waypoints are saved to your waypoint files. Each service owns its own cache,
refresh schedule and filters. Age expiry and filter changes work on cached
data even offline. Lowering a rate threshold or increasing the maximum age
restores eligible cached entries. Changing the CUP link clears that service's
old-source waypoints before downloading the new file.

If both services are deliberately pointed at the same CUP file, its climbs
appear as separate entries owned by each service. There is no cross-service
deduplication; normally use the OGN file for Recent climbs and the group file
for Collaborative flight.

When changing script code, deploy a new version using **Manage deployments**
to update the existing web app. Give the same endpoint, folder and secret to
group members. Keep the secret private; rotate it in Script properties and
all devices if it leaks. XCSoar stores it in its profile, masked in the UI
but **not encrypted**. Do not distribute profiles containing the secret.

## Readable climb names

New observations carry the **Public identity** configured in XCSoar. CUP names
and codes are `IDENTITY-ABCDE`: the identity (1–10 Unicode characters) followed
by a five-character uppercase base36 hash of stable event fields, including
the internal event ID and UTC times. The hash uses SHA-256 reduced to five
base36 characters; collisions are unlikely, not impossible. Long internal IDs
remain in the deduplication index, not the displayed CUP name or code.
Identity is saved with each queued observation, so editing it affects only
newly detected climbs. Previously queued observations without an identity
adopt the current one on upgrade.

**Receiver upgrade required:** replace the Apps Script code with this version
and use **Deploy → Manage deployments → Edit → New version → Deploy** on the
existing deployment, keeping its URL and Script properties. Updating XCSoar
alone cannot change CUP names: the receiver writes the file. Older clients and
stored entries remain supported and use `CF-ABCDE` until an identity is known.
An old receiver accepts uploads but discards the new identity and keeps the
old long names. The next successful upload to the updated receiver regenerates
the CUP file without changing its Drive ID or public link. Previously delivered
anonymous entries cannot recover a pilot identity retrospectively.

## Detection, low-priority work and offline behavior

- Only real, flying GPS fixes are sampled, at most once per second.
  Simulator and IGC/NMEA replay positions are excluded.
- Detection uses the Python RecentClimbs IGC circling model: a two-state
  Viterbi smoother, a five-second heading window, a turn threshold above
  6 degrees/second, speed above 15 km/h, a duration greater than 60 seconds
  and positive altitude gain. XCSoar's own flying state replaces the
  Python flight/landing HMM. Altitudes are GPS metres.
- Only closed climbs are emitted, after a five-minute lookahead to stabilize
  classification. **A three-minute upload interval is not a three-minute
  detection delay.** Initial sharing may take five minutes plus an upload
  interval after leaving the thermal.
- The detector retains at most 7,200 raw fixes in memory. Raw fixes are never
  saved or uploaded; an unfinished climb is lost if XCSoar exits.
  GPS gaps over 15 seconds or implausible jumps split detection.
- CPU/file work runs on an idle-priority worker. Detection and new uploads
  defer when the available CPU-load estimate is at least 80%. On platforms
  without a load estimate, the worker uses the platform's idle scheduling
  support. HTTP runs asynchronously on XCSoar's network event loop; its
  shared thread is not reprioritized.
- Any internet connection works, including mobile data or Wi-Fi. There is no
  modem or data subscription management. Offline/failing transfers retry
  after the configured interval when connectivity is available.
- Summaries are stored atomically in `XCSoarData`, in a separate
  `collaborative-flight-FOLDER_ID.json` queue for each folder. Up to 512 local
  summaries are retained for 24 hours, including acknowledgement records to
  prevent redetection duplicates. Expiry and capacity errors are logged.
  Pending summaries survive application restarts. Disabling the service stops
    collection and uploading and removes collaborative display waypoints, but does
    not delete queued summaries.
- A queue is never cleared on a timeout, HTTP error, invalid response or
  missing acknowledgement. Retried event IDs are deduplicated on the receiver.
  Changing folders does not send the previous folder's queue to a new group.
- Both devices' clocks must be correct. The receiver rejects climbs more than
  24 hours old or over five minutes in the future.

## Receiver operation and limits

The receiver authenticates before accessing Drive. It accepts batches of at most
64 summaries, locks updates against concurrent clients, rejects conflicting
event IDs and retains at most 4,096 unexpired group summaries. It never evicts
unexpired data to claim a successful upload. If full, clients retain their queues
and retry. Google Apps Script and Drive quotas apply; this is intended for a
small group, not a public high-volume service.

`collaborative-flight.json` is the shared deduplication index, not a token
store. Do not edit, delete or rename this file or `climbs.cup` while the receiver
is in use. Updates preserve the CUP file ID/share link. An interrupted CUP write
is repaired by the next successful retry. Old receiver entries are pruned on the
next upload; each service's age filter also expires displayed climbs when
no upload occurs.

Only HTTPS is accepted and certificates are verified. Apps Script's normal
ContentService redirect is fetched with a separate GET: the POST secret/body
is never forwarded to the redirect destination.

Enable **Log transfers** to see attempt counts, acknowledgements and CPU
deferrals in XCSoar's log. Errors and expired-summary counts are always logged.
Secrets, request bodies and individual locations are not written to transfer
logs. If no uploads appear:

1. Confirm you are using the XCSoar build containing this service, in Fly mode,
   with a real GPS, a correct clock and a flying-state indication.
2. Allow enough time to finish a qualifying thermal and its five-minute
   classification lookahead.
3. Check that the receiver is deployed for **Anyone**, executes as the folder
   owner, and has matching Script properties and XCSoar settings.
4. Inspect XCSoar logs and the script's **Executions** page. The receiver
   deliberately returns generic failures to avoid disclosing private data.

Local tests cover classification, queue/protocol policy and the receiver with
mock Google services. A real Drive transfer must still be checked after you
deploy and authorize the web app.
