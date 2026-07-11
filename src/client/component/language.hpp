#pragma once

namespace language
{
	enum class lang
	{
		english,
		schinese,
	};

	lang get_language();
	const char* get_language_string();
	void set_language(lang lang);

	// Get a translated string for a key, or nullptr if not found
	const char* get_translation(const std::string& key);
}
