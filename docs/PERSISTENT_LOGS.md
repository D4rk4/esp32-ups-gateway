# Persistent event log

The gateway stores events in the raw `logs` partition of 1 MiB (`0x7F0000`),
separately from configuration and OTA slots. It is not a filesystem. The ring
holds up to 3825 records; one of the 256 sectors is left spare for safe rotation
and cleanup. When full, old events are evicted. The last 48 events are also
available in RAM for existing handlers. UPS measurements are not written to the
event log: measurement history remains limited to 240 samples in RAM and is lost
on restart.

Event recording is scheduled at accumulation of 16 events or after 30 seconds;
on a managed restart the gateway tries to flush the buffer. A sudden power loss
may lose events not yet written. A flash error or an accumulated queue may
increase this interval; on overflow events may be lost. No zero-loss guarantee
and no flash lifetime guarantee are claimed. Records are added sequentially with
sector rotation, without rewriting the NVS counter, without idle writes or
formatting at boot. Cleanup uses a marker in a new sector, not erasure of the
whole partition.

In **Logs → Events** a filter, **Newest** and **Older** buttons, and CSV export
of the current page are available. The newest page updates automatically; the
old one stays put until a manual refresh. The indicator shows storage
availability, error, and the number of recorded, pending, and dropped events. If
UTC is unknown, the uptime of the boot in which the event occurred is shown, not
the age of the current boot. An administrator can clear the event log in RAM and
flash; the SNMP traps queue is not cleared in this case. A persistent cleanup
error is returned to the user, not reported as success.

`GET /api/events` and `/api/events.csv` accept `min_severity`, `contains`,
`limit` (1–48) and an optional exclusive cursor `before`. The JSON contains
`next_before` (0 at end-of-chain) and `persistence` with `available`, `status`,
`error`, `stored`, `capacity`, `pending`, `dropped`, `flush_interval_s`. The CSV
contains only the requested page. Filtering is done over the whole available
ring, not just the last 48 events. Ordinary install/update operations do not
erase the `logs` partition; full flash erase and hardware failure can destroy the
data.
