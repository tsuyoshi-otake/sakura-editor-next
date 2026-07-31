# Win32 Request Runtime Tests

Keep these tests deterministic and free of public-network access. Exercise input
validation before WinHTTP can connect, explicit terminal failure mapping, and the
clock/scheduler/jitter adapters independently. Do not add live proxy, PAC, TLS,
or credential integration coverage here; those require a controlled integration
environment and must remain opt-in.

Verify that one caller deadline/cancellation token spans proxy resolution,
credential lookup, transport, redirect, retry delay, and response consumption.
Cover `Off` as a no-lookup guarantee, invalid system/PAC resolver output,
deduplicated in-flight work, bounded retry/redirect behavior, `Retry-After`, and
every terminal result without exposing endpoint, proxy, or credential values.

For detached clients, prove that an immutable proxy-policy snapshot remains
usable after the configuration service and policy adapter are destroyed. A raw
configuration reference in a request worker is a lifetime failure even if
ordinary request tests pass.
