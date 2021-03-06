# ChangeLog

### v0.10.14

 * Fixes to fq-client.lua on OmniOS

### v0.10.13

 * Add `fqs` tool for sending messages from stdin
 * Test suite utilizing `mtevbusted` from
   [libmtev](https://github.com/circonus-labs/libmtev/) (PR #37)

### v0.10.12

 * Fix misuse of stack for freeing messages (0.10.11 fix was bad).
 * Add Linux futex support for lower-latency idle network wake-up.
 * Ensure message ordering on per-client data connections.

### v0.10.11

 * Fix crash when shutting down client that has never seen a message.

### v0.10.10

 * Fix source management issue. 0.10.9 tag exluded commits.
 * Change message free-lists to prevent use-after-free on thread exit.

### v0.10.9

 * Fix builds on newer Mac OS X
 * Change message free-lists to prevent use-after-free on thread exit.
 * Fix bug in server->client heartbeats not beating.

### v0.10.8

 * Fix querystring parsing crash when parameters are not k=v form.
 * Resume on-disk queues at the right checkpoint location.

### v0.10.7

 * Fix bug in route binding prefix matching causing misdirected messages
   (clients could get more than they asked for).
 * Fix bug on some Linux systems regarding exposed symbols.

### v0.10.6

 * Fix crashing issue enqueueing messages due to unsafe use of spsc fifos.
 * Add dynamic loading of routing program extensions.
 * Move the "sample" function to a dynamic extension for example purposes.
