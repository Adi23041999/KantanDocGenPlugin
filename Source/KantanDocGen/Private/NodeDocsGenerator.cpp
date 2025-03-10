// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Copyright (C) 2016-2017 Cameron Angus. All Rights Reserved.

#pragma once

#include "NodeDocsGenerator.h"
#include "KantanDocGenLog.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "NodeFactory.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintBoundNodeSpawner.h"
#include "BlueprintComponentNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintBoundEventNodeSpawner.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Message.h"
#include "HighResScreenshot.h"
#include "XmlFile.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "ThreadingHelpers.h"
#include "Stats/StatsMisc.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersionComparison.h"
#include "AnimGraphNode_Base.h"

FNodeDocsGenerator::~FNodeDocsGenerator()
{
	CleanUp();
}

bool FNodeDocsGenerator::GT_Init(FString const& InDocsTitle, FString const& InOutputDir, UClass* BlueprintContextClass)
{
	DummyBP = CastChecked<UBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		BlueprintContextClass, ::GetTransientPackage(), NAME_None, EBlueprintType::BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None));
	if (!DummyBP.IsValid())
	{
		return false;
	}

	Graph = FBlueprintEditorUtils::CreateNewGraph(DummyBP.Get(), TEXT("TempoGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	DummyBP->AddToRoot();
	Graph->AddToRoot();

	GraphPanel = SNew(SGraphPanel).GraphObj(Graph.Get());
	// We want full detail for rendering, passing a super-high zoom value will guarantee the highest LOD.
	GraphPanel->RestoreViewSettings(FVector2D(0, 0), 10.0f);

	DocsTitle = InDocsTitle;

	IndexXml = InitIndexXml(DocsTitle);
	ClassDocsMap.Empty();

	OutputDir = InOutputDir;

	return true;
}

UK2Node* FNodeDocsGenerator::GT_DocumentSimpleObject(UObject* SourceObject, FNodeProcessingState& OutState)
{
	if (UEnum* Enum = Cast<UEnum>(SourceObject))
	{
		UpdateIndexDocWithEnum(IndexXml.Get(), Enum);
		return nullptr;
	}
	else if (UScriptStruct* Struct = Cast<UScriptStruct>(SourceObject))
	{
		UpdateIndexDocWithStruct(IndexXml.Get(), Struct);
		return nullptr;
	}

	// InitializeForSpawner supports "nullptr" spawner for simple classes (handled gracefully in IsSpawnerDocumentable)
	return GT_InitializeForSpawner(nullptr, SourceObject, OutState);
}

UK2Node* FNodeDocsGenerator::GT_InitializeForSpawner(UBlueprintNodeSpawner* Spawner, UObject* SourceObject, FNodeProcessingState& OutState)
{
	UK2Node* K2NodeInst = nullptr;
	bool bIsDocumentable = IsSpawnerDocumentable(Spawner, SourceObject->IsA<UBlueprint>());

	// fix crash in UE5 while trying to spawn an AnimGraphNode into an Actor Graph
	UClass* ObjectAsClass = Cast<UClass>(SourceObject);
	bIsDocumentable &= ObjectAsClass == nullptr || !ObjectAsClass->GetDefaultObject()->IsA<UAnimGraphNode_Base>();
	if (bIsDocumentable)
	{
		// Spawn an instance into the graph
		auto NodeInst = Spawner->Invoke(Graph.Get(), IBlueprintNodeBinder::FBindingSet{}, FVector2D(0, 0));

		// Currently Blueprint nodes only
		K2NodeInst = Cast<UK2Node>(NodeInst);

		if (K2NodeInst == nullptr)
		{
			UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to create node from spawner of class %s with node class %s (Object %s)."), *Spawner->GetClass()->GetName(), Spawner->NodeClass ? *Spawner->NodeClass->GetName() : TEXT("None"), *GetNameSafe(SourceObject));
			return nullptr;
		}
	}

	auto AssociatedClass = MapToAssociatedClass(K2NodeInst, SourceObject);

	if (!ClassDocsMap.Contains(AssociatedClass))
	{
		// New class xml file needs adding
		ClassDocsMap.Add(AssociatedClass, InitClassDocXml(AssociatedClass));
		// Also update the index xml
		UpdateIndexDocWithClass(IndexXml.Get(), AssociatedClass, SourceObject);
	}

	if (bIsDocumentable)
	{
		OutState = FNodeProcessingState();
		OutState.ClassDocXml = ClassDocsMap.FindChecked(AssociatedClass);
		OutState.ClassDocsPath = OutputDir / GetClassDocId(AssociatedClass);
	}
	return K2NodeInst;
}

bool FNodeDocsGenerator::GT_Finalize(FString OutputPath)
{
	if (!SaveClassDocXml(OutputPath))
	{
		return false;
	}

	if (!SaveIndexXml(OutputPath))
	{
		return false;
	}

	return true;
}

void FNodeDocsGenerator::CleanUp()
{
	if (GraphPanel.IsValid())
	{
		GraphPanel.Reset();
	}

	if (DummyBP.IsValid())
	{
		DummyBP->RemoveFromRoot();
		DummyBP.Reset();
	}
	if (Graph.IsValid())
	{
		Graph->RemoveFromRoot();
		Graph.Reset();
	}
}

bool FNodeDocsGenerator::GenerateNodeImage(UEdGraphNode* Node, FNodeProcessingState& State)
{
	SCOPE_SECONDS_COUNTER(GenerateNodeImageTime);

	AdjustNodeForSnapshot(Node);

	FString NodeName = GetNodeDocId(Node);

	bool bSuccess = DocGenThreads::RunOnGameThreadRetVal(
		[&]
		{
			auto NodeWidget = FNodeFactory::CreateNodeWidget(Node);
			NodeWidget->SetOwner(GraphPanel.ToSharedRef());

			const bool bUseGammaCorrection = false;
			FWidgetRenderer Renderer(bUseGammaCorrection);
			Renderer.SetIsPrepassNeeded(true);
			auto RenderTarget = Renderer.DrawWidget(NodeWidget.ToSharedRef(), FVector2D(1024.0f, 1024.0f));

			auto Desired = NodeWidget->GetDesiredSize();

			FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
			FIntRect Rect = FIntRect(0, 0, (int32)Desired.X, (int32)Desired.Y);
			FReadSurfaceDataFlags ReadPixelFlags(RCM_UNorm);
			ReadPixelFlags.SetLinearToGamma(true); // @TODO: is this gamma correction, or something else?

			TArray<FColor> Pixels;
			if (RTResource->ReadPixels(Pixels, ReadPixelFlags, Rect) == false)
			{
				UE_LOG(LogKantanDocGen, Warning, TEXT("Failed to read pixels for node image."));
				return false;
			}

			IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
			TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);

			TArray<uint8> CompressedImage;

			const int32 X = (int32)Desired.X;
			const int32 Y = (int32)Desired.Y;
			if (!ImageWrapper->SetRaw(Pixels.GetData(), X * Y * sizeof(FColor), X, Y, ERGBFormat::BGRA, 8))
			{
				UE_LOG(LogKantanDocGen, Error, TEXT("Failed to set raw pixels for node %s."), *NodeName);
				return false;
			}

			CompressedImage = ImageWrapper->GetCompressed(90);

			State.RelImageBasePath = TEXT("../img");
			FString ImageBasePath = State.ClassDocsPath / TEXT("img"); // State.RelImageBasePath;
			FString ImgFilename = FString::Printf(TEXT("nd_img_%s.jpg"), *NodeName);
			FString ScreenshotSaveName = ImageBasePath / ImgFilename;

			if (!FFileHelper::SaveArrayToFile(CompressedImage, *ScreenshotSaveName))
			{
				UE_LOG(LogKantanDocGen, Error, TEXT("Failed to save %s."), *ScreenshotSaveName);
				return false;
			}
			State.ImageFilename = ImgFilename;
			return true;
		});
	return bSuccess;
}

