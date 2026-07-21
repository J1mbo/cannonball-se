# OpenTelemetry Logging Implementation Plan

## Current State

The codebase currently uses **OpenTelemetry traces/spans/events** for game telemetry:

- **Trace Hierarchy**: `game_session` → `stage_N` spans → events
- **Exporter**: OTLP HTTP to configurable endpoint (supports Grafana Cloud authentication)
- **Events Tracked**:
  - `coin_inserted` (orphan event, no parent span)
  - `high_score` (player position, initials, score)
  - `route_chosen` (left/right, stage, speed, score)
  - `crash` (crash_type: bump/spin/flip, speed, score)
  - `off_road` (speed, score)
  - `vehicle_overtake` (vehicle type/name/palette, speed, score)
  - `stage_started` (stage number, speed, score)
- **Span Attributes**: game_mode, music_selection, player_initials, final_score, completion_status, final_stage, time_remaining_seconds, score_start/end

### Limitations with Traces for Dashboard Queries

❌ **Cannot** display trace events as table rows in Grafana dashboards
❌ **Limited** aggregation (only metrics queries like `quantile`, `rate`)
❌ **Cannot** filter by event attributes in TraceQL for dashboards
✅ **Good** for request-flow context and session narrative

## Objectives

Add **structured OpenTelemetry logs** alongside traces to enable:

1. ✅ **Table visualization** of game events in Grafana dashboards
2. ✅ **Aggregate queries** using LogQL (count, avg, sum over time)
3. ✅ **Filtering** by any attribute (stage, crash_type, speed_kph > 100, etc.)
4. ✅ **Alerting** on patterns (e.g., crash rate exceeds threshold)
5. ✅ **Correlation** between logs and traces via `trace_id` and `span_id`

**Maintain dual approach**: Keep traces for session context, add logs for queryability.

---

## Technical Approach

### 1. Add OpenTelemetry Logs SDK

**Dependencies** (add to CMakeLists.txt):
```cmake
# Existing
opentelemetry-cpp::api
opentelemetry-cpp::sdk
opentelemetry-cpp::otlp_http_exporter

# Add logs support
opentelemetry-cpp::logs
opentelemetry-cpp::otlp_http_log_exporter
```

**Headers** (add to telemetry.cpp):
```cpp
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_exporter.h"
#include "opentelemetry/logs/provider.h"
```

### 2. Initialize Logs Provider

In `TelemetryManager::init()`:
- Create OTLP HTTP log exporter (same endpoint pattern: `otlp/v1/logs`)
- Create BatchLogRecordProcessor
- Create LoggerProvider with same resource attributes
- Set as global logs provider
- Get logger instance: `"cannonball-se"` with version `"1.0.0"`

### 3. Extend TelemetryImpl Structure

```cpp
struct TelemetryImpl {
    // Existing trace components
    opentelemetry::nostd::shared_ptr<opentelemetry::sdk::trace::TracerProvider> provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> game_session_span;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> stage_span;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> post_game_span;

    // Add logs components
    opentelemetry::nostd::shared_ptr<opentelemetry::sdk::logs::LoggerProvider> log_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> logger;

    // Store player_initials for attaching to all logs in a session
    std::string current_player_initials;
};
```

**Note**: Store `player_initials` in TelemetryImpl when `start_game_session()` is called, then automatically attach it to all subsequent logs during that session. This enables player-based filtering without passing initials to every log call.

### 4. Create Logging Methods

Add to `TelemetryManager` class:

```cpp
public:
    // Emit structured log with trace correlation
    void log_game_event(
        const std::string& event_name,
        opentelemetry::logs::Severity severity,
        const std::map<std::string, std::string>& string_attrs = {},
        const std::map<std::string, int64_t>& int_attrs = {},
        const std::map<std::string, double>& double_attrs = {}
    );

private:
    // Helper to attach trace context (trace_id, span_id) to log
    void attach_trace_context_to_log(opentelemetry::logs::LogRecord& log_record);
```

**Implementation Strategy**:
- Extract `trace_id` and `span_id` from current active span (if any)
- Add as log attributes to enable trace ↔ log correlation
- Emit log with JSON body containing all attributes
- Use severity levels: `INFO` (normal events), `WARN` (crashes/off-road), `ERROR` (system errors)

---

## Game Events to Log

### Event Catalog

Each event below should be logged with structured attributes:

