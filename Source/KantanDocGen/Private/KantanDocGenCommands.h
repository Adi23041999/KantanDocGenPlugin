// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Copyright (C) 2016-2017 Cameron Angus. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

class FKantanDocGenCommands : public TCommands<FKantanDocGenCommands>
{
public:
	FKantanDocGenCommands()
	: TCommands<FKantanDocGenCommands>("KantanDocGen", // Context name for fast lookup
		NSLOCTEXT("Contexts", "KantanDocGen", "Kantan Doc Gen"), // Localized context name for displaying
		NAME_None, // Parent
		FAppStyle::GetAppStyleSetName() // Icon Style Set
	)
	{
	}

	/**
	 * Initialize commands
	 */
	virtual void RegisterCommands() override;

public:
	// Mode Switch
	TSharedPtr<FUICommandInfo> ShowDocGenUI;

	// Map
	TMap<FName, TSharedPtr<FUICommandInfo>> NameToCommandMap;
};
