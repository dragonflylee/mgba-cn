/* Copyright (c) 2013-2018 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/font-metrics.h>
#include <mgba-util/image/png-io.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <GLES3/gl3.h>

#include <switch.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define GLYPH_HEIGHT 24
#define CELL_HEIGHT 32
#define CELL_WIDTH 32
#define MAX_GLYPHS 1024

#define CJK_ATLAS_SIZE 512
#define CJK_FONT_SIZE 24
/* 每个字形占用固定的方形槽位, 便于按槽回收/复用图集空间 */
#define CJK_SLOT_SIZE 26
#define CJK_GLYPH_PADDING 2
#define CJK_CELL_SIZE (CJK_SLOT_SIZE + CJK_GLYPH_PADDING)
#define CJK_ATLAS_GRID (CJK_ATLAS_SIZE / CJK_CELL_SIZE)
#define CJK_MAX_CACHE (CJK_ATLAS_GRID * CJK_ATLAS_GRID)

enum CjkGlyphState {
	CJK_GLYPH_BLANK, /* 无墨迹(如空格), 跳过绘制 */
	CJK_GLYPH_METRICS, /* 度量已知, 位图尚未栅格化 */
	CJK_GLYPH_READY, /* 位图已驻留图集槽位 */
	CJK_GLYPH_FAILED, /* 无法栅格化, 回退为 '?' */
};

struct CjkGlyphEntry {
	uint32_t codepoint;
	int slot; /* 图集槽位号, -1 表示未占用 */
	int atlasX, atlasY;
	int width, height;
	int advanceX;
	int bearingX, bearingY;
	uint32_t lastUsed; /* LRU 时间戳 */
	enum CjkGlyphState state;
	struct CjkGlyphEntry* next;
};

static const GLfloat _offsets[] = {
	0.f, 0.f,
	1.f, 0.f,
	1.f, 1.f,
	0.f, 1.f,
};

static const GLchar* const _gles3Header =
	"#version 300 es\n"
	"precision mediump float;\n";

static const char* const _vertexShader =
	"in vec2 offset;\n"
	"in vec3 origin;\n"
	"in vec2 glyph;\n"
	"in vec2 dims;\n"
	"in mat2 transform;\n"
	"in vec4 color;\n"
	"out vec4 fragColor;\n"
	"out vec2 texCoord;\n"

	"void main() {\n"
	"	texCoord = (glyph + offset * dims) / 512.0;\n"
	"	vec2 scaledOffset = (transform * (offset * 2.0 - vec2(1.0)) + vec2(1.0)) / 2.0 * dims;\n"
	"	fragColor = color;\n"
	"	gl_Position = vec4((origin.x + scaledOffset.x) / 640.0 - 1.0, -(origin.y + scaledOffset.y) / 360.0 + 1.0, origin.z, 1.0);\n"
	"}";

static const char* const _fragmentShader =
	"in vec2 texCoord;\n"
	"in vec4 fragColor;\n"
	"out vec4 outColor;\n"
	"uniform sampler2D tex;\n"
	"uniform float cutoff;\n"
	"uniform vec3 colorModulus;\n"

	"void main() {\n"
	"	vec4 texColor = texture2D(tex, texCoord);\n"
	"	texColor.a = clamp((texColor.a - cutoff) / (1.0 - cutoff), 0.0, 1.0);\n"
	"	texColor.rgb = fragColor.rgb * colorModulus;\n"
	"	texColor.a *= fragColor.a;\n"
	"	outColor = texColor;\n"
	"}";

struct GUIFont {
	GLuint font;
	int currentGlyph;
	GLuint program;
	GLuint vbo;
	GLuint vao;
	GLuint texLocation;
	GLuint cutoffLocation;
	GLuint colorModulusLocation;

	GLuint originLocation;
	GLuint glyphLocation;
	GLuint dimsLocation;
	GLuint transformLocation[2];
	GLuint colorLocation;

	GLuint originVbo;
	GLuint glyphVbo;
	GLuint dimsVbo;
	GLuint transformVbo[2];
	GLuint colorVbo;

