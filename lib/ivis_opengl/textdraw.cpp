/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2020  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "lib/framework/frame.h"
#include "lib/framework/file.h"
#include <stdlib.h>
#include <string.h>
#include "lib/framework/string_ext.h"
#include "lib/framework/geometry.h"
#include "lib/ivis_opengl/ivisdef.h"
#include "lib/ivis_opengl/piestate.h"
#include "lib/ivis_opengl/pieclip.h"
#include "lib/ivis_opengl/pieblitfunc.h"
#include "lib/ivis_opengl/piepalette.h"
#include "lib/ivis_opengl/textdraw.h"
#include "lib/ivis_opengl/bitimage.h"
#include "src/multiplay.h"
#include <algorithm>
#include <numeric>
#include <array>
#include <physfs.h>

#define ASCII_SPACE			(32)
#define ASCII_NEWLINE			('@')
#define ASCII_COLOURMODE		('#')

// Contains the font color in the following order: red, green, blue, alpha
static float font_colour[4] = {1.f, 1.f, 1.f, 1.f};

#include "fribidi.h"
#include "hb.h"
#include "hb-ft.h"
#include "ft2build.h"
#include <unordered_map>
#include <memory>
#include <limits>

/* Defined in order to use the convenient functions of utfcpp.*/
#define UTF_CPP_CPLUSPLUS 201103L

#include "3rdparty/utfcpp/source/utf8.h"


#if defined(HB_VERSION_ATLEAST) && HB_VERSION_ATLEAST(1,0,5)
//	#define WZ_FT_LOAD_FLAGS (FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD) // Needs further testing on low-DPI displays
	#define WZ_FT_LOAD_FLAGS (FT_LOAD_NO_HINTING | FT_LOAD_TARGET_LCD)
#else
	// Without `hb_ft_font_set_load_flags` (which requires Harfbuzz 1.0.5+),
	// must default FreeType to the same flags that Harfbuzz internally uses
	// (by default hb loads fonts without hinting)
	#define WZ_FT_LOAD_FLAGS FT_LOAD_NO_HINTING
#endif
#define WZ_FT_RENDER_MODE FT_RENDER_MODE_LCD

float _horizScaleFactor = 1.0f;
float _vertScaleFactor = 1.0f;

/***************************************************************************
 *
 *	Internal classes
 *
 ***************************************************************************/

namespace HBFeature
{
	const hb_tag_t KernTag = HB_TAG('k', 'e', 'r', 'n'); // kerning operations
	const hb_tag_t LigaTag = HB_TAG('l', 'i', 'g', 'a'); // standard ligature substitution
	const hb_tag_t CligTag = HB_TAG('c', 'l', 'i', 'g'); // contextual ligature substitution

	static hb_feature_t LigatureOn = { LigaTag, 1, 0, std::numeric_limits<unsigned int>::max() };
	static hb_feature_t KerningOn = { KernTag, 1, 0, std::numeric_limits<unsigned int>::max() };
	static hb_feature_t CligOn = { CligTag, 1, 0, std::numeric_limits<unsigned int>::max() };
}

struct RasterizedGlyph
{
	std::unique_ptr<unsigned char[]> buffer;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	int32_t bearing_x;
	int32_t bearing_y;
};

struct GlyphMetrics
{
	uint32_t width;
	uint32_t height;
	int32_t bearing_x;
	int32_t bearing_y;
};

struct FTFace
{
	FTFace(FT_Library &lib, const std::string &fileName, int32_t charSize, uint32_t horizDPI, uint32_t vertDPI)
	{
		UDWORD pFileSize = 0;
		if (!loadFile(fileName.c_str(), &pFileData, &pFileSize))
		{
			debug(LOG_FATAL, "Unknown font file format for %s", fileName.c_str());
		}
		FT_Error error = FT_New_Memory_Face(lib, (const FT_Byte*)pFileData, pFileSize, 0, &m_face);
		if (error == FT_Err_Unknown_File_Format)
		{
			debug(LOG_FATAL, "Unknown font file format for %s", fileName.c_str());
		}
		else if (error != FT_Err_Ok)
		{
			debug(LOG_FATAL, "Font file %s not found, or other error", fileName.c_str());
		}
		error = FT_Set_Char_Size(m_face, 0, charSize, horizDPI, vertDPI);
		if (error != FT_Err_Ok)
		{
			debug(LOG_FATAL, "Could not set character size");
		}
		m_font = hb_ft_font_create(m_face, nullptr);
#if defined(HB_VERSION_ATLEAST) && HB_VERSION_ATLEAST(1,0,5)
		hb_ft_font_set_load_flags(m_font, WZ_FT_LOAD_FLAGS);
#endif
	}

	~FTFace()
	{
		hb_font_destroy(m_font);
		FT_Done_Face(m_face);
		if (pFileData != nullptr)
		{
			free(pFileData);
		}
	}

	uint32_t getGlyphWidth(uint32_t codePoint)
	{
		FT_Error error = FT_Load_Glyph(m_face,
			codePoint, // the glyph_index in the font file
			WZ_FT_LOAD_FLAGS
		);
		ASSERT(error == FT_Err_Ok, "Unable to load glyph for %u", codePoint);
		return m_face->glyph->metrics.width;
	}