| Event Name | Severity | String Attributes | Int64 Attributes | Double Attributes | Notes |
|------------|----------|-------------------|------------------|-------------------|-------|
| `game.session.start` | INFO | `game_mode`, `player_initials` | `music_selection` | - | When game starts |
| `game.session.end` | INFO | `completion_status`, `screenshot_jpg` | `final_score`, `final_stage`, `longest_clean_seconds` | - | Game over (`longest_clean_seconds` = longest crash/off-road-free stretch, wall-clock; logged before the session span ends so it retains `trace_id`) |
| `game.stage.start` | INFO | - | `stage_number`, `score_start`, `speed_kph`, `stage_id` | - | Stage begins (`stage_id` = canonical section id / `stage_lookup_off`, identifies map branch) |
| `game.stage.end` | INFO | - | `stage_number`, `score_end`, `time_remaining_seconds`, `score_delta` | - | Stage completes |
| `game.post_game.start` | INFO | - | - | - | High score entry |
| `game.post_game.end` | INFO | - | - | - | Post-game complete |
| `game.coin_inserted` | INFO | - | `credits` | - | Coin inserted |
| `game.high_score` | INFO | `position`, `initials` | `score` | - | High score achieved |
| `game.route_chosen` | INFO | `direction` | `stage`, `speed_kph`, `score` | - | Fork decision |
| `game.crash` | WARN | `crash_type` | `speed_kph`, `score`, `stage_number` | - | bump/spin/flip |
| `game.off_road` | WARN | - | `speed_kph`, `score`, `stage_number` | - | Wheels off track |
| `game.vehicle_overtake` | INFO | `vehicle_type`, `vehicle`, `palette` | `speed_kph`, `score`, `stage_number` | - | Traffic overtake |

**Additional Computed Attributes** (automatically added to all logs):
- `trace_id` (hex string from span context - **also serves as session_id**)
- `span_id` (hex string from span context)
- `player_initials` (stored at session start, attached to all in-game logs)
- `service.name` = `"cannonball-se"` (from resource)
- `host.name` (from resource)
- `timestamp` (automatic from OTel)

**Note**: The `trace_id` from the `game_session` span uniquely identifies each game session, eliminating the need for a separate session_id. Combined with `player_initials`, you can query both by player (all sessions) and by session (all events in one game).

### Severity Guidelines

- **INFO**: Normal gameplay events (starts, ends, overtakes, route choices)
- **WARN**: Player errors (crashes, off-road)
- **ERROR**: System/telemetry errors (initialization failures)

---

## Implementation Steps

### Step 1: Update CMakeLists.txt

```cmake
# In cmake/CMakeLists.txt after line 312
find_package(opentelemetry-cpp REQUIRED COMPONENTS api sdk otlp_http_exporter logs otlp_http_log_exporter)

# Update target_link_libraries around line 342
target_link_libraries(cannonball-se PRIVATE
    # ... existing libs ...
    opentelemetry-cpp::logs
    opentelemetry-cpp::otlp_http_log_exporter
)
```

### Step 2: Update telemetry.hpp

```cpp
// Add new public method after line 48
void log_game_event(
    const std::string& event_name,
    int severity_number,  // Use int to avoid exposing OTel types in header
    const std::map<std::string, std::string>& string_attrs = {},
    const std::map<std::string, int64_t>& int_attrs = {},
    const std::map<std::string, double>& double_attrs = {}
);

// Add severity constants
enum Severity {
    SEV_INFO = 9,   // INFO level
    SEV_WARN = 13,  // WARN level
    SEV_ERROR = 17  // ERROR level
};

private:
// Add helper method
void attach_trace_context(
    const std::map<std::string, std::string>& string_attrs,
    const std::map<std::string, int64_t>& int_attrs,
    std::map<std::string, std::string>& out_strings,
    std::map<std::string, std::string>& out_trace_attrs
);
```

### Step 3: Update telemetry.cpp

#### 3.1 Add Includes (after line 21)
```cpp
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_exporter.h"
#include "opentelemetry/logs/provider.h"
```

#### 3.2 Update TelemetryImpl (line 24)
```cpp
struct TelemetryImpl {
    // Existing fields...

    // Add:
    opentelemetry::nostd::shared_ptr<opentelemetry::sdk::logs::LoggerProvider> log_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> logger;
};
```