	GLfloat originData[MAX_GLYPHS][3];
	GLfloat glyphData[MAX_GLYPHS][2];
	GLfloat dimsData[MAX_GLYPHS][2];
	GLfloat transformData[2][MAX_GLYPHS][2];
	GLfloat colorData[MAX_GLYPHS][4];

	/* CJK font rendering */
	FT_Library ftLibrary;
	FT_Face cjkFace;
	GLuint cjkTexture;
	struct CjkGlyphEntry* cjkCache[CJK_MAX_CACHE];
	struct CjkGlyphEntry* cjkSlots[CJK_MAX_CACHE];
	int cjkFreeSlots[CJK_MAX_CACHE];
	int cjkFreeCount;
	uint32_t cjkClock;
	int cjkGlyphCount;
	int cjkCurrentGlyph;

	GLfloat cjkOriginData[MAX_GLYPHS][3];
	GLfloat cjkGlyphData[MAX_GLYPHS][2];
	GLfloat cjkDimsData[MAX_GLYPHS][2];
	GLfloat cjkTransformData[2][MAX_GLYPHS][2];
	GLfloat cjkColorData[MAX_GLYPHS][4];
};

/* 定义见下方, GUIFontGlyphWidth 需要按需装载字形以取得真实推进宽度 */
static struct CjkGlyphEntry* _cjkCacheGet(struct GUIFont* font, uint32_t codepoint, bool render);

static bool _loadTexture(const char* path) {
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return false;
	}
	png_structp png = PNGReadOpen(vf, 0);
	png_infop info = png_create_info_struct(png);
	png_infop end = png_create_info_struct(png);
	bool success = false;
	if (png && info && end) {
		success = PNGReadHeader(png, info);
	}
	void* pixels = NULL;
	if (success) {
		unsigned height = png_get_image_height(png, info);
		unsigned width = png_get_image_width(png, info);
		pixels = malloc(width * height);
		if (pixels) {
			success = PNGReadPixels8(png, info, pixels, width, height, width);
			success = success && PNGReadFooter(png, end);
		} else {
			success = false;
		}
		if (success) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, pixels);
		}
	}
	PNGReadClose(png, info, end);
	if (pixels) {
		free(pixels);
	}
	vf->close(vf);
	return success;
}

