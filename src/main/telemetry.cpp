/***************************************************************************
    OpenTelemetry Telemetry Manager Implementation

    See license.txt for more details.
***************************************************************************/

#include "telemetry.hpp"
#include <iostream>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>

#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/tracer.h"
#include "opentelemetry/trace/span.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/trace/provider.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter.h"
#include "opentelemetry/logs/provider.h"

// Pimpl implementation - contains the actual OpenTelemetry objects
struct TelemetryImpl {
    opentelemetry::nostd::shared_ptr<opentelemetry::sdk::trace::TracerProvider> provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> game_session_span;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> stage_span;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> post_game_span;
    opentelemetry::nostd::shared_ptr<opentelemetry::sdk::logs::LoggerProvider> log_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::logs::Logger> logger;
    std::string current_player_initials;
    // Clean-driving streak tracking (wall-clock). Reset at session start, updated on each
    // crash/off-road, folded with the tail when read at session end.
    std::chrono::steady_clock::time_point last_incident_time{};
    int64_t longest_clean_seconds = 0;
};

// Base64 encoding helper
static std::string base64_encode(const std::string& input) {
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    int val = 0;
    int valb = -6;

    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }

    if (valb > -6) {
        result.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }

    while (result.size() % 4) {
        result.push_back('=');
    }

    return result;
}

TelemetryManager& TelemetryManager::instance() {
    static TelemetryManager instance;
    return instance;
}

TelemetryManager::TelemetryManager() : impl_(std::make_unique<TelemetryImpl>()), initialized_(false) {}

TelemetryManager::~TelemetryManager() {
    cleanup();
}

void TelemetryManager::init(const std::string& otlp_endpoint, const std::string& instance_id, const std::string& auth_token, bool debug) {
    if (initialized_) {
        std::cerr << "TelemetryManager: Already initialized" << std::endl;
        return;
    }

    try {
        if (debug) {
            std::cout << "\n=== Telemetry Initialization ===" << std::endl;
            std::cout << "Endpoint: " << otlp_endpoint << std::endl;
        }

        // Get hostname for resource attributes
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            strncpy(hostname, "unknown", sizeof(hostname));
        }

        if (debug) {
            std::cout << "Hostname: " << hostname << std::endl;
        }

        // Create resource attributes
        auto resource_attributes = opentelemetry::sdk::resource::ResourceAttributes{
            {"service.name", "cannonball-se"},
            {"host.name", std::string(hostname)}
        };
        auto resource_ptr = opentelemetry::sdk::resource::Resource::Create(resource_attributes);

        // Configure OTLP HTTP exporter
        opentelemetry::exporter::otlp::OtlpHttpExporterOptions exporter_opts;
        exporter_opts.url = otlp_endpoint;

        // Add authentication header if token is configured
        if (!auth_token.empty() && !instance_id.empty()) {
            // For Grafana Cloud: use Basic auth with base64(instance_id:token)
            std::string credentials = instance_id + ":" + auth_token;
            std::string encoded = base64_encode(credentials);
            exporter_opts.http_headers.insert({"Authorization", "Basic " + encoded});

            if (debug) {
                std::cout << "Auth configured: instance_id=" << instance_id << ", token_length=" << auth_token.length() << std::endl;
                std::cout << "Authorization header: Basic " << encoded << std::endl;
            }
        } else if (debug) {
            std::cout << "Auth token configured: NO" << std::endl;
        }
        // Cast to SpanExporter base class for BatchSpanProcessor
        std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter(
            new opentelemetry::exporter::otlp::OtlpHttpExporter(exporter_opts));

        // Create batch span processor (uses background thread, default 5s interval, 512 batch size)
        auto processor = std::make_unique<opentelemetry::sdk::trace::BatchSpanProcessor>(
            std::move(exporter), opentelemetry::sdk::trace::BatchSpanProcessorOptions());

        // Create tracer provider using nostd::shared_ptr
        impl_->provider = opentelemetry::nostd::shared_ptr<opentelemetry::sdk::trace::TracerProvider>(
            new opentelemetry::sdk::trace::TracerProvider(std::move(processor), resource_ptr));

        // Set as global provider (copy the shared_ptr to satisfy rvalue reference requirement)
        auto provider_copy = impl_->provider;
        opentelemetry::trace::Provider::SetTracerProvider(std::move(provider_copy));

        // Get tracer
        impl_->tracer = impl_->provider->GetTracer("cannonball-se", "1.0.0");

        // Initialize logs provider
        // Derive logs endpoint by replacing /v1/traces with /v1/logs
        std::string logs_endpoint = otlp_endpoint;
        size_t pos = logs_endpoint.find("/v1/traces");
        if (pos != std::string::npos) {
            logs_endpoint.replace(pos, 10, "/v1/logs");
        }

        opentelemetry::exporter::otlp::OtlpHttpLogRecordExporterOptions log_exporter_opts;
        log_exporter_opts.url = logs_endpoint;

        if (!auth_token.empty() && !instance_id.empty()) {
            std::string credentials = instance_id + ":" + auth_token;
            std::string encoded = base64_encode(credentials);
            log_exporter_opts.http_headers.insert({"Authorization", "Basic " + encoded});
        }

        std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> log_exporter(
            new opentelemetry::exporter::otlp::OtlpHttpLogRecordExporter(log_exporter_opts));
        auto log_processor = std::make_unique<opentelemetry::sdk::logs::BatchLogRecordProcessor>(
            std::move(log_exporter));

        impl_->log_provider = opentelemetry::nostd::shared_ptr<opentelemetry::sdk::logs::LoggerProvider>(
            new opentelemetry::sdk::logs::LoggerProvider(std::move(log_processor), resource_ptr));

        auto log_provider_copy = impl_->log_provider;
        opentelemetry::logs::Provider::SetLoggerProvider(std::move(log_provider_copy));

        impl_->logger = impl_->log_provider->GetLogger("cannonball-se", "1.0.0");

        initialized_ = true;

        if (debug) {
            std::cout << "Logs endpoint: " << logs_endpoint << std::endl;
            std::cout << "Telemetry initialization: SUCCESS" << std::endl;
            std::cout << "================================\n" << std::endl;
        } else {
            std::cout << "Telemetry initialized: " << otlp_endpoint << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Failed to initialize: " << e.what() << std::endl;
        std::cerr << "TelemetryManager: Continuing without telemetry" << std::endl;
    }
}