	RasterizedGlyph get(uint32_t codePoint, Vector2i subpixeloffset64)
	{
		FT_Vector delta;
		delta.x = subpixeloffset64.x;
		delta.y = subpixeloffset64.y;
		FT_Set_Transform(m_face, nullptr, &delta);
		FT_Error error = FT_Load_Glyph(m_face,
			codePoint, // the glyph_index in the font file
			WZ_FT_LOAD_FLAGS
		);
		ASSERT(error == FT_Err_Ok, "Unable to load glyph %u", codePoint);

		FT_GlyphSlot slot = m_face->glyph;
		FT_Render_Glyph(m_face->glyph, WZ_FT_RENDER_MODE);
		FT_Bitmap ftBitmap = slot->bitmap;

		RasterizedGlyph g;
		g.buffer.reset(new unsigned char[ftBitmap.pitch * ftBitmap.rows]);
		if (ftBitmap.buffer != nullptr)
		{
			memcpy(g.buffer.get(), ftBitmap.buffer, ftBitmap.pitch * ftBitmap.rows);
		}
		else
		{
			ASSERT(ftBitmap.pitch == 0 || ftBitmap.rows == 0, "Glyph buffer missing (%d and %d)", ftBitmap.pitch, ftBitmap.rows);
		}
		g.width = ftBitmap.width / 3;
		g.height = ftBitmap.rows;
		g.bearing_x = slot->bitmap_left;
		g.bearing_y = slot->bitmap_top;
		g.pitch = ftBitmap.pitch;
		return g;
	}

	GlyphMetrics getGlyphMetrics(uint32_t codePoint, Vector2i subpixeloffset64)
	{
		FT_Vector delta;
		delta.x = subpixeloffset64.x;
		delta.y = subpixeloffset64.y;
		FT_Set_Transform(m_face, nullptr, &delta);
		FT_Error error = FT_Load_Glyph(m_face,
		                               codePoint, // the glyph_index in the font file
		                               WZ_FT_LOAD_FLAGS
		);
		if (error != FT_Err_Ok)
		{
			debug(LOG_FATAL, "unable to load glyph");
		}

		FT_GlyphSlot slot = m_face->glyph;
		return {
			static_cast<uint32_t>(slot->metrics.width),
			static_cast<uint32_t>(slot->metrics.height),
			slot->bitmap_left, slot->bitmap_top
		};
	}

	operator FT_Face()
	{
		return m_face;
	}

	FT_Face &face() { return m_face; }

	hb_font_t *m_font;
	char *pFileData = nullptr;

private:
	FT_Face m_face;
};

struct FTlib
{
	FTlib()
	{
		FT_Init_FreeType(&lib);
	}

	~FTlib()
	{
		FT_Done_FreeType(lib);
	}

	FT_Library lib;
};

struct TextRun
{
	std::string text;
	std::string language;

	int startOffset;
	int endOffset;
	hb_script_t script;
	hb_direction_t direction;
	hb_buffer_t* buffer;
	unsigned int glyphCount;
	hb_glyph_info_t* glyphInfos;
    hb_glyph_position_t* glyphPositions;

	uint32_t* codePoints;

	TextRun() : startOffset(0), endOffset(0) {}

	TextRun(const std::string &t, const std::string &l, hb_script_t s, hb_direction_t d) :
		text(t), language(l), script(s), direction(d) {}
};

struct TextLayoutMetrics
{
	TextLayoutMetrics(uint32_t _width, uint32_t _height) : width(_width), height(_height) { }
	TextLayoutMetrics() : width(0), height(0) { }
	uint32_t width;
	uint32_t height;
};

struct RenderedText
{
	RenderedText(std::unique_ptr<unsigned char[]> &&_data, uint32_t _width, uint32_t _height, int32_t _offset_x, int32_t _offset_y)
	: data(std::move(_data)), width(_width), height(_height), offset_x(_offset_x), offset_y(_offset_y)
	{ }

	RenderedText() : data(nullptr) , width(0) , height(0) , offset_x(0) , offset_y(0)
	{ }

	std::unique_ptr<unsigned char[]> data;
	uint32_t width;
	uint32_t height;
	int32_t offset_x;
	int32_t offset_y;
};

struct DrawTextResult
{
	DrawTextResult(RenderedText &&_text, TextLayoutMetrics _layoutMetrics) : text(std::move(_text)), layoutMetrics(_layoutMetrics)
	{ }

	DrawTextResult()
	{ }

	RenderedText text;
	TextLayoutMetrics layoutMetrics;
};

// Note:
// Technically glyph antialiasing is dependent of text rotation.
// Rotated text needs to set transform inside freetype2.
// However there is few rotated text in wz2100 and it's likely to make
// only minimal visual difference.
struct TextShaper
{
	hb_buffer_t* m_buffer;

	struct HarfbuzzPosition
	{
		hb_codepoint_t codepoint;
		Vector2i penPosition;

		HarfbuzzPosition(hb_codepoint_t c, Vector2i &&p) : codepoint(c), penPosition(p) {}
	};

	struct ShapingResult
	{
		std::vector<HarfbuzzPosition> glyphes;
		int32_t x_advance = 0;
		int32_t y_advance = 0;
	};

	TextShaper()
	{
		m_buffer = hb_buffer_create();
	}

	~TextShaper()
	{
		hb_buffer_destroy(m_buffer);
	}