struct GUIFont* GUIFontCreate(void) {
	struct GUIFont* font = malloc(sizeof(struct GUIFont));
	if (!font) {
		return NULL;
	}
	glGenTextures(1, &font->font);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, font->font);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	if (!_loadTexture("romfs:/font-new.png")) {
		GUIFontDestroy(font);
		return NULL;
	}

	font->currentGlyph = 0;
	font->program = glCreateProgram();
	GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	const GLchar* shaderBuffer[2];

	shaderBuffer[0] = _gles3Header;

	shaderBuffer[1] = _vertexShader;
	glShaderSource(vertexShader, 2, shaderBuffer, NULL);

	shaderBuffer[1] = _fragmentShader;
	glShaderSource(fragmentShader, 2, shaderBuffer, NULL);

	glAttachShader(font->program, vertexShader);
	glAttachShader(font->program, fragmentShader);

	glCompileShader(fragmentShader);

	GLint success;
	glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetShaderInfoLog(fragmentShader, sizeof(msg), NULL, msg);
		puts(msg);
	}

	glCompileShader(vertexShader);

	glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetShaderInfoLog(vertexShader, sizeof(msg), NULL, msg);
		puts(msg);
	}

	glLinkProgram(font->program);

	glGetProgramiv(font->program, GL_LINK_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetProgramInfoLog(font->program, sizeof(msg), NULL, msg);
		puts(msg);
	}

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	font->texLocation = glGetUniformLocation(font->program, "tex");
	font->cutoffLocation = glGetUniformLocation(font->program, "cutoff");
	font->colorModulusLocation = glGetUniformLocation(font->program, "colorModulus");

	font->originLocation = glGetAttribLocation(font->program, "origin");
	font->glyphLocation = glGetAttribLocation(font->program, "glyph");
	font->dimsLocation = glGetAttribLocation(font->program, "dims");
	font->transformLocation[0] = glGetAttribLocation(font->program, "transform");
	font->transformLocation[1] = font->transformLocation[0] + 1;
	font->colorLocation = glGetAttribLocation(font->program, "color");

	GLuint offsetLocation = glGetAttribLocation(font->program, "offset");

	glGenVertexArrays(1, &font->vao);
	glBindVertexArray(font->vao);

	glGenBuffers(1, &font->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(_offsets), _offsets, GL_STATIC_DRAW);
	glVertexAttribPointer(offsetLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(offsetLocation, 0);
	glEnableVertexAttribArray(offsetLocation);

	glGenBuffers(1, &font->originVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->originVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 3 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->originLocation, 3, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->originLocation, 1);
	glEnableVertexAttribArray(font->originLocation);

	glGenBuffers(1, &font->glyphVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->glyphVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->glyphLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->glyphLocation, 1);
	glEnableVertexAttribArray(font->glyphLocation);

	glGenBuffers(1, &font->dimsVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->dimsVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->dimsLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->dimsLocation, 1);
	glEnableVertexAttribArray(font->dimsLocation);

	glGenBuffers(2, font->transformVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->transformLocation[0], 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->transformLocation[0], 1);
	glEnableVertexAttribArray(font->transformLocation[0]);
	glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->transformLocation[1], 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->transformLocation[1], 1);
	glEnableVertexAttribArray(font->transformLocation[1]);

	glGenBuffers(1, &font->colorVbo);
	glBindBuffer(GL_ARRAY_BUFFER, font->colorVbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 4 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
	glVertexAttribPointer(font->colorLocation, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glVertexAttribDivisor(font->colorLocation, 1);
	glEnableVertexAttribArray(font->colorLocation);

	glBindVertexArray(0);

	/* Initialize CJK font rendering */
	font->ftLibrary = NULL;
	font->cjkFace = NULL;
	font->cjkTexture = 0;
	font->cjkClock = 0;
	font->cjkGlyphCount = 0;
	font->cjkCurrentGlyph = 0;
	memset(font->cjkCache, 0, sizeof(font->cjkCache));
	memset(font->cjkSlots, 0, sizeof(font->cjkSlots));
	font->cjkFreeCount = CJK_MAX_CACHE;
	for (int i = 0; i < CJK_MAX_CACHE; ++i) {
		font->cjkFreeSlots[i] = i;
	}

	if (!FT_Init_FreeType(&font->ftLibrary)) {
		PlFontData fd;
		if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_ChineseSimplified))) {
			if (!FT_New_Memory_Face(font->ftLibrary, (const FT_Byte*) fd.address, fd.size, 0, &font->cjkFace)) {
				FT_Set_Pixel_Sizes(font->cjkFace, 0, CJK_FONT_SIZE);

				glGenTextures(1, &font->cjkTexture);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, font->cjkTexture);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

				/* Initialize atlas with transparent pixels */
				size_t atlasSize = CJK_ATLAS_SIZE * CJK_ATLAS_SIZE;
				uint8_t* clearData = calloc(1, atlasSize);
				if (clearData) {
					glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, CJK_ATLAS_SIZE, CJK_ATLAS_SIZE, 0, GL_ALPHA, GL_UNSIGNED_BYTE, clearData);
					free(clearData);
				}
				glActiveTexture(GL_TEXTURE0);
			}
		}
	}

	return font;
}

void GUIFontDestroy(struct GUIFont* font) {
	glDeleteBuffers(1, &font->vbo);
	glDeleteBuffers(1, &font->originVbo);
	glDeleteBuffers(1, &font->glyphVbo);
	glDeleteBuffers(1, &font->dimsVbo);
	glDeleteBuffers(2, font->transformVbo);
	glDeleteBuffers(1, &font->colorVbo);
	glDeleteProgram(font->program);
	glDeleteTextures(1, &font->font);
	glDeleteVertexArrays(1, &font->vao);

	/* Clean up CJK resources */
	if (font->cjkTexture) {
		glDeleteTextures(1, &font->cjkTexture);
	}
	if (font->cjkFace) {
		FT_Done_Face(font->cjkFace);
	}
	if (font->ftLibrary) {
		FT_Done_FreeType(font->ftLibrary);
	}

	/* Free CJK glyph cache entries */
	for (int i = 0; i < CJK_MAX_CACHE; i++) {
		struct CjkGlyphEntry* entry = font->cjkCache[i];
		while (entry) {
			struct CjkGlyphEntry* next = entry->next;
			free(entry);
			entry = next;
		}
	}

	free(font);
}

