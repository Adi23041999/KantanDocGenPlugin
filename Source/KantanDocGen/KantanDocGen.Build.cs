/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright (C) 2016-2017 Cameron Angus. All Rights Reserved.

using UnrealBuildTool;

public class KantanDocGen : ModuleRules
{
    public KantanDocGen(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bEnforceIWYU = true;

        PublicDependencyModuleNames.AddRange(
            new string[] {
                "AnimGraph",
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
                "Slate",
                "SlateCore",
                "UnrealEd",
                "PropertyEditor",
                "EditorStyle",
                "BlueprintGraph",
                "GraphEditor",
                "MainFrame",
                "LevelEditor",
                "XmlParser",
                "UMG",
                "Projects",
                "ImageWriteQueue"
            }
        );
    }
}