void TelemetryManager::shutdown() {
    if (!initialized_) return;

    try {
        cleanup();

        if (impl_->log_provider) {
            impl_->log_provider->ForceFlush();
            impl_->log_provider->Shutdown();
        }

        if (impl_->provider) {
            // Force flush to ensure all pending spans are exported
            impl_->provider->ForceFlush();
            impl_->provider->Shutdown();
        }

        std::cout << "Telemetry shutdown complete" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error during shutdown: " << e.what() << std::endl;
    }
}

void TelemetryManager::start_game_session(const std::string& game_mode, int music_selection, const std::string& player_initials) {
    if (!initialized_ || !impl_->tracer) return;

    try {
        // Clean up any existing session first
        cleanup();

        // Store player initials for attaching to all in-session logs
        impl_->current_player_initials = player_initials;

        // Reset clean-driving streak tracking for the new session
        impl_->last_incident_time = std::chrono::steady_clock::now();
        impl_->longest_clean_seconds = 0;

        // Start new game session span
        impl_->game_session_span = impl_->tracer->StartSpan("game_session");

        if (impl_->game_session_span) {
            impl_->game_session_span->SetAttribute("game_mode", game_mode);
            impl_->game_session_span->SetAttribute("music_selection", music_selection);
            impl_->game_session_span->SetAttribute("player_initials", player_initials);
        }
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error starting game session: " << e.what() << std::endl;
    }
}

void TelemetryManager::end_game_session(int64_t final_score, const std::string& completion_status, int final_stage) {
    if (!initialized_ || !impl_->game_session_span) return;

    try {
        // End current stage span if exists
        if (impl_->stage_span) {
            impl_->stage_span->End();
            impl_->stage_span = nullptr;
        }

        // Set final attributes
        impl_->game_session_span->SetAttribute("final_score", final_score);
        impl_->game_session_span->SetAttribute("completion_status", completion_status);
        impl_->game_session_span->SetAttribute("final_stage", final_stage);

        // End session
        impl_->game_session_span->End();
        impl_->game_session_span = nullptr;

    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error ending game session: " << e.what() << std::endl;
    }
}

