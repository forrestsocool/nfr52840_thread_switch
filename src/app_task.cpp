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

#include <hal/nrf_saadc.h>

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

	if (!device_is_ready(task.mAdcDev)) {
		LOG_ERR("ADC device not ready in poll handler!");
		k_work_reschedule(&task.mSenseWork, K_MSEC(500));
		return;
	}

	/* ADC read: 12-bit, gain 1/6, internal 0.6V ref → 3.6V range
	 * 8x hardware oversampling to reduce noise */
	struct adc_sequence sequence = {
		.channels = BIT(kAdcChannelId),
		.buffer = &task.mAdcBuffer,
		.buffer_size = sizeof(task.mAdcBuffer),
		.resolution = 12,
		.oversampling = 3,  /* 2^3 = 8x hardware oversampling */
		.calibrate = !task.mAdcCalibrated,
	};

	int err = adc_read(task.mAdcDev, &sequence);
	if (err) {
		LOG_ERR("ADC read failed: %d", err);
		k_work_reschedule(&task.mSenseWork, K_MSEC(100));
		return;
	}

	int16_t adcVal = task.mAdcBuffer;
	task.mAdcCalibrated = true;
	task.mPollCount++;

	/* Reject clearly invalid values (SAADC errata #89: incorrect sign) */
	if (adcVal < 0 || adcVal > 4095) {
		task.mInvalidCount++;
		if ((task.mInvalidCount % 50) == 1) {
			LOG_WRN("ADC INVALID: val=%d count=%u", adcVal, task.mInvalidCount);
		}
		k_work_reschedule(&task.mSenseWork, K_MSEC(100));
		return;
	}

	/* EMA smoothing: alpha=0.25 */
	if (!task.mEmaInitialized) {
		task.mEmaValue = adcVal;
		task.mEmaInitialized = true;
	} else {
		task.mEmaValue = (task.mEmaValue * 3 + adcVal) / 4;
	}

	/* Warmup: let EMA stabilize (first 10 samples = 1 second) */
	if (task.mWarmupCount < 10) {
		task.mWarmupCount++;
		int mv = (adcVal * 3600) / 4096;
		int emv = (task.mEmaValue * 3600) / 4096;
		LOG_INF("Warmup[%u]: adc=%d (%dmV) ema=%d (%dmV)",
			task.mWarmupCount, adcVal, mv, task.mEmaValue, emv);

		if (task.mWarmupCount == 10) {
			task.mVotedIsOn = (task.mEmaValue <= kThresholdEnterOn);
			LOG_INF("*** Warmup complete: ema=%d -> initial state=%s",
				task.mEmaValue, task.mVotedIsOn ? "ON" : "OFF");

			Nrf::PostTask([isOn = task.mVotedIsOn] {
				Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(isOn);
			});

			bool isOn = task.mVotedIsOn;
			SystemLayer().ScheduleLambda([isOn] {
				OnOff::Attributes::OnOff::Set(kLightEndpointId, isOn);
				LOG_INF("Matter OnOff initialized: %s", isOn ? "ON" : "OFF");
			});
		}
		k_work_reschedule(&task.mSenseWork, K_MSEC(100));
		return;
	}

	/* Hysteresis Detection:
	 *   ON  ≈ 2.48V → ADC ≈ 2821
	 *   OFF ≈ 3.20V → ADC ≈ 3641
	 *   Midpoint ≈ 3231
	 *
	 * Currently OFF → ON  when EMA <= 3100 (~2.72V)
	 * Currently ON  → OFF when EMA >  3400 (~2.99V)
	 * Dead band 3100–3400 */
	bool prevState = task.mVotedIsOn;

	if (task.mVotedIsOn) {
		if (task.mEmaValue > kThresholdExitOn) {
			task.mVotedIsOn = false;
		}
	} else {
		if (task.mEmaValue <= kThresholdEnterOn) {
			task.mVotedIsOn = true;
		}
	}

	if (prevState != task.mVotedIsOn) {
		int emv = (task.mEmaValue * 3600) / 4096;
		LOG_INF("*** STATE CHANGE: %s -> %s (ema=%d =%dmV adc=%d)",
			prevState ? "ON" : "OFF", task.mVotedIsOn ? "ON" : "OFF",
			task.mEmaValue, emv, adcVal);

		Nrf::PostTask([isOn = task.mVotedIsOn] {
			Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(isOn);
		});

		if (!task.mIsPulsing) {
			bool isOn = task.mVotedIsOn;
			SystemLayer().ScheduleLambda([isOn] {
				bool currentState = false;
				OnOff::Attributes::OnOff::Get(kLightEndpointId, &currentState);
				if (currentState != isOn) {
					OnOff::Attributes::OnOff::Set(kLightEndpointId, isOn);
					LOG_INF("Matter OnOff synced: %s", isOn ? "ON" : "OFF");
				}
			});
		}
	}

	/* Periodic logging */
	bool shouldLog = (task.mPollCount <= 30) || ((task.mPollCount % 10) == 0);
	if (shouldLog) {
		int val_mv = (adcVal * 3600) / 4096;
		int ema_mv = (task.mEmaValue * 3600) / 4096;
		LOG_INF("ADC[%u]: val=%d (%dmV) ema=%d (%dmV) state=%s",
			task.mPollCount, adcVal, val_mv, task.mEmaValue, ema_mv,
			task.mVotedIsOn ? "ON" : "OFF");
	}

	k_work_reschedule(&task.mSenseWork, K_MSEC(100));
}

