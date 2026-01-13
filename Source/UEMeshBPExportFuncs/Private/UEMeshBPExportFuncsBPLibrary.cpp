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
#endif

UUEMeshBPExportFuncsBPLibrary::UUEMeshBPExportFuncsBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{

}


bool UUEMeshBPExportFuncsBPLibrary::ExportSkelMeshes(AActor* Actor, const FString& ExportPath, int32 TextureSizeLimit)
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

	// 确保导出路径存在
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*ExportPath))
	{
		if (!PlatformFile.CreateDirectoryTree(*ExportPath))
		{
			UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: Failed to create directory: %s"), *ExportPath);
			return false;
		}
	}

	// 收集所有的SkeletalMeshComponent
	TArray<USkeletalMeshComponent*> SkelMeshComponents;
	Actor->GetComponents<USkeletalMeshComponent>(SkelMeshComponents);

	if (SkelMeshComponents.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ExportSkelMeshes: No SkeletalMeshComponent found in Actor"));
		return false;
	}

	// 用于去重
	TSet<USkeletalMesh*> ProcessedMeshes;
	TSet<UMaterialInterface*> ProcessedMaterials;
	TSet<UTexture*> ProcessedTextures;

	int32 MeshIndex = 0;

	// 遍历所有SkeletalMeshComponent
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

		// 导出骨骼网格为FBX
		FString MeshName = SkelMesh->GetName();
		FString FbxFileName = FString::Printf(TEXT("%s/%s_%d.fbx"), *ExportPath, *MeshName, MeshIndex);

		UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
		ExportTask->Object = SkelMesh;
		ExportTask->Exporter = nullptr; // 自动选择合适的导出器
		ExportTask->Filename = FbxFileName;
		ExportTask->bSelected = false;
		ExportTask->bReplaceIdentical = true;
		ExportTask->bPrompt = false;
		ExportTask->bUseFileArchive = false;
		ExportTask->bWriteEmptyFiles = false;
		ExportTask->bAutomated = true;

		// ★ 关键：显式创建 FBX Export Options
		UFbxExportOption* FbxOptions = NewObject<UFbxExportOption>();

		// 根据需要配置（示例）
		FbxOptions->bExportMorphTargets = false;
		FbxOptions->bExportPreviewMesh = false;
		FbxOptions->bExportLocalTime = false;
		FbxOptions->bForceFrontXAxis = false;
		FbxOptions->Collision = false;
		FbxOptions->LevelOfDetail = false;

		ExportTask->Options = FbxOptions;

		UExporter::RunAssetExportTask(ExportTask);

		// 检查导出是否成功（通过检查错误数组）
		if (ExportTask->Errors.Num() == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Successfully exported mesh to %s"), *FbxFileName);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ExportSkelMeshes: Failed to export mesh to %s"), *FbxFileName);
			for (const FString& Error : ExportTask->Errors)
			{
				UE_LOG(LogTemp, Warning, TEXT("  Error: %s"), *Error);
			}
		}

		// 收集材质
		const TArray<FSkeletalMaterial>& SkeletalMaterials = SkelMesh->GetMaterials();

		for (int32 MatIdx = 0; MatIdx < SkeletalMaterials.Num(); MatIdx++)
		{
			UMaterialInterface* Material = SkeletalMaterials[MatIdx].MaterialInterface;
			if (!Material || ProcessedMaterials.Contains(Material))
			{
				continue;
			}

			ProcessedMaterials.Add(Material);

			// 创建材质JSON数据
			TSharedPtr<FJsonObject> MaterialJson = MakeShareable(new FJsonObject);
			MaterialJson->SetStringField(TEXT("MaterialName"), Material->GetName());
			MaterialJson->SetStringField(TEXT("MaterialPath"), Material->GetPathName());

			// 获取所有纹理参数信息，并直接收集需要导出的纹理
			TArray<FMaterialParameterInfo> TextureParameterInfos;
			TArray<FGuid> TextureParameterIds;
			Material->GetAllTextureParameterInfo(TextureParameterInfos, TextureParameterIds);

			// 创建纹理到参数名称的映射，同时收集纹理
			TMap<UTexture2D*, FName> TextureToParamName;
			for (const FMaterialParameterInfo& ParamInfo : TextureParameterInfos)
			{
				UTexture* ParamTexture = nullptr;
				if (Material->GetTextureParameterValue(ParamInfo, ParamTexture) && ParamTexture)
				{
					UTexture2D* Texture2D = Cast<UTexture2D>(ParamTexture);
					if (Texture2D && !TextureToParamName.Contains(Texture2D))
					{
						// 只保存第一个参数名称
						TextureToParamName.Add(Texture2D, ParamInfo.Name);
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> TextureArray;

			// 遍历收集到的纹理进行导出
			for (const TPair<UTexture2D*, FName>& TexturePair : TextureToParamName)
			{
				UTexture2D* Texture2D = TexturePair.Key;
				FName ParameterName = TexturePair.Value;

				if (!Texture2D || ProcessedTextures.Contains(Texture2D))
				{
					continue;
				}

				ProcessedTextures.Add(Texture2D);

				// 导出贴图为PNG（使用UAssetExportTask）
				FString TextureName = Texture2D->GetName();
				FString TextureFileName = FString::Printf(TEXT("%s/%s.png"), *ExportPath, *TextureName);

				// 创建导出任务
				UAssetExportTask* TextureExportTask = NewObject<UAssetExportTask>();
				TextureExportTask->Object = Texture2D;
				TextureExportTask->Exporter = nullptr; // 自动选择合适的导出器
				TextureExportTask->Filename = TextureFileName;
				TextureExportTask->bSelected = false;
				TextureExportTask->bReplaceIdentical = true;
				TextureExportTask->bPrompt = false;
				TextureExportTask->bUseFileArchive = false;
				TextureExportTask->bWriteEmptyFiles = false;

				// 执行导出
				UExporter::RunAssetExportTask(TextureExportTask);

				// 检查导出是否成功
				if (TextureExportTask->Errors.Num() == 0)
				{
					// 检查并缩放图像（如果尺寸大于TextureSizeLimit）
					TArray<uint8> FileData;
					if (FFileHelper::LoadFileToArray(FileData, *TextureFileName))
					{
						IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
						TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

						if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
						{
							int32 Width = ImageWrapper->GetWidth();
							int32 Height = ImageWrapper->GetHeight();

							// 如果宽度或高度大于TextureSizeLimit，需要缩放
							if (Width > TextureSizeLimit || Height > TextureSizeLimit)
							{
								UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Texture %s size is %dx%d, resizing to fit %d"), *TextureName, Width, Height, TextureSizeLimit);

								// 计算缩放比例，保持宽高比
								float Scale = FMath::Min(float(TextureSizeLimit) / Width, (TextureSizeLimit) / Height);
								int32 NewWidth = FMath::RoundToInt(Width * Scale);
								int32 NewHeight = FMath::RoundToInt(Height * Scale);

								// 解压图像数据
								TArray<uint8> RawData;
								if (ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
								{
									// 创建源图像数据
									TArray<FColor> SourceColors;
									SourceColors.SetNum(Width * Height);
									FMemory::Memcpy(SourceColors.GetData(), RawData.GetData(), Width * Height * sizeof(FColor));

									// 创建目标图像数据
									TArray<FColor> ResizedColors;
									ResizedColors.SetNum(NewWidth * NewHeight);

									// 简单的双线性插值缩放
									for (int32 Y = 0; Y < NewHeight; Y++)
									{
										for (int32 X = 0; X < NewWidth; X++)
										{
											float SrcX = (X + 0.5f) / Scale - 0.5f;
											float SrcY = (Y + 0.5f) / Scale - 0.5f;

											int32 X0 = FMath::Clamp(FMath::FloorToInt(SrcX), 0, Width - 1);
											int32 Y0 = FMath::Clamp(FMath::FloorToInt(SrcY), 0, Height - 1);
											int32 X1 = FMath::Clamp(X0 + 1, 0, Width - 1);
											int32 Y1 = FMath::Clamp(Y0 + 1, 0, Height - 1);

											float FracX = SrcX - X0;
											float FracY = SrcY - Y0;

											FColor C00 = SourceColors[Y0 * Width + X0];
											FColor C10 = SourceColors[Y0 * Width + X1];
											FColor C01 = SourceColors[Y1 * Width + X0];
											FColor C11 = SourceColors[Y1 * Width + X1];

											// 双线性插值
											FColor Result;
											Result.R = FMath::RoundToInt(
												C00.R * (1 - FracX) * (1 - FracY) +
												C10.R * FracX * (1 - FracY) +
												C01.R * (1 - FracX) * FracY +
												C11.R * FracX * FracY
											);
											Result.G = FMath::RoundToInt(
												C00.G * (1 - FracX) * (1 - FracY) +
												C10.G * FracX * (1 - FracY) +
												C01.G * (1 - FracX) * FracY +
												C11.G * FracX * FracY
											);
											Result.B = FMath::RoundToInt(
												C00.B * (1 - FracX) * (1 - FracY) +
												C10.B * FracX * (1 - FracY) +
												C01.B * (1 - FracX) * FracY +
												C11.B * FracX * FracY
											);
											Result.A = FMath::RoundToInt(
												C00.A * (1 - FracX) * (1 - FracY) +
												C10.A * FracX * (1 - FracY) +
												C01.A * (1 - FracX) * FracY +
												C11.A * FracX * FracY
											);

											ResizedColors[Y * NewWidth + X] = Result;
										}
									}

									// 重新压缩为PNG
									TSharedPtr<IImageWrapper> NewImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
									if (NewImageWrapper.IsValid())
									{
										NewImageWrapper->SetRaw(ResizedColors.GetData(), ResizedColors.Num() * sizeof(FColor), NewWidth, NewHeight, ERGBFormat::BGRA, 8);
										const TArray64<uint8>& CompressedData = NewImageWrapper->GetCompressed();

										// 覆盖原文件
										if (FFileHelper::SaveArrayToFile(CompressedData, *TextureFileName))
										{
											UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Resized texture to %dx%d and saved"), NewWidth, NewHeight);
										}
										else
										{
											UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: Failed to save resized texture"));
										}
									}
								}
							}
						}
					}

					// 添加到JSON
					TSharedPtr<FJsonObject> TextureJson = MakeShareable(new FJsonObject);

					// 添加参数名称（放在TextureName之前）
					TextureJson->SetStringField(TEXT("ParameterName"), ParameterName.ToString());
					TextureJson->SetStringField(TEXT("TextureName"), TextureName);
					TextureJson->SetStringField(TEXT("TexturePath"), Texture2D->GetPathName());
					TextureJson->SetStringField(TEXT("ExportedFile"), TextureFileName);

					TextureArray.Add(MakeShareable(new FJsonValueObject(TextureJson)));

					UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Exported texture to %s"), *TextureFileName);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: Failed to export texture %s"), *TextureName);
				}
			}

			MaterialJson->SetArrayField(TEXT("Textures"), TextureArray);

			// 导出材质JSON
			FString MaterialJsonFileName = FString::Printf(TEXT("%s/%s.json"), *ExportPath, *Material->GetName());
			FString JsonString;
			TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonString);
			FJsonSerializer::Serialize(MaterialJson.ToSharedRef(), JsonWriter);

			if (FFileHelper::SaveStringToFile(JsonString, *MaterialJsonFileName))
			{
				UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Successfully exported material JSON to %s"), *MaterialJsonFileName);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("ExportSkelMeshes: Failed to export material JSON to %s"), *MaterialJsonFileName);
			}
		}

		MeshIndex++;
	}

	UE_LOG(LogTemp, Log, TEXT("ExportSkelMeshes: Export completed. Processed %d meshes, %d materials, %d textures"),
		ProcessedMeshes.Num(), ProcessedMaterials.Num(), ProcessedTextures.Num());

	return true;
#else
	UE_LOG(LogTemp, Error, TEXT("ExportSkelMeshes: This function is only available in Editor"));
	return false;
#endif
}

