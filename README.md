<p align="center"><img src="frontend/assets/logo.png" width="76" height="76" alt="Hands"></p>

# Macaroni

Macaroni streams a Mac desktop and its system audio to a browser over WebRTC. The browser can also send its microphone into a Mac application. An owner can invite people to watch the same screen, chat, and hand keyboard and mouse control to one participant.

This is a Hands fork of Bruno Martin’s [MoonlightWeb](https://github.com/linckosz/moonlight-web), based on v0.4.3. The viewer uses the Hands Computer mark, Figtree body type, Caprasimo headings and warm charcoal palette. The invitation, participant menu and control handoff follow the Hands Computer viewer and its [Neko client](https://github.com/david-kijko/neko/tree/c63e8ae4c4af29bc405717d6c5932a81b1e9169d/client).

## Build and open it on a Mac

Use an Apple Silicon Mac with a logged-in graphical session, Xcode Command Line Tools and Homebrew. The locally signed application uses the Homebrew Qt and OpenSSL installations on the build machine.

```sh
brew install cmake ninja pkg-config qtbase qtwebsockets openssl@3
git clone --recursive https://github.com/david-kijko/Macaroni.git
cd Macaroni
./scripts/build-macaroni-macos.sh
ditto build/Macaroni.app /Applications/Macaroni.app
open /Applications/Macaroni.app
```

To put the launcher on the Mac desktop, make an alias to **Macaroni.app** in Finder. Open the viewer address reported by the app. In **System Settings → Privacy & Security**, grant Macaroni **Screen & System Audio Recording** for picture and playback and **Accessibility** for keyboard and mouse. Choose **Quit & Reopen** if macOS asks. Chat-only rooms work while screen recording is unavailable.

## Microphone and playback

We added a macOS output for the existing WebRTC Opus microphone stream. The host decodes incoming Opus, buffers it for CoreAudio and writes mono samples to the output side of a virtual audio device. Mac applications record from that device’s input side. ScreenCaptureKit captures Mac playback and excludes the streaming process’s own audio so the injected microphone does not come straight back to the viewer.

1. Install **BlackHole 2ch** for the browser microphone. On a headless Mac, also install **BlackHole 16ch** as a separate playback destination.
2. Select **BlackHole 2ch** as the microphone in the Mac application receiving your voice. Set the Mac’s sound output to **BlackHole 16ch** for the headless setup. On a physical Mac you can keep your normal output.
3. Start the stream, turn on the microphone in the owner’s stream controls and grant the browser microphone access. The browser needs a secure context.

```sh
brew install --cask blackhole-2ch blackhole-16ch
```

On the development VM, a Chromium WebRTC test sent a 733 Hz tone into the Mac microphone input and received a separate 997 Hz tone played by the Mac. The test used real Opus packets in both directions. Desktop capture is a separate macOS permission.

## Invite people

Open **Share** in the Mac host card menu or while streaming. Select a display/application or **Chat only**, then **Create invitation → Copy link**. An invitation is reusable for four hours from creation. Each guest opens it, enters a name and receives their own secure, HttpOnly guest cookie. The invitation secret travels in the URL fragment. Up to **three guests** can join each host process alongside the owner.

- **Room chat** works in the guest lobby and during a stream. Messages and the participant list update while chat is open. The host keeps up to 80 messages per room in memory. A new participant sees messages sent during their invitation slot’s lifetime.
- **Guests can control** starts off. The owner can use a participant’s **Manage** menu to grant, transfer or revoke keyboard and mouse control. Turning guest requests off takes control back. The permission change reaches the stream worker without making the guest reconnect.
- With guest requests enabled, **Request control** gives the guest an unoccupied desktop or registers a request when someone holds it. The current guest controller can release control or hand it to another participant. Chat-only invitations never authorize desktop input or streaming.
- **Remove** disconnects that guest session. Anyone who still holds an active invitation can rejoin. **End sharing** revokes the link and all guest sessions for that room. Restarting the host also invalidates these invitations.

### Where the link works

Open the host on a **guest-reachable HTTPS origin** for a second computer to join. Use the Mac’s LAN or private VPN address with a certificate the guest trusts, or a deployment reachable by both browsers. The host can set `MACARONI_GUEST_ORIGIN` to its guest HTTPS origin before launch. A local-only SSH forward or a loopback link works on its own machine. The invitation does not create a public tunnel. For desktop sharing across networks, the guest also needs a working WebRTC media path.

Keep admin and guest access distinct when deploying a reverse proxy. The native host treats loopback as a trusted owner connection. A public proxy forwarding unrestricted traffic to that listener would grant its callers that trust. The development VM used a private SSH forward for the owner.

## Source map

| Feature | Implementation |
| --- | --- |
| Mac microphone and echo prevention | [CoreAudio microphone output](backend/src/streaming/MicOutputMac.cpp), [ScreenCaptureKit capture](backend/native-host/src/capture/macos/SckCapture.mm) |
| Invitations and live input permissions | [Hands sharing routes](backend/src/server/routes/HandsSharing.cpp), [share manager](backend/src/server/ShareManager.cpp) |
| Participant UI and chat | [invite panel](frontend/js/ui/HandsInvite.js), [chat panel](frontend/js/ui/RoomChat.js), [chat routes](backend/src/server/routes/RoomChat.cpp) |
| Mac bundle | [build script](scripts/build-macaroni-macos.sh) |

The [upstream engineering wiki](docs/wiki/Home.md) covers the underlying streaming engine and its other platforms. Macaroni is distributed under GPL-3.0-or-later. See [LICENSE](LICENSE), [original credits and component licenses](COPYRIGHT), and the font licenses in [frontend/assets/fonts](frontend/assets/fonts).
