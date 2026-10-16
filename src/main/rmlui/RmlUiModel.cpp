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

#include "pch.h"
#include "RmlUiModel.h"

#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Variant.h>

#include <string>

namespace mq::rmlui {
namespace {

// Applies a value RmlUi wrote back through a two-way binding, such as a
// "value" attribute on an input that carries data-model.
//
// A nil variant clears the value. Types with no model representation are
// refused rather than silently clearing, so a binding that writes something
// unexpected leaves the stored value intact.
bool AssignFromVariant(RmlUiValue& target, const Rml::Variant& variant)
{
	switch (variant.GetType())
	{
	case Rml::Variant::NONE:
		target = RmlUiValue();
		return true;
	case Rml::Variant::BOOL:
		target = RmlUiValue(variant.Get<bool>());
		return true;
	case Rml::Variant::BYTE:
		target = RmlUiValue(static_cast<double>(variant.Get<Rml::byte>()));
		return true;
	case Rml::Variant::CHAR:
		target = RmlUiValue(static_cast<double>(variant.Get<char>()));
		return true;
	case Rml::Variant::FLOAT:
		target = RmlUiValue(static_cast<double>(variant.Get<float>()));
		return true;
	case Rml::Variant::DOUBLE:
		target = RmlUiValue(variant.Get<double>());
		return true;
	case Rml::Variant::INT:
		target = RmlUiValue(static_cast<double>(variant.Get<int>()));
		return true;
	case Rml::Variant::INT64:
		target = RmlUiValue(static_cast<double>(variant.Get<int64_t>()));
		return true;
	case Rml::Variant::UINT:
		target = RmlUiValue(static_cast<double>(variant.Get<unsigned int>()));
		return true;
	case Rml::Variant::UINT64:
		target = RmlUiValue(static_cast<double>(variant.Get<uint64_t>()));
		return true;
	case Rml::Variant::STRING:
	{
		const Rml::String& text = variant.Get<Rml::String>();
		target = RmlUiValue(std::string(text.begin(), text.end()));
		return true;
	}
	default:
		return false;
	}
}

// Projects an RmlUiValue into RmlUi for every model kind at once.
//
// RmlUi never branches on a variable's declared DataVariableType. Address
// resolution always goes through Child(), array iteration goes through Size()
// followed by Child() with an index (see DataViewFor::Update), and
// interpolation goes through Get(). Relying on that is what lets one definition
// cover scalars, arrays, and records.
//
// It also matters that DataModel::BindVariable refuses to rebind a name that
// already exists and offers no way to unbind one. A single definition means a
// variable can move between scalar, array, and record over its lifetime
// without ever needing to be rebound, so type changes cost nothing.
//
// All state lives in the RmlUiValue behind ptr, so one instance serves every
// model on every context.
class DynamicValueDefinition final : public Rml::VariableDefinition
{
public:
	DynamicValueDefinition() : VariableDefinition(Rml::DataVariableType::Struct) {}

	bool Get(void* ptr, Rml::Variant& variant) override
	{
		const RmlUiValue* value = static_cast<const RmlUiValue*>(ptr);
		if (!value)
			return false;

		switch (value->GetType())
		{
		case RmlUiValue::Type::Bool:
			variant = value->AsBool();
			return true;
		case RmlUiValue::Type::Number:
			variant = value->AsNumber();
			return true;
		case RmlUiValue::Type::String:
		{
			// Rml::String carries RmlUi's own traits and allocator, so it does
			// not convert implicitly to std::string in either direction.
			const std::string& text = value->AsString();
			variant = Rml::String(text.begin(), text.end());
			return true;
		}
		default:
			// Arrays and records have no scalar representation. Returning false
			// leaves the variant untouched, which RmlUi renders as an empty
			// value rather than treating as an error.
			return false;
		}
	}

	bool Set(void* ptr, const Rml::Variant& variant) override
	{
		RmlUiValue* value = static_cast<RmlUiValue*>(ptr);
		return value != nullptr && AssignFromVariant(*value, variant);
	}

	int Size(void* ptr) override
	{
		const RmlUiValue* value = static_cast<const RmlUiValue*>(ptr);
		return value ? static_cast<int>(value->Size()) : 0;
	}

	Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override
	{
		RmlUiValue* value = static_cast<RmlUiValue*>(ptr);
		if (!value)
			return Rml::DataVariable();

		// An address entry is built from either a member name or an index.
		// Asking for one against a value of the other kind yields no variable,
		// which RmlUi treats as an empty value. ElementAt and Find already
		// reject the wrong kind of value and out-of-range indices.
		RmlUiValue* child = address.index >= 0
			? value->ElementAt(static_cast<size_t>(address.index))
			: value->Find(address.name);

		if (!child)
			return Rml::DataVariable();

		return Rml::DataVariable(this, child);
	}
};

DynamicValueDefinition s_valueDefinition;

bool MeasureValue(const RmlUiValue& value, size_t depth, size_t maxDepth, size_t maxFields,
	size_t maxArrayLength, ModelValueUsage& usage, std::string& error)
{
	if (depth > usage.depth)
		usage.depth = depth;

	if (depth > maxDepth)
	{
		error = "value nests deeper than the configured limit of " + std::to_string(maxDepth);
		return false;
	}

	switch (value.GetType())
	{
	case RmlUiValue::Type::Nil:
		return true;

	case RmlUiValue::Type::Bool:
		usage.bytes += 1;
		return true;

	case RmlUiValue::Type::Number:
		usage.bytes += sizeof(double);
		return true;

	case RmlUiValue::Type::String:
		usage.bytes += value.AsString().size();
		return true;

	case RmlUiValue::Type::Array:
	{
		if (value.Size() > maxArrayLength)
		{
			error = "an array holds " + std::to_string(value.Size())
				+ " entries, over the configured limit of " + std::to_string(maxArrayLength);
			return false;
		}

		usage.arrayEntries += value.Size();
		usage.bytes += value.Size() * sizeof(void*);

		for (size_t index = 0; index < value.Size(); ++index)
		{
			const RmlUiValue* element = value.ElementAt(index);
			if (!element || !MeasureValue(*element, depth + 1, maxDepth, maxFields, maxArrayLength, usage, error))
				return false;
		}
		return true;
	}

	case RmlUiValue::Type::Record:
	{
		if (value.Size() > maxFields)
		{
			error = "a record holds " + std::to_string(value.Size())
				+ " fields, over the configured limit of " + std::to_string(maxFields);
			return false;
		}

		bool failed = false;
		value.ForEachField([&](const std::string& key, const RmlUiValue& field) {
			++usage.fields;
			usage.bytes += key.size() + sizeof(void*);

			if (!MeasureValue(field, depth + 1, maxDepth, maxFields, maxArrayLength, usage, error))
			{
				failed = true;
				return false;
			}
			return true;
		});

		return !failed;
	}
	}

	return true;
}

} // namespace

Rml::DataVariable MakeModelVariable(RmlUiValue& value)
{
	return Rml::DataVariable(&s_valueDefinition, &value);
}

bool MeasureModelValue(const RmlUiValue& value, size_t maxDepth, size_t maxFields, size_t maxArrayLength,
	ModelValueUsage& usage, std::string& error)
{
	usage = ModelValueUsage();
	error.clear();
	return MeasureValue(value, 1, maxDepth, maxFields, maxArrayLength, usage, error);
}

} // namespace mq::rmlui