#### 3.3 Extend init() Method (around line 145)

After tracer initialization, add logs initialization:

```cpp
// Create logs exporter (change endpoint from /traces to /logs)
opentelemetry::exporter::otlp::OtlpHttpLogExporterOptions log_exporter_opts;
std::string logs_endpoint = otlp_endpoint;
// Replace /v1/traces with /v1/logs
size_t pos = logs_endpoint.find("/v1/traces");
if (pos != std::string::npos) {
    logs_endpoint.replace(pos, 10, "/v1/logs");
}
log_exporter_opts.url = logs_endpoint;

// Copy auth headers
if (!auth_token.empty() && !instance_id.empty()) {
    std::string credentials = instance_id + ":" + auth_token;
    std::string encoded = base64_encode(credentials);
    log_exporter_opts.http_headers.insert({"Authorization", "Basic " + encoded});
}

// Create log exporter and processor
std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> log_exporter(
    new opentelemetry::exporter::otlp::OtlpHttpLogExporter(log_exporter_opts));
auto log_processor = std::make_unique<opentelemetry::sdk::logs::BatchLogRecordProcessor>(
    std::move(log_exporter));

// Create logger provider
impl_->log_provider = opentelemetry::nostd::shared_ptr<opentelemetry::sdk::logs::LoggerProvider>(
    new opentelemetry::sdk::logs::LoggerProvider(std::move(log_processor), resource_ptr));

// Set as global provider
auto log_provider_copy = impl_->log_provider;
opentelemetry::logs::Provider::SetLoggerProvider(std::move(log_provider_copy));

// Get logger
impl_->logger = impl_->log_provider->GetLogger("cannonball-se", "1.0.0");

if (debug) {
    std::cout << "Logs endpoint: " << logs_endpoint << std::endl;
}
```

#### 3.4 Implement log_game_event() Method

```cpp
void TelemetryManager::log_game_event(
    const std::string& event_name,
    int severity_number,
    const std::map<std::string, std::string>& string_attrs,
    const std::map<std::string, int64_t>& int_attrs,
    const std::map<std::string, double>& double_attrs)
{
    if (!initialized_ || !impl_->logger) return;

    try {
        // Get current span context for trace correlation
        std::string trace_id_hex;
        std::string span_id_hex;

        auto current_span = impl_->post_game_span ? impl_->post_game_span :
                           (impl_->stage_span ? impl_->stage_span : impl_->game_session_span);

        if (current_span) {
            auto ctx = current_span->GetContext();
            if (ctx.IsValid()) {
                char trace_buf[33] = {0};
                ctx.trace_id().ToLowerBase16(trace_buf);
                trace_id_hex = trace_buf;

                char span_buf[17] = {0};
                ctx.span_id().ToLowerBase16(span_buf);
                span_id_hex = span_buf;
            }
        }

        // Build log attributes
        std::vector<std::pair<std::string, opentelemetry::common::AttributeValue>> attrs;

        // Add trace correlation
        if (!trace_id_hex.empty()) {
            attrs.push_back({"trace_id", trace_id_hex});
            attrs.push_back({"span_id", span_id_hex});
        }

        // Add event name as attribute
        attrs.push_back({"event", event_name});

        // Add player_initials if available (for player-based filtering)
        if (!impl_->current_player_initials.empty()) {
            attrs.push_back({"player_initials", impl_->current_player_initials});
        }

        // Add all user attributes
        for (const auto& kv : string_attrs) {
            attrs.push_back({kv.first, kv.second});
        }
        for (const auto& kv : int_attrs) {
            attrs.push_back({kv.first, kv.second});
        }
        for (const auto& kv : double_attrs) {
            attrs.push_back({kv.first, kv.second});
        }

        // Emit log
        auto log_record = impl_->logger->CreateLogRecord();
        log_record->SetSeverity(static_cast<opentelemetry::logs::Severity>(severity_number));
        log_record->SetBody(event_name);

        for (const auto& attr : attrs) {
            log_record->SetAttribute(attr.first, attr.second);
        }

        impl_->logger->EmitLogRecord(std::move(log_record));

    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error emitting log: " << e.what() << std::endl;
    }
}
```

#### 3.5 Store Player Initials for Session

In `start_game_session()` (around line 171):
```cpp
// At the start of the method, after cleanup():
impl_->current_player_initials = player_initials;
```

