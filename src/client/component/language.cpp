#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "language.hpp"
#include "localized_strings.hpp"
//#include "d3d11_overlay.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "command.hpp"
#include "component/gsc/script_loading.hpp"

#include <utils/io.hpp>
#include <utils/hook.hpp>
#include "game/ui_scripting/execution.hpp"

namespace language
{
	namespace
	{
		lang current_language = lang::english;
		std::unordered_map<std::string, std::string> translations_cache;
		std::unordered_set<std::string> g_untranslated;

		// Track fastfile loading activity and game init time to detect
		// when map loading finishes (for SL_ConvertToString hook activation).
		std::chrono::steady_clock::time_point g_last_fastfile_time{};
		std::chrono::steady_clock::time_point g_game_init_time{};
		std::chrono::steady_clock::time_point g_sv_loaded_time{};

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

			const auto files = utils::io::list_files("data/translations");
			size_t file_count = 0;

			for (const auto& file : files)
			{
				if (file.size() < 6 || !file.ends_with(".json"))
					continue;

				std::string data;
				if (!utils::io::read_file(file, &data))
					continue;

				rapidjson::Document doc;
				if (doc.Parse(data).HasParseError() || !doc.IsObject())
					continue;

				for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
				{
					if (it->value.IsString())
					{
						translations_cache[it->name.GetString()] = it->value.GetString();
					}
				}
				file_count++;
			}

			console::info("language: Loaded %zu translations from %zu files\n",
				translations_cache.size(), file_count);
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

		// Stable storage for patched LocalizeEntry asset value pointers.
		// Must be persistent — asset value pointers point INTO these strings.
		static std::deque<std::string> lentry_stable_strings;
		static std::unordered_set<void*> lentry_patched_addrs;

		// SL_ConvertToString hook: intercepts all GSC string conversions.
		// Engine.Localize() resolves keys through SL_ConvertToString
		// internally, bypassing SEH_StringEd_GetString. We intercept
		// the returned English text and replace it with Chinese.
		static std::unordered_map<std::string, std::string> sl_reverse;
		static std::vector<std::string> sl_reverse_stable;

		// Maps normalized form -> {template_english, chinese_translation}
		// Used at render time to diff template vs runtime English and
		// resolve key bindings ([{+activate}] -> "F") in translations.

		// Helper: normalize key binding tokens in a string so that
		// "Hold ^3F^7 To Enable" and "Hold ^3[{+activate}]^7 To Enable"
		// both become "Hold ^3&&1^7 To Enable".
		std::string norm_key_bindings(const std::string& s)
		{
			std::string r;
			for (const char* p = s.c_str(); *p; )
			{
				if (p[0] == '[' && p[1] == '{' && p[2] == '+')
				{
					// [{+anything}] -> &&1
					auto* end = strstr(p, "}]");
					if (end) { r += "&&1"; p = end + 2; continue; }
				}
				if (p[0] == '^' && (p[1] >= '0' && p[1] <= '9'))
				{
					r.push_back(*p++); // ^
					r.push_back(*p++); // digit
					// Collect key name (letters until next ^), only replace if non-empty
					const char* key_start = p;
					while (*p && *p != '^') p++;
					ptrdiff_t klen = p - key_start;
					if (klen > 0 && klen <= 15 && key_start[0] != ' ')
						r += "&&1";
					else
						r.append(key_start, klen);
					continue;
				}
				r.push_back(*p++);
			}
			return r;
		}

