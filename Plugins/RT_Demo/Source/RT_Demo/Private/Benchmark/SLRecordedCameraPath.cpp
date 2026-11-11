#include "SLRecordedCameraPath.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

bool FSLRecordedCameraPath::MakeTrimmedCopy(int32 StartFrame, int32 EndFrame, FSLRecordedCameraPath& OutPath) const
{
	if (StartFrame < 0 || EndFrame <= StartFrame || !Frames.IsValidIndex(EndFrame)) return false;
	FSLRecordedCameraPath Copy;
	Copy.Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Copy.Name = Name + TEXT(" (trimmed)");
	Copy.Map = Map;
	Copy.FPS = FPS;
	Copy.Frames.Append(Frames.GetData() + StartFrame, EndFrame - StartFrame + 1);
	OutPath = MoveTemp(Copy);
	return true;
}

FSLRecordedCameraSample FSLRecordedCameraPath::Interpolate(const FSLRecordedCameraSample& A,
	const FSLRecordedCameraSample& B, double Alpha)
{
	FSLRecordedCameraSample Result = A;
	if (Alpha >= 1.0) Result.bConstrainAspectRatio = B.bConstrainAspectRatio;
	Result.Position = FMath::Lerp(A.Position, B.Position, Alpha);
	Result.Rotation = FQuat::Slerp(A.Rotation, B.Rotation, Alpha).GetNormalized();
	Result.FOV = FMath::Lerp(A.FOV, B.FOV, Alpha);
	Result.AspectRatio = FMath::Lerp(A.AspectRatio, B.AspectRatio, Alpha);
	return Result;
}

bool FSLRecordedCameraPath::Save(const FString& Filename) const
{
	if (Frames.Num() < 2) return false;
	const auto Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetStringField(TEXT("id"), Id);
	Root->SetStringField(TEXT("name"), Name);
	Root->SetStringField(TEXT("map"), Map);
	Root->SetNumberField(TEXT("fps"), FPS);
	TArray<TSharedPtr<FJsonValue>> Samples;
	for (const auto& Frame : Frames)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (double Value : {Frame.Position.X, Frame.Position.Y, Frame.Position.Z,
			Frame.Rotation.X, Frame.Rotation.Y, Frame.Rotation.Z, Frame.Rotation.W,
			double(Frame.FOV), double(Frame.AspectRatio), Frame.bConstrainAspectRatio ? 1.0 : 0.0})
			Values.Add(MakeShared<FJsonValueNumber>(Value));
		Samples.Add(MakeShared<FJsonValueArray>(Values));
	}
	Root->SetArrayField(TEXT("frames"), Samples);
	FString Json;
	if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json))) return false;
	// Publish only complete files. A failed save leaves the in-memory recording available.
	const FString Temp = Filename + TEXT(".tmp");
	return FFileHelper::SaveStringToFile(Json, *Temp)
		&& IFileManager::Get().Move(*Filename, *Temp, false, true);
}

bool FSLRecordedCameraPath::Load(const FString& Filename)
{
	if (IFileManager::Get().FileSize(*Filename) > 64 * 1024 * 1024) return false;
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *Filename)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) return false;
	FSLRecordedCameraPath Parsed;
	double Version = 0, Rate = 0;
	const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
	FGuid Guid;
	if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1
		|| !Root->TryGetStringField(TEXT("id"), Parsed.Id) || !FGuid::ParseExact(Parsed.Id, EGuidFormats::Digits, Guid)
		|| !Root->TryGetStringField(TEXT("name"), Parsed.Name) || Parsed.Name.IsEmpty()
		|| !Root->TryGetStringField(TEXT("map"), Parsed.Map) || Parsed.Map.IsEmpty()
		|| !Root->TryGetNumberField(TEXT("fps"), Rate) || Rate < 1 || Rate > 240 || Rate != FMath::FloorToDouble(Rate)
		|| !Root->TryGetArrayField(TEXT("frames"), Samples) || Samples->Num() < 2 || Samples->Num() > 108000) return false;
	Parsed.FPS = int32(Rate);
	for (const auto& Sample : *Samples)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Sample || !Sample->TryGetArray(Values) || Values->Num() != 10) return false;
		double V[10];
		for (int32 I = 0; I < 10; ++I)
			if (!(*Values)[I] || !(*Values)[I]->TryGetNumber(V[I]) || !FMath::IsFinite(V[I])) return false;
		FSLRecordedCameraSample Frame;
		Frame.Position = FVector(V[0], V[1], V[2]);
		Frame.Rotation = FQuat(V[3], V[4], V[5], V[6]);
		if (FMath::Abs(Frame.Rotation.SizeSquared() - 1.0) > 0.01
			|| V[7] <= 1 || V[7] >= 179 || V[8] < 0.1 || V[8] > 10 || (V[9] != 0 && V[9] != 1)) return false;
		Frame.Rotation.Normalize();
		Frame.FOV = V[7];
		Frame.AspectRatio = V[8];
		Frame.bConstrainAspectRatio = V[9] != 0;
		Parsed.Frames.Add(Frame);
	}
	*this = MoveTemp(Parsed);
	return true;
}