In `cleanup()` (around line 353):
```cpp
// At the end of cleanup, after ending all spans:
impl_->current_player_initials.clear();
```

#### 3.6 Update shutdown() Method (around line 161)

```cpp
if (impl_->log_provider) {
    impl_->log_provider->ForceFlush();
    impl_->log_provider->Shutdown();
}
```

### Step 4: Update Game Event Call Sites

Replace or supplement `add_event()` calls with `log_game_event()`:

#### 4.1 src/main/engine/outrun.cpp

**Line ~482** (game start):
```cpp
// Keep existing:
TelemetryManager::instance().start_game_session(mode, omusic.get_music_selected(), oname.get_initials());

// Add log:
TelemetryManager::instance().log_game_event("game.session.start",
    TelemetryManager::SEV_INFO,
    {
        {"game_mode", mode},
        {"player_initials", oname.get_initials()}
    },
    {
        {"music_selection", omusic.get_music_selected()}
    }
);
```

**Line ~635** (high score):
```cpp
// Keep existing event, add log:
TelemetryManager::instance().log_game_event("game.high_score",
    TelemetryManager::SEV_INFO,
    {
        {"position", std::to_string(pos + 1)},
        {"initials", initials}
    },
    {
        {"score", TelemetryManager::bcd_score_to_decimal(ohiscore.scores[pos].score)}
    }
);
```

**Line ~658** (game end):
```cpp
// Keep existing:
TelemetryManager::instance().end_game_session(final_score, completion, final_stage);

// Add log:
TelemetryManager::instance().log_game_event("game.session.end",
    TelemetryManager::SEV_INFO,
    {
        {"completion_status", completion}
    },
    {
        {"final_score", final_score},
        {"final_stage", final_stage}
    }
);
```

#### 4.2 src/main/engine/oinitengine.cpp

**Line ~669** (route chosen):
```cpp
TelemetryManager::instance().log_game_event("game.route_chosen",
    TelemetryManager::SEV_INFO,
    {
        {"direction", route_selected == 0 ? "right" : "left"}
    },
    {
        {"stage", ostats.routes[0]},
        {"speed_kph", car_increment >> 16},
        {"score", TelemetryManager::bcd_score_to_decimal(ostats.score)}
    }
);
```

**Line ~799** (stage end) + **Line ~801** (stage start):
```cpp
// Stage end
TelemetryManager::instance().log_game_event("game.stage.end",
    TelemetryManager::SEV_INFO,
    {},
    {
        {"stage_number", ostats.cur_stage},
        {"score_end", TelemetryManager::bcd_score_to_decimal(ostats.score)},
        {"time_remaining_seconds", time_remaining}
    }
);

// Stage start
TelemetryManager::instance().log_game_event("game.stage.start",
    TelemetryManager::SEV_INFO,
    {},
    {
        {"stage_number", ostats.cur_stage + 1},
        {"score_start", TelemetryManager::bcd_score_to_decimal(ostats.score)},
        {"speed_kph", car_increment >> 16}
    }
);
```

#### 4.3 src/main/engine/ocrash.cpp

**Lines ~945, ~984, ~1055** (crashes):
```cpp
// Bump
TelemetryManager::instance().log_game_event("game.crash",
    TelemetryManager::SEV_WARN,
    {
        {"crash_type", "bump"}
    },
    {
        {"speed_kph", oinitengine.car_increment >> 16},
        {"score", TelemetryManager::bcd_score_to_decimal(ostats.score)},
        {"stage_number", ostats.cur_stage + 1}
    }
);

// Spin and Flip: same pattern, change crash_type value
```

#### 4.4 src/main/engine/oferrari.cpp

**Line ~1684** (off road):
```cpp
TelemetryManager::instance().log_game_event("game.off_road",
    TelemetryManager::SEV_WARN,
    {},
    {
        {"speed_kph", oinitengine.car_increment >> 16},
        {"score", TelemetryManager::bcd_score_to_decimal(ostats.score)},
        {"stage_number", ostats.cur_stage + 1}
    }
);
```

#### 4.5 src/main/engine/otraffic.cpp

**Line ~473** (overtake):
```cpp
TelemetryManager::instance().log_game_event("game.vehicle_overtake",
    TelemetryManager::SEV_INFO,
    {
        {"vehicle_type", std::to_string(sprite_type)},
        {"vehicle", vehicle_name},
        {"palette", std::to_string(sprite->pal_src)}
    },
    {
        {"speed_kph", oinitengine.car_increment >> 16},
        {"score", TelemetryManager::bcd_score_to_decimal(ostats.score)},
        {"stage_number", ostats.cur_stage + 1}
    }
);
```

