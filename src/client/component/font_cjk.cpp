#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "font_cjk.hpp"
#include "language.hpp"
#include "scheduler.hpp"
#include "game/game.hpp"
#include "console.hpp"

#include <utils/hook.hpp>
#include <utils/io.hpp>
#include <d3d11.h>

#pragma warning(push)
#pragma warning(disable: 4459)
#pragma warning(disable: 4996)
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#pragma warning(pop)

namespace font_cjk
{
	namespace
	{
		constexpr int ATLAS_W = 2048;
		constexpr int ATLAS_H = 2048;
		constexpr float FONT_SIZE = 32.0f;

		bool ready = false;
		game::GfxImage* font_image = nullptr;

		ID3D11Texture2D* cjk_atlas_tex = nullptr;
		ID3D11ShaderResourceView* cjk_atlas_srv = nullptr;

		std::vector<game::Glyph> cjk_glyphs;
		std::vector<unsigned char> atlas_rgba;

		std::vector<game::Font_s*> registered_fonts;
		std::unordered_set<game::Font_s*> injected_fonts;

		// Per-font max ascent ratio captured from original glyphs before CJK injection.
		std::unordered_map<game::Font_s*, float> font_ascent_ratio;

		game::Material* cjk_fallback_material = nullptr;

		// CJK font metrics computed during bake()
		float cjk_ascent_ratio = 0.72f; // max(|yoff|) / FONT_SIZE
		float cjk_height_ratio = 0.90f; // max(pixelHeight) / FONT_SIZE

		utils::hook::detour r_add_cmd_draw_text_hook;
		utils::hook::detour r_add_cmd_draw_text_with_cursor_hook;
		utils::hook::detour r_register_font_hook;
		utils::hook::detour image_setup_hook;

		void replace_font_atlas_texture(game::GfxImage* font_img, const char* matched_name);
		void try_early_cjk_init(game::GfxImage* image);

		bool is_font_atlas_image(const char* name)
		{
			if (!name) return false;
			const char* p = name;
			while (*p)
			{
				if ((p[0] == 'g' || p[0] == 'G') &&
					(p[1] == 'a' || p[1] == 'A') &&
					(p[2] == 'm' || p[2] == 'M') &&
					(p[3] == 'e' || p[3] == 'E') &&
					(p[4] == 'f' || p[4] == 'F') &&
					(p[5] == 'o' || p[5] == 'O') &&
					(p[6] == 'n' || p[6] == 'N') &&
					(p[7] == 't' || p[7] == 'T') &&
					(p[8] == 's' || p[8] == 'S'))
					return true;
				p++;
			}
			return false;
		}

		char* image_setup_stub(game::GfxImage* image, uint32_t a, uint32_t b,
			uint32_t c, uint32_t d, uint32_t e, DXGI_FORMAT f, const char* name,
			const D3D11_SUBRESOURCE_DATA* g)
		{
			auto* r = image_setup_hook.invoke<char*>(image, a, b, c, d, e, f, name, g);

			if (name && is_font_atlas_image(name))
			{
				console::info("font_cjk: Image_Setup font atlas: '%s' fmt=%d ready=%d\n",
					name, (int)f, ready ? 1 : 0);
			}

			if (!ready && name && strcmp(name, "gamefonts_pc") == 0)
			{
				try_early_cjk_init(image);
			}

			if (ready && name && is_font_atlas_image(name) && cjk_atlas_srv)
			{
				replace_font_atlas_texture(image, name);
			}
			return r;
		}

		std::string find_cjk_font()
		{
			const std::vector<std::string> paths = {
				"data/fonts/cjk.ttf", "data/fonts/cjk.otf",
				"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msyhbd.ttc",
				"C:\\Windows\\Fonts\\simsun.ttc", "C:\\Windows\\Fonts\\simhei.ttf",
			};
			for (const auto& p : paths)
				if (utils::io::file_exists(p)) return p;
			return {};
		}

