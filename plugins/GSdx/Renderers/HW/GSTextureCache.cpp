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

#include "stdafx.h"
#include "GSTextureCache.h"
#include "GSRendererHW.h"
#include "GSUtil.h"

bool GSTextureCache::m_disable_partial_invalidation = false;
bool GSTextureCache::m_wrap_gs_mem = false;

GSTextureCache::GSTextureCache(GSRenderer* r)
	: m_renderer(r)
	, m_palette_map(r)
{
	if (theApp.GetConfigB("UserHacks")) {
		UserHacks_HalfPixelOffset      = theApp.GetConfigI("UserHacks_HalfPixelOffset") == 1;
		m_preload_frame                = theApp.GetConfigB("preload_frame_with_gs_data");
		m_disable_partial_invalidation = theApp.GetConfigB("UserHacks_DisablePartialInvalidation");
		m_can_convert_depth            = !theApp.GetConfigB("UserHacks_DisableDepthSupport");
		m_cpu_fb_conversion            = theApp.GetConfigB("UserHacks_CPU_FB_Conversion");
		m_texture_inside_rt            = theApp.GetConfigB("UserHacks_TextureInsideRt");
		m_wrap_gs_mem                  = theApp.GetConfigB("wrap_gs_mem");
	} else {
		UserHacks_HalfPixelOffset      = false;
		m_preload_frame                = false;
		m_disable_partial_invalidation = false;
		m_can_convert_depth            = true;
		m_cpu_fb_conversion            = false;
		m_texture_inside_rt            = false;
		m_wrap_gs_mem                  = false;
	}

	m_paltex = theApp.GetConfigB("paltex");
	m_crc_hack_level = theApp.GetConfigT<CRCHackLevel>("crc_hack_level");
	if (m_crc_hack_level == CRCHackLevel::Automatic)
		m_crc_hack_level = GSUtil::GetRecommendedCRCHackLevel(theApp.GetCurrentRendererType());

	// In theory 4MB is enough but 9MB is safer for overflow (8MB
	// isn't enough in custom resolution)
	// Test: onimusha 3 PAL 60Hz
	m_temp = (uint8*)_aligned_malloc(9 * 1024 * 1024, 32);

	m_texture_inside_rt_cache.reserve(m_texture_inside_rt_cache_size);
}

GSTextureCache::~GSTextureCache()
{
	RemoveAll();

	m_texture_inside_rt_cache.clear();

	_aligned_free(m_temp);
}

void GSTextureCache::RemoveAll()
{
	if (m_renderer->m_dev)
	{
		GL_INS("TC: RemoveAll.");
		constexpr uint32 bp = 0;
		constexpr uint32 bw = 1;
		constexpr uint32 psm = PSM_PSMCT32;
		const GSOffset* off = m_renderer->m_mem.GetOffset(bp, bw, psm);
		const GSVector2i s = GSLocalMemory::m_psm[psm].pgs;
		const GSVector4i r = GSVector4i(0, 0, s.x, s.y * MAX_PAGES);
		InvalidateLocalMem(off, r);  // Read back whole memory.
		GL_INS("TC: Invalidated local memory.");
	}
	else
	{
		printf("TC: RemoveAll - Cannot readback\n");
	}

	for (PageInfo& pi : m_pages)
	{
		assert(pi.state == PageState::CPU);
		assert(!pi.fb || m_dst.find(pi.fb) != m_dst.cend());
		pi.fb = nullptr;  // Clear references.
	}
	
	// Delete targets.
	for (Target* t : m_dst)
		delete t;
	m_dst.clear();

	m_src.RemoveAll();

	m_palette_map.Clear();
}

GSTextureCache::Source* GSTextureCache::LookupSource(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GSVector4i& r)
{
	ASSERT(TEX0.TBW > 0);
	ASSERT(TEX0.TBP0 < 0x4000);
	ASSERT(TEX0.PSM < 64);
	ASSERT((r >= GSVector4i(0, 0, 0, 0)).alltrue());
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];
	//const GSLocalMemory::psm_t& cpsm = psm.pal > 0 ? GSLocalMemory::m_psm[TEX0.CPSM] : psm;

	// Until DX is fixed
	if(psm_s.pal > 0)
		m_renderer->m_mem.m_clut.Read32(TEX0, TEXA);

	const uint32* clut = m_renderer->m_mem.m_clut;

	Source* src = nullptr;

	const uint32 page = TEX0.TBP0 >> 5;

	auto& m = m_src.m_map[page];

	for(auto i = m.begin(); i != m.end(); ++i)
	{
		Source* s = *i;

		if (((TEX0.u32[0] ^ s->m_TEX0.u32[0]) | ((TEX0.u32[1] ^ s->m_TEX0.u32[1]) & 3)) != 0) // TBP0 TBW PSM TW TH
			continue;

		// TODO Check readback.
		// We request a palette texture (psm_s.pal). If the texture was
		// converted by the CPU (!s->m_palette), we need to ensure
		// palette content is the same.
		if (psm_s.pal > 0 && !s->m_palette && !s->ClutMatch({ clut, psm_s.pal }))
			continue;

		// We request a 24/16 bit RGBA texture. Alpha expansion was done by
		// the CPU.  We need to check that TEXA is identical
		if (psm_s.pal == 0 && psm_s.fmt > 0 && s->m_TEXA.u64 != TEXA.u64)
			continue;

		m.MoveFront(i.Index());

		src = s;

		break;
	}

	if(src == nullptr)
	{
		GL_CACHE("TC: src miss (0x%X, 0x%X, %s)", TEX0.TBP0, psm_s.pal > 0 ? TEX0.CBP : 0, psm_str(TEX0.PSM));
		src = new Source(m_renderer, TEX0, TEXA, m_temp);
		const int tw = 1 << TEX0.TW;
		const int th = 1 << TEX0.TH;
		const uint16 pal = psm_s.pal;
		const int w = tw * src->m_upscale_multiplier;
		const int h = th * src->m_upscale_multiplier;
		if (m_paltex && pal > 0)
		{
			src->m_texture = m_renderer->m_dev->CreateTexture(w, h, Get8bitFormat());
			AttachPaletteToSource(src, pal, true);
		}
		else
		{
			src->m_texture = m_renderer->m_dev->CreateTexture(w, h);
			if (pal > 0)
				AttachPaletteToSource(src, pal, false);
		}
		ASSERT(src->m_texture);
		m_src.Add(src, TEX0, src->m_off);
	} else {
		GL_CACHE("TC: src hit: %d (0x%X, 0x%X, %s)",
					src->m_texture ? src->m_texture->GetID() : 0,
					TEX0.TBP0, psm_s.pal > 0 ? TEX0.CBP : 0,
					psm_str(TEX0.PSM));
		if (src->m_palette && !src->ClutMatch({ clut, psm_s.pal }))
			AttachPaletteToSource(src, psm_s.pal, true);
	}

	UpdateSurface(src, r);

	m_src.m_used = true;

	ASSERT(src->m_dirty_pages.empty());
	return src;
}

