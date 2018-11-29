/*
Copyright(c) 2016-2018 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES ==============================
#include "Renderer.h"
#include "Rectangle.h"
#include "Grid.h"
#include "Font.h"
#include "Deferred/ShaderVariation.h"
#include "Deferred/LightShader.h"
#include "Deferred/GBuffer.h"
#include "../RHI/RHI_Device.h"
#include "../RHI/RHI_CommonBuffers.h"
#include "../RHI/RHI_VertexBuffer.h"
#include "../RHI/RHI_Sampler.h"
#include "../RHI/RHI_Pipeline.h"
#include "../RHI/RHI_RenderTexture.h"
#include "../RHI/RHI_Shader.h"
#include "../World/Actor.h"
#include "../World/Components/Transform.h"
#include "../World/Components/Renderable.h"
#include "../World/Components/Skybox.h"
#include "../Physics/Physics.h"
#include "../Physics/PhysicsDebugDraw.h"
#include "../Profiling/Profiler.h"
#include "../Core/Context.h"
#include "../Math/BoundingBox.h"
#include "../RHI/RHI_ConstantBuffer.h"
//=========================================

//= NAMESPACES ================
using namespace std;
using namespace Directus::Math;
using namespace Helper;
//=============================

#define GIZMO_MAX_SIZE 5.0f
#define GIZMO_MIN_SIZE 0.1f

namespace TAA_Sequence
{
	inline float Halton(int index, int base)
	{
		float f = 1; float r = 0;
		while (index > 0)
		{
			f = f / (float)base;
			r = r + f * (index % base);
			index = index / base;
		}
		return r;
	}

	inline Vector2 Halton2D(int index, int baseA, int baseB)
	{
		return Vector2(Halton(index, baseA), Halton(index, baseB));
	}
}

namespace Directus
{
	static ResourceManager* g_resourceMng = nullptr;
	bool Renderer::m_isRendering = false;
	Renderer::Renderer(Context* context, void* drawHandle) : Subsystem(context)
	{
		
		m_nearPlane		= 0.0f;
		m_farPlane		= 0.0f;
		m_camera		= nullptr;
		m_rhiDevice		= nullptr;
		m_frameNum		= 0;
		m_flags			= 0;
		m_flags			|= Render_Physics;
		m_flags			|= Render_SceneGrid;
		m_flags			|= Render_Light;
		m_flags			|= Render_Bloom;
		m_flags			|= Render_FXAA;
		m_flags			|= Render_SSDO;
		m_flags			|= Render_SSR;
		//m_flags			|= Render_TAA;
		m_flags			|= Render_Correction;
		//m_flags		|= Render_Sharpening;
		//m_flags		|= Render_ChromaticAberration;

		// Create RHI device
		m_rhiDevice		= make_shared<RHI_Device>(drawHandle);
		m_rhiPipeline	= make_shared<RHI_Pipeline>(m_rhiDevice);

		// Subscribe to events
		SUBSCRIBE_TO_EVENT(EVENT_RENDER, EVENT_HANDLER(Render));
		SUBSCRIBE_TO_EVENT(EVENT_WORLD_SUBMIT, EVENT_HANDLER_VARIANT(Renderables_Acquire));
	}

	Renderer::~Renderer()
	{
		m_actors.clear();
		m_camera = nullptr;
	}

	bool Renderer::Initialize()
	{
		// Create/Get required systems		
		g_resourceMng		= m_context->GetSubsystem<ResourceManager>();

		// Get standard resource directories
		string fontDir			= g_resourceMng->GetStandardResourceDirectory(Resource_Font);
		string shaderDirectory	= g_resourceMng->GetStandardResourceDirectory(Resource_Shader);
		string textureDirectory = g_resourceMng->GetStandardResourceDirectory(Resource_Texture);

		m_viewport = make_shared<RHI_Viewport>();
		// Load a font (used for performance metrics)
		m_font = make_unique<Font>(m_context, fontDir + "CalibriBold.ttf", 12, Vector4(0.7f, 0.7f, 0.7f, 1.0f));
		// Make a grid (used in editor)
		m_grid = make_unique<Grid>(m_rhiDevice);
		// Light gizmo icon rectangle
		m_gizmoRectLight = make_unique<Rectangle>(m_context);
		// Create a constant buffer that will be used for most shaders
		m_bufferGlobal = make_shared<RHI_ConstantBuffer>(m_rhiDevice);
		m_bufferGlobal->Create(sizeof(ConstantBuffer_Global));

		CreateRenderTextures(Settings::Get().Resolution_GetWidth(), Settings::Get().Resolution_GetHeight());

		// SAMPLERS
		{
			m_samplerPointClampAlways		= make_shared<RHI_Sampler>(m_rhiDevice, Texture_Sampler_Point,			Texture_Address_Clamp,	Texture_Comparison_Always);
			m_samplerPointClampGreater		= make_shared<RHI_Sampler>(m_rhiDevice, Texture_Sampler_Point,			Texture_Address_Clamp,	Texture_Comparison_GreaterEqual);
			m_samplerBilinearClampGreater	= make_shared<RHI_Sampler>(m_rhiDevice, Texture_Sampler_Bilinear,		Texture_Address_Clamp,	Texture_Comparison_GreaterEqual);
			m_samplerBilinearWrapGreater	= make_shared<RHI_Sampler>(m_rhiDevice, Texture_Sampler_Bilinear,		Texture_Address_Wrap,	Texture_Comparison_GreaterEqual);
			m_samplerBilinearClampAlways	= make_shared<RHI_Sampler>(m_rhiDevice, Texture_Sampler_Bilinear,		Texture_Address_Clamp,	Texture_Comparison_Always);
			m_samplerAnisotropicWrapAlways	= make_shared<RHI_Sampler>(m_rhiDevice, Texture_Sampler_Anisotropic,	Texture_Address_Wrap,	Texture_Comparison_Always);	
		}

		// SHADERS
		{
			// Light
			m_shaderLight = make_shared<LightShader>(m_rhiDevice);
			m_shaderLight->Compile(shaderDirectory + "Light.hlsl", m_context);

			// Transparent
			m_shaderTransparent = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderTransparent->Compile_VertexPixel(shaderDirectory + "Transparent.hlsl", Input_PositionTextureTBN, m_context);
			m_shaderTransparent->AddBuffer<Struct_Transparency>();

			// Depth
			m_shaderLightDepth = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderLightDepth->Compile_VertexPixel(shaderDirectory + "ShadowingDepth.hlsl", Input_Position, m_context);
			m_shaderLightDepth->AddBuffer<Struct_Matrix_Matrix_Float>();

			// Font
			m_shaderFont = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderFont->Compile_VertexPixel(shaderDirectory + "Font.hlsl", Input_PositionTexture, m_context);
			m_shaderFont->AddBuffer<Struct_Matrix_Vector4>();

			// Transformation gizmo
			m_shaderTransformationGizmo = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderTransformationGizmo->Compile_VertexPixel(shaderDirectory + "TransformationGizmo.hlsl", Input_PositionTextureTBN, m_context);
			m_shaderTransformationGizmo->AddBuffer<Struct_Matrix_Vector3_Vector3>();

			// SSDO
			m_shaderSSDO = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderSSDO->Compile_VertexPixel(shaderDirectory + "SSDO.hlsl", Input_PositionTexture, m_context);
			m_shaderSSDO->AddBuffer<Struct_Matrix_Matrix_Vector2>();

			// Shadow mapping
			m_shaderShadowMapping = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderShadowMapping->Compile_VertexPixel(shaderDirectory + "ShadowMapping.hlsl", Input_PositionTexture, m_context);
			m_shaderShadowMapping->AddBuffer<Struct_ShadowMapping>();

			// Line
			m_shaderLine = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderLine->Compile_VertexPixel(shaderDirectory + "Line.hlsl", Input_PositionColor, m_context);
			m_shaderLine->AddBuffer<Struct_Matrix_Matrix>();

			// Texture
			m_shaderTexture = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderTexture->AddDefine("PASS_TEXTURE");
			m_shaderTexture->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// FXAA
			m_shaderFXAA = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderFXAA->AddDefine("PASS_FXAA");
			m_shaderFXAA->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Luma
			m_shaderLuma = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderLuma->AddDefine("PASS_LUMA");
			m_shaderLuma->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Sharpening
			m_shaderSharpening = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderSharpening->AddDefine("PASS_SHARPENING");
			m_shaderSharpening->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Chromatic aberration
			m_shaderChromaticAberration = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderChromaticAberration->AddDefine("PASS_CHROMATIC_ABERRATION");
			m_shaderChromaticAberration->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Blur Box
			m_shaderBlurBox = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderBlurBox->AddDefine("PASS_BLUR_BOX");
			m_shaderBlurBox->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Blur Gaussian Horizontal
			m_shaderBlurGaussian = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderBlurGaussian->AddDefine("PASS_BLUR_GAUSSIAN");
			m_shaderBlurGaussian->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Blur Bilateral Gaussian Horizontal
			m_shaderBlurBilateralGaussian = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderBlurBilateralGaussian->AddDefine("PASS_BLUR_BILATERAL_GAUSSIAN");
			m_shaderBlurBilateralGaussian->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Bloom - bright
			m_shaderBloom_Bright = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderBloom_Bright->AddDefine("PASS_BRIGHT");
			m_shaderBloom_Bright->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Bloom - blend
			m_shaderBloom_BlurBlend = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderBloom_BlurBlend->AddDefine("PASS_BLEND_ADDITIVE");
			m_shaderBloom_BlurBlend->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// Tone-mapping
			m_shaderCorrection = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderCorrection->AddDefine("PASS_CORRECTION");
			m_shaderCorrection->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);

			// TAA
			m_shaderTAA = make_shared<RHI_Shader>(m_rhiDevice);
			m_shaderTAA->AddDefine("PASS_TAA_RESOLVE");
			m_shaderTAA->Compile_VertexPixel(shaderDirectory + "Quad.hlsl", Input_PositionTexture, m_context);
		}

		// PIPELINE STATES
		{
			m_pipelineLine.primitiveTopology	= PrimitiveTopology_LineList;
			m_pipelineLine.cullMode				= Cull_Back;
			m_pipelineLine.fillMode				= Fill_Solid;
			m_pipelineLine.vertexShader			= m_shaderLine;
			m_pipelineLine.pixelShader			= m_shaderLine;
			m_pipelineLine.constantBuffer		= m_shaderLine->GetConstantBuffer();
			m_pipelineLine.sampler				= m_samplerPointClampGreater;
		}

		// TEXTURES
		{
			// Noise texture (used by SSDO shader)
			m_texNoiseNormal = make_shared<RHI_Texture>(m_context);
			m_texNoiseNormal->LoadFromFile(textureDirectory + "noise.png");

			m_texWhite = make_shared<RHI_Texture>(m_context);
			m_texWhite->LoadFromFile(textureDirectory + "white.png");

			m_texBlack = make_shared<RHI_Texture>(m_context);
			m_texBlack->LoadFromFile(textureDirectory + "black.png");

			// Gizmo icons
			m_gizmoTexLightDirectional = make_shared<RHI_Texture>(m_context);
			m_gizmoTexLightDirectional->LoadFromFile(textureDirectory + "sun.png");

			m_gizmoTexLightPoint = make_shared<RHI_Texture>(m_context);
			m_gizmoTexLightPoint->LoadFromFile(textureDirectory + "light_bulb.png");

			m_gizmoTexLightSpot = make_shared<RHI_Texture>(m_context);
			m_gizmoTexLightSpot->LoadFromFile(textureDirectory + "flashlight.png");
		}

		return true;
	}

	void Renderer::SetBackBufferAsRenderTarget(bool clear /*= true*/)
	{
		m_rhiDevice->Set_BackBufferAsRenderTarget();
		m_viewport->SetWidth((float)Settings::Get().Resolution_GetWidth());
		m_viewport->SetHeight((float)Settings::Get().Resolution_GetHeight());
		m_rhiPipeline->SetViewport(m_viewport);
		m_rhiPipeline->Bind();
		if (clear) m_rhiDevice->ClearBackBuffer(m_camera ? m_camera->GetClearColor() : Vector4(0, 0, 0, 1));
	}

	void* Renderer::GetFrameShaderResource()
	{
		return m_renderTexFull_FinalFrame ? m_renderTexFull_FinalFrame->GetShaderResource() : nullptr;
	}

	void Renderer::Present()
	{
		m_rhiDevice->Present();
	}

	void Renderer::Render()
	{
		if (!m_rhiDevice || !m_rhiDevice->IsInitialized())
			return;

		if (!m_camera)
		{
			m_rhiDevice->ClearBackBuffer(Vector4(0.0f, 0.0f, 0.0f, 1.0f));
			return;
		}

		// Get camera matrices
		{
			m_nearPlane						= m_camera->GetNearPlane();
			m_farPlane						= m_camera->GetFarPlane();
			m_view							= m_camera->GetViewMatrix();
			m_viewBase						= m_camera->GetBaseViewMatrix();
			m_projection					= m_camera->GetProjectionMatrix();
			m_viewProjection				= m_view * m_projection;
			m_projectionOrthographic		= Matrix::CreateOrthographicLH((float)Settings::Get().Resolution_GetWidth(), (float)Settings::Get().Resolution_GetHeight(), m_nearPlane, m_farPlane);		
			m_viewProjection_Orthographic	= m_viewBase * m_projectionOrthographic;
		}

		// If there is nothing to render clear to camera's color and present
		if (m_actors.empty())
		{
			m_rhiDevice->ClearBackBuffer(m_camera->GetClearColor());
			m_rhiDevice->Present();
			m_isRendering = false;
			return;
		}

		TIME_BLOCK_START_MULTI();
		m_isRendering = true;
		Profiler::Get().Reset();
		m_frameNum++;
		m_isOddFrame = (m_frameNum % 2) == 1;

		Pass_DepthDirectionalLight(GetLightDirectional());
		
		Pass_GBuffer();

		Pass_PreLight(
			m_renderTexHalf_Spare,		// IN:	
			m_renderTexHalf_Shadows,	// OUT: Shadows
			m_renderTexHalf_SSDO		// OUT: DO
		);

		Pass_Light(
			m_renderTexHalf_Shadows,	// IN:	Shadows
			m_renderTexHalf_SSDO,		// IN:	SSDO
			m_renderTexFull_Light		// Out: Result
		);

		Pass_Transparent(m_renderTexFull_Light);
		
		Pass_PostLight(
			m_renderTexFull_Light,		// IN:	Light pass result
			m_renderTexFull_FinalFrame	// OUT: Result
		);
	
		Pass_GBufferVisualize(m_renderTexFull_FinalFrame);
		Pass_Lines(m_renderTexFull_FinalFrame);
		Pass_Gizmos(m_renderTexFull_FinalFrame);
		Pass_PerformanceMetrics(m_renderTexFull_FinalFrame);

		m_isRendering = false;
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::SetBackBufferSize(unsigned int width, unsigned int height)
	{
		if (width == 0 || height == 0)
			return;

		m_rhiDevice->Set_Resolution(width, height);
		m_viewport->SetWidth((float)width);
		m_viewport->SetHeight((float)height);
		m_rhiPipeline->SetViewport(m_viewport);
		m_rhiPipeline->Bind();
	}

	void Renderer::SetResolution(unsigned int width, unsigned int height)
	{
		// Return if resolution is invalid
		if (width == 0 || height == 0)
		{
			LOG_WARNING("Renderer::SetResolutionInternal: Invalid resolution");
			return;
		}

		// Return if resolution already set
		if (Settings::Get().Resolution_Get().x == width && Settings::Get().Resolution_Get().y == height)
			return;

		// Make sure we are pixel perfect
		width	-= (width	% 2 != 0) ? 1 : 0;
		height	-= (height	% 2 != 0) ? 1 : 0;

		Settings::Get().Resolution_Set(Vector2((float)width, (float)height));
		CreateRenderTextures(width, height);
		LOGF_INFO("Renderer::SetResolution: Resolution was set to %dx%d", width, height);
	}

	void Renderer::AddBoundigBox(const BoundingBox& box, const Vector4& color)
	{
		// Compute points from min and max
		Vector3 boundPoint1 = box.GetMin();
		Vector3 boundPoint2 = box.GetMax();
		Vector3 boundPoint3 = Vector3(boundPoint1.x, boundPoint1.y, boundPoint2.z);
		Vector3 boundPoint4 = Vector3(boundPoint1.x, boundPoint2.y, boundPoint1.z);
		Vector3 boundPoint5 = Vector3(boundPoint2.x, boundPoint1.y, boundPoint1.z);
		Vector3 boundPoint6 = Vector3(boundPoint1.x, boundPoint2.y, boundPoint2.z);
		Vector3 boundPoint7 = Vector3(boundPoint2.x, boundPoint1.y, boundPoint2.z);
		Vector3 boundPoint8 = Vector3(boundPoint2.x, boundPoint2.y, boundPoint1.z);

		// top of rectangular cuboid (6-2-8-4)
		AddLine(boundPoint6, boundPoint2, color);
		AddLine(boundPoint2, boundPoint8, color);
		AddLine(boundPoint8, boundPoint4, color);
		AddLine(boundPoint4, boundPoint6, color);

		// bottom of rectangular cuboid (3-7-5-1)
		AddLine(boundPoint3, boundPoint7, color);
		AddLine(boundPoint7, boundPoint5, color);
		AddLine(boundPoint5, boundPoint1, color);
		AddLine(boundPoint1, boundPoint3, color);

		// legs (6-3, 2-7, 8-5, 4-1)
		AddLine(boundPoint6, boundPoint3, color);
		AddLine(boundPoint2, boundPoint7, color);
		AddLine(boundPoint8, boundPoint5, color);
		AddLine(boundPoint4, boundPoint1, color);
	}

	void Renderer::AddLine(const Vector3& from, const Vector3& to, const Vector4& colorFrom, const Vector4& colorTo)
	{
		m_lineVertices.emplace_back(from, colorFrom);
		m_lineVertices.emplace_back(to, colorTo);
	}

	void Renderer::CreateRenderTextures(unsigned int width, unsigned int height)
	{
		// Resize everything
		m_gbuffer = make_unique<GBuffer>(m_rhiDevice, width, height);
		m_quad = make_unique<Rectangle>(m_context);
		m_quad->Create(0, 0, (float)width, (float)height);

		// Full res
		m_renderTexFull_Light		= make_unique<RHI_RenderTexture>(m_rhiDevice, width, height, Texture_Format_R16G16B16A16_FLOAT);
		m_renderTexFull_TAA_Current	= make_unique<RHI_RenderTexture>(m_rhiDevice, width, height, Texture_Format_R16G16B16A16_FLOAT);
		m_renderTexFull_TAA_History	= make_unique<RHI_RenderTexture>(m_rhiDevice, width, height, Texture_Format_R16G16B16A16_FLOAT);
		m_renderTexFull_FinalFrame	= make_unique<RHI_RenderTexture>(m_rhiDevice, width, height, Texture_Format_R16G16B16A16_FLOAT);

		// Half res
		m_renderTexHalf_Shadows = make_unique<RHI_RenderTexture>(m_rhiDevice, width / 2, height / 2, Texture_Format_R16G16B16A16_FLOAT);
		m_renderTexHalf_SSDO	= make_unique<RHI_RenderTexture>(m_rhiDevice, width / 2, height / 2, Texture_Format_R16G16B16A16_FLOAT);
		m_renderTexHalf_Spare	= make_unique<RHI_RenderTexture>(m_rhiDevice, width / 2, height / 2, Texture_Format_R16G16B16A16_FLOAT);

		// Quarter res
		m_renderTexQuarter_Blur1 = make_unique<RHI_RenderTexture>(m_rhiDevice, width / 4, height / 4, Texture_Format_R16G16B16A16_FLOAT);
		m_renderTexQuarter_Blur2 = make_unique<RHI_RenderTexture>(m_rhiDevice, width / 4, height / 4, Texture_Format_R16G16B16A16_FLOAT);
	}

	void Renderer::SetGlobalBuffer(const Matrix& mMVP, unsigned int resolutionWidth, unsigned int resolutionHeight, float blur_sigma, const Math::Vector2& blur_direction)
	{
		auto buffer = (ConstantBuffer_Global*)m_bufferGlobal->Map();

		buffer->mMVP					= mMVP;
		buffer->mView					= m_view;
		buffer->mProjection				= m_projection;
		buffer->camera_position			= m_camera->GetTransform()->GetPosition();
		buffer->camera_near				= m_camera->GetNearPlane();
		buffer->camera_far				= m_camera->GetFarPlane();
		buffer->resolution				= Vector2((float)resolutionWidth, (float)resolutionHeight);
		buffer->fxaa_subPixel			= m_fxaaSubPixel;
		buffer->fxaa_edgeThreshold		= m_fxaaEdgeThreshold;
		buffer->fxaa_edgeThresholdMin	= m_fxaaEdgeThresholdMin;
		buffer->blur_direction			= blur_direction;
		buffer->blur_sigma				= blur_sigma;
		buffer->bloom_intensity			= m_bloomIntensity;
		buffer->sharpen_strength		= m_sharpenStrength;
		buffer->sharpen_clamp			= m_sharpenClamp;

		m_bufferGlobal->Unmap();
		m_rhiPipeline->SetConstantBuffer(m_bufferGlobal, 0, Buffer_Global);
	}

	//= RENDERABLES ============================================================================================
	void Renderer::Renderables_Acquire(const Variant& actorsVariant)
	{
		TIME_BLOCK_START_CPU();

		// Clear previous state
		m_actors.clear();
		m_camera = nullptr;
		
		auto actorsVec = actorsVariant.Get<vector<shared_ptr<Actor>>>();
		for (const auto& actorShared : actorsVec)
		{
			auto actor = actorShared.get();
			if (!actor)
				continue;

			// Get all the components we are interested in
			auto renderable = actor->GetComponent<Renderable>();
			auto light		= actor->GetComponent<Light>();
			auto skybox		= actor->GetComponent<Skybox>();
			auto camera		= actor->GetComponent<Camera>();

			if (renderable)
			{
				bool isTransparent = !renderable->Material_Exists() ? false : renderable->Material_Ptr()->GetColorAlbedo().w < 1.0f;
				m_actors[isTransparent ? Renderable_ObjectTransparent : Renderable_ObjectOpaque].emplace_back(actor);
			}

			if (light)
			{
				m_actors[Renderable_Light].emplace_back(actor);
			}

			if (skybox)
			{
				m_actors[Renderable_Skybox].emplace_back(actor);
			}

			if (camera)
			{
				m_actors[Renderable_Camera].emplace_back(actor);
				m_camera = camera.get();
			}
		}

		Renderables_Sort(&m_actors[Renderable_ObjectOpaque]);
		Renderables_Sort(&m_actors[Renderable_ObjectTransparent]);

		TIME_BLOCK_END_CPU();
	}

	void Renderer::Renderables_Sort(vector<Actor*>* renderables)
	{
		if (renderables->size() <= 2)
			return;

		sort(renderables->begin(), renderables->end(),[](Actor* a, Actor* b)
		{
			// Get renderable component
			auto a_renderable = a->GetRenderable_PtrRaw();
			auto b_renderable = b->GetRenderable_PtrRaw();

			// Validate renderable components
			if (!a_renderable || !b_renderable)
				return false;

			// Get geometry parents
			auto a_geometryModel = a_renderable->Geometry_Model();
			auto b_geometryModel = b_renderable->Geometry_Model();

			// Validate geometry parents
			if (!a_geometryModel || !b_geometryModel)
				return false;

			// Get materials
			auto a_material = a_renderable->Material_Ptr();
			auto b_material = b_renderable->Material_Ptr();

			if (!a_material || !b_material)
				return false;

			// Get key for models
			auto a_keyModel = a_geometryModel->Resource_GetID();
			auto b_keyModel = b_geometryModel->Resource_GetID();

			// Get key for shaders
			auto a_keyShader = a_material->GetShader().lock()->Resource_GetID();
			auto b_keyShader = b_material->GetShader().lock()->Resource_GetID();

			// Get key for materials
			auto a_keyMaterial = a_material->Resource_GetID();
			auto b_keyMaterial = b_material->Resource_GetID();

			auto a_key = 
				(((unsigned long long)a_keyModel)		<< 48u)	| 
				(((unsigned long long)a_keyShader)		<< 32u)	|
				(((unsigned long long)a_keyMaterial)	<< 16u);

			auto b_key = 
				(((unsigned long long)b_keyModel)		<< 48u)	|
				(((unsigned long long)b_keyShader)		<< 32u)	|
				(((unsigned long long)b_keyMaterial)	<< 16u);
	
			return a_key < b_key;
		});
	}
	//==========================================================================================================

	//= PASSES =================================================================================================
	void Renderer::Pass_DepthDirectionalLight(Light* light)
	{
		if (!light || !light->GetCastShadows())
			return;

		TIME_BLOCK_START_MULTI();

		// Variables that help reduce state changes
		unsigned int currentlyBoundGeometry = 0;
		
		auto& actors = m_actors[Renderable_ObjectOpaque];

		if (!actors.empty())
		{
			m_rhiDevice->EventBegin("Pass_DepthDirectionalLight");
			m_rhiPipeline->SetShader(m_shaderLightDepth);
			m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);

			for (unsigned int i = 0; i < light->ShadowMap_GetCount(); i++)
			{
				if (auto shadowMap = light->ShadowMap_GetRenderTexture(i))
				{
					m_rhiPipeline->SetRenderTarget(shadowMap, shadowMap->GetDepthStencilView(), true);
					m_rhiPipeline->SetViewport(shadowMap->GetViewport());
				}

				for (const auto& actor : actors)
				{
					// Acquire renderable component
					auto renderable = actor->GetRenderable_PtrRaw();
					if (!renderable)
						continue;

					// Acquire material
					auto material = renderable->Material_Ptr();
					if (!material)
						continue;

					// Acquire geometry
					auto geometry = renderable->Geometry_Model();
					if (!geometry || !geometry->GetVertexBuffer() || !geometry->GetIndexBuffer())
						continue;

					// Skip meshes that don't cast shadows
					if (!renderable->GetCastShadows())
						continue;

					// Skip transparent meshes (for now)
					if (material->GetColorAlbedo().w < 1.0f)
						continue;

					// Bind geometry
					if (currentlyBoundGeometry != geometry->Resource_GetID())
					{
						m_rhiPipeline->SetIndexBuffer(geometry->GetIndexBuffer());
						m_rhiPipeline->SetVertexBuffer(geometry->GetVertexBuffer());					
						currentlyBoundGeometry = geometry->Resource_GetID();
					}

					auto worldView				= actor->GetTransform_PtrRaw()->GetMatrix() * light->GetViewMatrix();
					auto worldViewProjection	= worldView * light->ShadowMap_GetProjectionMatrix(i);
					auto buffer					= Struct_Matrix_Matrix_Float(worldView, worldViewProjection, m_camera->GetFarPlane());
					m_shaderLightDepth->UpdateBuffer(&buffer);
					m_rhiPipeline->SetConstantBuffer(m_shaderLightDepth->GetConstantBuffer(), 0, Buffer_Global);
					m_rhiPipeline->Bind();

					m_rhiDevice->DrawIndexed(renderable->Geometry_IndexCount(), renderable->Geometry_IndexOffset(), renderable->Geometry_VertexOffset());
				}
			}
			m_rhiDevice->EventEnd();
		}
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_GBuffer()
	{
		if (!m_rhiDevice)
			return;

		if (m_actors[Renderable_ObjectOpaque].empty())
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_GBuffer");

		// Set common states
		m_gbuffer->SetAsRenderTarget(m_rhiPipeline);
		m_rhiPipeline->SetSampler(m_samplerAnisotropicWrapAlways);
		m_rhiPipeline->SetFillMode(Fill_Solid);
		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		SetGlobalBuffer();

		// Variables that help reduce state changes
		bool vertexShaderBound				= false;
		unsigned int currentlyBoundGeometry = 0;
		unsigned int currentlyBoundShader	= 0;
		unsigned int currentlyBoundMaterial = 0;

		for (auto actor : m_actors[Renderable_ObjectOpaque])
		{
			// Get renderable and material
			Renderable* renderable	= actor->GetRenderable_PtrRaw();
			Material* material		= renderable ? renderable->Material_Ptr().get() : nullptr;

			if (!renderable || !material)
				continue;

			// Get shader and geometry
			auto shader	= material->GetShader().lock();
			auto model	= renderable->Geometry_Model();

			// Validate shader
			if (!shader || shader->GetState() != Shader_Built)
				continue;

			// Validate geometry
			if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
				continue;

			// Skip objects outside of the view frustum
			if (!m_camera->IsInViewFrustrum(renderable))
				continue;

			// set face culling (changes only if required)
			m_rhiPipeline->SetCullMode(material->GetCullMode());

			// Bind geometry
			if (currentlyBoundGeometry != model->Resource_GetID())
			{	
				m_rhiPipeline->SetIndexBuffer(model->GetIndexBuffer());
				m_rhiPipeline->SetVertexBuffer(model->GetVertexBuffer());
				currentlyBoundGeometry = model->Resource_GetID();
			}

			// Bind shader
			if (currentlyBoundShader != shader->Resource_GetID())
			{
				if (!vertexShaderBound)
				{
					m_rhiPipeline->SetVertexShader(shared_ptr<RHI_Shader>(shader));
					vertexShaderBound = true;
				}
				m_rhiPipeline->SetPixelShader(shared_ptr<RHI_Shader>(shader));
				currentlyBoundShader = shader->Resource_GetID();
			}

			// Bind textures
			if (currentlyBoundMaterial != material->Resource_GetID())
			{
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Albedo).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Roughness).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Metallic).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Normal).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Height).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Occlusion).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Emission).ptr_raw);
				m_rhiPipeline->SetTexture(material->GetTextureSlotByType(TextureType_Mask).ptr_raw);

				currentlyBoundMaterial = material->Resource_GetID();
			}

			// UPDATE PER OBJECT BUFFER
			shader->UpdatePerObjectBuffer(actor->GetTransform_PtrRaw(), material, m_view, m_projection);			
			m_rhiPipeline->SetConstantBuffer(shader->GetPerObjectBuffer(), 1, Buffer_Global);

			m_rhiPipeline->Bind();

			// Render	
			m_rhiDevice->DrawIndexed(renderable->Geometry_IndexCount(), renderable->Geometry_IndexOffset(), renderable->Geometry_VertexOffset());
			Profiler::Get().m_rendererMeshesRendered++;

		} // Actor/MESH ITERATION

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_PreLight(
		shared_ptr<RHI_RenderTexture>& texIn_Spare,
		shared_ptr<RHI_RenderTexture>& texOut_Shadows,
		shared_ptr<RHI_RenderTexture>& texOut_SSAO
	)
	{
		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_PreLight");

		m_rhiPipeline->SetIndexBuffer(m_quad->GetIndexBuffer());
		m_rhiPipeline->SetVertexBuffer(m_quad->GetVertexBuffer());
		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		
		// Shadow mapping + Blur
		if (auto lightDir = GetLightDirectional())
		{
			Pass_ShadowMapping(texIn_Spare, GetLightDirectional());
			float sigma			= 1.0f;
			float pixelStride	= 1.0f;
			Pass_BlurBilateralGaussian(texIn_Spare, texOut_Shadows, sigma, pixelStride);
		}

		// SSDO + Blur
		if (m_flags & Render_SSDO)
		{
			Pass_SSDO(texIn_Spare);
			float sigma			= 3.0f;
			float pixelStride	= 2.0f;
			Pass_BlurBilateralGaussian(texIn_Spare, texOut_SSAO, sigma, pixelStride);
		}

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_ShadowMapping(shared_ptr<RHI_RenderTexture>& texOut, Light* inDirectionalLight)
	{
		if (!inDirectionalLight)
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_Shadowing");

		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetShader(m_shaderShadowMapping);
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Normal));
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		m_rhiPipeline->SetTexture(inDirectionalLight->ShadowMap_GetRenderTexture(0));
		m_rhiPipeline->SetTexture(inDirectionalLight->ShadowMap_GetRenderTexture(1));
		m_rhiPipeline->SetTexture(inDirectionalLight->ShadowMap_GetRenderTexture(2));
		m_rhiPipeline->SetSampler(m_samplerPointClampGreater);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampGreater);
		auto buffer = Struct_ShadowMapping
		(
			m_viewProjection_Orthographic,
			(m_viewProjection).Inverted(),
			inDirectionalLight,
			m_camera
		);
		m_shaderShadowMapping->UpdateBuffer(&buffer);
		m_rhiPipeline->SetConstantBuffer(m_shaderShadowMapping->GetConstantBuffer(), 0, Buffer_Global);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_SSDO(shared_ptr<RHI_RenderTexture>& texOut)
	{
		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_SSDO");

		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetShader(m_shaderSSDO);
		m_rhiPipeline->SetTexture(m_renderTexFull_FinalFrame);
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Normal));
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		m_rhiPipeline->SetTexture(m_texNoiseNormal);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampGreater);	// SSDO (clamp)
		m_rhiPipeline->SetSampler(m_samplerBilinearWrapGreater);	// SSDO noise texture (wrap)
		auto buffer = Struct_Matrix_Matrix_Vector2
		(
			m_viewProjection_Orthographic,
			(m_viewProjection).Inverted(),
			Vector2(texOut->GetWidth(), texOut->GetHeight()),
			m_camera->GetFarPlane()
		);
		m_shaderSSDO->UpdateBuffer(&buffer);
		m_rhiPipeline->SetConstantBuffer(m_shaderSSDO->GetConstantBuffer(), 0, Buffer_Global);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_BlurBox(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut, float sigma)
	{
		m_rhiDevice->EventBegin("Pass_Blur");

		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight(), sigma);
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetShader(m_shaderBlurBox);
		m_rhiPipeline->SetTexture(texIn); // Shadows are in the alpha channel
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_BlurGaussian(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut, float sigma)
	{
		if (texIn->GetWidth() != texOut->GetWidth() ||
			texIn->GetHeight() != texOut->GetHeight() ||
			texIn->GetFormat() != texOut->GetFormat())
		{
			LOG_ERROR("Renderer::Pass_BlurGaussian: Invalid parameters, textures must match because they will get swapped");
			return;
		}

		m_rhiDevice->EventBegin("Pass_BlurGaussian");

		// Set common states
		m_rhiPipeline->SetViewport(texIn->GetViewport());

		// Horizontal Gaussian blur
		auto direction = Vector2(1.0f, 0.0f);
		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight(), sigma, direction);
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetShader(m_shaderBlurGaussian);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// Vertical Gaussian blur	
		direction = Vector2(0.0f, 1.0f);
		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight(), sigma, direction);
		m_rhiPipeline->SetRenderTarget(texIn);
		m_rhiPipeline->SetPixelShader(m_shaderBlurGaussian);
		m_rhiPipeline->SetTexture(texOut);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// Swap textures
		texIn.swap(texOut);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_BlurBilateralGaussian(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut, float sigma, float pixelStride)
	{
		if (texIn->GetWidth() != texOut->GetWidth() ||
			texIn->GetHeight() != texOut->GetHeight() ||
			texIn->GetFormat() != texOut->GetFormat())
		{
			LOG_ERROR("Renderer::Pass_BlurBilateralGaussian: Invalid parameters, textures must match because they will get swapped");
			return;
		}

		m_rhiDevice->EventBegin("Pass_BlurBilateralGaussian");

		// Set common states
		m_rhiPipeline->SetViewport(texIn->GetViewport());

		// Horizontal Gaussian blur
		auto direction = Vector2(pixelStride, 0.0f);
		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight(), sigma, direction);
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetShader(m_shaderBlurBilateralGaussian);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// Vertical Gaussian blur
		direction = Vector2(0.0f, pixelStride);
		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight(), sigma, direction);
		m_rhiPipeline->SetRenderTarget(texIn);
		m_rhiPipeline->SetPixelShader(m_shaderBlurBilateralGaussian);
		m_rhiPipeline->SetTexture(texOut);
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		texIn.swap(texOut);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_Light(shared_ptr<RHI_RenderTexture>& texShadows, shared_ptr<RHI_RenderTexture>& texSSDO, shared_ptr<RHI_RenderTexture>& texOut)
	{
		if (m_shaderLight->GetState() != Shader_Built)
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_Light");

		// TAA - Apply jitter to view projection matrix
		Matrix m_projectionJittered = m_viewProjection_Orthographic;
		if (Flags_IsSet(Render_TAA))
		{
			int samples				= 16;
			int index				= m_frameNum % samples;
			Vector2 jitter			= TAA_Sequence::Halton2D(index, 2, 3) * 2.0f - 1.0f;
			jitter.x				= jitter.x / (float)texOut->GetWidth();
			jitter.y				= jitter.y / (float)texOut->GetHeight();
			Matrix jitterMatrix		= Matrix::CreateTranslation(Vector3(jitter.x, -jitter.y, 0.0f));
			m_projectionJittered	= m_viewProjection_Orthographic * jitterMatrix;
		}

		// Update constant buffer
		m_shaderLight->UpdateConstantBuffer(
			m_projectionJittered,
			m_view,
			m_projection,
			m_actors[Renderable_Light],
			m_camera,
			Flags_IsSet(Render_SSR)
		);

		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetShader(shared_ptr<RHI_Shader>(m_shaderLight));
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Albedo));
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Normal));
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Material));
		m_rhiPipeline->SetTexture(texShadows);
		if (Flags_IsSet(Render_SSDO)) { m_rhiPipeline->SetTexture(texSSDO); } else { m_rhiPipeline->SetTexture(m_texBlack); }
		m_rhiPipeline->SetTexture(m_renderTexFull_FinalFrame); // SSR
		m_rhiPipeline->SetTexture(GetSkybox() ? GetSkybox()->GetTexture() : m_texWhite);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		m_rhiPipeline->SetSampler(m_samplerPointClampGreater);
		m_rhiPipeline->SetConstantBuffer(m_shaderLight->GetConstantBuffer(), 0, Buffer_Global);
		m_rhiPipeline->Bind();

		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_PostLight(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_PostLight");

		// All post-process passes share the following, so set them once here
		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetVertexBuffer(m_quad->GetVertexBuffer());
		m_rhiPipeline->SetIndexBuffer(m_quad->GetIndexBuffer());
		m_rhiPipeline->SetShader(m_shaderBloom_Bright);
		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight());

		// Render target swapping
		auto SwapTargets = [&texIn, &texOut]() { texOut.swap(texIn); };
		SwapTargets();

		// TAA	
		if (Flags_IsSet(Render_TAA))
		{
			SwapTargets();
			Pass_TAA(texIn, texOut);
		}

		// BLOOM
		if (Flags_IsSet(Render_Bloom))
		{
			SwapTargets();
			Pass_Bloom(texIn, texOut);
		}

		// CORRECTION
		if (Flags_IsSet(Render_Correction))
		{
			SwapTargets();
			Pass_Correction(texIn, texOut);
		}

		// FXAA
		if (Flags_IsSet(Render_FXAA))
		{
			SwapTargets();
			Pass_FXAA(texIn, texOut);
		}

		// SHARPENING
		if (Flags_IsSet(Render_Sharpening))
		{
			SwapTargets();
			Pass_Sharpening(texIn, texOut);
		}

		// CHROMATIC ABERRATION
		if (Flags_IsSet(Render_ChromaticAberration))
		{
			SwapTargets();
			Pass_ChromaticAberration(texIn, texOut);
		}

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_TAA(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_TAA");

		// Resolve
		SetGlobalBuffer(m_viewProjection_Orthographic, m_renderTexFull_TAA_Current->GetWidth(), m_renderTexFull_TAA_Current->GetHeight());
		m_rhiPipeline->SetRenderTarget(m_renderTexFull_TAA_Current);
		m_rhiPipeline->SetViewport(m_renderTexFull_TAA_Current->GetViewport());
		m_rhiPipeline->SetShader(m_shaderTAA);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		m_rhiPipeline->SetTexture(m_renderTexFull_TAA_History);
		m_rhiPipeline->SetTexture(texIn);
		//m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Velocity));
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// Output to texOut
		SetGlobalBuffer(m_viewProjection_Orthographic, texOut->GetWidth(), texOut->GetHeight());
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderTexture);
		m_rhiPipeline->SetSampler(m_samplerPointClampGreater);
		m_rhiPipeline->SetTexture(m_renderTexFull_TAA_Current);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// Swap textures so current becomes history
		m_renderTexFull_TAA_Current.swap(m_renderTexFull_TAA_History);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_Transparent(shared_ptr<RHI_RenderTexture>& texOut)
	{
		if (!GetLightDirectional())
			return;

		auto& actors_transparent = m_actors[Renderable_ObjectTransparent];
		if (actors_transparent.empty())
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_Transparent");

		m_rhiDevice->Set_AlphaBlendingEnabled(true);
		m_rhiPipeline->SetShader(m_shaderTransparent);
		m_rhiPipeline->SetRenderTarget(texOut, m_gbuffer->GetTexture(GBuffer_Target_Depth)->GetDepthStencilView());
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		m_rhiPipeline->SetTexture(GetSkybox() ? GetSkybox()->GetTexture() : nullptr);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampGreater);

		for (auto& actor : actors_transparent)
		{
			// Get renderable and material
			Renderable* renderable	= actor->GetRenderable_PtrRaw();
			Material* material		= renderable ? renderable->Material_Ptr().get() : nullptr;

			if (!renderable || !material)
				continue;

			// Get geometry
			auto model = renderable->Geometry_Model();
			if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
				continue;

			// Skip objects outside of the view frustum
			if (!m_camera->IsInViewFrustrum(renderable))
				continue;

			// Set the following per object
			m_rhiPipeline->SetCullMode(material->GetCullMode());
			m_rhiPipeline->SetIndexBuffer(model->GetIndexBuffer());
			m_rhiPipeline->SetVertexBuffer(model->GetVertexBuffer());

			// Constant buffer
			auto buffer = Struct_Transparency(
				actor->GetTransform_PtrRaw()->GetMatrix(),
				m_view,
				m_projection,
				material->GetColorAlbedo(),
				m_camera->GetTransform()->GetPosition(),
				GetLightDirectional()->GetDirection(),
				material->GetRoughnessMultiplier()
			);
			m_shaderTransparent->UpdateBuffer(&buffer);
			m_rhiPipeline->SetConstantBuffer(m_shaderTransparent->GetConstantBuffer(), 0, Buffer_Global);

			m_rhiPipeline->Bind();

			// Render	
			m_rhiDevice->DrawIndexed(renderable->Geometry_IndexCount(), renderable->Geometry_IndexOffset(), renderable->Geometry_VertexOffset());
			Profiler::Get().m_rendererMeshesRendered++;

		} // Actor/MESH ITERATION

		m_rhiDevice->Set_AlphaBlendingEnabled(false);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_Bloom(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		m_rhiDevice->EventBegin("Pass_Bloom");

		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		SetGlobalBuffer(m_viewProjection_Orthographic, texOut->GetWidth(), texOut->GetHeight());

		// Bright pass
		m_rhiPipeline->SetRenderTarget(m_renderTexQuarter_Blur1);
		m_rhiPipeline->SetViewport(m_renderTexQuarter_Blur1->GetViewport());
		m_rhiPipeline->SetShader(m_shaderBloom_Bright);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		float sigma = 2.0f;
		Pass_BlurGaussian(m_renderTexQuarter_Blur1, m_renderTexQuarter_Blur2, sigma);

		// Additive blending
		SetGlobalBuffer(m_viewProjection_Orthographic, texOut->GetWidth(), texOut->GetHeight());
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderBloom_BlurBlend);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->SetTexture(m_renderTexQuarter_Blur2);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_Correction(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		m_rhiDevice->EventBegin("Pass_Correction");

		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetSampler(m_samplerPointClampAlways);
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderCorrection);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_FXAA(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		m_rhiDevice->EventBegin("Pass_FXAA");

		// Common states
		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		SetGlobalBuffer(m_viewProjection_Orthographic, texIn->GetWidth(), texIn->GetHeight());

		// Luma
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderLuma);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// FXAA
		m_rhiPipeline->SetRenderTarget(texIn);
		m_rhiPipeline->SetViewport(texIn->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderFXAA);	
		m_rhiPipeline->SetTexture(texOut);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		// Swap the textures
		texIn.swap(texOut);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_ChromaticAberration(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		m_rhiDevice->EventBegin("Pass_ChromaticAberration");

		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderChromaticAberration);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_Sharpening(shared_ptr<RHI_RenderTexture>& texIn, shared_ptr<RHI_RenderTexture>& texOut)
	{
		m_rhiDevice->EventBegin("Pass_Sharpening");

		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		m_rhiPipeline->SetRenderTarget(texOut);
		m_rhiPipeline->SetViewport(texOut->GetViewport());
		m_rhiPipeline->SetPixelShader(m_shaderSharpening);
		m_rhiPipeline->SetTexture(texIn);
		m_rhiPipeline->Bind();

		m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

		m_rhiDevice->EventEnd();
	}

	void Renderer::Pass_Lines(shared_ptr<RHI_RenderTexture>& texOut)
	{
		bool drawPickingRay = m_flags & Render_PickingRay;
		bool drawAABBs		= m_flags & Render_AABB;
		bool drawGrid		= m_flags & Render_SceneGrid;
		bool draw			= drawPickingRay | drawAABBs | drawGrid;
		if (!draw)
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_Lines");

		m_rhiPipeline->SetState(m_pipelineLine);
		m_rhiDevice->Set_AlphaBlendingEnabled(true);
		m_rhiPipeline->SetRenderTarget(texOut, m_gbuffer->GetTexture(GBuffer_Target_Depth)->GetDepthStencilView());
		m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(GBuffer_Target_Depth));
		{
			// Picking ray
			if (drawPickingRay)
			{
				const Ray& ray = m_camera->GetPickingRay();
				AddLine(ray.GetOrigin(), ray.GetEnd(), Vector4(0, 1, 0, 1));
			}

			// bounding boxes
			if (drawAABBs)
			{
				for (const auto& actor : m_actors[Renderable_ObjectOpaque])
				{
					if (auto renderable = actor->GetRenderable_PtrRaw())
					{
						AddBoundigBox(renderable->Geometry_BB(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
					}
				}

				for (const auto& actor : m_actors[Renderable_ObjectTransparent])
				{
					if (auto renderable = actor->GetRenderable_PtrRaw())
					{
						AddBoundigBox(renderable->Geometry_BB(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
					}
				}
			}

			auto lineVertexBufferSize = (unsigned int)m_lineVertices.size();
			if (lineVertexBufferSize != 0)
			{
				if (lineVertexBufferSize > m_lineVertexCount)
				{
					m_lineVertexBuffer = make_shared<RHI_VertexBuffer>(m_rhiDevice);
					m_lineVertexBuffer->CreateDynamic(sizeof(RHI_Vertex_PosCol), lineVertexBufferSize);
					m_lineVertexCount = lineVertexBufferSize;
				}

				// Update line vertex buffer
				void* data = m_lineVertexBuffer->Map();
				memcpy(data, &m_lineVertices[0], sizeof(RHI_Vertex_PosCol) * lineVertexBufferSize);
				m_lineVertexBuffer->Unmap();

				// Set pipeline state
				m_rhiPipeline->SetVertexBuffer(m_lineVertexBuffer);
				auto buffer = Struct_Matrix_Matrix(m_view, m_projection);
				m_shaderLine->UpdateBuffer(&buffer);
				m_rhiPipeline->Bind();
				m_rhiDevice->Draw(lineVertexBufferSize);

				m_lineVertices.clear();
			}
		}
		
		// Grid
		if (drawGrid)
		{
			m_rhiPipeline->SetIndexBuffer(m_grid->GetIndexBuffer());
			m_rhiPipeline->SetVertexBuffer(m_grid->GetVertexBuffer());
			auto buffer = Struct_Matrix_Matrix(m_grid->ComputeWorldMatrix(m_camera->GetTransform()) * m_view, m_projection);
			m_shaderLine->UpdateBuffer(&buffer);
			m_rhiPipeline->Bind();
			m_rhiDevice->DrawIndexed(m_grid->GetIndexCount(), 0, 0);
		}

		m_rhiDevice->Set_AlphaBlendingEnabled(false);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_Gizmos(shared_ptr<RHI_RenderTexture>& texOut)
	{
		bool draw = m_flags & Render_Light;
		if (!draw)
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_Gizmos");

		auto& lights = m_actors[Renderable_Light];
		if (lights.size() != 0)
		{
			m_rhiDevice->EventBegin("Lights");

			m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
			m_rhiPipeline->SetShader(m_shaderTexture);
			m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
			for (const auto& actor : lights)
			{
				Vector3 lightWorldPos = actor->GetTransform_PtrRaw()->GetPosition();
				Vector3 cameraWorldPos = m_camera->GetTransform()->GetPosition();

				// Compute light screen space position and scale (based on distance from the camera)
				Vector2 lightScreenPos = m_camera->WorldToScreenPoint(lightWorldPos);
				float distance = Clamp(Vector3::Length(lightWorldPos, cameraWorldPos), 0.0f, FLT_MAX);
				float scale = GIZMO_MAX_SIZE / distance;
				scale = Clamp(scale, GIZMO_MIN_SIZE, GIZMO_MAX_SIZE);

				// Skip if the light is not in front of the camera
				if (!m_camera->IsInViewFrustrum(lightWorldPos, Vector3(1.0f)))
					continue;

				// Skip if the light if it's too small
				if (scale < GIZMO_MIN_SIZE)
					continue;

				shared_ptr<RHI_Texture> lightTex = nullptr;
				LightType type = actor->GetComponent<Light>()->GetLightType();
				if (type == LightType_Directional)
				{
					lightTex = m_gizmoTexLightDirectional;
				}
				else if (type == LightType_Point)
				{
					lightTex = m_gizmoTexLightPoint;
				}
				else if (type == LightType_Spot)
				{
					lightTex = m_gizmoTexLightSpot;
				}

				// Construct appropriate rectangle
				float texWidth = lightTex->GetWidth() * scale;
				float texHeight = lightTex->GetHeight() * scale;
				m_gizmoRectLight->Create(
					lightScreenPos.x - texWidth * 0.5f,
					lightScreenPos.y - texHeight * 0.5f,
					texWidth,
					texHeight
				);
			
				SetGlobalBuffer(m_viewProjection_Orthographic, texOut->GetWidth(), texOut->GetHeight());
				m_rhiPipeline->SetTexture(lightTex);
				m_rhiPipeline->SetIndexBuffer(m_gizmoRectLight->GetIndexBuffer());
				m_rhiPipeline->SetVertexBuffer(m_gizmoRectLight->GetVertexBuffer());
				m_rhiPipeline->Bind();
				m_rhiDevice->Set_AlphaBlendingEnabled(true);
				m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);
				m_rhiDevice->Set_AlphaBlendingEnabled(false);
			}
			m_rhiDevice->EventEnd();
		}

		// Transformation Gizmo		
		//m_rhiDevice->EventBegin("Transformation");
		//{
		//	TransformationGizmo* gizmo = m_camera->GetTransformationGizmo();
		//	gizmo->SetBuffers();
		//	m_shaderTransformationGizmo->Bind();

		//	// X - Axis
		//	m_shaderTransformationGizmo->Bind_Buffer(gizmo->GetTransformationX() * m_mView * m_mProjection, Vector3::Right, Vector3::Zero, 0);
		//	m_rhiDevice->DrawIndexed(gizmo->GetIndexCount(), 0, 0);
		//	// Y - Axis
		//	m_shaderTransformationGizmo->Bind_Buffer(gizmo->GetTransformationY() * m_mView * m_mProjection, Vector3::Up, Vector3::Zero, 0);
		//	m_rhiDevice->DrawIndexed(gizmo->GetIndexCount(), 0, 0);
		//	// Z - Axis
		//	m_shaderTransformationGizmo->Bind_Buffer(gizmo->GetTransformationZ() * m_mView * m_mProjection, Vector3::Forward, Vector3::Zero, 0);
		//	m_rhiDevice->DrawIndexed(gizmo->GetIndexCount(), 0, 0);
		//}
		//m_rhiDevice->EventEnd();

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	void Renderer::Pass_PerformanceMetrics(shared_ptr<RHI_RenderTexture>& texOut)
	{
		bool draw = m_flags & Render_PerformanceMetrics;
		if (!draw)
			return;

		TIME_BLOCK_START_MULTI();
		m_rhiDevice->EventBegin("Pass_PerformanceMetrics");

		Vector2 textPos = Vector2(-(int)Settings::Get().Viewport_GetWidth() * 0.5f + 1.0f, (int)Settings::Get().Viewport_GetHeight() * 0.5f);
		m_font->SetText(Profiler::Get().GetMetrics(), textPos);

		m_rhiDevice->Set_AlphaBlendingEnabled(true);
		m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_rhiPipeline->SetCullMode(Cull_Back);
		m_rhiPipeline->SetFillMode(Fill_Solid);
		m_rhiPipeline->SetIndexBuffer(m_font->GetIndexBuffer());
		m_rhiPipeline->SetVertexBuffer(m_font->GetVertexBuffer());
		m_rhiPipeline->SetRenderTarget(texOut);	
		m_rhiPipeline->SetTexture(m_font->GetTexture());
		m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
		m_rhiPipeline->SetShader(m_shaderFont);
		auto buffer = Struct_Matrix_Vector4(m_viewProjection_Orthographic, m_font->GetColor());
		m_shaderFont->UpdateBuffer(&buffer);
		m_rhiPipeline->SetConstantBuffer(m_shaderFont->GetConstantBuffer(), 0, Buffer_Global);
		m_rhiPipeline->Bind();
		m_rhiDevice->DrawIndexed(m_font->GetIndexCount(), 0, 0);
		m_rhiDevice->Set_AlphaBlendingEnabled(false);

		m_rhiDevice->EventEnd();
		TIME_BLOCK_END_MULTI();
	}

	bool Renderer::Pass_GBufferVisualize(shared_ptr<RHI_RenderTexture>& texOut)
	{
		GBuffer_Texture_Type texType = GBuffer_Target_Unknown;
		texType	= Flags_IsSet(Render_Albedo)	? GBuffer_Target_Albedo		: texType;
		texType = Flags_IsSet(Render_Normal)	? GBuffer_Target_Normal		: texType;
		texType = Flags_IsSet(Render_Material)	? GBuffer_Target_Material	: texType;
		texType = Flags_IsSet(Render_Velocity)	? GBuffer_Target_Velocity	: texType;
		texType = Flags_IsSet(Render_Depth)		? GBuffer_Target_Depth		: texType;

		if (texType != GBuffer_Target_Unknown)
		{
			TIME_BLOCK_START_MULTI();
			m_rhiDevice->EventBegin("Pass_GBufferVisualize");

			SetGlobalBuffer(m_viewProjection_Orthographic, texOut->GetWidth(), texOut->GetHeight());
			m_rhiPipeline->SetRenderTarget(texOut);
			m_rhiPipeline->Clear();
			m_rhiPipeline->SetVertexBuffer(m_quad->GetVertexBuffer());
			m_rhiPipeline->SetIndexBuffer(m_quad->GetIndexBuffer());
			m_rhiPipeline->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
			m_rhiPipeline->SetFillMode(Fill_Solid);
			m_rhiPipeline->SetCullMode(Cull_Back);
			m_rhiPipeline->SetInputLayout(m_shaderTexture->GetInputLayout());
			m_rhiPipeline->SetShader(m_shaderTexture);
			m_rhiPipeline->SetViewport(m_gbuffer->GetTexture(texType)->GetViewport());
			m_rhiPipeline->SetTexture(m_gbuffer->GetTexture(texType));
			m_rhiPipeline->SetSampler(m_samplerBilinearClampAlways);
			m_rhiPipeline->Bind();

			m_rhiDevice->DrawIndexed(m_quad->GetIndexCount(), 0, 0);

			m_rhiDevice->EventEnd();
			TIME_BLOCK_END_MULTI();
		}

		return true;
	}
	//=============================================================================================================

	Light* Renderer::GetLightDirectional()
	{
		auto actors = m_actors[Renderable_Light];

		for (const auto& actor : actors)
		{
			Light* light = actor->GetComponent<Light>().get();
			if (light->GetLightType() == LightType_Directional)
				return light;
		}

		return nullptr;
	}

	Skybox* Renderer::GetSkybox()
	{
		auto actors = m_actors[Renderable_Skybox];
		if (actors.empty())
			return nullptr;

		auto skyboxActor = actors.front();
		return skyboxActor ? skyboxActor->GetComponent<Skybox>().get() : nullptr;
	}
}