unsigned GUIFontHeight(const struct GUIFont* font) {
	UNUSED(font);
	return GLYPH_HEIGHT;
}

unsigned GUIFontGlyphWidth(const struct GUIFont* font, uint32_t glyph) {
	if (glyph > 0x7F) {
		/* 未缓存时按需装载度量, 保证宽度与实际绘制的推进量一致 */
		struct CjkGlyphEntry* entry = _cjkCacheGet((struct GUIFont*) font, glyph, false);
		if (entry && entry->state != CJK_GLYPH_FAILED) {
			return entry->advanceX;
		}
		return defaultFontMetrics['?'].width * 2;
	}
	return defaultFontMetrics[glyph].width * 2;
}

void GUIFontIconMetrics(const struct GUIFont* font, enum GUIIcon icon, unsigned* w, unsigned* h) {
	UNUSED(font);
	if (icon >= GUI_ICON_MAX) {
		if (w) {
			*w = 0;
		}
		if (h) {
			*h = 0;
		}
	} else {
		if (w) {
			*w = defaultIconMetrics[icon].width * 2;
		}
		if (h) {
			*h = defaultIconMetrics[icon].height * 2;
		}
	}
}

static struct CjkGlyphEntry* _cjkCacheLookup(struct GUIFont* font, uint32_t codepoint) {
	int slotIdx = codepoint % CJK_MAX_CACHE;
	struct CjkGlyphEntry* entry = font->cjkCache[slotIdx];
	while (entry) {
		if (entry->codepoint == codepoint) {
			return entry;
		}
		entry = entry->next;
	}
	return NULL;
}

static void _cjkCacheUnlink(struct GUIFont* font, struct CjkGlyphEntry* entry) {
	struct CjkGlyphEntry** link = &font->cjkCache[entry->codepoint % CJK_MAX_CACHE];
	while (*link) {
		if (*link == entry) {
			*link = entry->next;
			break;
		}
		link = &(*link)->next;
	}
}

/* 归还条目占用的图集槽位供后续复用 */
static void _cjkCacheReleaseSlot(struct GUIFont* font, struct CjkGlyphEntry* entry) {
	if (entry->slot < 0) {
		return;
	}
	font->cjkSlots[entry->slot] = NULL;
	font->cjkFreeSlots[font->cjkFreeCount++] = entry->slot;
	entry->slot = -1;
}

static void _cjkCacheFree(struct GUIFont* font, struct CjkGlyphEntry* entry) {
	_cjkCacheUnlink(font, entry);
	_cjkCacheReleaseSlot(font, entry);
	--font->cjkGlyphCount;
	free(entry);
}

/* 图集内容即将被覆写; 先提交挂起的批次, 否则已排队的图集坐标会指向新内容 */
static void _cjkFlushPending(struct GUIFont* font) {
	if (font->cjkCurrentGlyph > 0) {
		GUIFontDrawSubmit(font);
	}
}

/* 淘汰最久未使用的条目并回收其槽位 */
static void _cjkCacheEvictOldest(struct GUIFont* font, const struct CjkGlyphEntry* exclude) {
	uint32_t oldest = UINT32_MAX;
	struct CjkGlyphEntry* victim = NULL;
	for (int i = 0; i < CJK_MAX_CACHE; ++i) {
		struct CjkGlyphEntry* entry = font->cjkCache[i];
		while (entry) {
			if (entry != exclude && entry->lastUsed < oldest) {
				oldest = entry->lastUsed;
				victim = entry;
			}
			entry = entry->next;
		}
	}
	if (victim) {
		_cjkCacheFree(font, victim);
	}
}