inline FString WrapAsCDATA(FString const& InString)
{
	return TEXT("<![CDATA[") + InString + TEXT("]]>");
}

inline FXmlNode* AppendChild(FXmlNode* Parent, FString const& Name)
{
	Parent->AppendChildNode(Name, FString());
	return Parent->GetChildrenNodes().Last();
}

inline FXmlNode* AppendChildRaw(FXmlNode* Parent, FString const& Name, FString const& TextContent)
{
	Parent->AppendChildNode(Name, TextContent);
	return Parent->GetChildrenNodes().Last();
}

inline FXmlNode* AppendChildCDATA(FXmlNode* Parent, FString const& Name, FString const& TextContent)
{
	Parent->AppendChildNode(Name, WrapAsCDATA(TextContent));
	return Parent->GetChildrenNodes().Last();
}

// For K2 pins only!
bool ExtractPinInformation(UEdGraphPin* Pin, FString& OutName, FString& OutType, FString& OutDescription)
{
	FString Tooltip;
	Pin->GetOwningNode()->GetPinHoverText(*Pin, Tooltip);

	if (!Tooltip.IsEmpty())
	{
		// @NOTE: This is based on the formatting in UEdGraphSchema_K2::ConstructBasicPinTooltip.
		// If that is changed, this will fail!

		auto TooltipPtr = *Tooltip;

		// Parse name line
		FParse::Line(&TooltipPtr, OutName);
		// Parse type line
		FParse::Line(&TooltipPtr, OutType);

		// Currently there is an empty line here, but FParse::Line seems to gobble up empty lines as part of the previous call.
		// Anyway, attempting here to deal with this generically in case that weird behaviour changes.
		while (*TooltipPtr == TEXT('\n'))
		{
			FString Buf;
			FParse::Line(&TooltipPtr, Buf);
		}

		// What remains is the description
		OutDescription = TooltipPtr;
	}

	// @NOTE: Currently overwriting the name and type as suspect this is more robust to future engine changes.

	OutName = Pin->GetDisplayName().ToString();
	if (OutName.IsEmpty() && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		OutName = Pin->Direction == EEdGraphPinDirection::EGPD_Input ? TEXT("In") : TEXT("Out");
	}

	OutType = UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString();

	return true;
}

