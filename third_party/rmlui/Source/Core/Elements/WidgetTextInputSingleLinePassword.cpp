#include "WidgetTextInputSingleLinePassword.h"
#include "../../../Include/RmlUi/Core/ElementText.h"

namespace Rml {

// Forge: passwords show bullets (U+2022, three bytes in UTF-8) as browsers do, instead of asterisks.
static const char password_bullet[] = "\xE2\x80\xA2";
static constexpr int password_bullet_bytes = 3;

WidgetTextInputSingleLinePassword::WidgetTextInputSingleLinePassword(ElementFormControl* parent) : WidgetTextInputSingleLine(parent) {}

void WidgetTextInputSingleLinePassword::TransformValue(String& value)
{
	const size_t character_length = StringUtilities::LengthUTF8(value);
	String bullets;
	bullets.reserve(character_length * password_bullet_bytes);
	for (size_t i = 0; i < character_length; i++)
		bullets += password_bullet;
	value = std::move(bullets);
}

int WidgetTextInputSingleLinePassword::DisplayIndexToAttributeIndex(int display_index, const String& attribute_value)
{
	// Each character of the attribute value shows as one bullet.
	return StringUtilities::ConvertCharacterOffsetToByteOffset(attribute_value, display_index / password_bullet_bytes);
}

int WidgetTextInputSingleLinePassword::AttributeIndexToDisplayIndex(int attribute_index, const String& attribute_value)
{
	return (int)StringUtilities::LengthUTF8(StringView(attribute_value, 0, (size_t)attribute_index)) * password_bullet_bytes;
}

} // namespace Rml