GSTextureCache::Target* GSTextureCache::LookupTarget(const GIFRegTEX0& TEX0, const SurfaceType type)
{
	ASSERT(type == SurfaceType::RenderTarget || type == SurfaceType::DepthStencil);
	ASSERT(TEX0.TBW > 0);
	ASSERT(TEX0.TBP0 < 0x4000);
	ASSERT(TEX0.PSM < 64);
	ASSERT(TEX0.TW > 0);
	ASSERT(TEX0.TH > 0);
	ASSERT(TEX0.TW <= 12);
	ASSERT(TEX0.TH <= 12);
	const uint32 page = TEX0.TBP0 >> 5;
	const PageInfo& pi = m_pages.at(page);
	Target* t = nullptr;
	if (pi.fb && pi.fb->m_TEX0.PSM == TEX0.PSM && pi.fb->m_TEX0.TBW == TEX0.TBW && pi.fb->m_TEX0.TBP0 == TEX0.TBP0)
	{
		// Recycle pi.fb.
		t = pi.fb;
		t->Extend(TEX0);
		GL_CACHE("TC: dst hit (0x%X, %s, FbAddr: 0x%08X, FbTexID: %d)", TEX0.TBP0, psm_str(TEX0.PSM), t, t->m_texture ? t->m_texture->GetID() : -1);
	}
	else
	{
		// Create new target.
		t = new Target(m_renderer, TEX0, m_temp, type);
		ASSERT(t);
		ASSERT(m_dst.find(t) == m_dst.cend());
		m_dst.emplace(t);
		// t will be added to m_pages in InvalidateVideoMem after draw.
		GL_CACHE("TC: dst miss (0x%X, %s)", TEX0.TBP0, psm_str(TEX0.PSM));
	}
	ASSERT(t);
	UpdateSurface(t, t->m_rect);
	ASSERT(t->m_dirty_pages.empty());
	return t;
}

void GSTextureCache::ScaleTexture(GSTexture* texture)
{
	if (!m_renderer->CanUpscale())
		return;

	float multiplier = static_cast<float>(m_renderer->GetUpscaleMultiplier());
	bool custom_resolution = (multiplier == 0);
	GSVector2 scale_factor(multiplier);

	if (custom_resolution)
	{
		int width = m_renderer->GetDisplayRect().width();
		int height = m_renderer->GetDisplayRect().height();

		GSVector2i requested_resolution = m_renderer->GetCustomResolution();
		scale_factor.x = static_cast<float>(requested_resolution.x) / width;
		scale_factor.y = static_cast<float>(requested_resolution.y) / height;
	}

	texture->SetScale(scale_factor);
}

bool GSTextureCache::ShallSearchTextureInsideRt()
{
	return m_texture_inside_rt || (m_renderer->m_game.flags & CRC::Flags::TextureInsideRt);
}

// Goal: invalidate data sent to the GPU when the source (GS memory) is modified
// Called each time you want to write to the GS memory
void GSTextureCache::InvalidateVideoMem(const GSOffset* off, const GSVector4i& rect, Target* fb)
{
	if (!off)
	{
		printf("ERROR - NULL offset in InvalidateVideoMem.\n");
		return; // Fixme. Crashes Dual Hearts, maybe others as well. Was fine before r1549.
	}

	ASSERT(!fb || m_dst.find(fb) != m_dst.cend());  // fb is null or in m_dst.
	ASSERT(!fb || off == fb->m_off);  // fb is null or same offset.

	const uint32 bp = off->bp;
	const uint32 bw = off->bw;
	const uint32 psm = off->psm;

	GSVector4i r;
	uint32* pages = (uint32*)m_temp;
	off->GetPages(rect, pages, &r);

	// Invalidate pages.
	for (const uint32* pptr = pages; pptr != nullptr && *pptr != GSOffset::EOP; ++pptr)
	{
		const uint32 p = *pptr;
		PageInfo& pi = m_pages.at(p);
		auto& list = m_src.m_map.at(p);
		// Invalidate Source(s).
		for (Source* s : list)
		{
			ASSERT(s);
			s->SetDirtyPage(p);
			if (s->FullyDirty())
				m_src.RemoveAt(s);
		}

		// Invalidate Target.
		Target* old_fb = pi.fb;
		if (fb != old_fb)
		{
			// GL_INS("Page invalidated on GPU: #%u, %s", p, pi.to_string().c_str());
			if (old_fb)
			{
				// Set page as dirty in old_fb.
				old_fb->SetDirtyPage(p);
				pi.fb = nullptr;  // Remove reference.
				if (old_fb->FullyDirty())
				{
					// Delete target if it has no valid page.
					assert(old_fb != fb);
					m_dst.erase(old_fb);  // Remove from tracking set.
					delete old_fb;
				}
			}

			assert(!pi.fb);
			pi.fb = fb;  // Replace fb.
			pi.state = fb ? PageState::GPU : PageState::CPU;  // Update state.
			assert(!pi.fb || !pi.fb->IsDirtyPage(p));

			// GL_INS("New page state: #%u, %s", p, pi.to_string().c_str());
		}
	}
}

