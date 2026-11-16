# Run your own relay (TURN)

Gridline connects collaborators directly, computer to computer. Some pairs of routers can't do
that: when one side is a *symmetric NAT* (Test my connection says so) and the other side lets in
only answers it asked for, Gridline tells you "these two routers can't connect directly". A relay
fixes that: both computers talk to a small server you run, and it passes the traffic between them.

Gridline does not run a relay for you. You run your own, so nobody else is involved. The traffic
through it stays end-to-end encrypted: the relay sees both IP addresses and how much data passes,
never your music or your edits.

## What you need

- A computer that is reachable from the internet and stays on while you collaborate: a rented
  server (the smallest size is plenty), a home server or NAS with a forwarded port, or a computer
  at a place with a public address.
- About 15 minutes.
- Only **one** of the two collaborators needs the relay set in Gridline. You can both use the same
  one.

## 1. Install coturn

coturn is a free, open-source TURN server.

- Debian or Ubuntu: `sudo apt install coturn`
- Fedora: `sudo dnf install coturn`
- Arch: `sudo pacman -S coturn`
- Other systems: see the coturn project's own instructions, or run its official container image.

On Debian and Ubuntu, also set `TURNSERVER_ENABLED=1` in `/etc/default/coturn`.

## 2. Configure it

Replace the whole of `/etc/turnserver.conf` (keep a copy of the old one) with this, then change the
three marked values:

```
# Gridline relay
listening-port=3478
fingerprint
lt-cred-mech
realm=gridline-relay

# CHANGE: a user name and a long random password (letters and digits)
user=gridline:CHANGE-THIS-PASSWORD

# CHANGE: only when this server is behind a router (home server, NAS): its public address
# external-ip=203.0.113.10

# the ports the relay uses for the traffic itself
min-port=49160
max-port=49200

# no admin console, no TLS needed for Gridline
no-cli
no-tls
no-dtls

# never relay into private networks (protects the network the server sits in)
no-multicast-peers
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=100.64.0.0-100.127.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255

# keep logs short: no IP addresses kept longer than needed
no-stdout-log
simple-log
```

Restart it: `sudo systemctl restart coturn` (and `sudo systemctl enable coturn` so it starts with
the computer).

## 3. Open the ports

On the server's firewall, and on its router when it is behind one (forward them to the server):

| Port | Protocol | What for |
| --- | --- | --- |
| 3478 | UDP | Gridline asks the relay for an address here |
| 49160 to 49200 | UDP | the relayed traffic |

For example with `ufw`: `sudo ufw allow 3478/udp` and `sudo ufw allow 49160:49200/udp`.
A rented server's provider may have its own firewall page: open the same ports there.

## 4. Fill in Gridline

In **Settings > Collaboration > TURN relay**:

- **Server**: the server's name or public address (for example `turn.example.org`), port `3478`.
- **User**: `gridline` (the part before the colon in the `user=` line).
- **Password**: the password from the `user=` line. Gridline keeps it in the system's secure store.

Then press **Test my connection**. It should say **TURN relay: works**. If it says it failed:

- *did not answer in time*: check the address, that port 3478 UDP is open on the firewall and the
  router, and that coturn is running (`sudo systemctl status coturn`).
- *gave no relay address*: check the user name and password, and that the relay ports
  (49160 to 49200 UDP) are open.
- *TURN server missing*: a user name or password is filled in but the server is empty.

## 5. Collaborate

Make a new invite (or join again). The codes now carry the relay's address, and when a direct
connection can't work the connection goes through your relay. The Collaboration panel shows the
path as *relay*.

Share the password only with people you collaborate with: anyone who has it can use your relay.
Change it (and restart coturn) when you stop collaborating with someone.

## Other ways that need no relay

- **Forward a port.** In Settings > Collaboration, set **UDP port** (for example 50000), forward that
  UDP port on your router to this computer, and tick **My router forwards this UDP port to this
  computer**. Collaborators then reach you directly whatever their router.
- **IPv6.** When both homes have IPv6 (Test my connection shows it), the routers usually don't get in
  the way at all.
- **A private network that both computers join** (a virtual network both of you install) also
  works: Gridline connects over it like on one home network.
- **A collaborator who reaches you both** carries the connection.
