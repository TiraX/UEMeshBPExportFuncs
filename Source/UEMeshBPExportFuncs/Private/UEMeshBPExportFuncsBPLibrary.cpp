// Copyright Epic Games, Inc. All Rights Reserved.

#include "UEMeshBPExportFuncsBPLibrary.h"
#include "UEMeshBPExportFuncs.h"

#if WITH_EDITOR
#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"
#include "ImageUtils.h"
#include "Exporters/FbxExportOption.h"
#include "Factories/FbxImportUI.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "AssetImportTask.h"
#include "Factories/FbxFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#endif

UUEMeshBPExportFuncsBPLibrary::UUEMeshBPExportFuncsBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}

#if WITH_EDITOR
// Helper function: Get relative path from /Game
static FString GetRelativePathFromGame(const FString& AssetPath)
{
	FString RelativePath = AssetPath;
	
	// Remove package name suffix (e.g., "/Game/MyAsset.MyAsset" -> "/Game/MyAsset")
	int32 DotIndex;
	if (RelativePath.FindLastChar('.', DotIndex))
	{
		RelativePath = RelativePath.Left(DotIndex);
	}
	
	// Remove /Game prefix
	if (RelativePath.StartsWith(TEXT("/Game/")))
	{
		RelativePath = RelativePath.RightChop(6); // Remove "/Game/"
	}
	
	return RelativePath;
}

// Helper function: Export texture to PNG
static bool ExportTextureToPNG(UTexture2D* Texture, const FString& OutputPath)
{
	if (!Texture || OutputPath.IsEmpty())
	{
		return false;
	}
	
	// Check if file already exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*OutputPath))
	{
		UE_LOG(LogTemp, Log, TEXT("Texture PNG already exists, skipping: %s"), *OutputPath);
		return true;
	}
	
	// Export using UAssetExportTask
	UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
	ExportTask->Object = Texture;
	ExportTask->Exporter = nullptr;
	ExportTask->Filename = OutputPath;
	ExportTask->bSelected = false;
	ExportTask->bReplaceIdentical = false;
	ExportTask->bPrompt = false;
	ExportTask->bUseFileArchive = false;
	ExportTask->bWriteEmptyFiles = false;
	
	bool bSuccess = UExporter::RunAssetExportTask(ExportTask);
	
	if (bSuccess && ExportTask->Errors.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Exported texture to: %s"), *OutputPath);
		return true;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export texture: %s"), *Texture->GetName());
		return false;
	}
}

// Helper function: Export skeletal mesh to FBX
static bool ExportSkeletalMeshToFBX(USkeletalMesh* SkeletalMesh, const FString& OutputPath)
{
	if (!SkeletalMesh || OutputPath.IsEmpty())
	{
		return false;
	}
	
	// Check if file already exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*OutputPath))
	{
		UE_LOG(LogTemp, Log, TEXT("FBX already exists, skipping: %s"), *OutputPath);
		return true;
	}
	
	// Export using UAssetExportTask
	UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
	ExportTask->Object = SkeletalMesh;
	ExportTask->Exporter = nullptr;
	ExportTask->Filename = OutputPath;
	ExportTask->bSelected = false;
	ExportTask->bReplaceIdentical = false;
	ExportTask->bPrompt = false;
	ExportTask->bUseFileArchive = false;
	ExportTask->bWriteEmptyFiles = false;
	ExportTask->bAutomated = true;
	
	UFbxExportOption* FbxOptions = NewObject<UFbxExportOption>();
	FbxOptions->bExportMorphTargets = false;
	FbxOptions->bExportPreviewMesh = false;
	FbxOptions->bExportLocalTime = false;
	FbxOptions->bForceFrontXAxis = false;
	FbxOptions->Collision = false;
	FbxOptions->LevelOfDetail = false;

	ExportTask->Options = FbxOptions;
	
	bool bSuccess = UExporter::RunAssetExportTask(ExportTask);
	
	if (bSuccess && ExportTask->Errors.Num() == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Exported skeletal mesh to: %s"), *OutputPath);
		return true;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export skeletal mesh: %s"), *SkeletalMesh->GetName());
		return false;
	}
}

