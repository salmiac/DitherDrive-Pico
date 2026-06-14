#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "hardware/pwm.h"     // Native RP2040 PWM SDK API for core compatibility

// --- PIN DEFINITIONS ---
const int PIN_POT = 26;       // GP26 (ADC0) for setting target current
const int PIN_PWM = 15;       // GP15 for MOSFET gate driver PWM

// --- CONSTANTS & CONFIGURATION ---
const float TARGET_MIN_MA = 150.0;
const float TARGET_MAX_MA = 700.0;
const float SHUTOFF_THRESHOLD_MA = 160.0; // Under this, coil is shut down completely

// PI Controller gains (can be calibrated as needed)
const float KP = 2.5f;
const float KI = 15.0f;       // Integrated over time, KI * error * dt

// PWM configuration
const uint32_t PWM_FREQ_HZ = 20000;   // 20 kHz carrier (inaudible)
const uint32_t MAX_PWM = 4095;        // 12-bit PWM resolution (0 - 4095)

// Dither configuration (122 Hz)
const uint32_t DITHER_FREQ_HZ = 122;
const uint32_t DITHER_PERIOD_US = 1000000 / DITHER_FREQ_HZ; // ~8196 us
const uint32_t DITHER_HALF_PERIOD_US = DITHER_PERIOD_US / 2; // ~4098 us
const int32_t DITHER_AMPLITUDE_PWM = 205; // ~5% of MAX_PWM (can adjust to vary physical dither amplitude)

// Loop timing
const unsigned long LOOP_INTERVAL_MS = 10; // 100 Hz loop rate (dt = 0.01s)

// Protection thresholds
const float OVERCURRENT_LIMIT_MA = 1200.0;
const float OPEN_LOAD_DUTY_THRESHOLD = 0.85f * MAX_PWM; // 85% duty cycle
const float OPEN_LOAD_CURRENT_THRESHOLD_MA = 30.0;
const unsigned long OPEN_LOAD_DELAY_MS = 2000;          // 2 seconds before trip

// Potentiometer filter alpha (EMA)
const float POT_FILTER_ALPHA = 0.1f;

// --- GLOBAL VARIABLES ---
Adafruit_INA219 ina219;

float filteredAdc = 0.0f;
float activeTargetCurrent = 0.0f; // Ramped target current
float iTerm = 0.0f;               // Integral term accumulator

// Dither state variables
volatile bool ditherState = false;
unsigned long lastDitherToggle = 0;

// Loop timing variable
unsigned long lastLoopTime = 0;

// Protection state variables
bool overcurrentTripped = false;
bool openLoadTripped = false;
unsigned long openLoadTimer = 0;

// Function declarations
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max);
void checkProtections(float actualCurrent, uint32_t baseDuty);
void handleSerialDebug(float potTarget, float actualCurrent, uint32_t baseDuty, uint32_t finalDuty);

