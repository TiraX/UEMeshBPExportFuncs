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
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/TextureFactory.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
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

#if WITH_EDITOR
// Helper function: Import texture from file path
static UTexture2D* ImportTextureFromFile(const FString& FilePath, const FString& DestinationPath, bool bSRGB, TextureGroup LODGroup = TEXTUREGROUP_World)
{
	// Check if file exists
	if (!FPaths::FileExists(FilePath))
	{
		UE_LOG(LogTemp, Warning, TEXT("ImportTextureFromFile: File does not exist: %s"), *FilePath);
		return nullptr;
	}
	
	// Get texture name from file path
	FString TextureName = FPaths::GetBaseFilename(FilePath);
	FString PackageName = DestinationPath / TextureName;
	
	// Check if texture already exists
	UPackage* ExistingPackage = FindPackage(nullptr, *PackageName);
	if (ExistingPackage)
	{
		UTexture2D* ExistingTexture = FindObject<UTexture2D>(ExistingPackage, *TextureName);
		if (ExistingTexture)
		{
			UE_LOG(LogTemp, Log, TEXT("Texture already exists, skipping: %s"), *PackageName);
			return ExistingTexture;
		}
	}
	
	// Create texture factory
	UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
	TextureFactory->SuppressImportOverwriteDialog();
	TextureFactory->bUseHashAsGuid = true;
	
	// Load texture data
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load texture file: %s"), *FilePath);
		return nullptr;
	}
	
	// Create package
	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	
	// Import texture
	const uint8* BufferStart = FileData.GetData();
	const uint8* BufferEnd = BufferStart + FileData.Num();
	
	UTexture2D* Texture = Cast<UTexture2D>(TextureFactory->FactoryCreateBinary(
		UTexture2D::StaticClass(),
		Package,
		*TextureName,
		RF_Standalone | RF_Public,
		nullptr,
		*FPaths::GetExtension(FilePath),
		BufferStart,
		BufferEnd,
		nullptr
	));
	
	if (Texture)
	{
		// Set texture properties
		Texture->SRGB = bSRGB;
		Texture->CompressionSettings = bSRGB ? TC_Default : TC_Normalmap;
		Texture->LODGroup = LODGroup;
		
		// Notify asset registry
		FAssetRegistryModule::AssetCreated(Texture);
		Package->MarkPackageDirty();
		
		UE_LOG(LogTemp, Log, TEXT("Successfully imported texture: %s"), *PackageName);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to import texture: %s"), *FilePath);
	}
	
	return Texture;
}

// Helper function: Extract relative path and import texture
static UTexture2D* ImportTextureWithRelativePath(const FString& _TexturePath, const FString& _TargetUEPath, const FString& SourceFbxPath, bool bSRGB, TextureCompressionSettings CompressionSettings = TC_Default, TextureGroup LODGroup = TEXTUREGROUP_World)
{
	FString TexturePath = _TexturePath.Replace(TEXT("\\"), TEXT("/"));
	if (!FPaths::FileExists(TexturePath))
	{
		return nullptr;
	}
	
	// Extract relative path from absolute path
	FString SourceRoot = SourceFbxPath.EndsWith(TEXT("/")) ? SourceFbxPath : SourceFbxPath + TEXT("/");
	FString RelativePath = TexturePath;
	int32 GameIndex = RelativePath.Find(SourceRoot, ESearchCase::IgnoreCase);
	if (GameIndex != INDEX_NONE)
	{
		RelativePath = RelativePath.RightChop(GameIndex + SourceRoot.Len());
		RelativePath = FPaths::GetPath(RelativePath);
	}
	else
	{
		RelativePath = FPaths::GetPath(FPaths::GetBaseFilename(TexturePath));
	}
	
	FString TargetUEPath = _TargetUEPath.EndsWith(TEXT("/")) ? _TargetUEPath : _TargetUEPath + TEXT("/");
	FString DestPath = TargetUEPath + RelativePath;
	UTexture2D* Texture = ImportTextureFromFile(TexturePath, DestPath, bSRGB, LODGroup);
	
	// Apply compression settings if texture was imported
	if (Texture && CompressionSettings != TC_Default)
	{
		Texture->CompressionSettings = CompressionSettings;
		Texture->UpdateResource();
	}
	
	return Texture;
}

