/***************************************************************************
    OpenTelemetry Telemetry Manager
    
    Singleton manager for OpenTelemetry tracing with nested span support.
    Tracks game sessions as root spans with child spans for each stage.
    
    See license.txt for more details.
***************************************************************************/

#pragma once

#include <string>
#include <memory>
#include <map>
#include <cstdint>

// Pimpl forward declaration to hide OpenTelemetry types
struct TelemetryImpl;

class TelemetryManager {
public:
    static TelemetryManager& instance();
    
    // Initialize telemetry with OTLP endpoint, instance ID, and optional auth token
    void init(const std::string& otlp_endpoint, const std::string& instance_id = "", const std::string& auth_token = "", bool debug = false);
    
    // Graceful shutdown - flushes pending spans
    void shutdown();
    
    // Game session management
    void start_game_session(const std::string& game_mode, int music_selection, const std::string& player_initials);
    void end_game_session(int64_t final_score, const std::string& completion_status, int final_stage);

    // Longest continuous clean-driving stretch for the current session (wall-clock seconds
    // with no crash or off-road). Folds in the time since the last incident, so it is correct
    // when read at session end. Emitted as an attribute on game.session.end.
    int64_t get_longest_clean_seconds() const;
    
    // Stage span management
    void start_stage_span(int stage_num, int64_t score_start);
    void end_stage_span(int time_remaining_seconds, int64_t score_end);
    
    // Post-game span management (for high score entry, etc.)
    void start_post_game_span();
    void end_post_game_span();
    
    // Add event to current stage span (or orphan if no span)
    void add_event(const std::string& name, const std::map<std::string, std::string>& string_attrs = {},
                   const std::map<std::string, int64_t>& int_attrs = {});

    // Emit structured log with trace correlation
    void log_game_event(
        const std::string& event_name,
        int severity_number,
        const std::map<std::string, std::string>& string_attrs = {},
        const std::map<std::string, int64_t>& int_attrs = {},
        const std::map<std::string, double>& double_attrs = {}
    );

    // Severity constants (map to OpenTelemetry severity numbers)
    enum Severity {
        SEV_INFO  = 9,   // INFO
        SEV_WARN  = 13,  // WARN
        SEV_ERROR = 17   // ERROR
    };
    
    // Add orphan event without parent context (e.g., coin inserts)
    void add_orphan_event(const std::string& name, const std::map<std::string, std::string>& string_attrs = {},
                          const std::map<std::string, int64_t>& int_attrs = {});
    
    // Update attribute on current stage span
    void update_stage_attribute(const std::string& key, int64_t value);
    
    // Clean up any active spans (for abandoned games)
    void cleanup();
    
    // Helper to convert BCD time to decimal seconds
    static int bcd_to_seconds(int16_t bcd_value);
    
    // Helper to convert BCD score to decimal
    static int64_t bcd_score_to_decimal(uint32_t bcd_score);
    
private:
    TelemetryManager();
    ~TelemetryManager();
    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;
    
    // Pimpl idiom - hides OpenTelemetry types from header
    std::unique_ptr<TelemetryImpl> impl_;
    bool initialized_;
};