int64_t TelemetryManager::get_longest_clean_seconds() const {
    if (!initialized_ || !impl_->game_session_span) return impl_->longest_clean_seconds;
    // Fold in the current (still-open) clean stretch: time since the last incident.
    auto now = std::chrono::steady_clock::now();
    int64_t tail = std::chrono::duration_cast<std::chrono::seconds>(
        now - impl_->last_incident_time).count();
    return std::max(impl_->longest_clean_seconds, tail);
}

void TelemetryManager::start_stage_span(int stage_num, int64_t score_start) {
    if (!initialized_ || !impl_->tracer || !impl_->game_session_span) return;

    try {
        // End previous stage span if exists
        if (impl_->stage_span) {
            impl_->stage_span->End();
            impl_->stage_span = nullptr;
        }

        // Create stage span name
        std::string span_name = "stage_" + std::to_string(stage_num);

        // Start new stage span as child of game session
        opentelemetry::trace::StartSpanOptions options;
        options.parent = impl_->game_session_span->GetContext();

        impl_->stage_span = impl_->tracer->StartSpan(span_name, options);

        if (impl_->stage_span) {
            impl_->stage_span->SetAttribute("stage_number", stage_num);
            impl_->stage_span->SetAttribute("score_start", score_start);
        }

    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error starting stage span: " << e.what() << std::endl;
    }
}

void TelemetryManager::end_stage_span(int time_remaining_seconds, int64_t score_end) {
    if (!initialized_ || !impl_->stage_span) return;

    try {
        impl_->stage_span->SetAttribute("time_remaining_seconds", time_remaining_seconds);
        impl_->stage_span->SetAttribute("score_end", score_end);
        impl_->stage_span->End();
        impl_->stage_span = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error ending stage span: " << e.what() << std::endl;
    }
}

void TelemetryManager::start_post_game_span() {
    if (!initialized_ || !impl_->game_session_span) return;

    try {
        // End any active stage span first
        if (impl_->stage_span) {
            impl_->stage_span->End();
            impl_->stage_span = nullptr;
        }

        // Start post-game span as child of game session
        opentelemetry::trace::StartSpanOptions options;
        options.parent = impl_->game_session_span->GetContext();

        impl_->post_game_span = impl_->tracer->StartSpan("post_game", options);

    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error starting post-game span: " << e.what() << std::endl;
    }
}

void TelemetryManager::end_post_game_span() {
    if (!initialized_ || !impl_->post_game_span) return;

    try {
        impl_->post_game_span->End();
        impl_->post_game_span = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error ending post-game span: " << e.what() << std::endl;
    }
}

void TelemetryManager::add_event(const std::string& name,
                                  const std::map<std::string, std::string>& string_attrs,
                                  const std::map<std::string, int64_t>& int_attrs) {
    if (!initialized_) return;

    try {
        // Priority: post_game_span > stage_span > game_session_span
        auto target_span = impl_->post_game_span ? impl_->post_game_span :
                          (impl_->stage_span ? impl_->stage_span : impl_->game_session_span);

        if (target_span) {
            // Build attributes vector
            std::vector<std::pair<std::string, opentelemetry::common::AttributeValue>> attr_vec;

            for (const auto& kv : string_attrs) {
                attr_vec.push_back({kv.first, kv.second});
            }
            for (const auto& kv : int_attrs) {
                attr_vec.push_back({kv.first, kv.second});
            }

            target_span->AddEvent(name, attr_vec);
        } else {
            // No active span - create orphan event via separate span
            add_orphan_event(name, string_attrs, int_attrs);
        }
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error adding event: " << e.what() << std::endl;
    }
}

void TelemetryManager::add_orphan_event(const std::string& name,
                                         const std::map<std::string, std::string>& string_attrs,
                                         const std::map<std::string, int64_t>& int_attrs) {
    if (!initialized_ || !impl_->tracer) return;

    try {
        // Create a short-lived span for the event
        auto span = impl_->tracer->StartSpan(name);

        if (span) {
            for (const auto& kv : string_attrs) {
                span->SetAttribute(kv.first, kv.second);
            }
            for (const auto& kv : int_attrs) {
                span->SetAttribute(kv.first, kv.second);
            }
            span->End();
        }
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error adding orphan event: " << e.what() << std::endl;
    }
}

