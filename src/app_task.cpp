/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"
#include "app/matter_init.h"
#include "app/task_executor.h"
#include "clusters/identify.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/persistence/AttributePersistenceProviderInstance.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/persistence/DeferredAttributePersistenceProvider.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;
using namespace ::chip::app::Clusters;

namespace
{
constexpr EndpointId kLightEndpointId = 1;

Nrf::Matter::IdentifyCluster sIdentifyCluster(kLightEndpointId, true, []() {
	Nrf::PostTask([] { Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(false); });
});

/* We will persist OnOff state across reboots if necessary, although physical sensing is better. */
DefaultAttributePersistenceProvider gSimpleAttributePersistence;

} /* namespace */

void AppTask::UpdateClusterState()
{
	if (!gpio_is_ready_dt(&mSensePin)) {
		return;
	}

	/* Logic: 3.3V (HIGH) -> OFF, 0V (LOW) -> ON */
	bool isOn = (gpio_pin_get_dt(&mSensePin) == 0);

	LOG_INF("Sensing Feedback: Pin is %s -> Status: %s", isOn ? "LOW" : "HIGH", isOn ? "ON" : "OFF");

	SystemLayer().ScheduleLambda([isOn] {
		// Update the attribute on the Matter thread
		bool currentState = false;
		OnOff::Attributes::OnOff::Get(kLightEndpointId, &currentState);

		if (currentState != isOn) {
			OnOff::Attributes::OnOff::Set(kLightEndpointId, isOn);
			LOG_INF("Matter OnOff attribute updated to: %s", isOn ? "ON" : "OFF");
		}
	});
}

void AppTask::SensePinHandler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	/* Reschedule debounce work (50ms). Any new jitter within this window will reset the timer. */
	AppTask &task = Instance();
	k_work_reschedule(&task.mSenseWork, K_MSEC(50));
}

void AppTask::SensingDebounceHandler(struct k_work *work)
{
	/* This is called after the pin has been stable for 50ms */
	Instance().UpdateClusterState();
}

void AppTask::ControlPulseHandler(struct k_work *work)
{
	AppTask &task = Instance();
	/* Restore both to INPUT (Hi-Z) after pulse */
	gpio_pin_configure_dt(&task.mCtrlPinOn, GPIO_INPUT);
	gpio_pin_configure_dt(&task.mCtrlPinOff, GPIO_INPUT);
	LOG_INF("Control Pulse finished, pins restored to Hi-Z.");
}

void AppTask::InitiateAction(bool actionOn)
{
	LOG_INF("Remote Action requested: %s (Blind Pulse)", actionOn ? "ON" : "OFF");

	struct gpio_dt_spec *targetPin = actionOn ? &mCtrlPinOn : &mCtrlPinOff;

	/* Trigger: Switch to OUTPUT and pull LOW */
	gpio_pin_configure_dt(targetPin, GPIO_OUTPUT_LOW);
	LOG_INF("Control Pin (%s) pulled LOW (Pulse start)", actionOn ? "ON" : "OFF");

	/* Schedule release after 300ms */
	k_work_reschedule(&mPulseWork, K_MSEC(300));
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize Matter stack */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{ .mPostServerInitClbk = []() {
		gSimpleAttributePersistence.Init(Nrf::Matter::GetPersistentStorageDelegate());
		return CHIP_NO_ERROR;
	} }));

	if (!Nrf::GetBoard().Init(ButtonEventHandler)) {
		LOG_ERR("User interface initialization failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	/* Register Matter event handler */
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));
	ReturnErrorOnFailure(sIdentifyCluster.Init());

	// Initialize our custom Hardware Pins (Control and Sense)
	mCtrlPinOn = GPIO_DT_SPEC_GET(DT_NODELABEL(ctrl_pin_1), gpios);
	mCtrlPinOff = GPIO_DT_SPEC_GET(DT_NODELABEL(ctrl_pin_2), gpios);
	mSensePin = GPIO_DT_SPEC_GET(DT_NODELABEL(sense_pin_1), gpios);

	if (gpio_is_ready_dt(&mCtrlPinOn) && gpio_is_ready_dt(&mCtrlPinOff)) {
		/* Start in Hi-Z (INPUT) mode as requested */
		gpio_pin_configure_dt(&mCtrlPinOn, GPIO_INPUT);
		gpio_pin_configure_dt(&mCtrlPinOff, GPIO_INPUT);
		LOG_INF("Bath Heater Control Pins initialized in Hi-Z.");
	} else {
		LOG_ERR("Bath Heater Control Pins NOT READY.");
	}

	if (gpio_is_ready_dt(&mSensePin)) {
		gpio_pin_configure_dt(&mSensePin, GPIO_INPUT);
		gpio_init_callback(&mSensePinCbData, SensePinHandler, BIT(mSensePin.pin));
		gpio_add_callback(mSensePin.port, &mSensePinCbData);
		gpio_pin_interrupt_configure_dt(&mSensePin, GPIO_INT_EDGE_BOTH);

		LOG_INF("Sensing Feedback Pin (GPIO %d) initialized.", mSensePin.pin);

		/* Initial sync */
		UpdateClusterState();
	} else {
		LOG_ERR("Sensing Feedback Pin NOT READY.");
	}

	/* Initialize Pulse and Sense Work */
	k_work_init_delayable(&mPulseWork, ControlPulseHandler);
	k_work_init_delayable(&mSenseWork, SensingDebounceHandler);

	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}
	return CHIP_NO_ERROR;
}

void AppTask::ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged)
{
	if ((DK_BTN2_MSK & hasChanged) & state) {
		SystemLayer().ScheduleLambda([] {
			bool currentState = false;
			OnOff::Attributes::OnOff::Get(kLightEndpointId, &currentState);
			Instance().InitiateAction(!currentState);
		});
	}
}
