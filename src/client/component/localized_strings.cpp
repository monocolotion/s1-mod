#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "localized_strings.hpp"
#include "game/game.hpp"
#include "font_cjk.hpp"
#include "language.hpp"
#include "console.hpp"
#include "command.hpp"
#include <utils/hook.hpp>
#include <utils/string.hpp>
#include <utils/concurrency.hpp>
#include <utils/io.hpp>

namespace localized_strings
{
	namespace
	{
		utils::hook::detour seh_string_ed_get_string_hook;

		using localized_map = std::unordered_map<std::string, std::string>;
		utils::concurrency::container<localized_map> localized_overrides;

		// LocalizeEntry asset struct (not in game/structs.hpp)
		// Fields are ordered: value first, then name (key)
		struct LocalizeEntry
		{
			const char* value;  // localized text (first field)
			const char* name;   // localization key (second field)
		};

		const char* seh_string_ed_get_string(const char* reference)
		{
			return localized_overrides.access<const char*>([&](const localized_map& map)
			{
				const auto entry = map.find(reference);
				if (entry != map.end())
				{

						// Diagnostic: log VIDSUBTITLES lookups
						if (reference && strstr(reference, "VIDSUBTITLES"))
						{
							static int vid_log_count = 0;
							if (vid_log_count < 50)
							{
								vid_log_count++;
								console::info("SEH: VIDSUBTITLES lookup %s -> %s\n",
									reference, entry->second.c_str());
							}
						}
					// If CJK font atlas isn't ready yet, fall back to English for CJK
					// strings. Without this the original font atlas (no CJK glyphs) would
					// render every Chinese character as "...". The brief English display
					// is acceptable because apply_translations() now runs in post_unpack
					// and CJK init completes during fastfile loading (try_early_cjk_init).
					if (!font_cjk::is_cjk_available())
					{
						bool has_cjk = false;
						for (const char* s = entry->second.data(); *s; s++)
						{
							if (static_cast<unsigned char>(*s) >= 0x80) { has_cjk = true; break; }
						}
						if (has_cjk)
							return seh_string_ed_get_string_hook.invoke<const char*>(reference);
					}

					// Return pointer directly into the map. std::unordered_map
					// guarantees reference stability — c_str() is safe as long as
					// we never erase or modify existing values (we only insert).
					// Avoid utils::string::va here — its ring buffer wraps under
					// heavy load and would corrupt earlier string pointers.
					const auto* result = entry->second.c_str();

					static int override_log_count = 0;
					if (override_log_count < 20)
					{
						override_log_count++;
						console::info("localized_strings: lookup('%s') -> override '%s'\n",
							reference, result);
					}
					bool has_cjk = false;
					for (const char* s = result; *s; s++)
					{
						if (static_cast<unsigned char>(*s) >= 0x80) { has_cjk = true; break; }
					}
					if (has_cjk)
					{
						static int cjk_lookup_count = 0;
						if (cjk_lookup_count < 40)
						{
							cjk_lookup_count++;
							console::info("localized_strings: CJK lookup #%d: key='%s' -> '%s'\n",
								cjk_lookup_count, reference, result);
						}
					}
					return result;
				}

				// Diagnostic: log ALL VID/CINE/SUBTITLE lookups (even misses)
			// to determine if Bink cutscene subtitles go through SEH_StringEd_GetString
			if (reference && (strstr(reference, "VID") || strstr(reference, "CINE")))
			{
				static int miss_log = 0;
				if (miss_log < 100)
				{
					miss_log++;
					console::info("SEH: MISS '%s' (not in overrides, calling original)\n", reference);
				}
			}
			// Fallback: try sl_reverse (GSC display strings with key bindings)
			const char* rev = language::get_translation_by_english(reference);
			if (rev) return rev;

			return seh_string_ed_get_string_hook.invoke<const char*>(reference);
			});
		}
	}

	void override(const std::string& key, const std::string& value)
	{
		localized_overrides.access([&](localized_map& map)
		{
			map[key] = value;
		});
	}

	const char* lookup(const std::string& key)
	{
		return localized_overrides.access<const char*>([&](const localized_map& map)
		{
			const auto entry = map.find(key);
			if (entry != map.end())
			{
				return utils::string::va("%s", entry->second.data());
			}
			return seh_string_ed_get_string_hook.invoke<const char*>(key.data());
		});
	}