TSharedPtr<FXmlFile> FNodeDocsGenerator::InitIndexXml(FString const& IndexTitle)
{
	const FString FileTemplate = R"xxx(<?xml version="1.0" encoding="UTF-8"?>
<root></root>)xxx";

	TSharedPtr<FXmlFile> File = MakeShared<FXmlFile>(FileTemplate, EConstructMethod::ConstructFromBuffer);
	auto Root = File->GetRootNode();

	AppendChildCDATA(Root, TEXT("display_name"), IndexTitle);
	AppendChild(Root, TEXT("classes"));
	AppendChild(Root, TEXT("structs"));
	AppendChild(Root, TEXT("enums"));

	return File;
}

namespace
{
	FString GetClassDescription(const UClass* Class)
	{
		check(Class);
		if (Class->HasAllClassFlags(CLASS_Interface))
			return "## UInterface cannot be documented ##";

		FString ClassToolTip = Class->GetToolTipText().ToString();
		if (ClassToolTip != FBlueprintEditorUtils::GetFriendlyClassDisplayName(Class).ToString())
			return ClassToolTip;
		return "";
	}

	FString GetClassDocName(const UClass* Class)
	{
		check(Class);
		FString Name = Class->GetName();
		if (!Class->HasAnyClassFlags(CLASS_Native))
		{
			Name.RemoveFromEnd(TEXT("_C"));
			Name.RemoveFromStart(TEXT("SKEL_"));
		}
		static const FName DisplayName("DisplayName");
		if (Class->HasMetaData(DisplayName))
			return Name + " (" + Class->GetDisplayNameText().ToString() + ")";
		return Name;
	}

	FString GetNodeDescription(UEdGraphNode* Node)
	{
		check(Node);
		FString NodeDesc = Node->GetTooltipText().ToString();
		if (NodeDesc != Node->GetClass()->GetToolTipText().ToString())
		{
			int32 TargetIdx = NodeDesc.Find(TEXT("Target is "), ESearchCase::CaseSensitive);
			if (TargetIdx != INDEX_NONE)
			{
				NodeDesc = NodeDesc.Left(TargetIdx).TrimEnd();
			}
			return NodeDesc;
		}
		return "";
	}
};

