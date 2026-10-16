#include "pch.h"
#include "RmlUiRendererDX9.h"
#include "RmlUiManager.h"

#include "mq/base/WString.h"
#include "eqlib/game/Globals.h"
#include "eqlib/game/UI.h"
#include "eqlib/game/UITextures.h"
#include "eqlib/graphics/GraphicsResources.h"

#if HAS_DIRECTX_9

#include <RmlUi/Core/Log.h>
#include <dxsdk-d3dx/d3dx9tex.h>

#include <charconv>
#include <cstring>
#include <memory>
#include <optional>

namespace mq::rmlui {
namespace {

std::optional<size_t> GetTextureByteSize(int width, int height,
	int maximumDimension, size_t maximumBytes)
{
	if (width <= 0 || height <= 0 || width > maximumDimension || height > maximumDimension)
		return std::nullopt;
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (pixels > maximumBytes / 4)
		return std::nullopt;
	return pixels * 4;
}

D3DMATRIX IdentityMatrix()
{
	D3DMATRIX matrix = {};
	matrix._11 = matrix._22 = matrix._33 = matrix._44 = 1.0f;
	return matrix;
}

D3DMATRIX Multiply(const D3DMATRIX& lhs, const D3DMATRIX& rhs)
{
	D3DMATRIX result = {};
	for (int row = 0; row < 4; ++row)
	{
		for (int column = 0; column < 4; ++column)
		{
			for (int i = 0; i < 4; ++i)
				result.m[row][column] += lhs.m[row][i] * rhs.m[i][column];
		}
	}
	return result;
}

enum class EqTextureKind
{
	Spell,
	Item,
};

struct EqTextureSource
{
	EqTextureKind kind;
	int iconId = 0;
};

std::optional<EqTextureSource> ParseEqTextureSource(std::string_view source)
{
	size_t separator = source.find("://");
	if (separator == std::string_view::npos)
		separator = source.find("|//");
	if (separator == std::string_view::npos || source.substr(0, separator) != "eq")
		return std::nullopt;

	const std::string_view path = source.substr(separator + 3);
	const size_t slash = path.find('/');
	if (slash == std::string_view::npos || slash == 0 || slash + 1 >= path.size())
		return std::nullopt;

	EqTextureKind kind;
	const std::string_view category = path.substr(0, slash);
	if (category == "spell")
		kind = EqTextureKind::Spell;
	else if (category == "item")
		kind = EqTextureKind::Item;
	else
		return std::nullopt;

	const std::string_view number = path.substr(slash + 1);
	int iconId = 0;
	const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), iconId);
	if (error != std::errc{} || end != number.data() + number.size() || iconId < 0 || iconId > 100000)
		return std::nullopt;
	if (kind == EqTextureKind::Item && iconId < 500)
		return std::nullopt;

	return EqTextureSource{ kind, iconId };
}

} // namespace

size_t RendererDX9::GetTextureBytes(RmlOwnerHandle owner) const
{
	const auto iter = m_ownerTextureBytes.find(owner);
	return iter != m_ownerTextureBytes.end() ? iter->second : 0;
}

bool RendererDX9::ShouldLogTextureWarning(std::string_view source)
{
	const auto now = std::chrono::steady_clock::now();
	auto& last = m_textureWarnings[std::string(source)];
	if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(5))
		return false;
	last = now;
	return true;
}

void RendererDX9::BeginFrame(int width, int height)
{
	m_statistics.drawCalls = 0;
	m_statistics.vertices = 0;
	m_statistics.triangles = 0;
	m_width = width;
	m_height = height;
	m_inFrame = m_device != nullptr && width > 0 && height > 0;
	if (!m_inFrame)
		return;

	D3DVIEWPORT9 viewport = { 0, 0, static_cast<DWORD>(width), static_cast<DWORD>(height), 0.0f, 1.0f };
	m_device->SetViewport(&viewport);
	m_device->SetPixelShader(nullptr);
	m_device->SetVertexShader(nullptr);
	m_device->SetFVF(VertexFormat);

	m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	m_device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
	m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
	m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	m_device->SetRenderState(D3DRS_CLIPPING, TRUE);
	m_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0f);
	m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
	m_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	m_device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
	m_device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
	m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	m_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	m_device->SetRenderState(D3DRS_STENCILMASK, 0xff);
	m_device->SetRenderState(D3DRS_STENCILWRITEMASK, 0xff);

	m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	m_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	m_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	m_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	m_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	const float w = static_cast<float>(width);
	const float h = static_cast<float>(height);
	m_projection = {};
	m_projection._11 = 2.0f / w;
	m_projection._22 = -2.0f / h;
	m_projection._33 = 1.0f / 20000.0f;
	m_projection._41 = -1.0f - 1.0f / w;
	m_projection._42 = 1.0f + 1.0f / h;
	m_projection._43 = 0.5f;
	m_projection._44 = 1.0f;

	const D3DMATRIX identity = IdentityMatrix();
	m_device->SetTransform(D3DTS_VIEW, &identity);
	m_device->SetTransform(D3DTS_PROJECTION, &m_projection);
	m_device->SetTransform(D3DTS_WORLD, &identity);
	m_hasTransform = false;
	m_stencilValue = 0;
}

