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

void AppTask::SensePinChangedHandler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	/* This is an ISR context! We must post work to the Matter thread. */
	SystemLayer().ScheduleLambda([] {
		Instance().UpdateClusterState();
	});
}

void AppTask::UpdateClusterState()
{
	// Read the actual physical state of the Bath Heater from the Sense Pin
	bool isOn = (gpio_pin_get_dt(&mSensePin) == 1);

	LOG_INF("Physical Sense Pin status: %s", isOn ? "ON" : "OFF");

	// Synchronize Matter Data Model with physical state
	Protocols::InteractionModel::Status status =
		OnOff::Attributes::OnOff::Set(kLightEndpointId, isOn);

	if (status != Protocols::InteractionModel::Status::Success) {
		LOG_ERR("Updating on/off cluster failed: %x", to_underlying(status));
	}
}

void AppTask::InitiateAction(bool actionOn)
{
	// Called by zcl_callbacks.cpp when Matter attribute turns ON/OFF via Home App
	bool isCurrentlyOn = (gpio_pin_get_dt(&mSensePin) == 1);

	LOG_INF("Remote Action requested: %s, Current Physical State: %s", 
			actionOn ? "ON" : "OFF", isCurrentlyOn ? "ON" : "OFF");

	if (actionOn == isCurrentlyOn) {
		// Physical state already matches requested state!
		LOG_INF("Physical state matches requested state. (Ignored physical block for testing)");
		// return; // DONT RETURN, allow pulse so we can test toggling without sense pin
	}

	// Pulse the control pin momentarily (like a relay button click)
	LOG_INF("Pulsing Control Pin...");
	gpio_pin_set_dt(&mCtrlPin, 1);
	k_msleep(200);
	gpio_pin_set_dt(&mCtrlPin, 0);

	// The physical change will eventually toggle the SensePin which triggers SensePinChangedHandler.
	// We can forcibly update state or let the GPIO interrupt handle it.
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
	// Fallbacks if DT node alias is not correctly defined (though we did define them)
	mCtrlPin = GPIO_DT_SPEC_GET(DT_NODELABEL(ctrl_pin_1), gpios);
	mSensePin = GPIO_DT_SPEC_GET(DT_NODELABEL(sense_pin_1), gpios);

	if (gpio_is_ready_dt(&mCtrlPin)) {
		gpio_pin_configure_dt(&mCtrlPin, GPIO_OUTPUT_INACTIVE);
		LOG_INF("Bath Heater Control Pin initialized.");
	} else {
		LOG_ERR("Bath Heater Control Pin NOT READY.");
	}

	if (gpio_is_ready_dt(&mSensePin)) {
		gpio_pin_configure_dt(&mSensePin, GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&mSensePin, GPIO_INT_EDGE_BOTH);
		
		gpio_init_callback(&mSenseCbData, SensePinChangedHandler, BIT(mSensePin.pin));
		gpio_add_callback(mSensePin.port, &mSenseCbData);
		LOG_INF("Bath Heater Sense Pin initialized and Interrupts attached.");
	} else {
		LOG_ERR("Bath Heater Sense Pin NOT READY.");
	}

	// Initial sync of the matter state at boot with physical state
	SystemLayer().ScheduleLambda([this] {
		UpdateClusterState();
	});

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
	// We could use physical buttons on the board to mock the remote app click if needed.
	if ((DK_BTN2_MSK & hasChanged) & state) {
		SystemLayer().ScheduleLambda([] {
			// Get current Matter state and toggle it (simulating app behavior)
			bool currentState = false;
			OnOff::Attributes::OnOff::Get(kLightEndpointId, &currentState);
			Instance().InitiateAction(!currentState);
		});
	}
}
