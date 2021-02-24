# failoverproxy

This is a trivial opaque proxy that forwards all connections to a specific host. It's designed to handle high connection volume and high traffic, and supports atomically changing the destination host and disconnecting all connections. This is potentially useful for close-to-zero-downtime failovers for certain services.

## Building

0. Make sure libevent is installed.
1. Build and install [phosg](https://github.com/fuzziqersoftware/phosg).
2. Run `make`.

## Running

Run it like `./failoverproxy <listen-port> <shell-port> <dest-host> [dest-port]`. If `dest-port` is not given, it's assumed to be the same as `listen-port`. The proxy will listen for new connections on `listen-port`, and for each one, make a connection to `dest-host` on `dest-port` and forward all data between the two endpoints bidirectionally. You can see the number of open connections and perform the atomic destination switchover by connecting to the shell port with something like `nc localhost <shell-port>`; in this shell, you can run `help` to see what the commands are.
