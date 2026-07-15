# scrcpy-rfb

`scrcpy-rfb` bridges the packetized H.264 video and control sockets from
scrcpy 4.1 to one RFB/VNC port.

- Clients that advertise Open H.264 encoding 50 receive the original Android
  H.264 packets without server-side transcoding.
- Other VNC clients automatically receive Tight/JPEG from a lazily decoded
  framebuffer on the same port.
- When the first ordinary VNC client connects, the bridge asks scrcpy 4.1 to
  reset the video encoder so a static Android screen still produces an
  immediate config packet and keyframe instead of a black fallback frame.
- Ordinary clients wait for that first decoded frame. The bridge compares
  32-pixel tiles and marks only changed runs, so Tight/JPEG, ZRLE, Hextile, and
  Raw do not receive an unconditional full-screen rectangle on every frame.
- The decoded fallback is a latest-frame-only buffer. Slow clients accumulate
  damage against the current framebuffer instead of queueing stale video
  frames, and publication adapts between 60, 30, and 20 FPS using the slowest
  ordinary client's measured update time.
- The bridge disables LibVNCServer's default 5 ms update defer because decoded
  frames are already coalesced. Each client socket is capped to a 256 KiB send
  buffer and, on Linux, a 64 KiB `TCP_NOTSENT_LOWAT`, so a slow connection
  applies backpressure instead of building a long queue of visually stale
  updates.
- Ordinary Tight clients that request JPEG adapt between Q92, Q86, and Q80 from
  measured encode-and-send time, never exceeding the client's requested
  quality. Tight compression level 1 minimizes server-side latency; clients
  that do not request JPEG retain lossless Tight behavior.
- A high-confidence luma matcher recognizes vertical Android scrolling and
  emits RFB CopyRect for clients that advertise it. The normal tile comparison
  runs afterward, repairing exposed rows, fixed app bars, or a false match and
  providing the normal pixel fallback to clients without CopyRect.
- Framebuffer publication uses a writer lock while ordinary client encoders use
  shared reader locks. Multiple ordinary clients can encode the same stable
  frame concurrently without observing a black or partially replaced frame.
- Each H.264 client has its own frame cursor; a slow client skips to a later
  keyframe without blocking other clients.
- H.264 subscriptions stay active so new frames are pushed without a
  per-frame RFB request/response round trip. H.264 clients retain their own
  full-screen streaming subscription, so their wakeups do not turn ordinary
  clients' tile damage back into full-screen updates.
- Client network output runs in independent threads. Input messages are
  serialized, and one client owns an active pointer drag until button-up or
  disconnect so concurrent users cannot leave Android with a stuck touch.

Pushes to the `dev` branch publish the following assets to the moving `dev`
prerelease:

- `scrcpy-rfb-linux-amd64`
- `scrcpy-rfb-linux-arm64`
- one `.sha256` file for each binary
- `build-info.json`

The binaries target the Ubuntu 22.04 userspace ABI and dynamically require
only glibc, libm, zlib, and libjpeg.so.8. FFmpeg's H.264 decoder and swscale are
linked statically.

For an H.264-enabled TigerVNC viewer, turn off automatic encoding selection so
it does not override the explicit H.264 preference. The bridge exposes the
fixed scrcpy session size, so remote desktop resizing must also be disabled:

```sh
vncviewer -AutoSelect=0 -PreferredEncoding=H.264 -RemoteResize=0 -Shared=1 \
  host::5900
```

Ordinary VNC clients use Tight/JPEG, ZRLE, Hextile, or Raw fallback with their
default settings. For maximum compatibility, the bridge suppresses dynamic
desktop-size extensions on this path and exposes the fixed scrcpy size from
ServerInit. A client that advertises H.264 but selects Tight is given lossless
Tight, avoiding JPEG ABI problems in specialized H.264 viewer builds; regular
VNC clients retain performant Tight/JPEG.

Apache Guacamole does not include Tight in its default VNC encoding list. For
the optimized ordinary path, configure the connection with:

```text
encodings: tight copyrect zrle hextile raw
compress-level: 1
quality-level: 8
disable-display-resize: true
```

Quality level 8 initially maps to Q92; the server may lower it to Q86 or Q80
when that client's send time shows backpressure. ZRLE and Raw remain available
as maximum-compatibility fallbacks.
