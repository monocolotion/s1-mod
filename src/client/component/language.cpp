#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "language.hpp"
#include "localized_strings.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "command.hpp"
#include "component/gsc/script_loading.hpp"

#include <utils/io.hpp>

namespace language
{
	namespace
	{
		lang current_language = lang::english;
		std::unordered_map<std::string, std::string> translations_cache;
		std::unordered_set<std::string> g_untranslated;

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

		void load_sl_reverse()
		{
			sl_reverse.clear();
			sl_reverse_stable.clear();

			std::string data;
			if (!utils::io::read_file("data/sl_reverse.json", &data))
			{
				console::info("language: sl_reverse.json not found, "
					"GSC display strings will not be translated\n");
				return;
			}

			rapidjson::Document doc;
			if (doc.Parse(data).HasParseError() || !doc.IsObject())
			{
				console::error("language: Failed to parse sl_reverse.json\n");
				return;
			}

			sl_reverse_stable.reserve(doc.MemberCount() * 2);
			for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
			{
				if (!it->value.IsString())
					continue;

				sl_reverse_stable.emplace_back(it->name.GetString());
				sl_reverse_stable.emplace_back(it->value.GetString());
			}

			for (size_t i = 0; i + 1 < sl_reverse_stable.size(); i += 2)
			{
				sl_reverse[sl_reverse_stable[i]] = sl_reverse_stable[i + 1];
			}

			console::info("language: Loaded %zu reverse mappings "
				"(English -> Chinese) from sl_reverse.json\n",
				sl_reverse.size());
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
		//

		// SL_ConvertToString hook: intercepts all GSC string conversions.
		static std::unordered_map<std::string, std::string> sl_reverse;
		static std::vector<std::string> sl_reverse_stable;
		static utils::hook::detour sl_convert_to_string_hook;

		const char* sl_convert_to_string_stub(game::scr_string_t stringValue)
		{
			const auto* original = sl_convert_to_string_hook.invoke<const char*>(stringValue);
			if (!original || current_language != lang::schinese)
				return original;

			// Only translate while the game is fully running.
			if (!game::CL_IsCgameInitialized())
				return original;

			if (strchr(original, '^'))
			{
				const auto it = sl_reverse.find(original);
				if (it != sl_reverse.end())
					return it->second.c_str();
			}

			if (original[0])
				g_untranslated.insert(original);
			return original;
		}
		// Stable storage that outlives patch_localize_assets() calls.
		// Asset value pointers point INTO this vector, so it must be persistent.
		static std::deque<std::string> lentry_stable_strings;
		static std::unordered_set<std::string> lentry_patched;

		void patch_localize_assets()
		{
			if (current_language != lang::schinese) return;

			size_t patched = 0;

			struct PatchCtx
			{
				decltype(&translations_cache) cache;
				size_t* counter;
				std::deque<std::string>* stable;
				std::unordered_set<std::string>* seen;
			} ctx = { &translations_cache, &patched, &lentry_stable_strings, &lentry_patched };

			game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_LOCALIZE_ENTRY,
				[](game::XAssetHeader header, void* data)
			{
				auto* ctx = static_cast<PatchCtx*>(data);

				struct LocalizeEntry { const char* value; const char* name; };
				auto* entry = static_cast<LocalizeEntry*>(header.data);
				if (!entry || !entry->name || !entry->value) return;

				// Skip if already patched on a previous call
				if (ctx->seen->contains(entry->name)) return;

				const auto it = ctx->cache->find(entry->name);
				if (it == ctx->cache->end()) return;

				ctx->stable->push_back(it->second);
				entry->value = ctx->stable->back().c_str();
				ctx->seen->insert(entry->name);
				(*ctx->counter)++;
			}, &ctx, false);

			if (patched > 0)
			{
				console::info("language: Patched %zu new LocalizeEntry assets "
					"(%zu total stable strings)\n",
					patched, lentry_stable_strings.size());
			}
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

		const char* get_translation_by_english(const std::string& english)
		{
			if (current_language != lang::schinese) return nullptr;
			const auto it = sl_reverse.find(english);
			if (it != sl_reverse.end())
			{
				return it->second.c_str();
			}
			return nullptr;
		}

		const char* get_translation_any(const std::string& str)
		{
			const char* result = get_translation(str);
			if (result) return result;
			return get_translation_by_english(str);
		}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			load_language_from_config();
			load_translations_cache();
			load_sl_reverse();

			apply_translations();
			patch_localize_assets();

			scheduler::on_game_initialized([]()
			{
				patch_localize_assets();

				// Enable SL_ConvertToString hook for GSC display strings.
				// Guarded by CL_IsCgameInitialized() to avoid map verification issues.
				sl_convert_to_string_hook.create(
					SELECT_VALUE(0x140314850, 0x1403F0F10), sl_convert_to_string_stub);
			}, scheduler::pipeline::main);

			// Batch-export all collected untranslated strings to JSON
			command::add("dumpuntranslated", [](const command::params& params)
			{
				const char* filename = "data/untranslated.json";
				if (params.size() >= 2)
					filename = params[1];

				console::info("Dumping %zu untranslated strings to %s ...\n",
					g_untranslated.size(), filename);

				std::string json = "{\n  \"untranslated\": [\n";
				bool first = true;
				for (const auto& s : g_untranslated)
				{
					if (!first) json += ",\n";
					first = false;
					json += "    \"";
					for (char c : s)
					{
						switch (c) {
						case '"': json += "\\\""; break;
						case '\\': json += "\\\\"; break;
						case '\n': json += "\\n"; break;
						case '\r': json += "\\r"; break;
						case '\t': json += "\\t"; break;
						default: json += c;
						}
					}
					json += "\"";
				}
				json += "\n  ]\n}\n";

				utils::io::write_file(filename, json, false);
				console::info("Dumped %zu untranslated strings to %s\n",
					g_untranslated.size(), filename);
			});

			// Dump all StringTable assets (scr_string_t hash->string mappings)
			// from loaded fastfiles.
			command::add("dumpstringtable", [](const command::params& params)
			{
				const char* filename = "data/stringtables.json";
				if (params.size() >= 2)
					filename = params[1];

				struct DumpCtx
				{
					std::string* json;
					int total_strings;
					int table_count;
					bool first_table;
				};

				console::info("Dumping string tables to %s ...\n", filename);

				std::string json = "{";
				DumpCtx ctx = { &json, 0, 0, true };

				game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_STRINGTABLE,
					[](game::XAssetHeader header, void* data)
				{
					auto* ctx_ = static_cast<DumpCtx*>(data);
					auto* table = header.stringTable;
					if (!table || !table->name || !table->values) return;

					const int entry_count = table->columnCount * table->rowCount;
					if (entry_count <= 0) return;

					if (!ctx_->first_table)
						ctx_->json->append(",\n");
					ctx_->first_table = false;
					ctx_->table_count++;

					ctx_->json->append("  \"");
					for (const char* p = table->name; *p; p++)
					{
						switch (*p) {
						case '"': ctx_->json->append("\\\""); break;
						case '\\': ctx_->json->append("\\\\"); break;
						default: ctx_->json->push_back(*p);
						}
					}
					ctx_->json->append("\": [\n");

					bool first_entry = true;
					for (int i = 0; i < entry_count; i++)
					{
						const char* s = table->values[i].string;
						if (!s || !s[0]) continue;

						if (!first_entry) ctx_->json->append(",\n");
						first_entry = false;

						ctx_->json->append("    {\"h\": ");
						ctx_->json->append(std::to_string(table->values[i].hash));
						ctx_->json->append(", \"s\": \"");
						for (const char* p = s; *p; p++)
						{
							switch (*p) {
							case '"': ctx_->json->append("\\\""); break;
							case '\\': ctx_->json->append("\\\\"); break;
							case '\n': ctx_->json->append("\\n"); break;
							case '\r': ctx_->json->append("\\r"); break;
							case '\t': ctx_->json->append("\\t"); break;
							default: ctx_->json->push_back(*p);
							}
						}
						ctx_->json->append("\"}");
						ctx_->total_strings++;
					}
					ctx_->json->append("\n  ]");
				}, &ctx, false);

				json += "\n}\n";

				utils::io::write_file(filename, json, false);
				console::info("Dumped %d string tables (%d strings) to %s\n",
					ctx.table_count, ctx.total_strings, filename);
			});