#### 4.6 src/main/engine/oinputs.cpp

**Line ~262** (coin insert):
```cpp
TelemetryManager::instance().log_game_event("game.coin_inserted",
    TelemetryManager::SEV_INFO,
    {},
    {
        {"credits", ostats.credits}
    }
);
```

#### 4.7 src/main/engine/ostats.cpp

**Line ~86** (stage started event - may be redundant with start_stage_span):
```cpp
// Consider if still needed - may replace with log_game_event("game.stage.start")
```

---

## Configuration Updates

### config.xml

**No changes needed!** The code automatically derives the logs endpoint from the configured traces endpoint.

**How it works**:
- **Configured**: `https://otlp-gateway-prod-<region>.grafana.net/otlp/v1/traces`
- **Auto-derived**: `https://otlp-gateway-prod-<region>.grafana.net/otlp/v1/logs`
  - Same hostname/gateway
  - Same authentication (Basic auth with instance_id:token)
  - Different path (replaces `/v1/traces` with `/v1/logs`)

**Grafana Cloud OTLP Gateway routing**:
- Acts as a smart router for all OpenTelemetry signals
- Routes `/v1/traces` → **Tempo** (trace storage backend)
- Routes `/v1/logs` → **Loki** (log storage backend)
- Routes `/v1/metrics` → **Mimir** (metrics storage backend)
- All signals use the same authentication and are correlated via `trace_id`

**Result**: Traces appear in Tempo, logs appear in Loki, and they're linked via `trace_id` for seamless correlation in Grafana dashboards.

---

## Testing Strategy

### 1. Local Testing with OTLP Collector

```bash
# Run local OpenTelemetry Collector
docker run -p 4318:4318 otel/opentelemetry-collector-contrib:latest \
  --config /path/to/otel-collector-config.yaml
```

**Collector config** (`otel-collector-config.yaml`):
```yaml
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318

exporters:
  logging:
    loglevel: debug
  otlp:
    endpoint: https://otlp-gateway-prod-<region>.grafana.net/otlp
    headers:
      authorization: Basic <base64(instance_id:token)>

service:
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [logging, otlp]
    logs:
      receivers: [otlp]
      exporters: [logging, otlp]
```

Update `config.xml`:
```xml
<otlp_endpoint>http://localhost:4318/v1/traces</otlp_endpoint>
```

### 2. Verify Log Structure

Expected JSON log format in Loki (session start):
```json
{
  "timestamp": "2026-02-09T12:34:56.789Z",
  "severity": "INFO",
  "body": "game.session.start",
  "trace_id": "1234567890abcdef1234567890abcdef",
  "span_id": "1234567890abcdef",
  "service.name": "cannonball-se",
  "host.name": "arcade-pi",
  "event": "game.session.start",
  "game_mode": "original",
  "player_initials": "AAA",
  "music_selection": 1
}
```

Expected JSON log format (crash event - note player_initials automatically included):
```json
{
  "timestamp": "2026-02-09T12:35:23.456Z",
  "severity": "WARN",
  "body": "game.crash",
  "trace_id": "1234567890abcdef1234567890abcdef",
  "span_id": "fedcba0987654321",
  "service.name": "cannonball-se",
  "host.name": "arcade-pi",
  "event": "game.crash",
  "player_initials": "AAA",
  "crash_type": "flip",
  "speed_kph": 245,
  "score": 125000,
  "stage_number": 3
}
```

### 3. Grafana Loki Queries

**Example LogQL queries**:

```logql
# All game events
{service_name="cannonball-se"}

# Crashes only
{service_name="cannonball-se"} | json | event="game.crash"

# High-speed crashes
{service_name="cannonball-se"} | json | event="game.crash" | speed_kph > 100

# Crash rate over time
rate({service_name="cannonball-se"} | json | event="game.crash" [5m])

# Average speed at crash
avg_over_time({service_name="cannonball-se"} | json | event="game.crash" | unwrap speed_kph [1h])

# Events by stage
{service_name="cannonball-se"} | json | stage_number > 0

# Link to trace (click trace_id in table)
{service_name="cannonball-se"} | json | trace_id="..."
```