		std::vector<int> collect_codepoints()
		{
			std::vector<int> cp;
			for (int ic = 32; ic <= 126; ic++) cp.push_back(ic);
			std::string json;
			if (utils::io::read_file("data/translations.json", &json))
			{
				rapidjson::Document doc;
				if (!doc.Parse(json).HasParseError() && doc.HasMember("schinese"))
				{
					for (auto it = doc["schinese"].MemberBegin(); it != doc["schinese"].MemberEnd(); ++it)
					{
						if (!it->value.IsString()) continue;
						const char* s = it->value.GetString();
						while (*s)
						{
							unsigned char cval = static_cast<unsigned char>(*s);
							int u;
							if (cval < 0x80) { u = cval; s += 1; }
							else if ((cval & 0xE0) == 0xC0) { u = (cval & 0x1F) << 6; u |= (static_cast<unsigned char>(s[1]) & 0x3F); s += 2; }
							else if ((cval & 0xF0) == 0xE0) { u = (cval & 0x0F) << 12; u |= (static_cast<unsigned char>(s[1]) & 0x3F) << 6; u |= (static_cast<unsigned char>(s[2]) & 0x3F); s += 3; }
							else if ((cval & 0xF8) == 0xF0) { u = (cval & 0x07) << 18; u |= (static_cast<unsigned char>(s[1]) & 0x3F) << 12; u |= (static_cast<unsigned char>(s[2]) & 0x3F) << 6; u |= (static_cast<unsigned char>(s[3]) & 0x3F); s += 4; }
							else { s += 1; continue; }
							if (u > 127 && u <= 0xFFFF) cp.push_back(u);
						}
					}
				}
			}
			if (!cp.empty())
				for (int u = 0x4E00; u <= 0x5200; u++) cp.push_back(u);
			std::sort(cp.begin(), cp.end());
			cp.erase(std::unique(cp.begin(), cp.end()), cp.end());
			return cp;
		}