	// Returns the text width and height *IN PIXELS*
	TextLayoutMetrics getTextMetrics(const std::string& text, FTFace &face)
	{
		const ShapingResult& shapingResult = shapeText(text, face);

		if (shapingResult.glyphes.empty())
		{
			return TextLayoutMetrics(shapingResult.x_advance / 64, shapingResult.y_advance / 64);
		}

		int32_t min_x;
		int32_t max_x;
		int32_t min_y;
		int32_t max_y;

		std::tie(min_x, max_x, min_y, max_y) = std::accumulate(shapingResult.glyphes.begin(), shapingResult.glyphes.end(), std::make_tuple(1000, -1000, 1000, -1000),
			[&face] (const std::tuple<int32_t, int32_t, int32_t, int32_t> &bounds, const HarfbuzzPosition &g) {
			RasterizedGlyph glyph = face.get(g.codepoint, g.penPosition % 64);
			int32_t x0 = g.penPosition.x / 64 + glyph.bearing_x;
			int32_t y0 = g.penPosition.y / 64 - glyph.bearing_y;
			return std::make_tuple(
				std::min(x0, std::get<0>(bounds)),
				std::max(static_cast<int32_t>(x0 + glyph.width), std::get<1>(bounds)),
				std::min(y0, std::get<2>(bounds)),
				std::max(static_cast<int32_t>(y0 + glyph.height), std::get<3>(bounds))
				);
			});

		const uint32_t texture_width = max_x - min_x + 1;
		const uint32_t texture_height = max_y - min_y + 1;
		const uint32_t x_advance = (shapingResult.x_advance / 64);
		const uint32_t y_advance = (shapingResult.y_advance / 64);

		// return the maximum of the x_advance / y_advance (converted from harfbuzz units) and the texture dimensions
		return TextLayoutMetrics(std::max(texture_width, x_advance), std::max(texture_height, y_advance));
	}

	FriBidiBracketType getBaseDirection()
	{
		std::string language = getLanguage();

		if (language == "ar_SA")
		{
			return HB_DIRECTION_RTL;
		}
		else
		{
			return HB_DIRECTION_LTR;
		}
	}

	// Draws the text and returns the text buffer, width and height, etc *IN PIXELS*
	DrawTextResult drawText(const std::string& text, FTFace &face)
	{
		ShapingResult shapingResult = shapeText(text, face);

		if (shapingResult.glyphes.empty())
		{
			return DrawTextResult(RenderedText(), TextLayoutMetrics(shapingResult.x_advance / 64, shapingResult.y_advance / 64));
		}

		int32_t min_x = 1000;
		int32_t max_x = -1000;
		int32_t min_y = 1000;
		int32_t max_y = -1000;

		// build glyphes
		struct glyphRaster
		{
			std::unique_ptr<unsigned char[]> buffer;
			Vector2i pixelPosition;
			Vector2i size;
			uint32_t pitch;

			glyphRaster(std::unique_ptr<unsigned char[]> &&b, Vector2i &&p, Vector2i &&s, uint32_t _pitch)
				: buffer(std::move(b)), pixelPosition(p), size(s), pitch(_pitch) {}
		};

		std::vector<glyphRaster> glyphs;
		std::transform(shapingResult.glyphes.begin(), shapingResult.glyphes.end(), std::back_inserter(glyphs),
			[&] (const HarfbuzzPosition &g) {
			RasterizedGlyph glyph = face.get(g.codepoint, g.penPosition % 64);
			int32_t x0 = g.penPosition.x / 64 + glyph.bearing_x;
			int32_t y0 = g.penPosition.y / 64 - glyph.bearing_y;
			min_x = std::min(x0, min_x);
			max_x = std::max(static_cast<int32_t>(x0 + glyph.width), max_x);
			min_y = std::min(y0, min_y);
			max_y = std::max(static_cast<int32_t>(y0 + glyph.height), max_y);
			return glyphRaster(std::move(glyph.buffer), Vector2i(x0, y0), Vector2i(glyph.width, glyph.height), glyph.pitch);
			});

		const uint32_t texture_width = max_x - min_x + 1;
		const uint32_t texture_height = max_y - min_y + 1;
		const uint32_t x_advance = (shapingResult.x_advance / 64);
		const uint32_t y_advance = (shapingResult.y_advance / 64);

		const size_t stringTextureSize = 4 * texture_width * texture_height;

		std::unique_ptr<unsigned char[]> stringTexture(new unsigned char[stringTextureSize]);
		memset(stringTexture.get(), 0, stringTextureSize);

		// TODO: Someone should document this piece.
		size_t glyphNum = 0;
		std::for_each(glyphs.begin(), glyphs.end(),
			[&](const glyphRaster &g)
			{
				const auto glyphBufferSize = g.pitch * g.size.y;
				for (int i = 0; i < g.size.y; ++i)
				{
					uint32_t i0 = g.pixelPosition.y - min_y;
					for (int j = 0; j < g.size.x; ++j)
					{
						uint32_t j0 = g.pixelPosition.x - min_x;
						const auto srcBufferPos = i * g.pitch + 3 * j;
						ASSERT(srcBufferPos + 2 < glyphBufferSize, "Invalid source (%" PRIu32" / %" PRIu32") reading glyph %zu for string \"%s\"; (%d, %d, %d, %d, %" PRIu32 ", %d, %d, %d, %" PRIu32 ", %" PRIu32 ")", srcBufferPos, glyphBufferSize, glyphNum, text.c_str(), i, g.size.y, g.pixelPosition.y, min_y, i0, j, g.pixelPosition.x, min_x, j0, g.pitch);
						uint8_t const *src = &g.buffer[srcBufferPos];
						const auto stringTexturePos = 4 * ((i0 + i) * texture_width + j + j0);
						ASSERT(stringTexturePos + 3 < stringTextureSize, "Invalid destination (%" PRIu32" / %zu) writing glyph %zu for string \"%s\"; (%d, %d, %d, %d, %" PRIu32 ", %d, %d, %d, %" PRIu32 ", %" PRIu32 ")", stringTexturePos, stringTextureSize, glyphNum, text.c_str(), i, g.size.y, g.pixelPosition.y, min_y, i0, j, g.pixelPosition.x, min_x, j0, texture_width);
						uint8_t *dst = &stringTexture[stringTexturePos];
						dst[0] = std::min(dst[0] + src[0], 255);
						dst[1] = std::min(dst[1] + src[1], 255);
						dst[2] = std::min(dst[2] + src[2], 255);
						dst[3] = std::min(dst[3] + ((src[0] * 77 + src[1] * 150 + src[2] * 29) >> 8), 255);
					}
				}
				++glyphNum;
			});

		return DrawTextResult(
				RenderedText(std::move(stringTexture), texture_width, texture_height, min_x, min_y),
				TextLayoutMetrics(std::max(texture_width, x_advance), std::max(texture_height, y_advance))
		);
	}

