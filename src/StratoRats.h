#ifndef STRATORATS_H
#define STRATORATS_H

#include <time.h>
#include "StratoCore.h"
#include "RATSHardware.h"
#include "RATSConfigs.h"
#include "MCBComm.h"
#include "ECULoRa.h"
#include "ECUReport.h"
#include "etl/bit_stream.h"
#include "etl/array.h"

// Set this true to disable some error checking and logging during development testing.
#define DISABLE_DEVEL_ERROR_CHECKING false

#define EXTRA_LOGGING false

// RATSReport reporting period, when scheduled by ACTION_RATS_REPORT.
#define RATS_REPORT_PERIOD_SECS 360

// Send a RATSReport when NUM_ECU_REPORTS have been received.
// But if RATS_REPORT_PERIOD_SECS has elapsed, the report will be sent regardless.
#define NUM_ECU_REPORTS 180

// RATS_REPORT_MAX_BYTES is the maximum size of a RATS report in bytes. 
#define RATS_REPORT_MAX_BYTES (RATS_HEADER_SIZE_BYTES+(NUM_ECU_REPORTS+1)*ECU_REPORT_SIZE_BYTES)

// Verify that a RATS report will fit in the TM message buffer.
#if RATS_REPORT_MAX_BYTES > 8192
// TODO: Verify that this is the true limit for the TM binary payload. It's
// likely that the limit is actually includes all of the XML header and other
// TM overhead, so this may be unsafe.
#error RATS_REPORT_MAX_BYTES exceeds TM message buffer size
#endif

#ifndef LOG_ZEPHYR_COMMS_SHARED
#define ZEPHYR_SERIAL   Serial1
#else
// This allows for use of the OBD_Simulator with just the Teensy programming port, 
// by sharing it for both Zephyr and StratoCore log messages.
#define ZEPHYR_SERIAL   Serial
#endif

// Our instrument name
#define INSTRUMENT      RATS

// Number of LoRa messages to wait for before moving on
#define LORA_MSG_COUNT  3
// Seconds to wait for all LoRa messages to be received during warmup
#define LORA_WARMUP_MSG_TIMEOUT 15

#define MCB_SERIAL_BUFFER_SIZE    4096

// Buffers for msg reception and transmission to/from Zephyr. Should be large enough
// to hold a complete TM, some of which which will contain the measurement data.
#define ZEPHYR_SERIAL_BUFFER_SIZE (2*8192)

// Number of loops before a flag becomes stale and is reset
#define FLAG_STALE      3
//
#define MCB_RESEND_TIMEOUT      10

// The size of a buffer used for binary transfers between RATS and MCB.
#define MCB_BINARY_BUFFER_SIZE MAX_MCB_BINARY
#define HEARTBEAT_LED_PIN	3

#define ZEPHYR_RESEND_TIMEOUT   60

    // Actions
enum ScheduleAction_t : uint8_t {
    NO_ACTION = NO_SCHEDULED_ACTION,
    SEND_IMR,

    RESEND_RA,
    RESEND_MOTION_COMMAND,
    RESEND_TM,
    RESEND_SAFETY,

    ACTION_START_TELEMETRY,
    ACTION_GPS_WAIT_MSG,
    ACTION_LORA_COUNT_MSGS,
    ACTION_RATS_REPORT,
    ACTION_REEL_OUT,
    ACTION_REEL_IN,
    ACTION_IN_NO_LW,

    ACTION_MOTION_STOP,
    ACTION_MOTION_TIMEOUT,

    NUM_ACTIONS
};

enum MCBMotion_t : uint8_t {
    NO_MOTION,
    MOTION_REEL_IN,
    MOTION_REEL_OUT,
    MOTION_IN_NO_LW
};

enum WarmupStatus_t : uint8_t {
    WARMUP_NOT_STARTED,
    WARMUP_INPROCESS,
    WARMUP_FAILED,
    WARMUP_COMPLETE
};

