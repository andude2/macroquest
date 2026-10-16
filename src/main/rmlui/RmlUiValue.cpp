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

#include "mq/api/RmlUi.h"

#include <algorithm>

namespace mq {

void RmlUiValue::EnsureType(Type type)
{
	if (m_type == type)
		return;

	// Drop the previous payload. Only the members belonging to the new type
	// are kept live, so a value can be retyped in place without leaving a
	// stale container behind for the model bridge to walk.
	switch (m_type)
	{
	case Type::String:
		m_string.clear();
		m_string.shrink_to_fit();
		break;
	case Type::Array:
		// Swap rather than clear: clear() keeps the capacity, so retyping a
		// variable away from a large array would leave memory the owner's byte
		// quota no longer accounts for.
		std::vector<RmlUiValue>().swap(m_array);
		break;
	case Type::Record:
		m_record.clear();
		break;
	default:
		break;
	}
	m_type = type;
}

size_t RmlUiValue::Size() const
{
	switch (m_type)
	{
	case Type::Array:
		return m_array.size();
	case Type::Record:
		return m_record.size();
	default:
		return 0;
	}
}

RmlUiValue& RmlUiValue::AddElement(RmlUiValue value)
{
	EnsureType(Type::Array);
	m_array.push_back(std::move(value));
	return m_array.back();
}

RmlUiValue& RmlUiValue::AddElement()
{
	EnsureType(Type::Array);
	m_array.emplace_back();
	return m_array.back();
}

bool RmlUiValue::SpliceArray(size_t index, size_t deleteCount,
	const std::vector<RmlUiValue>& values)
{
	if (m_type != Type::Array || index > m_array.size())
		return false;

	const size_t available = m_array.size() - index;
	deleteCount = std::min(deleteCount, available);
	auto first = m_array.begin() + static_cast<std::ptrdiff_t>(index);
	auto last = first + static_cast<std::ptrdiff_t>(deleteCount);
	first = m_array.erase(first, last);
	m_array.insert(first, values.begin(), values.end());
	return true;
}

RmlUiValue& RmlUiValue::SetField(std::string key, RmlUiValue value)
{
	EnsureType(Type::Record);
	auto existing = m_record.find(key);
	if (existing == m_record.end())
		return m_record.emplace(std::move(key), std::move(value)).first->second;

	existing->second = std::move(value);
	return existing->second;
}

RmlUiValue& RmlUiValue::GetOrAddField(std::string key)
{
	EnsureType(Type::Record);
	return m_record[std::move(key)];
}

RmlUiValue* RmlUiValue::Find(std::string_view key)
{
	if (m_type != Type::Record)
		return nullptr;

	auto existing = m_record.find(std::string(key));
	return existing == m_record.end() ? nullptr : &existing->second;
}

const RmlUiValue* RmlUiValue::Find(std::string_view key) const
{
	if (m_type != Type::Record)
		return nullptr;

	auto existing = m_record.find(std::string(key));
	return existing == m_record.end() ? nullptr : &existing->second;
}

RmlUiValue* RmlUiValue::ElementAt(size_t index)
{
	if (m_type != Type::Array || index >= m_array.size())
		return nullptr;

	return &m_array[index];
}

const RmlUiValue* RmlUiValue::ElementAt(size_t index) const
{
	if (m_type != Type::Array || index >= m_array.size())
		return nullptr;

	return &m_array[index];
}

} // namespace mq
