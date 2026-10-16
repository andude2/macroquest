/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-present MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include "mq/api/RmlUi.h"

#include <RmlUi/Core/DataVariable.h>

#include <cstddef>
#include <string>

namespace mq::rmlui {

// Builds the DataVariable that projects a stored value into RmlUi.
//
// The result holds a raw pointer to the value, so the value must outlive the
// model it is bound to. Because RmlUi's model storage is node-based, the
// address of an existing value stays valid as siblings are added.
Rml::DataVariable MakeModelVariable(RmlUiValue& value);

// Resource usage of a value tree, measured before it is stored so a single
// oversized value cannot slip past the per-owner quotas.
struct ModelValueUsage
{
	size_t depth = 0;
	size_t fields = 0;
	size_t arrayEntries = 0;
	size_t bytes = 0;
};

// Walks a value tree and accumulates its usage. Returns false and fills error
// with a diagnostic when the tree is deeper than maxDepth, a record holds more
// than maxFields fields, or an array is longer than maxArrayLength. Enforcing
// all three here also bounds the cost of this walk.
//
// The byte count is an approximation used only for quota accounting; the
// caller's per-owner retained-byte budget is what ultimately bounds memory.
bool MeasureModelValue(const RmlUiValue& value, size_t maxDepth, size_t maxFields,
	size_t maxArrayLength, ModelValueUsage& usage, std::string& error);

} // namespace mq::rmlui