		void build_sl_reverse_from_cache()
		{
			// Build new map, then atomic swap — avoids thread-safety crash.
			decltype(sl_reverse) new_map;
			std::vector<std::string> new_stable;
			new_stable.reserve(translations_cache.size() * 2 + 20000);

			// Source 1: translations cache
			for (const auto& [english, chinese] : translations_cache)
			{
				new_stable.emplace_back(english);
				new_stable.emplace_back(chinese);
				// Also insert normalized key for key-binding variants
				std::string nkey = norm_key_bindings(english);
				if (nkey != english)
				{
					new_stable.emplace_back(std::move(nkey));
					new_stable.emplace_back(chinese);
				}
			}

			// Source 2: sl_reverse.json
			std::string rev_data;
			if (utils::io::read_file("data/sl_reverse.json", &rev_data))
			{
				rapidjson::Document doc;
				if (doc.Parse(rev_data).HasParseError())
				{
					console::error("language: sl_reverse.json parse error at offset %zu\n",
						doc.GetErrorOffset());
				}
				else if (!doc.IsObject())
				{
					console::error("language: sl_reverse.json is not an object\n");
				}
				else
				{
					size_t sl_count = 0;
					for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
					{
						if (!it->value.IsString()) continue;
						std::string key = it->name.GetString();
						std::string val = it->value.GetString();
						if (!translations_cache.contains(key))
						{
							new_stable.emplace_back(key);
							new_stable.emplace_back(val);
							std::string nkey = norm_key_bindings(key);
							if (nkey != key)
							{
								new_stable.emplace_back(std::move(nkey));
								new_stable.emplace_back(val);
							}
						sl_count++;
					}
				}
					console::info("language: Loaded %zu entries from sl_reverse.json\n", sl_count);
				}
			}

			for (size_t i = 0; i + 1 < new_stable.size(); i += 2)
				new_map[new_stable[i]] = new_stable[i + 1];

			sl_reverse_stable.swap(new_stable);
			sl_reverse.swap(new_map);
			console::info("language: Built %zu reverse mappings\n", sl_reverse.size());
		}
		static utils::hook::detour sl_convert_to_string_hook;
		static bool g_sl_hook_active = false;

		const char* sl_convert_to_string_stub(game::scr_string_t stringValue)
		{
			const auto* original = sl_convert_to_string_hook.invoke<const char*>(stringValue);
			if (!original || current_language != lang::schinese)
				return original;

				// Only translate after a post-init delay.
			// During fastfile loading / map verification, the hook
			// stays transparent to avoid interfering with integrity checks.
			// Timer-based guard works in both SP and MP.
			if (!g_sl_hook_active)
				return original;

			// Only intercept strings containing ^ (color codes).
		if (strchr(original, '^'))
		{
			const auto it = sl_reverse.find(original);
			if (it != sl_reverse.end())
				return it->second.c_str();

			// Exact match failed — try normalizing key bindings.
			// Runtime strings have "F", "Space" etc. but sl_reverse keys
			// have "[{+activate}]", "[{+forward}]" placeholders.
			std::string nkey = norm_key_bindings(original);
			static int ndiag = 0;
			if (nkey != original && ndiag < 10) { ndiag++; console::info("SL norm: '%s' -> '%s'\n", original, nkey.c_str()); }
			if (nkey != original)
			{
				const auto it2 = sl_reverse.find(nkey);
				if (it2 != sl_reverse.end())
					return it2->second.c_str();
			}
		}

		if (original[0])
			g_untranslated.insert(original);
		return original;
		}

		size_t patch_localize_assets()
		{
			if (current_language != lang::schinese) return 0;

			size_t patched = 0;

			struct PatchCtx
			{
				decltype(&translations_cache) cache;
				size_t* counter;
				std::deque<std::string>* stable;
				std::unordered_set<void*>* seen;
			} ctx = { &translations_cache, &patched, &lentry_stable_strings, &lentry_patched_addrs };

			game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_LOCALIZE_ENTRY,
				[](game::XAssetHeader header, void* data)
			{
				auto* ctx = static_cast<PatchCtx*>(data);

				struct LocalizeEntry { const char* value; const char* name; };
				auto* entry = static_cast<LocalizeEntry*>(header.data);
				if (!entry || !entry->name || !entry->value) return;

				// Dedup by asset pointer: same name can appear in
				// multiple zones (base + override), each is a
				// different object needing its own patch.
				if (ctx->seen->contains(entry)) return;

				const auto it = ctx->cache->find(entry->name);
				if (it == ctx->cache->end()) return;

				ctx->stable->push_back(it->second);
				entry->value = ctx->stable->back().c_str();

					// Diagnostic: log VIDSUBTITLES patches
					if (entry->name && strstr(entry->name, "VIDSUBTITLES"))
					{
						static int vidpatch_log = 0;
						if (vidpatch_log < 10)
						{
							vidpatch_log++;
							console::info("PATCH: VIDSUBTITLES %s -> %s\n",
								entry->name, entry->value);
						}
					}
				ctx->seen->insert(entry);
				(*ctx->counter)++;
			}, &ctx, true);

			if (patched > 0)
			{
				console::info("language: Patched %zu new LocalizeEntry assets "
					"(%zu total stable strings)\n",
					patched, lentry_stable_strings.size());
			}

