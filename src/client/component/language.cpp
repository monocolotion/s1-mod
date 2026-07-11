#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "language.hpp"
#include "localized_strings.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"

#include <utils/io.hpp>

namespace language
{
	namespace
	{
		lang current_language = lang::english;
		std::unordered_map<std::string, std::string> translations_cache;

		void load_language_from_config()
		{
			std::string data;
			const auto config_path = "data/language.txt";
			if (utils::io::read_file(config_path, &data))
			{
				data.erase(0, data.find_first_not_of(" \t\r\n"));
				data.erase(data.find_last_not_of(" \t\r\n") + 1);

				console::info("language: Read language.txt = '%s'\n", data.data());

				if (data == "schinese" || data == "chinese" || data == "zh" || data == "zh-cn")
				{
					current_language = lang::schinese;
					console::info("language: Set language to schinese\n");
				}
				else
				{
					console::info("language: Unrecognized language '%s', defaulting to english\n", data.data());
				}
			}
			else
			{
				console::info("language: language.txt not found, defaulting to english\n");
			}
		}

		void load_translations_cache()
		{
			translations_cache.clear();

			std::string data;
			if (!utils::io::read_file("data/translations.json", &data))
			{
				console::info("language: translations.json not found\n");
				return;
			}

			rapidjson::Document doc;
			if (doc.Parse(data).HasParseError())
			{
				console::error("language: Failed to parse translations.json\n");
				return;
			}

			const char* lang_key = (current_language == lang::schinese) ? "schinese" : "english";
			if (!doc.HasMember(lang_key))
			{
				console::info("language: No '%s' section in translations.json\n", lang_key);
				return;
			}

			const auto& translations = doc[lang_key];
			for (auto it = translations.MemberBegin(); it != translations.MemberEnd(); ++it)
			{
				if (it->value.IsString())
				{
					translations_cache[it->name.GetString()] = it->value.GetString();
				}
			}

			console::info("language: Loaded %zu translations for '%s'\n", translations_cache.size(), lang_key);
		}

		void apply_translations()
		{
			if (current_language != lang::schinese)
			{
				console::info("language: Not applying translations (language is not schinese)\n");
				return;
			}

			size_t count = 0;
			for (const auto& [key, value] : translations_cache)
			{
				localized_strings::override(key, value);
				count++;
			}

			console::info("language: Applied %zu translation overrides\n", count);
		}

		// Directly patch LocalizeEntry asset values in the game asset database.
		// LUI Engine.Localize() may read these assets through a different path
		// (e.g. SL_ConvertToString) that bypasses SEH_StringEd_GetString entirely.
		// By patching the assets themselves we cover ALL lookup paths.
		void patch_localize_assets()
		{
			if (current_language != lang::schinese) return;

			std::vector<std::string> stable_strings;
			stable_strings.reserve(translations_cache.size());
			size_t patched = 0;

			struct PatchCtx
			{
				decltype(&translations_cache) cache;
				size_t* counter;
				std::vector<std::string>* stable;
			} ctx = { &translations_cache, &patched, &stable_strings };

			game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_LOCALIZE_ENTRY,
				[](game::XAssetHeader header, void* data)
			{
				auto* ctx = static_cast<PatchCtx*>(data);

				struct LocalizeEntry { const char* value; const char* name; };
				auto* entry = static_cast<LocalizeEntry*>(header.data);
				if (!entry || !entry->name || !entry->value) return;

				const auto it = ctx->cache->find(entry->name);
				if (it == ctx->cache->end()) return;

				ctx->stable->push_back(it->second);
				entry->value = ctx->stable->back().c_str();
				(*ctx->counter)++;
			}, &ctx, false);

			console::info("language: Patched %zu LocalizeEntry assets with schinese translations\n", patched);
		}
	}

	lang get_language()
	{
		return current_language;
	}

	const char* get_language_string()
	{
		switch (current_language)
		{
		case lang::schinese:
			return "schinese";
		case lang::english:
		default:
			return "english";
		}
	}

	void set_language(const lang lang)
	{
		current_language = lang;
		apply_translations();
	}

	const char* get_translation(const std::string& key)
	{
		if (current_language != lang::schinese) return nullptr;
		const auto it = translations_cache.find(key);
		if (it != translations_cache.end())
		{
			return it->second.c_str();
		}
		return nullptr;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			load_language_from_config();
			load_translations_cache();

			// Apply translations IMMEDIATELY for SEH_StringEd_GetString path
			apply_translations();
		}
	};
}

REGISTER_COMPONENT(language::component)