	// Scr_GetString hook: intercepts GSC stack string resolution.
	// When a GSC built-in (e.g. sethintstring) receives a VAR_ISTRING
	// (&"..." localized reference), Scr_GetString resolves it through
	// a code path that bypasses SEH_StringEd_GetString. This hook
	// catches those resolutions and returns the Chinese translation.
	utils::hook::detour scr_get_string_hook;

	const char* scr_get_string_stub(unsigned int index)
	{
		// Only intercept VAR_ISTRING (&"..." GSC references), not
		// regular strings. Intercepting regular strings breaks internal
		// game functions (persistent data, etc.) that pass raw strings
		// which may happen to match translation key names.
		bool is_istring = false;
		if (index < game::scr_VmPub->outparamcount)
		{
			auto* value = &game::scr_VmPub->top[-static_cast<int>(index)];
			if (value->type == 0x3) // VAR_ISTRING
				is_istring = true;
		}

		const char* original = scr_get_string_hook.invoke<const char*>(index);

		if (is_istring && original && original[0])
		{
			// Log all VAR_ISTRING resolutions to understand the path
			static int scrlog = 0;
			if (scrlog < 200)
			{
				scrlog++;
				console::info("localized_strings: Scr_GetString[ISTRING](%u) -> '%s'\n",
					index, original);
			}

			const char* translated = localized_overrides.access<const char*>(
				[&](const localized_map& map) -> const char*
			{
				const auto entry = map.find(original);
				if (entry != map.end())
				{
					if (!font_cjk::is_cjk_available())
					{
						bool has_cjk = false;
						for (const char* s = entry->second.data(); *s; s++)
							if (static_cast<unsigned char>(*s) >= 0x80)
								{ has_cjk = true; break; }
						if (has_cjk) return nullptr;
					}
					static int matchlog = 0;
					if (matchlog < 50)
					{
						matchlog++;
						console::info("localized_strings: Scr_GetString MATCH '%s' -> '%s'\n",
							original, entry->second.c_str());
					}
					return entry->second.c_str();
				}
				return nullptr;
			});
			if (translated) return translated;
		}

		return original;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// Change some localized strings
			seh_string_ed_get_string_hook.create(SELECT_VALUE(0x140339CF0, 0x140474FC0), &seh_string_ed_get_string);
			console::info("localized_strings: Hooked SEH_StringEd_GetString\n");

			// Scr_GetString hook is DISABLED — it interferes with internal
			// game functions (persistent data, etc.). Instead we override
			// the sethintstring GSC method via script_extension.
			// scr_get_string_hook.create(SELECT_VALUE(0x14031C570, 0x1403F8C50), &scr_get_string_stub);
			// console::info("localized_strings: Hooked Scr_GetString\n");

			// Console command to dump all original localized strings from loaded fastfiles
			command::add("dumplocalized", [](const command::params& params)
			{
				const char* filename = "data/localized_strings_dump.json";
				if (params.size() >= 2)
				{
					filename = params[1];
				}

				console::info("Dumping localized strings to %s ...\n", filename);

				std::string json = "{";

				game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_LOCALIZE_ENTRY,
					[](game::XAssetHeader header, void* data)
				{
					auto* result = static_cast<std::string*>(data);
					// The XAssetHeader.data for LOCALIZE_ENTRY points to LocalizeEntry
					auto* entry = static_cast<LocalizeEntry*>(header.data);
					if (!entry || !entry->name || !entry->value) return;

					// Escape JSON strings
					auto escape_json = [](const char* s) -> std::string
					{
						std::string out;
						out.reserve(strlen(s) + 2);
						for (const char* p = s; *p; p++)
						{
							switch (*p)
							{
							case '"': out += "\\\""; break;
							case '\\': out += "\\\\"; break;
							case '\n': out += "\\n"; break;
							case '\r': out += "\\r"; break;
							case '\t': out += "\\t"; break;
							default: out += *p;
							}
						}
						return out;
					};

					static int c = 0;
					c++;
					if (c > 1) result->append(",\n");
					result->append("  \"");
					result->append(escape_json(entry->name));
					result->append("\": \"");
					result->append(escape_json(entry->value));
					result->append("\"");
				}, &json, false);

				json += "\n}\n";

				utils::io::write_file(filename, json, false);
				console::info("Dumped localized strings to %s\n", filename);
			});
		}
	};
}

REGISTER_COMPONENT(localized_strings::component)