			return patched;
		}
	// Stable storage for patched subtitle strings
	static std::deque<std::string> subtitle_stable_strings;

	bool g_subtitle_patch_done = false;

	void patch_subtitle_csv()
	{
		if (current_language != lang::schinese) return;
		if (g_subtitle_patch_done) return;

		game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_STRINGTABLE,
			[](game::XAssetHeader header, void*)
		{
			auto* st = header.stringTable;
			if (!st || !st->name || !strstr(st->name, "subtitles.csv")) return;

			int ncols = st->columnCount;
			int nrows = st->rowCount;
			int patched = 0;
			console::info("language: Patching subtitle StringTable cols=%d rows=%d\n", ncols, nrows);

			// Column 3 is the subtitle text (0=video_name, 1=start, 2=end, 3=text)
			int text_col = (ncols >= 4) ? 3 : (ncols - 1);

			for (int r = 0; r < nrows; r++)
			{
				int idx = r * ncols + text_col;
				auto* cell = &st->values[idx];
				if (!cell || !cell->string || !cell->string[0]) continue;

				// Look up English text in sl_reverse -> Chinese
				const auto it = sl_reverse.find(cell->string);
				if (it == sl_reverse.end()) continue;

				subtitle_stable_strings.push_back(it->second);
				cell->string = subtitle_stable_strings.back().c_str();
				// Update hash to match the new string (avoids verification mismatch)
				cell->hash = game::SL_FindString(cell->string);
				patched++;
			}
			console::info("language: Patched %d/%d subtitle strings\n", patched, nrows);
			g_subtitle_patch_done = true;
		}, nullptr, true);
	}
		// DB_LoadXAssets hook: patches newly-loaded LocalizeEntry assets

		// immediately after each fastfile loads, before any game system

		// (subtitles, HUD, etc.) can cache the original English values.

		static utils::hook::detour db_load_xassets_hook;

	// Bink SDK hooks — intercept subtitle text during video playback
	using HBINK = void*;
	using u32 = unsigned int;

	static HBINK (__stdcall *BinkOpen_orig)(const char* name, u32 flags);
	static void (__stdcall *BinkClose_orig)(HBINK bink);
	static u32 (__stdcall *BinkOpenTrack_orig)(HBINK bink, u32 track_idx);
	static void (__stdcall *BinkCloseTrack_orig)(HBINK bink, u32 track_id);
	static u32 (__stdcall *BinkGetTrackType_orig)(HBINK bink, u32 track_id);
	static u32 (__stdcall *BinkGetTrackData_orig)(HBINK bink, u32 track_id, void* buf, u32 buf_size);
	static u32 (__stdcall *BinkDoFrame_orig)(HBINK bink);
	static void (__stdcall *BinkNextFrame_orig)(HBINK bink);
	static int g_bink_playing = 0; // shared flag for diagnostics

	// Track type names for logging
	const char* bink_track_type_name(u32 t) {
		switch (t) {
		case 0: return "video";
		case 1: return "audio";
		default: return "other";
		}
	}

	HBINK __stdcall BinkOpen_stub(const char* name, u32 flags)
	{
		auto* result = BinkOpen_orig(name, flags);
		if (name && strstr(name, ".bik"))
		{
			static int logc = 0;
			if (logc < 30)
			{
				logc++;
				console::info("BINK: BinkOpen('%s') flags=0x%X -> %p\n", name, flags, result);
			}
			// Activate D3D11 overlay for cutscene videos (but not menu backgrounds)
			if (result && !strstr(name, "menus_bg") && !strstr(name, "sp_menus"))
			{
//				d3d11_overlay::set_cutscene_active(true);
//				d3d11_overlay::set_cutscene_video(name);
				console::info("d3d11_overlay: cutscene started\n");
			}
		}
		return result;
	}

	u32 __stdcall BinkOpenTrack_stub(HBINK bink, u32 track_idx)
	{
		u32 track_id = BinkOpenTrack_orig(bink, track_idx);
		if (track_id)
		{
			u32 ttype = BinkGetTrackType_orig ? BinkGetTrackType_orig(bink, track_id) : 0xFFFFFFFF;
			static int logc = 0;
			if (logc < 50)
			{
				logc++;
				console::info("BINK: BinkOpenTrack(bink=%p, idx=%u) -> id=%u type=%u(%s)\n",
					bink, track_idx, track_id, ttype, bink_track_type_name(ttype));
			}
		}
		return track_id;
	}

	u32 __stdcall BinkGetTrackData_stub(HBINK bink, u32 track_id, void* buf, u32 buf_size)
	{
		u32 bytes = BinkGetTrackData_orig(bink, track_id, buf, buf_size);
		if (bytes > 0 && buf && buf_size >= 4)
		{
			static int logc = 0;
			if (logc < 80)
			{
				logc++;
				// Try to interpret as text
				char preview[128] = {};
				u32 plen = bytes < 120 ? bytes : 120;
				memcpy(preview, buf, plen);
				for (u32 i = 0; i < plen; i++)
					if (preview[i] == '\r' || preview[i] == '\n') preview[i] = ' ';
				bool is_text = true;
				for (u32 i = 0; i < plen && is_text; i++)
					if ((unsigned char)preview[i] < 0x20 && preview[i] != '\0') is_text = false;
				console::info("BINK: GetTrackData(bink=%p, id=%u) -> %u bytes, text=%s: '%s'\n",
					bink, track_id, bytes, is_text ? "yes" : "no", is_text ? preview : "(binary)");
			}
		}
		return bytes;
	}

	void __stdcall BinkClose_stub(HBINK bink)
	{
		static int logc = 0;
		if (logc < 30) { logc++; console::info("BINK: BinkClose(%p)\n", bink); }
		g_bink_playing = 0;
//		d3d11_overlay::set_cutscene_active(false);
		BinkClose_orig(bink);
	}

	u32 __stdcall BinkDoFrame_stub(HBINK bink)
	{
		static u32 fc = 0; fc++;
		if (fc <= 3 || fc % 300 == 0)
			console::info("BINK: BinkDoFrame #%u (bink=%p, playing=%d)\n", fc, bink, g_bink_playing);
		g_bink_playing = 1;
		return BinkDoFrame_orig(bink);
	}

	void __stdcall BinkNextFrame_stub(HBINK bink)
	{
		static u32 fc = 0; fc++;
		if (fc <= 3 || fc % 300 == 0)
			console::info("BINK: BinkNextFrame #%u (bink=%p)\n", fc, bink);
		BinkNextFrame_orig(bink);
	}

	void init_bink_hooks()
	{
		HMODULE bink = GetModuleHandleA("bink2w64.dll");
		if (!bink)
		{
			console::info("language: bink2w64.dll not loaded, skipping Bink hooks\n");
			return;
		}

		auto get_proc = [&](const char* name) -> void* {
			void* p = GetProcAddress(bink, name);
			if (!p) console::error("language: Bink function '%s' not found\n", name);
			return p;
		};

		BinkOpen_orig = (decltype(BinkOpen_orig))get_proc("BinkOpen");
		BinkClose_orig = (decltype(BinkClose_orig))get_proc("BinkClose");
		BinkOpenTrack_orig = (decltype(BinkOpenTrack_orig))get_proc("BinkOpenTrack");
		BinkCloseTrack_orig = (decltype(BinkCloseTrack_orig))get_proc("BinkCloseTrack");
		BinkGetTrackType_orig = (decltype(BinkGetTrackType_orig))get_proc("BinkGetTrackType");
		BinkGetTrackData_orig = (decltype(BinkGetTrackData_orig))get_proc("BinkGetTrackData");
		BinkDoFrame_orig = (decltype(BinkDoFrame_orig))get_proc("BinkDoFrame");
		BinkNextFrame_orig = (decltype(BinkNextFrame_orig))get_proc("BinkNextFrame");

		// Only hook if all functions found
		if (!BinkOpenTrack_orig || !BinkGetTrackData_orig || !BinkGetTrackType_orig)
		{
			console::error("language: Missing Bink functions, skipping hooks\n");
			return;
		}

		// Use MinHook directly for exported DLL functions
		MH_Initialize();

		auto mh_hook = [](void* target, void* detour, void** orig, const char* name) {
			MH_STATUS s = MH_CreateHook(target, detour, orig);
			if (s == MH_OK) {
				MH_EnableHook(target);
				console::info("language: Hooked Bink %s\n", name);
			} else {
				console::error("language: Failed to hook Bink %s: %d\n", name, (int)s);
			}
		};

		mh_hook(BinkOpenTrack_orig, BinkOpenTrack_stub, (void**)&BinkOpenTrack_orig, "BinkOpenTrack");
		mh_hook(BinkGetTrackData_orig, BinkGetTrackData_stub, (void**)&BinkGetTrackData_orig, "BinkGetTrackData");
		mh_hook(BinkOpen_orig, BinkOpen_stub, (void**)&BinkOpen_orig, "BinkOpen");
		mh_hook(BinkDoFrame_orig, BinkDoFrame_stub, (void**)&BinkDoFrame_orig, "BinkDoFrame");
		mh_hook(BinkClose_orig, BinkClose_stub, (void**)&BinkClose_orig, "BinkClose");
		mh_hook(BinkNextFrame_orig, BinkNextFrame_stub, (void**)&BinkNextFrame_orig, "BinkNextFrame");

		console::info("language: Bink hooks installed\n");
	}




		void db_load_xassets_stub(game::XZoneInfo* zoneInfo, unsigned int zoneCount, game::DBSyncMode syncMode)

		{

			db_load_xassets_hook.invoke<void>(zoneInfo, zoneCount, syncMode);

			if (current_language == lang::schinese)

			{

				g_last_fastfile_time = std::chrono::steady_clock::now();
				patch_localize_assets();

				// SV_Loaded() monitoring is handled by the
				// continuous loop in on_game_initialized.

			}

		}





	}

	lang get_language()
	{
		return current_language;
	}

	bool is_bink_playing()
	{
		return g_bink_playing != 0;
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
				return it->second.c_str();

			// Exact match failed — try normalizing key bindings.
			// Runtime strings have literal key names ("F", "Space")
			// but sl_reverse keys have "[{+activate}]" placeholders
			// or "&&1" normalized markers. The normalization collapses
			// both forms to "&&1" so they can match.
			if (strchr(english.c_str(), '^'))
			{
				std::string nkey = norm_key_bindings(english);
				if (nkey != english)
				{
					const auto it2 = sl_reverse.find(nkey);
					if (it2 != sl_reverse.end())
						return it2->second.c_str();
				}
			}

			// Diagnostic: log first few misses for debugging
			static int miss = 0;
			if (miss < 20 && english.size() > 5)
			{
				miss++;
				console::info("language: sl_reverse MISS '%s'\n", english.c_str());
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
			build_sl_reverse_from_cache();

			apply_translations();

			// patch_localize_assets at post_unpack is too early — no
			// fastfiles are loaded yet. Log the count for diagnostics.
			size_t initial_patched = patch_localize_assets();
			console::info("language: Initial patch (post_unpack): %zu assets\n", initial_patched);
			//
			// Instead, we intercept Engine.Localize() at the Lua level.
			// After the Lua VM initializes, we inject a script that wraps
			// Engine.Localize with our translation lookup. This is safe
			// because it only affects Lua-level localization calls, not
			// the underlying C string infrastructure that the persistent
			// data system relies on.


			scheduler::on_game_initialized([]
				{
				if (current_language != lang::schinese) return;

				// Immediately patch all currently-loaded LocalizeEntry assets.
				patch_localize_assets();
				patch_subtitle_csv();
				// Hook DB_LoadXAssets to patch assets after each fastfile load.
				db_load_xassets_hook.create(
					SELECT_VALUE(0x14017FB20, 0x140270F30), db_load_xassets_stub);
				console::info("language: Hooked DB_LoadXAssets for immediate patching\n");

				// Hook Bink SDK to intercept cutscene subtitle text
				init_bink_hooks();

				[&]{
				if (!game::hks::lua_state) return;
				const auto state = *game::hks::lua_state;
				if (!state)
				{
					console::info("language: Lua state not ready, cannot hook Engine.Localize\n");
					return;
				}

				ui_scripting::table globals(state->globals.v.table);

				// Verify Engine.Localize exists before wrapping
				auto engine = globals["Engine"];
				if (!engine.is<ui_scripting::table>())
				{
					console::info("language: Engine table not found in Lua\n");
					return;
				}

				auto localize = engine.as<ui_scripting::table>().get("Localize");
				if (!localize.is<ui_scripting::function>())
				{
					console::info("language: Engine.Localize not found or not a function\n");
					return;
				}

				// Lua script that wraps Engine.Localize with our translation cache.
				// game:gettranslatedstring(key) is registered in ui_scripting.cpp
				// setup_functions() and returns the Chinese translation or nil.
				const char* wrap_script =
					"local origLocalize = Engine.Localize\n"
					"Engine.Localize = function(key, ...)\n"
					"    if type(key) == \"string\" then\n"
					"        local translated = game:gettranslatedstring(key)\n"
					"        if translated ~= nil then\n"
					"            local n = select(\"#\", ...)\n"
					"            if n > 0 then\n"
					"                local result = translated\n"
					"                for i = 1, n do\n"
					"                    local arg = select(i, ...)\n"
					"                    result = result:gsub(\"&&\" .. i, tostring(arg))\n"
					"                end\n"
					"                return result\n"
					"            end\n"
					"            return translated\n"
					"        end\n"
					"    end\n"
					"    return origLocalize(key, ...)\n"
					"end\n";

				// Compile and execute via loadstring+pcall for safety
				auto loadstring_fn = globals["loadstring"];
				if (!loadstring_fn.is<ui_scripting::function>())
				{
					console::info("language: loadstring not found in Lua\n");
					return;
				}

				auto compiled = loadstring_fn.as<ui_scripting::function>()(wrap_script, "language_hook");
				if (compiled.empty() || !compiled[0].is<ui_scripting::function>())
				{
					console::info("language: Failed to compile Engine.Localize wrapper\n");
					return;
				}

				auto pcall_fn = globals["pcall"];
				if (pcall_fn.is<ui_scripting::function>())
				{
					pcall_fn.as<ui_scripting::function>()(compiled[0]);
				}
				else
				{
					// No pcall available, just call directly
					compiled[0].as<ui_scripting::function>()();
				}

				console::info("language: Hooked Engine.Localize via Lua wrapper\n");


			}();
				// Enable SL_ConvertToString hook. Initially dormant
				// (g_sl_hook_active=false), activated after fastfile
				// loading settles to avoid interfering with map
				// loading integrity verification.
				sl_convert_to_string_hook.create(
					SELECT_VALUE(0x140314850, 0x1403F0F10), sl_convert_to_string_stub);
				console::info("language: Hooked SL_ConvertToString for GSC display strings\n");
				g_game_init_time = std::chrono::steady_clock::now();
				g_last_fastfile_time = g_game_init_time;
				// Continuous SV_Loaded() monitor: detects map
				// entry/exit and manages SL hook activation.
				// Activates 10s after map load; deactivates on
				// map exit so the next map can verify cleanly.
				scheduler::schedule([]
				{
					static bool was_loaded = false;
					bool is_loaded = game::SV_Loaded();

					if (was_loaded && !is_loaded)
					{
						g_sl_hook_active = false;
						g_sv_loaded_time = std::chrono::steady_clock::time_point{};
						console::info("language: Map exited, SL deactivated\n");
					}
					was_loaded = is_loaded;

					if (g_sl_hook_active || current_language != lang::schinese)
						return scheduler::cond_continue;

					auto now = std::chrono::steady_clock::now();

					if (is_loaded && g_sv_loaded_time == std::chrono::steady_clock::time_point{})
					{
						g_sv_loaded_time = now;
						console::info("language: Map loaded, waiting 10s...\n");
					}

					auto ff_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
						now - g_last_fastfile_time).count();
					auto sv_elapsed = g_sv_loaded_time != std::chrono::steady_clock::time_point{}
						? std::chrono::duration_cast<std::chrono::seconds>(
							now - g_sv_loaded_time).count() : 0;

					if ((g_sv_loaded_time != std::chrono::steady_clock::time_point{} && sv_elapsed >= 10)
						|| ff_elapsed >= 30)
					{
						g_sl_hook_active = true;
						scheduler::once([] { patch_subtitle_csv(); }, scheduler::pipeline::main, 10s);
						console::info("language: SL_ConvertToString now active "
							"(sv=%llds ff=%llds)\n",
							static_cast<long long>(sv_elapsed),
							static_cast<long long>(ff_elapsed));
					}

					return scheduler::cond_continue;
				}, scheduler::pipeline::main, 1s);

				// Periodically patch newly-loaded LocalizeEntry assets.
				// Fastfiles for each map are loaded after game init,
				// so a single call at post_unpack is too early.
				// Deduplication (lentry_patched_addrs) makes repeat
				// calls cheap — only new assets are patched.
				scheduler::loop([]
				{
					if (current_language != lang::schinese) return;
					size_t patched = patch_localize_assets();
					if (patched > 0)
					{
						console::info("language: Patched %zu new LocalizeEntry assets\n",
							patched);
					}
				}, scheduler::pipeline::main, 3s);
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
