# Known Issues

## Spurious OTLP log export errors (Status:204)

### Symptom

The following error messages appear in stdout during gameplay:

```
[OTLP HTTP Client] Export failed, Status:204, ...
[OTLP LOG HTTP Exporter] ERROR: Export N log(s) error: 1
```

### Impact

**Logs are being ingested into Loki correctly** despite these messages. No data is lost. The errors are purely cosmetic — the SDK is misreporting a successful HTTP 204 response as a failure. Traces are unaffected (Grafana Cloud's traces endpoint returns 200).

### Root Cause

The installed version of opentelemetry-cpp (v1.14.2) incorrectly treats HTTP 204 "No Content" as a failure. 204 is a valid 2xx success response per RFC 7231, but older SDK versions only accept 200 as success.

This was fixed in opentelemetry-cpp PR [#2712](https://github.com/open-telemetry/opentelemetry-cpp/pull/2712) — *"All 2xx return codes should be considered successful"* — released in **v1.16.0** (June 2024).

### Fix

Upgrade opentelemetry-cpp from v1.14.2 to v1.16.0 or later. The `install.sh` script pins the version and will need updating accordingly.

### References

- https://github.com/open-telemetry/opentelemetry-cpp/pull/2712
- https://github.com/open-telemetry/opentelemetry-cpp/releases/tag/v1.16.0
