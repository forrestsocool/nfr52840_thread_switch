/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "board/board.h"
#include <platform/CHIPDeviceLayer.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
struct Identify;

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();
	void UpdateClusterState();

	// Called from ZCL callback when remote command (Home App) changes the OnOff attribute
	void InitiateAction(bool actionOn);
	bool GetVotedIsOn() { return mVotedIsOn; }

private:
	CHIP_ERROR Init();

	// Handlers
	static void ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged);
	static void ControlPulseHandler(struct k_work *work);
	static void PostEventTask(void *event);

	// Zephyr GPIO configurations for Bath Heater
	struct gpio_dt_spec mCtrlPin;     // P0.17 — control pin (ON/OFF toggle pulse)

	// ADC sensing for bath heater status detection (P0.02 = AIN0)
	const struct device *mAdcDev;
	struct adc_channel_cfg mAdcChannelCfg;
	int16_t mAdcBuffer;               // ADC sample buffer
	bool mAdcCalibrated;              // Whether SAADC offset has been calibrated
	static constexpr uint8_t kAdcChannelId = 6;  // AIN6 = P0.30 (board silkscreen '031')

	/* Hysteresis thresholds:
	 *   ON ≈ 2.48V → ADC ≈ 2821
	 *   OFF ≈ 3.20V → ADC ≈ 3641
	 *   Midpoint ≈ 3231
	 *
	 * Enter ON:  EMA must drop below 3100 (~2.72V)
	 * Exit ON:   EMA must rise above 3400 (~2.99V)
	 * Dead band: 3100–3400 (no transitions in this zone)
	 */
	static constexpr int32_t kThresholdEnterOn = 3100;
	static constexpr int32_t kThresholdExitOn  = 3400;

	// Exponential Moving Average for smoothed ADC reading
	int32_t mEmaValue;
	bool mEmaInitialized;
	uint16_t mWarmupCount;            // Samples to skip before detection starts

	struct k_work_delayable mPulseWork;
	struct k_work_delayable mSenseWork;
	bool mVotedIsOn;
	bool mIsPulsing;
	uint32_t mPollCount;              // Counter for periodic debug logging
	uint32_t mInvalidCount;           // Counter for invalid ADC readings (errata/glitch)

	static void SensingPollHandler(struct k_work *work);
};