**Dashboard panels**:
- Table: Recent game events with all attributes
- Time series: Event counts by type
- Stat: Total games, completion rate
- Gauge: Average session duration
- Bar chart: Crashes by stage

---

## Expected Output Format

### Log Record Structure

Each log will have:

**Standard Fields**:
- `timestamp` (ISO8601)
- `severity` (INFO/WARN/ERROR)
- `body` (event name)

**Resource Attributes**:
- `service.name` = `"cannonball-se"`
- `host.name` (hostname)

**Log Attributes**:
- `trace_id` (32-char hex, links to trace **and serves as session_id**)
- `span_id` (16-char hex, links to span)
- `event` (event name, e.g., `"game.crash"`)
- `player_initials` (included in all in-game logs for player identification)
- All game-specific attributes (stage_number, speed_kph, crash_type, etc.)

---

## Rollout Plan

1. **Phase 1**: Implement logs infrastructure
   - Update CMakeLists.txt
   - Extend telemetry.hpp/cpp
   - Test with local collector

2. **Phase 2**: Add logs to high-priority events
   - Game session start/end
   - Stage start/end
   - Crashes

3. **Phase 3**: Add logs to remaining events
   - Overtakes
   - Off-road
   - Route choices
   - Coins
   - High scores

4. **Phase 4**: Dashboard creation
   - Create Grafana dashboard with LogQL queries
   - Set up alerts (crash rate, session failures)

5. **Phase 5**: Validation
   - Run test sessions
   - Verify log-trace correlation
   - Tune batch processor settings if needed

---

## Performance Considerations

- **Batch Processor**: Uses background thread, default 5s interval (same as traces)
- **Overhead**: ~100-200μs per log emission (negligible for game events)
- **Network**: Logs batched and compressed (gzip) before sending
- **Fallback**: If logs initialization fails, game continues normally (telemetry is non-blocking)

---

## Maintenance Notes

### Adding New Events

To add a new game event log:

1. Choose event name: `game.<category>.<action>` (e.g., `game.bonus.awarded`)
2. Choose severity: INFO (normal), WARN (error state), ERROR (system fault)
3. Add call to `log_game_event()` at appropriate location
4. Include relevant attributes (stage_number, score, speed_kph as applicable)
5. Update this document's event catalog

### Debugging

Enable debug mode in `config.xml`:
```xml
<debug>1</debug>
```

This will print:
- Logs endpoint URL
- Initialization status
- Any export errors

---

## Example Log Queries for Dashboard

### Game Sessions Table
```logql
{service_name="cannonball-se"}
| json
| event=~"game.session.start|game.session.end"
| line_format "{{.timestamp}} {{.event}} {{.game_mode}} {{.player_initials}} score={{.final_score}}"
```

### Crash Heatmap by Stage
```logql
sum by (stage_number, crash_type) (
  count_over_time({service_name="cannonball-se"} | json | event="game.crash" [1h])
)
```

### Player Performance (all sessions for a player)
```logql
{service_name="cannonball-se"}
| json
| player_initials="AAA"
| event="game.session.end"
```

### Session Aggregation (all events in one game session)
```logql
{service_name="cannonball-se"}
| json
| trace_id="1234567890abcdef1234567890abcdef"
```

### Player-Specific Crashes
```logql
{service_name="cannonball-se"}
| json
| player_initials="AAA"
| event="game.crash"
| line_format "{{.timestamp}} Stage {{.stage_number}}: {{.crash_type}} at {{.speed_kph}}kph"
```

### Speed Distribution at Crashes
```logql
{service_name="cannonball-se"}
| json
| event="game.crash"
| unwrap speed_kph
```

---

## Success Criteria

- ✅ All game events emitted as structured logs
- ✅ Logs include `trace_id` and `span_id` for correlation
- ✅ Logs queryable in Grafana Loki with LogQL
- ✅ Dashboard created showing game statistics
- ✅ No performance degradation (< 1ms overhead per event)
- ✅ Existing trace functionality unchanged
- ✅ Graceful degradation if logs fail to initialize

---

## Future Enhancements

- Add `frame_time_ms` attribute for performance monitoring
- Add `player_level` or `difficulty` attributes if exposed
- Consider metrics SDK for counters/histograms (complementary to logs)
- Add exemplars to link metrics → logs → traces