TSharedPtr<FXmlFile> FNodeDocsGenerator::InitClassDocXml(UClass* Class)
{
	const FString FileTemplate = R"xxx(<?xml version="1.0" encoding="UTF-8"?>
<root></root>)xxx";

	TSharedPtr<FXmlFile> File = MakeShared<FXmlFile>(FileTemplate, EConstructMethod::ConstructFromBuffer);
	auto Root = File->GetRootNode();

	AppendChildCDATA(Root, TEXT("docs_name"), DocsTitle);
	AppendChildCDATA(Root, TEXT("id"), GetClassDocId(Class));
	AppendChildCDATA(Root, TEXT("display_name"), GetClassDocName(Class));
	AppendChildCDATA(Root, TEXT("description"), GetClassDescription(Class));

	static const FName PathKey("ModuleRelativePath");
	FString Path = Class->GetMetaData(PathKey);
	if (Path.IsEmpty())
	{
		Path = Class->GetPathName();
		Path.RemoveFromEnd("." + Class->GetName());
	}
	AppendChildCDATA(Root, TEXT("sourcepath"), Path);

	const UClass* Parent = Class->GetSuperClass();
	FString ClassTreeStr = *GetClassDocName(Class);
	while (nullptr != Parent)
	{
		ClassTreeStr = FString::Printf(TEXT("%s > %s"), *GetClassDocName(Parent), *ClassTreeStr);
		Parent = Parent->GetSuperClass();
	}
	AppendChildCDATA(Root, TEXT("classTree"), ClassTreeStr);

	FXmlNode* Props = AppendChild(Root, TEXT("properties"));
	for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if ((It->PropertyFlags & (CPF_BlueprintVisible | CPF_Edit)) != 0)
		{
			FXmlNode* Prop = AppendChild(Props, TEXT("property"));
			AppendChildCDATA(Prop, TEXT("type"), It->GetCPPType());
			FString DisplayName = It->GetDisplayNameText().ToString();
			AppendChildCDATA(Prop, TEXT("display_name"), DisplayName.Replace(TEXT(" "), TEXT("")));
			FString ToolTip = It->GetToolTipText().ToString();
#if UE_VERSION_OLDER_THAN(5, 2, 0)
			FString Fixup = DisplayName + ":" + LINE_TERMINATOR_ANSI;
			if (ToolTip.StartsWith(Fixup))
			{
				ToolTip = ToolTip.Replace(*Fixup, TEXT(""));
			}
#endif
			if (ToolTip != DisplayName)
				AppendChildCDATA(Prop, TEXT("description"), ToolTip);
		}
	}
	AppendChild(Root, TEXT("nodes"));

	return File;
}

bool FNodeDocsGenerator::UpdateIndexDocWithClass(FXmlFile* DocFile, UClass* Class, UObject* SourceObject)
{
	check(DocFile);
	check(Class);
	check(SourceObject);
	auto ClassId = GetClassDocId(Class);
	auto Classes = DocFile->GetRootNode()->FindChildNode(TEXT("classes"));
	auto ClassElem = AppendChild(Classes, TEXT("class"));
	AppendChildCDATA(ClassElem, TEXT("id"), ClassId);
	AppendChildCDATA(ClassElem, TEXT("display_name"), GetClassDocName(Class));
	const bool bIsNative = Class->HasAnyClassFlags(CLASS_Native);
	if (bIsNative)
	{
		TArray<FString> Groups;
		Class->GetClassGroupNames(Groups);
		if (Groups.Num() > 0)
			AppendChildCDATA(ClassElem, TEXT("group"), Groups[0]);
	}
	else
	{
		UBlueprint* BP = Cast<UBlueprint>(SourceObject);
		if (nullptr != BP)
		{
			if (!BP->BlueprintCategory.IsEmpty())
				AppendChildCDATA(ClassElem, TEXT("group"), BP->BlueprintCategory);
			else if (!BP->BlueprintNamespace.IsEmpty())
				AppendChildCDATA(ClassElem, TEXT("group"), BP->BlueprintNamespace);
		}
	}
	AppendChildCDATA(ClassElem, TEXT("type"), bIsNative ? "C++" : "Blueprint");
	AppendChildCDATA(ClassElem, TEXT("description"), GetClassDescription(Class));
	return true;
}