// Helper function: Import material from JSON
static void ImportMaterialFromJson(const FString& JsonPath, const FString& TargetUEPath, const FString& SourceFbxPath, const TArray<FString>& ImportedObjectPaths, UObject* ParentMaterialAsset)
{
	// Check if JSON file exists
	if (!FPaths::FileExists(JsonPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("ImportMaterialFromJson: JSON file does not exist: %s"), *JsonPath);
		return;
	}
	
	// Load JSON file
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load JSON file: %s"), *JsonPath);
		return;
	}
	
	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to parse JSON file: %s"), *JsonPath);
		return;
	}
	
	// Check if parent material is valid
	UMaterialInterface* ParentMaterial = Cast<UMaterialInterface>(ParentMaterialAsset);
	if (!ParentMaterial)
	{
		UE_LOG(LogTemp, Error, TEXT("ImportMaterialFromJson: ParentMaterialAsset is not a valid material"));
		return;
	}
	
	// Process each imported object
	for (const FString& ObjectPath : ImportedObjectPaths)
	{
		// Load the imported mesh
		UObject* LoadedObject = LoadObject<UObject>(nullptr, *ObjectPath);
		if (!LoadedObject)
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to load imported object: %s"), *ObjectPath);
			continue;
		}
		
		// Get material slots
		TArray<FName> MaterialSlotNames;
		TArray<UMaterialInterface*> Materials;
		
		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObject))
		{
			// Get static mesh materials
			for (const FStaticMaterial& StaticMaterial : StaticMesh->GetStaticMaterials())
			{
				MaterialSlotNames.Add(StaticMaterial.MaterialSlotName);
				Materials.Add(StaticMaterial.MaterialInterface);
			}
		}
		else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(LoadedObject))
		{
			// Get skeletal mesh materials
			for (const FSkeletalMaterial& SkeletalMaterial : SkeletalMesh->GetMaterials())
			{
				MaterialSlotNames.Add(SkeletalMaterial.MaterialSlotName);
				Materials.Add(SkeletalMaterial.MaterialInterface);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Imported object is not a mesh: %s"), *ObjectPath);
			continue;
		}
		
		// Get mesh directory for material instance creation
		FString MeshPackagePath = FPaths::GetPath(ObjectPath);
		
		// Process each material slot
		for (int32 SlotIndex = 0; SlotIndex < MaterialSlotNames.Num(); SlotIndex++)
		{
			FString MaterialSlotName = MaterialSlotNames[SlotIndex].ToString();
			
			// Find material in JSON
			if (!JsonObject->HasField(MaterialSlotName))
			{
				UE_LOG(LogTemp, Warning, TEXT("Material not found in JSON: %s"), *MaterialSlotName);
				continue;
			}
			
			TSharedPtr<FJsonObject> MaterialJson = JsonObject->GetObjectField(MaterialSlotName);
			if (!MaterialJson.IsValid() || !MaterialJson->HasField(TEXT("Classified")))
			{
				UE_LOG(LogTemp, Warning, TEXT("Material JSON missing Classified field: %s"), *MaterialSlotName);
				continue;
			}
			
			TSharedPtr<FJsonObject> ClassifiedJson = MaterialJson->GetObjectField(TEXT("Classified"));
			
			// Import textures from Classified field
			UTexture2D* DiffuseTexture = nullptr;
			UTexture2D* NormalTexture = nullptr;
			UTexture2D* RoughnessTexture = nullptr;
			UTexture2D* MetallicTexture = nullptr;
						
			// Import textures from Classified field
			if (ClassifiedJson->HasField(TEXT("Diffuse")))
			{
				FString DiffusePath = ClassifiedJson->GetStringField(TEXT("Diffuse"));
				DiffuseTexture = ImportTextureWithRelativePath(DiffusePath, TargetUEPath, SourceFbxPath, true);
			}
			
			if (ClassifiedJson->HasField(TEXT("Normal")))
			{
				FString NormalPath = ClassifiedJson->GetStringField(TEXT("Normal"));
				NormalTexture = ImportTextureWithRelativePath(NormalPath, TargetUEPath, SourceFbxPath, false, TC_Normalmap, TEXTUREGROUP_WorldNormalMap);
			}
			
			if (ClassifiedJson->HasField(TEXT("Roughness")))
			{
				FString RoughnessPath = ClassifiedJson->GetStringField(TEXT("Roughness"));
				RoughnessTexture = ImportTextureWithRelativePath(RoughnessPath, TargetUEPath, SourceFbxPath, false, TC_Masks);
			}
			
			if (ClassifiedJson->HasField(TEXT("Metallic")))
			{
				FString MetallicPath = ClassifiedJson->GetStringField(TEXT("Metallic"));
				MetallicTexture = ImportTextureWithRelativePath(MetallicPath, TargetUEPath, SourceFbxPath, false, TC_Masks);
			}
			
			// Create material instance
			FString MaterialInstanceName = MaterialSlotName;
			FString MaterialInstancePackageName = MeshPackagePath / MaterialInstanceName;
			
			// Check if material instance already exists
			UPackage* ExistingMIPackage = FindPackage(nullptr, *MaterialInstancePackageName);
			UMaterialInstanceConstant* MaterialInstance = nullptr;
			
			if (ExistingMIPackage)
			{
				MaterialInstance = FindObject<UMaterialInstanceConstant>(ExistingMIPackage, *MaterialInstanceName);
				if (MaterialInstance)
				{
					UE_LOG(LogTemp, Log, TEXT("Material instance already exists, skipping: %s"), *MaterialInstancePackageName);
				}
			}
			
			if (!MaterialInstance)
			{
				// Create new material instance
				UPackage* MIPackage = CreatePackage(*MaterialInstancePackageName);
				MIPackage->FullyLoad();
				
				MaterialInstance = NewObject<UMaterialInstanceConstant>(
					MIPackage,
					*MaterialInstanceName,
					RF_Standalone | RF_Public
				);
				
				if (MaterialInstance)
				{
					// Set parent material
					MaterialInstance->SetParentEditorOnly(ParentMaterial);
					
					// Notify asset registry
					FAssetRegistryModule::AssetCreated(MaterialInstance);
					MIPackage->MarkPackageDirty();
					
					UE_LOG(LogTemp, Log, TEXT("Created material instance: %s"), *MaterialInstancePackageName);
				}
			}
			
			// Set texture parameters on material instance
			if (MaterialInstance)
			{
				bool bModified = false;
				
				if (DiffuseTexture)
				{
					MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("BaseColorTexture")), DiffuseTexture);
					bModified = true;
				}
				
				if (NormalTexture)
				{
					MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("NormalTexture")), NormalTexture);
					bModified = true;
				}
				
				if (RoughnessTexture)
				{
					MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("RoughnessTexture")), RoughnessTexture);
					bModified = true;
				}
				
				if (MetallicTexture)
				{
					MaterialInstance->SetTextureParameterValueEditorOnly(FName(TEXT("MetallicTexture")), MetallicTexture);
					bModified = true;
				}
				
				if (bModified)
				{
					MaterialInstance->PostEditChange();
				}
				
				// Apply material instance to mesh slot
				if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObject))
				{
					StaticMesh->SetMaterial(SlotIndex, MaterialInstance);
					StaticMesh->PostEditChange();
					UE_LOG(LogTemp, Log, TEXT("Applied material instance to static mesh slot %d"), SlotIndex);
				}
				else if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(LoadedObject))
				{
					SkeletalMesh->GetMaterials()[SlotIndex].MaterialInterface = MaterialInstance;
					SkeletalMesh->PostEditChange();
					UE_LOG(LogTemp, Log, TEXT("Applied material instance to skeletal mesh slot %d"), SlotIndex);
				}
			}
		}
	}
}
#endif