static struct CjkGlyphEntry* _cjkCacheAlloc(struct GUIFont* font, uint32_t codepoint) {
	struct CjkGlyphEntry* entry = calloc(1, sizeof(struct CjkGlyphEntry));
	if (!entry) {
		return NULL;
	}
	entry->codepoint = codepoint;
	entry->slot = -1;
	entry->state = CJK_GLYPH_METRICS;
	entry->lastUsed = ++font->cjkClock;

	int slotIdx = codepoint % CJK_MAX_CACHE;
	entry->next = font->cjkCache[slotIdx];
	font->cjkCache[slotIdx] = entry;
	++font->cjkGlyphCount;

	/* 仅度量未栅格化的条目同样占用内存, 限制总量避免无界增长 */
	if (font->cjkGlyphCount > CJK_MAX_CACHE) {
		_cjkCacheEvictOldest(font, entry);
	}
	return entry;
}

/* 申请一个图集槽位, 必要时按 LRU 淘汰 */
static bool _cjkCacheAcquireSlot(struct GUIFont* font, struct CjkGlyphEntry* entry) {
	while (font->cjkFreeCount <= 0) {
		int before = font->cjkGlyphCount;
		_cjkCacheEvictOldest(font, entry);
		if (font->cjkGlyphCount == before) {
			return false;
		}
	}
	/* 槽位可能被旧字形占用, 覆写前提交挂起批次 */
	_cjkFlushPending(font);
	int slot = font->cjkFreeSlots[--font->cjkFreeCount];
	font->cjkSlots[slot] = entry;
	entry->slot = slot;
	entry->atlasX = (slot % CJK_ATLAS_GRID) * CJK_CELL_SIZE + CJK_GLYPH_PADDING;
	entry->atlasY = (slot / CJK_ATLAS_GRID) * CJK_CELL_SIZE + CJK_GLYPH_PADDING;
	return true;
}

static bool _cjkLoadGlyph(struct GUIFont* font, uint32_t codepoint) {
	FT_UInt glyphIndex = FT_Get_Char_Index(font->cjkFace, codepoint);
	if (!glyphIndex && codepoint != 0) {
		return false;
	}
	return !FT_Load_Glyph(font->cjkFace, glyphIndex, FT_LOAD_DEFAULT);
}

/* 只取度量: 无需栅格化, 用于 GUIFontGlyphWidth 的按需查询 */
static bool _cjkCacheLoadMetrics(struct GUIFont* font, struct CjkGlyphEntry* entry, uint32_t codepoint) {
	if (!_cjkLoadGlyph(font, codepoint)) {
		return false;
	}
	FT_GlyphSlot slot = font->cjkFace->glyph;
	entry->advanceX = slot->advance.x >> 6;
	entry->bearingX = slot->metrics.horiBearingX >> 6;
	entry->bearingY = slot->metrics.horiBearingY >> 6;
	entry->width = (slot->metrics.width + 63) >> 6;
	entry->height = (slot->metrics.height + 63) >> 6;
	return true;
}

static bool _cjkCacheRender(struct GUIFont* font, struct CjkGlyphEntry* entry, uint32_t codepoint) {
	if (!_cjkLoadGlyph(font, codepoint)) {
		return false;
	}
	FT_GlyphSlot slot = font->cjkFace->glyph;
	if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) {
		return false;
	}
	FT_Bitmap* bitmap = &slot->bitmap;
	if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY) {
		return false;
	}

	int gw = bitmap->width;
	int gh = bitmap->rows;
	if (gw <= 0 || gh <= 0) {
		/* 空白字形, 仅保留度量 */
		entry->state = CJK_GLYPH_BLANK;
		return true;
	}
	/* 槽位固定尺寸, 超出部分截断以免覆写相邻字形 */
	if (gw > CJK_SLOT_SIZE) {
		gw = CJK_SLOT_SIZE;
	}
	if (gh > CJK_SLOT_SIZE) {
		gh = CJK_SLOT_SIZE;
	}

	entry->width = gw;
	entry->height = gh;
	entry->bearingX = slot->bitmap_left;
	entry->bearingY = slot->bitmap_top;

	if (!_cjkCacheAcquireSlot(font, entry)) {
		return false;
	}

	uint8_t* buf = malloc(gw * gh);
	if (!buf) {
		_cjkCacheReleaseSlot(font, entry);
		return false;
	}
	for (int row = 0; row < gh; ++row) {
		memcpy(buf + row * gw, bitmap->buffer + row * bitmap->pitch, gw);
	}

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, font->cjkTexture);
	GLint oldUnpackAlignment;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, entry->atlasX, entry->atlasY, gw, gh, GL_ALPHA, GL_UNSIGNED_BYTE, buf);
	glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpackAlignment);
	glActiveTexture(GL_TEXTURE0);
	free(buf);

	entry->state = CJK_GLYPH_READY;
	return true;
}

