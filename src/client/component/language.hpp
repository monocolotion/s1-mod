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

	// Get a Chinese translation by English text (reverse lookup), or nullptr
	const char* get_translation_by_english(const std::string& english);

	// Try both forward (key) and reverse (English) lookups
	const char* get_translation_any(const std::string& str);

	// Diagnostic: true when a Bink video is actively playing frames
	bool is_bink_playing();

}