void setup() {
    // Start USB Serial for debugging
    Serial.begin(115200);
    
    // Configure analog read pin
    pinMode(PIN_POT, INPUT);
    
    // Configure PWM using native RP2040 hardware PWM SDK API
    gpio_set_function(PIN_PWM, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(PIN_PWM);
    
    // Set clock divider and wrap for 20 kHz carrier frequency at 12-bit resolution
    // f = 125 MHz / (clkdiv * (wrap + 1))
    // 20000 = 125000000 / (clkdiv * 4096) -> clkdiv = 6250 / 4096 = 1.5258789
    pwm_set_clkdiv(slice_num, 1.526f);
    pwm_set_wrap(slice_num, MAX_PWM);
    pwm_set_gpio_level(PIN_PWM, 0); // Start at 0% duty cycle
    pwm_set_enabled(slice_num, true);

    // Initialize I2C. Default I2C0 pins are GP4 (SDA) and GP5 (SCL) on standard RP2040.
    Wire.begin();

    Serial.println("--- Proportional Valve Controller Starting ---");

    if (!ina219.begin()) {
        Serial.println("CRITICAL ERROR: Failed to find INA219 current sensor chip!");
        while (1) { delay(10); } // Halt if sensor not found
    }
    
    // By default, INA219 initializes to 32V / 2A range, which fits our 24V / 0.7A max load.
    Serial.println("INA219 Current Sensor Initialized successfully.");
    
    // Seed the pot filter with initial reading
    filteredAdc = analogRead(PIN_POT);
    lastLoopTime = millis();
    lastDitherToggle = micros();
}

void loop() {
    unsigned long currentMillis = millis();

    // 1. DITHER TOGGLE BLOCK (Independent of main 10ms loop for timing accuracy)
    unsigned long currentMicros = micros();
    if (currentMicros - lastDitherToggle >= DITHER_HALF_PERIOD_US) {
        ditherState = !ditherState;
        lastDitherToggle = currentMicros;
    }

    // 2. MAIN CONTROL LOOP (Runs at 100 Hz / 10ms)
    if (currentMillis - lastLoopTime >= LOOP_INTERVAL_MS) {
        lastLoopTime = currentMillis;

        // Read sensor current
        float actualCurrent = 0.0f;
        if (!overcurrentTripped && !openLoadTripped) {
            actualCurrent = abs(ina219.getCurrent_mA());
        }

        // Read and filter target setpoint from potentiometer
        float rawAdc = analogRead(PIN_POT);
        filteredAdc = (1.0f - POT_FILTER_ALPHA) * filteredAdc + POT_FILTER_ALPHA * rawAdc;

        // Map filtered ADC to target current range
        float potTarget = mapFloat(filteredAdc, 0.0f, 4095.0f, TARGET_MIN_MA, TARGET_MAX_MA);

        // Reset trips if potentiometer is turned to the absolute minimum
        if (potTarget < SHUTOFF_THRESHOLD_MA && (overcurrentTripped || openLoadTripped)) {
            overcurrentTripped = false;
            openLoadTripped = false;
            iTerm = 0.0f;
            Serial.println("Protection trips reset via low setpoint command.");
        }

        // Handle valve shutoff below threshold
        float targetCurrent = 0.0f;
        if (potTarget >= SHUTOFF_THRESHOLD_MA && !overcurrentTripped && !openLoadTripped) {
            targetCurrent = potTarget;
        }

        // Soft Start / Ramping: limit target current rate of change
        const float rampRate = 5.0f; // mA limit per 10ms (500 mA/second ramp rate)
        if (activeTargetCurrent < targetCurrent) {
            activeTargetCurrent = min(activeTargetCurrent + rampRate, targetCurrent);
        } else if (activeTargetCurrent > targetCurrent) {
            activeTargetCurrent = max(activeTargetCurrent - rampRate, targetCurrent);
        }

        // PI Controller calculation
        uint32_t baseDuty = 0;
        if (activeTargetCurrent > 0.0f) {
            float error = activeTargetCurrent - actualCurrent;
            float pTerm = KP * error;
            iTerm += KI * error * 0.01f; // dt = 10ms = 0.01s

            // Anti-windup clamping
            iTerm = constrain(iTerm, 0.0f, (float)MAX_PWM);

            float output = pTerm + iTerm;
            baseDuty = constrain((int32_t)output, 0, (int32_t)MAX_PWM);
        } else {
            iTerm = 0.0f; // Reset integrator when shut off
            baseDuty = 0;
        }

        // Safety checking
        checkProtections(actualCurrent, baseDuty);

        // Calculate and apply dither
        uint32_t finalDuty = 0;
        if (activeTargetCurrent > 0.0f && !overcurrentTripped && !openLoadTripped) {
            int32_t ditherOffset = ditherState ? DITHER_AMPLITUDE_PWM : -DITHER_AMPLITUDE_PWM;
            finalDuty = constrain((int32_t)baseDuty + ditherOffset, 0, (int32_t)MAX_PWM);
        } else {
            finalDuty = 0; // Disable PWM output entirely if stopped or tripped
        }

        // Write PWM level using native RP2040 SDK API
        pwm_set_gpio_level(PIN_PWM, finalDuty);

        // Debug logging to USB Serial
        handleSerialDebug(potTarget, actualCurrent, baseDuty, finalDuty);
    }
}

float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void checkProtections(float actualCurrent, uint32_t baseDuty) {
    // 1. Overcurrent Protection
    if (actualCurrent > OVERCURRENT_LIMIT_MA) {
        overcurrentTripped = true;
        pwm_set_gpio_level(PIN_PWM, 0); // Shut down immediately
        Serial.print("CRITICAL ERROR: Overcurrent tripped! Measured: ");
        Serial.print(actualCurrent);
        Serial.println(" mA");
    }

    // 2. Open-Load Protection (Broken Wire / Disconnected Valve)
    if (baseDuty > OPEN_LOAD_DUTY_THRESHOLD && actualCurrent < OPEN_LOAD_CURRENT_THRESHOLD_MA) {
        if (openLoadTimer == 0) {
            openLoadTimer = millis();
        } else if (millis() - openLoadTimer >= OPEN_LOAD_DELAY_MS) {
            openLoadTripped = true;
            pwm_set_gpio_level(PIN_PWM, 0); // Shut down immediately
            Serial.println("CRITICAL ERROR: Open Load tripped! Duty cycle high but no current detected.");
        }
    } else {
        openLoadTimer = 0; // Reset open load timer if conditions aren't met
    }
}

void handleSerialDebug(float potTarget, float actualCurrent, uint32_t baseDuty, uint32_t finalDuty) {
    static unsigned long lastLogTime = 0;
    if (millis() - lastLogTime >= 200) { // Log every 200ms
        lastLogTime = millis();

        if (overcurrentTripped) {
            Serial.println("[STATUS] TRIPPED: Overcurrent. Turn potentiometer to min to reset.");
        } else if (openLoadTripped) {
            Serial.println("[STATUS] TRIPPED: Open Load (Broken wire). Turn potentiometer to min to reset.");
        } else {
            Serial.print("Target: ");
            Serial.print(potTarget, 1);
            Serial.print(" mA | Ramped: ");
            Serial.print(activeTargetCurrent, 1);
            Serial.print(" mA | Measured: ");
            Serial.print(actualCurrent, 1);
            Serial.print(" mA | PI Duty: ");
            Serial.print(mapFloat((float)baseDuty, 0, MAX_PWM, 0, 100), 1);
            Serial.print("% | Dither: ");
            Serial.print(ditherState ? "+" : "-");
            Serial.print(" | Final: ");
            Serial.print(mapFloat((float)finalDuty, 0, MAX_PWM, 0, 100), 1);
            Serial.println("%");
        }
    }
}