// Goal: retrive the data from the GPU to the GS memory.
// Called each time you want to read from the GS memory
void GSTextureCache::InvalidateLocalMem(const GSOffset* off, const GSVector4i& r)
{
	const uint32 bp = off->bp;
	const uint32 bw = off->bw;
	const uint32 psm = off->psm;

	uint32* pages = (uint32*)m_temp;
	off->GetPages(r, pages, nullptr);

	const bool depth_fmt = psm == PSM_PSMZ32 || psm == PSM_PSMZ24 || psm == PSM_PSMZ16 || psm == PSM_PSMZ16S;

	// Readback pages.
	uint32 count = 0;
	for (const uint32* pptr = pages; pptr != nullptr && *pptr != GSOffset::EOP; ++pptr)
	{
		++count;
		const uint32 p = *pptr;
		// GL_INS("TC: InvalidateLocalMem %d page.", p);
		PageInfo& pi = m_pages.at(p);
		if (pi.state == PageState::GPU)
		{
			assert(pi.fb);
			assert(!pi.fb->IsDirtyPage(p));
			ASSERT(m_dst.find(pi.fb) != m_dst.cend());  // fb is null or in m_dst.
			// GL_INS("Page to be readback from GPU: #%u, %s", p, pi.to_string().c_str());
			const GSVector4i rp = pi.fb->m_off->GetRect(p);
			Read(pi.fb, rp);  // TODO Slow! Merge multiple small reads when possible.
			pi.state = PageState::CPU;
			// pi.fb still valid.
			assert(pi.is_sync());
		}
	}
	// GL_INS("TC: InvalidateLocalMem %d pages.", count);
}

void GSTextureCache::UpdateSurfacePage(Surface* s, const Surface* t, const uint32 p, bool& out_result)
{
	// Copy page p from t to s.
	assert(s->IsDirtyPage(p));   // Page is dirty in s.
	assert(!t->IsDirtyPage(p));  // Page is clean in t.
	assert(s && s->m_texture);
	assert(t && t->m_texture);
	out_result = false;

	const uint32 psm_s = s->m_TEX0.PSM;
	const uint32 psm_t = t->m_TEX0.PSM;

	if (psm_s == PSM_PSMT4)
	{
		// There is no shader code to convert 4 bits.
		// Read it back for now.
		// GL_INS("ERROR - TC - UpdateSurfacePage - unsupported PSM_PSMT4.");
		return;
	}

	const bool is_pal_t = GSLocalMemory::m_psm[psm_t].pal != 0;
	if (t->m_type == SurfaceType::Source && is_pal_t)
	{
		GL_INS("ERROR - TC - UpdateSurfacePage - unsupported PALETTED-SOURCE-COPY.");
		return;
	}

	const uint32 bp_t = t->m_TEX0.TBP0;
	const uint32 bp_s = s->m_TEX0.TBP0;
	if (p == t->GetBlockPointerPage() && bp_t != bp_s)
	{
		// TODO Handle delicate case.
		// GL_INS("ERROR - TC - UpdateSurfacePage - unsupported OFFSET-COPY.");
		return;
	}

	const GSVector4i p_rect_t = t->m_off->GetRect(p);
	const GSVector4i p_rect_s = s->m_off->GetRect(p);
	if (!p_rect_s.rintersect(s->m_rect).eq(p_rect_s))
	{
		// The page is not fully covered by the source texture.
		// Read it back for now.
		// TODO Handle delicate case.
		// GL_INS("ERROR - TC - UpdateSurfacePage - unsupported PARTIAL-PAGE-COPY.");
		return;
	}

	const bool linear = psm_s == PSM_PSMCT32 || psm_s == PSM_PSMCT24;
	GSTexture* sTex = t->m_texture;  // Copy source.
	GSTexture* dTex = s->m_texture;  // Copy destination.
	const int w_t = t->m_rect.width();
	const int h_t = t->m_rect.height();
	const GSVector4 size_t = GSVector4(w_t, h_t).xyxy();
	ASSERT((size_t == GSVector4(w_t, h_t, w_t, h_t)).alltrue());

	if (psm_s == psm_t && (p_rect_s == p_rect_t).alltrue())
	{
		// Copy.
		const GSVector4i r = p_rect_s;
		m_renderer->m_dev->CopyRect(sTex, dTex, r);
		out_result = true;
		return;
	}

	if (psm_s == psm_t)
	{
		// Copy.
		const GSVector4 sRect = GSVector4(p_rect_t) / size_t;
		const GSVector4 dRect = GSVector4(p_rect_s) * (float)s->m_upscale_multiplier;
		constexpr ShaderConvert shader = ShaderConvert_COPY;
		m_renderer->m_dev->StretchRect(sTex, sRect, dTex, dRect, shader, linear);
		out_result = true;
		return;
	}

	// TODO Solve this.
	return;

	// Copy convert.
	if (s->m_type == SurfaceType::RenderTarget && t->m_type == SurfaceType::DepthStencil) {
		int shader;
		bool fmt_16_bits = (GSLocalMemory::m_psm[psm_s].bpp == 16 &&
							GSLocalMemory::m_psm[psm_t].bpp == 16);
		if (s->m_type == SurfaceType::DepthStencil) {
			shader = (fmt_16_bits) ? ShaderConvert_RGB5A1_TO_FLOAT16 : ShaderConvert_RGBA8_TO_FLOAT32 + GSLocalMemory::m_psm[psm_s].fmt;
		} else {
			shader = (fmt_16_bits) ? ShaderConvert_FLOAT16_TO_RGB5A1 : ShaderConvert_FLOAT32_TO_RGBA8;
		}
		const GSVector4 sRect = GSVector4(p_rect_t) / size_t;
		const GSVector4 dRect = GSVector4(p_rect_s) * (float)s->m_upscale_multiplier;
		m_renderer->m_dev->StretchRect(t->m_texture, sRect, s->m_texture, dRect, shader, false);
		return;
	}

	const bool is_8bits = psm_s == PSM_PSMT8;
	const int shader = is_8bits ? ShaderConvert_RGBA_TO_8I
		: (s->m_type == t->m_type || t->m_type == SurfaceType::RenderTarget) ? ShaderConvert_COPY
		: ShaderConvert_FLOAT32_TO_RGBA8;

	if (is_8bits)
	{
		GL_INS("Reading RT as a packed-indexed 8 bits format");
	}

	const uint32 tw = p_rect_s.width();
	const uint32 th = p_rect_s.height();

	// do not round here!!! if edge becomes a black pixel and addressing mode is clamp => everything outside the clamped area turns into black (kh2 shadows)

	int w = (int)(s->m_texture->GetScale().x * tw);
	int h = (int)(s->m_texture->GetScale().y * th);
	if (is_8bits) {
		// Unscale 8 bits textures, quality won't be nice but format is really awful
		w = tw;
		h = th;
	}

	const GSVector2i dstsize = s->m_texture->GetSize();
	GSVector2 scale = s->m_texture->GetScale();
	GSVector4 dRect(p_rect_s.x, p_rect_s.y, p_rect_s.z + w, p_rect_s.w + h);

	// Lengthy explanation of the rescaling code.
	// Here an example in 2x:
	// RT is 1280x1024 but only contains 512x448 valid data (so 256x224 pixels without upscaling)
	//
	// PS2 want to read it back as a 1024x1024 pixels (they don't care about the extra pixels)
	// So in theory we need to shrink a 2048x2048 RT into a 1024x1024 texture. Obviously the RT is
	// too small.
	//
	// So we will only limit the resize to the available data in RT.
	// Therefore we will resize the RT from 1280x1024 to 1280x1024/2048x2048 % of the new texture
	// size (which is 1280x1024) (i.e. 800x512)
	// From the rendering point of view. UV coordinate will be normalized on the real GS texture size
	// This way it can be used on an upscaled texture without extra scaling factor (only requirement is
	// to have same proportion)
	//
	// FIXME: The scaling will create a bad offset. For example if texture coordinate start at 0.5 (pixel 0)
	// At 2x it will become 0.5/128 * 256 = 1 (pixel 1)
	// I think it is the purpose of the UserHacks_HalfPixelOffset below. However implementation is less
	// than ideal.
	// 1/ It suppose games have an half pixel offset on texture coordinate which could be wrong
	// 2/ It doesn't support rescaling of the RT (tw = 1024)
	// Maybe it will be more easy to just round the UV value in the Vertex Shader

	if (!is_8bits) {
		// 8 bits handling is special due to unscaling. It is better to not execute this code
		if (w > dstsize.x)
		{
			scale.x = (float)dstsize.x / tw;
			dRect.z = (float)dstsize.x * scale.x / s->m_texture->GetScale().x;
			w = dstsize.x;
		}

		if (h > dstsize.y)
		{
			scale.y = (float)dstsize.y / th;
			dRect.w = (float)dstsize.y * scale.y / s->m_texture->GetScale().y;
			h = dstsize.y;
		}
	}

	GSVector4 sRect(p_rect_t.x, p_rect_t.y, p_rect_t.z + w, p_rect_t.w + h);

	// Disable linear filtering for various GS post-processing effect
	// 1/ Palette is used to interpret the alpha channel of the RT as an index.
	// Star Ocean 3 uses it to emulate a stencil buffer.
	// 2/ Z formats are a bad idea to interpolate (discontinuties).
	// 3/ 16 bits buffer is used to move data from a channel to another.
	//
	// I keep linear filtering for standard color even if I'm not sure that it is
	// working correctly.
	// Indeed, texture is reduced so you need to read all covered pixels (9 in 3x)
	// to correctly interpolate the value. Linear interpolation is likely acceptable
	// only in 2x scaling
	//
	// Src texture will still be bilinear interpolated so I'm really not sure
	// that we need to do it here too.
	//
	// Future note: instead to do
	// RT 2048x2048 -> T 1024x1024 -> RT 2048x2048
	// We can maybe sample directly a bigger texture
	// RT 2048x2048 -> T 2048x2048 -> RT 2048x2048
	// Pro: better quality. Copy instead of StretchRect (must be faster)
	// Cons: consume more memory
	//
	// In distant future: investigate to reuse the RT directly without any
	// copy. Likely a speed boost and memory usage reduction.
	
	if ((sRect == dRect).alltrue() && shader == ShaderConvert_COPY)
	{
		// TODO Check sRect.
		m_renderer->m_dev->CopyRect(sTex, dTex, GSVector4i(sRect));
	}
	else
	{
		// Different size or not the same format
		sRect /= size_t;

		m_renderer->m_dev->StretchRect(sTex, sRect, dTex, dRect, shader, linear);
	}

	if (s->m_texture)
		s->m_texture->SetScale(scale);

	ASSERT(s->m_texture);

	/*
	// Offset hack. Can be enabled via GSdx options.
	// The offset will be used in Draw().

	float modx = 0.0f;
	float mody = 0.0f;

	if (UserHacks_HalfPixelOffset && hack)
	{
		switch (m_renderer->GetUpscaleMultiplier())
		{
		case 0: //Custom Resolution
		{
			const float offset = 0.2f;
			modx = dst->m_texture->GetScale().x + offset;
			mody = dst->m_texture->GetScale().y + offset;
			dst->m_texture->LikelyOffset = true;
			break;
		}
		case 2:  modx = 2.2f; mody = 2.2f; dst->m_texture->LikelyOffset = true;  break;
		case 3:  modx = 3.1f; mody = 3.1f; dst->m_texture->LikelyOffset = true;  break;
		case 4:  modx = 4.2f; mody = 4.2f; dst->m_texture->LikelyOffset = true;  break;
		case 5:  modx = 5.3f; mody = 5.3f; dst->m_texture->LikelyOffset = true;  break;
		case 6:  modx = 6.2f; mody = 6.2f; dst->m_texture->LikelyOffset = true;  break;
		case 8:  modx = 8.2f; mody = 8.2f; dst->m_texture->LikelyOffset = true;  break;
		default: modx = 0.0f; mody = 0.0f; dst->m_texture->LikelyOffset = false; break;
		}
	}

	dst->m_texture->OffsetHack_modx = modx;
	dst->m_texture->OffsetHack_mody = mody;
	*/

	// TODO Restore offset hack.
}