			// Dump all GSC script token strings from the gsc-tool token table.
			command::add("dumpgscstrings", [](const command::params& params)
			{
				const char* filename = "data/gsc_tokens.json";
				if (params.size() >= 2)
					filename = params[1];

				if (!gsc::gsc_ctx)
				{
					console::error("gsc_ctx is not initialized\n");
					return;
				}

				console::info("Dumping GSC token strings to %s ...\n", filename);

				constexpr unsigned int max_id = 0xA7DC;
				std::string json = "{\n  \"tokens\": [\n";
				bool first = true;
				int count = 0;

				for (unsigned int i = 0; i <= max_id; i++)
				{
					auto name = gsc::gsc_ctx->token_name(i);
					if (name.starts_with("_id_"))
						continue;

					if (!first) json += ",\n";
					first = false;
					count++;

					json += "    {\"h\": ";
					json += std::to_string(i);
					json += ", \"s\": \"";
					for (char c : name)
					{
						switch (c) {
						case '"': json += "\\\""; break;
						case '\\': json += "\\\\"; break;
						default: json.push_back(c);
						}
					}
					json += "\"}";
				}

				json += "\n  ]\n}\n";

				utils::io::write_file(filename, json, false);
				console::info("Dumped %d GSC tokens to %s\n", count, filename);
			});
		}
	};
}

REGISTER_COMPONENT(language::component)