void RendererDX9::EndFrame()
{
	m_inFrame = false;
}

Rml::CompiledGeometryHandle RendererDX9::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
	Rml::Span<const int> indices)
{
	auto geometry = std::make_unique<Geometry>();
	geometry->vertices.resize(vertices.size());
	for (size_t i = 0; i < vertices.size(); ++i)
	{
		const Rml::Vertex& source = vertices[i];
		Vertex& target = geometry->vertices[i];
		target.x = source.position.x;
		target.y = source.position.y;
		target.z = 0.0f;
		target.color = D3DCOLOR_ARGB(source.colour.alpha, source.colour.red,
			source.colour.green, source.colour.blue);
		target.u = source.tex_coord.x;
		target.v = source.tex_coord.y;
	}

	if (vertices.size() <= UINT16_MAX)
	{
		geometry->indices16.reserve(indices.size());
		for (int index : indices)
			geometry->indices16.push_back(static_cast<uint16_t>(index));
	}
	else
	{
		geometry->indices32.reserve(indices.size());
		for (int index : indices)
			geometry->indices32.push_back(static_cast<uint32_t>(index));
	}
	geometry->allocatedBytes = geometry->vertices.capacity() * sizeof(Vertex)
		+ geometry->indices16.capacity() * sizeof(uint16_t)
		+ geometry->indices32.capacity() * sizeof(uint32_t);
	m_statistics.geometryBytes += geometry->allocatedBytes;

	return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
}

void RendererDX9::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
	Rml::TextureHandle texture)
{
	if (!m_inFrame || !handle)
		return;

	ApplyWorld(translation);
	auto* allocation = reinterpret_cast<TextureAllocation*>(texture);
	auto* d3dTexture = allocation ? allocation->texture : nullptr;
	m_device->SetTexture(0, d3dTexture);
	m_device->SetTextureStageState(0, D3DTSS_COLOROP, d3dTexture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
	m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, d3dTexture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
	DrawGeometry(reinterpret_cast<const Geometry*>(handle), allocation);
}

void RendererDX9::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
	auto* compiled = reinterpret_cast<Geometry*>(geometry);
	if (compiled)
	{
		m_statistics.geometryBytes -= std::min(m_statistics.geometryBytes, compiled->allocatedBytes);
		delete compiled;
	}
}

Rml::TextureHandle RendererDX9::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source)
{
	if (!m_device || m_resourceRoot.empty())
		return {};
	if (source.starts_with("eq://") || source.starts_with("eq|//"))
		return LoadEqTexture(dimensions, source);

	const auto resolved = ResolveRmlUiPath(m_resourceRoot, m_pluginsRoot, m_luaRoot, source);
	if (!resolved)
		return {};

	const std::filesystem::path filePath = *resolved;
	const RmlOwnerHandle resourceOwner = m_resourceOwner != 0 ? m_resourceOwner
		: m_resourceOwnerResolver ? m_resourceOwnerResolver(source) : 0;
	D3DXIMAGE_INFO imageInfo = {};
	const HRESULT infoResult = D3DXGetImageInfoFromFileW(filePath.c_str(), &imageInfo);
	const auto allocationBytes = SUCCEEDED(infoResult)
		? GetTextureByteSize(static_cast<int>(imageInfo.Width), static_cast<int>(imageInfo.Height),
			m_maxTextureDimension, m_maxTextureBytesPerOwner)
		: std::nullopt;
	if (!allocationBytes)
	{
		if (ShouldLogTextureWarning(source))
			Rml::Log::Message(Rml::Log::LT_WARNING,
				"Rejected texture '%s': invalid image or dimensions exceed %dx%d",
				source.c_str(), m_maxTextureDimension, m_maxTextureDimension);
		return {};
	}
	const size_t ownerBytes = GetTextureBytes(resourceOwner);
	if (resourceOwner != 0 && ownerBytes > m_maxTextureBytesPerOwner - *allocationBytes)
	{
		if (ShouldLogTextureWarning(source))
			Rml::Log::Message(Rml::Log::LT_WARNING,
				"Rejected texture '%s': owner texture quota exceeded", source.c_str());
		return {};
	}

	IDirect3DTexture9* texture = nullptr;
	const HRESULT result = D3DXCreateTextureFromFileExW(m_device, filePath.c_str(),
		D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, 0, D3DFMT_A8R8G8B8,
		D3DPOOL_MANAGED, D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, &imageInfo,
		nullptr, &texture);
	if (FAILED(result))
	{
		Rml::Log::Message(Rml::Log::LT_WARNING, "Could not load texture '%s' (0x%08x)",
			source.c_str(), static_cast<unsigned int>(result));
		return {};
	}

	dimensions = { static_cast<int>(imageInfo.Width), static_cast<int>(imageInfo.Height) };
	return MakeTextureHandle(texture, *allocationBytes, resourceOwner);
}