class StratoRATS : public StratoCore {
#define RATS_HEADER_SIZE_BITS (8+16+16+1+13)
#define RATS_HEADER_SIZE_BYTES 7
    struct RATSReportHeader_t {
        uint8_t header_size_bytes;
        // The number of ECU records in the report. There may be zero records 
        // if the ECU was not powered on.
        uint16_t num_ecu_records : 16;
        uint16_t ecu_record_size_bytes : 16;
        // If the ECU is powered on.
        uint8_t ecu_pwr_on : 1;
        // The 56V voltage in 0.01V units (0-8191 : 0.00V to 81.91V)
        uint16_t v56 : 13;
    };
    
    // A byte array to hold the serialized RATS report header.
    typedef etl::array<uint8_t, RATS_HEADER_SIZE_BYTES> RATSReportHeaderBytes_t;

    // The RATS report serialized bytes are collected here. But note that each item
    // will need to be added to the TM buffer individually.
    struct RATSReportTM_t {
        // The RATS report header
        RATSReportHeaderBytes_t header_bytes;
        // The ECU report data. There may be zero records if the ECU was not powered on.
        // Extra space just in case?
        ECUReportBytes_t records[1+NUM_ECU_REPORTS];
    };
public:
    StratoRATS();
    ~StratoRATS() { };

    // called before the loop begins
    void InstrumentSetup();

    // called at the end of each loop
    void InstrumentLoop();

    // called in each main loop
    void RunMCBRouter();

private:
    // internal serial interface objects for the MCB and ECU
    MCBComm mcbComm;

    // EEPROM interface object
    RATSConfigs ratsConfigs;

    // Initialize the flight mode status values
    void FlightModeInit();

    // Mode functions (implemented in unique source files)
    void StandbyMode();
    void FlightMode();
    void LowPowerMode();
    void SafetyMode();
    void EndOfFlightMode();
    
    void RATS_Shutdown();

    // The FLIGHT mode substates.
    // Moved from Flight.cpp to here so that TCHandler() can use them to vet TCs.
    enum FLStates_t : uint8_t {
        FL_ENTRY = MODE_ENTRY,
        FL_GPS_WAIT,
        FL_WARMUP,
        FL_MEASURE,
        FL_REEL,
        FL_ERROR = MODE_ERROR,
        FL_SHUTDOWN = MODE_SHUTDOWN,
        FL_EXIT = MODE_EXIT
    };

    // Flight states (each in own .cpp file)
    // When starting a state, initiate it by calling with state = true,
    // then call with restart==false until the function returns true,
    // a which indicates that the state has completed.
    // HOWEVER, THE STATE FUNCTION CAN CHANGE inst_substate (E.g. TO FL_ERROR)
    // AND RETURN FALSE, WHICH WILL FORCE THE FLIGHT MODE TO THE NEW STATE.
    // TODO: This seems hard to follow. Perhaps the flight state should
    // return a specific status code, and let Flight() determine what
    // to do. As it is, the complete state machine logic is split up
    // among multiple files (via inst_state), making it very hard to 
    // see the logic in one place.
    //
    // A sub-sub state machine to manage reel operations.
    bool Flight_Reel(bool restart);
    // A sub-sub state machine to manage the warmup operations.
    bool Flight_Warmup(bool restart);

    // Telcommand handler - returns ack/nak
    bool TCHandler(Telecommand_t telecommand);

    // *** Action processing ***
    // Action handler for scheduled actions
    void ActionHandler(uint8_t action);
    // Safely check and clear action flags
    bool CheckAction(uint8_t action);
    // Correctly set an action flag
    void SetAction(uint8_t action);
    // Monitor the action flags and clear old ones
    void WatchFlags();
    // Used by StratoRATS action logic (I wonder why this logic is not in the StratoCore class?)
    ActionFlag_t action_flags[NUM_ACTIONS] = {{0}}; // initialize all flags to false


    // Global variable to track flight_substate_map[inst_subst] during flight mode
    uint8_t flight_mode_substate = 0;

    // *** LoRa support ***
    // Call this during every loop to check for incoming LoRa messages.
    void LoRaRX();
    // The most recently received LoRa message.
    ECULoRaMsg_t lora_msg;
    // The total number of LoRa messages received since the application started.
    uint32_t total_lora_count = 0;
    // A temporary counter to track the number of LoRa messages received during warmup.
    uint32_t lora_count = 0;
    // Count incoming lora messages. If reset is set, the
    // count is set to 0. 
    // Return the current count.
    // Uses the global variable lora_count.
    uint32_t lora_count_check(bool reset=false);

