# Guest HTTPS gateway for an SSH-only Mac

Use the gateway on an internet-facing machine with an existing private SSH route to the Mac. The owner keeps a separate private browser address. The guest origin serves the name-entry page, public viewer assets, guest APIs and the three guest signaling sockets. It forwards only the guest cookie. Its HTTP listener binds to loopback for a TLS reverse proxy or tunnel on the same machine.

1. Forward the native host’s HTTPS port to loopback on the gateway machine. The example expects `127.0.0.1:18443`. Verify the Mac certificate fingerprint through the trusted SSH connection and save its SHA-256 hex digest to a local file.
2. Forward the Mac’s guest ICE-TCP ports **48552–48554** to the same loopback ports on the gateway. These map to guest slots 2, 3 and 4. The owner media and signaling ports stay private.
3. Route a dedicated guest HTTPS hostname to the gateway’s loopback port **18992**, preserving the public Host and WebSocket Upgrade. Install the Python dependency with `python3 -m pip install aiohttp` in a virtual environment.
4. Start the gateway with the public origin, fingerprint file and the gateway’s public IPv4 address.

```sh
python3 deploy/guest/gateway.py \
  --origin https://guests.example.com \
  --pin-file /path/to/verified-mac-certificate.sha256 \
  --media-ip 203.0.113.10
```

Allow inbound TCP to those three media ports on that public interface. The gateway accepts media for a slot during its authorized guest WebSocket connection. HTTPS carries page content, chat and signaling. The WebRTC payload uses the separate ICE-TCP connection with DTLS. Without `--media-ip`, chat and signaling work, and the deployment supplies its own reachable media path.

5. Write the guest HTTPS origin to `~/Library/Application Support/Macaroni/guest-origin` on the Mac. Open the private owner viewer, create an invitation and open the generated link in another browser. Confirm that `/api/hosts` and `/api/share/room` on the public origin return 404. With chat-only selected, a guest can exchange messages without screen-recording permission.

For a time-limited VM, set the edge, gateway and firewall rule to expire with the reservation. Ending a room invalidates its invitation and bound guest sessions immediately. The gateway does not purchase or renew compute.