static struct CjkGlyphEntry* _cjkCacheGet(struct GUIFont* font, uint32_t codepoint, bool render) {
	if (!font->cjkFace) {
		return NULL;
	}

	struct CjkGlyphEntry* entry = _cjkCacheLookup(font, codepoint);
	if (entry) {
		entry->lastUsed = ++font->cjkClock;
		if (render && entry->state == CJK_GLYPH_METRICS) {
			if (!_cjkCacheRender(font, entry, codepoint)) {
				entry->state = CJK_GLYPH_FAILED;
			}
		}
		return entry;
	}

	entry = _cjkCacheAlloc(font, codepoint);
	if (!entry) {
		return NULL;
	}
	if (!_cjkCacheLoadMetrics(font, entry, codepoint)) {
		/* 字体中不存在该码点: 缓存失败结果, 避免每帧重复查找 */
		entry->state = CJK_GLYPH_FAILED;
		return entry;
	}
	if (entry->width <= 0 || entry->height <= 0) {
		entry->state = CJK_GLYPH_BLANK;
		return entry;
	}
	if (render && !_cjkCacheRender(font, entry, codepoint)) {
		entry->state = CJK_GLYPH_FAILED;
	}
	return entry;
}

void GUIFontDrawGlyph(struct GUIFont* font, int x, int y, uint32_t color, uint32_t glyph) {
	if (glyph > 0x7F) {
		/* CJK character rendering */
		struct CjkGlyphEntry* entry = _cjkCacheGet(font, glyph, true);
		if (!entry || entry->state == CJK_GLYPH_FAILED) {
			/* 字体缺少该码点或无法栅格化, 回退为 '?' */
			glyph = '?';
			goto ascii;
		}
		if (entry->state != CJK_GLYPH_READY || entry->width <= 0) {
			/* 空白字形, 位置推进由 GUIFontGlyphWidth 负责 */
			return;
		}

		if (font->cjkCurrentGlyph >= MAX_GLYPHS) {
			GUIFontDrawSubmit(font);
		}

		int cjkOffset = font->cjkCurrentGlyph;

		font->cjkOriginData[cjkOffset][0] = x + entry->bearingX;
		font->cjkOriginData[cjkOffset][1] = y - entry->bearingY;
		font->cjkOriginData[cjkOffset][2] = 0;
		font->cjkGlyphData[cjkOffset][0] = entry->atlasX;
		font->cjkGlyphData[cjkOffset][1] = entry->atlasY;
		font->cjkDimsData[cjkOffset][0] = entry->width;
		font->cjkDimsData[cjkOffset][1] = entry->height;
		font->cjkTransformData[0][cjkOffset][0] = 1.0f;
		font->cjkTransformData[0][cjkOffset][1] = 0.0f;
		font->cjkTransformData[1][cjkOffset][0] = 0.0f;
		font->cjkTransformData[1][cjkOffset][1] = 1.0f;
		font->cjkColorData[cjkOffset][0] = (color & 0xFF) / 255.0f;
		font->cjkColorData[cjkOffset][1] = ((color >> 8) & 0xFF) / 255.0f;
		font->cjkColorData[cjkOffset][2] = ((color >> 16) & 0xFF) / 255.0f;
		font->cjkColorData[cjkOffset][3] = ((color >> 24) & 0xFF) / 255.0f;

		++font->cjkCurrentGlyph;
		return;
	}

ascii:
	{
		struct GUIFontGlyphMetric metric = defaultFontMetrics[glyph];

		if (font->currentGlyph >= MAX_GLYPHS) {
			GUIFontDrawSubmit(font);
		}

		int offset = font->currentGlyph;

		font->originData[offset][0] = x;
		font->originData[offset][1] = y - GLYPH_HEIGHT + metric.padding.top * 2;
		font->originData[offset][2] = 0;
		font->glyphData[offset][0] = (glyph & 15) * CELL_WIDTH + metric.padding.left * 2;
		font->glyphData[offset][1] = (glyph >> 4) * CELL_HEIGHT + metric.padding.top * 2;
		font->dimsData[offset][0] = CELL_WIDTH - (metric.padding.left + metric.padding.right) * 2;
		font->dimsData[offset][1] = CELL_HEIGHT - (metric.padding.top + metric.padding.bottom) * 2;
		font->transformData[0][offset][0] = 1.0f;
		font->transformData[0][offset][1] = 0.0f;
		font->transformData[1][offset][0] = 0.0f;
		font->transformData[1][offset][1] = 1.0f;
		font->colorData[offset][0] = (color & 0xFF) / 255.0f;
		font->colorData[offset][1] = ((color >> 8) & 0xFF) / 255.0f;
		font->colorData[offset][2] = ((color >> 16) & 0xFF) / 255.0f;
		font->colorData[offset][3] = ((color >> 24) & 0xFF) / 255.0f;

		++font->currentGlyph;
	}
}

