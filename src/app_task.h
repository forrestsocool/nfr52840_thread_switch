/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include "board/board.h"
#include <platform/CHIPDeviceLayer.h>
#include <zephyr/drivers/gpio.h>
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

private:
	CHIP_ERROR Init();

	// Handlers
	static void ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged);
	static void ControlPulseHandler(struct k_work *work);
	static void PostEventTask(void *event);

	// Zephyr GPIO configurations for Bath Heater
	struct gpio_dt_spec mCtrlPinOn;
	struct gpio_dt_spec mCtrlPinOff;
	struct gpio_dt_spec mSensePin;
	struct gpio_callback mSensePinCbData;
	struct k_work_delayable mPulseWork;
	struct k_work_delayable mSenseWork;
	uint16_t mSenseHistory;

	static void SensePinHandler(const struct device *port, struct gpio_callback *cb, uint32_t pins);
	static void SensingPollHandler(struct k_work *work);
};
