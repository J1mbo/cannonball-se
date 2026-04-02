# OTel Metrics Analysis for Cannonball SE

## Context

This document evaluates whether to add OpenTelemetry **Metrics** alongside the traces and logs
described in `ADD_LOGS.md`. The ultimate goal is a Grafana dashboard showing aggregate gameplay
data across all sessions, potentially from multiple concurrent machine instances.

### Current Signal Architecture

| Signal | Plan | Backend | Query Language |
|--------|------|---------|----------------|
| Traces | ✅ Implemented | Tempo | TraceQL |
| Logs | 📋 Planned (ADD_LOGS.md) | Loki | LogQL |
| Metrics | ❓ This doc | Mimir | PromQL |

The Grafana Cloud OTLP gateway already routes all three signal types via the same endpoint/auth:
`/v1/traces` → Tempo, `/v1/logs` → Loki, `/v1/metrics` → Mimir.

---

## What Metrics Add Over Logs

Both signals can answer "how many crashes happened in the last hour?", but differently:

**Logs approach (LogQL)**:
```logql
count_over_time(
  {service_name="cannonball-se"} | json | event="game.crash" [1h]
)
```
This parses raw log records at query time — Loki scans, deserializes JSON, and filters.

**Metrics approach (PromQL)**:
```promql
increase(cannonball_crashes_total[1h])
```
This reads pre-aggregated counters stored as time-series — no parsing, sub-millisecond for any
volume of data.

### When Metrics Win

| Use Case | Logs | Metrics |
|----------|------|---------|
| "How many games this week?" | Workable | Faster |
| "Crash rate per stage over time" | Slow (JSON parse) | Fast (pre-aggregated) |
| "Score histogram across all players" | Awkward | Natural (histogram type) |
| "Multi-instance aggregation" | Requires label join | Built-in (sum by instance) |
| "Individual crash details" | ✅ Excellent | ✗ Not possible |
| "What stage did player AAA crash on?" | ✅ Excellent | ✗ Not possible |
| "Alert when crash rate > 10/min" | Possible but laggy | ✅ Excellent |
| Long-term retention (months+) | Expensive | Cheap |

---

## Recommendation: Logs First, Metrics if Needed

**For this project at its current stage: implement logs per ADD_LOGS.md first, skip metrics.**

Reasons:

1. **Event volume is low.** An arcade machine generates tens of events per game, a few games per
   hour. Loki can aggregate this effortlessly — the JSON parsing overhead is negligible at this
   scale.

2. **Logs give you flexibility.** With metrics you define instruments upfront; with logs you can
   query attributes you didn't think of at build time. You might want to ask "how many crashes at
   the split where stage 2 goes left vs right?" — trivial in LogQL, impossible in metrics unless
   you pre-labelled it.

3. **Third signal = third dependency.** The OTel Metrics SDK is separate from the Traces and Logs
   SDKs. Adding it means more CMakeLists.txt changes, more `#include`s, more shutdown paths, and
   more concepts to keep in sync.

4. **Logs + Traces already cover the dashboard use cases.** The LogQL queries at the bottom of
   ADD_LOGS.md already show crash heatmaps, session tables, and score distributions. Grafana's
   Loki data source supports all the panel types you'd want.

**Revisit metrics if any of these become true:**
- The machine runs for months and Loki storage costs become a concern
- You want sub-second dashboard load times on aggregate panels
- You want Prometheus-style alerting (e.g., PagerDuty when the machine goes offline)
- You add multiple machines and want cross-instance rollup without LogQL joins

---

## What Metrics Would Look Like (If Added)

If you do add metrics, here is the complete design.

### Instrument Mapping

The OTel Metrics SDK provides three instrument types relevant here:

| Instrument | Behaviour | Use for |
|------------|-----------|---------|
| `Counter` | monotonically increasing | totals: games played, crashes, coins |
| `Histogram` | samples a distribution | scores, speeds, durations |
| `Gauge` | current snapshot | credits in machine |

### Proposed Instruments

#### Counters

```cpp
// Total games played (labels: game_mode, host)
cannonball_games_total{game_mode="original", host="arcade-pi"}

// Games by completion outcome (labels: completion_status, host)
cannonball_games_completed_total{completion_status="completed", host="arcade-pi"}

// Crashes (labels: crash_type, stage_number, host)
cannonball_crashes_total{crash_type="flip", stage_number="3", host="arcade-pi"}

// Off-road incidents (labels: stage_number, host)
cannonball_offroads_total{stage_number="2", host="arcade-pi"}

// Route choices (labels: direction, stage, host)
cannonball_route_choices_total{direction="left", stage="1", host="arcade-pi"}

// Coins inserted (labels: host)
cannonball_coins_total{host="arcade-pi"}

// Vehicles overtaken (labels: vehicle_type, host)
cannonball_overtakes_total{vehicle_type="car", host="arcade-pi"}

// High scores recorded (labels: host)
cannonball_highscores_total{host="arcade-pi"}
```

#### Histograms

```cpp
// Final score distribution (bucket boundaries TBD based on actual score range)
cannonball_session_final_score{host="arcade-pi"}

// Session duration in seconds
cannonball_session_duration_seconds{host="arcade-pi"}

// Speed at crash (kph)
cannonball_crash_speed_kph{crash_type="flip", stage_number="3", host="arcade-pi"}

// Time remaining when completing a stage
cannonball_stage_time_remaining_seconds{stage_number="2", host="arcade-pi"}
```

