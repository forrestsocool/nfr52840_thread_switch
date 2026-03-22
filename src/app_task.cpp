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
	/* Handled by polling logic in SensingPollHandler */
}

void AppTask::SensingPollHandler(struct k_work *work)
{
	AppTask &task = Instance();

	if (!gpio_is_ready_dt(&task.mSensePin)) {
		return;
	}

	/* Read physical pin: 1 for 2.9V/HIGH, 0 for 0V/LOW */
	uint8_t rawValue = gpio_pin_get_dt(&task.mSensePin) & 1;

	/* Update history (last 10 samples) */
	task.mSenseHistory = (task.mSenseHistory << 1) | rawValue;

	/* Voting logic: Count how many LOW (0) samples we have in the last 10 */
	int lowCount = 0;
	for (int i = 0; i < 10; i++) {
		if (!((task.mSenseHistory >> i) & 1)) {
			lowCount++;
		}
	}

	/* Majority Vote: Only if 8/10 are LOW do we say ON. Else OFF. */
	bool votedIsOn = (lowCount >= 8);
	task.mVotedIsOn = votedIsOn;

	/* Update Matter state if voted status changed AND not currently pulsing */
	if (!task.mIsPulsing) {
		SystemLayer().ScheduleLambda([votedIsOn] {
			bool currentState = false;
			OnOff::Attributes::OnOff::Get(kLightEndpointId, &currentState);

			if (currentState != votedIsOn) {
				OnOff::Attributes::OnOff::Set(kLightEndpointId, votedIsOn);
				LOG_INF("Matter OnOff attribute synced to VOTED status: %s (LowCount >= 8)", votedIsOn ? "ON" : "OFF");
			}
		});
	}

	/* Schedule next poll in 100ms */
	k_work_reschedule(&task.mSenseWork, K_MSEC(100));
}

void AppTask::SensePinHandler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	/* Interrupts disabled in favor of polling */
}

void AppTask::ControlPulseHandler(struct k_work *work)
{
	AppTask &task = Instance();
	/* Restore control pin to INPUT (Hi-Z) after pulse */
	gpio_pin_configure_dt(&task.mCtrlPinOn, GPIO_INPUT);
	LOG_INF("Control Pulse finished, ctrl pin restored to Hi-Z.");

	if (task.mPendingOff) {
		/* For OFF action: schedule standby pulse 1 second later */
		LOG_INF("OFF action: scheduling standby pulse in 1 second...");
		k_work_reschedule(&task.mStandbyWork, K_MSEC(1000));
	} else {
		task.mIsPulsing = false;
	}
}

void AppTask::StandbyPulseHandler(struct k_work *work)
{
	AppTask &task = Instance();
	/* Pull standby pin LOW for 300ms */
	gpio_pin_configure_dt(&task.mStandbyPin, GPIO_OUTPUT_LOW);
	LOG_INF("Standby Pin pulled LOW (pulse start)");
	k_work_reschedule(&task.mStandbyReleaseWork, K_MSEC(300));
}

void AppTask::StandbyReleaseHandler(struct k_work *work)
{
	AppTask &task = Instance();
	/* Restore standby pin to INPUT (Hi-Z) */
	gpio_pin_configure_dt(&task.mStandbyPin, GPIO_INPUT);
	task.mIsPulsing = false;
	task.mPendingOff = false;
	LOG_INF("Standby Pulse finished, pin restored to Hi-Z. OFF sequence complete.");
}

void AppTask::InitiateAction(bool actionOn)
{
	LOG_INF("Remote Action requested: %s (Blind Pulse)", actionOn ? "ON" : "OFF");

	mIsPulsing = true;
	mPendingOff = !actionOn;
	/* Warm up sensing history to the target state to avoid the 1-second lag-revert.
	 * If target is ON, set history to all 0s (LOW). If OFF, set to all 1s (HIGH). */
	mSenseHistory = actionOn ? 0x0000 : 0xFFFF;
	mVotedIsOn = actionOn;

	/* Both ON and OFF use the same control pin (gpio0.17) */
	gpio_pin_configure_dt(&mCtrlPinOn, GPIO_OUTPUT_LOW);
	LOG_INF("Control Pin (gpio0.17) pulled LOW for %s (Pulse start)", actionOn ? "ON" : "OFF");

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
	mStandbyPin = GPIO_DT_SPEC_GET(DT_NODELABEL(ctrl_pin_2), gpios);
	mSensePin = GPIO_DT_SPEC_GET(DT_NODELABEL(sense_pin_1), gpios);
	
	mSenseHistory = 0xFFFF; // Initial state: all HIGH (OFF)
	mVotedIsOn = false;
	mIsPulsing = false;
	mPendingOff = false;

	if (gpio_is_ready_dt(&mCtrlPinOn) && gpio_is_ready_dt(&mStandbyPin)) {
		/* Start in Hi-Z (INPUT) mode as requested */
		gpio_pin_configure_dt(&mCtrlPinOn, GPIO_INPUT);
		gpio_pin_configure_dt(&mStandbyPin, GPIO_INPUT);
		LOG_INF("Control Pin (gpio0.17) and Standby Pin (gpio0.31) initialized in Hi-Z.");
	} else {
		LOG_ERR("Control/Standby Pins NOT READY.");
	}

	if (gpio_is_ready_dt(&mSensePin)) {
		gpio_pin_configure_dt(&mSensePin, GPIO_INPUT);
		/* No interrupts: Voting Polling instead */
		LOG_INF("Sensing Feedback Polling (GPIO %d) initialized (100ms 8/10 vote).", mSensePin.pin);
	} else {
		LOG_ERR("Sensing Feedback Pin NOT READY.");
	}

	/* Initialize Pulse, Standby, and Sense Work */
	k_work_init_delayable(&mPulseWork, ControlPulseHandler);
	k_work_init_delayable(&mStandbyWork, StandbyPulseHandler);
	k_work_init_delayable(&mStandbyReleaseWork, StandbyReleaseHandler);
	k_work_init_delayable(&mSenseWork, SensingPollHandler);

	/* Start the polling loop immediately */
	k_work_schedule(&mSenseWork, K_NO_WAIT);

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