void GSTextureCache::UpdateSurface(Surface* s, const GSVector4i& rect, const int layer)
{
	ASSERT(rect.rintersect(s->m_rect).eq(rect));
	ASSERT(s->m_type == SurfaceType::Source || layer == 0);

	if (layer == 0 && s->IsComplete())
	{
		return;
	}

	const uint32 bp = s->m_TEX0.TBP0;
	const uint32 bw = s->m_TEX0.TBW;
	const uint32 psm = s->m_TEX0.PSM;

	for (auto it = s->m_dirty_pages.cbegin(); it != s->m_dirty_pages.cend(); )
	{
		const uint32 p = *it;
		bool p_deleted = false;
		PageInfo& pi = m_pages.at(p);
		if (pi.fb)
		{
			assert(!pi.fb->IsDirtyPage(p));
			assert(pi.fb != s);
			bool result = false;
			UpdateSurfacePage(s, pi.fb, p, result);
			if (result)
			{
				it = s->m_dirty_pages.erase(it);
				s->m_valid[p] = std::numeric_limits<uint32>::max();
				p_deleted = true;
			}
			else if (!pi.is_sync())
			{
				GL_INS("Page readback due to UNSUPPORTED UPDATE: #%u, %s", p, pi.to_string().c_str());
				const GSVector4i fb_p_rect = pi.fb->m_off->GetRect(p);
				Read(pi.fb, fb_p_rect);
				pi.state = PageState::CPU;
				assert(pi.is_sync());
			}
		}
		if (!p_deleted)
			++it;
	}

	if (layer == 0 && s->IsComplete())
	{
		return;
	}

	const GSVector2i& bs = GSLocalMemory::m_psm[psm].bs;
	const int tw = std::max<int>(1 << s->m_TEX0.TW, bs.x);
	const int th = std::max<int>(1 << s->m_TEX0.TH, bs.y);
	const GSVector4i r = rect.ralign<Align_Outside>(bs);
	const GSOffset* off = s->m_off;
	uint32 blocks = 0;
	
	if (s->m_repeating)
	{
		for (int y = r.top; y < r.bottom; y += bs.y)
		{
			const uint32 base = off->block.row[y >> 3u];

			for (int x = r.left, i = (y << 7) + x; x < r.right; x += bs.x, i += bs.x)
			{
				const uint32 block = base + off->block.col[x >> 3u];

				if (block < MAX_BLOCKS || m_wrap_gs_mem)
				{
					const uint32 addr = (i >> 3u) % MAX_BLOCKS;

					const uint32 row = addr >> 5u;
					const uint32 col = 1 << (addr & 31u);

					if ((s->m_valid[row] & col) == 0)
					{
						s->m_valid[row] |= col;

						s->Write(GSVector4i(x, y, x + bs.x, y + bs.y), layer);

						blocks++;
					}
				}
			}
		}
	}
	else
	{
		for (int y = r.top; y < r.bottom; y += bs.y)
		{
			const uint32 base = off->block.row[y >> 3u];

			for (int x = r.left; x < r.right; x += bs.x)
			{
				uint32 block = base + off->block.col[x >> 3u];

				if (block < MAX_BLOCKS || m_wrap_gs_mem)
				{
					block %= MAX_BLOCKS;

					const uint32 row = block >> 5u;
					const uint32 col = 1 << (block & 31u);

					if ((s->m_valid[row] & col) == 0)
					{
						s->m_valid[row] |= col;

						s->Write(GSVector4i(x, y, x + bs.x, y + bs.y), layer);

						blocks++;
					}
				}
			}
		}
	}

	if (blocks > 0)
	{
		m_renderer->m_perfmon.Put(GSPerfMon::Unswizzle, bs.x * bs.y * blocks << (s->m_palette ? 2 : 0));

		s->Flush(s->m_write.count, layer);
	}

	s->m_dirty_pages.clear();
}

