#include "Vehicle/NeonVehicleConfig.h"

float UNeonVehicleConfig::EvaluateLeanAngle(float AbsSteer) const
{
	AbsSteer = FMath::Clamp(AbsSteer, 0.f, 1.f);

	float Angle;
	if (AbsSteer < 1.f / 3.f)
	{
		Angle = FMath::Lerp(0.f, LeanAngleLow, AbsSteer * 3.f);
	}
	else if (AbsSteer < 2.f / 3.f)
	{
		Angle = FMath::Lerp(LeanAngleLow, LeanAngleMid, (AbsSteer - 1.f / 3.f) * 3.f);
	}
	else
	{
		Angle = FMath::Lerp(LeanAngleMid, LeanAngleHigh, (AbsSteer - 2.f / 3.f) * 3.f);
	}

	return FMath::Min(Angle, MaxLeanAngle);
}