// Helper function: Collect and export material parameters
static FString ExportMaterialToJSON(UMaterialInterface* Material, const FString& ExportBasePath, TSet<UTexture*>& ProcessedTextures)
{
	if (!Material)
	{
		return FString();
	}
	
	// Check if material JSON already exists
	FString MaterialRelativePath = GetRelativePathFromGame(Material->GetPathName());
	FString MaterialJsonPath = FPaths::Combine(ExportBasePath, MaterialRelativePath + TEXT("_material.json"));
	
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*MaterialJsonPath))
	{
		return MaterialRelativePath + TEXT("_material.json");
	}
	
	TSharedPtr<FJsonObject> MaterialJson = MakeShareable(new FJsonObject);
	MaterialJson->SetStringField(TEXT("MaterialName"), Material->GetName());
	MaterialJson->SetStringField(TEXT("MaterialAssetPath"), Material->GetPathName());
	
	// Collect scalar parameters
	TArray<FMaterialParameterInfo> ScalarParameterInfos;
	TArray<FGuid> ScalarParameterIds;
	Material->GetAllScalarParameterInfo(ScalarParameterInfos, ScalarParameterIds);
	
	TArray<TSharedPtr<FJsonValue>> ScalarParamsArray;
	for (const FMaterialParameterInfo& ParamInfo : ScalarParameterInfos)
	{
		float ParamValue = 0.0f;
		if (Material->GetScalarParameterValue(ParamInfo, ParamValue))
		{
			TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
			ParamJson->SetStringField(TEXT("Name"), ParamInfo.Name.ToString());
			ParamJson->SetNumberField(TEXT("Value"), ParamValue);
			ScalarParamsArray.Add(MakeShareable(new FJsonValueObject(ParamJson)));
		}
	}
	MaterialJson->SetArrayField(TEXT("ScalarParameters"), ScalarParamsArray);
	
	// Collect vector parameters
	TArray<FMaterialParameterInfo> VectorParameterInfos;
	TArray<FGuid> VectorParameterIds;
	Material->GetAllVectorParameterInfo(VectorParameterInfos, VectorParameterIds);
	
	TArray<TSharedPtr<FJsonValue>> VectorParamsArray;
	for (const FMaterialParameterInfo& ParamInfo : VectorParameterInfos)
	{
		FLinearColor ParamValue;
		if (Material->GetVectorParameterValue(ParamInfo, ParamValue))
		{
			TSharedPtr<FJsonObject> ParamJson = MakeShareable(new FJsonObject);
			ParamJson->SetStringField(TEXT("Name"), ParamInfo.Name.ToString());
			
			TSharedPtr<FJsonObject> ColorJson = MakeShareable(new FJsonObject);
			ColorJson->SetNumberField(TEXT("R"), ParamValue.R);
			ColorJson->SetNumberField(TEXT("G"), ParamValue.G);
			ColorJson->SetNumberField(TEXT("B"), ParamValue.B);
			ColorJson->SetNumberField(TEXT("A"), ParamValue.A);
			ParamJson->SetObjectField(TEXT("Value"), ColorJson);
			
			VectorParamsArray.Add(MakeShareable(new FJsonValueObject(ParamJson)));
		}
	}
	MaterialJson->SetArrayField(TEXT("VectorParameters"), VectorParamsArray);
	
	// Collect texture parameters
	TArray<FMaterialParameterInfo> TextureParameterInfos;
	TArray<FGuid> TextureParameterIds;
	Material->GetAllTextureParameterInfo(TextureParameterInfos, TextureParameterIds);
	
	TArray<TSharedPtr<FJsonValue>> TextureParamsArray;
	for (const FMaterialParameterInfo& ParamInfo : TextureParameterInfos)
	{
		UTexture* ParamTexture = nullptr;
		if (Material->GetTextureParameterValue(ParamInfo, ParamTexture) && ParamTexture)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(ParamTexture);
			if (Texture2D)
			{
				TSharedPtr<FJsonObject> TextureJson = MakeShareable(new FJsonObject);
				TextureJson->SetStringField(TEXT("ParameterName"), ParamInfo.Name.ToString());
				TextureJson->SetStringField(TEXT("TextureAssetPath"), Texture2D->GetPathName());
				
				// Get relative path and construct export path
				FString TextureRelativePath = GetRelativePathFromGame(Texture2D->GetPathName());
				FString TexturePNGPath = FPaths::Combine(ExportBasePath, TextureRelativePath + TEXT(".png"));
				
				// Ensure directory exists
				FString TextureDir = FPaths::GetPath(TexturePNGPath);
				if (!PlatformFile.DirectoryExists(*TextureDir))
				{
					PlatformFile.CreateDirectoryTree(*TextureDir);
				}
				
				// Export texture if not already processed
				if (!ProcessedTextures.Contains(Texture2D))
				{
					ExportTextureToPNG(Texture2D, TexturePNGPath);
					ProcessedTextures.Add(Texture2D);
				}
				
				// Store relative path in JSON
				TextureJson->SetStringField(TEXT("ExportedPNGPath"), TextureRelativePath + TEXT(".png"));
				
				TextureParamsArray.Add(MakeShareable(new FJsonValueObject(TextureJson)));
			}
		}
	}
	MaterialJson->SetArrayField(TEXT("TextureParameters"), TextureParamsArray);
	
	// Ensure directory exists
	FString MaterialDir = FPaths::GetPath(MaterialJsonPath);
	if (!PlatformFile.DirectoryExists(*MaterialDir))
	{
		PlatformFile.CreateDirectoryTree(*MaterialDir);
	}
	
	FString JsonString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(MaterialJson.ToSharedRef(), JsonWriter);
	
	if (FFileHelper::SaveStringToFile(JsonString, *MaterialJsonPath))
	{
		UE_LOG(LogTemp, Log, TEXT("Exported material JSON to: %s"), *MaterialJsonPath);
		return MaterialRelativePath + TEXT("_material.json");
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to export material JSON: %s"), *MaterialJsonPath);
		return FString();
	}
}

