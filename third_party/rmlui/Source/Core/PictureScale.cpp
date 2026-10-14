// Forge: see Include/RmlUi/Core/PictureScale.h.

#include "../../Include/RmlUi/Core/PictureScale.h"
#include "../../Include/RmlUi/Core/Element.h"
#include "../../Include/RmlUi/Core/Math.h"
#include "TransformState.h"
#include <cmath>

namespace Rml {

float PictureScale::Of(const Element* element)
{
	const TransformState* state = element ? element->GetTransformState() : nullptr;
	const Matrix4f* m = state ? state->GetTransform() : nullptr;
	if (!m)
		return 1.f;
	const Vector4f x = *m * Vector4f(1, 0, 0, 0), y = *m * Vector4f(0, 1, 0, 0);
	const float s = Math::Max(Math::SquareRoot(x.x * x.x + x.y * x.y), Math::SquareRoot(y.x * y.x + y.y * y.y));
	return s > 0.f && s < 1e6f ? s : 1.f; // NaN too
}

float PictureScale::Step(float scale)
{
	if (!(scale > 0.f))
		return 1.f;
	const float n = Math::Clamp(std::ceil(4.f * std::log2(scale) - 1e-3f), -12.f, 12.f);
	return std::exp2(n / 4.f);
}

bool PictureScale::Update(float scale)
{
	if (!(scale > 0.f) || Math::Absolute(scale - 1.f) < 1e-4f) // a turn alone leaves 1 give or take rounding
		scale = 1.f;
	if (seen == 0.f)
	{
		seen = scale; // the first scale seen is taken as one that holds
		still = kStill;
	}
	else if (Math::Absolute(scale - seen) > 1e-4f * Math::Max(1.f, scale))
	{
		seen = scale;
		still = 0;
	}
	else if (still < kStill)
		++still;
	const float want = (still >= kStill ? scale : Step(scale));
	if (want == raster)
		return false;
	raster = want;
	return true;
}

} // namespace Rml