void TelemetryManager::update_stage_attribute(const std::string& key, int64_t value) {
    if (!initialized_ || !impl_->stage_span) return;

    try {
        impl_->stage_span->SetAttribute(key, value);
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error updating attribute: " << e.what() << std::endl;
    }
}

void TelemetryManager::cleanup() {
    if (!initialized_) return;

    try {
        if (impl_->stage_span) {
            impl_->stage_span->End();
            impl_->stage_span = nullptr;
        }

        if (impl_->post_game_span) {
            impl_->post_game_span->End();
            impl_->post_game_span = nullptr;
        }

        if (impl_->game_session_span) {
            impl_->game_session_span->End();
            impl_->game_session_span = nullptr;
        }

        impl_->current_player_initials.clear();
    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error during cleanup: " << e.what() << std::endl;
    }
}

void TelemetryManager::log_game_event(
    const std::string& event_name,
    int severity_number,
    const std::map<std::string, std::string>& string_attrs,
    const std::map<std::string, int64_t>& int_attrs,
    const std::map<std::string, double>& double_attrs)
{
    if (!initialized_ || !impl_->logger) return;

    // Track clean-driving streaks: a crash or off-road resets the timer, recording the
    // stretch that just ended if it is the longest so far.
    if (impl_->game_session_span &&
        (event_name == "game.crash" || event_name == "game.off_road")) {
        auto now = std::chrono::steady_clock::now();
        int64_t clean = std::chrono::duration_cast<std::chrono::seconds>(
            now - impl_->last_incident_time).count();
        if (clean > impl_->longest_clean_seconds) impl_->longest_clean_seconds = clean;
        impl_->last_incident_time = now;
    }

    try {
        // Get trace/span IDs from the most specific active span
        std::string trace_id_hex;
        std::string span_id_hex;

        auto current_span = impl_->post_game_span ? impl_->post_game_span :
                           (impl_->stage_span ? impl_->stage_span : impl_->game_session_span);

        if (current_span) {
            auto ctx = current_span->GetContext();
            if (ctx.IsValid()) {
                char trace_buf[33] = {0};
                ctx.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(trace_buf, 32));
                trace_id_hex = trace_buf;

                char span_buf[17] = {0};
                ctx.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>(span_buf, 16));
                span_id_hex = span_buf;
            }
        }

        auto log_record = impl_->logger->CreateLogRecord();
        log_record->SetSeverity(static_cast<opentelemetry::logs::Severity>(severity_number));
        log_record->SetBody(event_name);

        log_record->SetAttribute("event", event_name);

        if (!trace_id_hex.empty()) {
            log_record->SetAttribute("trace_id", trace_id_hex);
            log_record->SetAttribute("span_id", span_id_hex);
        }

        if (!impl_->current_player_initials.empty()) {
            log_record->SetAttribute("player_initials", impl_->current_player_initials);
        }

        for (const auto& kv : string_attrs) {
            log_record->SetAttribute(kv.first, kv.second);
        }
        for (const auto& kv : int_attrs) {
            log_record->SetAttribute(kv.first, kv.second);
        }
        for (const auto& kv : double_attrs) {
            log_record->SetAttribute(kv.first, kv.second);
        }

        impl_->logger->EmitLogRecord(std::move(log_record));

    } catch (const std::exception& e) {
        std::cerr << "TelemetryManager: Error emitting log: " << e.what() << std::endl;
    }
}

int TelemetryManager::bcd_to_seconds(int16_t bcd_value) {
    // BCD format: high nibble = tens, low nibble = ones
    int tens = (bcd_value >> 4) & 0x0F;
    int ones = bcd_value & 0x0F;
    return tens * 10 + ones;
}

int64_t TelemetryManager::bcd_score_to_decimal(uint32_t bcd_score) {
    // Convert 8-digit BCD score to decimal
    // Each nibble (4 bits) is one decimal digit
    int64_t result = 0;
    for (int i = 0; i < 8; i++) {
        int digit = (bcd_score >> (i * 4)) & 0x0F;
        int64_t multiplier = 1;
        for (int j = 0; j < i; j++) {
            multiplier *= 10;
        }
        result += digit * multiplier;
    }
    return result;
}