Rml::TextureHandle RendererDX9::GenerateTexture(Rml::Span<const Rml::byte> source,
	Rml::Vector2i dimensions)
{
	const auto allocationBytes = GetTextureByteSize(dimensions.x, dimensions.y,
		m_maxTextureDimension, m_maxTextureBytesPerOwner);
	if (!allocationBytes)
		return {};
	const size_t ownerBytes = GetTextureBytes(m_resourceOwner);
	if (m_resourceOwner != 0 && ownerBytes > m_maxTextureBytesPerOwner - *allocationBytes)
		return {};

	IDirect3DTexture9* texture = CreateTexture(source.data(), dimensions.x, dimensions.y, false);
	return MakeTextureHandle(texture, texture ? *allocationBytes : 0, m_resourceOwner);
}

void RendererDX9::ReleaseTexture(Rml::TextureHandle texture)
{
	if (auto* allocation = reinterpret_cast<TextureAllocation*>(texture))
	{
		m_statistics.textureBytes -= std::min(m_statistics.textureBytes, allocation->bytes);
		if (allocation->bytes != 0)
		{
			auto ownerIter = m_ownerTextureBytes.find(allocation->owner);
			if (ownerIter != m_ownerTextureBytes.end())
			{
				ownerIter->second -= std::min(ownerIter->second, allocation->bytes);
				if (ownerIter->second == 0)
					m_ownerTextureBytes.erase(ownerIter);
			}
		}
		if (allocation->texture)
			allocation->texture->Release();
		delete allocation;
	}
}

