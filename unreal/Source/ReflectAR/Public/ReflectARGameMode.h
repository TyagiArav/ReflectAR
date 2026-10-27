#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "ReflectARGameMode.generated.h"

/** Spawns the mirror at startup; no pawn (the view comes from UMirrorLocalPlayer). */
UCLASS()
class REFLECTAR_API AReflectARGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AReflectARGameMode();
	virtual void StartPlay() override;
};

/**
 * Canvas UI: range prompt (top), garment carousel (bottom strip, off the body), fist-hold
 * progress ring at the hand, selection toast and a debug line. Laid out for 1920 x 1080.
 */
UCLASS()
class REFLECTAR_API AReflectARHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	float CarouselX = 0.f;
	TArray<float> CardScale;
	void Panel(float X, float Y, float W, float H, const FLinearColor& C);
	void Text(const FString& S, float X, float Y, float Scale, const FLinearColor& C, bool bCentre = true);
};
