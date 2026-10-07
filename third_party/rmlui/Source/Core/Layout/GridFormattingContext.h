#pragma once

#include "../../../Include/RmlUi/Core/Types.h"
#include "FormattingContext.h"

namespace Rml {

class LayoutBox;
class ContainerBox;
class FlexContainer;

/*
    Forge: formats a grid container (display: grid / inline-grid) and its items according to CSS grid layout rules.

    Supported: grid-template-columns/rows with px, %, em and other lengths, fr, auto, min-content, max-content, minmax(),
    fit-content() and repeat() (also auto-fill and auto-fit), named lines, grid-template-areas, the grid-template and grid
    shorthands, grid-auto-rows/columns, grid-auto-flow (row, column, dense), line numbers, spans and names in item placement,
    gaps, justify/align-content, justify/align-items and -self. Items always use the block formatting context.
*/
class GridFormattingContext final : public FormattingContext {
public:
	static UniquePtr<LayoutBox> Format(ContainerBox* parent_container, Element* element, const Box* override_initial_box);

	/// Computes the max-content size of a grid container.
	static Vector2f GetMaxContentSize(Element* element);

private:
	GridFormattingContext() = default;

	void Format(Vector2f& resulting_content_size, Vector2f& visible_overflow_size, Vector2f& scrollable_content_size, float& baseline) const;

	Vector2f available_content_size; // Negative for an indefinite size.
	Vector2f content_containing_block;
	Vector2f content_offset;

	Element* element_grid = nullptr;
	FlexContainer* container_box = nullptr;
};

} // namespace Rml
