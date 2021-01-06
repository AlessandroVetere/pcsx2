/*
 *	Copyright (C) 2007-2009 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#pragma once

#include <iomanip>
#include <limits>

#include "Renderers/Common/GSRenderer.h"
#include "Renderers/Common/GSFastList.h"
#include "Renderers/Common/GSDirtyRect.h"

class GSTextureCache
{
public:
	enum struct SurfaceType
	{
		Invalid = -1,
		Source = 0,
		RenderTarget = 1,
		DepthStencil = 2
	};

	class Surface : public GSAlignedClass<32>
	{
	protected:
		GSRenderer* m_renderer;

	public:
		struct { GSVector4i* rect; uint32 count; } m_write;
		GSTexture* m_texture;
		GIFRegTEX0 m_TEX0;
		GIFRegTEXA m_TEXA;
		uint8* m_temp;
		uint32 m_end_block;  // Hint of the surface area.
		uint32* m_pages_as_bit;
		GSOffset* m_off;
		GSVector4i m_rect;  // Hint of the surface rect.
		uint32 m_pages_count;
		uint32 m_valid[MAX_PAGES]; // each uint32 bits map to the 32 blocks of that page
		std::unordered_set<uint32> m_dirty_pages;
		bool m_repeating;
		std::vector<GSVector2i>* m_p2t;
		SurfaceType m_type;
		GSTexture* m_palette;
		GIFRegTEX0 m_layer_TEX0[7]; // Detect already loaded value
		int m_upscale_multiplier;

		Surface(GSRenderer* r, uint8* temp, const GIFRegTEX0& TEX0, const SurfaceType type);
		~Surface();

		bool Inside(const uint32 bp, const uint32 bw, const uint32 psm, const GSVector4i& rect);
		bool Overlaps(const uint32 bp, const uint32 bw, const uint32 psm, const GSVector4i& rect);
		bool FullyDirty() const;
		void SetDirtyPage(const uint32 p);
		uint32* GetPages();
		uint32 GetBlockPointerPage() const;
		void Write(const GSVector4i& r, const int layer);
		void Flush(const uint32 count, const int layer);
		bool IsDirtyPage(const uint32 p) const;
		bool IsComplete() const;
	};

	struct PaletteKey {
		const uint32* clut;
		uint16 pal;
	};

	class Palette
	{
	private:
		uint32* m_clut;
		uint16 m_pal;
		GSTexture* m_tex_palette;
		const GSRenderer* m_renderer;

	public:
		Palette(const GSRenderer* renderer, const uint16 pal, const bool need_gs_texture);
		~Palette();

		// Disable copy constructor and copy operator
		Palette(const Palette&) = delete;
		Palette& operator=(const Palette&) = delete;

		// Disable move constructor and move operator
		Palette(const Palette&&) = delete;
		Palette& operator=(const Palette&&) = delete;

		GSTexture* GetPaletteGSTexture() const;
		PaletteKey GetPaletteKey() const;
		void InitializeTexture();
	};

	struct PaletteKeyHash {
		// Calculate hash
		std::size_t operator()(const PaletteKey &key) const;
	};

	struct PaletteKeyEqual {
		// Compare pal value and clut contents
		bool operator()(const PaletteKey &lhs, const PaletteKey &rhs) const;
	};

	class Source : public Surface
	{
	public:
		std::shared_ptr<Palette> m_palette_obj;
		std::array<uint16, MAX_PAGES> m_erase_it;  // Keep a GSTextureCache::SourceMap::m_map iterator to allow fast erase.

		Source(GSRenderer* r, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, uint8* temp);

		bool ClutMatch(const PaletteKey& palette_key) const;
	};

	class Target : public Surface
	{
	public:
		Target(GSRenderer* r, const GIFRegTEX0& TEX0, uint8* temp, const SurfaceType type);

		void Extend(const GIFRegTEX0& TEX0);
	};

	class PaletteMap
	{
	private:
		static const uint16 MAX_SIZE = 65535; // Max size of each map.
		const GSRenderer* m_renderer;
		
		// Array of 2 maps, the first for 64B palettes and the second for 1024B palettes.
		// Each map stores the key PaletteKey (clut copy, pal value) pointing to the relevant shared pointer to Palette object.
		// There is one PaletteKey per Palette, and the hashing and comparison of PaletteKey is done with custom operators PaletteKeyHash and PaletteKeyEqual.
		std::array<std::unordered_map<PaletteKey, std::shared_ptr<Palette>, PaletteKeyHash, PaletteKeyEqual>, 2> m_maps;

	public:
		PaletteMap(const GSRenderer* renderer);

		// Retrieves a shared pointer to a valid Palette from m_maps or creates a new one adding it to the data structure
		std::shared_ptr<Palette> LookupPalette(const uint16 pal, const bool need_gs_texture);

		void Clear(); // Clears m_maps, thus deletes Palette objects
	};

	class SourceMap
	{
	public:
		std::unordered_set<Source*> m_surfaces;
		std::array<FastList<Source*>, MAX_PAGES> m_map;
		uint32 m_pages[16]; // bitmap of all pages
		bool m_used;

		SourceMap() : m_used(false) {memset(m_pages, 0, sizeof(m_pages));}

		void Add(Source* s, const GIFRegTEX0& TEX0, const GSOffset* off);
		void RemoveAll();
		void RemoveAt(Source* s);
	};

	struct TexInsideRtCacheEntry
	{
		uint32 psm;
		uint32 bp;
		uint32 bp_end;
		uint32 bw;
		uint32 t_tex0_tbp0;
		uint32 m_end_block;
		bool has_valid_offset;
		int x_offset;
		int y_offset;
	};

	enum struct PageState
	{
		CPU = 0,
		GPU = 1,
	};

	struct PageInfo
	{
		PageState state;
		// FastList<Source*> copies;  (Access from TC via m_src.m_map[p] instead).
		Target* fb;

		PageInfo() noexcept
			: state(PageState::CPU)
			, fb(nullptr)
		{
		}

		bool is_sync() const noexcept
		{
			return state == PageState::CPU && fb;
		}

		std::string to_string() const
		{
			std::stringstream ss;
			ss << "State: " << (state == PageState::CPU ? "CPU" : "GPU") << ", ";
			ss << "Sync: " << (is_sync() ? "YES" : "NO");
			if (fb)
			{
				ss << ", ";
				ss << "FbAddr: 0x" << std::hex << std::setw(8) << std::setfill('0') << fb << std::dec << ", ";
				ss << "FbTexID: " << (fb->m_texture ? fb->m_texture->GetID() : -1);
			}
			return ss.str();
		}
	};

protected:
	GSRenderer* m_renderer;
	PaletteMap m_palette_map;
	SourceMap m_src;
	std::unordered_set<Target*> m_dst;
	bool m_paltex;
	bool m_preload_frame;
	uint8* m_temp;
	bool m_can_convert_depth;
	bool m_cpu_fb_conversion;
	CRCHackLevel m_crc_hack_level;
	static bool m_disable_partial_invalidation;
	bool m_texture_inside_rt;
	static bool m_wrap_gs_mem;
	uint8 m_texture_inside_rt_cache_size = 255;
	std::vector<TexInsideRtCacheEntry> m_texture_inside_rt_cache;
	std::array<PageInfo, MAX_PAGES> m_pages;

	virtual int Get8bitFormat() = 0;

	// TODO: virtual void Write(Source* s, const GSVector4i& r) = 0;
	// TODO: virtual void Write(Target* t, const GSVector4i& r) = 0;

public:
	GSTextureCache(GSRenderer* r);
	virtual ~GSTextureCache();

	virtual void Read(Target* t, const GSVector4i& r) = 0;
	virtual void Read(Source* t, const GSVector4i& r) = 0;
	void RemoveAll();

	Source* LookupSource(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GSVector4i& r);
	Target* LookupTarget(const GIFRegTEX0& TEX0, const SurfaceType type);

	void InvalidateVideoMem(const GSOffset* off, const GSVector4i& r, Target* fb);
	void InvalidateLocalMem(const GSOffset* off, const GSVector4i& r);

	void UpdateSurfacePage(Surface* s, const Surface* t, const uint32 p, bool& out_result);  // dst is the source of the copy.
	void UpdateSurface(Surface* s, const GSVector4i& rect, const int layer = 0);
	void UpdateSurfaceLayer(Surface* s, const GIFRegTEX0& TEX0, const GSVector4i& rect, const int layer = 0);

	bool UserHacks_HalfPixelOffset;
	void ScaleTexture(GSTexture* texture);

	bool ShallSearchTextureInsideRt();

	const char* to_string(const SurfaceType type) noexcept {
		return (type == SurfaceType::DepthStencil) ? "Depth" : "Color";
	}

	void PrintMemoryUsage();

	void AttachPaletteToSource(Source* s, const uint16 pal, const bool need_gs_texture);
};