void GSTextureCache::UpdateSurfaceLayer(Surface* s, const GIFRegTEX0& TEX0, const GSVector4i& rect, const int layer)
{
	ASSERT(TEX0.TBP0 < 0x4000);
	ASSERT(TEX0.TBW > 0);
	ASSERT(TEX0.PSM < 64);
	if (layer > 6)
		return;

	if (TEX0 == s->m_layer_TEX0[layer])
		return;

	const GIFRegTEX0 old_TEX0 = s->m_TEX0;

	s->m_layer_TEX0[layer] = TEX0;
	s->m_TEX0 = TEX0;

	UpdateSurface(s, rect, layer);

	s->m_TEX0 = old_TEX0;
}

void GSTextureCache::PrintMemoryUsage()
{
#ifdef ENABLE_OGL_DEBUG
	uint32 tex    = 0;
	uint32 tex_rt = 0;
	uint32 rt     = 0;
	uint32 dss    = 0;
	for(auto s : m_src.m_surfaces) {
		ASSERT(s);
		if(s) {
			tex    += s->m_texture->GetMemUsage();
		}
	}
	for(auto t : m_dst) {
		ASSERT(t);
		if(t)
			rt += t->m_texture->GetMemUsage();
	}
	GL_PERF("MEM: RO Tex %dMB. RW Tex %dMB. Target %dMB. Depth %dMB", tex >> 20u, tex_rt >> 20u, rt >> 20u, dss >> 20u);
#endif
}

void GSTextureCache::AttachPaletteToSource(Source* s, const uint16 pal, const bool need_gs_texture)
{
	s->m_palette_obj = m_palette_map.LookupPalette(pal, need_gs_texture);
	s->m_palette = need_gs_texture ? s->m_palette_obj->GetPaletteGSTexture() : nullptr;
}

// GSTextureCache::Surface

GSTextureCache::Surface::Surface(GSRenderer* r, uint8* temp, const GIFRegTEX0& TEX0, const SurfaceType type)
	: m_renderer(r)
	, m_texture(nullptr)
	, m_temp(temp)
	, m_end_block(0)
	, m_off(nullptr)
	, m_rect(0, 0)
	, m_type(type)
	, m_palette(nullptr)
	, m_TEX0(TEX0)
{
	ASSERT(TEX0.TBP0 < 0x4000);
	ASSERT(TEX0.TBW > 0);
	ASSERT(TEX0.PSM < 64);
	ASSERT(TEX0.TW > 0);
	ASSERT(TEX0.TH > 0);
	ASSERT(type != SurfaceType::Invalid);
	memset(&m_layer_TEX0[0], 0, sizeof(m_layer_TEX0));
	memset(&m_valid[0], 0, sizeof(m_valid));
	m_write.rect = (GSVector4i*)_aligned_malloc(3 * sizeof(GSVector4i), 32);
	m_write.count = 0;
	m_repeating = type == SurfaceType::Source && m_TEX0.IsRepeating();
	m_p2t = m_repeating ? r->m_mem.GetPage2TileMap(m_TEX0) : nullptr;
	m_off = m_renderer->m_mem.GetOffset(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM);
	ASSERT(m_off);
	m_pages_as_bit = m_off->GetPagesAsBits(m_TEX0);
	const uint32 tw = 1 << TEX0.TW;
	const uint32 th = 1 << TEX0.TH;
	m_rect = GSVector4i(0, 0, tw, th);
	if (type == SurfaceType::RenderTarget || type == SurfaceType::DepthStencil)
		m_rect = m_rect.ralign<Align_Outside>(GSLocalMemory::m_psm[TEX0.PSM].pgs);
	m_dirty_pages.reserve(MAX_PAGES);
	uint32* pages = (uint32*)m_temp;  // Use temporary preallocated buffer.
	m_off->GetPages(m_rect, pages, nullptr);
	for (const uint32* pptr = pages; pptr != nullptr && *pptr != GSOffset::EOP; ++pptr)
	{
		const uint32 p = *pptr;
		m_dirty_pages.emplace(p);
	}
	m_pages_count = m_dirty_pages.size();
	m_upscale_multiplier = std::max<int>(1, r->GetUpscaleMultiplier());
}

