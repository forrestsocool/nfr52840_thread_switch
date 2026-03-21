/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <lib/support/logging/CHIPLogging.h>

#include "app_task.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>

using namespace ::chip;
using namespace ::chip::app::Clusters;
using namespace ::chip::app::Clusters::OnOff;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath, uint8_t type,
				       uint16_t size, uint8_t *value)
{
	ClusterId clusterId = attributePath.mClusterId;
	AttributeId attributeId = attributePath.mAttributeId;

	if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
		ChipLogProgress(Zcl, "Cluster OnOff: attribute OnOff set to %" PRIu8 "", *value);
		// Call our custom logic which pulses the pin if the physical state doesn't match
		AppTask::Instance().InitiateAction(*value);
	}
}

/** @brief OnOff Cluster Init
 *
 * This function is called when a specific cluster is initialized.
 */
void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
	Protocols::InteractionModel::Status status;
	bool storedValue;

	/* Read storedValue on/off value */
	status = Attributes::OnOff::Get(endpoint, &storedValue);

	// AppTask::Init() used to sync with Sense Pin, but it's now removed for blind control.
}

namespace chip {
namespace app {
namespace Clusters {
namespace LevelControl {
enum class Feature : uint32_t;
}
}
}
}

bool LevelControlHasFeature(chip::EndpointId endpoint, chip::app::Clusters::LevelControl::Feature feature)
{
    return false;
}

void emberAfOnOffClusterLevelControlEffectCallback(chip::EndpointId endpoint, bool newValue)
{
}

void MatterLevelControlPluginServerInitCallback()
{
}

#include <app/CommandHandler.h>
#include <app-common/zap-generated/cluster-objects.h>

bool emberAfLevelControlClusterMoveToLevelCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::MoveToLevel::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterMoveCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::Move::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterStepCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::Step::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterStopCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::Stop::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterMoveToLevelWithOnOffCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::MoveToLevelWithOnOff::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterMoveWithOnOffCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::MoveWithOnOff::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterStepWithOnOffCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::StepWithOnOff::DecodableType& commandData) { return false; }
bool emberAfLevelControlClusterStopWithOnOffCallback(chip::app::CommandHandler* commandObj, const chip::app::ConcreteCommandPath& commandPath, const chip::app::Clusters::LevelControl::Commands::StopWithOnOff::DecodableType& commandData) { return false; }

void emberAfLevelControlClusterServerInitCallback(chip::EndpointId endpoint)
{
}

void MatterLevelControlClusterServerShutdownCallback(chip::EndpointId endpoint)
{
}