bool FNodeDocsGenerator::UpdateIndexDocWithEnum(FXmlFile* DocFile, UEnum* Enum)
{
	check(Enum);
	auto Classes = DocFile->GetRootNode()->FindChildNode(TEXT("enums"));
	auto ClassElem = AppendChild(Classes, TEXT("enum"));
	AppendChildCDATA(ClassElem, TEXT("display_name"), Enum->CppType);
	AppendChildCDATA(ClassElem, TEXT("description"), Enum->UField::GetToolTipText(false).ToString());

	FString ValuesStr;
	for (int32 Idx = 0; Idx < Enum->NumEnums(); ++Idx)
	{
		if (Idx != 0)
			ValuesStr += "\n";
		FString Name = Enum->GetNameStringByIndex(Idx);
		ValuesStr += Name;
		FString DisplayName = Enum->GetDisplayNameTextByIndex(Idx).ToString();
		if (Name != DisplayName.Replace(TEXT(" "), TEXT("")))
			ValuesStr += " (" + DisplayName + ")";
	}
	AppendChildCDATA(ClassElem, TEXT("values"), ValuesStr);
	return true;
}

bool FNodeDocsGenerator::UpdateIndexDocWithStruct(FXmlFile* DocFile, UScriptStruct* Struct)
{
	check(Struct);
	auto Classes = DocFile->GetRootNode()->FindChildNode(TEXT("structs"));
	auto ClassElem = AppendChild(Classes, TEXT("struct"));

	AppendChildCDATA(ClassElem, TEXT("display_name"), Struct->GetStructCPPName());
	AppendChildCDATA(ClassElem, TEXT("description"), Struct->GetToolTipText(false).ToString());

	FXmlNode* Props = AppendChild(ClassElem, TEXT("properties"));
	
	for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if ((It->PropertyFlags & (CPF_BlueprintVisible | CPF_Edit)) != 0)
		{
			FXmlNode* Prop = AppendChild(Props, TEXT("property"));
			AppendChildCDATA(Prop, TEXT("type"), It->GetCPPType());
			FString DisplayName = It->GetDisplayNameText().ToString();
			AppendChildCDATA(Prop, TEXT("display_name"), DisplayName.Replace(TEXT(" "), TEXT("")));
			FString ToolTip = It->GetToolTipText().ToString();
#if UE_VERSION_OLDER_THAN(5, 2, 0)
			FString Fixup = DisplayName + ":" + LINE_TERMINATOR_ANSI;
			if (ToolTip.StartsWith(Fixup))
			{
				ToolTip = ToolTip.Replace(*Fixup, TEXT(""));
			}
#endif
			if (ToolTip != DisplayName)
				AppendChildCDATA(Prop, TEXT("description"), ToolTip);
		}
	}
	return true;
}