Rml::TextureHandle RendererDX9::LoadEqTexture(Rml::Vector2i& dimensions, const Rml::String& source)
{
	const auto parsed = ParseEqTextureSource(source);
	if (!parsed || !eqlib::pSidlMgr || !eqlib::pEQSuiteTextureLoader)
		return {};

	const char* animationName = parsed->kind == EqTextureKind::Spell ? "A_SpellIcons" : "A_DragItem";
	eqlib::CTextureAnimation* sourceAnimation = eqlib::pSidlMgr->FindAnimation(animationName);
	if (!sourceAnimation || !sourceAnimation->IsGrid() || sourceAnimation->Frames.IsEmpty()
		|| sourceAnimation->CellWidth <= 0 || sourceAnimation->CellHeight <= 0)
		return {};

	eqlib::CTextureAnimation animation = *sourceAnimation;
	const int cell = parsed->kind == EqTextureKind::Item ? parsed->iconId - 500 : parsed->iconId;
	int cellCount = 0;
	for (int frameIndex = 0; frameIndex < animation.Frames.GetLength(); ++frameIndex)
	{
		const eqlib::CUITexturePiece& piece = animation.Frames[frameIndex].Piece;
		const eqlib::CXRect& pieceRect = piece.GetRect();
		const eqlib::CXSize& textureSize = piece.GetTextureInfo().TextureSize;
		const int width = pieceRect.right > pieceRect.left
			? pieceRect.right - pieceRect.left : textureSize.cx;
		const int height = pieceRect.bottom > pieceRect.top
			? pieceRect.bottom - pieceRect.top : textureSize.cy;
		cellCount += (width / animation.CellWidth) * (height / animation.CellHeight);
	}
	if (cell < 0 || cell >= cellCount)
	{
		if (ShouldLogTextureWarning(source))
			Rml::Log::Message(Rml::Log::LT_WARNING, "EQ icon index is outside atlas bounds: '%s'", source.c_str());
		return {};
	}
	animation.SetCurCell(cell);
	const int frameIndex = animation.GetCurFrame();
	if (frameIndex < 0 || frameIndex >= animation.Frames.GetLength())
		return {};

	const eqlib::CXRect& cellRect = animation.GetCurCellRect();
	const eqlib::CUITextureInfo& constTextureInfo = animation.Frames[frameIndex].Piece.GetTextureInfo();
	if (cellRect.left < 0 || cellRect.top < 0 || cellRect.right <= cellRect.left
		|| cellRect.bottom <= cellRect.top || cellRect.right > constTextureInfo.TextureSize.cx
		|| cellRect.bottom > constTextureInfo.TextureSize.cy)
	{
		if (ShouldLogTextureWarning(source))
			Rml::Log::Message(Rml::Log::LT_WARNING, "EQ icon is outside atlas bounds: '%s'", source.c_str());
		return {};
	}

	eqlib::CUITextureInfo textureInfo = constTextureInfo;
	if (static_cast<int>(textureInfo.TextureId) == -1)
		textureInfo.TextureId = eqlib::pEQSuiteTextureLoader->CreateTexture(textureInfo);
	if (static_cast<int>(textureInfo.TextureId) == -1)
		return {};

	eqlib::BMI* bitmap = eqlib::pEQSuiteTextureLoader->GetTexture(textureInfo);
	IDirect3DTexture9* texture = bitmap && bitmap->pBmp ? bitmap->pBmp->GetD3DTexture() : nullptr;
	if (!texture)
		return {};

	const float atlasWidth = static_cast<float>(textureInfo.TextureSize.cx);
	const float atlasHeight = static_cast<float>(textureInfo.TextureSize.cy);
	dimensions = { cellRect.right - cellRect.left, cellRect.bottom - cellRect.top };
	texture->AddRef();
	return MakeTextureHandle(texture, 0, 0,
		static_cast<float>(dimensions.x) / atlasWidth,
		static_cast<float>(dimensions.y) / atlasHeight,
		static_cast<float>(cellRect.left) / atlasWidth,
		static_cast<float>(cellRect.top) / atlasHeight);
}

Rml::TextureHandle RendererDX9::MakeTextureHandle(IDirect3DTexture9* texture, size_t bytes,
	RmlOwnerHandle owner, float uvScaleX, float uvScaleY, float uvOffsetX, float uvOffsetY)
{
	if (!texture)
		return {};
	auto allocation = std::make_unique<TextureAllocation>();
	allocation->texture = texture;
	allocation->bytes = bytes;
	allocation->owner = owner;
	allocation->uvScaleX = uvScaleX;
	allocation->uvScaleY = uvScaleY;
	allocation->uvOffsetX = uvOffsetX;
	allocation->uvOffsetY = uvOffsetY;
	if (bytes != 0)
		m_ownerTextureBytes[owner] += bytes;
	m_statistics.textureBytes += bytes;
	return reinterpret_cast<Rml::TextureHandle>(allocation.release());
}

void RendererDX9::EnableScissorRegion(bool enable)
{
	if (m_inFrame)
		m_device->SetRenderState(D3DRS_SCISSORTESTENABLE, enable ? TRUE : FALSE);
}

void RendererDX9::SetScissorRegion(Rml::Rectanglei region)
{
	if (!m_inFrame)
		return;

	RECT rectangle = {
		Rml::Math::Clamp(region.Left(), 0, m_width),
		Rml::Math::Clamp(region.Top(), 0, m_height),
		Rml::Math::Clamp(region.Right(), 0, m_width),
		Rml::Math::Clamp(region.Bottom(), 0, m_height),
	};
	m_device->SetScissorRect(&rectangle);
}

void RendererDX9::EnableClipMask(bool enable)
{
	if (m_inFrame)
		m_device->SetRenderState(D3DRS_STENCILENABLE, enable ? TRUE : FALSE);
}