		bool bake(const std::string& font_path)
		{
			std::string fd;
			if (!utils::io::read_file(font_path, &fd)) return false;
			std::vector<unsigned char> gray(ATLAS_W * ATLAS_H, 0);
			auto cp = collect_codepoints();

			stbtt_pack_context pc{};
			if (!stbtt_PackBegin(&pc, gray.data(), ATLAS_W, ATLAS_H, 0, 1, nullptr)) return false;
			stbtt_PackSetOversampling(&pc, 1, 1);

			std::vector<stbtt_packedchar> packed(cp.size());
			std::vector<stbtt_pack_range> ranges;
			size_t start = 0;
			for (size_t i = 1; i <= cp.size(); i++)
			{
				if (i == cp.size() || cp[i] != cp[i - 1] + 1)
				{
					stbtt_pack_range r{};
					r.font_size = FONT_SIZE;
					r.first_unicode_codepoint_in_range = cp[start];
					r.num_chars = static_cast<int>(cp[i - 1] - cp[start] + 1);
					r.chardata_for_range = &packed[start];
					ranges.push_back(r);
					start = i;
				}
			}
			if (!stbtt_PackFontRanges(&pc, reinterpret_cast<const unsigned char*>(fd.data()), 0,
					ranges.data(), static_cast<int>(ranges.size())))
				{ stbtt_PackEnd(&pc); return false; }
			stbtt_PackEnd(&pc);

			cjk_glyphs.clear();
			cjk_glyphs.reserve(cp.size());
			atlas_rgba.assign(ATLAS_W * ATLAS_H * 4, 0);

			float max_ascent = 0.0f;
			float max_glyph_height = 0.0f;
			int cjk_glyph_count = 0;
			for (size_t i = 0; i < cp.size(); i++)
			{
				const auto& b = packed[i];
				game::Glyph g{};
				g.letter = static_cast<unsigned short>(cp[i]);

				auto to_char = [](float v, const char* field, int cp) -> char {
					int iv = static_cast<int>(v);
					if (iv > 127 || iv < -128) {
						static int warn_count = 0;
						if (warn_count < 20) {
							warn_count++;
							console::info("font_cjk: OVERFLOW %s=%d for U+%04X, clamping\n", field, iv, cp);
						}
						return static_cast<char>(iv > 127 ? 127 : -128);
					}
					return static_cast<char>(iv);
				};

				g.x0 = to_char(b.xoff, "xoff", cp[i]);
				g.y0 = to_char(b.yoff, "yoff", cp[i]);
				g.dx = to_char(b.xadvance, "dx", cp[i]);
				g.pixelWidth = to_char(static_cast<float>(b.x1 - b.x0), "pw", cp[i]);
				g.pixelHeight = to_char(static_cast<float>(b.y1 - b.y0), "ph", cp[i]);
				g.s0 = b.x0 / static_cast<float>(ATLAS_W);
				g.t0 = b.y0 / static_cast<float>(ATLAS_H);
				g.s1 = b.x1 / static_cast<float>(ATLAS_W);
				g.t1 = b.y1 / static_cast<float>(ATLAS_H);
				cjk_glyphs.push_back(g);

				if (cp[i] > 127)
				{
					float ascent = -b.yoff;
					if (ascent > max_ascent) max_ascent = ascent;
					float gh = (float)(b.y1 - b.y0);
					if (gh > max_glyph_height) max_glyph_height = gh;
					cjk_glyph_count++;
				}

				for (int y = b.y0; y < b.y1; y++)
					for (int x = b.x0; x < b.x1; x++)
					{
						size_t idx = (y * ATLAS_W + x) * 4;
						unsigned char val = gray[y * ATLAS_W + x];
						atlas_rgba[idx + 0] = val;
						atlas_rgba[idx + 1] = val;
						atlas_rgba[idx + 2] = val;
						atlas_rgba[idx + 3] = val;
					}
			}

			if (cjk_glyph_count > 0 && max_ascent > 0.0f)
			{
				cjk_ascent_ratio = max_ascent / FONT_SIZE;
				cjk_height_ratio = max_glyph_height / FONT_SIZE;
			}

			// Fine-tune CJK y0 so the glyph sits inside size_scale's expanded
			// bounding box. Target: ascent = 95% of cjk_height_ratio * FONT_SIZE.
			if (cjk_height_ratio > 0.01f && max_ascent > 0.0f)
			{
				float target_y0 = -(FONT_SIZE * cjk_height_ratio * 0.95f);
				float actual_shift = target_y0 - (-max_ascent);
				if (actual_shift < 0.0f)
				{
					for (auto& g : cjk_glyphs)
					{
						int iv = (int)((float)(signed char)g.y0 + actual_shift);
						g.y0 = (char)(iv > 127 ? 127 : (iv < -128 ? -128 : iv));
					}
					console::info("font_cjk: y0 shifted by %.1f (max_ascent %.1f -> %.1f)\n",
						actual_shift, max_ascent, -target_y0);
				}
			}

			stbi_write_png("data/fonts/cjk_atlas.png", ATLAS_W, ATLAS_H, 4,
				atlas_rgba.data(), ATLAS_W * 4);
			console::info("font_cjk: Baked %zu glyphs, saved atlas PNG\n", cjk_glyphs.size());
			return true;
		}

		int decode_utf8(const char*& p)
		{
			if (!*p) return -1;
			unsigned char cval = static_cast<unsigned char>(*p);
			int u;
			if (cval < 0x80) { u = cval; p += 1; }
			else if ((cval & 0xE0) == 0xC0) { u = (cval & 0x1F) << 6; u |= (static_cast<unsigned char>(p[1]) & 0x3F); p += 2; }
			else if ((cval & 0xF0) == 0xE0) { u = (cval & 0x0F) << 12; u |= (static_cast<unsigned char>(p[1]) & 0x3F) << 6; u |= (static_cast<unsigned char>(p[2]) & 0x3F); p += 3; }
			else if ((cval & 0xF8) == 0xF0) { u = (cval & 0x07) << 18; u |= (static_cast<unsigned char>(p[1]) & 0x3F) << 12; u |= (static_cast<unsigned char>(p[2]) & 0x3F) << 6; u |= (static_cast<unsigned char>(p[3]) & 0x3F); p += 4; }
			else { u = -1; p += 1; }
			return u;
		}