void GUIFontDrawIcon(struct GUIFont* font, int x, int y, enum GUIAlignment align, enum GUIOrientation orient, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) {
		return;
	}
	struct GUIIconMetric metric = defaultIconMetrics[icon];

	float hFlip = 1.0f;
	float vFlip = 1.0f;
	switch (align & GUI_ALIGN_HCENTER) {
	case GUI_ALIGN_HCENTER:
		x -= metric.width;
		break;
	case GUI_ALIGN_RIGHT:
		x -= metric.width * 2;
		break;
	}
	switch (align & GUI_ALIGN_VCENTER) {
	case GUI_ALIGN_VCENTER:
		y -= metric.height;
		break;
	case GUI_ALIGN_BOTTOM:
		y -= metric.height * 2;
		break;
	}

	switch (orient) {
	case GUI_ORIENT_HMIRROR:
		hFlip = -1.0;
		break;
	case GUI_ORIENT_VMIRROR:
		vFlip = -1.0;
		break;
	case GUI_ORIENT_0:
	default:
		// TODO: Rotate
		break;
	}
	if (font->currentGlyph >= MAX_GLYPHS) {
		GUIFontDrawSubmit(font);
	}

	int offset = font->currentGlyph;

	font->originData[offset][0] = x;
	font->originData[offset][1] = y;
	font->originData[offset][2] = 0;
	font->glyphData[offset][0] = metric.x * 2;
	font->glyphData[offset][1] = metric.y * 2 + 256;
	font->dimsData[offset][0] = metric.width * 2;
	font->dimsData[offset][1] = metric.height * 2;
	font->transformData[0][offset][0] = hFlip;
	font->transformData[0][offset][1] = 0.0f;
	font->transformData[1][offset][0] = 0.0f;
	font->transformData[1][offset][1] = vFlip;
	font->colorData[offset][0] = (color & 0xFF) / 255.0f;
	font->colorData[offset][1] = ((color >> 8) & 0xFF) / 255.0f;
	font->colorData[offset][2] = ((color >> 16) & 0xFF) / 255.0f;
	font->colorData[offset][3] = ((color >> 24) & 0xFF) / 255.0f;

	++font->currentGlyph;
}

