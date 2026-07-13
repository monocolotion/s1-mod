#pragma once

struct ID3D11ShaderResourceView;

namespace font_cjk
{
	bool is_cjk_available();

	// Access CJK atlas and glyph data for external text rendering (D3D11 overlay)
	ID3D11ShaderResourceView* get_cjk_atlas_srv();
	int get_cjk_atlas_size(); // returns width (square atlas)
	const void* get_cjk_glyphs_data(); // returns pointer to glyph array
	int get_cjk_glyph_count();
	float get_cjk_font_size();

	// Look up a glyph for a specific codepoint, returns index or -1
	int find_glyph(unsigned short codepoint);
}