bool FNodeDocsGenerator::UpdateClassDocWithNode(FXmlFile* DocFile, UEdGraphNode* Node)
{
	auto NodeId = GetNodeDocId(Node);
	auto Nodes = DocFile->GetRootNode()->FindChildNode(TEXT("nodes"));
	auto NodeElem = AppendChild(Nodes, TEXT("node"));
	AppendChildCDATA(NodeElem, TEXT("id"), NodeId);
	AppendChildCDATA(NodeElem, TEXT("shorttitle"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	AppendChildCDATA(NodeElem, TEXT("description"), GetNodeDescription(Node));
	return true;
}

inline bool ShouldDocumentPin(UEdGraphPin* Pin)
{
	return !Pin->bHidden;
}

bool FNodeDocsGenerator::GenerateNodeDocs(UK2Node* Node, FNodeProcessingState& State)
{
	SCOPE_SECONDS_COUNTER(GenerateNodeDocsTime);

	auto NodeDocsPath = State.ClassDocsPath / TEXT("nodes");
	FString DocFilePath = NodeDocsPath / (GetNodeDocId(Node) + TEXT(".xml"));

	const FString FileTemplate = R"xxx(<?xml version="1.0" encoding="UTF-8"?>
<root></root>)xxx";

	FXmlFile File(FileTemplate, EConstructMethod::ConstructFromBuffer);
	auto Root = File.GetRootNode();

	AppendChildCDATA(Root, TEXT("docs_name"), DocsTitle);
	// Since we pull these from the class xml file, the entries are already CDATA wrapped
	AppendChildRaw(Root, TEXT("class_id"), State.ClassDocXml->GetRootNode()->FindChildNode(TEXT("id"))->GetContent());
	AppendChildRaw(Root, TEXT("class_name"), State.ClassDocXml->GetRootNode()->FindChildNode(TEXT("display_name"))->GetContent());

	FString NodeShortTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	AppendChildCDATA(Root, TEXT("shorttitle"), NodeShortTitle.TrimEnd());

	FString NodeFullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	auto TargetIdx = NodeFullTitle.Find(TEXT("Target is "), ESearchCase::CaseSensitive);
	if (TargetIdx != INDEX_NONE)
	{
		NodeFullTitle = NodeFullTitle.Left(TargetIdx).TrimEnd();
	}
	AppendChildCDATA(Root, TEXT("fulltitle"), NodeFullTitle);

	AppendChildCDATA(Root, TEXT("description"), GetNodeDescription(Node));
	AppendChildCDATA(Root, TEXT("imgpath"), State.RelImageBasePath / State.ImageFilename);
	AppendChildCDATA(Root, TEXT("category"), Node->GetMenuCategory().ToString());

	auto Inputs = AppendChild(Root, TEXT("inputs"));
	for (auto Pin : Node->Pins)
	{
		if (Pin->Direction == EEdGraphPinDirection::EGPD_Input)
		{
			if (ShouldDocumentPin(Pin))
			{
				auto Input = AppendChild(Inputs, TEXT("param"));

				FString PinName, PinType, PinDesc;
				ExtractPinInformation(Pin, PinName, PinType, PinDesc);

				AppendChildCDATA(Input, TEXT("name"), PinName);
				AppendChildCDATA(Input, TEXT("type"), PinType);
				AppendChildCDATA(Input, TEXT("description"), PinDesc);
			}
		}
	}

	auto Outputs = AppendChild(Root, TEXT("outputs"));
	for (auto Pin : Node->Pins)
	{
		if (Pin->Direction == EEdGraphPinDirection::EGPD_Output)
		{
			if (ShouldDocumentPin(Pin))
			{
				auto Output = AppendChild(Outputs, TEXT("param"));

				FString PinName, PinType, PinDesc;
				ExtractPinInformation(Pin, PinName, PinType, PinDesc);

				AppendChildCDATA(Output, TEXT("name"), PinName);
				AppendChildCDATA(Output, TEXT("type"), PinType);
				AppendChildCDATA(Output, TEXT("description"), PinDesc);
			}
		}
	}

	if (!File.Save(DocFilePath))
	{
		return false;
	}

	if (!UpdateClassDocWithNode(State.ClassDocXml.Get(), Node))
	{
		return false;
	}

	return true;
}

bool FNodeDocsGenerator::SaveIndexXml(FString const& OutDir)
{
	auto Path = OutDir / TEXT("index.xml");
	IndexXml->Save(Path);

	return true;
}

bool FNodeDocsGenerator::SaveClassDocXml(FString const& OutDir)
{
	for (auto const& Entry : ClassDocsMap)
	{
		auto ClassId = GetClassDocId(Entry.Key.Get());
		auto Path = OutDir / ClassId / (ClassId + TEXT(".xml"));
		Entry.Value->Save(Path);
	}

	return true;
}

void FNodeDocsGenerator::AdjustNodeForSnapshot(UEdGraphNode* Node)
{
	// Hide default value box containing 'self' for Target pin
	if (auto K2_Schema = Cast<UEdGraphSchema_K2>(Node->GetSchema()))
	{
		if (auto TargetPin = Node->FindPin(K2_Schema->PN_Self))
		{
			TargetPin->bDefaultValueIsIgnored = true;
		}
	}
}

FString FNodeDocsGenerator::GetClassDocId(UClass* Class)
{
	return Class->GetName();
}

FString FNodeDocsGenerator::GetNodeDocId(UEdGraphNode* Node)
{
	// @TODO: Not sure this is right thing to use
	return Node->GetDocumentationExcerptName();
}

#include "BlueprintVariableNodeSpawner.h"
#include "BlueprintDelegateNodeSpawner.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"

/*
This takes a graph node object and attempts to map it to the class which the node conceptually belong to.
If there is no special mapping for the node, the function determines the class from the source object.
*/
UClass* FNodeDocsGenerator::MapToAssociatedClass(UK2Node* NodeInst, UObject* Source)
{
	// For nodes derived from UK2Node_CallFunction, associate with the class owning the called function.
	if (auto FuncNode = Cast<UK2Node_CallFunction>(NodeInst))
	{
		auto Func = FuncNode->GetTargetFunction();
		if (Func)
		{
			UClass* OwnerClass = Func->GetOwnerClass();
			if (OwnerClass->ClassGeneratedBy)
			{
				UBlueprint* ClassBP = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy);
				if (ClassBP && ClassBP->GeneratedClass)
					return ClassBP->GeneratedClass;
			}
			return OwnerClass;
		}
	}

	// Default fallback
	if (auto SourceClass = Cast<UClass>(Source))
	{
		return SourceClass;
	}
	else if (auto SourceBP = Cast<UBlueprint>(Source))
	{
		return SourceBP->GeneratedClass;
	}
	else
	{
		return nullptr;
	}
}