void GUIFontDrawIconSize(struct GUIFont* font, int x, int y, int w, int h, uint32_t color, enum GUIIcon icon) {
	if (icon >= GUI_ICON_MAX) {
		return;
	}
	struct GUIIconMetric metric = defaultIconMetrics[icon];

	if (!w) {
		w = metric.width * 2;
	}
	if (!h) {
		h = metric.height * 2;
	}

	if (font->currentGlyph >= MAX_GLYPHS) {
		GUIFontDrawSubmit(font);
	}

	int offset = font->currentGlyph;

	font->originData[offset][0] = x + w / 2 - metric.width;
	font->originData[offset][1] = y + h / 2 - metric.height;
	font->originData[offset][2] = 0;
	font->glyphData[offset][0] = metric.x * 2;
	font->glyphData[offset][1] = metric.y * 2 + 256;
	font->dimsData[offset][0] = metric.width * 2;
	font->dimsData[offset][1] = metric.height * 2;
	font->transformData[0][offset][0] = w * 0.5f / metric.width;
	font->transformData[0][offset][1] = 0.0f;
	font->transformData[1][offset][0] = 0.0f;
	font->transformData[1][offset][1] = h * 0.5f / metric.height;
	font->colorData[offset][0] = (color & 0xFF) / 255.0f;
	font->colorData[offset][1] = ((color >> 8) & 0xFF) / 255.0f;
	font->colorData[offset][2] = ((color >> 16) & 0xFF) / 255.0f;
	font->colorData[offset][3] = ((color >> 24) & 0xFF) / 255.0f;

	++font->currentGlyph;
}

void GUIFontDrawSubmit(struct GUIFont* font) {
	glUseProgram(font->program);
	glBindVertexArray(font->vao);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUniform1i(font->texLocation, 0);

	/* Render ASCII glyphs */
	if (font->currentGlyph > 0) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, font->font);

		glBindBuffer(GL_ARRAY_BUFFER, font->originVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 3 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 3 * font->currentGlyph, font->originData);

		glBindBuffer(GL_ARRAY_BUFFER, font->glyphVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->currentGlyph, font->glyphData);

		glBindBuffer(GL_ARRAY_BUFFER, font->dimsVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->currentGlyph, font->dimsData);

		glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[0]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->currentGlyph, font->transformData[0]);

		glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[1]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->currentGlyph, font->transformData[1]);

		glBindBuffer(GL_ARRAY_BUFFER, font->colorVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 4 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 4 * font->currentGlyph, font->colorData);

		glUniform1f(font->cutoffLocation, 0.1f);
		glUniform3f(font->colorModulusLocation, 0.f, 0.f, 0.f);
		glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, font->currentGlyph);

		glUniform1f(font->cutoffLocation, 0.7f);
		glUniform3f(font->colorModulusLocation, 1.f, 1.f, 1.f);
		glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, font->currentGlyph);

		font->currentGlyph = 0;
	}

	/* Render CJK glyphs */
	if (font->cjkCurrentGlyph > 0) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, font->cjkTexture);

		glBindBuffer(GL_ARRAY_BUFFER, font->originVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 3 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 3 * font->cjkCurrentGlyph, font->cjkOriginData);

		glBindBuffer(GL_ARRAY_BUFFER, font->glyphVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->cjkCurrentGlyph, font->cjkGlyphData);

		glBindBuffer(GL_ARRAY_BUFFER, font->dimsVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->cjkCurrentGlyph, font->cjkDimsData);

		glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[0]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->cjkCurrentGlyph, font->cjkTransformData[0]);

		glBindBuffer(GL_ARRAY_BUFFER, font->transformVbo[1]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 2 * font->cjkCurrentGlyph, font->cjkTransformData[1]);

		glBindBuffer(GL_ARRAY_BUFFER, font->colorVbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 4 * MAX_GLYPHS, NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 4 * font->cjkCurrentGlyph, font->cjkColorData);

		/* CJK glyphs are regular grayscale, use a single cutoff */
		glUniform1f(font->cutoffLocation, 0.05f);
		glUniform3f(font->colorModulusLocation, 1.f, 1.f, 1.f);
		glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, font->cjkCurrentGlyph);

		font->cjkCurrentGlyph = 0;
	}

	glBindVertexArray(0);
	glUseProgram(0);
}