#### Gauge

```cpp
// Current credit count (changes with coin inserts and game starts)
cannonball_credits{host="arcade-pi"}
```

### SDK Integration Sketch

```cpp
// In telemetry.hpp - new members on TelemetryImpl
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter.h"

struct TelemetryImpl {
    // ... existing trace + log members ...

    nostd::shared_ptr<metrics_api::MeterProvider>  meter_provider;
    nostd::shared_ptr<metrics_api::Meter>           meter;
    nostd::unique_ptr<metrics_api::Counter<uint64_t>> games_counter;
    nostd::unique_ptr<metrics_api::Counter<uint64_t>> crashes_counter;
    nostd::unique_ptr<metrics_api::Histogram<double>> score_histogram;
    // etc.
};
```

```cmake
# Additional CMakeLists.txt dependencies
opentelemetry-cpp::metrics
opentelemetry-cpp::otlp_http_metric_exporter
```

The metrics endpoint is derived the same way as logs:
- `/v1/traces` → `/v1/metrics` (same hostname, same auth)

Instrument creation happens once in `init()`. Observations happen at the existing call sites,
alongside the `log_game_event()` calls from ADD_LOGS.md:

```cpp
// Example: end_game_session()
void TelemetryManager::end_game_session(...) {
    // existing span/log code ...

    // Add metric observations
    std::map<std::string, std::string> labels = {
        {"completion_status", completion_status},
        {"host", hostname_}
    };
    impl_->games_counter->Add(1, labels);
    impl_->score_histogram->Record(final_score_decimal, labels);
}
```

### Exemplars: Linking Metrics → Logs → Traces

The OTel Metrics SDK supports **exemplars** — a sampled `trace_id`/`span_id` attached to a
histogram bucket observation. This closes the loop:

- Grafana shows a spike in `cannonball_crash_speed_kph`
- Click the bucket → exemplar gives you a `trace_id`
- Jump directly to that game session in Tempo
- From the trace, correlate to Loki logs via `trace_id`

Exemplars are automatically injected by the OTel SDK when a trace context is active (which it
always will be during a game session).

---

## Dashboard Strategy: Logs-Only vs Logs + Metrics

### Logs-Only Dashboard (Recommended Now)

All panels use the Loki data source with LogQL:

| Panel | Type | LogQL |
|-------|------|-------|
| Games played (last 7d) | Stat | `count_over_time({...} \| json \| event="game.session.start" [7d])` |
| Crash rate over time | Time series | `rate({...} \| json \| event="game.crash" [5m])` |
| Crashes by stage | Bar chart | `sum by (stage_number) (count_over_time({...} \| json \| event="game.crash" [24h]))` |
| Score distribution | Histogram | `{...} \| json \| event="game.session.end" \| unwrap final_score` |
| Completion rate | Gauge | ratio of `completion_status="completed"` to total sessions |
| Recent events | Table | `{...} \| json` ordered by time |

All of these work today once logs are implemented.

### Logs + Metrics Dashboard (Future Option)

Aggregate panels switch to Mimir (PromQL) for speed; detail panels stay on Loki:

| Panel | Source | Query |
|-------|--------|-------|
| Games played | Mimir | `increase(cannonball_games_total[7d])` |
| Crash rate | Mimir | `rate(cannonball_crashes_total[5m])` |
| Crashes by stage | Mimir | `sum by (stage_number) (cannonball_crashes_total)` |
| Score histogram | Mimir | `histogram_quantile(0.9, cannonball_session_final_score_bucket)` |
| Individual event drill-down | Loki | LogQL as above |
| Session trace detail | Tempo | click trace_id from Loki |

---

## Multi-Instance Considerations

Both approaches handle multiple machines, but differently:

**With logs only**: Loki stream selectors include `host_name` as a label (set in resource
attributes at init time). LogQL can filter per machine or aggregate with `sum by (host_name)`.

**With metrics**: The `host` label on every instrument naturally rolls up. PromQL
`sum(cannonball_crashes_total)` gives global totals; `sum by (host)(cannonball_crashes_total)`
breaks it out per machine.

For the dashboard, the pattern "show aggregate, click to drill into one machine" is slightly more
natural in Grafana with metrics variables + PromQL, but it is achievable with Loki label filters
too.

---

## Summary

| | Traces | Logs | Metrics |
|-|--------|------|---------|
| Status | ✅ Done | 📋 ADD_LOGS.md | ❓ Optional |
| Backend | Tempo | Loki | Mimir |
| Best for | Session narrative, waterfall | Per-event detail, ad-hoc queries | Aggregate time-series, alerting |
| Dashboard use | Session drilldown | Everything in the near term | Faster aggregates long-term |
| Implementation cost | — | Medium (ADD_LOGS.md) | Medium-High (additional SDK) |
| **Recommendation** | Keep | **Implement next** | Defer — revisit after logs are live |

Implement logs per ADD_LOGS.md. Run the game for a few weeks with that in place, observe which
dashboard queries feel slow or awkward, and use that evidence to decide whether to add the Metrics
SDK. The design above is ready to implement when that decision is made.