GSTextureCache::Surface::~Surface()
{
	m_renderer->m_dev->Recycle(m_texture);
	_aligned_free(m_write.rect);
}

bool GSTextureCache::Surface::Inside(const uint32 bp, const uint32 bw, const uint32 psm, const GSVector4i& rect)
{
	// Valid only for color formats.
	uint32 const end_block = GSLocalMemory::m_psm[psm].bn(rect.z - 1, rect.w - 1, bp, bw);
	return bp >= m_TEX0.TBP0 && end_block <= m_end_block;
}

bool GSTextureCache::Surface::Overlaps(const uint32 bp, const uint32 bw, const uint32 psm, const GSVector4i& rect)
{
	// Valid only for color formats.
	uint32 const end_block = GSLocalMemory::m_psm[psm].bn(rect.z - 1, rect.w - 1, bp, bw);
	return (m_TEX0.TBP0 <= bp        && bp        <= m_end_block)
		|| (m_TEX0.TBP0 <= end_block && end_block <= m_end_block);
}

bool GSTextureCache::Surface::FullyDirty() const
{
	return m_pages_count == m_dirty_pages.size();
}

void GSTextureCache::Surface::SetDirtyPage(const uint32 p)
{
	ASSERT(p < MAX_PAGES);
	assert((m_off->GetRect(p) >= GSVector4i(0, 0, 0, 0)).alltrue());  // Check that page belongs to surface.
	m_dirty_pages.emplace(p);
	uint32* RESTRICT valid = m_valid;
	if (m_repeating)
	{
		// Note: very hot path on snowbling engine games.
		for (const GSVector2i& k : m_p2t[p])
		{
			valid[k.x] &= k.y;
		}
	}
	else
	{
		valid[p] = 0;
	}
}

uint32* GSTextureCache::Surface::GetPages()
{
	uint32* pages = (uint32*)m_temp;
	m_off->GetPages(m_rect, pages);
	return pages;
}

uint32 GSTextureCache::Surface::GetBlockPointerPage() const
{
	return m_TEX0.TBP0 >> 5;
}

void GSTextureCache::Surface::Write(const GSVector4i& r, const int layer)
{
	m_write.rect[m_write.count++] = r;

	while (m_write.count >= 2)
	{
		GSVector4i& a = m_write.rect[m_write.count - 2];
		GSVector4i& b = m_write.rect[m_write.count - 1];

		if ((a == b.zyxw()).mask() == 0xfff0)
		{
			a.right = b.right; // extend right

			m_write.count--;
		}
		else if ((a == b.xwzy()).mask() == 0xff0f)
		{
			a.bottom = b.bottom; // extend down

			m_write.count--;
		}
		else
		{
			break;
		}
	}

	if (m_write.count > 2)
	{
		Flush(1, layer);
	}
}

void GSTextureCache::Surface::Flush(const uint32 count, const int layer)
{
	// This function as written will not work for paletted formats copied from framebuffers
	// because they are 8 or 4 bit formats on the GS and the GS local memory module reads
	// these into an 8 bit format while the D3D surfaces are 32 bit.
	// However the function is never called for these cases.  This is just for information
	// should someone wish to use this function for these cases later.
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[m_TEX0.PSM];

	int tw = 1 << m_TEX0.TW;
	int th = 1 << m_TEX0.TH;

	GSVector4i tr(0, 0, tw, th);

	int pitch = std::max(tw, psm.bs.x) * sizeof(uint32);

	GSLocalMemory& mem = m_renderer->m_mem;

	const GSOffset* off = m_off;

	GSLocalMemory::readTexture rtx = psm.rtx;

	if (m_palette)
	{
		pitch >>= 2;
		rtx = psm.rtxP;
	}

	uint8* buff = m_temp;

	for (uint32 i = 0; i < count; i++)
	{
		GSVector4i r = m_write.rect[i];

		if ((r > tr).mask() & 0xff00)
		{
			(mem.*rtx)(off, r, buff, pitch, m_TEXA);

			m_texture->Update(r.rintersect(tr), buff, pitch, layer);
		}
		else
		{
			GSTexture::GSMap m;

			if (m_texture->Map(m, &r, layer))
			{
				(mem.*rtx)(off, r, m.bits, m.pitch, m_TEXA);

				m_texture->Unmap();
			}
			else
			{
				(mem.*rtx)(off, r, buff, pitch, m_TEXA);

				m_texture->Update(r, buff, pitch, layer);
			}
		}
	}

	if (count < m_write.count)
	{
		// Warning src and destination overlap. Memmove must be used instead of memcpy
		memmove(&m_write.rect[0], &m_write.rect[count], (m_write.count - count) * sizeof(m_write.rect[0]));
	}

	m_write.count -= count;
}

bool GSTextureCache::Surface::IsDirtyPage(const uint32 p) const
{
	return m_dirty_pages.find(p) != m_dirty_pages.cend();
}

bool GSTextureCache::Surface::IsComplete() const
{
	return m_dirty_pages.empty();
}

// GSTextureCache::Source

GSTextureCache::Source::Source(GSRenderer* r, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, uint8* temp)
	: Surface(r, temp, TEX0, SurfaceType::Source)
	, m_palette_obj(nullptr)
{
	m_TEXA = TEXA;
}

bool GSTextureCache::Source::ClutMatch(const PaletteKey& palette_key) const {
	return PaletteKeyEqual()(palette_key, m_palette_obj->GetPaletteKey());
}

// GSTextureCache::Target

GSTextureCache::Target::Target(GSRenderer* r, const GIFRegTEX0& TEX0, uint8* temp, const SurfaceType type)
	: Surface(r, temp, TEX0, type)
{
	ASSERT(type == SurfaceType::RenderTarget || type == SurfaceType::DepthStencil);
	const uint32 w = m_rect.width() * m_upscale_multiplier;
	const uint32 h = m_rect.height() * m_upscale_multiplier;
	m_texture = type == SurfaceType::RenderTarget ?
		m_renderer->m_dev->CreateSparseRenderTarget(w, h) :
		m_renderer->m_dev->CreateSparseDepthStencil(w, h);
}