    // ECU control
    void ECUControl(bool enable);

    // *** Warmup state machine ***
    // LoRa message timeout counter
    uint32_t LoRaMsg_timer_start = 0;
    // The warmup status
    WarmupStatus_t warmup_status = WARMUP_INPROCESS;
    // Number of warmup cycles
    uint8_t warmup_cycles = 0;

    // *** Reel motion variables ***
    // Set in TCHandler(), used in Flight_Reel.
    float deploy_length = 0.0f;
    // Set in TCHandler(), used in Flight_Reel.
    float retract_length = 0.0f;
    // uint32_t start time of the current reel motion, in millis
    uint32_t reel_motion_start = 0;

    // *** MCB support ***
    // Handle ASCII messages from the MCB (in MCBRouter.cpp)
    void HandleMCBASCII();
    // Handle ACK messages from the MCB (in MCBRouter.cpp)
    void HandleMCBAck();
    // Handle binary messages from the MCB (in MCBRouter.cpp)
    void HandleMCBBin();
    // Handle string messages from the MCB (in MCBRouter.cpp)
    void HandleMCBString();
    // Start any type of MCB motion
    bool StartMCBMotion();
    // Set variables and initialize the TM for MCB binary data collection.
    // AddMCBTM() will add append MCB binary data to the TM buffer.
    void InitMCBMotionTracking();
    // Add the current MCB motion binary data to the TM buffer.
    // IF WE ARE IN REAL-TIME MODE, THE TM PACKET WILL BE SENT IMMEDIATELY.
    void AddMCBTM();
    // Send a TM with a StateMessage1 message, and the aggregated MCB binary info.
    // All of the aggregated MCB binary data are included in the TM packet.
    void SendMCBTM(StateFlag_t state_flag, const char * message);
    bool mcb_low_power = false;
    // Set when a reel motion is initiated, cleared when the motion is complete.
    bool mcb_motion_ongoing = false;
    // The maximum time allowed for a reel motion to complete.
    uint32_t max_reel_seconds = 0;
    // TODO: Will be used in the safety mode, which has been implemented yet
    bool mcb_reeling_in = false;
    // The buffer used to receive binary data from each incoming MCB message.
    // The data will be copied to the MCB_TM_buffer for TM transmission.
    uint8_t binary_mcb[MCB_BINARY_BUFFER_SIZE];
    // Counts the number of MCB binary messages received during a motion
    uint16_t mcb_tm_counter = 0;
    // The current reel position in revs, extracted from the MCB binary message.
    float reel_pos = 0.0;
    // array of error values for MCB motion fault
    uint16_t motion_fault[8] = {0};
    // A buffer to collect MCB binary data for the MCB TM.
    uint8_t MCB_TM_buffer[8192] = {0};
    // Next available index in the MCB TM buffer.
    uint16_t MCB_TM_buffer_idx = 0;
    // tracks the current type of motion
    MCBMotion_t mcb_motion = NO_MOTION;

    // *** Other TC handlers ***
    // Send a TM with MCB EEPROM contents
    void SendMCBEEPROM();
    // Send a TM with RATS EEPROM contents
    void SendRATSEEPROM();

    // *** RatsReports ***
    // Check if it's time for a ratsReport and send a TM if true.
    // If time_based is true, the report will be sent if the time period has elapsed.
    // If time_based is false, the report will be sent based on ACTION_RATS_REPORT.
    void ratsReportCheck(bool time_based=false);
    // Send a TM with the ratsReport message
    void ratsReportTM();
    // The RATS report header. It will be serialized and added to the TM buffer.
    RATSReportHeader_t rats_report_header;
    // Buffer for buliding the RATS report TM
    RATSReportTM_t rats_report_tm;
    // Time of last RATS report
    time_t last_rats_report = 0;
    // Accumulate RATS reports for transmission
    void ratsReportAccumulate(ECUReportBytes_t& ecu_report_bytes);

};
#endif /* STRATORATS_H */
