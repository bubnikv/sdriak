# Config access and module porting

Date: 2026-08-04

`ConfigManager::read()` returns a read-only access; `edit()` adds mutation. Both
own the manager mutex until the access object is destroyed, so keep them in the
smallest practical scope and copy values out before calling external code.

## Porting a module from upstream SDR++

Upstream modules bracket `config.conf` with `acquire()` and `release(modified)`.
`conf` is private here: replace a pair that releases with `false` by `read()`,
and one that releases with `true` by `edit()`. Pass an already-open access or
section into helpers that belong to the same operation.

Only one config access may be active on a thread, across all `ConfigManager`
instances. Nested access compiles, but throws `std::logic_error` at runtime. In
particular, release the current access before calling module callbacks or helpers
that may open either the core config or a module-local config.

## Autosave and desktop shutdown

Autosave coalesces changes for one second from the first notification. Disk I/O
uses a snapshot and does not hold the config mutex.

`shutdown()` is terminal. It rejects new accesses, unregisters the manager and
waits for an in-progress autosave, then synchronously commits any remaining dirty
state. Stop and destroy every object or worker that can write the config before
calling it. A failed shutdown remains retryable but continues rejecting access.

Desktop builds serialize commits from every SDRIAK process with the config-root
lock. A shutdown commit rereads the newest disk document and merges only changes
relative to its own baseline, so independent edits survive concurrent instance
shutdown. Conflicting edits to the same scalar, array, or object leaf remain
last-commit-wins.