void GSTextureCache::Target::Extend(const GIFRegTEX0& TEX0)
{
	ASSERT(TEX0.TBP0 == m_TEX0.TBP0 && TEX0.PSM == m_TEX0.PSM && TEX0.TBW == m_TEX0.TBW);
	if (m_TEX0.TW < TEX0.TW || m_TEX0.TH < TEX0.TH)
	{
		GL_INS("EXTEND!");
		const uint32 tex0tw = std::max<uint32>(m_TEX0.TW, TEX0.TW);
		const uint32 tex0th = std::max<uint32>(m_TEX0.TH, TEX0.TH);
		const uint32 tw = 1 << tex0tw;
		const uint32 th = 1 << tex0th;
		const GSVector4i r = GSVector4i(0, 0, tw, th).ralign<Align_Outside>(GSLocalMemory::m_psm[TEX0.PSM].pgs);
		const uint32 w = r.width() * m_upscale_multiplier;
		const uint32 h = r.height() * m_upscale_multiplier;
		GSTexture* tex = m_type == SurfaceType::RenderTarget ?
			m_renderer->m_dev->CreateSparseRenderTarget(w, h) :
			m_renderer->m_dev->CreateSparseDepthStencil(w, h);
		m_renderer->m_dev->CopyRect(m_texture, tex, m_rect);
		m_renderer->m_dev->Recycle(m_texture);
		uint32* pages = (uint32*)m_temp;  // Use temporary preallocated buffer.
		std::unordered_set<uint32> delta_dirty_pages;
		delta_dirty_pages.reserve(MAX_PAGES);
		m_off->GetPages(r, pages, nullptr);  // Add new rect pages.
		for (const uint32* pptr = pages; pptr != nullptr && *pptr != GSOffset::EOP; ++pptr)
		{
			const uint32 p = *pptr;
			delta_dirty_pages.emplace(p);
		}
		m_pages_count = delta_dirty_pages.size();  // New rect pages count.
		m_off->GetPages(m_rect, pages, nullptr);  // Remove previous rect pages.
		for (const uint32* pptr = pages; pptr != nullptr && *pptr != GSOffset::EOP; ++pptr)
		{
			const uint32 p = *pptr;
			delta_dirty_pages.erase(p);
		}
		m_texture = tex;
		m_rect = r;
		m_TEX0.TW = tex0tw;
		m_TEX0.TH = tex0th;
		m_pages_as_bit = m_off->GetPagesAsBits(m_TEX0);
		for (const uint32 p : delta_dirty_pages)
			SetDirtyPage(p);
	}
}

// GSTextureCache::SourceMap

void GSTextureCache::SourceMap::Add(Source* s, const GIFRegTEX0& TEX0, const GSOffset* off)
{
	m_surfaces.insert(s);

	// The source pointer will be stored/duplicated in all m_map[array of pages]
	for(size_t i = 0; i < countof(m_pages); i++)
	{
		if(uint32 p = s->m_pages_as_bit[i])
		{
			auto* m = &m_map[i << 5];
			auto* e = &s->m_erase_it[i << 5];

			unsigned long j;

			while(_BitScanForward(&j, p))
			{
				// FIXME: this statement could be optimized to a single ASM instruction (instead of 4)
				// Either BTR (AKA bit test and reset). Depends on the previous instruction.
				// Or BLSR (AKA Reset Lowest Set Bit). No dependency but require BMI1 (basically a recent CPU)
				p ^= 1U << j;

				e[j] = m[j].InsertFront(s);
			}
		}
	}
}

void GSTextureCache::SourceMap::RemoveAll()
{
	for (auto s : m_surfaces) delete s;

	m_surfaces.clear();

	for(size_t i = 0; i < countof(m_map); i++)
	{
		m_map[i].clear();
	}
}

void GSTextureCache::SourceMap::RemoveAt(Source* s)
{
	m_surfaces.erase(s);

	GL_CACHE("TC: Remove Src Texture: %d (0x%X)",
				s->m_texture ? s->m_texture->GetID() : 0,
				s->m_TEX0.TBP0);

	for(size_t i = 0; i < countof(m_pages); i++)
	{
		if(uint32 p = s->m_pages_as_bit[i])
		{
			auto* m = &m_map[i << 5];
			const auto* e = &s->m_erase_it[i << 5];

			unsigned long j;

			while(_BitScanForward(&j, p))
			{
				// FIXME: this statement could be optimized to a single ASM instruction (instead of 4)
				// Either BTR (AKA bit test and reset). Depends on the previous instruction.
				// Or BLSR (AKA Reset Lowest Set Bit). No dependency but require BMI1 (basically a recent CPU)
				p ^= 1U << j;

				m[j].EraseIndex(e[j]);
			}
		}
	}

	delete s;
}

// GSTextureCache::Palette

GSTextureCache::Palette::Palette(const GSRenderer* renderer, const uint16 pal, const bool need_gs_texture)
	: m_pal(pal)
	, m_tex_palette(nullptr)
	, m_renderer(renderer)
{
	uint16 palette_size = pal * sizeof(uint32);
	m_clut = (uint32*)_aligned_malloc(palette_size, 64);
	memcpy(m_clut, (const uint32*)m_renderer->m_mem.m_clut, palette_size);
	if (need_gs_texture) {
		InitializeTexture();
	}
}

GSTextureCache::Palette::~Palette() {
	m_renderer->m_dev->Recycle(m_tex_palette);
	_aligned_free(m_clut);
}

GSTexture* GSTextureCache::Palette::GetPaletteGSTexture() const {
	return m_tex_palette;
}

GSTextureCache::PaletteKey GSTextureCache::Palette::GetPaletteKey() const {
	return { m_clut, m_pal };
}

void GSTextureCache::Palette::InitializeTexture() {
	if (!m_tex_palette) {
		// A palette texture is always created with dimensions 256x1 (also in the case that m_pal is 16, thus a 16x1 texture
		// would be enough to store the CLUT data) because the coordinates that the shader uses for
		// sampling such texture are always normalized by 255.
		// This is because indexes are stored as normalized values of an RGBA texture (e.g. index 15 will be read as (15/255),
		// and therefore will read texel 15/255 * texture size).
		m_tex_palette = m_renderer->m_dev->CreateTexture(256, 1);
		m_tex_palette->Update(GSVector4i(0, 0, m_pal, 1), m_clut, m_pal * sizeof(m_clut[0]));
	}
}