// Helper function: Process a single skeletal mesh
static TSharedPtr<FJsonObject> ProcessSkeletalMesh(USkeletalMesh* SkeletalMesh, const FString& ExportBasePath, TSet<UTexture*>& ProcessedTextures)
{
	if (!SkeletalMesh)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> MeshJson = MakeShareable(new FJsonObject);
	MeshJson->SetStringField(TEXT("MeshName"), SkeletalMesh->GetName());
	MeshJson->SetStringField(TEXT("MeshAssetPath"), SkeletalMesh->GetPathName());
	
	// Get relative path and construct FBX export path
	FString MeshRelativePath = GetRelativePathFromGame(SkeletalMesh->GetPathName());
	FString FBXPath = FPaths::Combine(ExportBasePath, MeshRelativePath + TEXT(".fbx"));
	
	// Ensure directory exists
	FString MeshDir = FPaths::GetPath(FBXPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*MeshDir))
	{
		PlatformFile.CreateDirectoryTree(*MeshDir);
	}
	
	// Export skeletal mesh to FBX
	if (ExportSkeletalMeshToFBX(SkeletalMesh, FBXPath))
	{
		MeshJson->SetStringField(TEXT("ExportedFBXPath"), MeshRelativePath + TEXT(".fbx"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to export skeletal mesh: %s"), *SkeletalMesh->GetName());
		return nullptr;
	}
	
	// Process materials
	const TArray<FSkeletalMaterial>& SkeletalMaterials = SkeletalMesh->GetMaterials();
	TArray<TSharedPtr<FJsonValue>> MaterialsArray;
	
	for (int32 MatIdx = 0; MatIdx < SkeletalMaterials.Num(); MatIdx++)
	{
		UMaterialInterface* Material = SkeletalMaterials[MatIdx].MaterialInterface;
		if (Material)
		{
			TSharedPtr<FJsonObject> MaterialRefJson = MakeShareable(new FJsonObject);
			MaterialRefJson->SetNumberField(TEXT("MaterialSlotIndex"), MatIdx);
			MaterialRefJson->SetStringField(TEXT("MaterialSlotName"), SkeletalMaterials[MatIdx].MaterialSlotName.ToString());
			
			// Export material and get JSON path
			FString MaterialJsonPath = ExportMaterialToJSON(Material, ExportBasePath, ProcessedTextures);
			if (!MaterialJsonPath.IsEmpty())
			{
				MaterialRefJson->SetStringField(TEXT("MaterialJSONPath"), MaterialJsonPath);
			}
			
			MaterialsArray.Add(MakeShareable(new FJsonValueObject(MaterialRefJson)));
		}
	}
	
	MeshJson->SetArrayField(TEXT("Materials"), MaterialsArray);
	
	return MeshJson;
}
#endif

bool UUEMeshBPExportFuncsBPLibrary::ExportSkelMeshes(AActor* Actor, const FString& ExportName, const FString& ExportPath)
{
#if WITH_EDITOR
	if (!Actor)
	{
		UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: Actor is null"));
		return false;
	}
	
	if (ExportPath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: ExportPath is empty"));
		return false;
	}
	
	// Ensure export directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*ExportPath))
	{
		if (!PlatformFile.CreateDirectoryTree(*ExportPath))
		{
			UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: Failed to create directory: %s"), *ExportPath);
			return false;
		}
	}
	
	// Get all skeletal mesh components
	TArray<USkeletalMeshComponent*> SkelMeshComponents;
	Actor->GetComponents<USkeletalMeshComponent>(SkelMeshComponents);
	
	if (SkelMeshComponents.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ExportSkelMeshes: No SkeletalMeshComponent found in Actor"));
		return false;
	}
	
	// Track processed meshes to avoid duplicates
	TSet<USkeletalMesh*> ProcessedMeshes;
	TSet<UTexture*> ProcessedTextures;
	TArray<TSharedPtr<FJsonValue>> MeshesArray;
	
	// Process each skeletal mesh component
	for (USkeletalMeshComponent* SkelMeshComp : SkelMeshComponents)
	{
		if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
		{
			continue;
		}
		
		USkeletalMesh* SkelMesh = Cast<USkeletalMesh>(SkelMeshComp->GetSkeletalMeshAsset());
		if (!SkelMesh || ProcessedMeshes.Contains(SkelMesh))
		{
			continue;
		}
		
		ProcessedMeshes.Add(SkelMesh);
		
		// Process skeletal mesh
		TSharedPtr<FJsonObject> MeshJson = ProcessSkeletalMesh(SkelMesh, ExportPath, ProcessedTextures);
		if (MeshJson.IsValid())
		{
			MeshesArray.Add(MakeShareable(new FJsonValueObject(MeshJson)));
		}
	}
	
	// Create actor-level JSON
	TSharedPtr<FJsonObject> ActorJson = MakeShareable(new FJsonObject);
	ActorJson->SetStringField(TEXT("ActorName"), Actor->GetName());
	ActorJson->SetArrayField(TEXT("SkeletalMeshes"), MeshesArray);
	
	// Save actor JSON
	FString ActorJsonPath = FPaths::Combine(ExportPath, ExportName + TEXT(".json"));
	FString JsonString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(ActorJson.ToSharedRef(), JsonWriter);
	
	if (FFileHelper::SaveStringToFile(JsonString, *ActorJsonPath))
	{
		UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Successfully exported actor JSON to: %s"), *ActorJsonPath);
		UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Export completed. Processed %d meshes, %d textures"),
			ProcessedMeshes.Num(), ProcessedTextures.Num());
		return true;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: Failed to export actor JSON to: %s"), *ActorJsonPath);
		return false;
	}
	
#else
	UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: This function is only available in Editor"));
	return false;
#endif
}