		void inject_cjk_glyphs_into_font(game::Font_s* font)
		{
			if (!font || !font->glyphs || cjk_glyphs.empty()) return;
			if (injected_fonts.contains(font)) return;

			// Capture the MAX ascent from original glyphs (excluding space).
			int max_ascent = 0;
			for (int i = 0; i < font->glyphCount && i < 128; i++)
			{
				if (font->glyphs[i].pixelWidth > 0)
				{
					int a = -font->glyphs[i].y0;
					if (a > max_ascent) max_ascent = a;
				}
			}
			if (max_ascent > 0 && font->pixelHeight > 0)
				font_ascent_ratio[font] = (float)max_ascent / (float)font->pixelHeight;
			else
				font_ascent_ratio[font] = 0.72f;

			int nc = (int)cjk_glyphs.size();
			auto* ng = (game::Glyph*)malloc(sizeof(game::Glyph) * nc);
			if (!ng) return;

			// Per-font scaling: glyphs baked at FONT_SIZE=32, scale to pixelHeight.
			// All rendering paths get correct-size glyphs automatically.
			float per_font_scale = (font->pixelHeight > 0 && cjk_height_ratio > 0.01f)
				? ((float)font->pixelHeight / (FONT_SIZE * cjk_height_ratio))
				: 1.0f;

			for (int i = 0; i < nc; i++)
			{
				ng[i] = cjk_glyphs[i];
				auto clamp_char = [](int v) -> char {
					return (char)(v > 127 ? 127 : (v < -128 ? -128 : v));
				};
				ng[i].x0          = clamp_char((int)((signed char)cjk_glyphs[i].x0 * per_font_scale));
				ng[i].y0          = clamp_char((int)((signed char)cjk_glyphs[i].y0 * per_font_scale));
				ng[i].dx          = clamp_char((int)((signed char)cjk_glyphs[i].dx * per_font_scale));
				ng[i].pixelWidth  = clamp_char((int)((signed char)cjk_glyphs[i].pixelWidth * per_font_scale));
				ng[i].pixelHeight = clamp_char((int)((signed char)cjk_glyphs[i].pixelHeight * per_font_scale));
			}

			int oc = font->glyphCount;
			font->glyphs = ng; font->glyphCount = nc;
			injected_fonts.insert(font);

			if (!cjk_fallback_material && font->material && font->material->name &&
				strstr(font->material->name, "zombiefonts_pc"))
			{
				cjk_fallback_material = font->material;
			}

			console::info("font_cjk: Replaced glyphs in '%s' (%d->%d) pH=%d orig_ascent=%.3f\n",
				font->fontName ? font->fontName : "?", oc, nc,
				font->pixelHeight, font_ascent_ratio[font]);
		}

		game::Font_s* r_register_font_stub(const char* font_name)
		{
			auto* font = r_register_font_hook.invoke<game::Font_s*>(font_name);
			if (font)
			{
				registered_fonts.push_back(font);
				if (ready && !cjk_glyphs.empty())
					inject_cjk_glyphs_into_font(font);
			}
			return font;
		}

		void replace_font_atlas_texture(game::GfxImage* font_img, const char* matched_name)
		{
			if (!font_img || !font_img->textures.___u0.map) return;
			if (font_img->textures.___u0.map == cjk_atlas_tex) return;

			if (!cjk_atlas_tex)
			{
				ID3D11Device* device = nullptr;
				font_img->textures.___u0.map->GetDevice(&device);
				if (!device) return;

				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = ATLAS_W; desc.Height = ATLAS_H;
				desc.MipLevels = 1; desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_IMMUTABLE;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

				D3D11_SUBRESOURCE_DATA init{};
				init.pSysMem = atlas_rgba.data();
				init.SysMemPitch = ATLAS_W * 4;
				init.SysMemSlicePitch = ATLAS_W * ATLAS_H * 4;

				HRESULT hr = device->CreateTexture2D(&desc, &init, &cjk_atlas_tex);
				if (FAILED(hr))
				{
					console::error("font_cjk: CreateTexture2D failed hr=%08x\n", (unsigned)hr);
					device->Release(); return;
				}

				hr = device->CreateShaderResourceView(cjk_atlas_tex, nullptr, &cjk_atlas_srv);
				if (FAILED(hr))
				{
					console::error("font_cjk: CreateSRV failed hr=%08x\n", (unsigned)hr);
					cjk_atlas_tex->Release(); cjk_atlas_tex = nullptr;
					device->Release(); return;
				}

				device->Release();
				console::info("font_cjk: Created CJK atlas %dx%d\n", ATLAS_W, ATLAS_H);
			}

			font_img->textures.___u0.map = cjk_atlas_tex;
			font_img->textures.shaderView = cjk_atlas_srv;
			font_img->textures.shaderViewAlternate = cjk_atlas_srv;
			if (!font_image) font_image = font_img;

			D3D11_TEXTURE2D_DESC od{};
			font_img->textures.___u0.map->GetDesc(&od);
			console::info("font_cjk: Replaced '%s': %dx%d fmt=%d ptr=%p\n",
				matched_name, od.Width, od.Height, od.Format, font_img);
		}

