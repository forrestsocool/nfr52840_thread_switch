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
	/* No physical feedback pin. Status is blind-synced by incoming commands. */
	LOG_INF("Blind control mode: Status feedback disabled.");
}

void AppTask::ControlPulseHandler(struct k_work *work)
{
	AppTask &task = Instance();
	/* Restore to INPUT (Hi-Z) after pulse */
	gpio_pin_configure_dt(&task.mCtrlPin, GPIO_INPUT);
	LOG_INF("Control Pulse finished, pin restored to Hi-Z.");
}

void AppTask::InitiateAction(bool actionOn)
{
	LOG_INF("Remote Action requested: %s (Blind Pulse)", actionOn ? "ON" : "OFF");

	/* Trigger: Switch to OUTPUT and pull LOW */
	gpio_pin_configure_dt(&mCtrlPin, GPIO_OUTPUT_LOW);
	LOG_INF("Control Pin pulled LOW (Pulse start)");

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

	// Initialize our custom Hardware Pins (Control only)
	mCtrlPin = GPIO_DT_SPEC_GET(DT_NODELABEL(ctrl_pin_1), gpios);

	if (gpio_is_ready_dt(&mCtrlPin)) {
		/* Start in Hi-Z (INPUT) mode as requested */
		gpio_pin_configure_dt(&mCtrlPin, GPIO_INPUT);
		LOG_INF("Bath Heater Control Pin initialized in Hi-Z.");
	} else {
		LOG_ERR("Bath Heater Control Pin NOT READY.");
	}

	/* Initialize Pulse Work */
	k_work_init_delayable(&mPulseWork, ControlPulseHandler);

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