// GSTextureCache::PaletteKeyHash

// Hashes the content of the clut.
// The hashing function is implemented by taking two things into account:
// 1) The clut can be an array of 16 or 256 uint32 (depending on the pal parameter) and in order to speed up the computation of the hash
//    the array is hashed in blocks of 16 uint32, so for clut of size 16 uint32 the hashing is computed in one pass and for clut of 256 uint32
//    it is computed in 16 passes,
// 2) The clut can contain many 0s, so as a way to increase the spread of hashing values for small changes in the input clut the hashing function
//    is using addition in combination with logical XOR operator; The addition constants are large prime numbers, which may help in achieving what intended.
std::size_t GSTextureCache::PaletteKeyHash::operator()(const PaletteKey &key) const {
	uint16 pal = key.pal;
	const uint32* clut = key.clut;

	ASSERT((pal & 15) == 0);

	size_t clut_hash = 3831179159;
	for (uint16 i = 0; i < pal; i += 16) {
		clut_hash = (clut_hash + 1488000301) ^ (clut[i     ] +   33644011);
		clut_hash = (clut_hash + 3831179159) ^ (clut[i +  1] +   47627467);
		clut_hash = (clut_hash + 3659574209) ^ (clut[i +  2] +  577038523);
		clut_hash = (clut_hash +   33644011) ^ (clut[i +  3] + 3491555267);

		clut_hash = (clut_hash +  777771959) ^ (clut[i +  4] + 3301075993);
		clut_hash = (clut_hash + 4019618579) ^ (clut[i +  5] + 4186992613);
		clut_hash = (clut_hash + 3465668953) ^ (clut[i +  6] + 3043435883);
		clut_hash = (clut_hash + 3494478943) ^ (clut[i +  7] + 3441897883);

		clut_hash = (clut_hash + 3432010979) ^ (clut[i +  8] + 2167922789);
		clut_hash = (clut_hash + 1570862863) ^ (clut[i +  9] + 3401920591);
		clut_hash = (clut_hash + 1002648679) ^ (clut[i + 10] + 1293530519);
		clut_hash = (clut_hash +  551381741) ^ (clut[i + 11] + 2539834039);

		clut_hash = (clut_hash + 3768974459) ^ (clut[i + 12] +  169943507);
		clut_hash = (clut_hash +  862380703) ^ (clut[i + 13] + 2906932549);
		clut_hash = (clut_hash + 3433082137) ^ (clut[i + 14] + 4234384109);
		clut_hash = (clut_hash + 2679083843) ^ (clut[i + 15] + 2719605247);
	}
	return clut_hash;
};

// GSTextureCache::PaletteKeyEqual

bool GSTextureCache::PaletteKeyEqual::operator()(const PaletteKey &lhs, const PaletteKey &rhs) const {
	if (lhs.pal != rhs.pal) {
		return false;
	}

	return GSVector4i::compare64(lhs.clut, rhs.clut, lhs.pal * sizeof(lhs.clut[0]));
};

// GSTextureCache::PaletteMap

GSTextureCache::PaletteMap::PaletteMap(const GSRenderer* renderer)
	: m_renderer(renderer)
{
	for (auto& map : m_maps) {
		map.reserve(MAX_SIZE);
	}
}

std::shared_ptr<GSTextureCache::Palette> GSTextureCache::PaletteMap::LookupPalette(const uint16 pal, const bool need_gs_texture) {
	ASSERT(pal == 16 || pal == 256);

	// Choose which hash map search into:
	//    pal == 16  : index 0
	//    pal == 256 : index 1
	auto& map = m_maps[pal == 16 ? 0 : 1];

	const uint32* clut = (const uint32*)m_renderer->m_mem.m_clut;

	// Create PaletteKey for searching into map (clut is actually not copied, so do not store this key into the map)
	PaletteKey palette_key = { clut, pal };

	auto it1 = map.find(palette_key);

	if (it1 != map.end()) {
		// Clut content match, HIT
		if (need_gs_texture && !it1->second->GetPaletteGSTexture()) {
			// Generate GSTexture and upload clut content if needed and not done yet
			it1->second->InitializeTexture();
		}
		return it1->second;
	}

	// No palette with matching clut content, MISS

	if (map.size() > MAX_SIZE) {
		// If the map is too big, try to clean it by disposing and removing unused palettes, before adding the new one
		GL_INS("WARNING, %u-bit PaletteMap (Size %u): Max size %u exceeded, clearing unused palettes.", pal * sizeof(uint32), map.size(), MAX_SIZE);

		uint32 current_size = map.size();

		for (auto it = map.begin(); it != map.end(); ) {
			// If the palette is unused, there is only one shared pointers holding a reference to the unused Palette object,
			// and this shared pointer is the one stored in the map itself
			if (it->second.use_count() <= 1) {
				// Palette is unused
				it = map.erase(it); // Erase element from map
				// The palette object should now be gone as the shared pointer to the object in the map is deleted
			}
			else {
				++it;
			}
		}

		uint32 cleared_palette_count = current_size - (uint32)map.size();

		if (cleared_palette_count == 0) {
			GL_INS("ERROR, %u-bit PaletteMap (Size %u): Max size %u exceeded, could not clear any palette, negative performance impact.", pal * sizeof(uint32), map.size(), MAX_SIZE);
		}
		else {
			map.reserve(MAX_SIZE); // Ensure map capacity is not modified by the clearing
			GL_INS("INFO, %u-bit PaletteMap (Size %u): Cleared %u palettes.", pal * sizeof(uint32), map.size(), cleared_palette_count);
		}
	}

	std::shared_ptr<Palette> palette = std::make_shared<Palette>(m_renderer, pal, need_gs_texture);
	
	map.emplace(palette->GetPaletteKey(), palette);

	GL_CACHE("TC, %u-bit PaletteMap (Size %u): Added new palette.", pal * sizeof(uint32), map.size());
	
	return palette;
}

void GSTextureCache::PaletteMap::Clear() {
	for (auto& map : m_maps) {
		map.clear(); // Clear all the nodes of the map, deleting Palette objects managed by shared pointers as they should be unused elsewhere
		map.reserve(MAX_SIZE); // Ensure map capacity is not modified by the clearing
	}
}