	ShapingResult shapeText(const std::string& text, FTFace &face)
	{
		/* Fribidi assumes that the text is encoded in UTF-32, so we have to
		   convert from UTF-8 to UTF-32, assuming that the string is indeed in UTF-8.*/
		std::u32string u32 = utf8::utf8to32(text);

		// Step 1: Initialize fribidi variables.
		// TODO: Don't forget to delete them at the end.

		hb_script_t* scripts;
		FriBidiCharType* types;
		FriBidiLevel* levels;
		FriBidiBracketType* bracketedTypes;
		uint32_t* codePoints;

		FriBidiParType baseDirection;
		size_t size;

		baseDirection = getBaseDirection();
		size = u32.length();

		codePoints = new uint32_t[size];
		memset(codePoints, 0, size * sizeof(*codePoints));
		memcpy(codePoints, u32.c_str(), size * sizeof(*codePoints));
		scripts = new hb_script_t[size];
		memset(scripts, 0, size * sizeof(*scripts));
		types = new FriBidiCharType[size];
		memset(types, 0, size * sizeof(*types));
		levels = new FriBidiLevel[size];
		memset(levels, 0, size * sizeof(*levels));
		bracketedTypes = new FriBidiBracketType[size];
		memset(bracketedTypes, 0, size * sizeof(*bracketedTypes));


		// Step 2: Run fribidi.

		/* Get the bidi type of each character in the string.*/
		fribidi_get_bidi_types(codePoints, size, types);
		fribidi_get_bracket_types(codePoints, size, types, bracketedTypes);

#if defined(FRIBIDI_MAJOR_VERSION) && FRIBIDI_MAJOR_VERSION >= 1
		FriBidiLevel maxLevel = fribidi_get_par_embedding_levels_ex(types, bracketedTypes, size, &baseDirection, levels);
		ASSERT(maxLevel != 0, "Error in fribidi_get_par_embedding_levels_ex!");
#else
		FriBidiLevel maxLevel = fribidi_get_par_embedding_levels(types, size, &baseDirection, levels);
		ASSERT(maxLevel != 0, "Error in fribidi_get_par_embedding_levels_ex!");
#endif
		
		/* Fill the array of scripts with scripts of each character */
		hb_unicode_funcs_t* funcs = hb_unicode_funcs_get_default();
		for (int i = 0; i < size; ++i)
			scripts[i] = hb_unicode_script(funcs, codePoints[i]);


		// Step 3: Resolve common or inherited scripts.

		hb_script_t lastScriptValue = HB_SCRIPT_UNKNOWN;
		int lastScriptIndex = -1;
		int lastSetIndex = -1;

		for (int i = 0; i < size; ++i)
		{
			if (scripts[i] == HB_SCRIPT_COMMON || scripts[i] == HB_SCRIPT_INHERITED)
			{
				if (lastScriptIndex != -1)
				{
					scripts[i] = lastScriptValue;
					lastSetIndex = i;
				}
			}
			else
			{
				for (int j = lastSetIndex + 1; j < i; ++j)
				{
					scripts[j] = scripts[i];
				}
				lastScriptValue = scripts[i];
				lastScriptIndex = i;
				lastSetIndex = i;
			}
		}


		// Step 4: Create the different runs

		std::vector<TextRun> textRuns;

		hb_script_t lastScript = scripts[0];
		int lastLevel = levels[0];
		int lastRunStart = 0; // where the last run started

		/* i == size means that we've reached the end of the string,
		   and that the last run should be created.*/
		for (int i = 0; i <= size; ++i)
		{
			/* If the script or level is of the current point is the same as the previous one,
			   then this means that the we have not reached the end of the current run.
			   If there's change, create a new run.*/
			if (i == size || (scripts[i] != lastScript) || (levels[i] != lastLevel))
			{
				TextRun run;
				run.startOffset = lastRunStart;
				run.endOffset = i;
				run.script = lastScript;
				run.codePoints = codePoints;

				/* "lastLevel & 1" yields either 1 or 0, depending on the least significant bit of lastLevel.*/
				run.direction = lastLevel & 1 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR;

				textRuns.push_back(run);

				if (i < size)
				{
					lastScript = scripts[i];
					lastLevel = levels[i];
					lastRunStart = i;
				}
				else
				{
					break;
				}
			}
		}


		// Step 6: Shape each run using harfbuzz.

		ShapingResult shapingResult;

		for (int i = 0; i < textRuns.size(); ++i)
		{
			shapeHarfbuzz(textRuns[i], face);
		}

		int32_t x = 0;
		int32_t y = 0;

		/* Theoretically, the direction of loop must change depending on the base direction
		   (the current direction assumes that the text is RTL). However, since English and
		   other European strings does not include Arabic or Hebrew words, this direction
		   will be all that is needed.*/
		for (size_t i = (textRuns.size()); i > 0; --i)
		{
			TextRun run = textRuns[i - 1];

			for (unsigned int glyphIndex = 0; glyphIndex < run.glyphCount; ++glyphIndex)
			{
				hb_glyph_position_t& current_glyphPos = run.glyphPositions[glyphIndex];
				
				shapingResult.glyphes.emplace_back(run.glyphInfos[glyphIndex].codepoint, Vector2i(x + current_glyphPos.x_offset, y + current_glyphPos.y_offset));

				x += run.glyphPositions[glyphIndex].x_advance;
				y += run.glyphPositions[glyphIndex].y_advance;
			}
		}
		shapingResult.x_advance += x;
		shapingResult.y_advance += y;


		// Step 7: Finalize.

		delete[] scripts;
		delete[] types;
		delete[] levels;
		delete[] bracketedTypes;
		delete[] codePoints;

		return shapingResult;
	}