		void create_cjk_atlas_texture()
		{
			auto* first = game::DB_FindXAssetHeader(game::ASSET_TYPE_IMAGE, "gamefonts_pc", 1).image;
			if (!first || !first->textures.___u0.map)
			{ console::info("font_cjk: gamefonts_pc not ready\n"); return; }

			replace_font_atlas_texture(first, "gamefonts_pc");

			game::DB_EnumXAssets_FastFile(game::ASSET_TYPE_IMAGE, [](game::XAssetHeader h, void*)
			{
				auto* img = h.image;
				if (img && img->name)
				{
					bool has_font = false;
					for (const char* pn = img->name; *pn; pn++)
					{
						if ((pn[0]=='f'||pn[0]=='F') && (pn[1]=='o'||pn[1]=='O') &&
							(pn[2]=='n'||pn[2]=='N') && (pn[3]=='t'||pn[3]=='T'))
							{ has_font = true; break; }
					}
					if (is_font_atlas_image(img->name) || has_font)
					{
						console::info("font_cjk: found img: '%s' w=%d h=%d\n",
							img->name, img->width, img->height);
					}
				}
			}, nullptr, false);

			const char* zm_candidates[] = {
				"gamefonts_zm", "gamefonts_zm_pc", "gamefonts_pc_zm",
				"zm_gamefonts", "zm_gamefonts_pc",
				"zombiefonts_pc", "devfonts_pc",
			};
			for (const char* name : zm_candidates)
			{
				auto* img = game::DB_FindXAssetHeader(game::ASSET_TYPE_IMAGE, name, 1).image;
				if (img && img->textures.___u0.map && img->textures.___u0.map != cjk_atlas_tex)
				{
					replace_font_atlas_texture(img, name);
				}
			}

			console::info("font_cjk: Replaced gamefonts_pc with CJK atlas %dx%d\n", ATLAS_W, ATLAS_H);
		}

		void try_early_cjk_init(game::GfxImage* image)
		{
			if (ready) return;
			if (!image || !image->textures.___u0.map) return;

			replace_font_atlas_texture(image, "gamefonts_pc");
			if (!cjk_atlas_tex) return;

			console::info("font_cjk: early CJK init - injecting into %zu fonts\n", registered_fonts.size());
			for (auto* f : registered_fonts) inject_cjk_glyphs_into_font(f);

			ready = true;
			console::info("font_cjk: CJK rendering ACTIVE (early)\n");
		}

		void init_after_game_load()
		{
			if (ready) return;

			create_cjk_atlas_texture();
			if (!cjk_atlas_tex)
			{ console::info("font_cjk: Retrying in 50ms...\n");
				scheduler::once(init_after_game_load, scheduler::pipeline::async, 50ms); return; }

			console::info("font_cjk: Replacing glyphs in %zu fonts\n", registered_fonts.size());
			for (auto* f : registered_fonts) inject_cjk_glyphs_into_font(f);

			ready = true;
			console::info("font_cjk: CJK rendering ACTIVE\n");
		}