bool FNodeDocsGenerator::IsSpawnerDocumentable(UBlueprintNodeSpawner* Spawner, bool bIsBlueprint)
{
	if (nullptr == Spawner)
		return false;
	// Spawners of or deriving from the following classes will be excluded
	static const TSubclassOf<UBlueprintNodeSpawner> ExcludedSpawnerClasses[] = { UBlueprintVariableNodeSpawner::StaticClass(), UBlueprintDelegateNodeSpawner::StaticClass(),
		UBlueprintBoundNodeSpawner::StaticClass(), UBlueprintComponentNodeSpawner::StaticClass(), UBlueprintBoundEventNodeSpawner::StaticClass() };

	// Spawners of or deriving from the following classes will be excluded in a blueprint context
	static const TSubclassOf<UBlueprintNodeSpawner> BlueprintOnlyExcludedSpawnerClasses[] = {
		UBlueprintEventNodeSpawner::StaticClass(),
	};

	// Spawners for nodes of these types (or their subclasses) will be excluded
	static const TSubclassOf<UK2Node> ExcludedNodeClasses[] = {
		UK2Node_DynamicCast::StaticClass(),
		UK2Node_Message::StaticClass(),
	};

	// Function spawners for functions with any of the following metadata tags will also be excluded
	static const FName ExcludedFunctionMeta[] = { TEXT("BlueprintAutocast") };

	static const uint32 PermittedAccessSpecifiers = (FUNC_Public | FUNC_Protected);

	for (auto ExclSpawnerClass : ExcludedSpawnerClasses)
	{
		if (Spawner->IsA(ExclSpawnerClass))
		{
			return false;
		}
	}

	if (bIsBlueprint)
	{
		for (auto ExclSpawnerClass : BlueprintOnlyExcludedSpawnerClasses)
		{
			if (Spawner->IsA(ExclSpawnerClass))
			{
				return false;
			}
		}
	}

	for (auto ExclNodeClass : ExcludedNodeClasses)
	{
		if (Spawner->NodeClass->IsChildOf(ExclNodeClass))
		{
			return false;
		}
	}

	if (auto FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
	{
		auto Func = FuncSpawner->GetFunction();

		// @NOTE: We exclude based on access level, but only if this is not a spawner for a blueprint event
		// (custom events do not have any access specifiers)
		if ((Func->FunctionFlags & FUNC_BlueprintEvent) == 0 && (Func->FunctionFlags & PermittedAccessSpecifiers) == 0)
		{
			return false;
		}

		for (auto const& Meta : ExcludedFunctionMeta)
		{
			if (Func->HasMetaData(Meta))
			{
				return false;
			}
		}
	}

	return true;
}