	void shapeHarfbuzz(TextRun& run, FTFace& face)
	{
		run.buffer = hb_buffer_create();
		hb_buffer_set_direction(run.buffer, run.direction);
        hb_buffer_set_script(run.buffer, run.script);
        hb_buffer_add_utf32(run.buffer, run.codePoints + run.startOffset,
                            run.endOffset - run.startOffset, 0,
                            run.endOffset - run.startOffset);
		hb_buffer_set_flags(run.buffer, (hb_buffer_flags_t)(HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT));
		std::array<hb_feature_t, 3> features = { {HBFeature::KerningOn, HBFeature::LigatureOn, HBFeature::CligOn} };
        
		hb_shape(face.m_font, run.buffer, features.data(), static_cast<unsigned int>(features.size()));

		run.glyphInfos = hb_buffer_get_glyph_infos(run.buffer, &run.glyphCount);
		run.glyphPositions = hb_buffer_get_glyph_positions(run.buffer, &run.glyphCount);
	}
};

/***************************************************************************/
/*
 *	Main source
 */
/***************************************************************************/

void iV_font(const char *fontName, const char *fontFace, const char *fontFaceBold)
{
}

FTlib &getGlobalFTlib()
{
	static FTlib globalFT;
	return globalFT;
}

TextShaper &getShaper()
{
	static TextShaper shaper;
	return shaper;
}

inline float iV_GetHorizScaleFactor()
{
	return _horizScaleFactor;
}

inline float iV_GetVertScaleFactor()
{
	return _vertScaleFactor;
}

// The base DPI used internally.
// Do not change this, or various layout in the game interface & menus will break.
#define DEFAULT_DPI 72.0f

static FTFace *regular = nullptr;
static FTFace *regularBold = nullptr;
static FTFace *bold = nullptr;
static FTFace *medium = nullptr;
static FTFace *small = nullptr;
static FTFace *smallBold = nullptr;

struct iVFontsHash
{
    std::size_t operator()(iV_fonts FontID) const
    {
        return static_cast<std::size_t>(FontID);
    }
};
typedef std::unordered_map<iV_fonts, WzText, iVFontsHash> FontToEllipsisMapType;
static FontToEllipsisMapType fontToEllipsisMap;

static FTFace &getFTFace(iV_fonts FontID)
{
	switch (FontID)
	{
	default:
	case font_regular:
		return *regular;
	case font_regular_bold:
		return *regularBold;
	case font_large:
		return *bold;
	case font_medium:
		return *medium;
	case font_small:
		return *small;
	case font_bar:
		return *smallBold;
	}
}

static gfx_api::texture* textureID = nullptr;

void iV_TextInit(float horizScaleFactor, float vertScaleFactor)
{
	assert(horizScaleFactor >= 1.0f);
	assert(vertScaleFactor >= 1.0f);

	// Use the scaling factors to multiply the default DPI (72) to determine the desired internal font rendering DPI.
	_horizScaleFactor = horizScaleFactor;
	_vertScaleFactor = vertScaleFactor;
	uint32_t horizDPI = static_cast<uint32_t>(DEFAULT_DPI * horizScaleFactor);
	uint32_t vertDPI = static_cast<uint32_t>(DEFAULT_DPI * vertScaleFactor);
	debug(LOG_WZ, "Text-Rendering Scaling Factor: %f x %f; Internal Font DPI: %" PRIu32 " x %" PRIu32 "", _horizScaleFactor, _vertScaleFactor, horizDPI, vertDPI);

	regular = new FTFace(getGlobalFTlib().lib, "fonts/DejaVuSans.ttf", 12 * 64, horizDPI, vertDPI);
	regularBold = new FTFace(getGlobalFTlib().lib, "fonts/DejaVuSans-Bold.ttf", 12 * 64, horizDPI, vertDPI);
	bold = new FTFace(getGlobalFTlib().lib, "fonts/DejaVuSans-Bold.ttf", 21 * 64, horizDPI, vertDPI);
	medium = new FTFace(getGlobalFTlib().lib, "fonts/DejaVuSans.ttf", 16 * 64, horizDPI, vertDPI);
	small = new FTFace(getGlobalFTlib().lib, "fonts/DejaVuSans.ttf", 9 * 64, horizDPI, vertDPI);
	smallBold = new FTFace(getGlobalFTlib().lib, "fonts/DejaVuSans-Bold.ttf", 9 * 64, horizDPI, vertDPI);
}