		// -------------------------------------------------------------------
		// Unified text rendering: instead of custom per-glyph drawing, adjust
		// the draw parameters (scale to match pixelHeight, shift y to match
		// the original font's ascent) and pass through to the original game
		// function. This ensures cursor positioning, kerning, etc. all work.
		// -------------------------------------------------------------------

		// Minimal adjustment: only scale glyphs to match font pixelHeight.
		// The y0 shift in bake() already sets ascent to ~90% of FONT_SIZE,
		// so the original game function positions text correctly on its own.
		void r_add_cmd_draw_text_stub(const char* text, int max_chars, game::Font_s* font,
			float x, float y, float x_scale, float y_scale, float rotation, float* color, int style)
		{
			if (!ready) init_after_game_load();

			if (!ready || !text || !*text || !cjk_atlas_srv)
			{
				r_add_cmd_draw_text_hook.invoke<void>(text, max_chars, font,
					x, y, x_scale, y_scale, rotation, color, style);
				return;
			}

			inject_cjk_glyphs_into_font(font);

			static int cc = 0; cc++;
			if (cc <= 5 || cc % 5000 == 0)
			{
				bool has_cjk = false;
				for (const char* s = text; *s; s++)
					if (*(unsigned char*)s >= 0x80) { has_cjk = true; break; }
				char pv[128]; int pl = 0;
				for (const char* tp = text; *tp && pl < 120; ) pv[pl++] = *tp++;
				pv[pl] = 0;
				console::info("font_cjk: call#%d '%s' font=%s cjk=%s pH=%d\n",
					cc, pv, font->fontName ? font->fontName : "?",
					has_cjk ? "YES" : "no", font->pixelHeight);
			}

			r_add_cmd_draw_text_hook.invoke<void>(text, max_chars, font,
				x, y, x_scale, y_scale,
				rotation, color, style);
		}

		void r_add_cmd_draw_text_with_cursor_stub(const char* text, int max_chars, game::Font_s* font,
			float x, float y, float x_scale, float y_scale, float rotation, const float* color,
			int style, int cursor_pos, char something)
		{
			if (!ready) init_after_game_load();

			if (!ready || !text || !*text || !cjk_atlas_srv)
			{
				r_add_cmd_draw_text_with_cursor_hook.invoke<void>(text, max_chars, font,
					x, y, x_scale, y_scale, rotation, color, style, cursor_pos, something);
				return;
			}
			inject_cjk_glyphs_into_font(font);
			r_add_cmd_draw_text_with_cursor_hook.invoke<void>(text, max_chars, font,
				x, y, x_scale, y_scale,
				rotation, color, style, cursor_pos, something);
		}
	}

	bool is_cjk_available() { return ready; }
	bool baked = false;

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			if (game::environment::is_dedi()) return;
			const auto fp = find_cjk_font();
			if (fp.empty()) { console::info("font_cjk: No CJK font\n"); return; }
			if (!bake(fp)) { console::error("font_cjk: Bake failed\n"); return; }
			baked = true;
		}

		void post_unpack() override
		{
			if (game::environment::is_dedi()) return;
			if (!baked) return;

			image_setup_hook.create(SELECT_VALUE(0x1404858D0, 0x1405A3150), &image_setup_stub);
			console::info("font_cjk: Hooked Image_Setup\n");

			r_register_font_hook.create(SELECT_VALUE(0x140481F90, 0x14059F3C0), &r_register_font_stub);
			console::info("font_cjk: Hooked R_RegisterFont\n");

			r_add_cmd_draw_text_hook.create(
				SELECT_VALUE(0x1404A2BF0, 0x1405C1320), &r_add_cmd_draw_text_stub);
			r_add_cmd_draw_text_with_cursor_hook.create(
				SELECT_VALUE(0x1404A35E0, 0x1405C1D10), &r_add_cmd_draw_text_with_cursor_stub);

			scheduler::on_game_initialized([]() { init_after_game_load(); }, scheduler::pipeline::async);
			console::info("font_cjk: Waiting for game init...\n");
		}
	};
}

REGISTER_COMPONENT(font_cjk::component)
