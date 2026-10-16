#pragma once

#if HAS_DIRECTX_9

#include <RmlUi/Core/RenderInterface.h>
#include <d3d9.h>

#include <mq/api/RmlUi.h>

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mq::rmlui {

class RendererDX9 : public Rml::RenderInterface
{
public:
	struct Statistics
	{
		uint64_t drawCalls = 0;
		uint64_t vertices = 0;
		uint64_t triangles = 0;
		size_t geometryBytes = 0;
		size_t textureBytes = 0;
	};

	void SetDevice(IDirect3DDevice9* device) { m_device = device; }
	void SetResourceRoot(std::filesystem::path resourceRoot, std::filesystem::path pluginsRoot, std::filesystem::path luaRoot)
	{
		m_resourceRoot = std::move(resourceRoot);
		m_pluginsRoot = std::move(pluginsRoot);
		m_luaRoot = std::move(luaRoot);
	}
	void BeginFrame(int width, int height);
	void EndFrame();
	void SetResourceOwner(RmlOwnerHandle owner) { m_resourceOwner = owner; }
	void SetTextureLimits(int maximumDimension, size_t maximumBytesPerOwner)
	{
		m_maxTextureDimension = maximumDimension;
		m_maxTextureBytesPerOwner = maximumBytesPerOwner;
	}
	void SetResourceOwnerResolver(std::function<RmlOwnerHandle(std::string_view)> resolver)
	{
		m_resourceOwnerResolver = std::move(resolver);
	}
	const Statistics& GetStatistics() const { return m_statistics; }
	size_t GetTextureBytes(RmlOwnerHandle owner) const;

	Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
		Rml::Span<const int> indices) override;
	void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
		Rml::TextureHandle texture) override;
	void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

	Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
	Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
		Rml::Vector2i dimensions) override;
	void ReleaseTexture(Rml::TextureHandle texture) override;

	void EnableScissorRegion(bool enable) override;
	void SetScissorRegion(Rml::Rectanglei region) override;
	void EnableClipMask(bool enable) override;
	void RenderToClipMask(Rml::ClipMaskOperation operation,
		Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation) override;
	void SetTransform(const Rml::Matrix4f* transform) override;

private:
	struct Vertex
	{
		float x, y, z;
		D3DCOLOR color;
		float u, v;
	};

	struct Geometry
	{
		std::vector<Vertex> vertices;
		std::vector<uint16_t> indices16;
		std::vector<uint32_t> indices32;
		size_t allocatedBytes = 0;
	};

	struct TextureAllocation
	{
		IDirect3DTexture9* texture = nullptr;
		size_t bytes = 0;
		RmlOwnerHandle owner = 0;
		float uvScaleX = 1.0f;
		float uvScaleY = 1.0f;
		float uvOffsetX = 0.0f;
		float uvOffsetY = 0.0f;
	};

	static constexpr DWORD VertexFormat = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

	IDirect3DTexture9* CreateTexture(const uint8_t* rgba, int width, int height, bool premultiply);
	Rml::TextureHandle LoadEqTexture(Rml::Vector2i& dimensions, const Rml::String& source);
	Rml::TextureHandle MakeTextureHandle(IDirect3DTexture9* texture, size_t bytes,
		RmlOwnerHandle owner, float uvScaleX = 1.0f, float uvScaleY = 1.0f,
		float uvOffsetX = 0.0f, float uvOffsetY = 0.0f);
	bool ShouldLogTextureWarning(std::string_view source);
	void ApplyWorld(Rml::Vector2f translation);
	void DrawGeometry(const Geometry* geometry, const TextureAllocation* texture = nullptr);

	IDirect3DDevice9* m_device = nullptr;
	D3DMATRIX m_projection = {};
	D3DMATRIX m_transform = {};
	bool m_hasTransform = false;
	bool m_inFrame = false;
	int m_width = 0;
	int m_height = 0;
	DWORD m_stencilValue = 0;
	std::filesystem::path m_resourceRoot;
	std::filesystem::path m_pluginsRoot;
	std::filesystem::path m_luaRoot;
	RmlOwnerHandle m_resourceOwner = 0;
	int m_maxTextureDimension = 4096;
	size_t m_maxTextureBytesPerOwner = 64 * 1024 * 1024;
	std::function<RmlOwnerHandle(std::string_view)> m_resourceOwnerResolver;
	Statistics m_statistics;
	std::unordered_map<RmlOwnerHandle, size_t> m_ownerTextureBytes;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_textureWarnings;
	std::vector<Vertex> m_transformedVertices;
};

} // namespace mq::rmlui

#endif // HAS_DIRECTX_9