void iV_TextShutdown()
{
	delete regular;
	delete medium;
	delete bold;
	delete small;
	delete smallBold;
	small = nullptr;
	regular = nullptr;
	medium = nullptr;
	bold = nullptr;
	small = nullptr;
	smallBold = nullptr;
	delete textureID;
	textureID = nullptr;
	fontToEllipsisMap.clear();
}

void iV_TextUpdateScaleFactor(float horizScaleFactor, float vertScaleFactor)
{
	iV_TextShutdown();
	iV_TextInit(horizScaleFactor, vertScaleFactor);
}

static WzText& iV_Internal_GetEllipsis(iV_fonts fontID)
{
	auto it = fontToEllipsisMap.find(fontID);
	if (it == fontToEllipsisMap.end())
	{
		// We must create + cache an ellipsis for this fontID
		it = fontToEllipsisMap.insert(FontToEllipsisMapType::value_type(fontID, WzText("\u2026", fontID))).first;
	}
	return it->second;
}

int iV_GetEllipsisWidth(iV_fonts fontID)
{
	return iV_Internal_GetEllipsis(fontID).width();
}

void iV_DrawEllipsis(iV_fonts fontID, Vector2f position, PIELIGHT colour)
{
	iV_Internal_GetEllipsis(fontID).render(position, colour);
}

unsigned int width_pixelsToPoints(unsigned int widthInPixels)
{
	return static_cast<int>(ceil((float)widthInPixels / _horizScaleFactor));
}
unsigned int height_pixelsToPoints(unsigned int heightInPixels)
{
	return static_cast<int>(ceil((float)heightInPixels / _vertScaleFactor));
}

// Returns the text width *in points*
unsigned int iV_GetTextWidth(const char* string, iV_fonts fontID)
{
	TextLayoutMetrics metrics = getShaper().getTextMetrics(string, getFTFace(fontID));
	return width_pixelsToPoints(metrics.width);
}

// Returns the counted text width *in points*
unsigned int iV_GetCountedTextWidth(const char *string, size_t string_length, iV_fonts fontID)
{
	return iV_GetTextWidth(string, fontID);
}

// Returns the text height *in points*
unsigned int iV_GetTextHeight(const char* string, iV_fonts fontID)
{
	TextLayoutMetrics metrics = getShaper().getTextMetrics(string, getFTFace(fontID));
	return height_pixelsToPoints(metrics.height);
}

// Returns the character width *in points*
unsigned int iV_GetCharWidth(uint32_t charCode, iV_fonts fontID)
{
	return width_pixelsToPoints(getFTFace(fontID).getGlyphWidth(charCode) >> 6);
}

int metricsHeight_PixelsToPoints(int heightMetric)
{
	float ptMetric = (float)heightMetric / _vertScaleFactor;
	return (ptMetric < 0) ? static_cast<int>(floor(ptMetric)) : static_cast<int>(ceil(ptMetric));
}