void AppTask::ControlPulseHandler(struct k_work *work)
{
	AppTask &task = Instance();
	/* Restore control pin to INPUT (Hi-Z) after pulse */
	gpio_pin_configure_dt(&task.mCtrlPin, GPIO_INPUT);
	task.mIsPulsing = false;
	LOG_INF("Control Pulse finished, ctrl pin restored to Hi-Z.");
}

void AppTask::InitiateAction(bool actionOn)
{
	LOG_INF("Remote Action requested: %s (Pulse)", actionOn ? "ON" : "OFF");

	mIsPulsing = true;
	/* Pre-set the EMA and voted state to the target to prevent lag-revert */
	mVotedIsOn = actionOn;

	/* Toggle pulse: pull control pin LOW for 300ms */
	gpio_pin_configure_dt(&mCtrlPin, GPIO_OUTPUT_LOW);
	LOG_INF("Control Pin (P0.17) pulled LOW for %s (Pulse start)", actionOn ? "ON" : "OFF");

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

	// Initialize control pin (P0.17)
	mCtrlPin = GPIO_DT_SPEC_GET(DT_NODELABEL(ctrl_pin_1), gpios);

	mVotedIsOn = false;
	mIsPulsing = false;
	mAdcCalibrated = false;
	mEmaValue = 0;
	mEmaInitialized = false;
	mWarmupCount = 0;
	mPollCount = 0;
	mInvalidCount = 0;

	if (gpio_is_ready_dt(&mCtrlPin)) {
		gpio_pin_configure_dt(&mCtrlPin, GPIO_INPUT);
		LOG_INF("Control Pin (P0.17) initialized in Hi-Z.");
	} else {
		LOG_ERR("Control Pin NOT READY.");
	}

	/* Initialize ADC for bath heater sensing on AIN6 (P0.30, board silkscreen '031') */
	mAdcDev = DEVICE_DT_GET(DT_NODELABEL(adc));
	if (!device_is_ready(mAdcDev)) {
		LOG_ERR("ADC device not ready!");
		return CHIP_ERROR_INTERNAL;
	}

	mAdcChannelCfg = {};
	mAdcChannelCfg.gain = ADC_GAIN_1_6;
	mAdcChannelCfg.reference = ADC_REF_INTERNAL;
	mAdcChannelCfg.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40);
	mAdcChannelCfg.channel_id = kAdcChannelId;
	mAdcChannelCfg.input_positive = SAADC_CH_PSELP_PSELP_AnalogInput6; // P0.30 (silkscreen 031)

	int ret = adc_channel_setup(mAdcDev, &mAdcChannelCfg);
	if (ret < 0) {
		LOG_ERR("ADC channel setup failed: %d", ret);
		return CHIP_ERROR_INTERNAL;
	}
	LOG_INF("ADC sensing on AIN6 (P0.30 / silkscreen 031), enter=%d exit=%d",
		(int)kThresholdEnterOn, (int)kThresholdExitOn);

	/* Initialize work items */
	k_work_init_delayable(&mPulseWork, ControlPulseHandler);
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