TArray<FString> UUEMeshBPExportFuncsBPLibrary::ListFiles(const FString& Path, const FString& FilterString, bool bRecursive)
{
	TArray<FString> Result;
	
	// Check if path exists
	if (!FPaths::DirectoryExists(Path))
	{
		UE_LOG(LogTemp, Warning, TEXT("ListFiles: Directory does not exist: %s"), *Path);
		return Result;
	}
	
	// Prepare search pattern
	FString SearchPattern = FilterString.IsEmpty() ? TEXT("*.*") : FilterString;
	FString FullSearchPath = FPaths::Combine(Path, SearchPattern);
	
	// Find files in current directory
	IFileManager& FileManager = IFileManager::Get();
	
	if (bRecursive)
	{
		// Recursive search
		FileManager.FindFilesRecursive(Result, *Path, *SearchPattern, true, false);
	}
	else
	{
		// Non-recursive search
		TArray<FString> FoundFiles;
		FileManager.FindFiles(FoundFiles, *FullSearchPath, true, false);
		
		// Convert relative paths to full paths
		for (const FString& FileName : FoundFiles)
		{
			Result.Add(FPaths::Combine(Path, FileName));
		}
	}
	
	return Result;
}

bool UUEMeshBPExportFuncsBPLibrary::ImportMesh(const FString& TargetUEPath, const FString& MeshPath, bool bImportMaterial, bool bImportTexture, bool bImportSkeleton, UObject* ParentMaterialAsset)
{
#if WITH_EDITOR
	// Check if file exists
	if (!FPaths::FileExists(MeshPath))
	{
		UE_LOG(LogTemp, Error, TEXT("ImportMesh: File does not exist: %s"), *MeshPath);
		return false;
	}
	
	// Check if file is FBX
	FString Extension = FPaths::GetExtension(MeshPath).ToLower();
	if (Extension != TEXT("fbx"))
	{
		UE_LOG(LogTemp, Error, TEXT("ImportMesh: Only FBX files are supported, got: %s"), *Extension);
		return false;
	}
	
	// Create FBX factory
	UFbxFactory* FbxFactory = NewObject<UFbxFactory>();
	if (!FbxFactory)
	{
		UE_LOG(LogTemp, Error, TEXT("ImportMesh: Failed to create FbxFactory"));
		return false;
	}
	
	// Configure import settings
	FbxFactory->EnableShowOption();
	if (FbxFactory->ImportUI)
	{
		// Disable material import for now
		FbxFactory->ImportUI->bImportAsSkeletal = bImportSkeleton;
		FbxFactory->ImportUI->bImportMaterials = bImportMaterial;
		FbxFactory->ImportUI->bImportTextures = bImportTexture;
		FbxFactory->ImportUI->bImportAnimations = false;
		FbxFactory->ImportUI->bCreatePhysicsAsset = false;
		
		// Set automated import
		FbxFactory->ImportUI->bAutomatedImportShouldDetectType = true;
	}
	
	// Create import task
	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	ImportTask->AddToRoot();
	ImportTask->bAutomated = true;
	ImportTask->bReplaceExisting = true;
	ImportTask->bSave = false;
	ImportTask->Filename = MeshPath;
	ImportTask->DestinationPath = TargetUEPath;
	ImportTask->Factory = FbxFactory;
	ImportTask->Options = FbxFactory->ImportUI;
	
	// Set factory import task
	FbxFactory->SetAssetImportTask(ImportTask);
	
	// Execute import
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<UAssetImportTask*> ImportTasks;
	ImportTasks.Add(ImportTask);
	AssetToolsModule.Get().ImportAssetTasks(ImportTasks);
	
	// Check if import was successful
	bool bSuccess = ImportTask->ImportedObjectPaths.Num() > 0;
	
	if (bSuccess)
	{
		UE_LOG(LogTemp, Log, TEXT("ImportMesh: Successfully imported %d objects from %s"), ImportTask->ImportedObjectPaths.Num(), *MeshPath);
		for (const FString& ObjectPath : ImportTask->ImportedObjectPaths)
		{
			UE_LOG(LogTemp, Log, TEXT("  - %s"), *ObjectPath);
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("ImportMesh: Failed to import mesh from %s"), *MeshPath);
	}
	
	// Clean up
	ImportTask->RemoveFromRoot();
	
	return bSuccess;
#else
	UE_LOG(LogTemp, Error, TEXT("ImportMesh: This function is only available in editor builds"));
	return false;
#endif
}