int iV_GetTextLineSize(iV_fonts fontID)
{
	FT_Face face = getFTFace(fontID);
	return metricsHeight_PixelsToPoints((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
}

int iV_GetTextAboveBase(iV_fonts fontID)
{
	FT_Face face = getFTFace(fontID);
	return metricsHeight_PixelsToPoints(-(face->size->metrics.ascender >> 6));
}

int iV_GetTextBelowBase(iV_fonts fontID)
{
	FT_Face face = getFTFace(fontID);
	return metricsHeight_PixelsToPoints(face->size->metrics.descender >> 6);
}

void iV_SetTextColour(PIELIGHT colour)
{
	font_colour[0] = colour.byte.r / 255.0f;
	font_colour[1] = colour.byte.g / 255.0f;
	font_colour[2] = colour.byte.b / 255.0f;
	font_colour[3] = colour.byte.a / 255.0f;
}

static bool breaksLine(char const c)
{
	return c == ASCII_NEWLINE || c == '\n';
}

static bool breaksWord(char const c)
{
	return c == ASCII_SPACE || breaksLine(c);
}

std::vector<TextLine> iV_FormatText(const char *String, UDWORD MaxWidth, UDWORD Justify, iV_fonts fontID, bool ignoreNewlines /*= false*/)
{
	std::vector<TextLine> lineDrawResults;

	std::string FString;
	std::string FWord;
	const int x = 0;
	const int y = 0;
	int i;
	int jx = x;		// Default to left justify.
	int jy = y;
	UDWORD WWidth;
	int TWidth;
	const char *curChar = String;

	while (*curChar != 0)
	{
		bool GotSpace = false;
		bool NewLine = false;

		// Reset text draw buffer
		FString.clear();

		WWidth = 0;

		auto indexWithinLine = 0;

		// Parse through the string, adding words until width is achieved.
		while (*curChar != 0 && (WWidth == 0 || WWidth < MaxWidth) && !NewLine)
		{
			const char *startOfWord = curChar;
			const unsigned int FStringWidth = iV_GetTextWidth(FString.c_str(), fontID);

			// Get the next word.
			i = 0;
			FWord.clear();
			for (
				;
				*curChar && ((indexWithinLine == 0 && !breaksLine(*curChar)) || !breaksWord(*curChar));
				++i, ++curChar, ++indexWithinLine
			)
			{
				if (*curChar == ASCII_COLOURMODE) // If it's a colour mode toggle char then just add it to the word.
				{
					FWord.push_back(*curChar);

					// this character won't be drawn so don't deal with its width
					continue;
				}

				FWord.push_back(*curChar);

				// Update this line's pixel width.
				//WWidth = FStringWidth + iV_GetCountedTextWidth(FWord.c_str(), i + 1, fontID);  // This triggers tonnes of valgrind warnings, if the string contains unicode. Adding lots of trailing garbage didn't help... Using iV_GetTextWidth with a null-terminated string, instead.
				WWidth = FStringWidth + iV_GetTextWidth(FWord.c_str(), fontID);

				// If this word doesn't fit on the current line then break out
				if (indexWithinLine != 0 && WWidth > MaxWidth)
				{
					FWord.erase(FWord.size() - 1);
					break;
				}
			}

			// Don't forget the space.
			if (*curChar == ASCII_SPACE)
			{
				FWord.push_back(' ');
				++i;
				++curChar;
				GotSpace = true;
				auto spaceWidth = iV_GetCharWidth(' ', fontID);
				if (WWidth + spaceWidth <= MaxWidth)
				{
					WWidth += spaceWidth;
				}
			}
			// Check for new line character.
			else if (breaksLine(*curChar))
			{
				if (!ignoreNewlines)
				{
					NewLine = true;
				}
				++curChar;
			}

			// If we've passed a space on this line and the word goes past the
			// maximum width and this isn't caused by the appended space then
			// rewind to the start of this word and finish this line.
			if (GotSpace
			    && i != 0
			    && WWidth > MaxWidth
			    && FWord[i - 1] != ' ')
			{
				// Skip back to the beginning of this
				// word and draw it on the next line
				curChar = startOfWord;
				break;
			}

			// And add it to the output string.
			FString.append(FWord);
		}


		// Remove trailing spaces, useful when doing center alignment.
		while (!FString.empty() && FString[FString.size() - 1] == ' ')
		{
			FString.erase(FString.size() - 1);  // std::string has no pop_back().
		}

		TWidth = iV_GetTextWidth(FString.c_str(), fontID);

		// Do justify.
		switch (Justify)
		{
		case FTEXT_CENTRE:
			jx = x + (MaxWidth - TWidth) / 2;
			break;

		case FTEXT_RIGHTJUSTIFY:
			jx = x + MaxWidth - TWidth;
			break;

		case FTEXT_LEFTJUSTIFY:
			jx = x;
			break;
		}

		// Store the line of text and its position in the bounding rect
		lineDrawResults.push_back({FString, Vector2i(TWidth, iV_GetTextLineSize(fontID)), Vector2i(jx, jy)});

		// and move down a line.
		jy += iV_GetTextLineSize(fontID);
	}

	return lineDrawResults;
}

// Needs modification
void iV_DrawTextRotated(const char* string, float XPos, float YPos, float rotation, iV_fonts fontID)
{
	ASSERT_OR_RETURN(, string, "Couldn't render string!");

	if (rotation != 0.f)
	{
		rotation = 180.f - rotation;
	}

	PIELIGHT color;
	color.vector[0] = static_cast<UBYTE>(font_colour[0] * 255.f);
	color.vector[1] = static_cast<UBYTE>(font_colour[1] * 255.f);
	color.vector[2] = static_cast<UBYTE>(font_colour[2] * 255.f);
	color.vector[3] = static_cast<UBYTE>(font_colour[3] * 255.f);

	TextRun tr(string, "en", HB_SCRIPT_COMMON, HB_DIRECTION_LTR);
	DrawTextResult drawResult = getShaper().drawText(string, getFTFace(fontID));

	if (drawResult.text.width > 0 && drawResult.text.height > 0)
	{
		if (textureID)
			delete textureID;
		textureID = gfx_api::context::get().create_texture(1, drawResult.text.width, drawResult.text.height, gfx_api::pixel_format::FORMAT_RGBA8_UNORM_PACK8);
		textureID->upload(0u, 0u, 0u, drawResult.text.width, drawResult.text.height, gfx_api::pixel_format::FORMAT_RGBA8_UNORM_PACK8, drawResult.text.data.get());
		iV_DrawImageText(*textureID, Vector2f(XPos, YPos), Vector2f((float)drawResult.text.offset_x / _horizScaleFactor, (float)drawResult.text.offset_y / _vertScaleFactor), Vector2f((float)drawResult.text.width / _horizScaleFactor, (float)drawResult.text.height / _vertScaleFactor), rotation, color);
	}
}

int WzText::width()
{
	updateCacheIfNecessary();
	return width_pixelsToPoints(layoutMetrics.x);
}
int WzText::height()
{
	updateCacheIfNecessary();
	return height_pixelsToPoints(layoutMetrics.y);
}
int WzText::aboveBase()
{
	updateCacheIfNecessary();
	return mPtsAboveBase;
}
int WzText::belowBase()
{
	updateCacheIfNecessary();
	return mPtsBelowBase;
}
int WzText::lineSize()
{
	updateCacheIfNecessary();
	return mPtsLineSize;
}

void WzText::setText(const std::string &string, iV_fonts fontID/*, bool delayRender*/)
{
	if (mText == string && fontID == mFontID)
	{
		return; // cached
	}
	drawAndCacheText(string, fontID);
}

void WzText::drawAndCacheText(const std::string& string, iV_fonts fontID)
{
	mFontID = fontID;
	mText = string;
	mRenderingHorizScaleFactor = iV_GetHorizScaleFactor();
	mRenderingVertScaleFactor = iV_GetVertScaleFactor();

	FTFace &face = getFTFace(fontID);
	FT_Face &type = face.face();

	mPtsAboveBase = metricsHeight_PixelsToPoints(-(type->size->metrics.ascender >> 6));
	mPtsLineSize = metricsHeight_PixelsToPoints((type->size->metrics.ascender - type->size->metrics.descender) >> 6);
	mPtsBelowBase = metricsHeight_PixelsToPoints(type->size->metrics.descender >> 6);

	DrawTextResult drawResult = getShaper().drawText(string, face);
	dimensions = Vector2i(drawResult.text.width, drawResult.text.height);
	offsets = Vector2i(drawResult.text.offset_x, drawResult.text.offset_y);
	layoutMetrics = Vector2i(drawResult.layoutMetrics.width, drawResult.layoutMetrics.height);

	if (texture)
	{
		delete texture;
		texture = nullptr;
	}

	if (dimensions.x > 0 && dimensions.y > 0)
	{
		texture = gfx_api::context::get().create_texture(1, dimensions.x, dimensions.y, gfx_api::pixel_format::FORMAT_RGBA8_UNORM_PACK8);
		texture->upload(0u, 0u, 0u, dimensions.x , dimensions.y, gfx_api::pixel_format::FORMAT_RGBA8_UNORM_PACK8, drawResult.text.data.get());
	}
}

void WzText::redrawAndCacheText()
{
	drawAndCacheText(mText, mFontID);
}

WzText::WzText(const std::string &string, iV_fonts fontID)
{
	setText(string, fontID);
}

WzText::~WzText()
{
	if (texture)
		delete texture;
}

WzText& WzText::operator=(WzText&& other)
{
	if (this != &other)
	{
		// Free the existing texture, if any.
		if (texture)
		{
			delete texture;
		}

		// Get the other data
		texture = other.texture;
		mFontID = other.mFontID;
		mText = std::move(other.mText);
		mPtsAboveBase = other.mPtsAboveBase;
		mPtsBelowBase = other.mPtsBelowBase;
		mPtsLineSize = other.mPtsLineSize;
		offsets = other.offsets;
		dimensions = other.dimensions;
		mRenderingHorizScaleFactor = other.mRenderingHorizScaleFactor;
		mRenderingVertScaleFactor = other.mRenderingVertScaleFactor;
		layoutMetrics = other.layoutMetrics;

		// Reset other's texture
		other.texture = nullptr;
	}
	return *this;
}

WzText::WzText(WzText&& other)
{
	*this = std::move(other);
}

inline void WzText::updateCacheIfNecessary()
{
	if (mText.empty())
	{
		return; // string is empty (or hasn't yet been set), thus changes have no effect
	}
	if (mRenderingHorizScaleFactor != iV_GetHorizScaleFactor() || mRenderingVertScaleFactor != iV_GetVertScaleFactor())
	{
		// The text rendering subsystem's scale factor has changed, so the rendered (cached) text must be re-rendered.
		redrawAndCacheText();
		// debug(LOG_WZ, "Redrawing / re-calculating WzText text - scale factor has changed.");
	}
}

void WzText::render(Vector2f position, PIELIGHT colour, float rotation, int maxWidth, int maxHeight)
{
	updateCacheIfNecessary();

	if (texture == nullptr)
	{
		// A texture will not always be created. (For example, if the rendered text is empty.)
		// No need to render if there's nothing to render.
		return;
	}

	if (rotation != 0.f)
	{
		rotation = 180.f - rotation;
	}

	if (maxWidth <= 0 && maxHeight <= 0)
	{
		iV_DrawImageText(*texture, position, Vector2f(offsets.x / mRenderingHorizScaleFactor, offsets.y / mRenderingVertScaleFactor), Vector2f(dimensions.x / mRenderingHorizScaleFactor, dimensions.y / mRenderingVertScaleFactor), rotation, colour);
	}
	else
	{
		WzRect clippingRectInPixels;
		clippingRectInPixels.setWidth((maxWidth > 0) ? static_cast<int>((float)maxWidth * mRenderingHorizScaleFactor) : dimensions.x);
		clippingRectInPixels.setHeight((maxHeight > 0) ? static_cast<int>((float)maxHeight * mRenderingVertScaleFactor) : dimensions.y);
		iV_DrawImageTextClipped(*texture, dimensions, position, Vector2f(offsets.x / mRenderingHorizScaleFactor, offsets.y / mRenderingVertScaleFactor), Vector2f((maxWidth > 0) ? maxWidth : dimensions.x / mRenderingHorizScaleFactor, (maxHeight > 0) ? maxHeight : dimensions.y / mRenderingVertScaleFactor), rotation, colour, clippingRectInPixels);
	}
}

void WzText::renderOutlined(int x, int y, PIELIGHT colour, PIELIGHT outlineColour)
{
	for (auto i = -1; i <= 1; i++)
	{
		for (auto j = -1; j <= 1; j++)
		{
			render(x + i, y + j, outlineColour);
		}
	}
	render(x, y, colour);
}

// Sets the text, truncating to a desired width limit (in *points*) if needed
// returns: the length of the string that will be drawn (may be less than the input text.length() if truncated)
size_t WidthLimitedWzText::setTruncatableText(const std::string &text, iV_fonts fontID, size_t limitWidthInPoints)
{
	if ((mFullText == text) && (mLimitWidthPts == limitWidthInPoints) && (getFontID() == fontID))
	{
		return getText().length(); // skip; no change
	}

	mFullText = text;
	mLimitWidthPts = limitWidthInPoints;

	std::string truncatedText = text;
	while ((truncatedText.length() > 0) && (iV_GetTextWidth(truncatedText.c_str(), fontID) > limitWidthInPoints))
	{
		truncatedText.pop_back();
	}

	WzText::setText(truncatedText, fontID);
	return truncatedText.length();
}