bool UUEMeshBPExportFuncsBPLibrary::ImportMesh(const FString& TargetUEPath, const FString& SourceFbxPath, const FString& MeshName, bool bImportMaterial, bool bImportSkeleton, UObject* ParentMaterialAsset, float Scale)
{
#if WITH_EDITOR
	FString MeshBaseName = FPaths::GetBaseFilename(MeshName);
	FString MeshPath = FPaths::Combine(SourceFbxPath, MeshName);
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
		// Explicitly set import type for automated import
		FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;
		FbxFactory->ImportUI->bImportAsSkeletal = bImportSkeleton;
		FbxFactory->ImportUI->MeshTypeToImport = bImportSkeleton ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
		FbxFactory->ImportUI->bImportMesh = true;
		
		// Enforce material/texture/animation/physics import flags
		FbxFactory->ImportUI->bImportMaterials = false;
		FbxFactory->ImportUI->bImportTextures = false;
		FbxFactory->ImportUI->bImportAnimations = false;
		FbxFactory->ImportUI->bCreatePhysicsAsset = false;

		if (FbxFactory->ImportUI->StaticMeshImportData)
		{
			FbxFactory->ImportUI->StaticMeshImportData->ImportUniformScale = Scale;
		}
		if (FbxFactory->ImportUI->SkeletalMeshImportData)
		{
			FbxFactory->ImportUI->SkeletalMeshImportData->ImportUniformScale = Scale;
		}
	}
	
	// Create import task
	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	FString UEMeshPath = FPaths::Combine(TargetUEPath, MeshBaseName);
	ImportTask->AddToRoot();
	ImportTask->bAutomated = true;
	ImportTask->bReplaceExisting = true;
	ImportTask->bSave = false;
	ImportTask->Filename = MeshPath;
	ImportTask->DestinationPath = UEMeshPath;
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
	
	if (bImportMaterial)
	{
		FString JsonPath = MeshPath.Replace(TEXT(".fbx"), TEXT(".json"));
		ImportMaterialFromJson(JsonPath, TargetUEPath, SourceFbxPath, ImportTask->ImportedObjectPaths, ParentMaterialAsset);
	}
	
	return bSuccess;
#else
	UE_LOG(LogTemp, Error, TEXT("ImportMesh: This function is only available in editor builds"));
	return false;
#endif
}