void RendererDX9::RenderToClipMask(Rml::ClipMaskOperation operation,
	Rml::CompiledGeometryHandle handle, Rml::Vector2f translation)
{
	if (!m_inFrame || !handle)
		return;

	const bool clear = operation == Rml::ClipMaskOperation::Set
		|| operation == Rml::ClipMaskOperation::SetInverse;
	if (clear)
	{
		m_device->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f,
			operation == Rml::ClipMaskOperation::SetInverse ? 1 : 0);
	}

	m_device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
	m_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
	m_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	m_device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	m_device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);

	switch (operation)
	{
	case Rml::ClipMaskOperation::Set:
		m_device->SetRenderState(D3DRS_STENCILREF, 1);
		m_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
		m_stencilValue = 1;
		break;
	case Rml::ClipMaskOperation::SetInverse:
		m_device->SetRenderState(D3DRS_STENCILREF, 0);
		m_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
		m_stencilValue = 1;
		break;
	case Rml::ClipMaskOperation::Intersect:
		m_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		m_device->SetRenderState(D3DRS_STENCILREF, m_stencilValue);
		m_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCRSAT);
		++m_stencilValue;
		break;
	}

	ApplyWorld(translation);
	m_device->SetTexture(0, nullptr);
	m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
	m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	DrawGeometry(reinterpret_cast<const Geometry*>(handle));
	m_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0f);
	m_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
	m_device->SetRenderState(D3DRS_STENCILREF, m_stencilValue);
	m_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
}

void RendererDX9::SetTransform(const Rml::Matrix4f* transform)
{
	m_hasTransform = transform != nullptr;
	if (transform)
	{
		static_assert(sizeof(D3DMATRIX) == sizeof(float) * 16);
		std::memcpy(&m_transform, transform->data(), sizeof(D3DMATRIX));
	}
}

IDirect3DTexture9* RendererDX9::CreateTexture(const uint8_t* rgba, int width, int height,
	bool premultiply)
{
	if (!m_device || !rgba || width <= 0 || height <= 0)
		return nullptr;

	IDirect3DTexture9* texture = nullptr;
	if (FAILED(m_device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
		D3DPOOL_MANAGED, &texture, nullptr)))
	{
		return nullptr;
	}

	D3DLOCKED_RECT locked = {};
	if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
	{
		texture->Release();
		return nullptr;
	}

	for (int y = 0; y < height; ++y)
	{
		const uint8_t* source = rgba + static_cast<size_t>(y) * width * 4;
		uint8_t* target = static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
		for (int x = 0; x < width; ++x, source += 4, target += 4)
		{
			const uint8_t alpha = source[3];
			target[0] = premultiply ? static_cast<uint8_t>((source[2] * alpha + 127) / 255) : source[2];
			target[1] = premultiply ? static_cast<uint8_t>((source[1] * alpha + 127) / 255) : source[1];
			target[2] = premultiply ? static_cast<uint8_t>((source[0] * alpha + 127) / 255) : source[0];
			target[3] = alpha;
		}
	}

	texture->UnlockRect(0);
	return texture;
}

void RendererDX9::ApplyWorld(Rml::Vector2f translation)
{
	D3DMATRIX world = IdentityMatrix();
	world._41 = translation.x;
	world._42 = translation.y;
	if (m_hasTransform)
		world = Multiply(world, m_transform);
	m_device->SetTransform(D3DTS_WORLD, &world);
}

void RendererDX9::DrawGeometry(const Geometry* geometry, const TextureAllocation* texture)
{
	if (!geometry || geometry->vertices.empty())
		return;

	const Vertex* vertices = geometry->vertices.data();
	if (texture && (texture->uvScaleX != 1.0f || texture->uvScaleY != 1.0f
		|| texture->uvOffsetX != 0.0f || texture->uvOffsetY != 0.0f))
	{
		m_transformedVertices = geometry->vertices;
		for (Vertex& vertex : m_transformedVertices)
		{
			vertex.u = texture->uvOffsetX + vertex.u * texture->uvScaleX;
			vertex.v = texture->uvOffsetY + vertex.v * texture->uvScaleY;
		}
		vertices = m_transformedVertices.data();
	}

	const UINT vertexCount = static_cast<UINT>(geometry->vertices.size());
	m_statistics.drawCalls++;
	m_statistics.vertices += vertexCount;
	if (!geometry->indices16.empty())
	{
		m_statistics.triangles += geometry->indices16.size() / 3;
		m_device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertexCount,
			static_cast<UINT>(geometry->indices16.size() / 3), geometry->indices16.data(),
			D3DFMT_INDEX16, vertices, sizeof(Vertex));
	}
	else if (!geometry->indices32.empty())
	{
		m_statistics.triangles += geometry->indices32.size() / 3;
		m_device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vertexCount,
			static_cast<UINT>(geometry->indices32.size() / 3), geometry->indices32.data(),
			D3DFMT_INDEX32, vertices, sizeof(Vertex));
	}
}

} // namespace mq::rmlui

#endif // HAS_DIRECTX_9
