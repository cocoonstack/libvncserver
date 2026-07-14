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
- Ordinary clients wait for that first decoded frame. Framebuffer publication
  is synchronized with each client's complete RFB update, so concurrent
  Tight/JPEG or Raw clients never observe a black or partially replaced frame.
- Each H.264 client has its own frame cursor; a slow client skips to a later
  keyframe without blocking other clients.
- H.264 subscriptions stay active so new frames are pushed without a
  per-frame RFB request/response round trip.
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

An ordinary TigerVNC viewer uses the Tight/JPEG fallback with
`-RemoteResize=0 -Shared=1 host::5900`.
