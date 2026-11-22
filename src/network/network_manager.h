#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "wifi_join_failure.h"

enum class NetworkState : uint8_t {
    WIFI_DISABLED,
    WIFI_NO_CREDENTIALS,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_RETRY_WAIT,
    WIFI_SETUP_REQUIRED,
    WIFI_SETUP_AP,
    // Joined network left for the length of a grind; rejoined by itself
    WIFI_RESTING
};

class SmartGrindNetworkManager {
public:
    void init(Preferences* preferences);
    void update();

    bool set_enabled(bool enabled);
    // Switches Wi-Fi on or off from another task, such as the UI: update()
    // carries it out and stores the choice.
    void request_enabled(bool enabled) { enable_request_.store(enabled ? 1 : 0); }
    // The choice as it stands, counting one that update() has yet to carry out.
    bool wants_enabled() const {
        const int8_t request = enable_request_.load();
        return request < 0 ? enabled_.load() : request == 1;
    }

    // Whether the radio is switched off while a grind runs, to spare a weak
    // power supply (see radio_rest_policy.h). Stored; until the user chooses,
    // it is on for a grinder that has restarted on a brownout before.
    bool rest_while_grinding() const { return rest_while_grinding_.load(); }
    bool set_rest_while_grinding(bool rest);
    // Asks for the radio to be off (a grind is running) or back on. update()
    // carries it out. May be called from any task.
    void set_radio_rest(bool rest) { radio_rest_wanted_.store(rest); }
    bool set_credentials(const String& ssid, const String& password);
    bool connect_saved_credentials();
    void clear_credentials();
    bool start_setup_access_point(const String& ssid, const String& password);

    bool is_enabled() const { return enabled_.load(); }
    bool is_connected() const { return state() == NetworkState::WIFI_CONNECTED; }
    bool has_credentials() const;
    NetworkState state() const { return state_.load(); }
    String hostname() const;
    String device_id() const;
    String network_name() const;
    String ip_address() const;
    // Signal strength of the joined network in dBm; 0 while not connected.
    int rssi_dbm() const;

    // Why the last attempt to join the saved network failed; NONE once joined.
    WifiJoinFailure last_join_failure() const { return last_join_failure_.load(); }
    // "Could not join <network>: <reason>." in plain ASCII, or empty.
    String join_failure_summary() const;
    // The summary followed by what the user can do about it, or empty.
    String join_failure_explanation() const;

private:
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
    static constexpr uint32_t RETRY_DELAY_MS = 30000;

    Preferences* preferences_ = nullptr;
    std::atomic<NetworkState> state_{NetworkState::WIFI_DISABLED};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> enabled_{false};
    // -1: nothing asked; 0 or 1: switch Wi-Fi off or on at the next update()
    std::atomic<int8_t> enable_request_{-1};
    std::atomic<bool> rest_while_grinding_{false};
    std::atomic<bool> radio_rest_wanted_{false};
    // Written by the Wi-Fi event task, read when a join attempt times out.
    std::atomic<uint16_t> last_disconnect_reason_{wifi_disconnect_reason::NONE};
    std::atomic<WifiJoinFailure> last_join_failure_{WifiJoinFailure::NONE};
    bool ever_connected_ = false;
    bool mdns_started_ = false;
    uint32_t state_changed_at_ms_ = 0;
    String ssid_;
    String password_;
    String hostname_;
    mutable SemaphoreHandle_t settings_mutex_ = nullptr;

    void load_settings();
    void apply_enable_request();
    void apply_radio_rest();
    void begin_connection();
    void handle_connected();
    void stop_network();
    void set_state(NetworkState state);
    static String default_hostname();
    static String sanitize_hostname(const String& hostname);
};

extern SmartGrindNetworkManager network_manager;
