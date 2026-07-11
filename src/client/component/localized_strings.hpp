#pragma once

namespace localized_strings
{
	void override(const std::string& key, const std::string& value);
	const char* lookup(const std::string& key);
}
